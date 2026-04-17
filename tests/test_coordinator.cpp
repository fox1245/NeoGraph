// Pure-unit tests for CheckpointCoordinator. These exercise the
// coordinator's contract without any GraphEngine involvement:
//
//   * disabled semantics (null store / empty thread_id → no-ops)
//   * save_super_step round-trip through the store
//   * load_for_resume phase-offset rules for every CheckpointPhase
//   * pending-writes lifecycle: record → replay via ResumeContext → clear
//
// Splitting this out from execute_graph means future engine refactors
// (NodeExecutor extraction) can move around the call sites without
// touching checkpoint semantics — these tests freeze the contract.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

static ReducerFn overwrite_fn() { return ReducerRegistry::instance().get("overwrite"); }

// A minimal single-channel GraphState for save/restore round trips.
// GraphState holds a shared_mutex so it is not movable — caller passes
// one in and we populate it.
void populate_state(GraphState& s, const std::string& channel, const json& value) {
    s.init_channel(channel, ReducerType::OVERWRITE, overwrite_fn(), json("init"));
    s.write(channel, value);
}

} // namespace

// =========================================================================
// Disabled semantics
// =========================================================================

TEST(CoordinatorDisabled, NullStoreMakesEverythingNoOp) {
    CheckpointCoordinator coord(nullptr, "thread-null");
    EXPECT_FALSE(coord.enabled());

    GraphState state; populate_state(state, "x", json(1));
    auto id = coord.save_super_step(state, "n", {"n"},
                                    CheckpointPhase::Completed, 0, "", {});
    EXPECT_TRUE(id.empty());

    auto ctx = coord.load_for_resume();
    EXPECT_FALSE(ctx.have_cp);

    // Pending write / clear must not throw on a null store.
    coord.record_pending_write("", "t0", "t0", "n", NodeResult{}, 0);
    coord.clear_pending_writes("");
}

TEST(CoordinatorDisabled, EmptyThreadIdMakesEverythingNoOp) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, /*thread_id=*/"");
    EXPECT_FALSE(coord.enabled());

    GraphState state; populate_state(state, "x", json(1));
    EXPECT_TRUE(coord.save_super_step(state, "n", {"n"},
                                      CheckpointPhase::Completed, 0, "", {}).empty());
    // Nothing should have landed in the store.
    EXPECT_EQ(store->size(), 0u);
}

// =========================================================================
// save_super_step round-trip
// =========================================================================

TEST(CoordinatorSave, RoundTripsCoreFields) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "round-trip");
    GraphState state; populate_state(state, "x", json(42));

    BarrierState bstate = {{"join", {"a", "b"}}};
    auto cp_id = coord.save_super_step(state, "router", {"a", "b"},
                                        CheckpointPhase::Completed,
                                        /*step=*/3, /*parent=*/"", bstate);
    EXPECT_FALSE(cp_id.empty());

    auto cp = store->load_by_id(cp_id);
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->thread_id, "round-trip");
    EXPECT_EQ(cp->current_node, "router");
    EXPECT_EQ(cp->next_nodes, (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(cp->interrupt_phase, CheckpointPhase::Completed);
    EXPECT_EQ(cp->step, 3);
    EXPECT_EQ(cp->barrier_state, bstate);
    EXPECT_EQ(cp->schema_version, CHECKPOINT_SCHEMA_VERSION);
    EXPECT_EQ(cp->channel_values["channels"]["x"]["value"].get<int>(), 42);
}

// =========================================================================
// load_for_resume: empty + phase offsets
// =========================================================================

TEST(CoordinatorResume, HaveCpFalseWhenStoreEmpty) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "never-ran");
    auto ctx = coord.load_for_resume();
    EXPECT_FALSE(ctx.have_cp);
    EXPECT_TRUE(ctx.next_nodes.empty());
    EXPECT_TRUE(ctx.replay_results.empty());
    EXPECT_TRUE(ctx.barrier_state.empty());
    EXPECT_EQ(ctx.start_step, 0);
}

TEST(CoordinatorResume, PhaseBeforeReentersSameStep) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "phase-before");
    GraphState state; populate_state(state, "x", json(1));

    coord.save_super_step(state, "n", {"n"},
                          CheckpointPhase::Before, /*step=*/5, "", {});
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.start_step, 5) << "Before phase re-enters AT the cp step";
    EXPECT_EQ(ctx.phase, CheckpointPhase::Before);
}

