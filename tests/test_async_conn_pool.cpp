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
#include <asio/buffers_iterator.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/streambuf.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <istream>
#include <mutex>
#include <optional>
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
    std::atomic<int>               active{0};
    std::atomic<int>               max_active{0};
    std::atomic<bool>              force_close{false};
    std::atomic<bool>              stale_close{false};
    std::atomic<bool>              release_stale{false};
    std::atomic<bool>              stale_waiting{false};
    std::atomic<bool>              stale_closed{false};
    std::atomic<int>               response_delay_ms{0};
    std::mutex                     request_mu;
    std::mutex                     state_mu;
    std::condition_variable        state_cv;
    std::string                    last_request;
    const std::string              keep_alive_headers;
    unsigned short                 port = 0;

    explicit MockServer(std::string headers = {})
        : keep_alive_headers(std::move(headers)) {
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
        struct ActiveGuard {
            std::atomic<int>& value;
            ~ActiveGuard() { --value; }
        } active_guard{active};
        const int now_active = ++active;
        int previous = max_active.load();
        while (now_active > previous &&
               !max_active.compare_exchange_weak(previous, now_active)) {}
        int connection_requests = 0;
        try {
            for (;;) {
                asio::streambuf buf;
                co_await asio::async_read_until(
                    sock, buf, "\r\n\r\n", asio::use_awaitable);

                {
                    const auto data = buf.data();
                    std::lock_guard lock(request_mu);
                    last_request.assign(asio::buffers_begin(data),
                                        asio::buffers_end(data));
                }

                std::istream is(&buf);
                std::string line;
                std::string request_path;
                long content_length = 0;
                bool client_keepalive = true;
                std::getline(is, line);  // request line
                {
                    auto sp1 = line.find(' ');
                    auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
                    if (sp1 != std::string::npos && sp2 != std::string::npos) {
                        request_path = line.substr(sp1 + 1, sp2 - sp1 - 1);
                    }
                }
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
                ++connection_requests;

                const std::string body = request_path == "/x"
                    ? R"({"ok":true})"
                    : R"({"ok":false})";
                const bool send_close = force_close.load() || !client_keepalive;
                std::string resp;
                resp.reserve(160 + body.size());
                resp.append("HTTP/1.1 200 OK\r\n");
                resp.append("Content-Type: application/json\r\n");
                resp.append("Content-Length: ")
                    .append(std::to_string(body.size()))
                    .append("\r\n");
                resp.append(send_close ? "Connection: close\r\n"
                                       : "Connection: keep-alive\r\n");
                if (!send_close) resp.append(keep_alive_headers);
                resp.append("\r\n");
                resp.append(body);
                if (const int delay = response_delay_ms.load(); delay > 0) {
                    asio::steady_timer timer(io);
                    timer.expires_after(std::chrono::milliseconds(delay));
                    co_await timer.async_wait(asio::use_awaitable);
                }
                co_await asio::async_write(sock, asio::buffer(resp),
                                           asio::use_awaitable);
                if (send_close) break;
                if (stale_close.load() && connection_requests == 1) {
                    stale_waiting = true;
                    state_cv.notify_all();
                    while (!release_stale.load()) {
                        asio::steady_timer timer(io);
                        timer.expires_after(std::chrono::milliseconds(1));
                        co_await timer.async_wait(asio::use_awaitable);
                    }
                    stale_waiting = false;
                    stale_closed = true;
                    state_cv.notify_all();
                    break;
                }
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

TEST(ConnPool, ServerKeepAliveZeroPreventsCaching) {
    MockServer srv("Keep-Alive: max=10, TIMEOUT = 0\r\n"
                   "keep-alive: timeout=5\r\n");
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 2; ++i) {
                const auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port), "/x", "{}", {}, false);
                EXPECT_EQ(response.status, 200);
                EXPECT_EQ(pool.idle_count(), 0u);
            }
        };
    }());

    EXPECT_EQ(srv.requests.load(), 2);
    EXPECT_EQ(srv.accepted.load(), 2);
}

