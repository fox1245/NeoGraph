/**
 * @file http_client.h
 * @brief Minimal async HTTP/1.1 POST client on asio coroutines.
 *
 * Stage 3 / Semester 1.1 — adds TLS via asio::ssl::stream. Still
 * HTTP/1.1 only with no chunked transfer, keep-alive, redirects, or
 * auth helpers; the TLS path is what makes real LLM endpoints
 * reachable. Keep-alive, SSE, redirects land in 1.2–1.5.
 *
 * Every call opens a fresh connect → (handshake) → write → read →
 * close cycle. The TLS branch verifies the peer certificate against
 * OpenSSL's default trust store and sets SNI to the requested host.
 */
#pragma once

#include <asio/awaitable.hpp>
#include <asio/any_io_executor.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

struct HttpResponse {
    int         status = 0;
    std::string body;
};

/// Async HTTP(S) POST. Returns the response body and status on the
/// given executor's thread(s). Throws asio::system_error (or
/// std::system_error wrapping SSL errors) on transport failure —
/// caller decides retry policy.
///
/// @param ex      Executor (io_context / strand) where the coroutine runs.
/// @param host    Target host (e.g. "127.0.0.1" or "api.anthropic.com").
///                When `tls` is true this is also used as the SNI value
///                and the name verified against the peer certificate.
/// @param port    Port string (e.g. "8080", "443"). Resolver-compatible.
/// @param path    Path with leading slash (e.g. "/v1/messages").
/// @param body    Raw request body. `Content-Length` is computed from it.
/// @param headers Optional extra headers (name, value) pairs. Callers
///                typically set "Authorization", "Content-Type",
///                "x-api-key" etc.
/// @param tls     When true, wrap the socket in asio::ssl::stream, do
///                TLS handshake with SNI=host, and verify the peer
///                certificate against the system trust store.
asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bool tls = false);

} // namespace neograph::async
