// Regression tests for the signal-based dispatch rewrite (previously the
// BSP engine kept a static predecessor map that deadlocked whenever a
// conditional edge targeted its own source). These graphs would have
// failed to progress under the old all_predecessors_done gating.
//
// They all use only conditional edges — no Command / Send escapes — to
// verify that the signal model *really* dispatches off routed targets
// instead of waiting on static predecessor sets.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A node that bumps a counter, then writes "__route__" to drive the
// conditional edge from itself. The route value is controlled by a
// scripted sequence so each test can decide exactly when to terminate.
class ScriptedRouter : public GraphNode {
public:
    ScriptedRouter(std::string name, std::vector<std::string> script)
        : name_(std::move(name)), script_(std::move(script)) {}

    std::vector<ChannelWrite> execute(const GraphState&) override {
        std::string r = idx_ < script_.size() ? script_[idx_] : "end";
        ++idx_;
        return {ChannelWrite{"__route__", json(r)},
                ChannelWrite{name_ + "_count", json(static_cast<int>(idx_))}};
    }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::vector<std::string> script_;
    size_t idx_ = 0;
};

class Collector : public GraphNode {
public:
    explicit Collector(std::string name) : name_(std::move(name)) {}
    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {ChannelWrite{"hits", json::array({name_})}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
};

void register_nodes(std::vector<std::string> loop_script = {"loop", "loop", "end"}) {
    NodeFactory::instance().register_type("loop_router",
        [loop_script](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<ScriptedRouter>(name, loop_script);
        });
    NodeFactory::instance().register_type("collector",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<Collector>(name);
        });
}

} // namespace

// -------------------------------------------------------------------------
// 1. Conditional self-loop: a node whose true-branch points back at itself.
// Under the old static predecessor map, predecessors[looper] = {looper}
// and the first attempt to signal `looper` from itself would fail because
// looper wasn't yet in `completed`. Signal dispatch makes this trivial.
// -------------------------------------------------------------------------
TEST(SignalDispatch, ConditionalSelfLoopTerminates) {
    register_nodes({"loop", "loop", "loop", "end"});

    json graph = {
        {"name", "self_loop"},
        {"channels", {
            {"__route__",  {{"reducer", "overwrite"}}},
            {"looper_count", {{"reducer", "overwrite"}}},
            {"hits", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"looper", {{"type", "loop_router"}}},
            {"done",   {{"type", "collector"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "looper"}},
            {{"from", "looper"}, {"type", "conditional"},
             {"condition", "route_channel"},
             {"routes", {{"loop", "looper"}, {"end", "done"}}}},
            {{"from", "done"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 20;
    auto result = engine->run(cfg);

    // 4 x looper (loop, loop, loop, end) + 1 x done
    EXPECT_EQ(result.execution_trace.size(), 5u);
    EXPECT_EQ(result.execution_trace.front(), "looper");
    EXPECT_EQ(result.execution_trace.back(), "done");
    EXPECT_EQ(result.output["channels"]["looper_count"]["value"].get<int>(), 4);
}

// -------------------------------------------------------------------------
// 2. Mutual recursion through conditional edges: A <-> B with neither
// acting as its own predecessor. Old engine sometimes got away with this
// because A and B each had the other as their sole predecessor, but any
// extra branching — e.g. routing both 'loop' AND 'end' candidates from
// the same node — exercises the set-based dispatch path.
// -------------------------------------------------------------------------
TEST(SignalDispatch, MutualRecursionBetweenConditionals) {
    NodeFactory::instance().register_type("ping",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<ScriptedRouter>(name,
                std::vector<std::string>{"pong", "pong", "end"});
        });
    NodeFactory::instance().register_type("pong",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<ScriptedRouter>(name,
                std::vector<std::string>{"ping", "ping", "end"});
        });
    NodeFactory::instance().register_type("collector",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<Collector>(name);
        });

    json graph = {
        {"name", "ping_pong"},
        {"channels", {
            {"__route__", {{"reducer", "overwrite"}}},
            {"ping_count", {{"reducer", "overwrite"}}},
            {"pong_count", {{"reducer", "overwrite"}}},
            {"hits", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"ping", {{"type", "ping"}}},
            {"pong", {{"type", "pong"}}},
            {"done", {{"type", "collector"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "ping"}},
            {{"from", "ping"}, {"type", "conditional"},
             {"condition", "route_channel"},
             {"routes", {{"pong", "pong"}, {"end", "done"}}}},
            {{"from", "pong"}, {"type", "conditional"},
             {"condition", "route_channel"},
             {"routes", {{"ping", "ping"}, {"end", "done"}}}},
            {{"from", "done"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 30;
    auto result = engine->run(cfg);

    // ping -> pong -> ping -> pong -> ping (ends) -> done
    // Actually ping runs: call 1 emits "pong"; after first pong we come
    // back to ping, call 2 emits "pong"; pong again, call 3 emits "end".
    // So ping runs 3 times, pong 2 times, done once.
    EXPECT_EQ(result.output["channels"]["ping_count"]["value"].get<int>(), 3);
    EXPECT_EQ(result.output["channels"]["pong_count"]["value"].get<int>(), 2);
    EXPECT_EQ(result.execution_trace.back(), "done");
}

// -------------------------------------------------------------------------
// 3. Parallel fan-in: two upstream nodes running in the same super-step
// both signal the same downstream node. Under signal dispatch the
// downstream runs exactly once (set dedup), not twice.
// -------------------------------------------------------------------------
TEST(SignalDispatch, ParallelFanInRunsOnce) {
    static std::atomic<int> join_hits{0};
    join_hits = 0;

    class LeafNode : public GraphNode {
    public:
        explicit LeafNode(std::string n) : n_(std::move(n)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            return {ChannelWrite{"leaf_" + n_, json(true)}};
        }
        std::string name() const override { return n_; }
    private:
        std::string n_;
    };
    class JoinNode : public GraphNode {
    public:
        std::vector<ChannelWrite> execute(const GraphState&) override {
            join_hits.fetch_add(1, std::memory_order_relaxed);
            return {ChannelWrite{"joined", json(true)}};
        }
        std::string name() const override { return "join"; }
    };

    NodeFactory::instance().register_type("leaf",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<LeafNode>(name);
        });
    NodeFactory::instance().register_type("join",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<JoinNode>();
        });

    json graph = {
        {"name", "fan_in"},
        {"channels", {
            {"leaf_a", {{"reducer", "overwrite"}}},
            {"leaf_b", {{"reducer", "overwrite"}}},
            {"joined", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",    {{"type", "leaf"}}},
            {"b",    {{"type", "leaf"}}},
            {"join", {{"type", "join"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "b"}},
            {{"from", "a"}, {"to", "join"}},
            {{"from", "b"}, {"to", "join"}},
            {{"from", "join"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    EXPECT_EQ(join_hits.load(), 1) << "join should execute exactly once";
    EXPECT_TRUE(result.output["channels"]["joined"]["value"].get<bool>());
}
