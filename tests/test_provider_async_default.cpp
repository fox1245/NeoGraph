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
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

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

// Issue #22 regression: a mock provider that overrides ONLY complete()
// must build (no stub for complete_stream needed) and the inherited
// default complete_stream must forward the assembled content as a
// single chunk.
class MockNoStreamOverrideProvider : public Provider {
  public:
    ChatCompletion complete(const CompletionParams& /*params*/) override {
        return make_completion("mock-out");
    }
    std::string get_name() const override { return "mock"; }
};

} // namespace

TEST(ProviderAsyncDefault, DefaultCompleteStreamForwardsAsSingleChunk) {
    MockNoStreamOverrideProvider p;
    CompletionParams params;

    std::vector<std::string> chunks;
    auto result = p.complete_stream(
        params,
        [&](const std::string& c) { chunks.push_back(c); });

    EXPECT_EQ(result.message.content, "mock-out");
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks.front(), "mock-out");
}

TEST(ProviderAsyncDefault, DefaultCompleteStreamHandlesNullCallback) {
    MockNoStreamOverrideProvider p;
    CompletionParams params;
    auto result = p.complete_stream(params, StreamCallback{});
    EXPECT_EQ(result.message.content, "mock-out");
}

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

// ---------------------------------------------------------------------------
// Issue #4 regression — `complete_stream_async` default bridge MUST NOT
// block the awaiter's executor and MUST deliver chunks on the awaiter's
// executor (not the internal worker thread).
//
// Pre-#4 the default bridge was `co_return complete_stream(...)`, which
// blocked the engine's io_context worker for the whole stream and fired
// the user's on_chunk on the same thread mid-coroutine. Reproducing that
// in test form: a slow streaming Provider plus a concurrent coroutine
// that advances a counter every io_context tick. Pre-#4 the counter
// stays at 0 until the stream completes; post-#4 it advances.
// ---------------------------------------------------------------------------

class SlowStreamingProvider : public Provider {
  public:
    int chunk_count = 5;
    std::chrono::milliseconds chunk_interval{20};
    std::atomic<std::thread::id> stream_thread_id{};
    std::atomic<int> chunks_emitted{0};

    ChatCompletion complete(const CompletionParams&) override {
        return make_completion("");
    }

    asio::awaitable<ChatCompletion>
    complete_async(const CompletionParams& p) override {
        co_return complete(p);
    }

    ChatCompletion complete_stream(const CompletionParams& /*params*/,
                                   const StreamCallback& on_chunk) override {
        // Sleeping in a sync streamer mirrors httplib's blocking recv
        // loop. If the post-#4 bridge is correct this thread is NOT
        // the awaiter's io_context worker.
        stream_thread_id.store(std::this_thread::get_id());
        std::string acc;
        for (int i = 0; i < chunk_count; ++i) {
            std::this_thread::sleep_for(chunk_interval);
            std::string chunk = "tok" + std::to_string(i);
            acc += chunk;
            on_chunk(chunk);
            ++chunks_emitted;
        }
        return make_completion(acc);
    }

    std::string get_name() const override { return "slow-stream"; }
};

TEST(ProviderAsyncDefault, StreamAsyncBridgeDoesNotBlockExecutor) {
    SlowStreamingProvider p;
    p.chunk_count = 5;
    p.chunk_interval = std::chrono::milliseconds(20);
    CompletionParams params;

    asio::io_context io;

    std::atomic<int> ticker{0};
    std::vector<std::string> received_chunks;
    std::vector<std::thread::id> chunk_thread_ids;
    ChatCompletion final_result;

    // A "ticker" coroutine that wakes every 5ms and bumps a counter.
    // If complete_stream_async blocks the executor, ticker stops while
    // the stream is running.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 50; ++i) {
                asio::steady_timer t(co_await asio::this_coro::executor);
                t.expires_after(std::chrono::milliseconds(5));
                co_await t.async_wait(asio::use_awaitable);
                ++ticker;
            }
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            final_result = co_await p.complete_stream_async(
                params,
                [&](const std::string& chunk) {
                    received_chunks.push_back(chunk);
                    chunk_thread_ids.push_back(std::this_thread::get_id());
                });
        },
        asio::detached);

    auto io_thread_id = std::this_thread::get_id();
    io.run();

    // Chunks delivered.
    EXPECT_EQ(received_chunks.size(), 5u);
    EXPECT_EQ(final_result.message.content, "tok0tok1tok2tok3tok4");

    // Chunks ran on the io_context thread — same thread that called
    // io.run() — NOT on the bridge's worker thread.
    auto worker_tid = p.stream_thread_id.load();
    EXPECT_NE(worker_tid, io_thread_id);
    for (auto tid : chunk_thread_ids) {
        EXPECT_EQ(tid, io_thread_id);
        EXPECT_NE(tid, worker_tid);
    }

    // Ticker advanced WHILE the stream was running. With chunk_count=5
    // × chunk_interval=20ms ≈ 100ms total, the 5ms ticker would land
    // ~20 times. Allow generous slack for CI jitter — anything > 5
    // proves the executor wasn't blocked end-to-end.
    EXPECT_GT(ticker.load(), 5);
}

class ThrowingStreamingProvider : public Provider {
  public:
    ChatCompletion complete(const CompletionParams&) override { return {}; }

    ChatCompletion complete_stream(const CompletionParams&,
                                   const StreamCallback&) override {
        throw std::runtime_error("stream-boom");
    }

    std::string get_name() const override { return "throwing-stream"; }
};

TEST(ProviderAsyncDefault, StreamAsyncBridgeRethrowsWorkerException) {
    ThrowingStreamingProvider p;
    CompletionParams params;
    asio::io_context io;

    std::exception_ptr caught;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await p.complete_stream_async(
                    params, [](const std::string&) {});
            } catch (...) {
                caught = std::current_exception();
            }
        },
        asio::detached);

    io.run();

    ASSERT_TRUE(caught);
    EXPECT_THROW(std::rethrow_exception(caught), std::runtime_error);
}
