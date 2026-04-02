#pragma once

#include <neograph/tool.h>
#include <string>
#include <vector>
#include <memory>

namespace neograph::mcp {

// MCP Tool — wraps a remote MCP server tool as a local Tool
class MCPTool : public Tool {
  public:
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    ChatTool get_definition() const override;
    std::string execute(const json& arguments) override;
    std::string get_name() const override { return name_; }

  private:
    std::string server_url_;
    std::string name_;
    std::string description_;
    json input_schema_;
    std::string session_id_;
};

// MCP Client — connects to MCP server, discovers tools
class MCPClient {
  public:
    explicit MCPClient(const std::string& server_url);

    // Initialize connection and handshake
    bool initialize(const std::string& client_name = "neograph");

    // Discover tools from server
    std::vector<std::unique_ptr<Tool>> get_tools();

    // Call a tool directly
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
