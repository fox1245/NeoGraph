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

    std::size_t               max_sessions        = 64;
    std::size_t               max_payload_bytes   = 1024 * 1024;
    std::size_t               http_threads        = 8;
    std::size_t               max_queued_requests = 64;
    std::chrono::milliseconds response_timeout    = std::chrono::minutes(10);

    /// Exact allowed Origin values. An Origin header is rejected unless listed.
    std::vector<std::string> allowed_origins;

    /**
     * Validate a bearer token and return its stable authorization scope.
     *
     * Sessions are bound to this scope, so a different valid principal cannot
     * reuse a leaked MCP-Session-Id. OAuth/JWT validation belongs here or in a
     * trusted reverse proxy, never in GraphEngine or HarnessService.
     */
    BearerAuthorizer bearer_authorizer;
};

/**
 * @brief Session-aware MCP Streamable HTTP server.
 *
 * This baseline returns one `application/json` response per POST and declines
 * optional GET/SSE streams with HTTP 405. `factory` must return a fresh,
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