TEST(ConnPool, ServerKeepAliveCapsClientIdleTtl) {
    MockServer srv("Keep-Alive: max=10, timeout=2\r\n");
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            const auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port), "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());
    ASSERT_EQ(pool.idle_count(), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    client_io.restart();
    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            const auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port), "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());

    EXPECT_EQ(srv.requests.load(), 2);
    EXPECT_EQ(srv.accepted.load(), 2);
    EXPECT_EQ(pool.idle_count(), 1u);
}

TEST(ConnPool, MalformedServerKeepAliveFallsBackToClientTtl) {
    MockServer srv("Keep-Alive: timeout=999999999999999999999, max=10\r\n"
                   "Keep-Alive: timeout=invalid\r\n");
    asio::io_context client_io;
    neograph::async::ConnPool pool(client_io.get_executor());

    run_on(client_io, [&] {
        return [&]() -> asio::awaitable<void> {
            for (int i = 0; i < 2; ++i) {
                const auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port), "/x", "{}", {}, false);
                EXPECT_EQ(response.status, 200);
            }
        };
    }());

    EXPECT_EQ(srv.requests.load(), 2);
    EXPECT_EQ(srv.accepted.load(), 1);
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

TEST(AsyncHttpOwnership, ConnPoolSnapshotsRequestBeforeFirstResume) {
    MockServer srv;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());
    std::string host = "127.0.0.1";
    std::string port = std::to_string(srv.port);
    std::string path = "/x";
    std::string body = "{}";

    auto operation = pool.async_post(host, port, path, body);
    path = "/mutated";

    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto resp = future.get();

    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.body, R"({"ok":true})");
}

TEST(ConnPool, NonDefaultPortAppearsInHostHeader) {
    MockServer srv;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());

    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());

    std::string request;
    {
        std::lock_guard lock(srv.request_mu);
        request = srv.last_request;
    }
    EXPECT_NE(request.find("Host: 127.0.0.1:" + std::to_string(srv.port) +
                           "\r\n"), std::string::npos);
}

TEST(ConnPool, UnsafeStaleReuseDoesNotReplayByDefault) {
    MockServer srv;
    srv.stale_close = true;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());

    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());
    ASSERT_EQ(pool.idle_count(), 1u);

    {
        std::unique_lock lock(srv.state_mu);
        ASSERT_TRUE(srv.state_cv.wait_for(
            lock, std::chrono::seconds(2),
            [&] { return srv.stale_waiting.load(); }));
    }
    srv.release_stale = true;
    srv.state_cv.notify_all();
    {
        std::unique_lock lock(srv.state_mu);
        ASSERT_TRUE(srv.state_cv.wait_for(
            lock, std::chrono::seconds(2),
            [&] { return srv.stale_closed.load(); }));
    }

    bool failed = false;
    io.restart();
    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            try {
                neograph::async::RequestOptions opts;
                opts.timeout = std::chrono::milliseconds(500);
                auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false, opts);
                (void)response;
            } catch (const std::exception&) {
                failed = true;
            }
        };
    }());

    EXPECT_TRUE(failed);
    EXPECT_EQ(srv.requests.load(), 1);
}

TEST(ConnPool, UnsafeStaleReuseCanBeExplicitlyReplayed) {
    MockServer srv;
    srv.stale_close = true;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());

    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());
    ASSERT_EQ(pool.idle_count(), 1u);

    {
        std::unique_lock lock(srv.state_mu);
        ASSERT_TRUE(srv.state_cv.wait_for(
            lock, std::chrono::seconds(2),
            [&] { return srv.stale_waiting.load(); }));
    }
    srv.release_stale = true;
    srv.state_cv.notify_all();
    {
        std::unique_lock lock(srv.state_mu);
        ASSERT_TRUE(srv.state_cv.wait_for(
            lock, std::chrono::seconds(2),
            [&] { return srv.stale_closed.load(); }));
    }

    bool succeeded = false;
    io.restart();
    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.timeout = std::chrono::milliseconds(500);
            opts.allow_replay = true;
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false, opts);
            succeeded = response.status == 200;
        };
    }());

    EXPECT_TRUE(succeeded);
    EXPECT_EQ(srv.requests.load(), 2);
    EXPECT_EQ(srv.accepted.load(), 2);
}

