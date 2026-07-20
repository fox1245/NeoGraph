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
            result.content = neograph::json::array({
                {{"type", "text"}, {"text", message}},
            });
            result.structured_content = {{"message", message}};
            return result;
        });
    server.run();
    return 0;
}
