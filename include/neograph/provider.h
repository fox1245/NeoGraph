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
     * @param params Completion parameters.
     * @param on_chunk Callback invoked per received token.
     * @return The full completion response after streaming is complete.
     */
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk) = 0;

    /**
     * @brief Get the provider name (e.g., "openai", "claude").
     * @return Provider identifier string.
     */
    virtual std::string get_name() const = 0;
};

} // namespace neograph
