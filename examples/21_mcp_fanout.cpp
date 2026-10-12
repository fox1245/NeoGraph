// NeoGraph Example 21: MCP tool calls fanned out with Send
//
// One planner node emits a Send per tool call. Each Send runs a node that calls the MCP
// server, and the three calls overlap inside one super-step instead of running one after
// another. No model is involved: the planner picks the calls itself, so the output is
// deterministic. The server is examples/mcp_demo_server.py (standard library only).
//
// Usage:
//   python3 examples/mcp_demo_server.py --http 8765          # terminal 1
//   ./example_mcp_fanout http://127.0.0.1:8765/mcp           # terminal 2
//
// smoke-companion: @PYTHON@ @SRC@/examples/mcp_demo_server.py --http @PORT@
// smoke-wait-port: @PORT@
// smoke-args: http://127.0.0.1:@PORT@/mcp
// smoke-expect: [calculate] 65537
// smoke-expect: [get_weather] Tokyo: 22C, clear
// smoke-expect: findings: 3
// smoke-forbid: [neograph] lint

#include <neograph/mcp/client.h>
#include <neograph/neograph.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Emits one Send per MCP call. Each Send's payload is written on its own branch,
// so the caller node reads tool_name and tool_args from its state.
class PlannerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        const std::pair<const char*, json> calls[] = {
            {"get_current_time", {{"timezone", "UTC"}}},
            {"get_weather", {{"city", "Tokyo"}}},
            {"calculate", {{"expression", "2 ** 16 + 1"}}},
        };
        NodeOutput out;
        for (const auto& [tool, args] : calls)
            out.sends.push_back(Send{"mcp_caller", json{{"tool_name", tool}, {"tool_args", args}}});
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
};

// One branch of the fan-out: call the tool, append what it returned to "findings".
class CallerNode : public GraphNode {
public:
    explicit CallerNode(std::shared_ptr<mcp::MCPClient> client) : client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const std::string tool = in.state.get("tool_name").get<std::string>();
        const auto reply = co_await client_->call_tool_result_async(tool, in.state.get("tool_args"));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "findings", json::array({json{{"tool", tool}, {"text", reply.content[0]["text"]}}})});
        co_return out;
    }
    std::string get_name() const override { return "mcp_caller"; }

private:
    std::shared_ptr<mcp::MCPClient> client_;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string url = argc > 1 ? argv[1] : "http://127.0.0.1:8765/mcp";
    try {
        // One client is shared by every branch.
        auto client = std::make_shared<mcp::MCPClient>(url);
        client->initialize("example-mcp-fanout");

        auto& factory = NodeFactory::instance();
        factory.register_type("planner", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>();
        });
        factory.register_type("mcp_caller", [client](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<CallerNode>(client);
        });

        const json definition = {
            {"schema_version", TOPOLOGY_SCHEMA_VERSION},
            {"name", "mcp_fanout"},
            {"channels", {{"tool_name", {{"reducer", "overwrite"}}},
                          {"tool_args", {{"reducer", "overwrite"}}},
                          {"findings", {{"reducer", "append"}}}}},
            {"nodes", {{"planner", {{"type", "planner"}}}, {"mcp_caller", {{"type", "mcp_caller"}}}}},
            {"edges", {{{"from", "__start__"}, {"to", "planner"}},
                       {{"from", "planner"}, {"to", "__end__"}}}},
        };

        // The branches overlap only with more than one worker. mcp_caller is reached only
        // through Send, which the linter cannot see, so its "unreachable" notice is switched off.
        EngineConfig config;
        config.worker_count = 3;
        config.stderr_notices = false;
        auto engine = GraphEngine::build(definition, std::move(config));

        const auto result = engine->run(RunConfig{});

        // Branches finish in any order: sort by tool name before printing.
        std::vector<json> sorted;
        for (const auto& finding : result.channel<json>("findings")) sorted.push_back(finding);
        std::sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
            return a["tool"].get<std::string>() < b["tool"].get<std::string>();
        });
        for (const auto& finding : sorted)
            std::cout << "[" << finding["tool"].get<std::string>() << "] "
                      << finding["text"].get<std::string>() << "\n";
        std::cout << "findings: " << sorted.size() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n"
                  << "Is an MCP server listening at " << url << "?  python3 examples/mcp_demo_server.py --http 8765\n";
        return 1;
    }
}
