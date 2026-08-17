/** @file mcp/hook_rpc.h @brief MCP-backed optional hook JSON-RPC transports. */
#pragma once

#include <neograph/hook_rpc.h>
#include <neograph/mcp/client.h>

namespace neograph::mcp {

/** Reuses MCPClient's shell-free stdio subprocess session. */
class NEOGRAPH_API StdioJsonRpcTransport final : public RpcTransport {
public:
    explicit StdioJsonRpcTransport(std::shared_ptr<MCPClient> client);
    asio::awaitable<std::string> request_async(RpcRequest request) override;
private:
    std::shared_ptr<MCPClient> client_;
};

/**
 * Reuses MCPClient's HTTP session. Headers and credentials remain owned by the
 * MCPClientConfig supplied by the host; the underlying client does not follow
 * redirects, so credentials are never forwarded to a redirect target.
 */
class NEOGRAPH_API HttpJsonRpcTransport final : public RpcTransport {
public:
    explicit HttpJsonRpcTransport(std::shared_ptr<MCPClient> client);
    asio::awaitable<std::string> request_async(RpcRequest request) override;
private:
    std::shared_ptr<MCPClient> client_;
};

} // namespace neograph::mcp
