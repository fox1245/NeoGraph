// NeoGraph MCP probe server for the official-SDK interop suites
// (bindings/python/tests/interop/mcp). Public MCPServer / MCPHttpServer API
// only. No model, no network beyond loopback.
//
//   neograph_mcp_interop_probe stdio [--untyped-tool]
//   neograph_mcp_interop_probe http <port> [--untyped-tool]
//     env NG_PROBE_ALLOWED_ORIGINS=a,b   exact allowed Origin values
//     env NG_PROBE_BEARER=<token>        require this bearer token, and publish
//                                        RFC 9728 protected resource metadata
#include <neograph/graph/types.h>
#include <neograph/mcp/http_server.h>
#include <neograph/mcp/server.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using neograph::json;
namespace mcp = neograph::mcp;

namespace {

json object_schema(json properties, json required) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

mcp::CallToolResult text_result(const std::string& text) {
    mcp::CallToolResult result;
    result.content = json::array({{{"type", "text"}, {"text", text}}});
    return result;
}

void register_probe_tools(mcp::MCPServer& server, bool untyped_tool) {
    {   // structured output, as examples/59_mcp_server.cpp
        mcp::ToolDefinition d;
        d.name          = "echo";
        d.title         = "Echo";
        d.description   = "Return the supplied message.";
        d.input_schema  = object_schema({{"message", {{"type", "string"}}}}, {"message"});
        d.output_schema = object_schema({{"message", {{"type", "string"}}}}, {"message"});
        d.annotations   = {{"readOnlyHint", true}, {"idempotentHint", true}};
        server.register_tool(d, [](const json& args, const auto& cancel) {
            cancel->throw_if_cancelled("before echo");
            auto message = args.at("message").get<std::string>();
            auto result  = text_result(json{{"message", message}}.dump());
            result.structured_content = {{"message", message}};
            return result;
        });
    }
    {   // plain text, no outputSchema
        mcp::ToolDefinition d;
        d.name         = "echo_text";
        d.description  = "Return the supplied message as text.";
        d.input_schema = object_schema({{"message", {{"type", "string"}}}}, {"message"});
        server.register_tool(d, [](const json& args, const auto&) {
            return text_result(args.at("message").get<std::string>());
        });
    }
    {   // handler throws: tool execution error
        mcp::ToolDefinition d;
        d.name         = "fail";
        d.description  = "Always throws.";
        d.input_schema = object_schema(json::object(), json::array());
        server.register_tool(d, [](const json&, const auto&) -> mcp::CallToolResult {
            // Only a failure's public message and code reach the client.
            throw neograph::graph::NodeFailureError("probe.failure", "probe tool failure", false);
        });
    }
    {   // handler reports isError itself
        mcp::ToolDefinition d;
        d.name         = "soft_error";
        d.description  = "Returns isError=true.";
        d.input_schema = object_schema(json::object(), json::array());
        server.register_tool(d, [](const json&, const auto&) {
            auto r     = text_result("soft failure reported by the tool");
            r.is_error = true;
            return r;
        });
    }
    {   // structuredContent violates the advertised outputSchema
        mcp::ToolDefinition d;
        d.name          = "bad_output";
        d.description   = "Returns structuredContent that violates outputSchema.";
        d.input_schema  = object_schema(json::object(), json::array());
        d.output_schema = object_schema({{"count", {{"type", "integer"}}}}, {"count"});
        server.register_tool(d, [](const json&, const auto&) {
            auto r               = text_result("{\"count\":\"x\"}");
            r.structured_content = {{"count", "x"}};
            return r;
        });
    }
    {   // cancellable slow tool
        mcp::ToolDefinition d;
        d.name         = "slow";
        d.description  = "Sleeps cooperatively; cancellable.";
        d.input_schema = object_schema({{"seconds", {{"type", "number"}}}}, {"seconds"});
        server.register_tool(d, [](const json& args, const auto& cancel) {
            const auto until = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(
                                   static_cast<long>(args.at("seconds").get<double>() * 1000));
            while (std::chrono::steady_clock::now() < until) {
                cancel->throw_if_cancelled("slow tool cancelled");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return text_result("slept");
        });
    }
    {   // 2025-11-25 metadata surface: title, icons, annotations, execution, _meta
        mcp::ToolDefinition d;
        d.name         = "decorated";
        d.title        = "Decorated tool";
        d.description  = "Carries every optional Tool field.";
        d.icons        = json::array({{{"src", "https://example.com/icon.png"},
                                       {"mimeType", "image/png"},
                                       {"sizes", json::array({"48x48"})}}});
        d.input_schema = {{"type", "object"},
                          {"$schema", "https://json-schema.org/draft/2020-12/schema"},
                          {"properties", {{"n", {{"type", "integer"}, {"minimum", 0}}}}}};
        d.annotations  = {{"title", "Decorated"}, {"readOnlyHint", true},
                          {"destructiveHint", false}, {"idempotentHint", true},
                          {"openWorldHint", false}};
        d.execution    = {{"taskSupport", "forbidden"}};
        d.meta         = {{"io.neograph.probe/kind", "decorated"}};
        server.register_tool(d, [](const json&, const auto&) {
            mcp::CallToolResult r;
            r.content = json::array({
                {{"type", "text"}, {"text", "rich"}},
                {{"type", "image"}, {"data", "iVBORw0KGgo="}, {"mimeType", "image/png"}},
                {{"type", "resource_link"}, {"uri", "file:///tmp/x.txt"}, {"name", "x.txt"}},
            });
            return r;
        });
    }
    if (untyped_tool) {   // the ToolDefinition default inputSchema ({}), no "type"
        mcp::ToolDefinition d;
        d.name        = "untyped";
        d.description = "Registered without setting input_schema.";
        server.register_tool(d, [](const json&, const auto&) { return text_result("ok"); });
    }
}

std::unique_ptr<mcp::MCPServer> make_server(bool untyped_tool) {
    mcp::MCPServerConfig config;
    config.server_info  = {{"name", "neograph-mcp-probe"}, {"version", "0.1.0"}};
    config.instructions = "NeoGraph MCP interop probe server.";
    auto server         = std::make_unique<mcp::MCPServer>(std::move(config));
    register_probe_tools(*server, untyped_tool);
    return server;
}

std::vector<std::string> split_csv(const char* value) {
    std::vector<std::string> out;
    if (!value) return out;
    std::stringstream in(value);
    std::string       item;
    while (std::getline(in, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: neograph_mcp_interop_probe stdio|http <port> [--untyped-tool]\n";
        return 2;
    }
    bool untyped = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--untyped-tool") == 0) untyped = true;
    }
    const std::string mode = argv[1];
    if (mode == "stdio") {
        auto server = make_server(untyped);
        server->run();
        return 0;
    }
    if (mode != "http" || argc < 3) {
        std::cerr << "usage: neograph_mcp_interop_probe stdio|http <port> [--untyped-tool]\n";
        return 2;
    }
    mcp::MCPHttpServerConfig config;
    config.host            = "127.0.0.1";
    config.port            = std::atoi(argv[2]);
    config.allowed_origins = split_csv(std::getenv("NG_PROBE_ALLOWED_ORIGINS"));
    config.response_timeout = std::chrono::seconds(30);
    if (const char* token = std::getenv("NG_PROBE_BEARER"); token && *token) {
        const std::string expected = token;
        config.bearer_authorizer =
            [expected](std::string_view candidate) -> std::optional<std::string> {
            if (candidate == expected) return std::string("probe-principal");
            return std::nullopt;
        };
        config.protected_resource_metadata = {
            {"resource", "http://127.0.0.1:" + std::to_string(config.port) + "/mcp"},
            {"authorization_servers", json::array({"https://auth.example"})},
            {"scopes_supported", json::array({"mcp:tools"})},
            {"bearer_methods_supported", json::array({"header"})},
        };
    }
    mcp::MCPHttpServer http(
        [untyped](std::string_view) {
            mcp::MCPHttpServerSession session;
            session.server = make_server(untyped);
            return session;
        },
        config);
    std::cerr << "neograph_mcp_interop_probe listening on 127.0.0.1:" << config.port << std::endl;
    return http.start() ? 0 : 1;
}
