// bench_async_http — does the async advantage from bench_async_fanout
// survive real HTTP/1.1 wire work (parse status, headers, body) against
// actual TCP sockets? The timer-only PoC measured the scheduling
// primitive in isolation; this one adds the layers that matter for
// real LLM calls: connect/write/read/parse-per-call.
//
// Shape:
//   - An in-process asio mock server listens on 127.0.0.1:<port>. Each
//     request: parse headers, read body, sleep for `--latency-ms`
//     (simulates LLM wall-clock), reply with fixed 200 OK + small JSON.
//     Server runs on its own io_context + `--server-threads` workers.
//   - Client mode picks how the load is driven:
//       --mode sync  : N OS threads, each does M httplib POSTs.
//                      Mirrors today's schema_provider wire shape.
//       --mode async : `--client-threads` io_context workers + N
//                      coroutines each doing M async_post() calls.
//
// Output identical to bench_async_fanout (wall, ops/s, peak RSS) so
// the two can be read side-by-side.
//
// Not measuring (out of PoC scope):
//   - TLS. Real Anthropic / OpenAI calls are HTTPS; asio::ssl works
//     but the handshake cost compounds per call. That's stage 3.
//   - JSON parse cost on the client side. Body is small fixed string.
//   - Keep-alive connection pooling. Every call is a fresh TCP handshake.

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <httplib.h>
#include <sys/socket.h>

#include <neograph/async/conn_pool.h>
#include <neograph/async/http_client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    std::string mode           = "async";
    int         concur         = 1000;
    int         rounds         = 3;
    int         latency_ms     = 50;
    int         client_threads = 0;     // async only; 0 = hardware_concurrency
    int         server_threads = 4;
    int         port           = 18080;
};

Config parse(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--mode")            c.mode           = next();
        else if (a == "--concur")          c.concur         = std::stoi(next());
        else if (a == "--rounds")          c.rounds         = std::stoi(next());
        else if (a == "--latency-ms")      c.latency_ms     = std::stoi(next());
        else if (a == "--client-threads")  c.client_threads = std::stoi(next());
        else if (a == "--server-threads")  c.server_threads = std::stoi(next());
        else if (a == "--port")            c.port           = std::stoi(next());
    }
    if (c.client_threads == 0)
        c.client_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (c.client_threads == 0) c.client_threads = 4;
    return c;
}

double peak_rss_mb() {
    std::ifstream f("/proc/self/status");
    if (!f) return -1.0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            auto pos = line.find_first_of("0123456789");
            if (pos == std::string::npos) return -1.0;
            return std::stol(line.substr(pos)) / 1024.0;
        }
    }
    return -1.0;
}

// ── Mock HTTP server ──────────────────────────────────────────────────

constexpr const char* kRespBody =
    R"({"content":[{"type":"text","text":"ok"}]})";

// Per-connection handler. Honors the client's Connection header —
// when keep-alive, loops to serve subsequent requests on the same
// socket; when close (or the request was malformed), exits after one
// response. SO_LINGER-0 is used only on the close path so
// keep-alive clients don't see an RST right after a clean response.
asio::awaitable<void> handle_client(asio::ip::tcp::socket sock, int latency_ms) {
    try {
        asio::streambuf buf;
        for (;;) {
            co_await asio::async_read_until(sock, buf, "\r\n\r\n",
                                            asio::use_awaitable);

            std::istream is(&buf);
            std::string line;
            long content_length = 0;
            bool client_keepalive = true;   // HTTP/1.1 default
            std::getline(is, line);  // request line
            while (std::getline(is, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                auto first = value.find_first_not_of(" \t");
                if (first != std::string::npos) value = value.substr(first);
                for (auto& ch : name) ch = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(ch)));
                if (name == "content-length") {
                    content_length = std::stol(value);
                } else if (name == "connection") {
                    std::string lv;
                    for (auto ch : value) lv.push_back(static_cast<char>(
                        std::tolower(static_cast<unsigned char>(ch))));
                    if (lv.find("close") != std::string::npos)
                        client_keepalive = false;
                }
            }
            auto already = buf.size();
            if (static_cast<long>(already) < content_length) {
                auto rem = content_length - static_cast<long>(already);
                std::vector<char> tail(rem);
                co_await asio::async_read(sock, asio::buffer(tail),
                                          asio::transfer_exactly(rem),
                                          asio::use_awaitable);
            } else if (static_cast<long>(already) > content_length) {
                buf.consume(content_length);
            }

            // simulate LLM latency
            asio::steady_timer t{co_await asio::this_coro::executor};
            t.expires_after(std::chrono::milliseconds(latency_ms));
            co_await t.async_wait(asio::use_awaitable);

            std::string body = kRespBody;
            std::string resp;
            resp.reserve(160 + body.size());
            resp.append("HTTP/1.1 200 OK\r\n");
            resp.append("Content-Type: application/json\r\n");
            resp.append("Content-Length: ")
                .append(std::to_string(body.size())).append("\r\n");
            resp.append(client_keepalive ? "Connection: keep-alive\r\n\r\n"
                                         : "Connection: close\r\n\r\n");
            resp.append(body);
            co_await asio::async_write(sock, asio::buffer(resp),
                                       asio::use_awaitable);

            if (!client_keepalive) {
                // Non-keep-alive: match the old SO_LINGER-0 behavior so
                // the --mode sync / --mode async numbers stay comparable
                // to pre-1.6 runs.
                asio::error_code ec;
                sock.set_option(asio::socket_base::linger(true, 0), ec);
                sock.close(ec);
                co_return;
            }
            // Keep-alive: loop for the next request on the same socket.
        }
    } catch (...) {
        // client disconnected / malformed request — drop
    }
}

