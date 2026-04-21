/**
 * @file llm/rate_limited_provider.h
 * @brief Provider decorator that honours HTTP 429 Retry-After.
 *
 * Wraps any `Provider` and turns `RateLimitError` throws into a
 * bounded sleep+retry loop. The inner Provider stays policy-free;
 * users opt into this behaviour by wrapping only when they need it
 * (e.g., on low-tier Anthropic, not on a self-hosted vLLM).
 */
#pragma once

#include <neograph/provider.h>

#include <memory>
#include <string>

namespace neograph::llm {

/**
 * @brief Decorator that retries on RateLimitError according to its
 *        Retry-After hint.
 *
 * Behaviour:
 *   * Calls through to the inner provider.
 *   * On `RateLimitError`, sleeps for `retry_after_seconds()` + 1s
 *     (or `default_wait_seconds` if the upstream didn't send a
 *     usable Retry-After), then retries.
 *   * Capped at `max_retries` attempts. After that the final
 *     `RateLimitError` propagates to the caller.
 *   * `max_wait_seconds` caps each individual sleep so a pathological
 *     Retry-After (server misconfigured, clock skew) can't stall the
 *     process — the decorator returns control to the caller, which
 *     can decide whether to keep going.
 *
 * Non-rate-limit exceptions pass through untouched.
 *
 * @code
 * auto inner = SchemaProvider::create({.schema_path = "claude", ...});
 * auto provider = RateLimitedProvider::create(std::move(inner));
 * // use `provider` like any other Provider
 * @endcode
 */
class RateLimitedProvider : public Provider {
public:
    /// Configuration for rate-limit handling.
    struct Config {
        int max_retries          = 3;   ///< Number of additional attempts after a RateLimitError.
        int default_wait_seconds = 30;  ///< Sleep duration when Retry-After is absent or invalid.
        int max_wait_seconds     = 120; ///< Upper cap per sleep, prevents runaway stalls.
    };

    /**
     * @brief Build a decorator around an inner provider.
     * @param inner  The Provider to wrap. Must be non-null.
     * @param cfg    Retry configuration.
     * @return A unique_ptr to the decorator.
     */
    static std::unique_ptr<RateLimitedProvider> create(
        std::shared_ptr<Provider> inner, Config cfg);

    /// Convenience: build with default Config.
    static std::unique_ptr<RateLimitedProvider> create(
        std::shared_ptr<Provider> inner) {
        return create(std::move(inner), Config{});
    }

    /// Async completion that retries on RateLimitError with a non-
    /// blocking `asio::steady_timer` sleep. Sync `complete()` is
    /// inherited from `Provider` and routes through this via
    /// `run_sync` — the inner timer drives a fresh io_context, so
    /// caller-thread blocking semantics are preserved.
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override;

    /// Streaming retry path remains synchronous — `complete_stream()`
    /// has no awaitable peer on Provider, and async streaming through
    /// the schema/openai providers still uses httplib. Once those
    /// migrate this method can grow an awaitable cousin.
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;

    std::string get_name() const override;

private:
    RateLimitedProvider(std::shared_ptr<Provider> inner, Config cfg);

    std::shared_ptr<Provider> inner_;
    Config                    cfg_;
};

} // namespace neograph::llm
