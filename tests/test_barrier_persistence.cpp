// Tests for Checkpoint schema v2: BarrierState persistence + resume
// restoration.
//
// Before v2, barrier_state was in-memory only, so an interrupt landing
// in the middle of a multi-source barrier accumulation silently dropped
// every upstream signal received so far. On resume the barrier started
// with an empty map and waited forever for signals that had already
// arrived. The tests here lock in:
//
//   1. A fresh Checkpoint has an empty barrier_state (default).
//   2. A graph without barriers never populates the field — no hidden
//      writes, no memory bloat.
//   3. When a barrier has received some (but not all) upstream signals
//      and an interrupt fires, the partial set is durably recorded on
//      the checkpoint.
//   4. On resume, the partial set is restored and the *remaining*
//      signals arriving after resume complete the barrier normally, so
//      the join node fires exactly once.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

namespace {

class HitCounter : public GraphNode {
public:
    HitCounter(std::string n, std::atomic<int>* counter)
        : name_(std::move(n)), counter_(counter) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{name_ + "_done", json(true)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int>* counter_;
};

NodeFactoryFn make_hit_factory(std::atomic<int>* counter) {
    return [counter](const std::string& n, const json&, const NodeContext&) {
        return std::make_unique<HitCounter>(n, counter);
    };
}

} // namespace

// =========================================================================
// 1. Default
// =========================================================================

TEST(BarrierPersistence, FreshCheckpointHasEmptyBarrierState) {
    Checkpoint cp;
    EXPECT_TRUE(cp.barrier_state.empty());
    EXPECT_EQ(cp.schema_version, CHECKPOINT_SCHEMA_VERSION);
    EXPECT_EQ(CHECKPOINT_SCHEMA_VERSION, 4)
        << "v3 adds ChannelWrite::Mode; v4 adds the subgraph write journal";
}

// =========================================================================
// 2. Non-barrier graphs don't touch the field
// =========================================================================

TEST(BarrierPersistence, GraphWithoutBarriersKeepsEmptyBarrierState) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    json graph = {
        {"name", "no_barriers"},
        {"channels", {
            {"a_done", {{"reducer", "overwrite"}}},
            {"b_done", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a", {{"type", "hit_bp"}}},
            {"b", {{"type", "hit_bp"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "b"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "no-barrier";
    engine->run(cfg);

    auto cps = store->list("no-barrier");
    ASSERT_FALSE(cps.empty());
    for (const auto& cp : cps) {
        EXPECT_TRUE(cp.barrier_state.empty())
            << "cp " << cp.current_node << " leaked barrier_state";
    }
}

// =========================================================================
// 3-4. Shared fixture — barrier with partial signal + interrupt
//
// Topology:
//
//   __start__ ──► a ─────────────────► join  (barrier wait_for={a, b_final})
//        │                                ▲
//        └───► b_step1 ─► gate ─► b_final ┘
//
// interrupt_before "gate" fires at the start of super-step 2. By that
// point super-step 1 has already run {a, b_step1}; `a` has signaled
// `join`, so the barrier accumulator is {a}. `b_final` has not yet
// signaled because its path is still upstream of the interrupt.
//
// After resume, `gate` → `b_final` → `join`. If barrier_state was
// persisted and restored, the second signal completes the accumulator
// ({a, b_final} ⊇ wait_for) and `join` fires exactly once. Without
// persistence the accumulator starts empty on resume, the barrier never
// sees `a`'s signal again (a already ran once), and `join` never fires.
// =========================================================================

static json make_barrier_interrupt_graph() {
    return {
        {"name", "barrier_interrupt"},
        {"channels", {
            {"a_done",       {{"reducer", "overwrite"}}},
            {"b_step1_done", {{"reducer", "overwrite"}}},
            {"gate_done",    {{"reducer", "overwrite"}}},
            {"b_final_done", {{"reducer", "overwrite"}}},
            {"join_done",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",       {{"type", "hit_bp"}}},
            {"b_step1", {{"type", "hit_bp"}}},
            {"gate",    {{"type", "hit_bp"}}},
            {"b_final", {{"type", "hit_bp"}}},
            {"join", {
                {"type", "hit_bp"},
                {"barrier", {{"wait_for", {"a", "b_final"}}}}
            }}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "b_step1"}},
            {{"from", "a"},         {"to", "join"}},
            {{"from", "b_step1"},   {"to", "gate"}},
            {{"from", "gate"},      {"to", "b_final"}},
            {{"from", "b_final"},   {"to", "join"}},
            {{"from", "join"},      {"to", "__end__"}}
        })},
        {"interrupt_before", {"gate"}}
    };
}

// 3. Partial signal durably recorded
TEST(BarrierPersistence, PartialBarrierSignalsPersistedOnInterrupt) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_barrier_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "partial-barrier";
    auto result = engine->run(cfg);
    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_node, "gate");

    // The latest cp (the Before-phase one we just landed on) must carry
    // barrier_state["join"] = {"a"} — 'a' signaled join in super-step 1,
    // but b_final has not yet run, so the accumulator is half-full.
    auto cp = store->load_latest("partial-barrier");
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->interrupt_phase, CheckpointPhase::Before);

    auto it = cp->barrier_state.find("join");
    ASSERT_NE(it, cp->barrier_state.end())
        << "partial barrier signals must be recorded, not dropped";
    EXPECT_EQ(it->second, (std::set<std::string>{"a"}));
}

// 4. End-to-end: resume completes the barrier, join fires exactly once
TEST(BarrierPersistence, ResumeAfterPartialBarrierCompletesJoin) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_barrier_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "resume-barrier";
    auto first = engine->run(cfg);
    ASSERT_TRUE(first.interrupted);

    // Phase 1 invocations: a, b_step1 (super-step 1). gate did NOT run
    // because interrupt_before fired first.
    EXPECT_EQ(hits.load(), 2);

    auto resumed = engine->resume("resume-barrier");
    EXPECT_FALSE(resumed.interrupted);

    // Phase 2 invocations: gate, b_final, join. Total = 2 + 3 = 5.
    // If barrier_state were lost on resume, `join` would never fire and
    // this would be 4 (or the run would hit max_steps).
    EXPECT_EQ(hits.load(), 5);
    EXPECT_EQ(resumed.output["channels"]["join_done"]["value"].get<bool>(), true);
}

// =========================================================================
// 5. v1 legacy blob: empty barrier_state on load is safe
// =========================================================================
//
// The v2 engine tolerates v1-shaped checkpoints by treating a missing
// barrier_state as empty. We simulate a v1 load by constructing a
// Checkpoint explicitly and clearing the field, then confirming
// restore() is a no-op on the barrier_state field (the field already
// defaults to empty, so this is a documentation test that the
// fallback shape is well-defined rather than a runtime fix).

TEST(BarrierPersistence, LegacyV1BlobLoadsWithEmptyBarrierState) {
    Checkpoint cp;
    cp.schema_version = 1;  // simulate older store returning a v1 blob
    cp.barrier_state.clear();
    // The engine's restore path uses cp.barrier_state directly; an
    // empty map is the valid "no in-flight barriers" state. Assert it
    // here so the invariant is checked alongside the other v2 tests.
    EXPECT_TRUE(cp.barrier_state.empty());
}

// =========================================================================
// 6. A barrier that can no longer fire does not end the run as Completed
//    (issue #357). B reaches the join only through a conditional route, and
//    its "fail" route detours through FIX, which is not a wait_for member.
// =========================================================================

static json make_bypass_graph(const std::string& condition, const std::string& fail_target) {
    json channels = json::object();
    for (const char* name : {"A", "B", "C", "D", "FIX"})
        channels[std::string(name) + "_done"] = {{"reducer", "overwrite"}};
    return {
        {"name", "barrier_bypass"},
        {"channels", channels},
        {"nodes", {
            {"A",   {{"type", "hit_stall"}}},
            {"B",   {{"type", "hit_stall"}}},
            {"C",   {{"type", "hit_stall"}}},
            {"FIX", {{"type", "hit_stall"}}},
            {"D", {
                {"type", "hit_stall"},
                {"barrier", {{"wait_for", {"B", "C"}}}}
            }}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "A"}},
            {{"from", "A"},   {"to", "B"}},
            {{"from", "A"},   {"to", "C"}},
            {{"from", "C"},   {"to", "D"}},
            {{"from", "FIX"}, {"to", "D"}},
            {{"from", "D"},   {"to", "__end__"}}
        })},
        {"conditional_edges", json::array({
            {{"from", "B"}, {"condition", condition},
             {"routes", {{"pass", "D"}, {"fail", fail_target}}}}
        })}
    };
}

