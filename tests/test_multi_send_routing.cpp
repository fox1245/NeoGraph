// Regression coverage for Send fan-in routing: a Send-spawned task's
// `Command.goto_node` and the Send target's default outgoing edge must
// both flow into the next super-step's routing decision after fan-in.
//
// Bug surface (closed by run_sends_async returning per-task StepRouting):
//
//   * Each task returned `Command(goto=X)`, run_sends_async dropped it.
//   * Each task's source node had a default outgoing edge `target → Y`,
//     run_sends_async dropped that too.
//
// Both paths exit the engine at the source node's own routing —
// regardless of what the spawned tasks asked for. These tests pin the
// expected behaviour: per-task `command_goto` preempts (LangGraph
// parity), and default outgoing edges from the Send target node are
// honoured when no `command_goto` was issued.
//
// Sibling-path miss number 12 closed by this file (see
// project_engine_audit memory for the prior eleven).

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <mutex>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Planner emitting `fanout` Sends to "worker" with distinct task_idx.
// No own writes / no Command — the default edge from "planner" is what
// drives the source-node routing in these tests.
class RoutingPlannerNode : public GraphNode {
public:
    explicit RoutingPlannerNode(int fanout) : fanout_(fanout) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "worker";
            s.input = {{"task_idx", i}};
            out.sends.push_back(std::move(s));
        }
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
private:
    int fanout_;
};

// Worker that returns `Command(goto=goto_target)` plus a baseline write
// so we can confirm both the writes and the routing transition fired.
class RoutingWorkerWithGoto : public GraphNode {
public:
    explicit RoutingWorkerWithGoto(std::string goto_target)
        : goto_target_(std::move(goto_target)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        out.writes.push_back(ChannelWrite{"results", json::array({idx})});
        Command cmd;
        cmd.goto_node = goto_target_;
        out.command = std::move(cmd);
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
private:
    std::string goto_target_;
};

// Worker that has *no* Command — the default outgoing edge from
// "worker" is what should drive the post-fan-in routing.
class RoutingWorkerNoCommand : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -1;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({idx})});
        co_return out;
    }
    std::string get_name() const override { return "worker"; }
};

// Terminal marker node: writes a sentinel into the "finalized" channel
// so the test can verify the engine actually transitioned here.
class FinalizeMarkerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"finalized", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "finalize"; }
};

// Bridge node that does nothing — used to give "planner" a non-END
// successor so its default edge doesn't immediately terminate the
// graph in the default-edge test.
class BridgeNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"bridged", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "bridge"; }
};

void register_finalize_and_bridge_once() {
    static std::once_flag f;
    std::call_once(f, []{
        NodeFactory::instance().register_type("__rt_finalize",
            [](const std::string&, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<FinalizeMarkerNode>();
            });
        NodeFactory::instance().register_type("__rt_bridge",
            [](const std::string&, const json&, const NodeContext&)
                -> std::unique_ptr<GraphNode> {
                return std::make_unique<BridgeNode>();
            });
    });
}

} // namespace

