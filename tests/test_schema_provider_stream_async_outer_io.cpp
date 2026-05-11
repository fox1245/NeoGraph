// Issue #4 downstream verification — drives `SchemaProvider("openai_responses")`
// HTTP/SSE through the post-#10 native `complete_stream_async` override
// from inside an outer `asio::io_context` + `co_spawn`, mirroring the
// ProjectDatePop demo/cpp_backend handler shape.
//
// Pre-#10 this exact path segfaulted (the default bridge ran the sync
// `complete_stream` inline on the engine's io_context worker, and for
// the WS branch additionally nested a fresh `run_sync` on top — both
// modes ended up racing on shared SchemaProvider state).
//
// Post-#10:
//   - HTTP/SSE branch → `Provider::complete_stream_async`'s new default
//     (dedicated worker thread for sync `complete_stream`, tokens
//     dispatched back onto the awaiter's executor via `asio::dispatch`).
//   - WS branch → `complete_stream_ws_responses` co_awaited directly,
//     no worker thread (separate test:
//     `test_schema_provider_ws_responses.cpp`).
//
// The synthetic `SlowStreamingProvider` test in
// `test_provider_async_default.cpp` proves the bridge mechanics. This
// test pins the *real* code path (the actual SchemaProvider machinery
// with its schema_mutex_, ConnPool, httplib client, SSE_EVENTS parser)
// against the actual failure shape from the issue: outer asio loop +
// co_spawn coroutine + co_await complete_stream_async.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;

namespace {

struct ResponsesMock {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    explicit ResponsesMock(std::string sse_body) {
        svr.Post("/v1/responses",
            [body = std::move(sse_body)]
            (const httplib::Request&, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(body, "text/event-stream");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~ResponsesMock() { svr.stop(); if (t.joinable()) t.join(); }
};

llm::SchemaProvider::Config cfg_for(int port) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path       = "openai_responses";
    cfg.api_key           = "test-key";
    cfg.default_model     = "gpt-test";
    cfg.timeout_seconds   = 10;
    cfg.base_url_override = "http://127.0.0.1:" + std::to_string(port);
    return cfg;
}

// Korean tokens — pinning the user's actual repro language so any
// UTF-8 truncation or encoding regression surfaces here.
constexpr const char* kKoreanFull = "안녕하세요, 반갑습니다";
constexpr const char* kKoreanChunks[] = {
    "안녕", "하세요, ", "반갑", "습니다"};

std::string make_korean_sse() {
    std::string body =
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
              "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
              "\"role\":\"assistant\"}}\n"
        "\n";
    for (const auto* chunk : kKoreanChunks) {
        body += "event: response.output_text.delta\n";
        body += "data: {\"type\":\"response.output_text.delta\","
                       "\"item_id\":\"msg_1\",\"delta\":\"";
        body += chunk;
        body += "\"}\n\n";
    }
    body +=
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0}\n"
        "\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\","
              "\"response\":{\"id\":\"resp_1\","
              "\"usage\":{\"input_tokens\":5,\"output_tokens\":10,"
                         "\"total_tokens\":15}}}\n"
        "\n";
    return body;
}

CompletionParams params_with(std::string user_msg) {
    CompletionParams p;
    p.model = "gpt-test";
    ChatMessage u; u.role = "user"; u.content = std::move(user_msg);
    p.messages.push_back(u);
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Issue #4 downstream verification — single outer-coroutine repro.
// ---------------------------------------------------------------------------

TEST(SchemaProviderStreamAsyncOuterIo, KoreanResponseStreamsViaOuterCoSpawn) {
    ResponsesMock mock{make_korean_sse()};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    asio::io_context io;
    std::vector<std::string> chunks;
    std::mutex chunks_mu;
    std::string final_content;
    std::exception_ptr caught;
    auto io_thread = std::this_thread::get_id();
    std::atomic<bool> chunk_off_io_thread{false};

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                auto p = params_with("ping");
                auto r = co_await provider->complete_stream_async(
                    p,
                    [&](const std::string& tok) {
                        // Same invariant the post-#10 fix guarantees:
                        // on_chunk runs on the awaiter's executor —
                        // here, io_thread. Anything else is a regression.
                        if (std::this_thread::get_id() != io_thread) {
                            chunk_off_io_thread.store(true);
                        }
                        std::lock_guard<std::mutex> lock(chunks_mu);
                        chunks.push_back(tok);
                    });
                final_content = r.message.content;
            } catch (...) {
                caught = std::current_exception();
            }
        },
        asio::detached);

