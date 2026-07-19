/**
 * @file observability/openinference.cpp
 * @brief Implementation of openinference_tracer + OpenInferenceProvider.
 *
 * Mirrors `bindings/python/neograph_engine/openinference.py`. The
 * Python module's Context-attach/detach dance has no C++ analog —
 * the abstract Tracer interface here is explicit-parent, so the
 * pending-span stack carries the parent pointer directly.
 */
#include <neograph/observability/openinference.h>

#include <neograph/json.h>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace neograph::observability {

namespace {

constexpr const char* kSpanKind = "openinference.span.kind";
constexpr const char* kInputValue = "input.value";
constexpr const char* kInputMime = "input.mime_type";
constexpr const char* kOutputValue = "output.value";
constexpr const char* kOutputMime = "output.mime_type";
constexpr const char* kLlmModel = "llm.model_name";
constexpr const char* kLlmInvocation = "llm.invocation_parameters";
constexpr const char* kLlmTokenPrompt = "llm.token_count.prompt";
constexpr const char* kLlmTokenCompletion = "llm.token_count.completion";
constexpr const char* kLlmTokenTotal = "llm.token_count.total";

std::string node_input_blob(const std::string& node_name, const json& data) {
    json blob = json::object();
    blob["node"] = node_name;
    if (data.is_object()) {
        for (auto it = data.begin(); it != data.end(); ++it) {
            blob[it.key()] = it.value();
        }
    }
    try {
        return blob.dump();
    } catch (...) {
        return node_name;
    }
}

std::string node_output_blob(const std::string& node_name, const json& data) {
    json blob = json::object();
    if (data.is_object()) {
        for (auto it = data.begin(); it != data.end(); ++it) {
            blob[it.key()] = it.value();
        }
    } else {
        blob["node"] = node_name;
    }
    try {
        return blob.dump();
    } catch (...) {
        return node_name;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// OpenInferenceTracerSession
// ---------------------------------------------------------------------------

struct OpenInferenceTracerSession::Impl {
    struct State {
        using SpanList = std::list<std::unique_ptr<Span>>;

        struct FinalizeBatch {
            SpanList spans;
            std::unique_ptr<Span> root;
            bool closes_state = false;
        };

        Tracer* tracer = nullptr;
        std::string root_name;
        std::string node_span_prefix;
        std::unique_ptr<Span> root_span;
        bool closed = false;
        bool finalizing = false;
        bool finalized = false;
        size_t in_flight = 0;
        std::thread::id finalizer_thread;

        // Session operations hold a gate, not this mutex, while invoking
        // arbitrary tracer code. close() waits for ordinary concurrent
        // operations, while a re-entrant close is finalized by the last
        // operation leaving the gate.
        mutable std::mutex mu;
        std::condition_variable cv;
        std::map<std::thread::id, size_t> operation_owners;
        std::map<std::string, SpanList> pending;
        SpanList retired;
        std::vector<Span*> active_nodes;

        // Best-effort current parent for callers without thread-local span
        // context. Protected by mu because ending a span invalidates
        // this raw pointer.
        Span* active_node = nullptr;

        bool acquire_operation(Span** parent = nullptr) {
            std::lock_guard<std::mutex> lock(mu);
            if (closed) return false;
            try {
                ++operation_owners[std::this_thread::get_id()];
            } catch (...) {
                return false;
            }
            ++in_flight;
            if (parent) {
                *parent = active_node ? active_node : root_span.get();
            }
            return true;
        }

        FinalizeBatch collect_locked(bool closing) {
            FinalizeBatch batch;
            batch.spans.splice(batch.spans.end(), retired);
            if (closing) {
                for (auto& [node, stack] : pending) {
                    batch.spans.splice(batch.spans.end(), stack);
                }
                pending.clear();
                active_nodes.clear();
                active_node = nullptr;
                batch.root = std::move(root_span);
                batch.closes_state = true;
                finalizing = true;
                finalizer_thread = std::this_thread::get_id();
            }
            return batch;
        }

        void run_batch(FinalizeBatch batch) noexcept {
            for (auto& span : batch.spans) {
                if (span) {
                    try { span->end(); } catch (...) {}
                }
            }
            if (batch.root) {
                try { batch.root->end(); } catch (...) {}
            }
            batch.spans.clear();
            batch.root.reset();
            if (batch.closes_state) {
                std::lock_guard<std::mutex> lock(mu);
                finalized = true;
                finalizing = false;
                finalizer_thread = {};
                cv.notify_all();
            }
        }

        void release_operation() noexcept {
            while (true) {
                FinalizeBatch batch;
                bool keep_lease = false;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    if (!closed && in_flight == 1 && !retired.empty()) {
                        batch = collect_locked(false);
                        keep_lease = true;
                    } else {
                        const auto owner = std::this_thread::get_id();
                        auto it = operation_owners.find(owner);
                        if (it != operation_owners.end()) {
                            if (--it->second == 0) operation_owners.erase(it);
                        }
                        if (in_flight > 0) --in_flight;
                        if (in_flight == 0 && closed
                            && !finalizing && !finalized) {
                            batch = collect_locked(true);
                        }
                    }
                }
                run_batch(std::move(batch));
                if (!keep_lease) return;
            }
        }

        void close() {
            FinalizeBatch batch;
            {
                std::unique_lock<std::mutex> lock(mu);
                if (finalized) return;
                closed = true;

                const auto caller = std::this_thread::get_id();
                const bool reentrant = operation_owners.contains(caller);
                if (finalizing) {
                    if (reentrant || finalizer_thread == caller) return;
                    cv.wait(lock, [&] { return finalized; });
                    return;
                }
                if (in_flight == 0) {
                    batch = collect_locked(true);
                } else if (reentrant) {
                    return;
                } else {
                    cv.wait(lock, [&] { return finalized; });
                    return;
                }
            }
            run_batch(std::move(batch));
        }

        void add_node_span(const std::string& node,
                           std::unique_ptr<Span> span) noexcept {
            std::unique_ptr<Span> failed;
            try {
                std::lock_guard<std::mutex> lock(mu);
                auto& stack = pending[node];
                stack.push_back(std::move(span));
                Span* current = stack.back().get();
                try {
                    active_nodes.push_back(current);
                } catch (...) {
                    retired.splice(retired.end(), stack, std::prev(stack.end()));
                    return;
                }
                active_node = current;
            } catch (...) {
                failed = std::move(span);
            }
            if (failed) {
                try { failed->end(); } catch (...) {}
            }
        }

        Span* retire_node_span(const std::string& node) {
            std::lock_guard<std::mutex> lock(mu);
            auto it = pending.find(node);
            if (it == pending.end() || it->second.empty()) return nullptr;

            Span* span = it->second.back().get();
            if (!span) return nullptr;

            active_nodes.erase(
                std::remove(active_nodes.begin(), active_nodes.end(), span),
                active_nodes.end());
            active_node = active_nodes.empty() ? nullptr : active_nodes.back();
            retired.splice(retired.end(), it->second, std::prev(it->second.end()));
            return span;
        }

        Span* token_span(const std::string& node) const noexcept {
            std::lock_guard<std::mutex> lock(mu);
            auto it = pending.find(node);
            if (it == pending.end() || it->second.empty()) return nullptr;
            return it->second.back().get();
        }

        Span* current_parent_snapshot() const noexcept {
            std::lock_guard<std::mutex> lock(mu);
            if (closed) return nullptr;
            return active_node ? active_node : root_span.get();
        }
    };

    struct Operation {
        explicit Operation(std::shared_ptr<State> state,
                           bool capture_parent = false)
            : state(std::move(state)),
              parent(nullptr),
              active(this->state->acquire_operation(
                  capture_parent ? &parent : nullptr)) {}
        ~Operation() {
            if (active) state->release_operation();
        }

        explicit operator bool() const noexcept { return active; }

        std::shared_ptr<State> state;
        Span* parent = nullptr;
        bool active = false;
    };

    std::shared_ptr<State> state = std::make_shared<State>();
};

OpenInferenceTracerSession::OpenInferenceTracerSession()
    : impl_(std::make_unique<Impl>()) {}

OpenInferenceTracerSession::~OpenInferenceTracerSession() {
    close();
}

OpenInferenceTracerSession::OpenInferenceTracerSession(
    OpenInferenceTracerSession&& other) noexcept
    : cb(std::move(other.cb)), impl_(std::move(other.impl_)) {}

OpenInferenceTracerSession& OpenInferenceTracerSession::operator=(
    OpenInferenceTracerSession&& other) noexcept {
    if (this == &other) return *this;
    close();
    cb = std::move(other.cb);
    impl_ = std::move(other.impl_);
    return *this;
}

void OpenInferenceTracerSession::close() {
    if (!impl_) return;
    auto state = impl_->state;
    if (!state) return;
    state->close();
}

Span* OpenInferenceTracerSession::current_parent() const noexcept {
    if (!impl_) return nullptr;
    auto state = impl_->state;
    if (!state) return nullptr;
    return state->current_parent_snapshot();
}

OpenInferenceTracerSession::ChildSpanStarter
OpenInferenceTracerSession::child_span_starter() const {
    std::weak_ptr<Impl::State> weak_state;
    if (impl_) weak_state = impl_->state;
    return [weak_state](Tracer& tracer, std::string_view name) {
        auto state = weak_state.lock();
        if (!state) return tracer.start_span(name, nullptr);

        Impl::Operation operation(state, true);
        if (!operation) return tracer.start_span(name, nullptr);
        return tracer.start_span(name, operation.parent);
    };
}

OpenInferenceTracerSession openinference_tracer(Tracer& tracer,
                                                std::string root_name,
                                                std::string node_span_prefix) {
    OpenInferenceTracerSession session;
    auto state = session.impl_->state;
    state->tracer = &tracer;
    state->root_name = std::move(root_name);
    state->node_span_prefix = std::move(node_span_prefix);
    state->root_span = tracer.start_span(state->root_name);
    if (state->root_span) {
        try {
            state->root_span->set_attribute(kSpanKind, "CHAIN");
        } catch (...) {}
    }

    std::weak_ptr<OpenInferenceTracerSession::Impl::State> weak_state = state;
    session.cb = [weak_state](const graph::GraphEvent& ev) {
        auto state = weak_state.lock();
        if (!state) return;

        OpenInferenceTracerSession::Impl::Operation operation(state);
        if (!operation || !state->tracer) return;

        const std::string& node = ev.node_name;
        try {
            switch (ev.type) {
            case graph::GraphEvent::Type::NODE_START: {
                Span* parent = state->root_span.get();
                auto span = state->tracer->start_span(
                    state->node_span_prefix + node, parent);
                if (!span) break;
                try {
                    span->set_attribute(kSpanKind, "CHAIN");
                    span->set_attribute("neograph.node", node);
                    if (ev.data.is_object()) {
                        for (auto it = ev.data.begin(); it != ev.data.end(); ++it) {
                            std::string val;
                            if (it.value().is_string()) {
                                val = it.value().get<std::string>();
                            } else {
                                try { val = it.value().dump(); }
                                catch (...) { val = ""; }
                            }
                            span->set_attribute(
                                std::string("neograph.") + it.key(), val);
                        }
                    }
                    span->set_attribute(kInputValue,
                                        node_input_blob(node, ev.data));
                    span->set_attribute(kInputMime, "application/json");
                } catch (...) {}

                state->add_node_span(node, std::move(span));
                break;
            }

            case graph::GraphEvent::Type::NODE_END: {
                Span* span = state->retire_node_span(node);
                if (!span) break;
                try {
                    if (ev.data.is_object()) {
                        for (auto kv = ev.data.begin(); kv != ev.data.end(); ++kv) {
                            std::string val;
                            if (kv.value().is_string()) {
                                val = kv.value().get<std::string>();
                            } else {
                                try { val = kv.value().dump(); }
                                catch (...) { val = ""; }
                            }
                            span->set_attribute(
                                std::string("neograph.") + kv.key(), val);
                        }
                    }
                    span->set_attribute(
                        kOutputValue, node_output_blob(node, ev.data));
                    span->set_attribute(kOutputMime, "application/json");
                    span->set_status_ok();
                } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::ERROR: {
                Span* span = state->retire_node_span(node);
                if (!span) break;
                std::string msg = "unknown error";
                try {
                    msg = ev.data.is_string()
                        ? ev.data.get<std::string>() : ev.data.dump();
                } catch (...) {}
                try {
                    span->set_attribute("neograph.error", msg);
                    span->set_status_error(msg);
                } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::INTERRUPT: {
                Span* span = state->retire_node_span(node);
                if (!span) break;
                try { span->set_attribute_bool("neograph.interrupted", true); }
                catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::LLM_TOKEN: {
                // Surface streamed tokens as discrete events on the
                // current node span. Phoenix renders these on the
                // timeline view; OTel SDK exporters treat them as
                // span events (not new spans) so cardinality stays
                // bounded.
                Span* current = state->token_span(node);
                if (!current) break;
                std::string payload = ev.data.is_string()
                    ? ev.data.get<std::string>() : ev.data.dump();
                try { current->add_event("llm.token", payload); }
                catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::CHANNEL_WRITE:
                // Not surfaced as a span event — channel writes are
                // structural noise in a Phoenix trace view. Users who
                // want them can wrap the cb themselves.
                break;
            }
        } catch (...) {
            // Tracing must never break the graph run.
        }
    };

    return session;
}

// ---------------------------------------------------------------------------
// OpenInferenceProvider
// ---------------------------------------------------------------------------

struct OpenInferenceProvider::Impl {
    std::shared_ptr<Provider> inner;
    Tracer* tracer = nullptr;
    std::function<Span*()> parent_lookup;
    OpenInferenceTracerSession::ChildSpanStarter child_span_starter;
    std::string span_name;
};

OpenInferenceProvider::OpenInferenceProvider(
    std::shared_ptr<Provider> inner,
    Tracer& tracer,
    std::function<Span*()> parent_lookup,
    std::string span_name)
    : impl_(std::make_unique<Impl>()) {
    if (!inner) {
        throw std::invalid_argument(
            "OpenInferenceProvider requires a non-null inner Provider");
    }
    impl_->inner = std::move(inner);
    impl_->tracer = &tracer;
    impl_->parent_lookup = std::move(parent_lookup);
    impl_->span_name = std::move(span_name);
}

OpenInferenceProvider::OpenInferenceProvider(
    std::shared_ptr<Provider> inner,
    Tracer& tracer,
    const OpenInferenceTracerSession& session,
    std::string span_name)
    : impl_(std::make_unique<Impl>()) {
    if (!inner) {
        throw std::invalid_argument(
            "OpenInferenceProvider requires a non-null inner Provider");
    }
    impl_->inner = std::move(inner);
    impl_->tracer = &tracer;
    impl_->child_span_starter = session.child_span_starter();
    impl_->span_name = std::move(span_name);
}

OpenInferenceProvider::~OpenInferenceProvider() = default;

std::string OpenInferenceProvider::get_name() const {
    try {
        return std::string("openinference(") + impl_->inner->get_name() + ")";
    } catch (...) {
        return "openinference(provider)";
    }
}

namespace {

// Drop messages onto an LLM-kind span as input.value + per-message
// attribute set, matching the Python module's record_input.
void record_input(Span* span, const CompletionParams& params) {
    if (!span) return;
    try {
        span->set_attribute(kSpanKind, "LLM");
        if (!params.model.empty()) {
            span->set_attribute(kLlmModel, params.model);
        }

        json invocation = json::object();
        invocation["temperature"] = params.temperature;
        if (params.max_tokens >= 0) {
            invocation["max_tokens"] = params.max_tokens;
        }
        try {
            span->set_attribute(kLlmInvocation, invocation.dump());
        } catch (...) {}

        json messages_blob = json::array();
        for (size_t i = 0; i < params.messages.size(); ++i) {
            const auto& m = params.messages[i];
            std::string base = "llm.input_messages." + std::to_string(i)
                             + ".message";
            span->set_attribute(base + ".role", m.role);
            span->set_attribute(base + ".content", m.content);
            json one;
            one["role"] = m.role;
            one["content"] = m.content;
            messages_blob.push_back(std::move(one));
        }
        if (!params.messages.empty()) {
            try { span->set_attribute(kInputValue, messages_blob.dump()); }
            catch (...) {}
            span->set_attribute(kInputMime, "application/json");
        }
    } catch (...) {}
}

void record_output(Span* span, const ChatCompletion& result) {
    if (!span) return;
    try {
        const auto& m = result.message;
        span->set_attribute("llm.output_messages.0.message.role", m.role);
        span->set_attribute("llm.output_messages.0.message.content", m.content);
        span->set_attribute(kOutputValue, m.content);
        span->set_attribute(kOutputMime, "text/plain");

        if (result.usage.prompt_tokens > 0) {
            span->set_attribute(kLlmTokenPrompt,
                                static_cast<int64_t>(result.usage.prompt_tokens));
        }
        if (result.usage.completion_tokens > 0) {
            span->set_attribute(kLlmTokenCompletion,
                                static_cast<int64_t>(result.usage.completion_tokens));
        }
        if (result.usage.total_tokens > 0) {
            span->set_attribute(kLlmTokenTotal,
                                static_cast<int64_t>(result.usage.total_tokens));
        }
    } catch (...) {}
}

class ProviderSpanState {
public:
    explicit ProviderSpanState(std::unique_ptr<Span> span)
        : span_(std::move(span)) {}

    ~ProviderSpanState() { end(); }

    void record(const CompletionParams& params) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (!ended_) record_input(span_.get(), params);
        } catch (...) {}
    }

    void add_token(const std::string& chunk) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (!ended_ && span_) {
                try { span_->add_event("llm.token", chunk); } catch (...) {}
            }
        } catch (...) {}
    }

    void finish_ok(const ChatCompletion& result,
                   const std::string* streamed_output = nullptr) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (ended_) return;
            record_output(span_.get(), result);
            if (span_ && streamed_output) {
                try { span_->set_attribute(kOutputValue, *streamed_output); }
                catch (...) {}
            }
            if (span_) {
                try { span_->set_status_ok(); } catch (...) {}
            }
            end_locked();
        } catch (...) {}
    }

    void finish_error(std::string_view message) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (ended_) return;
            if (span_) {
                try { span_->set_status_error(message); } catch (...) {}
            }
            end_locked();
        } catch (...) {}
    }

    void end() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            end_locked();
        } catch (...) {}
    }

