/**
 * @file async/curl_h2_pool.h
 * @brief HTTP/2 client backed by libcurl + curl_multi (multiplexing).
 *
 * Replaces the per-call connect+TLS pattern of `async_post` and the
 * single-stream `async_post_h2` PoC with libcurl's mature HTTP/2
 * implementation: connection pool, stream multiplexing, redirect
 * handling, gzip/brotli decoding, and ALPN-driven version pick all
 * come for free. Cloudflare and other anti-bot WAFs trust the curl
 * fingerprint, so no signature-massaging is needed.
 *
 * Threading: each `CurlH2Pool` owns one worker thread that drives
 * `curl_multi_perform` + `curl_multi_poll`. Caller-side `async_post`
 * submits a request to the worker via a mutex-protected queue, then
 * suspends on an asio handler that the worker posts back to via
 * `asio::post(caller_executor, ...)` once the request completes.
 *
 * Provider-style ownership: SchemaProvider / OpenAIProvider hold
 * one `CurlH2Pool` for their lifetime — successive completions reuse
 * the same connection cache. The pool itself is thread-safe; multiple
 * caller coroutines on different executors can call `async_post`
 * concurrently and the worker will multiplex them onto the same
 * underlying TCP when the host matches.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/async/http_client.h>  // HttpResponse + RequestOptions

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neograph::async {

class NEOGRAPH_API CurlH2Pool {
public:
    CurlH2Pool();
    ~CurlH2Pool();

    CurlH2Pool(const CurlH2Pool&)            = delete;
    CurlH2Pool& operator=(const CurlH2Pool&) = delete;

    /// HTTPS POST with HTTP/2 ALPN preference (falls back to 1.1 if the
    /// server doesn't speak h2). Multiple concurrent calls to the same
    /// host multiplex over a single TCP connection.
    ///
    /// `url` must be a full HTTPS URL (`https://host[:port]/path`).
    asio::awaitable<HttpResponse> async_post(
        std::string url,
        std::string body,
        std::vector<std::pair<std::string, std::string>> headers = {},
        RequestOptions opts = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::async