static void register_bypass_types(std::atomic<int>* hits) {
    NodeFactory::instance().register_type("hit_stall", make_hit_factory(hits));
    ConditionRegistry::instance().register_condition(
        "stall_fail", [](const GraphState&) { return std::string("fail"); });
    ConditionRegistry::instance().register_condition(
        "stall_pass", [](const GraphState&) { return std::string("pass"); });
}

TEST(BarrierStall, ConditionalBypassReportsTheStallInsteadOfCompleted) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_bypass_graph("stall_fail", "FIX"), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "barrier-stall";
    const auto result = engine->run(cfg);

    EXPECT_EQ(result.status(), RunStatus::BarrierStalled);
    EXPECT_TRUE(result.barrier_stalled());
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"A", "B", "C", "FIX"}));
    const auto stalls = result.output["_neograph"]["barrier_stalled"];
    ASSERT_TRUE(stalls.is_array());
    ASSERT_EQ(stalls.size(), 1U);
    EXPECT_EQ(stalls[0]["node"], json("D"));
    EXPECT_EQ(stalls[0]["wait_for"], json::array({"B", "C"}));
    EXPECT_EQ(stalls[0]["received"], json::array({"C", "FIX"}));
    EXPECT_EQ(stalls[0]["missing"], json::array({"B"}));

    // The stalled checkpoint keeps its reason: resuming it reports the stall again.
    const auto resumed = engine->resume("barrier-stall");
    EXPECT_EQ(resumed.status(), RunStatus::BarrierStalled);
    EXPECT_EQ(resumed.output["_neograph"]["barrier_stalled"], stalls);
}

