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

#include <atomic>
#include <exception>
#include <map>
#include <mutex>
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
    Tracer* tracer = nullptr;
    std::string root_name;
    std::string node_span_prefix;
    std::unique_ptr<Span> root_span;
    std::atomic<bool> closed{false};

    // Per-node span stacks. Stack semantics so nested fan-outs (a node
    // re-entered before its prior NODE_END landed) close in LIFO order.
    // Mutex-guarded — engine event callback may fire from worker pool
    // threads.
    std::mutex pending_mu;
    std::map<std::string, std::vector<std::unique_ptr<Span>>> pending;

    // The currently-active node span exposed to OpenInferenceProvider
    // via current_parent(). Tracks the most recently opened, not yet
    // ended, node span — best-effort under fan-out (single pointer,
    // not per-thread). For strict per-node-LLM nesting, callers should
    // capture parent_lookup returning the right span via their own
    // contextvar / threading.local instead.
    std::atomic<Span*> active_node{nullptr};
};

OpenInferenceTracerSession::OpenInferenceTracerSession()
    : impl_(std::make_unique<Impl>()) {}

OpenInferenceTracerSession::~OpenInferenceTracerSession() {
    close();
}

OpenInferenceTracerSession::OpenInferenceTracerSession(
    OpenInferenceTracerSession&&) noexcept = default;
OpenInferenceTracerSession& OpenInferenceTracerSession::operator=(
    OpenInferenceTracerSession&&) noexcept = default;

void OpenInferenceTracerSession::close() {
    if (!impl_) return;
    bool expected = false;
    if (!impl_->closed.compare_exchange_strong(expected, true)) return;

    {
        std::lock_guard<std::mutex> lock(impl_->pending_mu);
        for (auto& [node, stack] : impl_->pending) {
            while (!stack.empty()) {
                try { stack.back()->end(); } catch (...) {}
                stack.pop_back();
            }
        }
        impl_->pending.clear();
    }
    impl_->active_node.store(nullptr);
    if (impl_->root_span) {
        try { impl_->root_span->end(); } catch (...) {}
        impl_->root_span.reset();
    }
}

Span* OpenInferenceTracerSession::current_parent() const noexcept {
    if (!impl_) return nullptr;
    Span* node = impl_->active_node.load();
    return node ? node : impl_->root_span.get();
}

