#include <neograph/mcp/client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <stdexcept>
#include <iostream>

namespace neograph::mcp {

// --- MCPTool ---

MCPTool::MCPTool(const std::string& server_url,
                 const std::string& name,
                 const std::string& description,
                 const json& input_schema)
  : server_url_(server_url)
  , name_(name)
  , description_(description)
  , input_schema_(input_schema)
{
}

ChatTool MCPTool::get_definition() const {
    return { name_, description_, input_schema_ };
}

std::string MCPTool::execute(const json& arguments) {
    MCPClient client(server_url_);
    client.initialize();
    auto result = client.call_tool(name_, arguments);

    // MCP returns { content: [{type: "text", text: "..."}], isError: false }
    if (result.contains("content") && result["content"].is_array()) {
        std::string output;
        for (const auto& item : result["content"]) {
            if (item.value("type", "") == "text") {
                if (!output.empty()) output += "\n";
                output += item.value("text", "");
            }
        }
        return output;
    }

    return result.dump();
}

// --- MCPClient ---

MCPClient::MCPClient(const std::string& server_url)
  : server_url_(server_url)
{
    // Parse URL into host + path_prefix
    host_ = server_url;
    auto scheme_end = host_.find("://");
    if (scheme_end != std::string::npos) {
        auto path_start = host_.find('/', scheme_end + 3);
        if (path_start != std::string::npos) {
            path_prefix_ = host_.substr(path_start);
            host_ = host_.substr(0, path_start);
        }
    }
}

json MCPClient::rpc_call(const std::string& method, const json& params) {
    json body;
    body["jsonrpc"] = "2.0";
    body["id"] = ++request_id_;
    body["method"] = method;
    body["params"] = params;

    httplib::Client cli(host_);
    cli.set_read_timeout(30, 0);
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"Host", "localhost"}
    };

    if (!session_id_.empty()) {
        headers.insert({"Mcp-Session-Id", session_id_});
    }

    auto res = cli.Post(path_prefix_ + "/mcp", headers, body.dump(), "application/json");

    if (!res) {
        throw std::runtime_error("MCP request failed: " + httplib::to_string(res.error()));
    }

    // Store session ID from response
    auto sid = res->get_header_value("Mcp-Session-Id");
    if (!sid.empty()) {
        session_id_ = sid;
    }

    if (res->status != 200) {
        throw std::runtime_error("MCP error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    // Parse response — may be plain JSON or SSE (event: message\ndata: {...})
    std::string body_str = res->body;
    json resp;

    auto data_pos = body_str.find("data: ");
    if (data_pos != std::string::npos) {
        // SSE format: extract JSON from "data: " line
        auto json_start = data_pos + 6;
        auto json_end = body_str.find('\n', json_start);
        std::string json_str = (json_end != std::string::npos)
            ? body_str.substr(json_start, json_end - json_start)
            : body_str.substr(json_start);
        resp = json::parse(json_str);
    } else {
        resp = json::parse(body_str);
    }

    if (resp.contains("error")) {
        auto err = resp["error"];
        throw std::runtime_error("MCP RPC error: " + err.value("message", "unknown"));
    }

    return resp.value("result", json::object());
}

bool MCPClient::initialize(const std::string& client_name) {
    json params;
    params["protocolVersion"] = "2025-03-26";
    params["capabilities"] = json::object();
    params["clientInfo"] = {{"name", client_name}, {"version", "0.1.0"}};

    auto result = rpc_call("initialize", params);

    // Send initialized notification (no id = notification)
    json notify;
    notify["jsonrpc"] = "2.0";
    notify["method"] = "notifications/initialized";
    notify["params"] = json::object();

    httplib::Client cli(host_);
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json, text/event-stream"},
        {"Host", "localhost"}
    };
    if (!session_id_.empty()) {
        headers.insert({"Mcp-Session-Id", session_id_});
    }
    cli.Post(path_prefix_ + "/mcp", headers, notify.dump(), "application/json");

    return true;
}

std::vector<std::unique_ptr<Tool>> MCPClient::get_tools() {
    initialize();

    auto result = rpc_call("tools/list", json::object());
    std::vector<std::unique_ptr<Tool>> tools;

    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) {
            auto name = t.value("name", "");
            auto desc = t.value("description", "");
            auto schema = t.value("inputSchema", json::object());

            tools.push_back(std::make_unique<MCPTool>(
                server_url_, name, desc, schema));
        }
    }

    return tools;
}

json MCPClient::call_tool(const std::string& name, const json& arguments) {
    json params;
    params["name"] = name;
    params["arguments"] = arguments;
    return rpc_call("tools/call", params);
}

} // namespace neograph::mcp
