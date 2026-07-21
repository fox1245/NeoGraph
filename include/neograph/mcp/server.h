/**
 * @file mcp/server.h
 * @brief Reusable MCP 2025-11-25 stdio server substrate.
 *
 * This is the northbound server role. It is intentionally separate from
 * MCPClient, which connects NeoGraph to downstream MCP servers.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>
#include <neograph/mcp/types.h>

#include <cstddef>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <string>

namespace neograph::mcp {

inline constexpr const char* MCP_PROTOCOL_VERSION = "2025-11-25";

struct NEOGRAPH_API MCPServerConfig {
    json        server_info = json::object();
    std::string instructions;
    std::size_t max_concurrent_calls = 4;
    std::size_t max_pending_calls = 64;
};

/**
 * @brief Local, stdio-first MCP server with typed tools and bounded execution.
 *
 * Register tools before initialization, then call run(). Tool calls execute on
 * a fixed-size worker pool. The request-scoped CancelToken passed to a handler
 * is cancelled by `notifications/cancelled`; handlers cooperate by polling or
 * propagating that token into their own async work.
 */
class NEOGRAPH_API MCPServer {
  public:
    using ToolHandler = std::function<CallToolResult(
        const json&, const std::shared_ptr<graph::CancelToken>&)>;

    /// Extension hook for polymorphic tool results such as CreateTaskResult.
    using RawToolHandler =
        std::function<json(const json&, const std::shared_ptr<graph::CancelToken>&, const json&)>;

    /// Synchronous extension-method handler; the server wraps the JSON result.
    using MethodHandler = std::function<json(const json&, const json&)>;

    /// Receives complete JSON-RPC response envelopes produced asynchronously.
    using ResponseSink = std::function<void(const json&)>;

    explicit MCPServer(MCPServerConfig config);
    ~MCPServer();

    MCPServer(const MCPServer&) = delete;
    MCPServer& operator=(const MCPServer&) = delete;

    /// Add a tool. Names must be unique and registration closes at initialize.
    void register_tool(ToolDefinition definition, ToolHandler handler);

    /// Add a tool whose result shape is selected using request metadata.
    void register_raw_tool(ToolDefinition definition, RawToolHandler handler);

    /// Register an extension JSON-RPC method before initialization.
    void register_method(std::string method, MethodHandler handler);

    /// Advertise an explicitly enabled MCP extension capability.
    void register_extension(std::string identifier, json settings = json::object());

    /// Process one parsed JSON-RPC message. Tool calls may complete via sink.
    json handle_message(const json& envelope);

    /// Drive newline-delimited MCP stdio until EOF or a message-boundary stop.
    void run(std::istream& in, std::ostream& out);
    void run();

    /// Set the destination for asynchronously completed tool-call responses.
    void set_response_sink(ResponseSink sink);

    /// Cooperatively cancel outstanding calls and drain the worker pool.
    void stop();

    bool initialized() const;
    bool is_running() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::mcp
