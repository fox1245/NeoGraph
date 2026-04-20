#include <neograph/llm/rate_limited_provider.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

namespace neograph::llm {

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
    for (int attempt = 0;; ++attempt) {
        try {
            return inner_->complete_stream(params, on_chunk);
        } catch (const RateLimitError& e) {
            if (attempt >= cfg_.max_retries) throw;
            int wait = decide_sleep_seconds(e, cfg_);
            if (wait < 0) throw;
            std::this_thread::sleep_for(std::chrono::seconds(wait));
        }
    }
}

} // namespace neograph::llm
