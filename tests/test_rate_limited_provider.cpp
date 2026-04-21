// Regression coverage for RateLimitedProvider — the Provider decorator
// that honours RateLimitError (HTTP 429 with Retry-After) by sleeping
// and retrying on behalf of the caller.
//
// The decorator exists because framework-level providers (e.g.
// SchemaProvider) stay policy-free: they surface a typed RateLimitError
// on 429 but never sleep or retry themselves. Users whose backends
// don't rate-limit pay nothing for this mechanism; users who DO face
// rate limits opt in by wrapping their provider. Deep Research wires
// the wrapper by default because it fans out parallel researchers that
// regularly trip Anthropic's minute-window caps.

#include <gtest/gtest.h>
#include <neograph/llm/rate_limited_provider.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

using namespace neograph;

namespace {

// Test double: controllable inner provider. Each complete() call pops
// a programmed response off the script and either succeeds or throws.
class ScriptedProvider : public Provider {
public:
    struct Step {
        enum class Kind { Ok, RateLimit, OtherError };
        Kind kind;
        int retry_after_seconds = -1;  // only for RateLimit
    };

    explicit ScriptedProvider(std::vector<Step> steps)
        : steps_(std::move(steps)) {}

    ChatCompletion complete(const CompletionParams&) override {
        int i = calls_.fetch_add(1, std::memory_order_relaxed);
        if (i >= (int)steps_.size()) {
            throw std::runtime_error("scripted provider exhausted");
        }
        const auto& s = steps_[i];
        switch (s.kind) {
            case Step::Kind::Ok: {
                ChatCompletion c;
                c.message.role = "assistant";
                c.message.content = "ok #" + std::to_string(i);
                return c;
            }
            case Step::Kind::RateLimit:
                throw RateLimitError("rate limited (mock)",
                                     s.retry_after_seconds);
            case Step::Kind::OtherError:
                throw std::runtime_error("other failure (mock)");
        }
        throw std::runtime_error("unreachable");
    }

    ChatCompletion complete_stream(const CompletionParams& p,
                                   const StreamCallback&) override {
        return complete(p);
    }

    std::string get_name() const override { return "scripted"; }

    int call_count() const { return calls_.load(); }

private:
    std::vector<Step> steps_;
    std::atomic<int>  calls_{0};
};

using K = ScriptedProvider::Step::Kind;

std::shared_ptr<ScriptedProvider> make_inner(std::vector<ScriptedProvider::Step> s) {
    return std::make_shared<ScriptedProvider>(std::move(s));
}

} // namespace

TEST(RateLimitedProvider, PassThroughOnSuccess) {
    auto inner = make_inner({{K::Ok, 0}});
    auto wrapped = llm::RateLimitedProvider::create(inner);
    auto r = wrapped->complete({});
    EXPECT_EQ("ok #0", r.message.content);
    EXPECT_EQ(1, inner->call_count());
}

TEST(RateLimitedProvider, SleepsThenRetriesOnRateLimit) {
    // Retry-After of 1s — a real 429 followed by an OK.
    auto inner = make_inner({
        {K::RateLimit, 1},
        {K::Ok,        0}
    });
    auto wrapped = llm::RateLimitedProvider::create(inner);

    auto t0 = std::chrono::steady_clock::now();
    auto r = wrapped->complete({});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ("ok #1", r.message.content);
    EXPECT_EQ(2, inner->call_count());
    EXPECT_GE(elapsed, 900) << "decorator returned too fast; sleep skipped";
    EXPECT_LT(elapsed, 5000);
}

TEST(RateLimitedProvider, FallsBackToDefaultWaitWhenRetryAfterMissing) {
    // retry_after_seconds = -1 → decorator uses cfg.default_wait_seconds.
    auto inner = make_inner({
        {K::RateLimit, -1},
        {K::Ok,        0}
    });
    llm::RateLimitedProvider::Config cfg;
    cfg.default_wait_seconds = 1;  // keep the test fast
    cfg.max_retries = 3;
    auto wrapped = llm::RateLimitedProvider::create(inner, cfg);

    auto t0 = std::chrono::steady_clock::now();
    auto r = wrapped->complete({});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ("ok #1", r.message.content);
    EXPECT_GE(elapsed, 900);
    EXPECT_LT(elapsed, 5000);
}

