/**
 * @file mcp/client.h
 * @brief MCP (Model Context Protocol) client for connecting to remote tool servers.
 *
 * Provides MCPClient for discovering and calling tools on MCP servers,
 * and MCPTool for wrapping remote tools as local Tool objects.
 *
 * Two transports are supported:
 *   - **HTTP** (Streamable HTTP) — remote server, reachable over the network.
 *   - **stdio** — subprocess launched by the client; newline-delimited
 *     JSON-RPC messages exchanged over the child's stdin/stdout. The
 *     subprocess lives as long as any MCPTool produced by the client.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/types.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::graph { class CancelToken; }

namespace neograph::mcp {

namespace detail {
class ProtocolSession;
}

/**
 * @brief Wraps a remote MCP server tool as a local Tool.
 *
 * MCPTool implements the Tool interface through the originating protocol
 * session, independently of which transport owns the connection.
 */
class NEOGRAPH_API MCPTool : public AsyncTool {
  public:
    /// Direct HTTP constructor. Each execution opens an ephemeral
    /// protocol session; tools returned by MCPClient::get_tools() instead
    /// retain their originating protocol session and transport.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    /// Full MCP definition, including outputSchema and annotations.
    const ToolDefinition& get_mcp_definition() const noexcept { return definition_; }

    /// Typed execution preserving all content block kinds and MCP result fields.
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);

    ChatTool get_definition() const override;

    /**
     * @brief Execute the tool on the MCP server.
     * @param arguments JSON object containing the tool's input parameters.
     * @return Result string (text content joined by newlines) or JSON dump.
     */
    /// Native async execution — overlaps with sibling tool calls when a
    /// node dispatches several at once. stdio correlates in-flight calls by
    /// JSON-RPC id; discovered HTTP tools reuse their originating session.
    asio::awaitable<std::string> execute_async(const json& arguments) override;

    std::string get_name() const override { return definition_.name; }

  private:
    friend class MCPClient;
    MCPTool(std::shared_ptr<detail::ProtocolSession> session,
            ToolDefinition definition);

    std::string server_url_; ///< Legacy direct-HTTP constructor only.
    std::shared_ptr<detail::ProtocolSession> session_;
    ToolDefinition definition_;
};

/**
 * @brief Client for connecting to MCP (Model Context Protocol) servers.
 *
 * Handles the initialization handshake, tool discovery, and tool
 * invocation. Both HTTP and stdio transports are available; pick via
 * the appropriate constructor.
 *
 * @code
 * // HTTP
 * MCPClient http_client("http://localhost:8000");
 * http_client.initialize("my-agent");
 * auto tools = http_client.get_tools();
 *
 * // stdio — spawn a subprocess
 * MCPClient stdio_client({"python", "server.py"});
 * stdio_client.initialize("my-agent");
 * auto tools2 = stdio_client.get_tools();
 * @endcode
 */
class NEOGRAPH_API MCPClient {
  public:
    /**
     * @brief Construct an HTTP-mode MCP client.
     * @param server_url URL of the MCP server (e.g., "http://localhost:8000").
     */
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    /**
     * @brief Construct a stdio-mode MCP client by spawning a subprocess.
     * @param argv Command + arguments (e.g., {"python", "server.py"}). argv[0]
     *             is resolved via PATH before fork. Pipe/fork failures throw
     *             during construction; exec failure surfaces on the first RPC.
     *
     * The subprocess is terminated (SIGTERM + waitpid) when the last
     * reference to the underlying session is dropped — this is either the
     * MCPClient itself or any MCPTool produced by get_tools().
     */
    explicit MCPClient(std::vector<std::string> argv);

    MCPClient(const MCPClient&) = delete;
    MCPClient& operator=(const MCPClient&) = delete;
    MCPClient(MCPClient&&) = delete;
    MCPClient& operator=(MCPClient&&) = delete;

    /**
     * @brief Initialize the connection and perform the MCP handshake.
     * @param client_name Client identifier sent during handshake (default: "neograph").
     * @return True after initialization. Protocol and transport failures throw.
     */
    bool initialize(const std::string& client_name = "neograph");

    /// Async handshake + initialized notification for either transport.
    asio::awaitable<bool> initialize_async(const std::string& client_name = "neograph");

    bool is_initialized() const noexcept;
    InitializeResult get_initialize_result() const;

    /**
     * @brief Discover tools from the MCP server.
     * @return Vector of Tool unique_ptrs (MCPTool instances).
     */
    std::vector<std::unique_ptr<Tool>> get_tools();

    /// Fetch one tools/list page. The cursor is opaque and is echoed verbatim.
    ListToolsPage list_tools(
        const std::optional<std::string>& cursor = std::nullopt);
    asio::awaitable<ListToolsPage> list_tools_async(
        const std::optional<std::string>& cursor = std::nullopt);

    /// Fetch all pages while preserving the complete MCP definitions.
    std::vector<ToolDefinition> get_tool_definitions();

    /**
     * @brief Call a tool directly by name.
     * @param name Tool name on the server.
     * @param arguments JSON object of tool arguments.
     * @return Raw MCP tools/call result object from the server.
     */
    json call_tool(const std::string& name, const json& arguments);

    CallToolResult call_tool_result(const std::string& name,
                                    const json& arguments);

    /// Async variant of call_tool() — awaits the tools/call RPC without
    /// blocking. Used by MCPTool::execute_async for concurrent dispatch.
    asio::awaitable<json> call_tool_async(const std::string& name,
                                          const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name,
        const json& arguments);

    /**
     * @brief Send a transport-independent JSON-RPC request asynchronously.
     *
     * Both transports are coroutine-native. stdio owns its process and
     * correlation reader; HTTP owns its headers and HTTP request lifecycle.
     * @param method JSON-RPC method name.
     * @param params Method parameters.
     * @param deadline Absolute steady-clock deadline (unbounded by default).
     * @param cancel_token Optional cancellation token for this request.
     * @return The validated JSON-RPC `result` field.
     */
    asio::awaitable<json> rpc_call_async(
        const std::string& method,
        const json& params = json::object(),
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max(),
        std::shared_ptr<graph::CancelToken> cancel_token = {});

  private:
    std::shared_ptr<detail::ProtocolSession> session_;
};

} // namespace neograph::mcp