    io.run();

    ASSERT_FALSE(caught) << "co_await complete_stream_async threw";
    EXPECT_FALSE(chunk_off_io_thread.load())
        << "on_chunk fired off the awaiter's io_thread — bridge regressed";
    EXPECT_EQ(final_content, kKoreanFull);
    ASSERT_EQ(chunks.size(), std::size(kKoreanChunks));
    for (size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(chunks[i], kKoreanChunks[i])
            << "chunk #" << i << " content mismatch";
    }
}

TEST(SchemaProviderStreamAsyncOuterIo, OuterIoStaysResponsiveDuringStream) {
    // Companion to KoreanResponseStreamsViaOuterCoSpawn: the outer
    // io_context worker MUST NOT be blocked for the duration of the
    // stream. Verifies the post-#10 worker-thread bridge by running a
    // concurrent ticker coroutine that wakes every 5 ms while the
    // streaming complete_stream_async is in flight.
    ResponsesMock mock{make_korean_sse()};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    asio::io_context io;
    std::atomic<int> ticker{0};
    std::atomic<bool> stream_done{false};

    // Ticker coroutine: every 5ms increment a counter until the stream
    // coroutine signals done. Pre-#10 this would stall while the
    // engine's io_context worker thread was blocked inside the inline
    // sync httplib loop.
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            while (!stream_done.load()) {
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
            auto p = params_with("ping");
            (void)co_await provider->complete_stream_async(
                p, [](const std::string&) {});
            stream_done.store(true);
        },
        asio::detached);

    io.run();

    EXPECT_TRUE(stream_done.load());
    // The streaming HTTP roundtrip + SSE parse takes some ms (httplib
    // localhost roundtrip is typically a few ms, plus the worker
    // thread-spawn overhead). Even on a fast machine the ticker
    // should land at least a few times. Pre-#10 ticker would be 0.
    EXPECT_GT(ticker.load(), 0)
        << "outer io_context worker stayed blocked through the stream";
}

TEST(SchemaProviderStreamAsyncOuterIo, ConcurrentOuterCoroutinesDoNotRace) {
    // Reinforces #6 Gap 2: concurrent complete_stream_async calls on
    // ONE provider must not race on schema_mutex_-protected state nor
    // crash. Pre-#4 fix this combined with the outer io_context to
    // produce the segfault. Now: each call gets its own worker thread,
    // each on_chunk dispatches back to the awaiter's executor (single
    // shared io_context), and the per-call parse state stays local.
    ResponsesMock mock{make_korean_sse()};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    ASSERT_TRUE(provider);

    asio::io_context io;
    constexpr int kCallers = 6;
    std::atomic<int> succeeded{0};
    std::vector<std::string> finals(kCallers);

    for (int i = 0; i < kCallers; ++i) {
        asio::co_spawn(
            io,
            [&, i]() -> asio::awaitable<void> {
                auto p = params_with("ping " + std::to_string(i));
                auto r = co_await provider->complete_stream_async(
                    p, [](const std::string&) {});
                finals[i] = r.message.content;
                ++succeeded;
            },
            asio::detached);
    }

    io.run();

    EXPECT_EQ(succeeded.load(), kCallers);
    for (int i = 0; i < kCallers; ++i) {
        EXPECT_EQ(finals[i], kKoreanFull) << "caller " << i;
    }
}
