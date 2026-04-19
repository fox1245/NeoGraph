/**
 * @file http_client.h
 * @brief Minimal async HTTP/1.1 POST client on asio coroutines.
 *
 * PoC-grade — exists to measure whether the async runtime model survives
 * actual HTTP parsing + socket work (as opposed to the pure-timer bench
 * in bench_async_fanout). Not production-quality: HTTP/1.1 only, no
 * TLS, no chunked transfer, no keep-alive, no redirects, no auth.
 *
 * If Stage 2 benchmarks confirm async scales, this file will be
 * replaced by a proper client (TLS via asio::ssl, keep-alive pools,
 * retry, timeouts, etc). Until then, every call does a fresh
 * connect-write-read-close cycle against the target host:port.
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

/// Async HTTP POST. Returns the response body and status on the given
/// executor's thread(s). Throws asio::system_error on transport failure
/// — caller decides retry policy.
///
/// @param ex      Executor (io_context / strand) where the coroutine runs.
/// @param host    Target host (e.g. "127.0.0.1" or "api.anthropic.com").
/// @param port    Port string (e.g. "8080", "443"). Resolver-compatible.
/// @param path    Path with leading slash (e.g. "/v1/messages").
/// @param body    Raw request body. `Content-Length` is computed from it.
/// @param headers Optional extra headers (name, value) pairs. Callers
///                typically set "Authorization", "Content-Type",
///                "x-api-key" etc.
asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers = {});

} // namespace neograph::async