private:
    void end_locked() noexcept {
        if (ended_) return;
        ended_ = true;
        if (span_) {
            try { span_->end(); } catch (...) {}
        }
    }

    std::mutex mu_;
    std::unique_ptr<Span> span_;
    bool ended_ = false;
};

class ProviderSpanGuard {
public:
    explicit ProviderSpanGuard(std::shared_ptr<ProviderSpanState> state)
        : state_(std::move(state)) {}

    ~ProviderSpanGuard() {
        if (state_) state_->end();
    }

private:
    std::shared_ptr<ProviderSpanState> state_;
};

std::shared_ptr<ProviderSpanState> start_provider_span(
    Tracer* tracer,
    const std::function<Span*()>& parent_lookup,
    const OpenInferenceTracerSession::ChildSpanStarter& child_span_starter,
    const std::string& span_name,
    const CompletionParams& params) noexcept {
    std::unique_ptr<Span> span;
    try {
        if (child_span_starter) {
            span = child_span_starter(*tracer, span_name);
        } else {
            Span* parent = nullptr;
            if (parent_lookup) {
                try { parent = parent_lookup(); } catch (...) {}
            }
            span = tracer->start_span(span_name, parent);
        }
    } catch (...) {
        return {};
    }
    if (!span) return {};

    try {
        auto state = std::make_shared<ProviderSpanState>(std::move(span));
        state->record(params);
        return state;
    } catch (...) {
        if (span) {
            try { span->end(); } catch (...) {}
        }
        return {};
    }
}

} // namespace

