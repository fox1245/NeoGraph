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
 *     subprocess lives as long as any MCPTool produced by the client, unless
 *     MCPClient::shutdown() explicitly revokes the shared session.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/mcp/types.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <functional>
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
class NEOGRAPH_API MCPTool : public AsyncTool, public ContextualAsyncTool {
  public:
    /// Direct HTTP constructor for one known tool. The tool opens one MCP
    /// session on its first call, reuses it for every later call, and ends it
    /// with DELETE when the tool is destroyed. `server_url` is the server's
    /// MCP endpoint URL, used as given. Prefer MCPClient(url).get_tools(),
    /// which shares one session among all of a server's tools and keeps
    /// their complete definitions.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    /// Full MCP definition, including outputSchema and annotations.
    const ToolDefinition& get_mcp_definition() const noexcept { return definition_; }

    /// Typed execution preserving all content block kinds and MCP result fields.
    CallToolResult execute_result(const json& arguments);
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments);
    /// Typed execution bounded by the dispatcher's context: its cancel token
    /// and deadline stop waiting for the response, which is then dropped.
    /// Cancellation throws graph::CancelledException; a deadline or request
    /// timeout throws asio::system_error(asio::error::timed_out).
    asio::awaitable<CallToolResult> execute_result_async(const json& arguments,
                                                         ToolExecutionContext context);

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

    /// What every dispatcher calls (ToolExecutionController): the call is
    /// bounded by the run's cancel token and deadline, as above. An `isError`
    /// result throws std::runtime_error, an ordinary tool failure.
    asio::awaitable<std::string> execute_async(const json& arguments,
                                               ToolExecutionContext context) override;

    std::string get_name() const override { return definition_.name; }

  private:
    friend class MCPClient;
    MCPTool(std::shared_ptr<detail::ProtocolSession> session,
            ToolDefinition definition);
    /// The session calls use; the URL constructor creates it on first use.
    std::shared_ptr<detail::ProtocolSession> protocol_session();

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
 * // HTTP: the server's MCP endpoint URL, as given
 * MCPClient http_client("http://localhost:8000/mcp");
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
     * @brief Construct an HTTP-mode (Streamable HTTP) MCP client.
     * @param server_url The server's MCP endpoint URL, used as given, path and
     *        query included (e.g., "http://localhost:8000/mcp"). Nothing is
     *        appended to it.
     *
     * The client answers a server's `ping` and refuses other server requests
     * with -32601 (it declares no client capability), reads JSON and
     * Server-Sent Event responses, resumes an event stream the server closed
     * early (GET with Last-Event-ID), opens the optional GET event stream for
     * server messages between calls once its session is initialized, starts
     * a new session when the server answers 404 for its session and sends the
     * request again once, and ends its session with DELETE on shutdown() or
     * when the last reference goes. A 307 or 308 answer is followed, with the
     * request's method, body and headers, up to five times and only on the
     * endpoint URL's origin (scheme, host and port); any other redirect is
     * an error with its HTTP status.
     */
    explicit MCPClient(const std::string& server_url);
    MCPClient(const std::string& server_url, MCPClientConfig config);

    /**
     * @brief Construct a stdio-mode MCP client by spawning a subprocess.
     * @param argv Command + arguments (e.g., {"python", "server.py"}). argv[0]
     *             is resolved via PATH before fork. Pipe/fork failures throw
     *             during construction; exec failure surfaces on the first RPC.
     *
     * The subprocess tree is terminated when the last reference to the
     * underlying session is dropped, or when shutdown() is called.
     * This legacy overload inherits environment, cwd, and stderr.
     */
    explicit MCPClient(std::vector<std::string> argv);
    /// Hardened local launch with validated replacement environment, cwd,
    /// bounded stderr capture, startup/request deadlines, and process-tree ownership.
    explicit MCPClient(StdioClientConfig config);

    MCPClient(const MCPClient&) = delete;
    MCPClient& operator=(const MCPClient&) = delete;
    MCPClient(MCPClient&&) = delete;
    MCPClient& operator=(MCPClient&&) = delete;


    /// Permanently revoke this client and all tools retaining its session.
    /// Aborts and drains transport work and terminates the owned subprocess tree,
    /// independently of shared ownership. Repeated and concurrent calls are safe.
    /// A call from a transport callback cancels in place without self-joining;
    /// its current operation unwinds with MCPFailure::shutdown.
    /// Subsequent requests fail with MCPFailure::shutdown.
    void shutdown() noexcept;
    /**
     * @brief Initialize the connection and perform the MCP handshake.
     * @param client_name Client identifier sent during handshake (default: "neograph").
     * @return True after initialization. Protocol and transport failures throw.
     */
    bool initialize(const std::string& client_name = "neograph");

    /// Async handshake + initialized notification for either transport.
    asio::awaitable<bool> initialize_async(const std::string& client_name = "neograph");

    /// Bounded handshake: the deadline (absolute, steady clock) and cancel
    /// token stop waiting for it (MCPTransportError timeout or cancelled), and
    /// the client stays uninitialized. MCP never cancels initialize on the
    /// wire, so the server is not told: a stdio server may then refuse a
    /// second handshake, and a hardened stdio session ends.
    bool initialize(const std::string& client_name,
                    std::chrono::steady_clock::time_point deadline,
                    std::shared_ptr<graph::CancelToken> cancel_token);
    asio::awaitable<bool> initialize_async(const std::string& client_name,
                                           std::chrono::steady_clock::time_point deadline,
                                           std::shared_ptr<graph::CancelToken> cancel_token);

    /// Called when the server sends notifications/tools/list_changed: read
    /// get_tool_definitions() or get_tools() again for the new catalogue.
    /// Until then outputSchema checks are skipped, as the cached schemas may
    /// be stale. The notification may come on a call's response stream, on
    /// stdio at any time, or over HTTP on the optional GET event stream the
    /// client opens for its session after initialize. The handler runs on the
    /// client's transport thread: return promptly and do not call this
    /// client from it. An empty handler stops the callbacks.
    void set_tool_list_changed_handler(std::function<void()> handler);

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
    /// blocking and returns the raw result.
    asio::awaitable<json> call_tool_async(const std::string& name,
                                          const json& arguments);
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name,
        const json& arguments);
    /// Bounded variant: the deadline (absolute, steady clock) and cancel token
    /// stop waiting for this call only, as for rpc_call_async().
    asio::awaitable<CallToolResult> call_tool_result_async(
        const std::string& name,
        const json& arguments,
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<graph::CancelToken> cancel_token);

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
