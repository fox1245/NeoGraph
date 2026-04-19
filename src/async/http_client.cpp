// One-shot async HTTP/1.1 POST client. See header for scope + caveats.
//
// Wire shape per call:
//   resolve → connect → [TLS handshake] → write(request) →
//   read_until("\r\n\r\n") → parse status + Content-Length →
//   read_exactly(Content-Length) → close.
//
// Request build, header parse, and body read live in
// http_exchange_detail.h so the pooled path (conn_pool.cpp) can
// reuse them without duplicating HTTP parsing logic.
//
// The request always sends `Connection: close` — for keep-alive
// reuse, use neograph::async::ConnPool.

#include <neograph/async/http_client.h>
#include "http_exchange_detail.h"

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/use_awaitable.hpp>

#include <exception>
#include <string>

namespace neograph::async {

asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        std::string(host), std::string(port), asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = detail::build_request(
        host, path, body, headers, detail::ConnDirective::close);

    if (!tls) {
        auto r = co_await detail::run_exchange(sock, req);

        // SO_LINGER { on, 0 } → close() sends RST instead of FIN,
        // skipping TIME_WAIT. Only safe when the request/response
        // is fully exchanged before close; for production HTTP a
        // graceful shutdown is correct. Avoids ephemeral-port
        // exhaustion when 1000s of agents each open one connection
        // per LLM call. Keep-alive via ConnPool obsoletes this hack.
        asio::error_code ec;
        sock.set_option(asio::socket_base::linger(true, 0), ec);
        sock.close(ec);
        co_return r.response;
    }

    // ── TLS path ──────────────────────────────────────────────────
    // Fresh context per call is intentional for the one-shot API:
    // keeps the TU standalone and avoids sharing mutable OpenSSL
    // state across coroutines. ConnPool shares one context across
    // all pooled conns for amortization.
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);

    asio::ssl::stream<asio::ip::tcp::socket&> tls_stream{sock, ctx};

    // SNI: without this the server returns a generic cert (or the
    // wrong vhost) on shared hosting, and Anthropic/OpenAI both
    // require SNI for TLS 1.3 cert selection.
    std::string host_str(host);
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host_str.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    tls_stream.set_verify_callback(asio::ssl::host_name_verification{host_str});

    co_await tls_stream.async_handshake(
        asio::ssl::stream_base::client, asio::use_awaitable);

    auto r = co_await detail::run_exchange(tls_stream, req);

    // Graceful TLS shutdown. Servers commonly close first and we
    // receive a short_read / EOF — treat both as clean. We already
    // have the full body; swallow shutdown errors rather than
    // surfacing them as a failed request.
    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Benign: peer closed first, or TLS record-layer EOF.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return r.response;
}

} // namespace neograph::async