asio::awaitable<void> acceptor_loop(asio::ip::tcp::acceptor acc, int latency_ms) {
    for (;;) {
        asio::ip::tcp::socket s{co_await asio::this_coro::executor};
        co_await acc.async_accept(s, asio::use_awaitable);
        asio::co_spawn(acc.get_executor(),
            handle_client(std::move(s), latency_ms),
            asio::detached);
    }
}

struct Server {
    asio::io_context io;
    std::vector<std::thread> workers;
    asio::ip::tcp::endpoint endpoint;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;

    Server(int port, int threads, int latency_ms) {
        work = std::make_unique<
            asio::executor_work_guard<asio::io_context::executor_type>>(
                asio::make_work_guard(io));
        endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port);
        asio::ip::tcp::acceptor acc{io};
        acc.open(endpoint.protocol());
        acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acc.bind(endpoint);
        acc.listen();
        asio::co_spawn(io, acceptor_loop(std::move(acc), latency_ms),
                       asio::detached);
        for (int i = 0; i < threads; ++i)
            workers.emplace_back([this] { io.run(); });
    }

    ~Server() {
        work.reset();
        io.stop();
        for (auto& t : workers) if (t.joinable()) t.join();
    }
};

// ── Sync client (httplib) ─────────────────────────────────────────────

