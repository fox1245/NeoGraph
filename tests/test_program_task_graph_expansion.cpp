// End-to-end characterization of the task graph expansion scheduler.
//
// These tests pin the behavior of ExpandTaskGraph that later work builds on: dispatch in
// waves, a barrier per chunk, settlement in launch order, the failure policies, and the
// durable per-task record. They observe the scheduler only through the fragment store, as a
// host would: every compare-and-swap of the record is logged as the task transition it makes.

#include "program_expansion_nodes.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;
using namespace neograph::program_test;

using namespace neograph::program_test::expansion;

}  // namespace

TEST(TaskGraphExpansionCharacterization, ADependencyChainRunsOneTaskPerChunkInOrder) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "ok/v1"), task("b", "ok/v1", {"a"}),
                                        task("c", "ok/v1", {"b"})}));

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(run.events, (Events{"a:active:1", "a:completed:1", "b:active:1", "b:completed:1",
                                  "c:active:1", "c:completed:1", "terminal"}));
    ASSERT_TRUE(run.record.has_value());
    // publish, an Active and a settled write per task, and the terminal write
    EXPECT_EQ(run.record->revision, 8U);
    EXPECT_TRUE(run.record->terminal);
    EXPECT_EQ(run.result.remaining_budget().max_dynamic_compiles, 0U);
}

TEST(TaskGraphExpansionCharacterization, IndependentTasksShareAChunkUpToTheConcurrencyWidth) {
    ExpansionHarness h;
    h.reset_observations();
    gate_target.store(2);
    const auto run = h.run(proposal_of({task("a", "gate/v1"), task("b", "gate/v1")}),
                           RunOptions{.concurrency = 2});

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    // Both are Active before either settles: one chunk of two, then settlement in launch order.
    EXPECT_EQ(run.events, (Events{"a:active:1", "b:active:1", "a:completed:1", "b:completed:1",
                                  "terminal"}));
    EXPECT_EQ(gate_peak.load(), 2);
}

TEST(TaskGraphExpansionCharacterization, WidthOneRunsIndependentTasksInSeparateChunks) {
    ExpansionHarness h;
    h.reset_observations();
    gate_target.store(1);
    const auto run = h.run(proposal_of({task("a", "gate/v1"), task("b", "gate/v1")}),
                           RunOptions{.concurrency = 1});

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(run.events, (Events{"a:active:1", "a:completed:1", "b:active:1", "b:completed:1",
                                  "terminal"}));
    EXPECT_EQ(gate_peak.load(), 1);
}

TEST(TaskGraphExpansionCharacterization, ATaskReadyMidChunkWaitsForTheWholeChunk) {
    // a is slow, b is quick, c needs only b. c is ready as soon as b settles in the model, but
    // the scheduler takes a snapshot of ready tasks and waits for the whole chunk.
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "slow/v1"), task("b", "ok/v1"),
                                        task("c", "ok/v1", {"b"})}),
                           RunOptions{.concurrency = 2});

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed)
        << run.result.serialize_canonical();
    EXPECT_EQ(run.events, (Events{"a:active:1", "b:active:1", "a:completed:1", "b:completed:1",
                                  "c:active:1", "c:completed:1", "terminal"}));
    const auto log = node_log_copy();
    const auto slow_end = std::find(log.begin(), log.end(), "slow:end");
    ASSERT_NE(slow_end, log.end());
    // c's echo starts after the slow task ended; b's echo ended before it.
    EXPECT_EQ(std::count(log.begin(), slow_end, "echo:end"), 1);
    EXPECT_EQ(std::count(slow_end, log.end(), "echo:start"), 1);
}

TEST(TaskGraphExpansionCharacterization, SettlementFollowsLaunchOrderNotCompletionOrder) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "slow/v1"), task("b", "ok/v1")}),
                           RunOptions{.concurrency = 2});

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    const auto log = node_log_copy();
    const auto echo_end = std::find(log.begin(), log.end(), "echo:end");
    const auto slow_end = std::find(log.begin(), log.end(), "slow:end");
    ASSERT_NE(echo_end, log.end());
    ASSERT_NE(slow_end, log.end());
    EXPECT_LT(echo_end, slow_end);  // b finished first ...
    EXPECT_EQ(run.events, (Events{"a:active:1", "b:active:1", "a:completed:1", "b:completed:1",
                                  "terminal"}));  // ... and is still settled second
}