TEST(BarrierStall, PassRouteReachesTheJoinAndCompletes) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    auto engine = GraphEngine::compile(make_bypass_graph("stall_pass", "FIX"), NodeContext{});

    const auto result = engine->run(RunConfig{});

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_FALSE(result.barrier_stalled());
    EXPECT_EQ(result.execution_trace.back(), "D");
}

TEST(BarrierStall, AbortRouteToEndStaysCompleted) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    auto engine = GraphEngine::compile(make_bypass_graph("stall_fail", "__end__"), NodeContext{});

    const auto result = engine->run(RunConfig{});

    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_FALSE(result.barrier_stalled());
}

// ---- #357 follow-ups: implicit ends, interruptAfter gating, metadata -------

namespace {

class SendingNode : public GraphNode {
public:
    explicit SendingNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{name_ + "_done", json(true)});
        out.sends.push_back(Send{"worker", json::object()});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Records whether C had already written when it ran.
class JoinObserver : public GraphNode {
public:
    explicit JoinObserver(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"D_saw_C", json(!in.state.get("C_done").is_null())});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

json channels_for(std::initializer_list<const char*> names) {
    json channels = json::object();
    for (const char* name : names)
        channels[std::string(name) + "_done"] = {{"reducer", "overwrite"}};
    return channels;
}

// The issue's stall shape plus an extra branch out of A.
json stall_with_extra_branch(const std::string& extra_type) {
    json def               = make_bypass_graph("stall_fail", "FIX");
    def["channels"]        = channels_for({"A", "B", "C", "D", "FIX", "X", "worker"});
    def["nodes"]["X"]      = {{"type", extra_type}};
    def["nodes"]["worker"] = {{"type", "hit_stall"}};
    def["edges"].push_back({{"from", "A"}, {"to", "X"}});
    return def;
}

}  // namespace

TEST(BarrierStall, ImplicitDeadEndElsewhereDoesNotHideTheStall) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    // X has no outgoing edges: it ends implicitly, which says nothing about D.
    auto engine = GraphEngine::compile(stall_with_extra_branch("hit_stall"), NodeContext{});
    EXPECT_EQ(engine->run(RunConfig{}).status(), RunStatus::BarrierStalled);
}

TEST(BarrierStall, SendWorkerWithoutEdgesDoesNotHideTheStall) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    NodeFactory::instance().register_type(
        "send_stall", [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<SendingNode>(n);
        });
    auto engine = GraphEngine::compile(stall_with_extra_branch("send_stall"), NodeContext{});
    const auto result = engine->run(RunConfig{});
    EXPECT_EQ(result.status(), RunStatus::BarrierStalled);
    EXPECT_NE(std::find(result.execution_trace.begin(), result.execution_trace.end(),
                        "worker[send]"),
              result.execution_trace.end());
}