TEST(CoordinatorResume, PhaseNodeInterruptReentersSameStep) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "phase-ni");
    GraphState state; populate_state(state, "x", json(1));

    coord.save_super_step(state, "n", {"n"},
                          CheckpointPhase::NodeInterrupt, /*step=*/7, "", {});
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.start_step, 7);
}

TEST(CoordinatorResume, PhaseAfterAdvancesStep) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "phase-after");
    GraphState state; populate_state(state, "x", json(1));

    coord.save_super_step(state, "n", {"m"},
                          CheckpointPhase::After, /*step=*/4, "", {});
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.start_step, 5) << "After phase advances past the committed step";
}

TEST(CoordinatorResume, PhaseCompletedAdvancesStep) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "phase-completed");
    GraphState state; populate_state(state, "x", json(1));

    coord.save_super_step(state, "n", {"m"},
                          CheckpointPhase::Completed, /*step=*/9, "", {});
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.start_step, 10);
}

TEST(CoordinatorResume, PhaseUpdatedAdvancesStep) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "phase-updated");
    GraphState state; populate_state(state, "x", json(1));

    coord.save_super_step(state, "n", {"m"},
                          CheckpointPhase::Updated, /*step=*/2, "", {});
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.start_step, 3)
        << "Updated is treated like a committed cp for step advancement";
}

// =========================================================================
// BarrierState restoration on resume
// =========================================================================

TEST(CoordinatorResume, BarrierStateRoundTripsThroughCheckpoint) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "barrier-rt");
    GraphState state; populate_state(state, "x", json(1));

    BarrierState bstate = {
        {"join_a", {"upstream1", "upstream2"}},
        {"join_b", {"upstream3"}}
    };
    coord.save_super_step(state, "n", {"n"},
                          CheckpointPhase::Before, 2, "", bstate);

    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    EXPECT_EQ(ctx.barrier_state, bstate);
}

// =========================================================================
// Pending-writes lifecycle
// =========================================================================

TEST(CoordinatorPending, RecordReplayClearFullCycle) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "pending-cycle");
    GraphState state; populate_state(state, "x", json(1));

    auto parent_id = coord.save_super_step(state, "setup", {"a"},
                                           CheckpointPhase::Completed, 0, "", {});
    ASSERT_FALSE(parent_id.empty());

    NodeResult nr;
    nr.writes = {ChannelWrite{"result", json(123)}};
    nr.sends.push_back(Send{"worker", json{{"idx", 0}}});
    Command cmd;
    cmd.goto_node = "overridden";
    cmd.updates = {ChannelWrite{"route", json("x")}};
    nr.command = cmd;

    coord.record_pending_write(parent_id, "task-7", "task-7", "a", nr, /*step=*/1);

    // Load resume context and confirm replay_results carries the rehydrated
    // NodeResult verbatim — this is the path the engine uses on resume to
    // skip already-completed nodes.
    auto ctx = coord.load_for_resume();
    ASSERT_TRUE(ctx.have_cp);
    auto it = ctx.replay_results.find("task-7");
    ASSERT_NE(it, ctx.replay_results.end());
    const auto& replayed = it->second;
    ASSERT_EQ(replayed.writes.size(), 1u);
    EXPECT_EQ(replayed.writes[0].channel, "result");
    EXPECT_EQ(replayed.writes[0].value.get<int>(), 123);
    ASSERT_EQ(replayed.sends.size(), 1u);
    EXPECT_EQ(replayed.sends[0].target_node, "worker");
    ASSERT_TRUE(replayed.command.has_value());
    EXPECT_EQ(replayed.command->goto_node, "overridden");

    // Clear wipes the log; a fresh resume sees no replay entries.
    coord.clear_pending_writes(parent_id);
    auto ctx2 = coord.load_for_resume();
    EXPECT_TRUE(ctx2.replay_results.empty());
}

TEST(CoordinatorPending, ClearEmptyParentIdIsNoOp) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "clear-empty");
    // Must not crash, must not touch unrelated state.
    coord.clear_pending_writes("");
    EXPECT_EQ(store->size(), 0u);
}

TEST(CoordinatorPending, RecordIsNoOpWhenDisabled) {
    CheckpointCoordinator coord(nullptr, "disabled");
    // Exercises the early return in record_pending_write.
    coord.record_pending_write("parent", "t", "t", "n", NodeResult{}, 0);
    SUCCEED();
}
