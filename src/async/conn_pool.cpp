// Keep-alive HTTP(S) connection pool. See header for the user-facing
// contract. Implementation notes:
//
//   - Idle buckets keyed by (host, port, tls). No ALPN / SNI-extra
//     variations yet — good enough for the LLM / MCP traffic we
//     target, where each host reliably speaks one scheme per port.
//   - Check-out/check-in are O(1) under a mutex. Actual network I/O
//     always happens outside the lock.
//   - Retry-once semantics: a coroutine that checks out an idle
//     connection, fails mid-exchange, transparently retries on a
//     fresh connection. A fresh-conn failure is a real network
//     error and propagates to the caller.
//   - Stale conns are dropped by letting their unique_ptr go out of
//     scope — the socket destructor closes the fd. We *do not* call
//     async_shutdown on a stale conn: its state after the failed
//     exchange is unknown, and the close_notify ack may never come.
//
// Semester 1.2 scope bounds:
//   - No background idle reaper (lazy eviction on checkout is fine
//     since the pool can't grow past max_idle_per_host anyway).
//   - No TLS session cache tuning on the ssl::context.
//   - No per-host in-flight cap / wait queue. Requests beyond the
//     idle cap just open a temporary conn that doesn't return to
//     the pool.
//   - No HTTP/2. Keep-alive on HTTP/1.1 only.

#include <neograph/async/conn_pool.h>
#include "http_exchange_detail.h"

#include <asio/connect.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace neograph::async {

namespace {

struct Key {
    std::string host;
    std::string port;
    bool        tls;

    bool operator==(const Key& o) const noexcept {
        return tls == o.tls && host == o.host && port == o.port;
    }
};

struct KeyHash {
    // boost::hash_combine pattern: rotate seed and XOR. The previous
    // `h1 ^ (h2 << 1) ^ (tls << 2)` form gave port a 1-bit shift and
    // tls a 2-bit shift, leaving most of port's bits aliased onto
    // host's, which collided for hosts that differed only in port
    // (e.g. localhost:8080 vs localhost:9090).
    static void hash_combine(std::size_t& seed, std::size_t v) noexcept {
        seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    }
    std::size_t operator()(const Key& k) const noexcept {
        std::size_t seed = std::hash<std::string>{}(k.host);
        hash_combine(seed, std::hash<std::string>{}(k.port));
        hash_combine(seed, static_cast<std::size_t>(k.tls));
        return seed;
    }
};

// One pooled connection. Exactly one of plain/tls is engaged based
// on the bucket key. Owns the socket outright (no external refs),
// so a Connection can migrate between the idle deque and a caller's
// stack freely.
struct Connection {
    std::optional<asio::ip::tcp::socket>                    plain;
    std::optional<asio::ssl::stream<asio::ip::tcp::socket>> tls;
    std::chrono::steady_clock::time_point                   idle_since{};
};

}  // namespace

struct ConnPool::Impl {
    asio::any_io_executor ex;
    ConnPoolOptions       opts;
    // ssl_ctx must outlive every pooled TLS Connection, so declare it
    // before `idle`. Unordered_map destroys its elements before its
    // own destructor runs, so this ordering guarantees the ssl::stream
    // destructors see a live context.
    asio::ssl::context    ssl_ctx;

    mutable std::mutex                                                        mu;
    std::unordered_map<Key, std::deque<std::unique_ptr<Connection>>, KeyHash> idle;
    std::size_t                                                               total_idle = 0;

    Impl(asio::any_io_executor e, ConnPoolOptions o)
        : ex(std::move(e)),
          opts(o),
          ssl_ctx(asio::ssl::context::tls_client) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(asio::ssl::verify_peer);
    }

    std::unique_ptr<Connection> checkout(const Key& k) {
        std::lock_guard lk(mu);
        auto it = idle.find(k);
        if (it == idle.end()) return nullptr;
        auto now = std::chrono::steady_clock::now();
        while (!it->second.empty()) {
            auto c = std::move(it->second.back());
            it->second.pop_back();
            --total_idle;
            if (now - c->idle_since <= opts.idle_ttl) return c;
            // expired — drop c by letting it destruct here
        }
        return nullptr;
    }

    void checkin(const Key& k, std::unique_ptr<Connection> c) {
        c->idle_since = std::chrono::steady_clock::now();
        std::lock_guard lk(mu);
        auto& bucket = idle[k];
        if (bucket.size() >= opts.max_idle_per_host) return;  // drop
        bucket.push_back(std::move(c));
        ++total_idle;
    }

    // Core dispatch: try to reuse an idle conn, fall back to fresh.
    // Declared as a member so the Key / Connection types (defined in
    // this TU's anonymous namespace) are accessible — making this a
    // free function would force those types to be reachable from
    // outside the TU or to be duplicated. Definition follows the
    // anonymous-namespace helpers so `open`/`try_exchange`/
    // `exchange_fresh` are in scope at the point of use.
    asio::awaitable<HttpResponse> dispatch(Key key, const std::string& req);
};