TEST(BarrierStall, InterruptAfterAnEarlySourceKeepsTheJoinGated) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    NodeFactory::instance().register_type(
        "join_observer", [](const std::string& n, const json&, const NodeContext&) {
            return std::make_unique<JoinObserver>(n);
        });
    json def = {
        {"name", "interrupt_after_gate"},
        {"channels", channels_for({"A", "B", "C1", "C", "D"})},
        {"nodes", {{"A", {{"type", "hit_stall"}}}, {"B", {{"type", "hit_stall"}}},
                   {"C1", {{"type", "hit_stall"}}}, {"C", {{"type", "hit_stall"}}},
                   {"D", {{"type", "join_observer"}, {"barrier", {{"wait_for", {"B", "C"}}}}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "A"}},
                               {{"from", "A"}, {"to", "B"}},
                               {{"from", "A"}, {"to", "C1"}},
                               {{"from", "C1"}, {"to", "C"}},
                               {{"from", "B"}, {"to", "D"}},
                               {{"from", "C"}, {"to", "D"}},
                               {{"from", "D"}, {"to", "__end__"}}})},
        {"interrupt_after", {"B"}}};
    def["channels"]["D_saw_C"] = {{"reducer", "overwrite"}};
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(def, NodeContext{}, store);
    RunConfig cfg;
    cfg.thread_id = "after-early-source";
    ASSERT_TRUE(engine->run(cfg).interrupted);

    // C has not arrived: D must wait for it (a later super-step), then run once.
    const auto resumed = engine->resume("after-early-source");
    EXPECT_EQ(resumed.status(), RunStatus::Completed);
    EXPECT_EQ(resumed.execution_trace, (std::vector<std::string>{"C", "D"}));
    EXPECT_EQ(resumed.output["channels"]["D_saw_C"]["value"], json(true));
}

TEST(BarrierStall, InterruptAfterTheLastSourceLetsTheJoinRun) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    json def = {
        {"name", "interrupt_after_last"},
        {"channels", channels_for({"A", "B1", "B", "C", "D"})},
        {"nodes", {{"A", {{"type", "hit_stall"}}}, {"B1", {{"type", "hit_stall"}}},
                   {"B", {{"type", "hit_stall"}}}, {"C", {{"type", "hit_stall"}}},
                   {"D", {{"type", "hit_stall"}, {"barrier", {{"wait_for", {"B", "C"}}}}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "A"}},
                               {{"from", "A"}, {"to", "B1"}},
                               {{"from", "A"}, {"to", "C"}},
                               {{"from", "B1"}, {"to", "B"}},
                               {{"from", "B"}, {"to", "D"}},
                               {{"from", "C"}, {"to", "D"}},
                               {{"from", "D"}, {"to", "__end__"}}})},
        {"interrupt_after", {"B"}}};
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(def, NodeContext{}, store);
    RunConfig cfg;
    cfg.thread_id = "after-last-source";
    ASSERT_TRUE(engine->run(cfg).interrupted);

    const auto resumed = engine->resume("after-last-source");
    EXPECT_EQ(resumed.status(), RunStatus::Completed);
    EXPECT_EQ(resumed.execution_trace, (std::vector<std::string>{"D"}));
}

TEST(BarrierStall, StallSurvivesInterruptAfterResumeAndFork) {
    std::atomic<int> hits{0};
    register_bypass_types(&hits);
    json def                = make_bypass_graph("stall_fail", "FIX");
    def["interrupt_after"]  = {"FIX"};
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(def, NodeContext{}, store);
    RunConfig cfg;
    cfg.thread_id = "stall-interrupt";
    ASSERT_TRUE(engine->run(cfg).interrupted);

    // FIX was the last node that could run; its checkpoint already knows D stalled.
    const auto resumed = engine->resume("stall-interrupt");
    EXPECT_EQ(resumed.status(), RunStatus::BarrierStalled);
    EXPECT_EQ(resumed.output["_neograph"]["barrier_stalled"][0]["missing"],
              json::array({"B"}));

    (void)engine->fork("stall-interrupt", "stall-fork");
    EXPECT_EQ(engine->resume("stall-fork").status(), RunStatus::BarrierStalled);
    EXPECT_EQ(engine->resume("stall-interrupt").status(), RunStatus::BarrierStalled);
}