// Keep wrapping every stable Provider entry point so callers get the same
// tracing behavior regardless of which compatibility surface they use.

ChatCompletion OpenInferenceProvider::complete(const CompletionParams& params) {
    auto trace = start_provider_span(
        impl_->tracer, impl_->parent_lookup, impl_->child_span_starter,
        impl_->span_name, params);
    ProviderSpanGuard guard(trace);
    try {
        auto result = impl_->inner->complete(params);
        if (trace) trace->finish_ok(result);
        return result;
    } catch (const std::exception& e) {
        if (trace) trace->finish_error(e.what());
        throw;
    } catch (...) {
        if (trace) trace->finish_error("unknown error");
        throw;
    }
}

asio::awaitable<ChatCompletion>
OpenInferenceProvider::complete_async(const CompletionParams& params) {
    auto trace = start_provider_span(
        impl_->tracer, impl_->parent_lookup, impl_->child_span_starter,
        impl_->span_name, params);
    ProviderSpanGuard guard(trace);
    try {
        auto result = co_await impl_->inner->complete_async(params);
        if (trace) trace->finish_ok(result);
        co_return result;
    } catch (const std::exception& e) {
        if (trace) trace->finish_error(e.what());
        throw;
    } catch (...) {
        if (trace) trace->finish_error("unknown error");
        throw;
    }
}

