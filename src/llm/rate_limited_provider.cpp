#include <neograph/llm/rate_limited_provider.h>

#include <chrono>
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

ChatCompletion
RateLimitedProvider::complete(const CompletionParams& params) {
    for (int attempt = 0;; ++attempt) {
        try {
            return inner_->complete(params);
        } catch (const RateLimitError& e) {
            if (attempt >= cfg_.max_retries) throw;
            int wait = decide_sleep_seconds(e, cfg_);
            if (wait < 0) throw;  // retry-after too long; surface error
            std::this_thread::sleep_for(std::chrono::seconds(wait));
        }
    }
}

ChatCompletion
RateLimitedProvider::complete_stream(const CompletionParams& params,
                                     const StreamCallback& on_chunk) {
    // Same contract as complete(). Note that if the 429 happens mid-stream
    // (after some tokens have already been emitted via on_chunk), a retry
    // will re-emit tokens from the start — callers that assume "each
    // callback invocation is unique output" need to be aware. In practice
    // HTTP 429 is returned before any response body, so this case is rare.
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
