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
    std::string get_name() const override { return name_; }

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
    std::string get_name() const override { return name_; }
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
// 3. Command.goto_node overrides edge routing: `a` has a regular edge to
// `wrong` but returns Command{goto="right"}. The engine must dispatch
// to `right` — proving the Command path is still wired through after
// the predecessor-gate removal. This is end-to-end coverage for a code
// path that was only tested as a struct until now.
// -------------------------------------------------------------------------
TEST(SignalDispatch, CommandGotoOverridesEdgeRouting) {
    class GotoNode : public GraphNode {
    public:
        explicit GotoNode(std::string target) : target_(std::move(target)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            return {};
        }
        NodeResult execute_full(const GraphState&) override {
            NodeResult nr;
            Command c;
            c.goto_node = target_;
            nr.command = c;
            return nr;
        }
        asio::awaitable<NodeResult>
        execute_full_async(const GraphState& state) override {
            co_return execute_full(state);
        }
        std::string get_name() const override { return "a"; }
    private:
        std::string target_;
    };

    class SinkNode : public GraphNode {
    public:
        explicit SinkNode(std::string n) : n_(std::move(n)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            return {ChannelWrite{"hit_" + n_, json(true)}};
        }
        std::string get_name() const override { return n_; }
    private:
        std::string n_;
    };

    NodeFactory::instance().register_type("a_goto_right",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<GotoNode>("right");
        });
    NodeFactory::instance().register_type("sink",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<SinkNode>(name);
        });

    json graph = {
        {"name", "cmd_override"},
        {"channels", {
            {"hit_wrong", {{"reducer", "overwrite"}}},
            {"hit_right", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",     {{"type", "a_goto_right"}}},
            {"wrong", {{"type", "sink"}}},
            {"right", {{"type", "sink"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            // Static edge says a -> wrong, but Command overrides.
            {{"from", "a"},     {"to", "wrong"}},
            {{"from", "wrong"}, {"to", "__end__"}},
            {{"from", "right"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    auto channels = result.output["channels"];
    EXPECT_TRUE(channels["hit_right"]["value"].get<bool>())
        << "Command.goto_node must route to 'right'";
    EXPECT_FALSE(channels["hit_wrong"].contains("value") &&
                 channels["hit_wrong"]["value"].is_boolean() &&
                 channels["hit_wrong"]["value"].get<bool>())
        << "'wrong' must not run — Command overrode the static edge";

    // Also confirm the trace shape: a then right, no wrong.
    ASSERT_EQ(result.execution_trace.size(), 2u);
    EXPECT_EQ(result.execution_trace[0], "a");
    EXPECT_EQ(result.execution_trace[1], "right");
}

// -------------------------------------------------------------------------
// 4. Parallel fan-in: two upstream nodes running in the same super-step
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
        std::string get_name() const override { return n_; }
    private:
        std::string n_;
    };
    class JoinNode : public GraphNode {
    public:
        std::vector<ChannelWrite> execute(const GraphState&) override {
            join_hits.fetch_add(1, std::memory_order_relaxed);
            return {ChannelWrite{"joined", json(true)}};
        }
        std::string get_name() const override { return "join"; }
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

// -------------------------------------------------------------------------
// 5. Asymmetric serial fan-in: two paths of DIFFERENT lengths converge on
// the same node. Under the old predecessor-gated model, `join` would
// have waited for both upstream paths to complete and fired exactly
// once. Under signal dispatch there is no cross-super-step join barrier
// — `join` fires each super-step in which some upstream path routes to
// it, which for asymmetric paths means MULTIPLE executions.
//
// This is the documented contract from the signal-dispatch refactor:
// "serial fan-in is explicitly the user's responsibility (they can use
// a reducer or a barrier node)". The test locks in that semantics so
// any future change back to implicit join barriers is caught.
//
// Graph shape (edge lengths in parentheses):
//
//     __start__ ──(1)──→ a ──(1)────────────→ join ──→ finish ──→ __end__
//     __start__ ──(1)──→ s1 ──(1)→ s2 ──(1)─→ join
//
// Super-step schedule under signal dispatch:
//   step 0: {a, s1}     — both fired from __start__
//   step 1: {join, s2}  — a→join and s1→s2 in parallel; join runs (hit #1)
//   step 2: {finish, join} — join→finish and s2→join; join runs again (hit #2)
//   step 3: terminate   — finish→__end__ triggers hit_end
// -------------------------------------------------------------------------
TEST(SignalDispatch, AsymmetricSerialFanInFiresJoinPerPath) {
    static std::atomic<int> join_hits{0};
    static std::atomic<int> finish_hits{0};
    join_hits = 0;
    finish_hits = 0;

    class PassThrough : public GraphNode {
    public:
        explicit PassThrough(std::string n) : n_(std::move(n)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            return {ChannelWrite{n_ + "_ran", json(true)}};
        }
        std::string get_name() const override { return n_; }
    private:
        std::string n_;
    };
    class JoinCounter : public GraphNode {
    public:
        std::vector<ChannelWrite> execute(const GraphState&) override {
            join_hits.fetch_add(1, std::memory_order_relaxed);
            return {ChannelWrite{"join_ran", json(true)}};
        }
        std::string get_name() const override { return "join"; }
    };
    class FinishCounter : public GraphNode {
    public:
        std::vector<ChannelWrite> execute(const GraphState&) override {
            finish_hits.fetch_add(1, std::memory_order_relaxed);
            return {ChannelWrite{"finish_ran", json(true)}};
        }
        std::string get_name() const override { return "finish"; }
    };

    NodeFactory::instance().register_type("pass",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<PassThrough>(name);
        });
    NodeFactory::instance().register_type("join_ctr",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<JoinCounter>();
        });
    NodeFactory::instance().register_type("finish_ctr",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<FinishCounter>();
        });

    json graph = {
        {"name", "asym_fan_in"},
        {"channels", {
            {"a_ran",      {{"reducer", "overwrite"}}},
            {"s1_ran",     {{"reducer", "overwrite"}}},
            {"s2_ran",     {{"reducer", "overwrite"}}},
            {"join_ran",   {{"reducer", "overwrite"}}},
            {"finish_ran", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",      {{"type", "pass"}}},
            {"s1",     {{"type", "pass"}}},
            {"s2",     {{"type", "pass"}}},
            {"join",   {{"type", "join_ctr"}}},
            {"finish", {{"type", "finish_ctr"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "s1"}},
            {{"from", "a"},  {"to", "join"}},
            {{"from", "s1"}, {"to", "s2"}},
            {{"from", "s2"}, {"to", "join"}},
            {{"from", "join"},   {"to", "finish"}},
            {{"from", "finish"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    EXPECT_EQ(join_hits.load(), 2)
        << "join must fire TWICE under signal dispatch — once when path "
           "`a` signals it (step 1) and again when path `s1→s2` signals "
           "it (step 2). If this ever drops back to 1 the engine has "
           "silently reintroduced an implicit AND-join barrier, which "
           "would deadlock conditional self-loops.";
}

// -------------------------------------------------------------------------
// 6. Explicit barrier opt-in: the same asymmetric-fan-in graph as #5,
// but with `join` declared as a barrier node waiting for both `a` and
// `s2` to signal. Now join must fire exactly ONCE — the barrier
// accumulates the `a` signal in step 1, defers, then fires in step 2
// when `s2` also signals.
//
// This is the user-facing fix for the footgun locked in by test 5:
// serial fan-in semantics are now declarable, not "user writes a dedup
// reducer and hopes to remember why".
// -------------------------------------------------------------------------
TEST(SignalDispatch, DeclaredBarrierCoalescesAsymmetricFanIn) {
    static std::atomic<int> b_join_hits{0};
    b_join_hits = 0;

    class PassThrough : public GraphNode {
    public:
        explicit PassThrough(std::string n) : n_(std::move(n)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            return {ChannelWrite{n_ + "_ran", json(true)}};
        }
        std::string get_name() const override { return n_; }
    private:
        std::string n_;
    };
    class BarrierJoinCounter : public GraphNode {
    public:
        std::vector<ChannelWrite> execute(const GraphState&) override {
            b_join_hits.fetch_add(1, std::memory_order_relaxed);
            return {ChannelWrite{"bjoin_ran", json(true)}};
        }
        std::string get_name() const override { return "join"; }
    };
    NodeFactory::instance().register_type("bpass",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<PassThrough>(name);
        });
    NodeFactory::instance().register_type("bjoin_ctr",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<BarrierJoinCounter>();
        });

    json graph = {
        {"name", "asym_barrier"},
        {"channels", {
            {"a_ran",    {{"reducer", "overwrite"}}},
            {"s1_ran",   {{"reducer", "overwrite"}}},
            {"s2_ran",   {{"reducer", "overwrite"}}},
            {"bjoin_ran",{{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",    {{"type", "bpass"}}},
            {"s1",   {{"type", "bpass"}}},
            {"s2",   {{"type", "bpass"}}},
            {"join", {
                {"type", "bjoin_ctr"},
                {"barrier", {{"wait_for", json::array({"a", "s2"})}}}
            }}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "s1"}},
            {{"from", "a"},  {"to", "join"}},
            {{"from", "s1"}, {"to", "s2"}},
            {{"from", "s2"}, {"to", "join"}},
            {{"from", "join"}, {"to", "__end__"}}
        })}
    };

    auto engine = GraphEngine::compile(graph, NodeContext{});
    RunConfig cfg;
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    EXPECT_EQ(b_join_hits.load(), 1)
        << "With an explicit barrier declaration, asymmetric serial "
           "fan-in must coalesce to a single join execution. Firing "
           "twice means the scheduler stopped honoring the barrier.";
    EXPECT_TRUE(result.output["channels"]["bjoin_ran"]["value"].get<bool>());
}
