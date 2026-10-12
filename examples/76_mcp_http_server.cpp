// NeoGraph Example 76: an MCP server over Streamable HTTP, with a bearer token
//
// Example 59 serves one `echo` tool over stdio. This one serves the same tool over HTTP with
// neograph::mcp::MCPHttpServer, and calls it with NeoGraph's own client in the same process.
// The server takes a free port (port 0) and checks a bearer token on every request. The token is
// made up for this run; a real deployment validates tokens issued by its own authority in
// `bearer_authorizer`. A client that sends no token is refused with HTTP 401.
//
// Build with -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=ON (opt-in).
//
// smoke-expect: echo tool: {"message":"hello over http"}
// smoke-expect: without a token: refused, HTTP status 401
// smoke-expect: with a wrong token: refused, HTTP status 401
// smoke-forbid: [neograph] lint

#include <neograph/mcp/client.h>
#include <neograph/mcp/http_server.h>

#include <iostream>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>

using namespace neograph;

namespace {

// One session is one MCPServer with the echo tool registered, as in example 59.
std::unique_ptr<mcp::MCPServer> make_echo_server() {
    mcp::MCPServerConfig config;
    config.server_info = {{"name", "http-echo"}, {"version", "1.0.0"}};
    auto server = std::make_unique<mcp::MCPServer>(std::move(config));

    mcp::ToolDefinition echo;
    echo.name = "echo";
    echo.description = "Return the supplied message.";
    echo.input_schema = {{"type", "object"},
                         {"required", json::array({"message"})},
                         {"properties", {{"message", {{"type", "string"}}}}}};
    server->register_tool(std::move(echo), [](const json& arguments, const auto&) {
        mcp::CallToolResult result;
        result.structured_content = {{"message", arguments.at("message")}};
        result.content = json::array({{{"type", "text"}, {"text", result.structured_content.dump()}}});
        return result;
    });
    return server;
}

// What a refused handshake looks like to the caller.
void try_connect(const std::string& label, const std::string& url, mcp::HeaderList headers) {
    mcp::MCPClientConfig config;
    config.headers = std::move(headers);
    try {
        mcp::MCPClient client(url, std::move(config));
        client.initialize("example-mcp-http-server");
        std::cout << label << ": accepted\n";
    } catch (const mcp::MCPError& error) {  // the server's HTTP error response, with its status
        std::cout << label << ": refused, HTTP status " << error.http_status() << "\n";
    }
}

}  // namespace

int main() {
    try {
        // A different token on every run.
        std::random_device entropy;
        std::ostringstream token;
        for (int i = 0; i < 4; ++i) token << std::hex << std::setw(8) << std::setfill('0') << entropy();

        mcp::MCPHttpServerConfig config;
        config.port = 0;  // the OS picks a free port
        config.bearer_authorizer = [expected = token.str()](std::string_view presented) -> std::optional<std::string> {
            // The returned scope names the caller; sessions are bound to it.
            if (presented == expected) return std::string("demo-caller");
            return std::nullopt;
        };
        mcp::MCPHttpServer server(
            [](std::string_view /*scope*/) {
                mcp::MCPHttpServerSession session;
                session.server = make_echo_server();
                return session;
            },
            std::move(config));
        if (!server.start_async()) {
            std::cerr << "could not start the HTTP server\n";
            return 1;
        }
        const std::string url = "http://127.0.0.1:" + std::to_string(server.port()) + "/mcp";

        // With the token: the normal client path.
        mcp::MCPClientConfig with_token;
        with_token.headers = {{"Authorization", "Bearer " + token.str()}};
        mcp::MCPClient client(url, std::move(with_token));
        client.initialize("example-mcp-http-server");
        const auto reply = client.call_tool_result("echo", {{"message", "hello over http"}});
        std::cout << "echo tool: " << reply.content[0]["text"].get<std::string>() << "\n";

        // Without it, and with a wrong one.
        try_connect("without a token", url, {});
        try_connect("with a wrong token", url, {{"Authorization", "Bearer not-the-token"}});

        client.shutdown();
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
