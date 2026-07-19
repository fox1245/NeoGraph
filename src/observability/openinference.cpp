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
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
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
        Tracer* tracer = nullptr;
        std::string root_name;
        std::string node_span_prefix;
        std::unique_ptr<Span> root_span;
        bool closed = false;

        // Serializes complete callback executions with close(). A copied
        // callback holds only a weak_ptr, so it becomes a no-op after the
        // session state is destroyed.
        mutable std::mutex callback_mu;
        std::map<std::string, std::vector<std::unique_ptr<Span>>> pending;
        std::vector<Span*> active_nodes;

        // Best-effort current parent for callers without thread-local span
        // context. Protected by callback_mu because ending a span invalidates
        // this raw pointer.
        Span* active_node = nullptr;

        void add_node_span(const std::string& node,
                           std::unique_ptr<Span> span) noexcept {
            try {
                auto& stack = pending[node];
                stack.push_back(std::move(span));
                Span* current = stack.back().get();
                try {
                    active_nodes.push_back(current);
                } catch (...) {
                    span = std::move(stack.back());
                    stack.pop_back();
                    if (span) {
                        try { span->end(); } catch (...) {}
                    }
                    return;
                }
                active_node = current;
            } catch (...) {
                if (span) {
                    try { span->end(); } catch (...) {}
                }
            }
        }

        std::unique_ptr<Span> take_node_span(const std::string& node) {
            auto it = pending.find(node);
            if (it == pending.end() || it->second.empty()) return {};

            auto span = std::move(it->second.back());
            it->second.pop_back();
            if (!span) return {};

            active_nodes.erase(
                std::remove(active_nodes.begin(), active_nodes.end(), span.get()),
                active_nodes.end());
            active_node = active_nodes.empty() ? nullptr : active_nodes.back();
            return span;
        }
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

    std::lock_guard<std::mutex> lock(state->callback_mu);
    if (state->closed) return;
    state->closed = true;
    state->active_node = nullptr;
    state->active_nodes.clear();
    for (auto& [node, stack] : state->pending) {
        while (!stack.empty()) {
            try { stack.back()->end(); } catch (...) {}
            stack.pop_back();
        }
    }
    state->pending.clear();
    if (state->root_span) {
        try { state->root_span->end(); } catch (...) {}
        state->root_span.reset();
    }
}

Span* OpenInferenceTracerSession::current_parent() const noexcept {
    if (!impl_) return nullptr;
    auto state = impl_->state;
    if (!state) return nullptr;
    std::lock_guard<std::mutex> lock(state->callback_mu);
    if (state->closed) return nullptr;
    return state->active_node ? state->active_node : state->root_span.get();
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

        std::lock_guard<std::mutex> callback_lock(state->callback_mu);
        if (state->closed || !state->tracer) return;

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
                auto span = state->take_node_span(node);
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
                    span->set_attribute(kOutputValue,
                                        node_output_blob(node, ev.data));
                    span->set_attribute(kOutputMime, "application/json");
                    span->set_status_ok();
                } catch (...) {}
                try { span->end(); } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::ERROR: {
                auto span = state->take_node_span(node);
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
                try { span->end(); } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::INTERRUPT: {
                auto span = state->take_node_span(node);
                if (!span) break;
                try { span->set_attribute_bool("neograph.interrupted", true); }
                catch (...) {}
                try { span->end(); } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::LLM_TOKEN: {
                // Surface streamed tokens as discrete events on the
                // current node span. Phoenix renders these on the
                // timeline view; OTel SDK exporters treat them as
                // span events (not new spans) so cardinality stays
                // bounded.
                auto it = state->pending.find(node);
                if (it == state->pending.end() || it->second.empty()) break;
                Span* current = it->second.back().get();
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
    const std::string& span_name,
    const CompletionParams& params) noexcept {
    Span* parent = nullptr;
    if (parent_lookup) {
        try { parent = parent_lookup(); } catch (...) {}
    }

    std::unique_ptr<Span> span;
    try {
        span = tracer->start_span(span_name, parent);
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
        impl_->tracer, impl_->parent_lookup, impl_->span_name, params);
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
        impl_->tracer, impl_->parent_lookup, impl_->span_name, params);
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
        impl_->tracer, impl_->parent_lookup, impl_->span_name, params);
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
        impl_->tracer, impl_->parent_lookup, impl_->span_name, params);
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
