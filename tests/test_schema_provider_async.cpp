// Cancellation coverage for SchemaProvider's non-streaming transports and
// blocking HTTP/SSE bridge. Each case holds a local HTTP response indefinitely;
// completion before release proves cancellation reached the active socket.

#include <gtest/gtest.h>

#include <neograph/graph/cancel.h>
#include <neograph/llm/schema_provider.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace neograph;

namespace {

constexpr const char* kResponse = R"({
    "id": "chatcmpl-schema-cancel",
    "choices": [{
        "message": {"role": "assistant", "content": "pong"},
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}
})";

struct HoldingServer {
    httplib::Server svr;
    std::thread t;
    int port = 0;
    std::atomic<int> request_count{0};
    std::atomic<bool> hold{true};

    HoldingServer() {
        svr.Post("/v1/chat/completions",
                 [this](const httplib::Request&, httplib::Response& res) {
                     request_count.fetch_add(1, std::memory_order_release);
                     while (hold.load(std::memory_order_acquire)) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                     }
                     res.status = 200;
                     res.set_content(kResponse, "application/json");
                 });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~HoldingServer() {
        hold.store(false, std::memory_order_release);
        svr.stop();
        if (t.joinable()) t.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

CompletionParams make_params() {
    CompletionParams params;
    params.model = "gpt-4o-mini";
    params.messages.push_back(ChatMessage{"user", "ping"});
    return params;
}

void expect_socket_cancel(bool prefer_libcurl) {
    HoldingServer server;
    ASSERT_GT(server.port, 0);

    llm::SchemaProvider::Config config;
    config.schema_path = "openai";
    config.api_key = "test-key";
    config.default_model = "gpt-4o-mini";
    config.base_url_override = server.base_url();
    config.timeout_seconds = 5;
    config.prefer_libcurl = prefer_libcurl;
    auto provider = llm::SchemaProvider::create(config);

    auto params = make_params();
    auto token = std::make_shared<graph::CancelToken>();
    params.cancel_token = token;

    asio::io_context io;
    std::promise<asio::error_code> completion;
    auto result = completion.get_future();
    graph::CancelExecutorLease token_lease(token);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            token->bind_executor(co_await asio::this_coro::executor);
            (void)co_await provider->complete_async(params);
            completion.set_value(asio::error::fault);
        } catch (const asio::system_error& error) {
            completion.set_value(error.code());
        } catch (...) {
            completion.set_value(asio::error::fault);
        }
    }, asio::bind_cancellation_slot(token->slot(), asio::detached));
    std::thread runner([&] { io.run(); });

    const auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.request_count.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.request_count.load(std::memory_order_acquire) != 1) {
        server.hold.store(false, std::memory_order_release);
        runner.join();
        ADD_FAILURE() << "provider request did not reach the local server";
        return;
    }

    token->cancel();
    const auto completion_status = result.wait_for(std::chrono::seconds(1));
    if (completion_status != std::future_status::ready) {
        server.hold.store(false, std::memory_order_release);
        runner.join();
        ADD_FAILURE() << "provider cancellation waited for the held response";
        return;
    }
    EXPECT_EQ(result.get(), asio::error::operation_aborted);

    server.hold.store(false, std::memory_order_release);
    runner.join();
}

void expect_stream_socket_cancel() {
    HoldingServer server;
    ASSERT_GT(server.port, 0);

    llm::SchemaProvider::Config config;
    config.schema_path = "openai";
    config.api_key = "test-key";
    config.default_model = "gpt-4o-mini";
    config.base_url_override = server.base_url();
    config.timeout_seconds = 5;
    auto provider = llm::SchemaProvider::create(config);

    auto params = make_params();
    auto token = std::make_shared<graph::CancelToken>();
    params.cancel_token = token;

    asio::io_context io;
    std::promise<std::exception_ptr> completion;
    auto result = completion.get_future();
    graph::CancelExecutorLease token_lease(token);
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            token->bind_executor(co_await asio::this_coro::executor);
            (void)co_await provider->complete_stream_async(params, StreamCallback{});
            completion.set_value(nullptr);
        } catch (...) {
            completion.set_value(std::current_exception());
        }
    }, asio::detached);
    std::thread runner([&] { io.run(); });

    const auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (server.request_count.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (server.request_count.load(std::memory_order_acquire) != 1) {
        server.hold.store(false, std::memory_order_release);
        runner.join();
        ADD_FAILURE() << "provider stream did not reach the local server";
        return;
    }

    token->cancel();
    const auto completion_status = result.wait_for(std::chrono::seconds(1));
    if (completion_status != std::future_status::ready) {
        server.hold.store(false, std::memory_order_release);
        runner.join();
        ADD_FAILURE() << "provider stream cancellation waited for the held response";
        return;
    }
    const auto error = result.get();
    ASSERT_NE(error, nullptr);
    EXPECT_THROW(std::rethrow_exception(error), graph::CancelledException);

    server.hold.store(false, std::memory_order_release);
    runner.join();
}

} // namespace

TEST(SchemaProviderAsync, CancelTokenAbortsConnPoolSocket) {
    expect_socket_cancel(false);
}

TEST(SchemaProviderAsync, CancelTokenAbortsHttpSseSocket) {
    expect_stream_socket_cancel();
}

#if defined(NEOGRAPH_TESTS_HAVE_LIBCURL)
TEST(SchemaProviderAsync, CancelTokenAbortsCurlH2PoolSocket) {
    expect_socket_cancel(true);
}
#endif
