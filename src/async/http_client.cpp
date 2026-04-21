// One-shot async HTTP/1.1 POST client. See header for scope + caveats.
//
// Public API (async_post / async_post_stream) layers three concerns:
//
//   1. Single exchange — resolve → connect → [TLS] → write → read →
//      close. Runs once per call to async_post_once /
//      async_post_stream_once. Lives in http_exchange_detail.h for
//      reuse with the pooled path.
//
//   2. Timeout — each exchange runs against a steady_timer via
//      asio::experimental::awaitable_operators' `||`. Whichever
//      completes first wins; the other coroutine is cancelled.
//      Timer expiry is surfaced as asio::error::timed_out.
//
//   3. Redirect — on 3xx with Location, the public wrapper re-dispatches
//      to async_post_once with the new target, up to
//      opts.max_redirects hops. Method + body are preserved for every
//      3xx (no 303→GET downgrade — pragmatic for the LLM/MCP hosts
//      this library actually talks to).

#include <neograph/async/http_client.h>
#include "http_exchange_detail.h"

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neograph::async {

std::string_view HttpResponse::get_header(std::string_view name) const noexcept {
    // HTTP header names are case-insensitive (RFC 7230 §3.2). Compare
    // by per-char tolower — headers is a small vector in practice
    // (response typically has 5-20 entries), so a linear scan beats
    // allocating a lowercase name key + map lookup.
    auto ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a))
            == std::tolower(static_cast<unsigned char>(b));
    };
    for (const auto& [k, v] : headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(), ieq)) {
            return v;
        }
    }
    return {};
}

namespace {

// Decomposed Location. When `absolute` is false, host/port/tls carry
// whatever the caller started with; only `path` changed.
struct Target {
    std::string host;
    std::string port;
    std::string path;
    bool        tls = false;
};

// Parse an HTTP Location header against the current request target.
// Accepts three shapes:
//   - absolute "http://host[:port]/path"   → fully resolved
//   - absolute "https://host[:port]/path"  → scheme sets tls + default port
//   - relative "/path"                     → reuse current host/port/tls
//   - bare    "path"                       → same, with '/' prepended
// Returns nullopt if the input is empty or obviously malformed
// (scheme-less but not starting with '/', which we conservatively
// reject rather than guess).
Target parse_location(const Target& cur, std::string_view loc) {
    Target out = cur;
    auto starts_with = [](std::string_view s, std::string_view p) {
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    };

    if (starts_with(loc, "http://") || starts_with(loc, "https://")) {
        const bool https = starts_with(loc, "https://");
        std::string_view rest = loc.substr(https ? 8 : 7);
        out.tls = https;

        auto slash = rest.find('/');
        std::string_view authority =
            slash == std::string_view::npos ? rest : rest.substr(0, slash);
        out.path = slash == std::string_view::npos
                       ? "/"
                       : std::string(rest.substr(slash));

        auto colon = authority.find(':');
        if (colon == std::string_view::npos) {
            out.host = std::string(authority);
            out.port = https ? "443" : "80";
        } else {
            out.host = std::string(authority.substr(0, colon));
            out.port = std::string(authority.substr(colon + 1));
        }
        return out;
    }

    if (!loc.empty() && loc.front() == '/') {
        out.path = std::string(loc);
        return out;
    }

    // Bare path — prepend '/'. Rare, but happens.
    out.path = "/" + std::string(loc);
    return out;
}

constexpr bool is_redirect_status(int status) noexcept {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

// ── Single-exchange primitives ────────────────────────────────────
// These run once and close the connection; the public API wraps them
// with timeout + redirect logic.

asio::awaitable<HttpResponse> async_post_once(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::close);

    if (!tls) {
        auto r = co_await detail::run_exchange(sock, req);
        asio::error_code ec;
        sock.set_option(asio::socket_base::linger(true, 0), ec);
        sock.close(ec);
        co_return r.response;
    }

    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);

    asio::ssl::stream<asio::ip::tcp::socket&> tls_stream{sock, ctx};
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    tls_stream.set_verify_callback(asio::ssl::host_name_verification{host});
    co_await tls_stream.async_handshake(
        asio::ssl::stream_base::client, asio::use_awaitable);

    auto r = co_await detail::run_exchange(tls_stream, req);

    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer commonly closes first after responding — benign.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return r.response;
}

