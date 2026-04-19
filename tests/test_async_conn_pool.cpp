// Unit tests for neograph::async::ConnPool (Stage 3 Semester 1.2).
//
// Plain TCP only — TLS pool reuse is exercised via the out-of-band
// neograph_async_https_smoke binary. The default ctest suite must
// stay offline-safe, so we spin an in-process asio HTTP/1.1 mock
// server that honors the Connection: keep-alive vs. close semantics
// we care about:
//
//   1. BasicRoundTrip   — one pooled request lands and is retained.
//   2. SerialReuse      — N serial pooled requests reuse one socket.
//   3. ServerCloseEndsReuse — response `Connection: close` means
//      the pool doesn't reuse that conn, forcing N opens for N
//      requests (and ending with idle_count == 0).
//   4. ParallelConverges — M coroutines each doing R serial
//      requests converge to at most M open sockets + ≤ max_idle
//      retained, with all M*R requests served.

#include <neograph/async/conn_pool.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <exception>
#include <istream>
#include <string>
#include <thread>
#include <vector>

namespace {

// In-process asio HTTP/1.1 mock. Accepts connections in a background
// thread; per connection it loops handling keep-alive requests until
// either side closes. `force_close` lets tests flip the response's
// Connection header to exercise the drop-on-close path.
struct MockServer {
    asio::io_context               io;
    asio::ip::tcp::acceptor        acceptor{io};
    std::thread                    worker;
    std::atomic<int>               accepted{0};
    std::atomic<int>               requests{0};
    std::atomic<bool>              force_close{false};
    unsigned short                 port = 0;

    MockServer() {
        acceptor.open(asio::ip::tcp::v4());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        asio::co_spawn(io, accept_loop(), asio::detached);
        worker = std::thread([this]{ io.run(); });
    }

    ~MockServer() {
        asio::error_code ec;
        acceptor.close(ec);
        io.stop();
        if (worker.joinable()) worker.join();
    }

    asio::awaitable<void> handle(asio::ip::tcp::socket sock) {
        try {
            for (;;) {
                asio::streambuf buf;
                co_await asio::async_read_until(
                    sock, buf, "\r\n\r\n", asio::use_awaitable);

                std::istream is(&buf);
                std::string line;
                long content_length = 0;
                bool client_keepalive = true;
                std::getline(is, line);  // request line
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
                    if (name == "content-length") {
                        content_length = std::stol(value);
                    } else if (name == "connection") {
                        std::string lv;
                        for (auto c : value)
                            lv.push_back(static_cast<char>(
                                std::tolower(static_cast<unsigned char>(c))));
                        if (lv.find("close") != std::string::npos)
                            client_keepalive = false;
                    }
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

                const std::string body = R"({"ok":true})";
                const bool send_close = force_close.load() || !client_keepalive;
                std::string resp;
                resp.reserve(160 + body.size());
                resp.append("HTTP/1.1 200 OK\r\n");
                resp.append("Content-Type: application/json\r\n");
                resp.append("Content-Length: ")
                    .append(std::to_string(body.size()))
                    .append("\r\n");
                resp.append(send_close ? "Connection: close\r\n\r\n"
                                       : "Connection: keep-alive\r\n\r\n");
                resp.append(body);
                co_await asio::async_write(sock, asio::buffer(resp),
                                           asio::use_awaitable);
                if (send_close) break;
            }
        } catch (...) {
            // client disconnected / malformed — drop
        }
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
            ++accepted;
            asio::co_spawn(io, handle(std::move(sock)), asio::detached);
        }
    }
};

// Drain the client io_context by spawning a coroutine + running.
// Returns when the coroutine completes (and any fan-out spawned
// from it).
template <typename Coro>
void run_on(asio::io_context& io, Coro&& make_coro) {
    asio::co_spawn(io, std::forward<Coro>(make_coro)(), asio::detached);
    io.run();
}

TEST(ConnPool, BasicRoundTrip) {
    MockServer srv;
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    int status = 0;
    std::size_t body_len = 0;
    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            auto resp = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", R"({"hi":"there"})", {}, false);
            status = resp.status;
            body_len = resp.body.size();
        };
    }());

    EXPECT_EQ(status, 200);
    EXPECT_GT(body_len, 0u);
    EXPECT_EQ(pool.idle_count(), 1u);
    EXPECT_EQ(srv.accepted.load(), 1);
    EXPECT_EQ(srv.requests.load(), 1);
}

TEST(ConnPool, SerialReuse) {
    MockServer srv;
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    constexpr int N = 5;
    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            for (int i = 0; i < N; ++i) {
                auto resp = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false);
                EXPECT_EQ(resp.status, 200);
            }
        };
    }());

    EXPECT_EQ(srv.requests.load(), N);
    EXPECT_EQ(srv.accepted.load(), 1);    // one socket served all N
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(ConnPool, ServerCloseEndsReuse) {
    MockServer srv;
    srv.force_close = true;
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    constexpr int N = 3;
    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            for (int i = 0; i < N; ++i) {
                auto resp = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false);
                EXPECT_EQ(resp.status, 200);
            }
        };
    }());

    EXPECT_EQ(srv.requests.load(), N);
    EXPECT_EQ(srv.accepted.load(), N);    // server closed after each
    EXPECT_EQ(pool.idle_count(), 0u);
}

TEST(ConnPool, ParallelConverges) {
    MockServer srv;
    asio::io_context client_io;
    neograph::async::ConnPool pool(
        client_io.get_executor(),
        {
            /*.max_idle_per_host =*/ 4,
            /*.idle_ttl =*/           std::chrono::seconds(30),
        });

    constexpr int M = 4;   // concurrent coroutines
    constexpr int R = 5;   // serial rounds per coroutine
    std::atomic<int> finished{0};

    for (int i = 0; i < M; ++i) {
        asio::co_spawn(client_io,
            [&]() -> asio::awaitable<void> {
                for (int r = 0; r < R; ++r) {
                    auto resp = co_await pool.async_post(
                        "127.0.0.1", std::to_string(srv.port),
                        "/x", "{}", {}, false);
                    EXPECT_EQ(resp.status, 200);
                }
            },
            [&finished](std::exception_ptr e) {
                if (e) { try { std::rethrow_exception(e); }
                         catch (const std::exception& ex) {
                             ADD_FAILURE() << "coro: " << ex.what(); } }
                ++finished;
            });
    }

    // Run two worker threads so multiple coroutines can be in-flight
    // simultaneously. io.run() returns when there's no more work.
    std::thread t2([&]{ client_io.run(); });
    client_io.run();
    t2.join();

    EXPECT_EQ(finished.load(), M);
    EXPECT_EQ(srv.requests.load(), M * R);
    EXPECT_LE(srv.accepted.load(), M);    // ≤ one socket per coro
    EXPECT_LE(pool.idle_count(), 4u);
    EXPECT_GT(pool.idle_count(), 0u);     // at least some reuse retained
}

}  // namespace
