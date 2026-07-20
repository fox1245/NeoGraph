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

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::mcp {

namespace detail {
/// Opaque stdio session. Holds the subprocess pid, pipe fds, read buffer,
/// and a mutex serialising concurrent rpc_call() calls. Destroying the
/// session sends SIGTERM to the child and reaps it via waitpid.
class StdioSession;
class HttpSession;
class ClientMetadata;
}

/**
 * @brief Wraps a remote MCP server tool as a local Tool.
 *
 * MCPTool implements the Tool interface by forwarding execute() calls
 * through the owning transport (HTTP or stdio). Created automatically
 * by MCPClient::get_tools().
 */
class NEOGRAPH_API MCPTool : public AsyncTool {
  public:
    /// Legacy HTTP-mode constructor. Each execute() opens an ephemeral
    /// MCPClient against @p server_url. Tools returned by MCPClient::get_tools()
    /// instead retain the originating HTTP session.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    /// stdio-mode constructor. The MCPTool keeps the session alive —
    /// the subprocess stays up as long as any tool instance holds a
    /// reference to it.
    MCPTool(std::shared_ptr<detail::StdioSession> session,
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
    MCPTool(std::shared_ptr<detail::HttpSession> session,
            std::shared_ptr<detail::ClientMetadata> metadata,
            ToolDefinition definition);
    MCPTool(std::shared_ptr<detail::StdioSession> session,
            ToolDefinition definition);

    std::string server_url_;                              ///< Non-empty in HTTP mode.
    std::shared_ptr<detail::HttpSession> http_session_;   ///< Shared originating HTTP session.
    std::shared_ptr<detail::ClientMetadata> metadata_;   ///< Shared lifecycle/negotiation state.
    std::shared_ptr<detail::StdioSession> stdio_session_; ///< Non-null in stdio mode.
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
     *             is resolved via PATH (execvp). Throws on fork/exec failure.
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

    /// Async variant of initialize() — runs the handshake + initialized
    /// notification through rpc_call_async, so a coroutine can set up an
    /// HTTP client without blocking a worker thread in run_sync.
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
     * @brief Async variant of rpc_call for the HTTP transport.
     *
     * Both transports are coroutine-native. stdio uses a session-owned
     * io_context and request-id demultiplexer; HTTP awaits async_post.
     *
     * @param method JSON-RPC method name.
     * @param params Method parameters (defaults to empty object).
     * @return Awaitable resolving to the `result` field of the JSON-RPC response.
     */
    asio::awaitable<json> rpc_call_async(
        const std::string& method,
        const json& params = json::object());

  private:
    friend class MCPTool;
    MCPClient(std::shared_ptr<detail::HttpSession> session,
              std::shared_ptr<detail::ClientMetadata> metadata);

    /// Sync rpc_call — for stdio it dispatches synchronously; for HTTP
    /// it routes through `run_sync(rpc_call_async(...))`.
    json rpc_call(const std::string& method, const json& params = json::object());

    std::shared_ptr<detail::HttpSession> http_session_;
    std::shared_ptr<detail::StdioSession> stdio_session_;
    std::shared_ptr<detail::ClientMetadata> metadata_;
};

} // namespace neograph::mcp