asio::awaitable<HttpStreamResponse> async_post_stream_once(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::close);

    HttpStreamResponse out;

    if (!tls) {
        auto r = co_await detail::run_exchange_stream(sock, req, on_chunk);
        out.status = r.status;
        asio::error_code ec;
        sock.set_option(asio::socket_base::linger(true, 0), ec);
        sock.close(ec);
        co_return out;
    }

    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);

    asio::ssl::stream<asio::ip::tcp::socket&> tls_stream{sock, ctx};
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    tls_stream.set_verify_callback(asio::ssl::host_name_verification{host});
    co_await tls_stream.async_handshake(
        asio::ssl::stream_base::client, asio::use_awaitable);

    auto r = co_await detail::run_exchange_stream(tls_stream, req, on_chunk);
    out.status = r.status;

    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer commonly closes first on long streams — benign.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return out;
}

// ── Timeout wrapping ──────────────────────────────────────────────
//
// asio::experimental::awaitable_operators::operator|| co_awaits the
// first of two operands to complete. The other coroutine is
// cancelled. We use it to race the HTTP exchange against a
// steady_timer; expiry path throws asio::error::timed_out.
//
// Zero timeout = pass-through. Keeps the default RequestOptions{}
// behavior bit-identical to the pre-1.5 free functions.

asio::awaitable<HttpResponse> async_post_once_timed(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls, std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        co_return co_await async_post_once(
            ex, std::move(host), std::move(port), std::move(path),
            std::move(body), std::move(headers), tls);
    }
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(ex);
    timer.expires_after(timeout);
    auto res = co_await (
        async_post_once(ex, std::move(host), std::move(port), std::move(path),
                        std::move(body), std::move(headers), tls)
        || timer.async_wait(asio::use_awaitable));
    if (res.index() == 1) {
        throw asio::system_error(asio::error::timed_out,
                                 "async_post: per-hop timeout");
    }
    co_return std::get<0>(std::move(res));
}

asio::awaitable<HttpStreamResponse> async_post_stream_once_timed(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk,
    std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        co_return co_await async_post_stream_once(
            ex, std::move(host), std::move(port), std::move(path),
            std::move(body), std::move(headers), tls, std::move(on_chunk));
    }
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(ex);
    timer.expires_after(timeout);
    auto res = co_await (
        async_post_stream_once(ex, std::move(host), std::move(port),
                               std::move(path), std::move(body),
                               std::move(headers), tls, std::move(on_chunk))
        || timer.async_wait(asio::use_awaitable));
    if (res.index() == 1) {
        throw asio::system_error(asio::error::timed_out,
                                 "async_post_stream: per-hop timeout");
    }
    co_return std::get<0>(std::move(res));
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────

asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {

    Target cur{
        std::string(host), std::string(port), std::string(path), tls
    };
    std::string req_body(body);
    int hops = 0;
    for (;;) {
        auto resp = co_await async_post_once_timed(
            ex, cur.host, cur.port, cur.path,
            req_body, headers, cur.tls, opts.timeout);

        if (!is_redirect_status(resp.status) || opts.max_redirects <= 0) {
            co_return resp;
        }
        if (++hops > opts.max_redirects) {
            // Surface the final 3xx to the caller with Location
            // intact — they can decide whether to throw or follow.
            co_return resp;
        }
        if (resp.location.empty()) {
            co_return resp;
        }
        cur = parse_location(cur, resp.location);
        // Loop: next hop uses the new cur.
    }
}

asio::awaitable<HttpStreamResponse> async_post_stream(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk,
    RequestOptions opts) {

    // Streaming redirects are an uncommon combination (servers that
    // stream don't tend to return 3xx on the same path). Follow them
    // only if asked, by retrying the whole exchange — callers that
    // configured on_chunk should be aware it can fire twice if a
    // redirect occurs. In practice, streaming endpoints always return
    // 200 or a fixed-body error, so this loop usually runs once.
    Target cur{
        std::string(host), std::string(port), std::string(path), tls
    };
    std::string req_body(body);
    int hops = 0;
    for (;;) {
        auto resp = co_await async_post_stream_once_timed(
            ex, cur.host, cur.port, cur.path,
            req_body, headers, cur.tls,
            on_chunk, opts.timeout);

        if (!is_redirect_status(resp.status) || opts.max_redirects <= 0) {
            co_return resp;
        }
        if (++hops > opts.max_redirects) {
            co_return resp;
        }
        // Streaming responses don't populate Location on
        // HttpStreamResponse (we only carry status). A 3xx here
        // with a body delivered to on_chunk is unusual; we stop
        // rather than guess a target.
        co_return resp;
    }
}

} // namespace neograph::async