OpenInferenceTracerSession openinference_tracer(Tracer& tracer,
                                                std::string root_name,
                                                std::string node_span_prefix) {
    OpenInferenceTracerSession session;
    auto* impl = session.impl_.get();
    impl->tracer = &tracer;
    impl->root_name = std::move(root_name);
    impl->node_span_prefix = std::move(node_span_prefix);
    impl->root_span = tracer.start_span(impl->root_name);
    if (impl->root_span) {
        try {
            impl->root_span->set_attribute(kSpanKind, "CHAIN");
        } catch (...) {}
    }

    // Capture the impl pointer (not the session, which is moved) into
    // the callback. The session owns impl via unique_ptr; the callback
    // is owned by std::function inside the session, lifetimes match.
    session.cb = [impl](const graph::GraphEvent& ev) {
        if (impl->closed.load()) return;
        if (!impl->tracer) return;

        const std::string& node = ev.node_name;
        try {
            switch (ev.type) {
            case graph::GraphEvent::Type::NODE_START: {
                Span* parent = impl->root_span.get();
                auto span = impl->tracer->start_span(
                    impl->node_span_prefix + node, parent);
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

                impl->active_node.store(span.get());
                std::lock_guard<std::mutex> lock(impl->pending_mu);
                impl->pending[node].push_back(std::move(span));
                break;
            }

            case graph::GraphEvent::Type::NODE_END: {
                std::unique_ptr<Span> span;
                {
                    std::lock_guard<std::mutex> lock(impl->pending_mu);
                    auto it = impl->pending.find(node);
                    if (it == impl->pending.end() || it->second.empty()) break;
                    span = std::move(it->second.back());
                    it->second.pop_back();
                }
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
                // Best-effort: clear active_node if we just closed it.
                Span* expected = nullptr;
                impl->active_node.compare_exchange_strong(expected, nullptr);
                break;
            }

            case graph::GraphEvent::Type::ERROR: {
                std::unique_ptr<Span> span;
                {
                    std::lock_guard<std::mutex> lock(impl->pending_mu);
                    auto it = impl->pending.find(node);
                    if (it == impl->pending.end() || it->second.empty()) break;
                    span = std::move(it->second.back());
                    it->second.pop_back();
                }
                if (!span) break;
                std::string msg = ev.data.is_string()
                    ? ev.data.get<std::string>() : ev.data.dump();
                try {
                    span->set_attribute("neograph.error", msg);
                    span->set_status_error(msg);
                } catch (...) {}
                try { span->end(); } catch (...) {}
                break;
            }

            case graph::GraphEvent::Type::INTERRUPT: {
                std::unique_ptr<Span> span;
                {
                    std::lock_guard<std::mutex> lock(impl->pending_mu);
                    auto it = impl->pending.find(node);
                    if (it == impl->pending.end() || it->second.empty()) break;
                    span = std::move(it->second.back());
                    it->second.pop_back();
                }
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
                std::lock_guard<std::mutex> lock(impl->pending_mu);
                auto it = impl->pending.find(node);
                if (it == impl->pending.end() || it->second.empty()) break;
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

} // namespace

// Candidate 6 PR4: inner Provider's 4 legacy virtuals are now
// [[deprecated]]. OpenInferenceProvider is a tracing decorator that
// MUST keep wrapping all 4 surfaces through the deprecation window so
// downstream code calling any legacy method on a wrapped provider
// still gets a span. The 4 overrides below legitimately forward to
// `impl_->inner->complete*()` — bracket the region so the build
// doesn't drown in self-warnings. v1.0 collapses these to a single
// `invoke()` override.
NEOGRAPH_PUSH_IGNORE_DEPRECATED

ChatCompletion OpenInferenceProvider::complete(const CompletionParams& params) {
    Span* parent = impl_->parent_lookup ? impl_->parent_lookup() : nullptr;
    auto span = impl_->tracer->start_span(impl_->span_name, parent);
    record_input(span.get(), params);
    try {
        auto result = impl_->inner->complete(params);
        record_output(span.get(), result);
        if (span) { try { span->set_status_ok(); } catch (...) {} }
        if (span) { try { span->end(); } catch (...) {} }
        return result;
    } catch (const std::exception& e) {
        if (span) {
            try { span->set_status_error(e.what()); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        throw;
    } catch (...) {
        if (span) {
            try { span->set_status_error("unknown error"); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        throw;
    }
}

asio::awaitable<ChatCompletion>
OpenInferenceProvider::complete_async(const CompletionParams& params) {
    Span* parent = impl_->parent_lookup ? impl_->parent_lookup() : nullptr;
    auto span = impl_->tracer->start_span(impl_->span_name, parent);
    record_input(span.get(), params);
    try {
        auto result = co_await impl_->inner->complete_async(params);
        record_output(span.get(), result);
        if (span) { try { span->set_status_ok(); } catch (...) {} }
        if (span) { try { span->end(); } catch (...) {} }
        co_return result;
    } catch (const std::exception& e) {
        if (span) {
            try { span->set_status_error(e.what()); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        throw;
    }
}

ChatCompletion OpenInferenceProvider::complete_stream(
    const CompletionParams& params,
    const StreamCallback& on_chunk) {
    Span* parent = impl_->parent_lookup ? impl_->parent_lookup() : nullptr;
    auto span = impl_->tracer->start_span(impl_->span_name, parent);
    record_input(span.get(), params);

    std::string accumulated;
    StreamCallback wrapped = [&accumulated, &on_chunk, sp = span.get()]
        (const std::string& chunk) {
        accumulated += chunk;
        if (sp) { try { sp->add_event("llm.token", chunk); } catch (...) {} }
        if (on_chunk) on_chunk(chunk);
    };

    try {
        auto result = impl_->inner->complete_stream(params, wrapped);
        record_output(span.get(), result);
        if (span) {
            // Stream-specific: also surface the assembled output for
            // backends that prefer attributes over events.
            try { span->set_attribute(kOutputValue, accumulated); }
            catch (...) {}
            try { span->set_status_ok(); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        return result;
    } catch (const std::exception& e) {
        if (span) {
            try { span->set_status_error(e.what()); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        throw;
    }
}

asio::awaitable<ChatCompletion>
OpenInferenceProvider::complete_stream_async(
    const CompletionParams& params,
    const StreamCallback& on_chunk) {
    Span* parent = impl_->parent_lookup ? impl_->parent_lookup() : nullptr;
    auto span = impl_->tracer->start_span(impl_->span_name, parent);
    record_input(span.get(), params);

    auto accumulated = std::make_shared<std::string>();
    Span* sp = span.get();
    StreamCallback wrapped = [accumulated, on_chunk, sp]
        (const std::string& chunk) {
        *accumulated += chunk;
        if (sp) { try { sp->add_event("llm.token", chunk); } catch (...) {} }
        if (on_chunk) on_chunk(chunk);
    };

    try {
        auto result = co_await impl_->inner->complete_stream_async(params, wrapped);
        record_output(span.get(), result);
        if (span) {
            try { span->set_attribute(kOutputValue, *accumulated); }
            catch (...) {}
            try { span->set_status_ok(); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        co_return result;
    } catch (const std::exception& e) {
        if (span) {
            try { span->set_status_error(e.what()); } catch (...) {}
            try { span->end(); } catch (...) {}
        }
        throw;
    }
}

NEOGRAPH_POP_IGNORE_DEPRECATED

} // namespace neograph::observability
