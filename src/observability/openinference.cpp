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
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <type_traits>

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
    return impl_->inner->get_name();
}

std::string_view OpenInferenceProvider::family() const noexcept {
    return impl_->inner->family();
}

namespace {
const char* role_name(sp::Role role) noexcept {
    switch (role) {
        case sp::Role::System: return "system";
        case sp::Role::Developer: return "developer";
        case sp::Role::User: return "user";
        case sp::Role::Assistant: return "assistant";
        case sp::Role::Tool: return "tool";
    }
    return "unknown";
}

std::string visible_text(const sp::Message& message) {
    std::string text;
    for (const auto& part : message.parts) {
        if (const auto* visible = std::get_if<sp::Text>(&part)) text += visible->value;
    }
    return text;
}

json public_input_observation(const ProviderRequest& request) {
    return std::visit([](const auto& payload) -> json {
        json messages = json::array();
        if constexpr (std::is_same_v<std::decay_t<decltype(payload)>, sp::chat::Request>) {
            if (!payload.messages.empty()) {
                for (const auto& message : payload.messages) {
                    messages.push_back({{"role", role_name(message.role)}, {"content", message.text}});
                }
            } else {
                for (const auto& message : payload.canonical_messages) {
                    messages.push_back({{"role", role_name(message.role)},
                                        {"content", visible_text(message)}});
                }
            }
        } else {
            for (const auto& message : payload.messages) {
                messages.push_back({{"role", role_name(message.role)},
                                    {"content", visible_text(message)}});
            }
        }
        json invocation = json::object();
        if constexpr (requires { payload.temperature; }) {
            if (payload.temperature) invocation["temperature"] = *payload.temperature;
        }
        if constexpr (requires { payload.max_output_tokens; }) {
            if (payload.max_output_tokens) invocation["max_tokens"] = *payload.max_output_tokens;
        } else {
            if (payload.max_tokens) invocation["max_tokens"] = *payload.max_tokens;
        }
        return {{"messages", std::move(messages)}, {"invocation", std::move(invocation)}};
    }, request.payload);
}

void record_input(Span* span, const PreparedProviderRequest& request,
                  const json& observation) noexcept {
    if (!span) return;
    try {
        span->set_attribute(kSpanKind, "LLM");
        span->set_attribute(kLlmModel, request.model());
        if (observation.is_null()) return;
        span->set_attribute(kLlmInvocation, observation.at("invocation").dump());
        const auto& messages = observation.at("messages");
        for (size_t i = 0; i < messages.size(); ++i) {
            const auto base = "llm.input_messages." + std::to_string(i) + ".message";
            span->set_attribute(base + ".role", messages[i].at("role").get<std::string>());
            span->set_attribute(base + ".content", messages[i].at("content").get<std::string>());
        }
        if (!messages.empty()) {
            span->set_attribute(kInputValue, messages.dump());
            span->set_attribute(kInputMime, "application/json");
        }
    } catch (...) {}
}

void record_count(Span* span, const char* key,
                  const std::optional<sp::Count>& count) {
    if (!count) return;
    // Span's numeric sink is signed; never wrap a genuine uint64 report.
    if (count->value <= static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
        span->set_attribute(key, static_cast<int64_t>(count->value));
    } else {
        span->set_attribute(key, std::to_string(count->value));
    }
}

void record_output(Span* span, const sp::Outcome& outcome) noexcept {
    if (!span) return;
    try {
        const auto& messages = outcome_messages(outcome);
        std::string output;
        for (size_t i = 0; i < messages.size(); ++i) {
            const auto text = visible_text(messages[i]);
            const auto base = "llm.output_messages." + std::to_string(i) + ".message";
            span->set_attribute(base + ".role", role_name(messages[i].role));
            span->set_attribute(base + ".content", text);
            output += text;
        }
        span->set_attribute(kOutputValue, output);
        span->set_attribute(kOutputMime, "text/plain");
        const auto& usage = outcome_usage(outcome);
        record_count(span, kLlmTokenPrompt, usage.input_total);
        record_count(span, kLlmTokenCompletion, usage.output_total);
        record_count(span, kLlmTokenTotal, usage.total);
    } catch (...) {}
}

class ProviderSpanState {
public:
    ~ProviderSpanState() { end(); }

