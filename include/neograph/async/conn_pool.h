/**
 * @file conn_pool.h
 * @brief Keep-alive HTTP(S) connection pool for async_post.
 *
 * Stage 3 / Semester 1.2 — follows 1.1 (TLS) with the other half of
 * what makes real LLM/MCP endpoints usable at scale: amortizing the
 * TCP connect + TLS handshake over many requests to the same host.
 *
 * Semantics:
 *   - `ConnPool::async_post` mirrors the free `async_post` signature
 *     in `http_client.h`. The body, headers, path, and status/body
 *     return value behave identically.
 *   - Under the hood, the pool keys idle connections by
 *     (host, port, tls). A request with a matching key picks up an
 *     idle connection; otherwise it opens a fresh one.
 *   - If reusing an idle connection fails mid-exchange (server-side
 *     idle timeout race), the pool transparently retries once with a
 *     fresh connection so the caller never sees a spurious error.
 *   - After a successful exchange, the connection returns to the
 *     pool unless the server sent `Connection: close`, in which case
 *     it's dropped.
 *
 * Thread-safety: the pool may be shared across worker threads of one
 * or more io_contexts. Check-out/check-in are serialized by an
 * internal mutex; actual network I/O happens outside the lock.
 */
#pragma once

#include <neograph/async/http_client.h>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

struct ConnPoolOptions {
    /// Max idle connections retained per (host, port, tls) bucket.
    /// Requests beyond this cap still succeed — extras just aren't
    /// cached for reuse.
    std::size_t max_idle_per_host = 16;

    /// Idle connections older than this are dropped on checkout
    /// without being reused. Lazy — no background reaper.
    std::chrono::seconds idle_ttl{30};
};

class NEOGRAPH_API ConnPool {
public:
    explicit ConnPool(asio::any_io_executor ex,
                      ConnPoolOptions opts = {});
    ~ConnPool();

    ConnPool(const ConnPool&) = delete;
    ConnPool& operator=(const ConnPool&) = delete;

    /// Pooled HTTP(S) POST. See free async_post for parameter
    /// semantics. Throws asio::system_error on fresh-connection
    /// failure (DNS, connect, TLS handshake, write/read of the
    /// second attempt). A stale-idle-conn failure is absorbed.
    /// Pooled HTTP(S) POST. See free async_post for parameter
    /// semantics. `opts.timeout` bounds the entire call, covering
    /// both the reuse attempt (if any) and the fresh-connection
    /// retry. `opts.max_redirects` is intentionally *not* honored
    /// here — a redirect might cross host buckets, and silently
    /// reaching into another bucket from a pooled request hides
    /// too much for the library to do behind the caller's back.
    /// Use the free async_post for redirect-following.
    asio::awaitable<HttpResponse> async_post(
        std::string_view host,
        std::string_view port,
        std::string_view path,
        std::string_view body,
        std::vector<std::pair<std::string, std::string>> headers = {},
        bool tls = false,
        RequestOptions opts = {});

    /// Total idle connections across all host buckets. Diagnostic.
    std::size_t idle_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::async