TEST(RateLimitedProvider, SurfacesErrorAfterMaxRetries) {
    // 3 consecutive 429s, max_retries = 2 → third call throws.
    auto inner = make_inner({
        {K::RateLimit, 1},
        {K::RateLimit, 1},
        {K::RateLimit, 1}
    });
    llm::RateLimitedProvider::Config cfg;
    cfg.max_retries = 2;
    auto wrapped = llm::RateLimitedProvider::create(inner, cfg);

    EXPECT_THROW(wrapped->complete({}), RateLimitError);
    EXPECT_EQ(3, inner->call_count())
        << "expected initial call + max_retries attempts";
}

TEST(RateLimitedProvider, RespectsMaxWaitSeconds) {
    // Retry-After of 600s is pathological; max_wait_seconds = 10 rejects it
    // and the decorator should NOT sleep — it propagates the error so the
    // caller can decide.
    auto inner = make_inner({
        {K::RateLimit, 600}
    });
    llm::RateLimitedProvider::Config cfg;
    cfg.max_retries = 3;
    cfg.max_wait_seconds = 10;
    auto wrapped = llm::RateLimitedProvider::create(inner, cfg);

    auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(wrapped->complete({}), RateLimitError);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5) << "decorator slept past its max_wait cap";
    EXPECT_EQ(1, inner->call_count())
        << "decorator should have aborted without retrying";
}

TEST(RateLimitedProvider, PassesNonRateLimitErrorsThrough) {
    auto inner = make_inner({
        {K::OtherError, 0}
    });
    auto wrapped = llm::RateLimitedProvider::create(inner);
    EXPECT_THROW(wrapped->complete({}), std::runtime_error);
    EXPECT_EQ(1, inner->call_count());
}

TEST(RateLimitedProvider, NullInnerRejected) {
    EXPECT_THROW(
        llm::RateLimitedProvider::create(nullptr),
        std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Async path (Stage 3 / Semester 2.5)
//
// The Retry-After sleep migrated from std::this_thread::sleep_for to an
// asio::steady_timer co_await. The behavioural guarantees are unchanged
// (the existing tests above verify this through the sync facade), but
// the new property is "the wait does not block other work scheduled on
// the same io_context". This case pins exactly that.

#include <neograph/async/run_sync.h>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <optional>

TEST(RateLimitedProviderAsync, RetrySleepDoesNotBlockOtherCoroutines) {
    // Inner: 429(Retry-After=1s) -> Ok. Wrapper sleeps 2s (1s + 1s slack).
    auto inner = make_inner({
        {K::RateLimit, 1},
        {K::Ok, 0}
    });
    auto wrapped = llm::RateLimitedProvider::create(inner);

    asio::io_context io;

    std::optional<ChatCompletion> wrapped_result;
    auto driver = [&]() -> asio::awaitable<void> {
        wrapped_result.emplace(co_await wrapped->complete_async({}));
    };

    // Ticks every 100ms while the wait is in flight. If the rate-limit
    // sleep blocks the io_context, this can't tick.
    std::atomic<int> ticks{0};
    auto ticker = [&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        for (int i = 0; i < 25; ++i) {
            asio::steady_timer t(ex);
            t.expires_after(std::chrono::milliseconds(100));
            co_await t.async_wait(asio::use_awaitable);
            ticks.fetch_add(1, std::memory_order_relaxed);
            if (wrapped_result) co_return;
        }
    };

    asio::co_spawn(io, driver(), asio::detached);
    asio::co_spawn(io, ticker(), asio::detached);

    auto t0 = std::chrono::steady_clock::now();
    io.run();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    ASSERT_TRUE(wrapped_result.has_value());
    EXPECT_EQ(wrapped_result->message.content, "ok #1");
    EXPECT_EQ(inner->call_count(), 2);
    // Wrapper waits ~2s; ticker should have fired ~20 times if non-
    // blocking. Allow generous slack against scheduling jitter — anything
    // >= 5 proves the io_context wasn't frozen the whole time. A blocking
    // sleep would yield 0 ticks before the driver resumes.
    EXPECT_GE(ticks.load(), 5)
        << "ticker fired " << ticks.load() << " times in "
        << elapsed_ms << " ms — async sleep appears to block io_context";
}

TEST(RateLimitedProviderAsync, AsyncBubblesNonRateLimitErrors) {
    auto inner = make_inner({{K::OtherError, 0}});
    auto wrapped = llm::RateLimitedProvider::create(inner);

    EXPECT_THROW(
        neograph::async::run_sync(wrapped->complete_async({})),
        std::runtime_error);
    EXPECT_EQ(1, inner->call_count());
}