void run_sync(const Config& cfg) {
    const std::string host = "127.0.0.1";
    std::vector<std::thread> ts;
    ts.reserve(cfg.concur);
    for (int i = 0; i < cfg.concur; ++i) {
        ts.emplace_back([&] {
            httplib::Client cli(host, cfg.port);
            cli.set_read_timeout(30, 0);
            cli.set_connection_timeout(10, 0);
            // SO_LINGER-0 so close() issues RST, no TIME_WAIT. Keeps
            // the bench from exhausting ephemeral ports at high N.
            cli.set_socket_options([](auto sock) {
                struct linger l { 1, 0 };
                ::setsockopt(sock, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
            });
            for (int r = 0; r < cfg.rounds; ++r) {
                auto res = cli.Post("/v1/messages",
                    R"({"msg":"ping"})", "application/json");
                if (!res || res->status != 200) {
                    std::fprintf(stderr, "sync call failed\n");
                    return;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
}

// ── Async client (asio coroutines) ────────────────────────────────────

asio::awaitable<void> async_agent(asio::any_io_executor ex,
                                   int rounds, int port) {
    const std::string body = R"({"msg":"ping"})";
    for (int r = 0; r < rounds; ++r) {
        try {
            auto resp = co_await neograph::async::async_post(
                ex, "127.0.0.1", std::to_string(port),
                "/v1/messages", body, {});
            if (resp.status != 200) {
                std::fprintf(stderr, "async call status=%d\n", resp.status);
                co_return;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "async call err: %s\n", e.what());
            co_return;
        }
    }
}

void run_async(const Config& cfg) {
    asio::io_context io;
    auto work = asio::make_work_guard(io);

    std::atomic<int> remaining{cfg.concur};
    for (int i = 0; i < cfg.concur; ++i) {
        asio::co_spawn(io,
            [ex = io.get_executor(), rounds = cfg.rounds, port = cfg.port]()
                    -> asio::awaitable<void> {
                co_await async_agent(ex, rounds, port);
            },
            [&remaining](std::exception_ptr e) {
                if (e) { try { std::rethrow_exception(e); }
                         catch (const std::exception& ex) {
                             std::fprintf(stderr, "coro err: %s\n", ex.what()); } }
                --remaining;
            });
    }

    std::vector<std::thread> ts;
    ts.reserve(cfg.client_threads);
    for (int i = 0; i < cfg.client_threads; ++i)
        ts.emplace_back([&io] { io.run(); });

    while (remaining.load(std::memory_order_relaxed) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    work.reset();
    io.stop();
    for (auto& t : ts) t.join();
}

// ── Async client with keep-alive ConnPool ─────────────────────────
// Same coroutine shape as async_agent, but routed through one
// shared ConnPool so the TCP handshake is amortized across rounds
// (and, at high concurrency, across coroutines that happen to reuse
// recently-returned idle conns). Mock server honors keep-alive so
// this actually exercises reuse.

asio::awaitable<void> pool_agent(neograph::async::ConnPool& pool,
                                  int rounds, int port) {
    const std::string body = R"({"msg":"ping"})";
    for (int r = 0; r < rounds; ++r) {
        try {
            auto resp = co_await pool.async_post(
                "127.0.0.1", std::to_string(port),
                "/v1/messages", body, {}, false);
            if (resp.status != 200) {
                std::fprintf(stderr, "pool call status=%d\n", resp.status);
                co_return;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "pool call err: %s\n", e.what());
            co_return;
        }
    }
}

void run_async_pool(const Config& cfg) {
    asio::io_context io;
    auto work = asio::make_work_guard(io);
    neograph::async::ConnPool pool(io.get_executor());

    std::atomic<int> remaining{cfg.concur};
    for (int i = 0; i < cfg.concur; ++i) {
        asio::co_spawn(io,
            [&pool, rounds = cfg.rounds, port = cfg.port]()
                    -> asio::awaitable<void> {
                co_await pool_agent(pool, rounds, port);
            },
            [&remaining](std::exception_ptr e) {
                if (e) { try { std::rethrow_exception(e); }
                         catch (const std::exception& ex) {
                             std::fprintf(stderr, "coro err: %s\n", ex.what()); } }
                --remaining;
            });
    }

    std::vector<std::thread> ts;
    ts.reserve(cfg.client_threads);
    for (int i = 0; i < cfg.client_threads; ++i)
        ts.emplace_back([&io] { io.run(); });

    while (remaining.load(std::memory_order_relaxed) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    work.reset();
    io.stop();
    for (auto& t : ts) t.join();
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse(argc, argv);

    // Start the server. Its destructor (end of scope) stops the io.
    Server server(cfg.port, cfg.server_threads, cfg.latency_ms);
    // Tiny sleep so the acceptor is listening before clients connect.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto t0 = std::chrono::steady_clock::now();
    if      (cfg.mode == "sync")       run_sync(cfg);
    else if (cfg.mode == "async")      run_async(cfg);
    else if (cfg.mode == "async_pool") run_async_pool(cfg);
    else { std::cerr << "unknown mode: " << cfg.mode << "\n"; return 2; }
    auto wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    const bool driver_is_async =
        cfg.mode == "async" || cfg.mode == "async_pool";
    double total_ops = static_cast<double>(cfg.concur) * cfg.rounds;
    std::printf("mode=%-10s concur=%6d rounds=%d lat_ms=%d  "
                "wall=%6.3fs  ops/s=%9.1f  rss_mb=%7.1f  "
                "client_threads=%d server_threads=%d\n",
                cfg.mode.c_str(), cfg.concur, cfg.rounds, cfg.latency_ms,
                wall, total_ops / wall, peak_rss_mb(),
                driver_is_async ? cfg.client_threads : cfg.concur,
                cfg.server_threads);
    return 0;
}
