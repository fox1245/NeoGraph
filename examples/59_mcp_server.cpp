// NeoGraph Example 59: a minimal MCP server over stdio.
//
// One tool, `echo`, with an input schema, an output schema and structured content.
// It has no LLM and no MCP client dependency, and it is the same stdio binary the MCP
// interoperability tests drive. Point any MCP host at it, or pipe JSON-RPC to it:
//   printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}}' | ./example_mcp_server
// The server stops when stdin closes.
//
// smoke-stdin: {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}}
// smoke-stdin: {"jsonrpc":"2.0","method":"notifications/initialized"}
// smoke-stdin: {"jsonrpc":"2.0","id":2,"method":"tools/list"}
// smoke-expect: "serverInfo":{"name":"neograph-example"
// smoke-expect: "name":"echo"

#include <neograph/mcp/server.h>

#include <utility>

int main() {
    neograph::mcp::MCPServerConfig config;
    config.server_info = {
        {"name", "neograph-example"},
        {"version", "1.0.0"},
    };
    config.instructions = "Call echo to verify MCP server connectivity.";

    neograph::mcp::MCPServer server(std::move(config));
    neograph::mcp::ToolDefinition echo;
    echo.name = "echo";
    echo.title = "Echo";
    echo.description = "Return the supplied message.";
    echo.input_schema = {
        {"type", "object"},
        {"required", neograph::json::array({"message"})},
        {"properties", {{"message", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    echo.output_schema = {
        {"type", "object"},
        {"required", neograph::json::array({"message"})},
        {"properties", {{"message", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    echo.annotations = {{"readOnlyHint", true}, {"idempotentHint", true}};

    server.register_tool(std::move(echo),
        [](const neograph::json& arguments, const auto& cancel) {
            cancel->throw_if_cancelled("before echo");
            const auto message = arguments.at("message").get<std::string>();
            neograph::mcp::CallToolResult result;
            result.structured_content = {{"message", message}};
            // Structured results also carry their JSON as text, for clients
            // that do not read structuredContent (MCP tools, "Structured
            // Content").
            result.content = neograph::json::array({
                {{"type", "text"}, {"text", result.structured_content.dump()}},
            });
            return result;
        });
    server.run();
    return 0;
}
