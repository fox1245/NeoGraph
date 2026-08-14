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

using Target = detail::HttpTarget;

Target checked_redirect_target(const Target& current,
                               std::string_view location) {
    const auto next = detail::resolve_redirect_target(current, location);
    // Validate before the next loop iteration reaches resolver.async_resolve.
    // Do not include Location in the exception: query strings and userinfo can
    // contain bearer credentials.
    if (!next || (current.tls && !next->tls) ||
        !detail::same_origin(current, *next)) {
        throw std::runtime_error(
            "async HTTP: redirect rejected by same-origin policy");
    }
    return *next;
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
    bool tls,
    RequestOptions opts) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::close);

    if (!tls) {
        auto r = co_await detail::run_exchange(sock, req, opts);
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

    auto r = co_await detail::run_exchange(tls_stream, req, opts);

    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer commonly closes first after responding — benign.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return r.response;
}

asio::awaitable<detail::StreamExchangeResult> async_post_stream_once(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk,
    RequestOptions opts) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::close);

    if (!tls) {
        auto r = co_await detail::run_exchange_stream(sock, req, on_chunk, opts);
        asio::error_code ec;
        sock.set_option(asio::socket_base::linger(true, 0), ec);
        sock.close(ec);
        co_return r;
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

    auto r = co_await detail::run_exchange_stream(tls_stream, req, on_chunk, opts);

    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer commonly closes first on long streams — benign.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return r;
}

// ── Timeout wrapping ──────────────────────────────────────────────
//
// asio::experimental::awaitable_operators::operator|| co_awaits the
// first of two operands to complete. The other coroutine is
// cancelled. We use it to race the HTTP exchange against a
// steady_timer; expiry path throws asio::error::timed_out.
//
// Zero timeout = pass-through; response limits are still enforced by the
// exchange itself.

asio::awaitable<HttpResponse> async_post_once_timed(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls, RequestOptions opts) {
    if (opts.timeout.count() <= 0) {
        co_return co_await async_post_once(
            ex, std::move(host), std::move(port), std::move(path),
            std::move(body), std::move(headers), tls, opts);
    }
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(ex);
    timer.expires_after(opts.timeout);
    try {
        auto res = co_await (
            async_post_once(ex, std::move(host), std::move(port), std::move(path),
                            std::move(body), std::move(headers), tls, opts)
            || timer.async_wait(asio::use_awaitable));
        if (res.index() == 1) {
            throw asio::system_error(asio::error::timed_out,
                                     "async_post: per-hop timeout");
        }
        co_return std::get<0>(std::move(res));
    } catch (const asio::multiple_exceptions& error) {
        detail::rethrow_first_exception(error);
    }
}

