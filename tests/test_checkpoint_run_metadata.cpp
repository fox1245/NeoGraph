// #383: every checkpoint save path writes the same run metadata for the same
// run state, through one helper (detail::checkpoint_metadata_for).
//
// A checkpoint records how the run stopped once that is known (a barrier
// stall's witness, a failure exit) and the max_visits counters a resume
// continues from. A checkpoint saved during a super-step (a NodeInterrupt)
// resumes that step, so it keeps the counters from the start of the step; one
// saved between steps (an interrupt_before or interrupt_after gate, a step
// commit, a budget stop) keeps the counters as they are. Each test below
// compares the checkpoint one path saves with the checkpoint another path
// saves for the same run state, so a path that builds its metadata its own
// way shows up as a difference.

#include "../src/core/failure_routing.h"
#include "../src/core/run_context_runtime.h"

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
namespace gd = neograph::graph::detail;

namespace {

// Appends its name to "trace". With "ask" it raises NodeInterrupt until it is
// resumed with a value.
class MetaNode final : public GraphNode {
public:
    MetaNode(std::string name, json config) : name_(std::move(name)), config_(std::move(config)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        if (config_.value("ask", false) && !in.ctx.resume_value) throw NodeInterrupt("ask " + name_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    json        config_;
};

void register_types() {
    static const bool registered = [] {
        NodeFactory::instance().register_type(
            "meta.node",
            [](const std::string& name, const json& config, const NodeContext&) {
                return std::make_unique<MetaNode>(name, config);
            },
            json{{"type", "object"}});
        ConditionRegistry::instance().register_condition(
            "meta.fail", [](const GraphState&) { return std::string("fail"); });
        return true;
    }();
    (void)registered;
}

json node(json extra = json::object()) {
    json config{{"type", "meta.node"}};
    for (const auto& [key, value] : extra.items()) config[key] = value;
    return config;
}
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json definition_of(json nodes, json edges, json extra = json::object()) {
    json definition{
        {"schema_version", 1},
        {"name", "meta"},
        {"channels", json{{"trace", json{{"reducer", "append"}, {"initial", json::array()}}}}},
        {"nodes", std::move(nodes)},
        {"edges", std::move(edges)}};
    for (const auto& [key, value] : extra.items()) definition[key] = value;
    return definition;
}

// The named nodes declare max_visits, so every checkpoint carries their counters.
json counted(json extra = json::object(), std::vector<std::string> names = {"W", "T"}) {
    json policies = json::object();
    for (const auto& name : names) policies[name] = json{{"max_visits", 2}};
    extra["node_retry_policies"] = std::move(policies);
    return extra;
}

std::unique_ptr<GraphEngine> build(const json& definition, std::shared_ptr<CheckpointStore> store) {
    register_types();
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.stderr_notices   = false;
    return GraphEngine::build(definition, std::move(config));
}

// The checkpoint of `thread` with this phase and step.
Checkpoint saved(CheckpointStore& store, const std::string& thread, CheckpointPhase phase, int step) {
    std::optional<Checkpoint> found;
    for (const auto& checkpoint : store.list(thread, 1000))
        if (checkpoint.interrupt_phase == phase && checkpoint.step == step) found = checkpoint;
    if (!found) throw std::runtime_error("no checkpoint with that phase and step");
    return *found;
}

// The run metadata a checkpoint carries; the ephemeral guard depends on the
// channels a step wrote, not on the save path.
json run_metadata(const Checkpoint& checkpoint) {
    if (!checkpoint.metadata.is_object() || !checkpoint.metadata.contains("_neograph")) return json();
    return checkpoint.metadata["_neograph"];
}

}  // namespace

// ---------------------------------------------------------------------------
// The helper: both timings carry how the run stopped; only the counters differ
// ---------------------------------------------------------------------------

TEST(CheckpointRunMetadata, BothTimingsCarryHowTheRunStoppedAndDifferOnlyInTheCounters) {
    NodeRetryPolicySpec policy;
    policy.max_visits  = 3;
    const auto routing = gd::FailureRouting::build({{"T", policy}}, {}, {}, {});
    auto failure_state = std::make_shared<gd::FailureRunState>();
    ASSERT_TRUE(failure_state->activate(routing, "T"));
    failure_state->mark_step_start();
    ASSERT_TRUE(failure_state->activate(routing, "T"));

    auto runtime              = std::make_shared<gd::RunContextRuntime>();
    runtime->checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    runtime->failure_state    = failure_state;
    runtime->status_marks     = std::make_shared<gd::RunStatusMarks>();
    runtime->status_marks->barrier_stall = json::array({json{{"node", "J"}}});
    runtime->status_marks->failure_exit  = json{{"node", "ABORT"}, {"code", "NG.Aborted"}};
    RunContext ctx;
    gd::ScopedRunContextRuntime scope(ctx, runtime);

    const auto during  = gd::checkpoint_metadata_for(ctx, gd::CheckpointTiming::DuringStep);
    const auto between = gd::checkpoint_metadata_for(ctx, gd::CheckpointTiming::BetweenSteps);
    for (const auto* metadata : {&during, &between}) {
        EXPECT_EQ((*metadata)["_neograph"]["barrier_stalled"], *runtime->status_marks->barrier_stall);
        EXPECT_EQ((*metadata)["_neograph"]["failure_exit"], *runtime->status_marks->failure_exit);
    }
    // During a step: the counters from its start; between steps: as they are.
    EXPECT_EQ(during["_neograph"]["node_visits"], (json{{"T", 1}}));
    EXPECT_EQ(between["_neograph"]["node_visits"], (json{{"T", 2}}));
}

// ---------------------------------------------------------------------------
// Saved during a step: a NodeInterrupt matches the interrupt_before gate of
// the same step
// ---------------------------------------------------------------------------

TEST(CheckpointRunMetadata, ANodeInterruptOfOneNodeMatchesTheGateOfItsStep) {
    // W -> T. T is counted when it starts, and asks: its checkpoint resumes the
    // step, so it keeps the counters from the step's start, as the gate before
    // T does.
    const auto nodes = json{{"W", node()}, {"T", node()}};
    const auto edges = json::array({edge("__start__", "W"), edge("W", "T"), edge("T", "__end__")});

    auto asking_nodes        = nodes;
    asking_nodes["T"]["ask"] = true;
    auto asking_store        = std::make_shared<InMemoryCheckpointStore>();
    RunConfig asking;
    asking.thread_id = "asking";
    ASSERT_TRUE(build(definition_of(asking_nodes, edges, counted()), asking_store)->run(asking).interrupted);

    auto gate_store = std::make_shared<InMemoryCheckpointStore>();
    RunConfig gated;
    gated.thread_id = "gated";
    ASSERT_TRUE(build(definition_of(nodes, edges, counted(json{{"interrupt_before", json::array({"T"})}})), gate_store)
                    ->run(gated)
                    .interrupted);

    const auto interrupt = saved(*asking_store, "asking", CheckpointPhase::NodeInterrupt, 1);
    const auto gate      = saved(*gate_store, "gated", CheckpointPhase::Before, 1);
    EXPECT_EQ(run_metadata(interrupt), (json{{"node_visits", json{{"W", 1}}}}));
    EXPECT_EQ(run_metadata(interrupt), run_metadata(gate));
}

TEST(CheckpointRunMetadata, ANodeInterruptInAFanOutMatchesTheGateOfItsStep) {
    // W fans out to T and U; U asks while T, counted, completes in the same
    // step.
    const auto nodes = json{{"W", node()}, {"T", node()}, {"U", node()}};
    const auto edges = json::array({edge("__start__", "W"), edge("W", "T"), edge("W", "U"),
                                    edge("T", "__end__"), edge("U", "__end__")});

    auto asking_nodes        = nodes;
    asking_nodes["U"]["ask"] = true;
    auto asking_store        = std::make_shared<InMemoryCheckpointStore>();
    RunConfig asking;
    asking.thread_id = "fan-asking";
    ASSERT_TRUE(build(definition_of(asking_nodes, edges, counted()), asking_store)->run(asking).interrupted);

    auto gate_store = std::make_shared<InMemoryCheckpointStore>();
    RunConfig gated;
    gated.thread_id = "fan-gated";
    ASSERT_TRUE(build(definition_of(nodes, edges, counted(json{{"interrupt_before", json::array({"U"})}})), gate_store)
                    ->run(gated)
                    .interrupted);

    const auto interrupt = saved(*asking_store, "fan-asking", CheckpointPhase::NodeInterrupt, 1);
    const auto gate      = saved(*gate_store, "fan-gated", CheckpointPhase::Before, 1);
    EXPECT_EQ(run_metadata(interrupt), (json{{"node_visits", json{{"W", 1}}}}));
    EXPECT_EQ(run_metadata(interrupt), run_metadata(gate));
}

// ---------------------------------------------------------------------------
// Saved between steps: an interrupt_after gate, a step commit and a budget
// stop agree, including how the run stopped
// ---------------------------------------------------------------------------

namespace {

// The After checkpoint of `node` in a run with interrupt_after, and the
// Completed checkpoint of the same step in a run without it.
std::pair<Checkpoint, Checkpoint> after_and_commit(const json& definition, const char* node, int step) {
    auto plain_store = std::make_shared<InMemoryCheckpointStore>();
    RunConfig plain;
    plain.thread_id = "plain";
    (void)build(definition, plain_store)->run(plain);

    auto with_gate               = definition;
    with_gate["interrupt_after"] = json::array({node});
    auto after_store             = std::make_shared<InMemoryCheckpointStore>();
    RunConfig after;
    after.thread_id = "after";
    const auto paused = build(with_gate, after_store)->run(after);
    EXPECT_TRUE(paused.interrupted);
    return {saved(*after_store, "after", CheckpointPhase::After, step),
            saved(*plain_store, "plain", CheckpointPhase::Completed, step)};
}

}  // namespace

TEST(CheckpointRunMetadata, AnInterruptAfterMatchesTheCommitOfItsStep) {
    const auto [after, commit] = after_and_commit(
        definition_of(json{{"W", node()}, {"T", node()}},
              json::array({edge("__start__", "W"), edge("W", "T"), edge("T", "__end__")}), counted()),
        "T", 1);
    EXPECT_EQ(run_metadata(after), (json{{"node_visits", json{{"W", 1}, {"T", 1}}}}));
    EXPECT_EQ(run_metadata(after), run_metadata(commit));
}

TEST(CheckpointRunMetadata, ABarrierStallIsOnTheInterruptAfterAndTheCommit) {
    // X and C join at J. C routes to FIX, which ends; J waits for C forever.
    json nodes{{"A", node()}, {"X", node()}, {"C", node()}, {"FIX", node()}, {"J", node()}};
    nodes["J"]["barrier"] = json{{"wait_for", json::array({"X", "C"})}};
    auto definition       = definition_of(std::move(nodes),
                                  json::array({edge("__start__", "A"), edge("A", "X"), edge("A", "C"),
                                               edge("X", "J"), edge("J", "__end__")}),
                                  counted(json::object(), {"A"}));
    definition["conditional_edges"] = json::array(
        {json{{"from", "C"}, {"condition", "meta.fail"}, {"routes", json{{"pass", "J"}, {"fail", "FIX"}}}}});
    const auto [after, commit] = after_and_commit(definition, "FIX", 2);
    ASSERT_TRUE(run_metadata(commit).contains("barrier_stalled")) << run_metadata(commit).dump();
    EXPECT_EQ(run_metadata(after), run_metadata(commit));
}

TEST(CheckpointRunMetadata, AFailureExitIsOnTheInterruptAfterAndTheCommit) {
    auto definition = definition_of(json{{"W", node()}, {"ABORT", node()}},
                            json::array({edge("__start__", "W"), edge("W", "ABORT")}),
                            counted(json{{"failure_exits", json{{"ABORT", json{{"code", "plan.stop"}}}}}},
                                    {"W"}));
    const auto [after, commit] = after_and_commit(definition, "ABORT", 1);
    EXPECT_EQ(run_metadata(commit)["failure_exit"], (json{{"node", "ABORT"}, {"code", "plan.stop"}}));
    EXPECT_EQ(run_metadata(after), run_metadata(commit));
}

TEST(CheckpointRunMetadata, ABudgetStopMatchesTheCheckpointItResumed) {
    // A run stopped by its budget before T commits the state it resumed: the
    // counters of the interrupt_after checkpoint after W.
    auto definition = definition_of(json{{"W", node()}, {"T", node()}},
                            json::array({edge("__start__", "W"), edge("W", "T"), edge("T", "__end__")}),
                            counted(json{{"interrupt_after", json::array({"W"})}}));
    auto      store  = std::make_shared<InMemoryCheckpointStore>();
    auto      engine = build(definition, store);
    RunConfig run;
    run.thread_id = "budget";
    ASSERT_TRUE(engine->run(run).interrupted);

    run.budget_exhausted = std::make_shared<std::atomic_bool>(true);
    (void)engine->resume(run);
    const auto after  = saved(*store, "budget", CheckpointPhase::After, 0);
    const auto commit = saved(*store, "budget", CheckpointPhase::Completed, 1);
    EXPECT_EQ(run_metadata(after), (json{{"node_visits", json{{"W", 1}}}}));
    EXPECT_EQ(run_metadata(commit), run_metadata(after));
}
