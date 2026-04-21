// Integration tests for RequestOptions handling in async_post +
// ConnPool::async_post (Stage 3 Semester 1.5).
//
// Scope: redirect following, redirect-disabled pass-through, per-hop
// timeout, Retry-After extraction, pool-level timeout. All plain TCP
// against in-process asio mocks — TLS versions of the same code
// paths run through the http_client.cpp SSL branch which is already
// exercised by neograph_async_https_smoke.

#include <neograph/async/conn_pool.h>
#include <neograph/async/http_client.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/streambuf.hpp>
#include <asio/system_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Generic routed mock server: the caller passes a responder that
// maps the request path to a full HTTP/1.1 response string. The
// responder is invoked once per request; the TCP connection closes
// after writing the response (every response sends Connection:
// close, so pool reuse is suppressed by design in these tests).
struct RoutedMock {
    using Responder = std::function<std::string(const std::string& path)>;

    asio::io_context        io;
    asio::ip::tcp::acceptor acceptor{io};
    std::thread             worker;
    Responder               responder;
    std::atomic<int>        requests{0};
    unsigned short          port = 0;

    explicit RoutedMock(Responder r) : responder(std::move(r)) {
        acceptor.open(asio::ip::tcp::v4());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        asio::co_spawn(io, accept_loop(), asio::detached);
        worker = std::thread([this]{ io.run(); });
    }

    ~RoutedMock() {
        asio::error_code ec;
        acceptor.close(ec);
        io.stop();
        if (worker.joinable()) worker.join();
    }

    asio::awaitable<void> handle(asio::ip::tcp::socket sock) {
        try {
            asio::streambuf buf;
            co_await asio::async_read_until(
                sock, buf, "\r\n\r\n", asio::use_awaitable);

            std::istream is(&buf);
            std::string req_line;
            std::getline(is, req_line);
            if (!req_line.empty() && req_line.back() == '\r') req_line.pop_back();
            // POST /path HTTP/1.1
            std::string path = "/";
            {
                auto sp1 = req_line.find(' ');
                auto sp2 = req_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
                if (sp1 != std::string::npos && sp2 != std::string::npos) {
                    path = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
                }
            }

            long content_length = 0;
            std::string line;
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                auto f = value.find_first_not_of(" \t");
                if (f != std::string::npos) value = value.substr(f);
                for (auto& c : name)
                    c = static_cast<char>(std::tolower(
                        static_cast<unsigned char>(c)));
                if (name == "content-length") content_length = std::stol(value);
            }
            auto already = buf.size();
            if (static_cast<long>(already) < content_length) {
                long rem = content_length - static_cast<long>(already);
                std::vector<char> tail(rem);
                co_await asio::async_read(sock, asio::buffer(tail),
                    asio::transfer_exactly(rem), asio::use_awaitable);
            } else if (static_cast<long>(already) > content_length) {
                buf.consume(content_length);
            }

            ++requests;
            std::string resp = responder(path);
            co_await asio::async_write(sock, asio::buffer(resp),
                                       asio::use_awaitable);
        } catch (...) { }
        asio::error_code ec;
        sock.close(ec);
    }

    asio::awaitable<void> accept_loop() {
        for (;;) {
            asio::ip::tcp::socket sock{io};
            asio::error_code ec;
            co_await acceptor.async_accept(
                sock, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            asio::co_spawn(io, handle(std::move(sock)), asio::detached);
        }
    }
};

std::string http_response(int status, const std::string& body,
                          const std::string& extra_headers = {}) {
    std::string reason;
    switch (status) {
        case 200: reason = "OK"; break;
        case 301: reason = "Moved Permanently"; break;
        case 302: reason = "Found"; break;
        case 307: reason = "Temporary Redirect"; break;
        case 429: reason = "Too Many Requests"; break;
        case 500: reason = "Internal Server Error"; break;
        default:  reason = "Status"; break;
    }
    std::string out;
    out.append("HTTP/1.1 ").append(std::to_string(status))
       .append(" ").append(reason).append("\r\n");
    out.append("Content-Length: ").append(std::to_string(body.size()))
       .append("\r\n");
    out.append("Connection: close\r\n");
    out.append(extra_headers);
    out.append("\r\n");
    out.append(body);
    return out;
}

TEST(RequestOptions, RedirectFollowedWithinLimit) {
    RoutedMock srv([&](const std::string& path) {
        if (path == "/start") {
            return http_response(302, "redirecting",
                                 "Location: /final\r\n");
        }
        return http_response(200, "arrived");
    });

    asio::io_context client_io;
    int status = 0;
    std::string body;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.max_redirects = 3;
            auto resp = co_await neograph::async::async_post(
                client_io.get_executor(),
                "127.0.0.1", std::to_string(srv.port),
                "/start", "{}", {}, false, opts);
            status = resp.status;
            body   = resp.body;
        },
        asio::detached);
    client_io.run();

    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, "arrived");
    EXPECT_EQ(srv.requests.load(), 2);   // original + one hop
}

