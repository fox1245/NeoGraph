// ROADMAP_v1.md Candidate 6 PR3 — Provider::invoke() cancel propagation
// parity with the legacy sync `Provider::complete()`.
//
// `Provider::complete()` (legacy sync) reads the engine's
// thread-local `current_cancel_token()` so a node calling
// `provider->complete(params)` without explicitly setting
// `params.cancel_token` still gets the running graph's cancel signal.
// `Provider::invoke()` MUST mirror that fallback because PR3 migrates
// internal sync call sites (agent.cpp, deep_research_graph.cpp) from
// `complete()` to `run_sync(invoke())` — losing the thread-local would
// silently drop cancel propagation on every node-side LLM call.
//
// Coverage:
//   1. thread_local set, params.cancel_token unset → invoke() stamps
//      effective.cancel_token from the thread_local
//   2. thread_local set, params.cancel_token set → params.cancel_token
//      wins (explicit overrides implicit)
//   3. thread_local unset, params.cancel_token unset → fast path,
//      effective.cancel_token stays nullptr (no copy needed)

#include <gtest/gtest.h>
#include <neograph/provider.h>
#include <neograph/graph/cancel.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <memory>

using neograph::ChatCompletion;
using neograph::CompletionParams;
using neograph::Provider;
using neograph::StreamCallback;
using neograph::async::run_sync;
using neograph::graph::CancelToken;
using neograph::graph::CurrentCancelTokenScope;
using neograph::graph::current_cancel_token;

namespace {

// Records which cancel_token (if any) reached complete_async — proxy
// for "what did invoke() stamp into effective.cancel_token before
// forwarding". Provider override chain bottoms out at complete_async
// when on_chunk == nullptr (matches PR1 default routing).
class TokenInspectingProvider : public Provider {
  public:
    std::shared_ptr<CancelToken> seen_token;
    bool was_called = false;

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& params) override {
        was_called = true;
        seen_token = params.cancel_token;
        ChatCompletion c;
        c.message.role = "assistant";
        c.message.content = "ok";
        co_return c;
    }

    std::string get_name() const override { return "token-inspect"; }
};

} // namespace

TEST(InvokeCancelPropagation, ThreadLocalStampedWhenParamsUnset) {
    TokenInspectingProvider p;
    auto engine_token = std::make_shared<CancelToken>();
    CurrentCancelTokenScope scope(engine_token.get());
    ASSERT_EQ(current_cancel_token(), engine_token.get());

    CompletionParams params;
    ASSERT_FALSE(params.cancel_token);

    auto result = run_sync(p.invoke(params, nullptr));

    EXPECT_TRUE(p.was_called);
    EXPECT_EQ(result.message.content, "ok");
    ASSERT_TRUE(p.seen_token);
    // The engine's thread_local raw pointer was wrapped into a
    // non-owning shared_ptr inside invoke(). The .get() must point
    // back to the engine token (same instance).
    EXPECT_EQ(p.seen_token.get(), engine_token.get());
}

TEST(InvokeCancelPropagation, ExplicitParamsTokenWinsOverThreadLocal) {
    TokenInspectingProvider p;
    auto engine_token = std::make_shared<CancelToken>();
    auto explicit_token = std::make_shared<CancelToken>();
    CurrentCancelTokenScope scope(engine_token.get());

    CompletionParams params;
    params.cancel_token = explicit_token;

    auto result = run_sync(p.invoke(params, nullptr));

    EXPECT_TRUE(p.was_called);
    ASSERT_TRUE(p.seen_token);
    EXPECT_EQ(p.seen_token.get(), explicit_token.get());
    EXPECT_NE(p.seen_token.get(), engine_token.get());
}

TEST(InvokeCancelPropagation, NoThreadLocalFastPathLeavesTokenNull) {
    TokenInspectingProvider p;
    // No CurrentCancelTokenScope on this thread.
    ASSERT_EQ(current_cancel_token(), nullptr);

    CompletionParams params;
    ASSERT_FALSE(params.cancel_token);

    auto result = run_sync(p.invoke(params, nullptr));

    EXPECT_TRUE(p.was_called);
    EXPECT_FALSE(p.seen_token);
}