TEST(TaskGraphExpansionCharacterization, DispatchOrderIsCanonicalWhateverTheDeclarationOrder) {
    ExpansionHarness first;
    first.reset_observations();
    const auto forward = first.run(proposal_of({task("a", "ok/v1"), task("b", "ok/v1"),
                                                task("c", "ok/v1", {"a"})}));
    ExpansionHarness second;
    second.reset_observations();
    const auto reversed = second.run(proposal_of({task("c", "ok/v1", {"a"}), task("b", "ok/v1"),
                                                  task("a", "ok/v1")}));

    ASSERT_EQ(forward.result.status(), ProgramTerminalStatus::Completed);
    ASSERT_EQ(reversed.result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(forward.events, reversed.events);
    EXPECT_EQ(forward.events, (Events{"a:active:1", "a:completed:1", "b:active:1", "b:completed:1",
                                      "c:active:1", "c:completed:1", "terminal"}));
    ASSERT_TRUE(forward.record.has_value());
    ASSERT_TRUE(reversed.record.has_value());
    // The fragment id also binds the parent run; the proposal identity does not.
    EXPECT_EQ(forward.record->fragment.proposal_hash(), reversed.record->fragment.proposal_hash());
}

TEST(TaskGraphExpansionCharacterization, FailFastCancelsEveryTaskThatHasNotStarted) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "fail/v1"), task("b", "ok/v1"),
                                        task("c", "ok/v1", {"a"})}),
                           RunOptions{.failure_policy = "fail_fast"});

    EXPECT_EQ(run.result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(run.result.failure().has_value());
    EXPECT_EQ(run.result.failure()->code, "P_TASK_FAILED");
    EXPECT_EQ(run.events, (Events{"a:active:1", "a:failed:1", "b:cancelled:0", "c:cancelled:0",
                                  "terminal"}));
    ASSERT_TRUE(run.record.has_value());
    EXPECT_EQ((*run.record->tasks[1].failure)["code"], "P_TASK_FAIL_FAST");
    EXPECT_EQ(run.record->tasks[1].attempt, 0U);
}

TEST(TaskGraphExpansionCharacterization, CollectFailsOnlyTheDependentsOfAFailedTask) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "fail/v1"), task("b", "ok/v1"),
                                        task("c", "ok/v1", {"a"}), task("d", "ok/v1", {"b"})}));

    EXPECT_EQ(run.result.status(), ProgramTerminalStatus::Failed);
    // b runs in the same snapshot as a; c is failed by the next scan; d still runs.
    EXPECT_EQ(run.events, (Events{"a:active:1", "a:failed:1", "b:active:1", "b:completed:1",
                                  "c:failed:0", "d:active:1", "d:completed:1", "terminal"}));
    ASSERT_TRUE(run.record.has_value());
    const auto& c = run.record->tasks[2];
    EXPECT_EQ(c.task_id, "c");
    EXPECT_EQ(c.state, TaskGraphTaskState::Failed);
    EXPECT_EQ((*c.failure)["code"], "P_TASK_DEPENDENCY");
    EXPECT_TRUE(c.attempt_identity.empty());
}

TEST(TaskGraphExpansionCharacterization, EverySettledTaskKeepsAStableAttemptAndChildIdentity) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("a", "ok/v1"), task("b", "ok/v1", {"a"})}));

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    ASSERT_TRUE(run.record.has_value());
    for (const auto& record : run.record->tasks) {
        EXPECT_EQ(record.attempt, 1U);
        EXPECT_EQ(record.attempt_identity, attempt_identity(run.fragment_id, record.task_id, 1));
        ASSERT_TRUE(record.child_run_id.has_value());
        EXPECT_TRUE(record.output.has_value());
    }
    EXPECT_NE(*run.record->tasks[0].child_run_id, *run.record->tasks[1].child_run_id);
}

TEST(TaskGraphExpansionCharacterization, ARunWithNoDependenciesBetweenTasksSettlesThemInPositionOrder) {
    ExpansionHarness h;
    h.reset_observations();
    const auto run = h.run(proposal_of({task("b", "ok/v1"), task("a", "ok/v1"), task("c", "ok/v1")}),
                           RunOptions{.concurrency = 4});

    ASSERT_EQ(run.result.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(run.events, (Events{"a:active:1", "b:active:1", "c:active:1", "a:completed:1",
                                  "b:completed:1", "c:completed:1", "terminal"}));
}
