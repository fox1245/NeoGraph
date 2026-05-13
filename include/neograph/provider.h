/**
 * @file provider.h
 * @brief Abstract LLM provider interface.
 *
 * Defines the Provider base class that all LLM backends must implement.
 * Supports both synchronous and streaming completions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/types.h>

#include <asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace neograph::graph { class CancelToken; }

namespace neograph {

/**
 * @brief Thrown by a Provider when an upstream API returned HTTP 429
 *        (rate limit exceeded).
 *
 * This is a typed exception so decorators like `RateLimitedProvider`
 * can catch it specifically and apply backoff, without fragile string
 * parsing of a generic error message.
 *
 * The `retry_after_seconds()` value is the upstream's `Retry-After`
 * header in seconds, or -1 if no usable Retry-After was present.
 * Decorators should prefer the honest value when positive and fall
 * back to their own default when -1.
 */
class NEOGRAPH_API RateLimitError : public std::runtime_error {
public:
    RateLimitError(const std::string& message, int retry_after_seconds = -1)
        : std::runtime_error(message)
        , retry_after_seconds_(retry_after_seconds) {}

    /// @brief Seconds to wait per the upstream, or -1 if unknown.
    int retry_after_seconds() const noexcept { return retry_after_seconds_; }

private:
    int retry_after_seconds_;
};

/// Callback invoked per token during streaming completion.
/// @param chunk The token or text chunk received from the LLM.
using StreamCallback = std::function<void(const std::string& chunk)>;

/**
 * @brief Parameters for an LLM completion request.
 */
struct CompletionParams {
    std::string model;                  ///< Model name to use (e.g., "gpt-4o-mini").
    std::vector<ChatMessage> messages;  ///< Conversation history.
    std::vector<ChatTool> tools;        ///< Available tools the LLM may call.
    float temperature = 0.7f;           ///< Sampling temperature (0.0 = deterministic, 1.0 = creative).
    int max_tokens = -1;                ///< Maximum output tokens. -1 means provider default.

    /**
     * @brief Optional cancel handle (v0.3+).
     *
     * When set, async-native providers (OpenAIProvider, SchemaProvider)
     * bind ``cancel_token->slot()`` to their ``ConnPool::async_post``
     * co_await, so a caller's ``cancel()`` aborts the in-flight HTTPS
     * socket and stops billable LLM work mid-stream.
     *
     * The engine populates this from ``RunConfig::cancel_token`` when
     * a node calls ``ctx.provider->complete(params)``; user code that
     * constructs CompletionParams directly may set it to share an
     * abort across multiple completions.
     */
    std::shared_ptr<graph::CancelToken> cancel_token;

    /**
     * @brief Per-call body field bindings (issue #33, v0.8+).
     *
     * Map of `path → value` overrides that the provider stamps into
     * the outgoing request body for THIS call only. Companion to
     * the schema-static `request.extra_fields` block that ships
     * with every body — `extra_fields` here is the dynamic per-call
     * side of the same hook.
     *
     * For ``SchemaProvider``: the schema author declares which paths
     * are bindable per-call via `"request.per_call_fields": [...]`.
     * Only listed paths are honoured; unknown paths are silently
     * dropped (the schema, not the caller, owns the contract). This
     * keeps the per-call surface declarative and matches how
     * `temperature_path` / `max_tokens_path` already address specific
     * paths.
     *
     * Native ``Provider`` subclasses (e.g. ``OpenAIProvider``) may
     * choose to honour or ignore individual keys per their own
     * documented surface — the field is generic, the contract is
     * provider-defined.
     *
     * @code
     * // Reasoning model — high effort for hard call, low for cheap call.
     * CompletionParams hard;
     * hard.extra_fields = {{"reasoning.effort", "high"}};
     * auto deep = co_await provider->complete_async(hard);
     *
     * CompletionParams cheap;
     * cheap.extra_fields = {{"reasoning.effort", "low"}};
     * auto fast = co_await provider->complete_async(cheap);
     * @endcode
     *
     * Default: empty json. Same lifecycle shape as the existing
     * schema-static `request.extra_fields` block.
     */
    json extra_fields;
};