asio::awaitable<detail::StreamExchangeResult> async_post_stream_once_timed(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk,
    RequestOptions opts) {
    if (opts.timeout.count() <= 0) {
        co_return co_await async_post_stream_once(
            ex, std::move(host), std::move(port), std::move(path),
            std::move(body), std::move(headers), tls, std::move(on_chunk), opts);
    }
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(ex);
    timer.expires_after(opts.timeout);
    try {
        auto res = co_await (
            async_post_stream_once(ex, std::move(host), std::move(port),
                                   std::move(path), std::move(body),
                                   std::move(headers), tls, std::move(on_chunk), opts)
            || timer.async_wait(asio::use_awaitable));
        if (res.index() == 1) {
            throw asio::system_error(asio::error::timed_out,
                                     "async_post_stream: per-hop timeout");
        }
        co_return std::get<0>(std::move(res));
    } catch (const asio::multiple_exceptions& error) {
        detail::rethrow_first_exception(error);
    }
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────

static asio::awaitable<HttpResponse> async_post_owned(
    asio::any_io_executor ex,
    std::string host,
    std::string port,
    std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {

    Target cur{
        std::move(host), std::move(port), std::move(path), tls
    };
    std::string req_body(std::move(body));
    int hops = 0;
    for (;;) {
        auto resp = co_await async_post_once_timed(
            ex, cur.host, cur.port, cur.path,
            req_body, headers, cur.tls, opts);

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
        cur = checked_redirect_target(cur, resp.location);
        // Loop: next hop uses the new cur.
    }
}

asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {
    return async_post_owned(
        std::move(ex), std::string(host), std::string(port),
        std::string(path), std::string(body), std::move(headers), tls, opts);
}

// ── Async GET ─────────────────────────────────────────────────────
//
// GET path used by A2A discovery. It shares the POST timeout, response-limit,
// and same-origin redirect policy.

asio::awaitable<HttpResponse> async_get_once(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, /*body=*/"", headers, detail::ConnDirective::close, "GET");

    if (!tls) {
        auto r = co_await detail::run_exchange(sock, req, opts);
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

    auto r = co_await detail::run_exchange(tls_stream, req, opts);
    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer commonly closes first after responding — benign.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return r.response;
}

asio::awaitable<HttpResponse> async_get_once_timed(
    asio::any_io_executor ex,
    std::string host, std::string port, std::string path,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls, RequestOptions opts) {
    if (opts.timeout.count() <= 0) {
        co_return co_await async_get_once(
            ex, std::move(host), std::move(port), std::move(path),
            std::move(headers), tls, opts);
    }
    using asio::experimental::awaitable_operators::operator||;
    asio::steady_timer timer(ex);
    timer.expires_after(opts.timeout);
    try {
        auto res = co_await (
            async_get_once(ex, std::move(host), std::move(port), std::move(path),
                           std::move(headers), tls, opts)
            || timer.async_wait(asio::use_awaitable));
        if (res.index() == 1) {
            throw asio::system_error(asio::error::timed_out,
                                     "async_get: per-hop timeout");
        }
        co_return std::get<0>(std::move(res));
    } catch (const asio::multiple_exceptions& error) {
        detail::rethrow_first_exception(error);
    }
}

static asio::awaitable<HttpResponse> async_get_owned(
    asio::any_io_executor ex,
    std::string host,
    std::string port,
    std::string path,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {

    Target cur{std::move(host), std::move(port), std::move(path), tls};
    int hops = 0;
    for (;;) {
        auto resp = co_await async_get_once_timed(
            ex, cur.host, cur.port, cur.path, headers, cur.tls, opts);
        if (!is_redirect_status(resp.status) || opts.max_redirects <= 0) {
            co_return resp;
        }
        if (++hops > opts.max_redirects || resp.location.empty()) {
            co_return resp;
        }
        cur = checked_redirect_target(cur, resp.location);
    }
}

asio::awaitable<HttpResponse> async_get(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    RequestOptions opts) {
    return async_get_owned(
        std::move(ex), std::string(host), std::string(port),
        std::string(path), std::move(headers), tls, opts);
}

static asio::awaitable<HttpStreamResponse> async_post_stream_owned(
    asio::any_io_executor ex,
    std::string host,
    std::string port,
    std::string path,
    std::string body,
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
        std::move(host), std::move(port), std::move(path), tls
    };
    std::string req_body(std::move(body));
    int hops = 0;
    for (;;) {
        auto resp = co_await async_post_stream_once_timed(
            ex, cur.host, cur.port, cur.path,
            req_body, headers, cur.tls,
            on_chunk, opts);

        if (!is_redirect_status(resp.status) || opts.max_redirects <= 0) {
            co_return HttpStreamResponse{resp.status};
        }
        if (++hops > opts.max_redirects) {
            co_return HttpStreamResponse{resp.status};
        }
        if (resp.location.empty()) {
            co_return HttpStreamResponse{resp.status};
        }
        cur = checked_redirect_target(cur, resp.location);
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
    return async_post_stream_owned(
        std::move(ex), std::string(host), std::string(port),
        std::string(path), std::string(body), std::move(headers), tls,
        std::move(on_chunk), opts);
}

} // namespace neograph::async