ChatCompletion OpenInferenceProvider::complete_stream(
    const CompletionParams& params,
    const StreamCallback& on_chunk) {
    auto trace = start_provider_span(
        impl_->tracer, impl_->parent_lookup, impl_->child_span_starter,
        impl_->span_name, params);
    ProviderSpanGuard guard(trace);

    std::string accumulated;
    StreamCallback wrapped = [&accumulated, &on_chunk, trace]
        (const std::string& chunk) {
        try { accumulated += chunk; } catch (...) {}
        if (trace) trace->add_token(chunk);
        if (on_chunk) on_chunk(chunk);
    };

    try {
        auto result = impl_->inner->complete_stream(params, wrapped);
        if (trace) trace->finish_ok(result, &accumulated);
        return result;
    } catch (const std::exception& e) {
        if (trace) trace->finish_error(e.what());
        throw;
    } catch (...) {
        if (trace) trace->finish_error("unknown error");
        throw;
    }
}

asio::awaitable<ChatCompletion>
OpenInferenceProvider::complete_stream_async(
    const CompletionParams& params,
    const StreamCallback& on_chunk) {
    auto trace = start_provider_span(
        impl_->tracer, impl_->parent_lookup, impl_->child_span_starter,
        impl_->span_name, params);
    ProviderSpanGuard guard(trace);

    std::shared_ptr<std::string> accumulated;
    try { accumulated = std::make_shared<std::string>(); } catch (...) {}
    StreamCallback wrapped = [accumulated, on_chunk, trace]
        (const std::string& chunk) {
        if (accumulated) {
            try { *accumulated += chunk; } catch (...) {}
        }
        if (trace) trace->add_token(chunk);
        if (on_chunk) on_chunk(chunk);
    };

    try {
        auto result = co_await impl_->inner->complete_stream_async(params, wrapped);
        if (trace) trace->finish_ok(result, accumulated.get());
        co_return result;
    } catch (const std::exception& e) {
        if (trace) trace->finish_error(e.what());
        throw;
    } catch (...) {
        if (trace) trace->finish_error("unknown error");
        throw;
    }
}

// Compatibility callback-selected override. Route through the existing
// overrides so each call still emits exactly one span.
asio::awaitable<ChatCompletion>
OpenInferenceProvider::invoke(const CompletionParams& params, StreamCallback on_chunk) {
    if (on_chunk) {
        co_return co_await complete_stream_async(params, on_chunk);
    }
    co_return co_await complete_async(params);
}

} // namespace neograph::observability
