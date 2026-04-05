/**
 * @file mcp/client.h
 * @brief MCP (Model Context Protocol) client for connecting to remote tool servers.
 *
 * Provides MCPClient for discovering and calling tools on MCP servers,
 * and MCPTool for wrapping remote tools as local Tool objects.
 * Supports JSON-RPC 2.0 over HTTP (Streamable HTTP transport).
 */
#pragma once

#include <neograph/tool.h>
#include <string>
#include <vector>
#include <memory>

namespace neograph::mcp {

/**
 * @brief Wraps a remote MCP server tool as a local Tool.
 *
 * MCPTool implements the Tool interface by forwarding execute() calls
 * to the remote MCP server via JSON-RPC. Created automatically by
 * MCPClient::get_tools().
 */
class MCPTool : public Tool {
  public:
    /**
     * @brief Construct an MCP tool wrapper.
     * @param server_url URL of the MCP server (e.g., "http://localhost:8000").
     * @param name Tool name as reported by the server.
     * @param description Tool description as reported by the server.
     * @param input_schema JSON Schema for the tool's input parameters.
     */
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    ChatTool get_definition() const override;

    /**
     * @brief Execute the tool on the remote MCP server.
     * @param arguments JSON object containing the tool's input parameters.
     * @return Result string from the remote server.
     */
    std::string execute(const json& arguments) override;

    std::string get_name() const override { return name_; }

  private:
    std::string server_url_;
    std::string name_;
    std::string description_;
    json input_schema_;
    std::string session_id_;
};

/**
 * @brief Client for connecting to MCP (Model Context Protocol) servers.
 *
 * Handles the initialization handshake, tool discovery, and tool
 * invocation over JSON-RPC 2.0.
 *
 * @code
 * MCPClient client("http://localhost:8000");
 * client.initialize("my-agent");
 * auto tools = client.get_tools();  // Discover available tools
 * @endcode
 */
class MCPClient {
  public:
    /**
     * @brief Construct an MCP client.
     * @param server_url URL of the MCP server (e.g., "http://localhost:8000/mcp").
     */
    explicit MCPClient(const std::string& server_url);

    /**
     * @brief Initialize the connection and perform the MCP handshake.
     * @param client_name Client identifier sent during handshake (default: "neograph").
     * @return True if initialization succeeded, false otherwise.
     */
    bool initialize(const std::string& client_name = "neograph");

    /**
     * @brief Discover tools from the MCP server.
     *
     * Queries the server for available tools and returns them as
     * Tool objects ready for use with Agent or GraphEngine.
     *
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

  private:
    json rpc_call(const std::string& method, const json& params = json::object());

    std::string server_url_;
    std::string host_;
    std::string path_prefix_;
    std::string session_id_;
    int request_id_ = 0;
};

} // namespace neograph::mcp