namespace {

// Open + (for TLS) handshake a fresh connection for the given key.
asio::awaitable<std::unique_ptr<Connection>> open(
    asio::any_io_executor ex,
    asio::ssl::context&   ssl_ctx,
    const Key&            k) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        k.host, k.port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    auto conn = std::make_unique<Connection>();
    if (!k.tls) {
        conn->plain.emplace(std::move(sock));
        co_return conn;
    }

    conn->tls.emplace(std::move(sock), ssl_ctx);
    auto& s = *conn->tls;

    // SNI: Anthropic/OpenAI require it for TLS 1.3 cert selection.
    if (!SSL_set_tlsext_host_name(s.native_handle(), k.host.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    s.set_verify_callback(asio::ssl::host_name_verification{k.host});
    co_await s.async_handshake(asio::ssl::stream_base::client,
                               asio::use_awaitable);
    co_return conn;
}

// Inspect the HTTP request line to decide whether a stale-idle
// failure is safe to retry on a fresh connection. Per RFC 7231 §4.2.2,
// GET / HEAD / OPTIONS / TRACE are *safe* (read-only, side-effect-
// free); PUT / DELETE are idempotent but not safe; POST / PATCH are
// neither. Retrying a non-safe request on stale-idle would re-send
// the same body, double-applying any side effect the server already
// committed (token charge, email tool action, checkpoint write) when
// the failure landed on the response side rather than the request
// side.
//
// We can't tell from a generic exception whether bytes left the
// kernel before the failure, so the conservative rule is: retry only
// safe methods. Unsafe methods bubble the exception up; callers that
// know their POST is idempotent (LLM completions are stateless on
// the server) can layer their own retry on top.
bool is_safe_method(const std::string& req) {
    // Request line: "METHOD SP URI SP HTTP/1.1\r\n..."
    auto sp = req.find(' ');
    if (sp == std::string::npos) return false;
    std::string_view m(req.data(), sp);
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "TRACE";
}

// Attempt an HTTP exchange over a pooled connection. Returns nullopt
// only when the exchange threw AND the request method is safe to
// replay; otherwise the exception propagates so the caller can
// surface it (rather than silently double-applying a non-idempotent
// side effect on the retry path).
asio::awaitable<std::optional<detail::ExchangeResult>> try_exchange(
    Connection& conn, const std::string& req) {
    try {
        if (conn.plain) {
            auto r = co_await detail::run_exchange(*conn.plain, req);
            co_return r;
        }
        auto r = co_await detail::run_exchange(*conn.tls, req);
        co_return r;
    } catch (const std::exception&) {
        if (is_safe_method(req)) co_return std::nullopt;
        throw;
    }
}

// Exchange on a fresh connection. No internal catch: a failure on
// a brand-new conn is a real network error worth surfacing.
asio::awaitable<detail::ExchangeResult> exchange_fresh(
    Connection& conn, const std::string& req) {
    if (conn.plain) {
        auto r = co_await detail::run_exchange(*conn.plain, req);
        co_return r;
    }
    auto r = co_await detail::run_exchange(*conn.tls, req);
    co_return r;
}

}  // namespace

asio::awaitable<HttpResponse> ConnPool::Impl::dispatch(
    Key key, const std::string& req) {
    auto reused = checkout(key);
    if (reused) {
        auto maybe = co_await try_exchange(*reused, req);
        if (maybe) {
            if (maybe->server_directive == detail::ConnDirective::keep_alive) {
                checkin(key, std::move(reused));
            }
            co_return maybe->response;
        }
        reused.reset();
    }

    auto fresh = co_await open(ex, ssl_ctx, key);
    auto r = co_await exchange_fresh(*fresh, req);
    if (r.server_directive == detail::ConnDirective::keep_alive) {
        checkin(key, std::move(fresh));
    }
    co_return r.response;
}

ConnPool::ConnPool(asio::any_io_executor ex, ConnPoolOptions opts)
    : impl_(std::make_unique<Impl>(std::move(ex), opts)) {}

ConnPool::~ConnPool() = default;

std::size_t ConnPool::idle_count() const {
    std::lock_guard lk(impl_->mu);
    return impl_->total_idle;
}

asio::awaitable<HttpResponse> ConnPool::async_post(
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {

    Key key{ std::string(host), std::string(port), tls };
    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::keep_alive);

    if (opts.timeout.count() <= 0) {
        co_return co_await impl_->dispatch(std::move(key), req);
    }

    // Bound the entire call (reuse attempt + fresh fallback) by a
    // single timer. Same awaitable_operators shape as the free
    // async_post uses — a plain steady_timer racing one awaitable,
    // which GCC 13 codegen handles cleanly.
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(impl_->ex);
    timer.expires_after(opts.timeout);
    auto res = co_await (
        impl_->dispatch(std::move(key), req)
        || timer.async_wait(asio::use_awaitable));
    if (res.index() == 1) {
        throw asio::system_error(asio::error::timed_out,
                                 "ConnPool::async_post: timeout");
    }
    co_return std::get<0>(std::move(res));
}

}  // namespace neograph::async
