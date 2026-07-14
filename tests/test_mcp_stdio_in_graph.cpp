// A stdio MCP tool, used from inside a graph run.
//
// This is the combination nothing covered, and it is a use-after-free.
//
// StdioSession caches the asio wrappers for the subprocess's pipes, and binds
// them to whatever executor is current on the first async call. The member
// comment states the resulting precondition plainly:
//
//     "Callers must ensure the io_context driving rpc_call_async outlives the
//      session (reverse order of declaration in tests and call sites)."
//
// The graph engine cannot honour that. `GraphEngine::run` goes through
// `run_sync`, which stands up an io_context for the duration of one call and
// destroys it on the way out. The session — owned by the MCPClient, and by every
// MCPTool it produced — outlives that io_context, so the cached descriptors are
// left pointing at a destroyed one. ASan, on the client's destructor:
//
//     heap-use-after-free
//       #1 StdioSession::~StdioSession()
//       #7 MCPClient::~MCPClient()
//     freed by thread T0 here:
//       run_sync<RunResult>(...)
//       GraphEngine::run(...)
//
// The existing MCP tests drive the client directly on an io_context they own and
// outlive, which satisfies the precondition and hides this. The existing graph
// tests do not use MCP. Nothing sat in the intersection until a Python user was
// handed `NodeContext(tools=client.get_tools())` and told it was the whole
// integration (#95) — which it is, and which is exactly why the engine has to be
// allowed to own the io_context.

#include <gtest/gtest.h>

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/mcp/client.h>

#include <asio/awaitable.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::filesystem::path fixture_path() {
    std::filesystem::path here(__FILE__);
    return here.parent_path() / "fixtures" / "mcp_stdio_echo.py";
}

const char* python_cmd() {
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

// Stands in for the model: asks for the remote tool.
class ToolCallingNode : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput) override {
        json assistant;
        assistant["role"]       = "assistant";
        assistant["content"]    = "";
        assistant["tool_calls"] = json::array({
            json{{"id", "1"}, {"name", "echo"}, {"arguments", R"({"text":"hi"})"}}
        });
        NodeResult out;
        out.writes.push_back(ChannelWrite{"messages", json::array({assistant})});
        co_return out;
    }
    std::string get_name() const override { return "llm"; }
};

json graph_def() {
    return json{
        {"name", "mcp_stdio_in_graph"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"llm",   {{"type", "mcp_graph_llm"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "llm"}},
            json{{"from", "llm"},       {"to", "tools"}},
            json{{"from", "tools"},     {"to", "__end__"}}
        })}
    };
}

} // namespace

// RED under ASan before the fix: heap-use-after-free at ~MCPClient, the pipe
// descriptor having been destroyed with run_sync's per-call io_context.
//
// Green means the session's asio state no longer depends on an io_context it
// does not own — so a caller may run the graph as many times as it likes, and
// destroy the client whenever it likes.
TEST(MCPStdioInGraph, AToolFromAStdioServerSurvivesAGraphRun) {
    NodeFactory::instance().register_type("mcp_graph_llm",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ToolCallingNode>();
        });

    mcp::MCPClient client({python_cmd(), fixture_path().string()});
    ASSERT_TRUE(client.initialize("neograph-test")) << "MCP handshake failed";

    auto tools = client.get_tools();
    ASSERT_FALSE(tools.empty());

    NodeContext ctx;
    for (auto& t : tools) ctx.tools.push_back(t.get());

    auto engine = GraphEngine::compile(graph_def(), ctx);

    RunConfig cfg;
    cfg.thread_id = "mcp-stdio-graph";
    auto result = engine->run(cfg);

    const auto& messages = result.output["channels"]["messages"]["value"];
    ASSERT_TRUE(messages.is_array());
    bool saw_tool_result = false;
    for (const auto& m : messages) {
        if (m.value("role", "") == "tool") {
            saw_tool_result = true;
            EXPECT_EQ(m.value("content", "").find("error"), std::string::npos)
                << "the MCP call failed: " << m.value("content", "");
        }
    }
    EXPECT_TRUE(saw_tool_result) << "the tool never produced a result";
}

// The same session across two runs. Each run brings its own io_context, so a
// session that latched onto the first one is already wrong by the second — this
// fails even without a sanitizer once the descriptors dangle.
TEST(MCPStdioInGraph, TheSameClientSurvivesASecondRun) {
    NodeFactory::instance().register_type("mcp_graph_llm",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ToolCallingNode>();
        });

    mcp::MCPClient client({python_cmd(), fixture_path().string()});
    ASSERT_TRUE(client.initialize("neograph-test"));

    auto tools = client.get_tools();
    ASSERT_FALSE(tools.empty());

    NodeContext ctx;
    for (auto& t : tools) ctx.tools.push_back(t.get());
    auto engine = GraphEngine::compile(graph_def(), ctx);

    for (int i = 0; i < 2; ++i) {
        RunConfig cfg;
        cfg.thread_id = "mcp-stdio-run-" + std::to_string(i);
        auto result = engine->run(cfg);

        bool saw_tool_result = false;
        for (const auto& m : result.output["channels"]["messages"]["value"]) {
            if (m.value("role", "") == "tool") {
                saw_tool_result = true;
                EXPECT_EQ(m.value("content", "").find("error"), std::string::npos)
                    << "run " << i << ": " << m.value("content", "");
            }
        }
        EXPECT_TRUE(saw_tool_result) << "run " << i << " produced no tool result";
    }
}
