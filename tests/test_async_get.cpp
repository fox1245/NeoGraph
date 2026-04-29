// Coverage for the async::async_get + build_request(method=) plumbing
// added alongside the A2A client. The MCP/LLM POST paths are well-tested
// already, but the GET path was new — these tests prove:
//   1. async_get sends a real "GET /path HTTP/1.1\r\nHost: ..." line.
//   2. No Content-Length / Content-Type are emitted on a GET (servers
//      that reject GET-with-body would otherwise refuse).
//   3. POST behavior is unchanged (regression net for build_request's
//      new method parameter — default still "POST").

#include <gtest/gtest.h>

#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace neograph;

namespace {

struct CapturingServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;
    std::atomic<int>            request_count{0};
    std::string                 last_method;
    std::string                 last_path;
    std::string                 last_body;
    httplib::Headers            last_headers;

    CapturingServer() {
        svr.Get("/foo", [this](const httplib::Request& req, httplib::Response& res) {
            record(req, "GET");
            res.status = 200;
            res.set_content(R"({"got":"foo"})", "application/json");
        });
        svr.Post("/bar", [this](const httplib::Request& req, httplib::Response& res) {
            record(req, "POST");
            res.status = 200;
            res.set_content(R"({"got":"bar"})", "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~CapturingServer() { svr.stop(); if (t.joinable()) t.join(); }

  private:
    void record(const httplib::Request& req, const char* method) {
        request_count.fetch_add(1, std::memory_order_relaxed);
        last_method  = method;
        last_path    = req.path;
        last_body    = req.body;
        last_headers = req.headers;
    }
};

TEST(AsyncGet, SendsGetMethodAndNoBody) {
    CapturingServer srv;
    auto host = std::string("127.0.0.1");
    auto port = std::to_string(srv.port);

    async::HttpResponse resp;
    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        resp = co_await async::async_get(ex, host, port, "/foo");
    }());

    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body,   R"({"got":"foo"})");
    EXPECT_EQ(srv.last_method, "GET");
    EXPECT_EQ(srv.last_path,   "/foo");
    EXPECT_TRUE(srv.last_body.empty());

    // Crucially, no Content-Length or Content-Type headers should be
    // sent on a GET — servers that strictly enforce REST semantics
    // (notably some agent endpoints) reject GET-with-body otherwise.
    EXPECT_EQ(srv.last_headers.count("Content-Length"), 0u);
    EXPECT_EQ(srv.last_headers.count("Content-Type"),   0u);
}

TEST(AsyncGet, ForwardsCustomHeaders) {
    CapturingServer srv;
    auto host = std::string("127.0.0.1");
    auto port = std::to_string(srv.port);

    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer test-token"},
            {"X-Custom",      "yes"},
        };
        co_await async::async_get(ex, host, port, "/foo", std::move(headers));
    }());

    auto auth = srv.last_headers.find("Authorization");
    auto cust = srv.last_headers.find("X-Custom");
    ASSERT_NE(auth, srv.last_headers.end());
    ASSERT_NE(cust, srv.last_headers.end());
    EXPECT_EQ(auth->second, "Bearer test-token");
    EXPECT_EQ(cust->second, "yes");
}

TEST(AsyncPostUnchanged, StillPostsWithContentLengthAndType) {
    // Regression: build_request gained a `method` parameter. Ensure
    // the default POST path still emits Content-Length and (when no
    // explicit Content-Type passed) defaults to application/json.
    CapturingServer srv;
    auto host = std::string("127.0.0.1");
    auto port = std::to_string(srv.port);

    async::HttpResponse resp;
    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        resp = co_await async::async_post(ex, host, port, "/bar",
                                          /*body=*/R"({"hello":"world"})");
    }());

    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(srv.last_method, "POST");
    EXPECT_EQ(srv.last_path,   "/bar");
    EXPECT_EQ(srv.last_body,   R"({"hello":"world"})");

    auto cl = srv.last_headers.find("Content-Length");
    auto ct = srv.last_headers.find("Content-Type");
    ASSERT_NE(cl, srv.last_headers.end());
    EXPECT_EQ(cl->second, std::to_string(srv.last_body.size()));
    ASSERT_NE(ct, srv.last_headers.end());
    EXPECT_EQ(ct->second, "application/json");
}

TEST(AsyncGet, SurfacesNon200Status) {
    httplib::Server svr;
    svr.Get("/oops", [](const httplib::Request&, httplib::Response& res) {
        res.status = 503;
        res.set_content("transient", "text/plain");
    });
    auto port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    for (int i = 0; i < 200 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    async::HttpResponse resp;
    async::run_sync([&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        resp = co_await async::async_get(ex, std::string("127.0.0.1"),
                                         std::to_string(port), "/oops");
    }());

    EXPECT_EQ(resp.status, 503);
    EXPECT_EQ(resp.body,   "transient");

    svr.stop();
    if (t.joinable()) t.join();
}

}  // namespace