TEST(RequestOptions, RedirectDisabledReturns3xx) {
    RoutedMock srv([&](const std::string& path) {
        if (path == "/start") {
            return http_response(302, "bounce",
                                 "Location: /somewhere\r\n");
        }
        return http_response(200, "never");
    });

    asio::io_context client_io;
    int status = 0;
    std::string location;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            // Default RequestOptions — max_redirects = 0.
            auto resp = co_await neograph::async::async_post(
                client_io.get_executor(),
                "127.0.0.1", std::to_string(srv.port),
                "/start", "{}", {}, false, {});
            status   = resp.status;
            location = resp.location;
        },
        asio::detached);
    client_io.run();

    EXPECT_EQ(status, 302);
    EXPECT_EQ(location, "/somewhere");
    EXPECT_EQ(srv.requests.load(), 1);   // no follow
}

TEST(RequestOptions, RedirectHopLimitReturnsFinal3xx) {
    RoutedMock srv([&](const std::string& path) {
        (void)path;
        // Infinite loop — every response redirects back.
        return http_response(302, "loop",
                             "Location: /loop\r\n");
    });

    asio::io_context client_io;
    int status = 0;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.max_redirects = 2;
            auto resp = co_await neograph::async::async_post(
                client_io.get_executor(),
                "127.0.0.1", std::to_string(srv.port),
                "/loop", "{}", {}, false, opts);
            status = resp.status;
        },
        asio::detached);
    client_io.run();

    EXPECT_EQ(status, 302);
    // max_redirects=2 → initial + 2 hops = 3 total requests.
    EXPECT_EQ(srv.requests.load(), 3);
}

TEST(RequestOptions, RetryAfterExtracted) {
    RoutedMock srv([&](const std::string& path) {
        (void)path;
        return http_response(429, "slow down",
                             "Retry-After: 42\r\n");
    });

    asio::io_context client_io;
    int status = 0;
    std::string retry_after;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            auto resp = co_await neograph::async::async_post(
                client_io.get_executor(),
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false, {});
            status      = resp.status;
            retry_after = resp.retry_after;
        },
        asio::detached);
    client_io.run();

    EXPECT_EQ(status, 429);
    EXPECT_EQ(retry_after, "42");
}

TEST(RequestOptions, TimeoutTriggers) {
    // Server that never responds — accepts the socket, reads
    // the request, and then sits. The client should time out.
    struct StallingServer {
        asio::io_context io;
        asio::ip::tcp::acceptor acc{io};
        std::thread worker;
        unsigned short port = 0;
        std::vector<asio::ip::tcp::socket> held;
        std::mutex mu;

        StallingServer() {
            acc.open(asio::ip::tcp::v4());
            acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
            acc.listen();
            port = acc.local_endpoint().port();
            asio::co_spawn(io, loop(), asio::detached);
            worker = std::thread([this]{ io.run(); });
        }
        ~StallingServer() {
            asio::error_code ec;
            acc.close(ec);
            io.stop();
            if (worker.joinable()) worker.join();
        }
        asio::awaitable<void> loop() {
            for (;;) {
                asio::ip::tcp::socket sock{io};
                asio::error_code ec;
                co_await acc.async_accept(
                    sock, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                // Hold the socket so the TCP conn stays alive but
                // we never write a response.
                std::lock_guard lk(mu);
                held.push_back(std::move(sock));
            }
        }
    } srv;

    asio::io_context client_io;
    bool timed_out = false;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.timeout = std::chrono::milliseconds(150);
            try {
                co_await neograph::async::async_post(
                    client_io.get_executor(),
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false, opts);
            } catch (const asio::system_error& e) {
                if (e.code() == asio::error::timed_out) {
                    timed_out = true;
                }
            } catch (...) { }
        },
        asio::detached);

    auto t0 = std::chrono::steady_clock::now();
    client_io.run();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_TRUE(timed_out);
    // Sanity: completed close to the timeout, not way over.
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
    EXPECT_GE(elapsed, std::chrono::milliseconds(100));
}

