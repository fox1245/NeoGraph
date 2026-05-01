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

#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <string>
#include <vector>
#include <memory>

namespace neograph::mcp {

namespace detail {
/// Opaque stdio session. Holds the subprocess pid, pipe fds, read buffer,
/// and a mutex serialising concurrent rpc_call() calls. Destroying the
/// session sends SIGTERM to the child and reaps it via waitpid.
class StdioSession;
}

/**
 * @brief Wraps a remote MCP server tool as a local Tool.
 *
 * MCPTool implements the Tool interface by forwarding execute() calls
 * through the owning transport (HTTP or stdio). Created automatically
 * by MCPClient::get_tools().
 */
class MCPTool : public Tool {
  public:
    /// HTTP-mode constructor. Each execute() opens an ephemeral
    /// MCPClient against @p server_url.
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

    ChatTool get_definition() const override;

    /**
     * @brief Execute the tool on the MCP server.
     * @param arguments JSON object containing the tool's input parameters.
     * @return Result string (text content joined by newlines) or JSON dump.
     */
    std::string execute(const json& arguments) override;

    std::string get_name() const override { return name_; }

  private:
    std::string server_url_;                              ///< Non-empty in HTTP mode.
    std::shared_ptr<detail::StdioSession> stdio_session_; ///< Non-null in stdio mode.
    std::string name_;
    std::string description_;
    json input_schema_;
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
class MCPClient {
  public:
    /**
     * @brief Construct an HTTP-mode MCP client.
     * @param server_url URL of the MCP server (e.g., "http://localhost:8000").
     */
    explicit MCPClient(const std::string& server_url);

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

    /**
     * @brief Initialize the connection and perform the MCP handshake.
     * @param client_name Client identifier sent during handshake (default: "neograph").
     * @return True if initialization succeeded, false otherwise.
     */
    bool initialize(const std::string& client_name = "neograph");

    /**
     * @brief Discover tools from the MCP server.
     * @return Vector of Tool unique_ptrs (MCPTool instances).
     */
    std::vector<std::unique_ptr<Tool>> get_tools();

    /**
     * @brief Call a tool directly by name.
     * @param name Tool name on the server.
     * @param arguments JSON object of tool arguments.
     * @return JSON response from the server.
     */
    json call_tool(const std::string& name, const json& arguments);

    /**
     * @brief Async variant of rpc_call for the HTTP transport.
     *
     * stdio sessions still drive their own blocking exchange (see 2.7
     * for the asio::posix migration); this awaitable resolves to the
     * stdio result on the calling thread when the client is in stdio
     * mode, so callers can use one async path uniformly.
     *
     * @param method JSON-RPC method name.
     * @param params Method parameters (defaults to empty object).
     * @return Awaitable resolving to the `result` field of the JSON-RPC response.
     */
    asio::awaitable<json> rpc_call_async(
        const std::string& method,
        const json& params = json::object());

  private:
    /// Sync rpc_call — for stdio it dispatches synchronously; for HTTP
    /// it routes through `run_sync(rpc_call_async(...))`.
    json rpc_call(const std::string& method, const json& params = json::object());

    // HTTP state (empty strings when in stdio mode).
    std::string server_url_;
    std::string host_;
    std::string path_prefix_;
    std::string session_id_;
    /// Protocol version negotiated during initialize. The MCP spec
    /// requires the client to echo this on every subsequent HTTP request
    /// in the `MCP-Protocol-Version` header (transports/Streamable HTTP §
    /// "Protocol Version Header", spec 2025-11-25). Empty until
    /// initialize completes.
    std::string negotiated_protocol_version_;
    int request_id_ = 0;

    // stdio state (null when in HTTP mode).
    std::shared_ptr<detail::StdioSession> stdio_session_;
};

} // namespace neograph::mcp