    void start(Tracer* tracer,
               const std::function<Span*()>& parent_lookup,
               const OpenInferenceTracerSession::ChildSpanStarter& child_span_starter,
               const std::string& name,
               const PreparedProviderRequest& request,
               const json& messages) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (started_ || ended_) return;
            started_ = true;
            if (child_span_starter) {
                span_ = child_span_starter(*tracer, name);
            } else {
                Span* parent = nullptr;
                if (parent_lookup) {
                    try { parent = parent_lookup(); } catch (...) {}
                }
                span_ = tracer->start_span(name, parent);
            }
            record_input(span_.get(), request, messages);
        } catch (...) {}
    }

    void event(const sp::Event& event) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            const auto* delta = std::get_if<sp::PartDelta>(&event);
            if (!ended_ && span_ && delta && delta->payload.kind == sp::PartKind::Text &&
                delta->payload.channel == sp::DeltaChannel::Content) {
                // Only public visible-text deltas are telemetry. The original
                // typed event still reaches the caller unchanged.
                span_->add_event("llm.token", delta->payload.bytes);
            }
        } catch (...) {}
    }

    void finish(const sp::runtime::Result& result) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (ended_) return;
            if (span_ && result) {
                record_output(span_.get(), *result);
                try {
                    if (const auto* failure = std::get_if<sp::Failure>(result.get())) {
                        span_->set_status_error(failure->error.safe_message);
                    } else {
                        span_->set_status_ok();
                    }
                } catch (...) {}
            }
            end_locked();
        } catch (...) {}
    }

    void finish_error(std::exception_ptr error) noexcept {
        // Exceptions are observed, not translated into synthetic provider
        // results. The dispatch layer retains and rethrows the original.
        try {
            std::lock_guard<std::mutex> lock(mu_);
            if (ended_) return;
            if (span_ && error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& exception) {
                    try { span_->set_status_error(exception.what()); } catch (...) {}
                } catch (...) {
                    try { span_->set_status_error("non-standard dispatch exception"); } catch (...) {}
                }
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
    bool started_ = false;
    bool ended_ = false;
};

} // namespace

PreparedProviderRequest OpenInferenceProvider::prepare(ProviderRequest request) {
    // Snapshot only the existing public text/scalar telemetry surface before
    // ownership moves. Private native parts never enter this observation.
    json observation;
    try { observation = public_input_observation(request); } catch (...) {}
    // Prepare first: invalid admission must precede any tracer callbacks.
    auto prepared = impl_->inner->prepare(std::move(request));
    if (!prepared.valid()) return prepared;

    // Observation setup is best effort. If allocation/capture fails, return the
    // original admitted handle; never retry preparation or alter dispatch.
    try {
        auto trace = std::make_shared<ProviderSpanState>();
        std::function<void(const PreparedProviderRequest&)> before = [trace, tracer = impl_->tracer,
                       parent = impl_->parent_lookup,
                       starter = impl_->child_span_starter,
                       name = impl_->span_name,
                       observation = std::move(observation)](const PreparedProviderRequest& admitted) noexcept {
            trace->start(tracer, parent, starter, name, admitted, observation);
        };
        std::function<void(sp::runtime::Result)> after = [trace](sp::runtime::Result result) noexcept {
            trace->finish(result);
        };
        std::function<void(const sp::Event&)> event = [trace](const sp::Event& value) noexcept {
            trace->event(value);
        };
        std::function<void(std::exception_ptr)> error = [trace](std::exception_ptr exception) noexcept {
            trace->finish_error(std::move(exception));
        };
        return observe_prepared(std::move(prepared),
                                std::move(before), std::move(after), std::move(event),
                                std::move(error));
    } catch (...) {
        return prepared;
    }
}

} // namespace neograph::observability
