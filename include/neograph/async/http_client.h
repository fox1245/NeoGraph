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

#include <neograph/api.h>

#include <asio/awaitable.hpp>
#include <asio/any_io_executor.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

struct NEOGRAPH_API HttpResponse {
    int         status = 0;
    std::string body;

    /// Verbatim `Retry-After` header, if present. Seconds-integer
    /// ("120") or HTTP-date ("Wed, 21 Oct 2015 07:28:00 GMT") — the
    /// caller parses based on shape. Empty when the server didn't
    /// send one.
    std::string retry_after;

    /// Verbatim `Location` header, if present. Only populated when
    /// the response is a 3xx that the client did *not* transparently
    /// follow (because max_redirects was 0 or exhausted). Empty
    /// otherwise.
    std::string location;

    /// All response headers, preserved in wire order. Name comparisons
    /// against this vector should be case-insensitive (HTTP header
    /// names are case-insensitive per RFC 7230 §3.2); `get_header`
    /// below is the convenience accessor that does that correctly.
    ///
    /// Retry-After / Location are also represented here (in addition
    /// to being lifted into the dedicated fields above) — those two
    /// stay for backward-compatibility with callers that used them
    /// before the generic map existed. New code should prefer this
    /// vector for consistency.
    std::vector<std::pair<std::string, std::string>> headers;

    /// Case-insensitive header lookup. Returns the first match's value
    /// or an empty string_view when the header is absent. The returned
    /// view is valid as long as `headers` isn't mutated.
    std::string_view get_header(std::string_view name) const noexcept;
};

/// Per-call knobs. Default-constructed instance preserves the
/// library's historical behavior (no timeout, no redirect-following).
struct RequestOptions {
    /// Per-hop deadline covering connect + handshake + write + read
    /// of one HTTP exchange. Zero = no timeout (the default).
    /// Applied independently to each hop when following redirects
    /// rather than as a total budget.
    std::chrono::milliseconds timeout{0};

    /// Max 3xx hops to follow automatically. Zero (default) = never
    /// follow; the 3xx response comes straight back to the caller
    /// with `location` populated. Redirects preserve the POST method
    /// and body for all 3xx codes (pragmatic for the LLM/MCP hosts
    /// we target — no 303 → GET downgrade).
    int max_redirects = 0;
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
NEOGRAPH_API asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bool tls = false,
    RequestOptions opts = {});

/// Async HTTP(S) GET. Same shape as async_post but issues a GET
/// request with an empty body and no Content-Length / Content-Type
/// headers — used for resources like the A2A `/.well-known/agent-card.json`
/// discovery endpoint.
NEOGRAPH_API asio::awaitable<HttpResponse> async_get(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bool tls = false,
    RequestOptions opts = {});

/// Metadata returned by async_post_stream after the body has been
/// fully delivered via the callback. No `body` member — the body
/// was handed out chunk-by-chunk while the call was in flight.
struct HttpStreamResponse {
    int status = 0;
};

/// Async HTTP(S) POST where the server emits a Transfer-Encoding:
/// chunked response body. Each chunk is delivered to `on_chunk` in
/// wire order; the returned HttpStreamResponse carries only the
/// status line. Used for LLM streaming endpoints (OpenAI chat
/// completions, Anthropic messages in `stream: true` mode).
///
/// Throws asio::system_error on transport failure or
/// std::runtime_error if the response isn't chunked. HTTP-level
/// errors (4xx, 5xx) surface as status + their usually-small body
/// delivered to on_chunk — the caller decides what to do.
///
/// The request uses `Connection: close` — chunked streams are
/// typically long-lived and sharing a conn afterwards offers little
/// amortization. `ConnPool` integration is deferred to a later
/// Semester if/when a streaming workload wants it.
///
/// The bytes passed to `on_chunk` are only valid for the duration
/// of the callback invocation; copy them out if you need to retain.
NEOGRAPH_API asio::awaitable<HttpStreamResponse> async_post_stream(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls,
    std::function<void(std::string_view chunk)> on_chunk,
    RequestOptions opts = {});

} // namespace neograph::async
