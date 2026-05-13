// NeoGraph Example 21: MCP + Send Fan-out
//
// Pattern: one planner emits a Send per MCP tool call;
// asio::experimental::make_parallel_group runs them concurrently;
// each fan-out branch hits the MCP server independently; a
// summarizer aggregates the results.
//
// Compared to a plain ReAct loop, this collapses N sequential tool calls
// into a single super-step — useful when the model needs several
// independent lookups to answer one question.
//
// Usage (after starting examples/demo_mcp_server.py):
//   ./example_mcp_fanout
//
// No OpenAI required: planner hand-picks three MCP tool calls (time /
// weather / calculator) so the demo is deterministic and stays offline
// for the LLM axis.

#include <neograph/neograph.h>
#include <neograph/mcp/client.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

// =========================================================================
// PlannerNode — emits one Send per MCP tool we want to run in parallel.
// =========================================================================
class PlannerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        // Each Send's `input` is injected as channel writes on the
        // spawned branch, so the mcp_caller can read tool_name /
        // tool_args.
        struct Task { std::string tool; json args; };
        std::vector<Task> tasks = {
            {"get_current_time", json{{"timezone", "UTC"}}},
            {"get_weather",      json{{"city", "Tokyo"}}},
            {"calculate",        json{{"expression", "2 ** 16 + 1"}}},
        };

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"plan", json(tasks.size())});
        for (const auto& t : tasks) {
            out.sends.push_back(Send{"mcp_caller", json{
                {"tool_name", t.tool},
                {"tool_args", t.args}
            }});
        }
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
};

// =========================================================================
// MCPCallerNode — one branch of the fan-out. Reads tool_name / tool_args
// from its Send-injected state, invokes the shared MCP client, appends to
// `findings`.
// =========================================================================
class MCPCallerNode : public GraphNode {
    std::shared_ptr<neograph::mcp::MCPClient> client_;
public:
    explicit MCPCallerNode(std::shared_ptr<neograph::mcp::MCPClient> c)
      : client_(std::move(c)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string tool = in.state.get("tool_name").get<std::string>();
        json        args = in.state.get("tool_args");

        auto t0 = std::chrono::steady_clock::now();
        std::string content;
        try {
            json result = client_->call_tool(tool, args);
            if (result.contains("content") && result["content"].is_array()) {
                for (const auto& item : result["content"]) {
                    if (item.value("type", "") == "text") {
                        if (!content.empty()) content += "\n";
                        content += item.value("text", "");
                    }
                }
            } else {
                content = result.dump();
            }
        } catch (const std::exception& e) {
            content = std::string("(error: ") + e.what() + ")";
        }
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"findings", json::array({json{
            {"tool", tool},
            {"args", args},
            {"result", content},
            {"elapsed_ms", elapsed_ms}
        }})});
        co_return out;
    }
    std::string get_name() const override { return "mcp_caller"; }
};

// =========================================================================
// SummarizerNode — prints each finding; also writes a final string.
// =========================================================================
class SummarizerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto findings = in.state.get("findings");
        std::string summary = "=== MCP fan-out summary ===\n";
        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "  [" + f.value("tool", "?") + "] "
                        +  f.value("result", "")
                        +  "   (" + std::to_string(f.value("elapsed_ms", 0)) + " ms)\n";
            }
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"summary", json(summary)});
        co_return out;
    }
    std::string get_name() const override { return "summarizer"; }
};

int main(int argc, char** argv) {
    const std::string mcp_url = (argc >= 2) ? argv[1] : "http://localhost:8000";

    // One shared MCPClient — all fan-out branches reuse it. HTTP client is
    // session-per-rpc under the hood; for stdio the shared session would be
    // reused across tasks too (see example 22).
    auto client = std::make_shared<neograph::mcp::MCPClient>(mcp_url);
    client->initialize();

    // Register custom node types so the JSON graph can reference them.
    auto& factory = NodeFactory::instance();
    factory.register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>();
        });
    factory.register_type("mcp_caller",
        [client](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<MCPCallerNode>(client);
        });
    factory.register_type("summarizer",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SummarizerNode>();
        });

    json definition = {
        {"name", "mcp_fanout"},
        {"channels", {
            {"plan",       {{"reducer", "overwrite"}}},
            {"tool_name",  {{"reducer", "overwrite"}}},
            {"tool_args",  {{"reducer", "overwrite"}}},
            {"findings",   {{"reducer", "append"}}},
            {"summary",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",    {{"type", "planner"}}},
            {"mcp_caller", {{"type", "mcp_caller"}}},
            {"summarizer", {{"type", "summarizer"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"},  {"to", "planner"}},
            // Send-spawned mcp_caller runs in parallel in the same super-step;
            // after the barrier, control flows from planner to summarizer.
            {{"from", "planner"},    {"to", "summarizer"}},
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(definition, ctx);

    // 3 MCP 호출을 같은 super-step 에서 동시에 보내려면 워커 수를 늘려야 함.
    // v1.0 부터 기본 워커 수가 1 이라 (compile() 주석 참조), 명시 안 하면
    // 세 호출이 순차로 깎여서 wall-time 이 합산된다. set_worker_count_auto()
    // 는 hardware_concurrency() 만큼 풀어준다.
    engine->set_worker_count_auto();

    std::cout << "[*] Fanning out 3 MCP tool calls in parallel...\n\n";

    auto t0 = std::chrono::steady_clock::now();
    RunConfig cfg;
    auto r = engine->run(cfg);
    auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "Trace: ";
    for (size_t i = 0; i < r.execution_trace.size(); ++i) {
        std::cout << r.execution_trace[i];
        if (i + 1 < r.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n\n";

    // State JSON nests each channel under channels.<name>.value.
    auto channels  = r.output.value("channels", neograph::json::object());
    auto summary_v = channels.value("summary", neograph::json::object())
                            .value("value", std::string("(missing)"));
    std::cout << summary_v << "\n";
    std::cout << "Wall clock: " << wall << " ms (all 3 MCP calls concurrent)\n";
    return 0;
}
