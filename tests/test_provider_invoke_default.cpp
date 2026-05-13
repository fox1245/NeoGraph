// ROADMAP_v1.md Candidate 6 — `Provider::invoke()` regression coverage.
//
// `invoke(params, on_chunk)` is the v1.0 canonical single-dispatch
// entry point that replaces the 4-virtual cross-product
// (`complete` / `complete_async` / `complete_stream` /
// `complete_stream_async`). This first additive PR adds `invoke()`
// with a default body that forwards to the legacy chain so existing
// Provider subclasses keep working unchanged. These tests pin the
// forwarding semantics and the new override surface.

#include <gtest/gtest.h>
#include <neograph/provider.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <string>
#include <vector>

using neograph::ChatCompletion;
using neograph::CompletionParams;
using neograph::Provider;
using neograph::StreamCallback;
using neograph::async::run_sync;

namespace {

ChatCompletion make_completion(const std::string& text) {
    ChatCompletion c;
    c.message.role = "assistant";
    c.message.content = text;
    return c;
}

// Legacy provider — overrides only complete_async() and complete_stream(),
// the two pieces of the existing chain. Default invoke() must route to
// the right one based on on_chunk presence.
class LegacyChainProvider : public Provider {
  public:
    std::atomic<int> async_calls{0};
    std::atomic<int> stream_sync_calls{0};

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& /*params*/) override {
        ++async_calls;
        co_return make_completion("legacy-async");
    }

    ChatCompletion complete_stream(const CompletionParams& /*params*/,
                                   const StreamCallback& on_chunk) override {
        ++stream_sync_calls;
        if (on_chunk) on_chunk("chunk-1");
        return make_completion("legacy-stream");
    }

    std::string get_name() const override { return "legacy-chain"; }
};

// New-style provider — overrides invoke() directly. Legacy 4 virtuals
// are NOT overridden. Engine code that already calls the legacy methods
// gets the base-class default, which (in this PR) forwards through the
// chain — but if the user calls invoke() directly, the override fires.
class InvokeNativeProvider : public Provider {
  public:
    std::atomic<int> invoke_calls{0};
    std::atomic<int> chunks_emitted{0};

    asio::awaitable<ChatCompletion>
    invoke(const CompletionParams& /*params*/, StreamCallback on_chunk) override {
        ++invoke_calls;
        if (on_chunk) {
            on_chunk("native-chunk-A");
            on_chunk("native-chunk-B");
            chunks_emitted += 2;
        }
        co_return make_completion("native-invoke");
    }

    // Required: get_name is the only other pure virtual on Provider.
    std::string get_name() const override { return "invoke-native"; }
};

} // namespace

// ---------------------------------------------------------------------------
// Default invoke() routes to the legacy chain — no on_chunk → complete_async.
// ---------------------------------------------------------------------------

TEST(ProviderInvokeDefault, NoCallbackRoutesToCompleteAsync) {
    LegacyChainProvider p;
    CompletionParams params;

    auto result = run_sync(p.invoke(params, nullptr));

    EXPECT_EQ(result.message.content, "legacy-async");
    EXPECT_EQ(p.async_calls.load(), 1);
    EXPECT_EQ(p.stream_sync_calls.load(), 0);
}

TEST(ProviderInvokeDefault, EmptyCallbackTreatedAsNoCallback) {
    // StreamCallback is std::function — a default-constructed instance is
    // falsy, so the `if (on_chunk)` branch in the default invoke() must
    // route to complete_async, not complete_stream_async.
    LegacyChainProvider p;
    CompletionParams params;

    auto result = run_sync(p.invoke(params, StreamCallback{}));

    EXPECT_EQ(result.message.content, "legacy-async");
    EXPECT_EQ(p.async_calls.load(), 1);
    EXPECT_EQ(p.stream_sync_calls.load(), 0);
}

// ---------------------------------------------------------------------------
// Default invoke() routes to the legacy chain — on_chunk present →
// complete_stream_async (which itself bridges to complete_stream on a
// worker thread per the existing default).
// ---------------------------------------------------------------------------

TEST(ProviderInvokeDefault, WithCallbackRoutesToCompleteStreamAsync) {
    LegacyChainProvider p;
    CompletionParams params;

    std::vector<std::string> chunks;
    auto result = run_sync(p.invoke(
        params,
        [&](const std::string& c) { chunks.push_back(c); }));

    // The legacy default complete_stream_async spawns a worker thread that
    // invokes the sync complete_stream and dispatches chunks back onto the
    // awaiter's executor — same path as the established #4 fix. After
    // run_sync drains the io_context the chunk MUST have landed.
    EXPECT_EQ(result.message.content, "legacy-stream");
    EXPECT_EQ(p.stream_sync_calls.load(), 1);
    EXPECT_EQ(p.async_calls.load(), 0);
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks.front(), "chunk-1");
}

// ---------------------------------------------------------------------------
// New-style: subclass overrides invoke() directly. Calling invoke() must
// hit the override, not the legacy chain.
// ---------------------------------------------------------------------------

TEST(ProviderInvokeDefault, OverriddenInvokeFires) {
    InvokeNativeProvider p;
    CompletionParams params;

    auto result = run_sync(p.invoke(params, nullptr));

    EXPECT_EQ(result.message.content, "native-invoke");
    EXPECT_EQ(p.invoke_calls.load(), 1);
    EXPECT_EQ(p.chunks_emitted.load(), 0);
}

TEST(ProviderInvokeDefault, OverriddenInvokeDeliversChunks) {
    InvokeNativeProvider p;
    CompletionParams params;

    std::vector<std::string> chunks;
    auto result = run_sync(p.invoke(
        params,
        [&](const std::string& c) { chunks.push_back(c); }));

    EXPECT_EQ(result.message.content, "native-invoke");
    EXPECT_EQ(p.invoke_calls.load(), 1);
    EXPECT_EQ(p.chunks_emitted.load(), 2);
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0], "native-chunk-A");
    EXPECT_EQ(chunks[1], "native-chunk-B");
}

// ---------------------------------------------------------------------------
// invoke() composes under co_spawn on the caller's io_context — the
// production case where a node body co_awaits the provider directly.
// ---------------------------------------------------------------------------

TEST(ProviderInvokeDefault, AwaitableComposesOnCallerIoContext) {
    InvokeNativeProvider p;
    CompletionParams params;
    asio::io_context io;

    ChatCompletion out;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            out = co_await p.invoke(params, nullptr);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(out.message.content, "native-invoke");
    EXPECT_EQ(p.invoke_calls.load(), 1);
}
