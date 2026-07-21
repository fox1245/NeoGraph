#include <neograph/mcp/harness_mcp_backend.h>

#include <neograph/mcp/client.h>

#include <stdexcept>
#include <utility>

namespace neograph::mcp {

HarnessCapabilityExecutor make_mcp_harness_capability_executor(
    std::map<std::string, std::shared_ptr<MCPClient>> clients) {
    return [clients = std::move(clients)](
               const json& tool, const json& arguments,
               const std::shared_ptr<graph::CancelToken>& cancel) {
        cancel->throw_if_cancelled("before downstream MCP call");
        const auto executor = tool["executor"];
        const auto server_ref = executor["server_ref"].get<std::string>();
        auto it = clients.find(server_ref);
        if (it == clients.end() || !it->second) {
            throw std::runtime_error("unresolved MCP server_ref: " + server_ref);
        }
        const auto tool_name = executor.value("tool", tool["id"].get<std::string>());
        auto result = it->second->call_tool_result(tool_name, arguments);
        if (result.is_error) {
            throw std::runtime_error("downstream MCP tool returned isError: "
                                     + result.to_json().dump());
        }
        cancel->throw_if_cancelled("after downstream MCP call");
        if (!result.structured_content.is_null()) return result.structured_content;
        return result.to_json();
    };
}

} // namespace neograph::mcp
