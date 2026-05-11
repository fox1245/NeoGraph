/**
 * @file observability/openinference.h
 * @brief C++ peer of `neograph_engine.openinference` (issue #9).
 *
 * Two pieces, mirroring the Python module:
 *
 *   - `openinference_tracer(tracer)` — opens a CHAIN-kind root span,
 *     returns an `OpenInferenceTracerSession` whose `cb` field plugs
 *     into `engine.run_stream()` / `engine.run_stream_async()` and
 *     opens a CHAIN-kind child span per node, with NODE_START / END
 *     payloads stuffed into `input.value` / `output.value` JSON
 *     blobs and LLM_TOKEN streamed-chunk events recorded as discrete
 *     span events. Closing the session ends the root span.
 *
 *   - `OpenInferenceProvider(inner, tracer)` — wraps any `Provider`.
 *     Each `complete*` call opens an LLM-kind child span tagged with
 *     `llm.model_name`, `llm.invocation_parameters`,
 *     `llm.input_messages.{i}.message.{role,content}`,
 *     `llm.output_messages.0.message.{role,content}`, and
 *     `llm.token_count.{prompt,completion,total}`. The streaming
 *     overloads (`complete_stream` / `complete_stream_async`) also
 *     accumulate streamed tokens into the LLM span's `output.value`
 *     and emit one `llm.token` event per chunk.
 *
 * Phoenix / Arize / Langfuse render the resulting trace as a chat
 * chain with token counts and per-message bubbles — the same UX the
 * Python wrapper produces.
 *
 * Tracer adapter: NeoGraph itself does not link against
 * opentelemetry-cpp. Downstream provides an adapter implementing
 * `neograph::observability::Tracer` that wraps its own backend (OTel
 * SDK, in-memory test fake, logging recorder, etc.). See
 * `tests/test_openinference_cpp.cpp` for an `InMemoryTracer`
 * reference impl used by the parity tests.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/observability/tracer.h>
#include <neograph/provider.h>

#include <memory>
#include <string>

namespace neograph::observability {

/**
 * @brief RAII session returned by `openinference_tracer`.
 *
 * Holds the root span (kept alive for the duration of the engine
 * run) and the GraphStreamCallback wired to record per-node /
 * streaming events under that root.
 *
 * Usage:
 * @code
 *   auto session = openinference_tracer(my_tracer);
 *   engine.run_stream(cfg, session.cb);
 *   session.close();   // ends the root + any straggler node spans
 * @endcode
 *
 * `close()` is also called by the destructor — explicit close is
 * recommended only when the user wants to inspect the tracer's
 * exported spans before the session goes out of scope (e.g. in
 * tests). Double-close is a no-op.
 */
class NEOGRAPH_API OpenInferenceTracerSession {
public:
    /// Engine event callback. Pass to `engine.run_stream()`.
    graph::GraphStreamCallback cb;

    OpenInferenceTracerSession();
    ~OpenInferenceTracerSession();

    // Non-copyable, movable.
    OpenInferenceTracerSession(const OpenInferenceTracerSession&) = delete;
    OpenInferenceTracerSession& operator=(const OpenInferenceTracerSession&) = delete;
    OpenInferenceTracerSession(OpenInferenceTracerSession&&) noexcept;
    OpenInferenceTracerSession& operator=(OpenInferenceTracerSession&&) noexcept;

    /// End the root span + any pending node spans. Idempotent.
    void close();

    /// Internal — exposed so `OpenInferenceProvider` can open LLM
    /// child spans under the currently-active node span.
    Span* current_parent() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend OpenInferenceTracerSession openinference_tracer(
        Tracer&, std::string, std::string);
};

/// Open a CHAIN-kind root span and return a session whose `cb` opens
/// per-node CHAIN child spans + LLM_TOKEN events under it.
///
/// @param tracer            Tracer adapter. Must outlive the session.
/// @param root_name         Span name for the root (default "graph.run").
/// @param node_span_prefix  Prefix for per-node spans (default "node.").
NEOGRAPH_API OpenInferenceTracerSession openinference_tracer(
    Tracer& tracer,
    std::string root_name = "graph.run",
    std::string node_span_prefix = "node.");

/**
 * @brief Provider wrapper that emits OpenInference LLM spans.
 *
 * Pass-through to the inner Provider for all four virtual paths
 * (complete / complete_async / complete_stream / complete_stream_async).
 * Each call opens an `llm.complete` child span, attaches the
 * OpenInference LLM-kind attribute set, runs the inner call, captures
 * the response + usage onto the span, and ends it. Streaming overloads
 * additionally append each token to `output.value` and emit a
 * `llm.token` span event per chunk so Phoenix's timeline view shows
 * stream cadence.
 *
 * Tracing failures are caught and swallowed — observability must
 * never break the LLM call.
 */
class NEOGRAPH_API OpenInferenceProvider : public Provider {
public:
    /// @param inner         Provider to delegate to.
    /// @param tracer        Tracer for span emission. Must outlive `*this`.
    /// @param parent_lookup Optional callback returning the current node
    ///                      span the LLM call should nest under (e.g.
    ///                      bound to an `OpenInferenceTracerSession::current_parent`).
    ///                      May be null — null parent opens the LLM
    ///                      span as a root.
    /// @param span_name     Span name for each LLM call (default "llm.complete").
    OpenInferenceProvider(std::shared_ptr<Provider> inner,
                          Tracer& tracer,
                          std::function<Span*()> parent_lookup = nullptr,
                          std::string span_name = "llm.complete");
    ~OpenInferenceProvider() override;

    ChatCompletion complete(const CompletionParams& params) override;

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override;

    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;

    asio::awaitable<ChatCompletion>
    complete_stream_async(const CompletionParams& params,
                          const StreamCallback& on_chunk) override;

    std::string get_name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::observability