/**
 * @brief Abstract base class for LLM providers.
 *
 * Subclass this to integrate any LLM backend (OpenAI, Claude, Gemini, etc.).
 * Both synchronous and streaming completion must be implemented.
 *
 * @see neograph::llm::OpenAIProvider
 * @see neograph::llm::SchemaProvider
 */
class NEOGRAPH_API Provider {
  public:
    virtual ~Provider() = default;

    /**
     * @brief Perform a synchronous LLM completion.
     *
     * Default implementation bridges to `complete_async()` via an
     * internal io_context (see neograph::async::run_sync). Subclasses
     * written against the sync path override this directly; async-
     * native subclasses override `complete_async()` and inherit this.
     *
     * @param params Completion parameters including model, messages, and tools.
     * @return The full completion response with message and usage statistics.
     */
    virtual ChatCompletion complete(const CompletionParams& params);

    /**
     * @brief Perform an LLM completion as a coroutine.
     *
     * Returns an `asio::awaitable` that resolves to the completion
     * response. The awaitable does no I/O on the caller's thread —
     * resume it on an io_context to run the request.
     *
     * Default implementation delegates to the synchronous `complete()`
     * (runs on whatever thread resumes the coroutine — caller's I/O
     * loop will block on HTTP). Subclasses that perform async HTTP
     * should override this to co_await non-blocking operations; when
     * they do, `complete()` transparently bridges via `run_sync()`.
     *
     * @note Override at least one of `complete` / `complete_async`.
     * Overriding neither results in infinite mutual recursion when
     * the method is called.
     *
     * @param params Completion parameters.
     * @return An awaitable yielding the full completion response.
     */
    virtual asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params);

    /**
     * @brief Perform a streaming LLM completion.
     *
     * Calls @p on_chunk for each token as it arrives, then returns the
     * full assembled completion when done.
     *
     * Default implementation calls @ref complete and forwards the full
     * assembled message content as a single chunk — sufficient for
     * mocks, unit-test fixtures, and non-streaming-native providers
     * that just want to satisfy the streaming surface. Streaming-native
     * subclasses (OpenAI, schema-driven, etc.) override this to emit
     * tokens incrementally.
     *
     * @param params Completion parameters.
     * @param on_chunk Callback invoked per received token.
     * @return The full completion response after streaming is complete.
     */
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk);

    /**
     * @brief Async streaming completion. Awaitable peer of @ref complete_stream.
     *
     * Default implementation (post-#4): spawns a dedicated worker
     * thread that runs the synchronous `complete_stream`, dispatches
     * each token onto the awaiting coroutine's executor (so the
     * user's `on_chunk` runs single-threaded with the awaiter — no
     * reentrancy), and resumes the coroutine via a one-shot
     * `steady_timer.cancel()` posted on the awaiter's executor when
     * streaming finishes. Subclasses with a fully async streaming
     * transport (WebSocket Responses, native SSE coroutine, etc.)
     * SHOULD override this to drop the worker thread and stream
     * tokens straight onto the coroutine's executor.
     *
     * @note Pre-#4 the default was `co_return complete_stream(...)`
     * inline, which blocked the awaiting executor for the whole
     * stream and — when `complete_stream` itself called `run_sync()`
     * — nested two io_contexts on the same thread, racing on shared
     * provider state. The current default avoids both: the executor
     * stays responsive, and `complete_stream` runs on its own thread
     * with no implicit io_context reentry. `SchemaProvider` overrides
     * the WebSocket Responses branch to skip even the worker thread
     * (it's already async-native).
     *
     * @note **Subclass override contract for the streaming pair**
     * (mirrors the non-streaming `complete` / `complete_async` pair
     * above): subclasses MUST override at least one of
     * `complete_stream` / `complete_stream_async`. Subclasses whose
     * native sync `complete_stream` itself drives a `run_sync()` on
     * an internal `io_context` (the WebSocket Responses path in
     * `SchemaProvider` is the canonical example) MUST override
     * `complete_stream_async` directly to expose the async-native
     * peer — relying on the default bridge is functional but spawns
     * an extra worker thread per call. Subclasses whose
     * `complete_stream` is purely synchronous (e.g. blocking httplib)
     * can leave the default bridge in place — it routes the sync work
     * onto a worker thread and dispatches tokens back onto the
     * awaiter's executor.
     *
     * @note **`asio::io_context.run()` placement for the awaiter**
     * (issue #16): drive the outer `asio::io_context.run()` from
     * your application's main thread or a long-lived worker thread.
     * Nesting `io.run()` inside an HTTP server per-request callback
     * (e.g. httplib's `set_chunked_content_provider` lambda) has been
     * observed to SEGV in `getaddrinfo` under the per-request
     * worker-thread spawn this default bridge issues, on some
     * glibc/OpenSSL combinations. See `docs/concepts.md` §8 for
     * tested-good shapes and the two recommended workarounds.
     *
     * @param params   Completion parameters.
     * @param on_chunk Callback invoked per received token (runs on the
     *                 awaiting coroutine's executor — never on the
     *                 internal worker thread).
     * @return Awaitable resolving to the full completion response.
     */
    virtual asio::awaitable<ChatCompletion>
    complete_stream_async(const CompletionParams& params,
                          const StreamCallback& on_chunk);

    /**
     * @brief Single-dispatch async-streaming completion (v1.0 canonical).
     *
     * The unified entry point that future v1.0+ Provider subclasses
     * override. Replaces the current `(sync/async) × (stream/non-stream)`
     * 4-virtual cross-product (`complete` / `complete_async` /
     * `complete_stream` / `complete_stream_async`) with one async-
     * streaming-superset method. ROADMAP_v1.md Candidate 6.
     *
     * Semantics:
     *   - `on_chunk == nullptr` → caller wants the full assembled
     *     completion only; provider may skip streaming framing.
     *   - `on_chunk != nullptr` → caller wants tokens incrementally;
     *     provider invokes `on_chunk` on each chunk AND returns the
     *     full assembled completion when the stream ends.
     *
     * The returned awaitable resolves on the caller's executor; the
     * `on_chunk` callback runs there too (single-threaded with the
     * awaiter, same invariant `complete_stream_async` already provides).
     *
     * **Deprecation strategy** (Candidate 6 PR sequence):
     *   - This PR (additive): `invoke()` lands as a new virtual; default
     *     forwards to the legacy 4-virtual chain (`complete_stream_async`
     *     when `on_chunk` is set, `complete_async` otherwise) so every
     *     existing Provider subclass keeps working unchanged.
     *   - Subsequent PR: native subclasses (`OpenAIProvider`,
     *     `SchemaProvider`, `RateLimitedProvider`) override `invoke()`
     *     directly; their old 4 overrides become thin adapters.
     *   - Subsequent PR: 4 legacy virtuals get `[[deprecated]]` markers.
     *   - v1.0.0: 4 legacy virtuals removed; `invoke()` becomes the
     *     only Provider virtual besides `get_name()`.
     *
     * **Override contract**: subclasses overriding `invoke()` MUST NOT
     * call any of the 4 legacy `complete_*` methods on themselves —
     * those default to forwarding to `invoke()` (in a future PR), which
     * would recurse infinitely. New code: override `invoke()` and only
     * `invoke()`. Old code: keep overriding the 4 legacy methods; the
     * default `invoke()` here delegates to them.
     *
     * @param params   Completion parameters (model, messages, tools, ...).
     * @param on_chunk Optional per-chunk callback. `nullptr` for non-
     *                 streaming use.
     * @return Awaitable yielding the full assembled completion.
     */
    virtual asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& params, StreamCallback on_chunk = nullptr);

    /**
     * @brief Get the provider name (e.g., "openai", "claude").
     *
     * **Opaque debug identifier**, not a typed dispatch key. Different
     * subclasses pick different conventions:
     *   - `OpenAIProvider` always returns `"openai"`.
     *   - `SchemaProvider` returns whatever's in the schema's `name`
     *     field — could be `"openai"`, `"claude"`, `"openai-responses"`,
     *     `"gemini"`, or a user-defined schema id.
     *   - `RateLimitedProvider` delegates to its inner provider.
     *
     * Code branching on the exact string (e.g. `if (get_name() ==
     * "openai-responses")`) is brittle — a custom schema named
     * `"openai-responses-v2"` slips through, or a future rename
     * silently breaks the branch. Use it for logging, telemetry, or
     * version-pinning diagnostics. For typed behaviour, add a typed
     * `ProviderKind` accessor or branch on the schema's actual
     * fields.
     *
     * @return Opaque provider identifier string.
     */
    virtual std::string get_name() const = 0;
};

} // namespace neograph