TEST(ConnPool, TimeoutCancellationNeverReplaysOptedInPost) {
    MockServer srv;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());

    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false);
            EXPECT_EQ(response.status, 200);
        };
    }());
    ASSERT_EQ(pool.idle_count(), 1u);

    // Force the next response on the reused socket to outlive the caller's
    // deadline. The request has still reached the server, so a buggy replay
    // would be visible as a third request and a second accepted socket.
    srv.response_delay_ms = 200;
    bool timed_out = false;
    io.restart();
    run_on(io, [&] {
        return [&]() -> asio::awaitable<void> {
            neograph::async::RequestOptions opts;
            opts.timeout = std::chrono::milliseconds(30);
            opts.allow_replay = true;
            try {
                auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false, opts);
                (void)response;
            } catch (const asio::system_error& error) {
                timed_out = error.code() == asio::error::timed_out;
            } catch (...) {
            }
        };
    }());

    EXPECT_TRUE(timed_out);
    EXPECT_EQ(srv.requests.load(), 2);
    EXPECT_EQ(srv.accepted.load(), 1);
    EXPECT_EQ(pool.idle_count(), 0u);
}

TEST(ConnPool, MaxInFlightPerHostBoundsConcurrentSockets) {
    MockServer srv;
    srv.response_delay_ms = 40;
    asio::io_context io;
    neograph::async::ConnPool pool(
        io.get_executor(),
        {
            /*.max_idle_per_host =*/ 8,
            /*.idle_ttl =*/ std::chrono::seconds(30),
            /*.max_in_flight_per_host =*/ 2,
        });

    constexpr int requests = 8;
    std::atomic<int> finished{0};
    for (int i = 0; i < requests; ++i) {
        asio::co_spawn(io,
            [&]() -> asio::awaitable<void> {
                auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false);
                EXPECT_EQ(response.status, 200);
            },
            [&finished](std::exception_ptr error) {
                if (error) {
                    try { std::rethrow_exception(error); }
                    catch (const std::exception& ex) {
                        ADD_FAILURE() << "coro: " << ex.what();
                    }
                }
                ++finished;
            });
    }
    std::thread t2([&]{ io.run(); });
    io.run();
    t2.join();

    EXPECT_EQ(finished.load(), requests);
    EXPECT_EQ(srv.requests.load(), requests);
    EXPECT_LE(srv.max_active.load(), 2);
}

TEST(ConnPool, ZeroInFlightCapAllowsParallelExchanges) {
    MockServer srv;
    srv.response_delay_ms = 60;
    asio::io_context io;
    neograph::async::ConnPool pool(
        io.get_executor(),
        {
            /*.max_idle_per_host =*/ 8,
            /*.idle_ttl =*/ std::chrono::seconds(30),
            /*.max_in_flight_per_host =*/ 0,
        });

    constexpr int requests = 8;
    std::atomic<int> finished{0};
    for (int i = 0; i < requests; ++i) {
        asio::co_spawn(io,
            [&]() -> asio::awaitable<void> {
                auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false);
                EXPECT_EQ(response.status, 200);
            },
            [&finished](std::exception_ptr error) {
                if (error) {
                    try { std::rethrow_exception(error); }
                    catch (const std::exception& ex) {
                        ADD_FAILURE() << "coro: " << ex.what();
                    }
                }
                ++finished;
            });
    }
    std::thread t2([&]{ io.run(); });
    io.run();
    t2.join();

    EXPECT_EQ(finished.load(), requests);
    EXPECT_EQ(srv.requests.load(), requests);
    EXPECT_GE(srv.max_active.load(), 2);
}

