// Regression coverage for the Provider sync↔async crossover defaults
// (Stage 3 / Semester 2.1).
//
// Provider declares both complete() and complete_async() as non-pure
// virtuals; each has a default that delegates to the other. Concrete
// subclasses override at least one. This test fixture pins both
// directions of the bridge:
//   * SyncOnlyProvider overrides complete(); calling complete_async()
//     must resolve to the same ChatCompletion without deadlocking.
//   * AsyncOnlyProvider overrides complete_async(); calling complete()
//     must block on run_sync() and return the async result — including
//     exception propagation.

#include <gtest/gtest.h>
#include <neograph/provider.h>
#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <stdexcept>

using neograph::ChatCompletion;
using neograph::ChatMessage;
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

// Overrides only the synchronous path; complete_async() comes from the
// default (which co_returns complete()).
class SyncOnlyProvider : public Provider {
  public:
    std::atomic<int> sync_calls{0};

    ChatCompletion complete(const CompletionParams& /*params*/) override {
        ++sync_calls;
        return make_completion("sync");
    }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback&) override {
        return {};
    }

    std::string get_name() const override { return "sync-only"; }
};

// Overrides only the async path; complete() comes from the default
// (which calls run_sync on complete_async()).
class AsyncOnlyProvider : public Provider {
  public:
    std::atomic<int> async_calls{0};

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& /*params*/) override {
        ++async_calls;
        co_return make_completion("async");
    }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback&) override {
        return {};
    }

    std::string get_name() const override { return "async-only"; }
};

// Async-only path that throws — used to prove run_sync re-raises.
class ThrowingAsyncProvider : public Provider {
  public:
    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& /*params*/) override {
        throw std::runtime_error("boom");
        co_return ChatCompletion{};  // unreachable, keeps it a coroutine
    }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback&) override {
        return {};
    }

    std::string get_name() const override { return "throwing-async"; }
};

} // namespace

TEST(ProviderAsyncDefault, SyncOverrideBridgesToAsync) {
    SyncOnlyProvider p;
    CompletionParams params;

    // complete() obviously works — call it once to baseline.
    auto direct = p.complete(params);
    EXPECT_EQ(direct.message.content, "sync");
    EXPECT_EQ(p.sync_calls.load(), 1);

    // complete_async() with no override goes through the default body
    // (co_return complete(params)). Drive it to completion.
    auto result = run_sync(p.complete_async(params));
    EXPECT_EQ(result.message.content, "sync");
    EXPECT_EQ(p.sync_calls.load(), 2);
}

TEST(ProviderAsyncDefault, AsyncOverrideBridgesToSync) {
    AsyncOnlyProvider p;
    CompletionParams params;

    // complete() with no override goes through the default body
    // (run_sync(complete_async(params))). Blocks and returns.
    auto result = p.complete(params);
    EXPECT_EQ(result.message.content, "async");
    EXPECT_EQ(p.async_calls.load(), 1);
}

TEST(ProviderAsyncDefault, AsyncOverrideUsableDirectlyOnCallerIoContext) {
    // The important production case: caller already has an io_context
    // and wants to co_await complete_async() without the sync bridge.
    // The overridden coroutine must compose normally under co_spawn.
    AsyncOnlyProvider p;
    CompletionParams params;
    asio::io_context io;

    ChatCompletion out;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            out = co_await p.complete_async(params);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(out.message.content, "async");
    EXPECT_EQ(p.async_calls.load(), 1);
}

TEST(ProviderAsyncDefault, SyncDefaultRethrowsAsyncException) {
    ThrowingAsyncProvider p;
    CompletionParams params;

    EXPECT_THROW(p.complete(params), std::runtime_error);
}