TEST(RequestOptions, PoolTimeoutTriggers) {
    // Same stalling-server shape, but via ConnPool to verify the
    // pool's timeout wrapper is symmetric with the free function.
    struct StallingServer {
        asio::io_context io;
        asio::ip::tcp::acceptor acc{io};
        std::thread worker;
        unsigned short port = 0;
        std::vector<asio::ip::tcp::socket> held;
        std::mutex mu;

        StallingServer() {
            acc.open(asio::ip::tcp::v4());
            acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
            acc.listen();
            port = acc.local_endpoint().port();
            asio::co_spawn(io, loop(), asio::detached);
            worker = std::thread([this]{ io.run(); });
        }
        ~StallingServer() {
            asio::error_code ec;
            acc.close(ec);
            io.stop();
            if (worker.joinable()) worker.join();
        }
        asio::awaitable<void> loop() {
            for (;;) {
                asio::ip::tcp::socket sock{io};
                asio::error_code ec;
                co_await acc.async_accept(
                    sock, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                std::lock_guard lk(mu);
                held.push_back(std::move(sock));
            }
        }
    } srv;

    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());
    bool timed_out = false;
    asio::co_spawn(client_io,
        [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.timeout = std::chrono::milliseconds(150);
            try {
                co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false, opts);
            } catch (const asio::system_error& e) {
                if (e.code() == asio::error::timed_out) {
                    timed_out = true;
                }
            } catch (...) { }
        },
        asio::detached);

    client_io.run();
    EXPECT_TRUE(timed_out);
    // Timed-out exchange must not pollute the idle pool with a
    // connection in an unknown state.
    EXPECT_EQ(pool.idle_count(), 0u);
}

// ---------------------------------------------------------------------------
// HttpResponse::headers map (Sem 2.6 follow-up). The generic headers
// vector preserves wire order and original-cased names; get_header is
// case-insensitive. Regression: MCPClient relies on Mcp-Session-Id
// tracking which moved from httplib header accessors to this field.

TEST(HttpResponseHeaders, PreservesAllHeadersInWireOrder) {
    RoutedMock srv([](const std::string&) {
        return http_response(200, "ok",
            "X-Alpha: one\r\n"
            "X-Beta: two\r\n"
            "Mcp-Session-Id: sess-42\r\n"
            "X-Upper-MixedCase: three\r\n");
    });

    asio::io_context io;
    neograph::async::HttpResponse resp;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            resp = co_await neograph::async::async_post(
                io.get_executor(), "127.0.0.1",
                std::to_string(srv.port), "/any",
                "body", {}, false);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(resp.status, 200);
    // Every custom header should be present; order matches what the
    // server sent (asserted loosely via find-by-value since
    // Content-Length / Connection land in the list too).
    bool saw_alpha = false, saw_beta = false, saw_sid = false, saw_mixed = false;
    for (const auto& [k, v] : resp.headers) {
        if (k == "X-Alpha" && v == "one") saw_alpha = true;
        if (k == "X-Beta"  && v == "two") saw_beta = true;
        if (k == "Mcp-Session-Id" && v == "sess-42") saw_sid = true;
        // Original-cased name preserved — not lowercased.
        if (k == "X-Upper-MixedCase" && v == "three") saw_mixed = true;
    }
    EXPECT_TRUE(saw_alpha);
    EXPECT_TRUE(saw_beta);
    EXPECT_TRUE(saw_sid);
    EXPECT_TRUE(saw_mixed);

    // Case-insensitive accessor.
    EXPECT_EQ(resp.get_header("Mcp-Session-Id"), "sess-42");
    EXPECT_EQ(resp.get_header("mcp-session-id"), "sess-42");
    EXPECT_EQ(resp.get_header("MCP-SESSION-ID"), "sess-42");
    EXPECT_EQ(resp.get_header("X-Does-Not-Exist"), "");
}

TEST(HttpResponseHeaders, RetryAfterAndLocationAlsoInHeadersMap) {
    // Retry-After and Location were already exposed as dedicated
    // fields before the generic map. They now also appear in the
    // generic vector — callers that already use the dedicated fields
    // keep working, new code can prefer the uniform surface.
    RoutedMock srv([](const std::string&) {
        return http_response(429, "rate limited",
            "Retry-After: 42\r\n"
            "Location: /queue\r\n");
    });

    asio::io_context io;
    neograph::async::HttpResponse resp;
    asio::co_spawn(io,
        [&]() -> asio::awaitable<void> {
            resp = co_await neograph::async::async_post(
                io.get_executor(), "127.0.0.1",
                std::to_string(srv.port), "/any",
                "body", {}, false);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(resp.retry_after, "42");
    EXPECT_EQ(resp.location, "/queue");
    EXPECT_EQ(resp.get_header("Retry-After"), "42");
    EXPECT_EQ(resp.get_header("Location"), "/queue");
}

}  // namespace
