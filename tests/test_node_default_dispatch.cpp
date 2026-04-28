// Tests for the GraphNode default-dispatch behaviour and the
// recursion guard installed in src/core/graph_node.cpp.
//
// Background: GraphNode has four overridable hooks — execute,
// execute_async, execute_full, execute_full_async. The defaults
// route through each other so a subclass can override any one and
// the others come for free. Pre-fix (before commit-introducing-this-test),
// overriding only `execute_full` triggered infinite recursion through
// asio's awaitable_thread machinery (~90,000 stack frames). Now the
// async default routes through sync execute_full directly, AND a
// recursion guard catches the "user overrode nothing" case with a
// clear runtime_error instead of a stack overflow.
//
// See also: feedback_async_bridge_required (memory note).

#include <gtest/gtest.h>

#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Override only execute_full(sync) — emits Send. This was the broken
// case yesterday; today it must work without stack overflow.
class SendOnlyNode : public GraphNode {
public:
    std::string get_name() const override { return "send_only"; }
    NodeResult execute_full(const GraphState&) override {
        NodeResult r;
        r.sends.emplace_back(Send{"sink", json{{"v", 42}}});
        return r;
    }
};

// Sink that records that it ran (to prove the Send was dispatched).
class SinkNode : public GraphNode {
public:
    std::string get_name() const override { return "sink"; }
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int v = state.get("v").get<int>();
        return {ChannelWrite{"out", json::array({v})}};
    }
};

// Override absolutely nothing (only get_name). This used to stack-
// overflow; should now throw a clear runtime_error from the guard.
class MisbuiltNode : public GraphNode {
public:
    std::string get_name() const override { return "misbuilt"; }
};

GraphEngine* compile_two_node(const char* first_type) {
    auto& factory = NodeFactory::instance();
    factory.register_type("send_only",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SendOnlyNode>();
        });
    factory.register_type("sink",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SinkNode>();
        });
    factory.register_type("misbuilt",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<MisbuiltNode>();
        });

    json def = {
        {"name", "test"},
        {"channels", {{"v", {{"reducer", "overwrite"}}},
                      {"out", {{"reducer", "append"}}}}},
        {"nodes", {{"src", {{"type", first_type}}},
                   {"sink", {{"type", "sink"}}}}},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "src"}},
            // src reaches sink only via Send (proves the Send was
            // honoured). No regular edge from src → sink.
            json{{"from", "sink"},      {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    return GraphEngine::compile(def, ctx).release();
}

} // namespace

TEST(NodeDefaultDispatch, SyncExecuteFullEmittingSendNoLongerStackOverflows) {
    // Before the fix, this would stack-overflow inside asio's
    // awaitable_thread before the planner ever ran. Now the async
    // default routes through sync execute_full directly, so the
    // Sends survive the dispatch.
    std::unique_ptr<GraphEngine> engine{compile_two_node("send_only")};

    RunConfig cfg;
    cfg.thread_id = "t";
    auto r = engine->run(cfg);

    // Sink received the Send-injected value via the v channel.
    auto out = r.output.value("channels", json::object())
                       .value("out", json::object())
                       .value("value", json::array());
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].get<int>(), 42);

    // Trace shows the Send target ran. Engine records Send targets
    // as "target_node[send]" to distinguish them from regular-edge
    // dispatches.
    bool sink_ran = false;
    for (const auto& step : r.execution_trace) {
        if (step == "sink" || step == "sink[send]") {
            sink_ran = true;
            break;
        }
    }
    EXPECT_TRUE(sink_ran)
        << "Send target should appear in trace. Got: "
        << [&]{ std::string s; for (auto& t : r.execution_trace) { s += t + ","; } return s; }();
}

TEST(NodeDefaultDispatch, NoOverrideFailsWithClearMessage) {
    // Before the fix, this would stack-overflow ~90,000 frames deep.
    // Now the recursion guard fires with a clear runtime_error pointing
    // at the missing override.
    std::unique_ptr<GraphEngine> engine{compile_two_node("misbuilt")};

    RunConfig cfg;
    cfg.thread_id = "t";

    EXPECT_THROW({
        try {
            engine->run(cfg);
        } catch (const std::runtime_error& e) {
            std::string msg = e.what();
            // Must mention the offending node and the four override names
            // so the user knows where to fix it.
            EXPECT_NE(msg.find("misbuilt"), std::string::npos)
                << "error should name the offending node";
            EXPECT_NE(msg.find("execute"),  std::string::npos)
                << "error should mention `execute`";
            EXPECT_NE(msg.find("override"), std::string::npos)
                << "error should mention `override`";
            throw;
        }
    }, std::runtime_error);
}