// ─────────────────────────────────────────────────────────────────────
// Per-task Command(goto=X) — this is the case that broke example 17:
// every researcher emitted Command(goto="synthesize") but the engine
// silently dropped them, so synthesize never ran and the graph ended
// with no final report.
// ─────────────────────────────────────────────────────────────────────
TEST(MultiSendRouting, MultiSendPerTaskCommandGotoFollows) {
    register_finalize_and_bridge_once();
    NodeFactory::instance().register_type("__rt_planner_goto3",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingPlannerNode>(3);
        });
    NodeFactory::instance().register_type("__rt_worker_goto",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingWorkerWithGoto>("finalize");
        });

    json def = {
        {"name", "multi_send_per_task_goto"},
        {"channels", {
            {"task_idx",  {{"reducer", "overwrite"}}},
            {"results",   {{"reducer", "append"}}},
            {"finalized", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",  {{"type", "__rt_planner_goto3"}}},
            {"worker",   {{"type", "__rt_worker_goto"}}},
            {"finalize", {{"type", "__rt_finalize"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "finalize"},  {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig rc; rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    ASSERT_TRUE(result.output["channels"].contains("results"));
    EXPECT_EQ(3u, result.output["channels"]["results"]["value"].size());

    // The bug: workers all said goto=finalize but engine never went
    // there. The fix: every spawned task's goto_node merges into the
    // next super-step's routing, so finalize must have run.
    ASSERT_TRUE(result.output["channels"].contains("finalized"))
        << "finalize node never ran — Send-spawned Command(goto) dropped";
    EXPECT_TRUE(result.output["channels"]["finalized"]["value"].get<bool>());
}

// Single-Send parity — same expectation on the sequential single-send
// path, which historically worked but is worth pinning so the two
// branches can never diverge again.
TEST(MultiSendRouting, SingleSendCommandGotoFollows) {
    register_finalize_and_bridge_once();
    NodeFactory::instance().register_type("__rt_planner_goto1",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingPlannerNode>(1);
        });
    NodeFactory::instance().register_type("__rt_worker_goto1",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingWorkerWithGoto>("finalize");
        });

    json def = {
        {"name", "single_send_goto"},
        {"channels", {
            {"task_idx",  {{"reducer", "overwrite"}}},
            {"results",   {{"reducer", "append"}}},
            {"finalized", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",  {{"type", "__rt_planner_goto1"}}},
            {"worker",   {{"type", "__rt_worker_goto1"}}},
            {"finalize", {{"type", "__rt_finalize"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "finalize"},  {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig rc; rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    ASSERT_TRUE(result.output["channels"].contains("finalized"))
        << "finalize node never ran on single-Send Command(goto)";
    EXPECT_TRUE(result.output["channels"]["finalized"]["value"].get<bool>());
}

// ─────────────────────────────────────────────────────────────────────
// Default outgoing edge from the Send target — this is the case where
// the user defines `worker → finalize` in the graph and expects it to
// fire after every spawned worker finishes, no Command needed.
//
// Topology forces `bridge` to be planner's default successor so the
// run doesn't immediately hit __end__ from planner's side; with the
// fix, plan_next_step also sees worker's default edge to `finalize`,
// so both `bridge` and `finalize` are queued in the next super-step.
// ─────────────────────────────────────────────────────────────────────
TEST(MultiSendRouting, MultiSendDefaultOutgoingEdgeFollows) {
    register_finalize_and_bridge_once();
    NodeFactory::instance().register_type("__rt_planner_default3",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingPlannerNode>(3);
        });
    NodeFactory::instance().register_type("__rt_worker_nocmd",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<RoutingWorkerNoCommand>();
        });

    json def = {
        {"name", "multi_send_default_edge"},
        {"channels", {
            {"task_idx",  {{"reducer", "overwrite"}}},
            {"results",   {{"reducer", "append"}}},
            {"bridged",   {{"reducer", "overwrite"}}},
            {"finalized", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",  {{"type", "__rt_planner_default3"}}},
            {"worker",   {{"type", "__rt_worker_nocmd"}}},
            {"bridge",   {{"type", "__rt_bridge"}}},
            {"finalize", {{"type", "__rt_finalize"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "bridge"}},
            {{"from", "worker"},    {"to", "finalize"}},
            {{"from", "bridge"},    {"to", "__end__"}},
            {{"from", "finalize"},  {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    RunConfig rc; rc.max_steps = 10;
    auto result = engine->run_stream(rc, {});

    // bridge always ran (it was on planner's regular routing path).
    ASSERT_TRUE(result.output["channels"].contains("bridged"));
    EXPECT_TRUE(result.output["channels"]["bridged"]["value"].get<bool>());

    // finalize is the bug. Without the fix, the engine would only see
    // {planner} in next-step routing inputs and never resolve worker's
    // default edge → finalize never runs.
    ASSERT_TRUE(result.output["channels"].contains("finalized"))
        << "finalize node never ran — Send-spawned default edge dropped";
    EXPECT_TRUE(result.output["channels"]["finalized"]["value"].get<bool>());
}
