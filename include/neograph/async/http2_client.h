/**
 * @file async/http2_client.h
 * @brief HTTP/2 client backed by nghttp2 + asio.
 *
 * Drop-in for `async::async_post` (h2 over TLS) when the server speaks
 * HTTP/2 (negotiated via ALPN). Today: single-stream-per-connection
 * (no multiplexing) — same wall-clock as HTTP/1.1+pool but the
 * protocol layer is ready for the multiplexing follow-up where N
 * parallel `async_post_h2` calls share one TCP connection.
 *
 * Why not boost::beast / cpprestsdk: standalone-asio everywhere else
 * in this repo, and beast HTTP/2 has been "experimental" forever.
 * nghttp2 is the canonical low-level HTTP/2 lib and ships in every
 * distro. We only use its framing + flow control; the I/O layer
 * stays asio coroutines.
 *
 * Limits (will be lifted as the multiplexing work lands):
 *   - One request per connection. Concurrent calls open separate
 *     TCP+TLS handshakes — same shape as the HTTP/1.1+pool path,
 *     so the wire pattern won't differ much yet. The win arrives
 *     once `Http2Connection::async_post_multiplexed` is added.
 *   - No server push consumption (RFC 7540 §8.2).
 *   - Tiny header table (default nghttp2 settings).
 *   - HTTPS only — h2c (cleartext) not supported.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/async/http_client.h>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

/// HTTPS POST over HTTP/2. Mirrors `async_post` shape (headers, body,
/// HttpResponse) but negotiates `h2` via TLS ALPN. Throws
/// `std::runtime_error` if the server falls back to HTTP/1.1 — that
/// case is already covered by the HTTP/1.1+pool path; the caller
/// chooses which client to use up front.
NEOGRAPH_API asio::awaitable<HttpResponse> async_post_h2(
    asio::any_io_executor ex,
    std::string host,
    std::string port,
    std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers = {},
    RequestOptions opts = {});

} // namespace neograph::async