TEST(ConnPool, DefaultInFlightPathDoesNotImposeHiddenHostCap) {
    MockServer srv;
    srv.response_delay_ms = 100;
    asio::io_context io;
    neograph::async::ConnPool pool(io.get_executor());

    constexpr int requests = 32;
    std::atomic<int> finished{0};
    for (int i = 0; i < requests; ++i) {
        asio::co_spawn(io,
            [&]() -> asio::awaitable<void> {
                auto response = co_await pool.async_post(
                    "127.0.0.1", std::to_string(srv.port),
                    "/x", "{}", {}, false);
                EXPECT_EQ(response.status, 200);
            },
            [&finished](std::exception_ptr error) {
                if (error) {
                    try { std::rethrow_exception(error); }
                    catch (const std::exception& ex) {
                        ADD_FAILURE() << "coro: " << ex.what();
                    }
                }
                ++finished;
            });
    }
    std::thread t2([&]{ io.run(); });
    io.run();
    t2.join();

    EXPECT_EQ(finished.load(), requests);
    EXPECT_EQ(srv.requests.load(), requests);
    EXPECT_GE(srv.max_active.load(), 17);
}

TEST(ConnPool, InFlightWaiterTimeoutCancelsGateWait) {
    MockServer srv;
    srv.response_delay_ms = 200;
    asio::io_context io;
    neograph::async::ConnPool pool(
        io.get_executor(),
        {
            /*.max_idle_per_host =*/ 2,
            /*.idle_ttl =*/ std::chrono::seconds(30),
            /*.max_in_flight_per_host =*/ 1,
        });

    bool first_succeeded = false;
    bool second_timed_out = false;
    bool second_reached_gate = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        try {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false);
            first_succeeded = response.status == 200;
        } catch (...) {
        }
    }, asio::detached);

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        // Wait until the first request has been accepted. At that point it
        // owns the sole host permit, while its delayed response keeps that
        // permit occupied long enough for this request's deadline to fire.
        for (int i = 0; i < 500 && srv.active.load() == 0; ++i) {
            asio::steady_timer timer(io);
            timer.expires_after(std::chrono::milliseconds(1));
            co_await timer.async_wait(asio::use_awaitable);
        }
        second_reached_gate = srv.active.load() > 0;

        neograph::async::RequestOptions opts;
        opts.timeout = std::chrono::milliseconds(30);
        try {
            auto response = co_await pool.async_post(
                "127.0.0.1", std::to_string(srv.port),
                "/x", "{}", {}, false, opts);
            (void)response;
        } catch (const asio::system_error& error) {
            second_timed_out = error.code() == asio::error::timed_out;
        } catch (...) {
        }
    }, asio::detached);

    io.run();

    EXPECT_TRUE(first_succeeded);
    EXPECT_TRUE(second_reached_gate);
    EXPECT_TRUE(second_timed_out);
    EXPECT_EQ(srv.requests.load(), 1);
    EXPECT_EQ(srv.accepted.load(), 1);
}

TEST(ConnPool, InFlightOperationOwnsStateAfterPoolDestruction) {
    MockServer srv;
    srv.response_delay_ms = 500;
    asio::io_context io;
    std::optional<neograph::async::ConnPool> pool;
    pool.emplace(io.get_executor());
    bool timed_out = false;

    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
        neograph::async::RequestOptions opts;
        opts.timeout = std::chrono::milliseconds(50);
        auto operation = pool->async_post(
            "127.0.0.1", std::to_string(srv.port),
            "/x", "{}", {}, false, opts);
        pool.reset();
        try {
            (void)co_await std::move(operation);
        } catch (const asio::system_error& error) {
            timed_out = error.code() == asio::error::timed_out;
        }
    }, asio::detached);
    io.run();

    EXPECT_TRUE(timed_out);
}

}  // namespace
