// Wire-protocol coverage for OpenAIProvider after Stage 3 / Semester 2.3.
//
// The provider now implements complete_async() over neograph::async::
// async_post; the legacy sync complete() override is gone, so sync
// callers route through the base-class run_sync bridge. These tests
// pin three things end-to-end against a local httplib server:
//   1. complete_async() resolves to a parsed ChatCompletion when the
//      server returns a normal 200 OpenAI body.
//   2. complete() (inherited bridge) returns the same result on the
//      caller's thread.
//   3. A 429 with Retry-After surfaces as a typed RateLimitError
//      carrying the integer wait — required by RateLimitedProvider.

#include <gtest/gtest.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/async/run_sync.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/io_context.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace neograph;

namespace {

constexpr const char* kOpenAIBody = R"({
    "id": "chatcmpl-xyz",
    "object": "chat.completion",
    "model": "gpt-4o-mini",
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": "pong"
        },
        "finish_reason": "stop"
    }],
    "usage": {
        "prompt_tokens": 7,
        "completion_tokens": 2,
        "total_tokens": 9
    }
})";

struct MockServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;
    std::atomic<int> request_count{0};
    int             status = 200;
    std::string     body  = kOpenAIBody;
    std::string     retry_after;  // header value when status == 429

    MockServer() {
        svr.Post("/v1/chat/completions",
                 [this](const httplib::Request&, httplib::Response& res) {
                     request_count.fetch_add(1, std::memory_order_relaxed);
                     res.status = status;
                     if (!retry_after.empty()) {
                         res.set_header("Retry-After", retry_after);
                     }
                     res.set_content(body, "application/json");
                 });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~MockServer() {
        svr.stop();
        if (t.joinable()) t.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

llm::OpenAIProvider::Config make_config(const MockServer& mock) {
    llm::OpenAIProvider::Config cfg;
    cfg.api_key = "test-key";
    cfg.base_url = mock.base_url();
    cfg.default_model = "gpt-4o-mini";
    cfg.timeout_seconds = 5;
    return cfg;
}

CompletionParams make_params() {
    CompletionParams p;
    p.model = "gpt-4o-mini";
    ChatMessage u;
    u.role = "user";
    u.content = "ping";
    p.messages.push_back(u);
    return p;
}

} // namespace

TEST(OpenAIProviderAsync, CompleteAsyncReturnsParsedCompletion) {
    MockServer mock;
    ASSERT_GT(mock.port, 0);

    auto provider = llm::OpenAIProvider::create(make_config(mock));
    auto params = make_params();

    asio::io_context io;
    ChatCompletion result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            result = co_await provider->complete_async(params);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(result.message.role, "assistant");
    EXPECT_EQ(result.message.content, "pong");
    EXPECT_EQ(result.usage.prompt_tokens, 7);
    EXPECT_EQ(result.usage.completion_tokens, 2);
    EXPECT_EQ(result.usage.total_tokens, 9);
    EXPECT_EQ(mock.request_count.load(), 1);
}

TEST(OpenAIProviderAsync, SyncCompleteBridgesThroughRunSync) {
    MockServer mock;
    ASSERT_GT(mock.port, 0);

    auto provider = llm::OpenAIProvider::create(make_config(mock));
    auto params = make_params();

    auto result = provider->complete(params);

    EXPECT_EQ(result.message.content, "pong");
    EXPECT_EQ(result.usage.total_tokens, 9);
    EXPECT_EQ(mock.request_count.load(), 1);
}

TEST(OpenAIProviderAsync, RateLimitErrorCarriesRetryAfter) {
    MockServer mock;
    mock.status = 429;
    mock.retry_after = "12";
    mock.body = R"({"error":"rate limited"})";

    auto provider = llm::OpenAIProvider::create(make_config(mock));
    auto params = make_params();

    try {
        provider->complete(params);
        FAIL() << "expected RateLimitError";
    } catch (const RateLimitError& e) {
        EXPECT_EQ(e.retry_after_seconds(), 12);
    } catch (const std::exception& e) {
        FAIL() << "expected RateLimitError, got: " << e.what();
    }
}

TEST(OpenAIProviderAsync, NonRateLimitErrorSurfacesAsRuntimeError) {
    MockServer mock;
    mock.status = 500;
    mock.body = R"({"error":"server boom"})";

    auto provider = llm::OpenAIProvider::create(make_config(mock));
    auto params = make_params();

    EXPECT_THROW(provider->complete(params), std::runtime_error);
}
