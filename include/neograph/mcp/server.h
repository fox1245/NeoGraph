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
#include <optional>
#include <ostream>
#include <string>

namespace neograph::mcp {

/// The newest revision the server negotiates. It also answers the older
/// revisions in MCP_SUPPORTED_PROTOCOL_VERSIONS (types.h) when a client asks
/// for one of them.
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
 *
 * A handler's failure keeps its class on the wire (reference, "Protocol
 * failure mapping"): a HookBoundaryBlocked is JSON-RPC error -32010, a
 * NodeInterrupt -32011, a call stop() cancelled -32000, and every other
 * failure an isError result whose `_meta["io.neograph/failure"]` holds its
 * stable code (graph::classify_failure). A call the client cancelled with
 * `notifications/cancelled` gets no response at all, on every transport (MCP:
 * the receiver SHOULD NOT respond); a transport that holds the request open
 * learns that it ended through the NoResponseSink.
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

    /// Receives the id of a request that ended without a response: the client
    /// cancelled it, so no response is sent. A transport that keeps the
    /// request open (an HTTP POST) closes it here; nothing must reach the
    /// client as a response to that id.
    using NoResponseSink = std::function<void(const json& id)>;

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

    /// The outcome of one message given to handle_message(message, reply):
    /// its response (an object, or an array for a batch), or nullopt when it
    /// has none (a notification, a response, a call the client cancelled, a
    /// batch with nothing to answer).
    using Reply = std::function<void(std::optional<json>)>;

    /// Process one parsed JSON-RPC message. Tool calls may complete via sink.
    /// A batch (an array) is answered with one array, at once when it holds no
    /// tool call and through the response sink otherwise; a batch whose calls
    /// were all cancelled produces nothing.
    json handle_message(const json& envelope);

    /// Process one parsed message and pass its outcome to @p reply exactly
    /// once: before this returns, or later from a worker thread for a tool
    /// call. The response sinks are not used for it. A message may be a
    /// JSON-RPC batch when the negotiated revision has batches (2025-03-26,
    /// and 2024-11-05 peers alike); later revisions, and a batch before
    /// initialize, get one -32600 error object.
    void handle_message(const json& message, Reply reply);

    /// Drive newline-delimited MCP stdio until EOF or a message-boundary stop.
    void run(std::istream& in, std::ostream& out);
    void run();

    /// Set the destination for asynchronously completed tool-call responses.
    /// A call the client cancelled produces no response.
    void set_response_sink(ResponseSink sink);

    /// As above; @p no_response receives the id of each call that ends
    /// without a response. Both sinks are replaced together.
    void set_response_sink(ResponseSink sink, NoResponseSink no_response);

    /// Cooperatively cancel outstanding calls and drain the worker pool.
    void stop();

    bool initialized() const;
    bool is_running() const;

    /// The revision initialize negotiated: the client's when it is one of
    /// MCP_SUPPORTED_PROTOCOL_VERSIONS, MCP_PROTOCOL_VERSION otherwise. Empty
    /// before initialize.
    std::string protocol_version() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace neograph::mcp
