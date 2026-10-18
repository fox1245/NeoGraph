/**
 * @file mcp/http_server.h
 * @brief MCP 2025-11-25 Streamable HTTP transport for MCPServer.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/server.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::mcp {

struct NEOGRAPH_API MCPHttpServerSession {
    std::unique_ptr<MCPServer> server;

    /// Optional lifetime owner for handlers captured by `server`.
    std::shared_ptr<void> owner;
};

struct NEOGRAPH_API MCPHttpServerConfig {
    using BearerAuthorizer = std::function<std::optional<std::string>(std::string_view token)>;

    std::string host     = "127.0.0.1";
    int         port     = 0;
    std::string endpoint = "/mcp";

    /// Open sessions at most. When a new client initializes at capacity, the
    /// least recently used session with no request in flight is closed for
    /// it; only when every session is busy does initialize get 503.
    std::size_t               max_sessions        = 64;
    std::size_t               max_payload_bytes   = 1024 * 1024;
    /// HTTP worker threads started with the server. A connection holds one
    /// while it is open and a tools/call until its tool returns, so another
    /// starts whenever every thread is taken, up to max_http_threads (they
    /// then stay). Size max_http_threads above the tool calls expected in
    /// flight plus the client connections.
    std::size_t               http_threads        = 8;
    std::size_t               max_http_threads    = 64;
    std::size_t               max_queued_requests = 64;
    /// A request still waiting after this long gets 504, and its tool call is
    /// cancelled.
    std::chrono::milliseconds response_timeout    = std::chrono::minutes(10);
    /// A session with no request for this long is closed, and its id then
    /// answers 404 so the client initializes again. Clients that never send
    /// DELETE (the official TypeScript clients' close()) cannot exhaust the
    /// server. Zero keeps idle sessions until DELETE or eviction.
    std::chrono::milliseconds session_idle_timeout = std::chrono::minutes(30);

    /// Exact allowed Origin values. An Origin header is rejected unless
    /// listed, except that a loopback bind also admits its own loopback
    /// origin: `http://localhost:<port>`, `http://127.0.0.1:<port>` or
    /// `http://[::1]:<port>` at the port it is bound to.
    std::vector<std::string> allowed_origins;

    /// Host header names the server answers: host names, IPv4 addresses and
    /// bracketed IPv6 addresses (`[::1]`), compared with the Host header's
    /// host without its port and ignoring case. `*` admits any Host. A
    /// loopback bind always admits `localhost`, `127.0.0.1` and `[::1]` and
    /// refuses every other Host unless it is listed, with or without an
    /// Origin. On a remote bind an empty list checks nothing, and a non-empty
    /// one admits only the listed names.
    std::vector<std::string> allowed_hosts;

    /**
     * Validate a bearer token and return its stable authorization scope.
     *
     * Sessions are bound to this scope, so a different valid principal cannot
     * reuse a leaked MCP-Session-Id. OAuth/JWT validation belongs here or in a
     * trusted reverse proxy, never in GraphEngine or HarnessService. Without an
     * authorizer (allowed only on loopback binds) the scope is empty.
     */
    BearerAuthorizer bearer_authorizer;

    /**
     * OAuth 2.0 Protected Resource Metadata (RFC 9728) for deployments whose
     * bearer tokens come from an OAuth authorization server, as MCP
     * authorization describes. It needs `resource` (this server's http(s) URL,
     * the endpoint included) and a non-empty `authorization_servers`. The
     * server then publishes it at
     * `/.well-known/oauth-protected-resource<path of resource>` and at
     * `/.well-known/oauth-protected-resource`, and a 401 names it in
     * `WWW-Authenticate: Bearer resource_metadata="..."`, with
     * `scope="..."` from `scopes_supported`. Null (the default) publishes
     * nothing: static bearer tokens need no discovery.
     */
    json protected_resource_metadata;
};

/**
 * @brief Session-aware MCP Streamable HTTP server.
 *
 * This baseline returns one `application/json` response per POST (a call the
 * client cancelled ends with an empty `text/event-stream` instead, so no
 * response reaches it) and declines optional GET/SSE streams with HTTP 405.
 * Sessions end with DELETE, after session_idle_timeout without a request, or
 * by eviction at max_sessions; an ended session's id answers 404. `factory` must return a fresh,
 * registered, uninitialized MCPServer for every new HTTP session. It receives
 * the validated authorization scope so embeddings can isolate durable stores
 * without exposing authentication concepts to GraphEngine.
 */
class NEOGRAPH_API MCPHttpServer {
public:
    using ServerFactory = std::function<MCPHttpServerSession(std::string_view authorization_scope)>;

    MCPHttpServer(ServerFactory factory, MCPHttpServerConfig config = {});
    ~MCPHttpServer();

    MCPHttpServer(const MCPHttpServer&)            = delete;
    MCPHttpServer& operator=(const MCPHttpServer&) = delete;

    /// Bind and serve until stop() is called.
    bool start();

    /// Bind and serve on a background listener thread.
    bool start_async();

    /// Stop HTTP acceptance, cancel session work, and join the listener.
    void stop();

    bool is_running() const;
    int  port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::mcp
