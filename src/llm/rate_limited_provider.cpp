#include <neograph/llm/rate_limited_provider.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
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

// Decide how long to sleep before retry number `attempt` (0-based) of a
// retryable ProviderError. Prefer the upstream's Retry-After when it is
// positive. Without one, a rate limit (429) waits cfg.default_wait_seconds --
// the quota window is long -- while any other transient failure (500, 502,
// 503, 529, an in-stream `overloaded_error`) backs off exponentially from
// cfg.transient_base_wait_seconds, since a server hiccup clears in seconds.
// Always cap at cfg.max_wait_seconds so a bad value (or clock skew on the
// server side) can't stall the caller past a reasonable bound.
static int decide_sleep_seconds(int status, int retry_after_seconds, int attempt,
                                const RateLimitedProvider::Config& cfg) {
    const bool rate_limited = status == 429;
    long long s = retry_after_seconds;
    if (s <= 0) {
        if (rate_limited) {
            s = cfg.default_wait_seconds;
        } else if (cfg.transient_base_wait_seconds <= 0) {
            s = 0;
        } else {
            // base * 2^attempt without overflowing: stop doubling at the cap.
            s = cfg.transient_base_wait_seconds;
            for (int i = 0; i < attempt && s <= cfg.max_wait_seconds; ++i) s *= 2;
            s = std::min<long long>(s, cfg.max_wait_seconds);
        }
    }
    if (s > cfg.max_wait_seconds) return -1;  // too long; abort retry
    // A rate limit gets +1s of slack so we don't miss the reset boundary by
    // racing it; a plain backoff does not need it.
    return static_cast<int>(rate_limited ? s + 1 : s);
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
        // Stage 3 / Sem 1.5 conn_pool work). Either we got a result or we
        // caught a retryable typed error; the original exception object is
        // kept so the caller still sees its concrete type (RateLimitError)
        // if we give up. Non-retryable failures propagate normally.
        std::optional<ChatCompletion> result;
        std::exception_ptr failure;
        int retry_after = -1;
        int failure_status = 0;
        try {
            result.emplace(co_await inner_->complete_async(params));
        } catch (const ProviderError& e) {
            if (!e.retryable()) throw;
            retry_after = e.retry_after_seconds();
            failure_status = e.status();
            failure = std::current_exception();
        }

        if (result) co_return std::move(*result);

        // `failure` is populated since we got past the try-block without
        // re-throwing. Decide whether to retry, then sleep on an asio
        // timer so the io_context isn't blocked.
        if (attempt >= cfg_.max_retries) std::rethrow_exception(failure);
        int wait = decide_sleep_seconds(failure_status, retry_after, attempt, cfg_);
        if (wait < 0) std::rethrow_exception(failure);

        // max_total_wait_seconds budget check: refuse to sleep past it.
        if (cfg_.max_total_wait_seconds > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed + wait > cfg_.max_total_wait_seconds) std::rethrow_exception(failure);
        }

        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::seconds(wait));
        co_await timer.async_wait(asio::use_awaitable);
    }
}

ChatCompletion
RateLimitedProvider::complete_stream(const CompletionParams& params,
                                     const StreamCallback& on_chunk) {
    // Same contract as complete_async but on the sync streaming path, with
    // one extra rule: a stream is retried only while nothing has reached
    // `on_chunk`. A retry replays the response from its first token, so once a
    // chunk was delivered (an `overloaded_error` mid-stream, a cut connection)
    // retrying would hand the caller duplicate output it cannot tell apart from
    // new output. After the first delivered chunk the error propagates instead.
    bool delivered = false;
    StreamCallback guarded;
    if (on_chunk) {
        guarded = [&delivered, &on_chunk](const std::string& chunk) {
            delivered = true;
            on_chunk(chunk);
        };
    }
    auto start = std::chrono::steady_clock::now();
    for (int attempt = 0;; ++attempt) {
        try {
            return inner_->complete_stream(params, guarded ? guarded : on_chunk);
        } catch (const ProviderError& e) {
            if (delivered || !e.retryable() || attempt >= cfg_.max_retries) throw;
            int wait = decide_sleep_seconds(e.status(), e.retry_after_seconds(), attempt, cfg_);
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

// Compatibility callback-selected override. It routes through the stable
// existing overrides so each path keeps its retry/backoff behavior.
asio::awaitable<ChatCompletion>
RateLimitedProvider::invoke(const CompletionParams& params, StreamCallback on_chunk) {
    if (on_chunk) {
        // Sync streaming retry — bridge to async via the base default.
        co_return co_await Provider::complete_stream_async(params, on_chunk);
    }
    co_return co_await complete_async(params);
}

} // namespace neograph::llm
