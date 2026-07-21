#include <neograph/a2a/harness_backend.h>

#include <neograph/a2a/client.h>

#include <stdexcept>
#include <utility>

namespace neograph::a2a {

mcp::HarnessCapabilityExecutor make_harness_capability_executor(
    std::map<std::string, std::shared_ptr<A2AClient>> agents) {
    return [agents = std::move(agents)](
               const json& tool, const json& arguments,
               const std::shared_ptr<graph::CancelToken>& cancel) {
        cancel->throw_if_cancelled("before downstream A2A call");
        const auto agent = tool["executor"]["agent"].get<std::string>();
        auto it = agents.find(agent);
        if (it == agents.end() || !it->second) {
            throw std::runtime_error("unresolved A2A agent: " + agent);
        }
        auto task = it->second->send_message_sync(arguments.dump());
        cancel->throw_if_cancelled("after downstream A2A call");
        json result;
        to_json(result, task);
        return result;
    };
}

} // namespace neograph::a2a
