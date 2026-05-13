#include <neograph/llm/rate_limited_provider.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

namespace neograph::llm {

// Candidate 6 PR4: inner Provider's 4 legacy virtuals are now
// [[deprecated]]. RateLimitedProvider is a delegating decorator that
// wraps the legacy surface for retry/backoff; it MUST keep forwarding
// to the inner's legacy methods through the deprecation window.
// Bracket the whole TU so internal forwarders don't drown in self-
// warnings; v1.0 collapses these to a single invoke() override.
NEOGRAPH_PUSH_IGNORE_DEPRECATED

RateLimitedProvider::RateLimitedProvider(std::shared_ptr<Provider> inner,
                                         Config cfg)
    : inner_(std::move(inner)), cfg_(cfg) {
    if (!inner_) {
        throw std::invalid_argument(
            "RateLimitedProvider: inner provider must not be null");
    }
}

std::unique_ptr<RateLimitedProvider>
RateLimitedProvider::create(std::shared_ptr<Provider> inner, Config cfg) {
    return std::unique_ptr<RateLimitedProvider>(
        new RateLimitedProvider(std::move(inner), cfg));
}

std::string RateLimitedProvider::get_name() const {
    return inner_->get_name();
}

// Decide how long to sleep for a RateLimitError. Prefer the upstream's
// Retry-After when sane; fall back to cfg_.default_wait_seconds. Always
// cap at cfg_.max_wait_seconds so a bad value (or clock skew on the
// server side) can't stall the caller past a reasonable bound.
static int decide_sleep_seconds(const neograph::RateLimitError& e,
                                const RateLimitedProvider::Config& cfg) {
    int s = e.retry_after_seconds();
    if (s <= 0) s = cfg.default_wait_seconds;
    if (s > cfg.max_wait_seconds) return -1;  // too long; abort retry
    // +1s of slack so we don't miss the reset boundary by racing it.
    return s + 1;
}

asio::awaitable<ChatCompletion>
RateLimitedProvider::complete_async(const CompletionParams& params) {
    auto ex = co_await asio::this_coro::executor;

    // Track total elapsed wall-clock so cfg_.max_total_wait_seconds
    // can cap stacked retries (e.g. 5 × 60s would otherwise stall
    // for 5 minutes).
    auto start = std::chrono::steady_clock::now();

    for (int attempt = 0;; ++attempt) {
        // Capture the outcome of one inner call without doing a co_await
        // inside a catch block — GCC 13 ICEs on that shape (verified in
        // Stage 3 / Sem 1.5 conn_pool work). The two optionals are
        // mutually exclusive: either we got a result or we caught a
        // typed rate-limit error. Other exceptions propagate normally.
        std::optional<ChatCompletion> result;
        std::optional<RateLimitError> rate_err;
        try {
            result.emplace(co_await inner_->complete_async(params));
        } catch (const RateLimitError& e) {
            rate_err.emplace(e);
        }

        if (result) co_return std::move(*result);

        // rate_err must be populated since we got past the try-block
        // without re-throwing. Decide whether to retry, then sleep on
        // an asio timer so the io_context isn't blocked.
        const auto& e = *rate_err;
        if (attempt >= cfg_.max_retries) throw e;
        int wait = decide_sleep_seconds(e, cfg_);
        if (wait < 0) throw e;

        // max_total_wait_seconds budget check: refuse to sleep past it.
        if (cfg_.max_total_wait_seconds > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed + wait > cfg_.max_total_wait_seconds) throw e;
        }

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::seconds(wait));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

ChatCompletion
RateLimitedProvider::complete_stream(const CompletionParams& params,
                                     const StreamCallback& on_chunk) {
    // Same contract as complete_async but on the sync streaming path.
    // Note that if the 429 happens mid-stream (after some tokens have
    // already been emitted via on_chunk), a retry will re-emit tokens
    // from the start — callers that assume "each callback invocation
    // is unique output" need to be aware. In practice HTTP 429 is
    // returned before any response body, so this case is rare.
    auto start = std::chrono::steady_clock::now();
    for (int attempt = 0;; ++attempt) {
        try {
            return inner_->complete_stream(params, on_chunk);
        } catch (const RateLimitError& e) {
            if (attempt >= cfg_.max_retries) throw;
            int wait = decide_sleep_seconds(e, cfg_);
            if (wait < 0) throw;
            if (cfg_.max_total_wait_seconds > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed + wait > cfg_.max_total_wait_seconds) throw;
            }
            std::this_thread::sleep_for(std::chrono::seconds(wait));
        }
    }
}

NEOGRAPH_POP_IGNORE_DEPRECATED

} // namespace neograph::llm
