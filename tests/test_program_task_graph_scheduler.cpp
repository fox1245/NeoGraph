// The pure readiness rule of the task graph scheduler (src/program/task_graph_scheduler.h).

#include "task_graph_scheduler.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using neograph::program::TaskGraphTaskState;
using neograph::program::detail::compute_ready;
using neograph::program::detail::SchedulerTask;
using State = TaskGraphTaskState;

struct Node {
    std::string              id;
    std::vector<std::string> depends_on;
    State                    state = State::Pending;
};

std::vector<SchedulerTask> view_of(const std::vector<Node>& nodes) {
    std::vector<SchedulerTask> view;
    for (const auto& node : nodes) view.push_back({node.id, &node.depends_on, node.state});
    return view;
}

using Indices = std::vector<std::size_t>;

}  // namespace

TEST(TaskGraphSchedulerTest, TasksWithNoDependenciesAreReadyInPositionOrder) {
    const std::vector<Node> nodes{{"a", {}}, {"b", {}}, {"c", {"a"}}};
    const auto              ready = compute_ready(view_of(nodes));
    EXPECT_EQ(ready.ready, (Indices{0, 1}));
    EXPECT_TRUE(ready.cascade_failed.empty());
}

TEST(TaskGraphSchedulerTest, ATaskWaitsUntilEveryDependencyIsCompleted) {
    std::vector<Node> nodes{{"a", {}, State::Completed}, {"b", {}, State::Active},
                            {"c", {"a", "b"}}};
    EXPECT_EQ(compute_ready(view_of(nodes)).ready, (Indices{1}));
    nodes[1].state = State::Completed;
    EXPECT_EQ(compute_ready(view_of(nodes)).ready, (Indices{2}));
}

TEST(TaskGraphSchedulerTest, ActiveTasksAreReadyAgainAfterARestart) {
    // A crash can leave a task Active; the scheduler treats it like a Pending one.
    const std::vector<Node> nodes{{"a", {}, State::Active}, {"b", {"a"}}};
    EXPECT_EQ(compute_ready(view_of(nodes)).ready, (Indices{0}));
}

TEST(TaskGraphSchedulerTest, SettledTasksAreNeverReady) {
    const std::vector<Node> nodes{{"a", {}, State::Completed}, {"b", {}, State::Failed},
                                  {"c", {}, State::Cancelled}};
    const auto              ready = compute_ready(view_of(nodes));
    EXPECT_TRUE(ready.ready.empty());
    EXPECT_TRUE(ready.cascade_failed.empty());
}

TEST(TaskGraphSchedulerTest, AFailedOrCancelledDependencyFailsTheDependentAndItsDependents) {
    const std::vector<Node> nodes{{"a", {}, State::Failed},
                                  {"b", {}, State::Cancelled},
                                  {"c", {"a"}},
                                  {"d", {"b", "x"}},
                                  {"e", {"c"}},
                                  {"f", {}}};
    const auto ready = compute_ready(view_of(nodes));
    // e depends on c, which the same scan has just failed.
    EXPECT_EQ(ready.cascade_failed, (Indices{2, 3, 4}));
    EXPECT_EQ(ready.ready, (Indices{5}));
}

TEST(TaskGraphSchedulerTest, FailureTakesPrecedenceOverAWaitingDependency) {
    const std::vector<Node> nodes{{"a", {}}, {"b", {}, State::Failed}, {"c", {"a", "b"}}};
    const auto              ready = compute_ready(view_of(nodes));
    EXPECT_EQ(ready.cascade_failed, (Indices{2}));
    EXPECT_EQ(ready.ready, (Indices{0}));
}

TEST(TaskGraphSchedulerTest, AnUnknownDependencyKeepsTheTaskWaiting) {
    const std::vector<Node> nodes{{"a", {"missing"}}};
    const auto              ready = compute_ready(view_of(nodes));
    EXPECT_TRUE(ready.ready.empty());
    EXPECT_TRUE(ready.cascade_failed.empty());
}

TEST(TaskGraphSchedulerTest, TheSameStatesGiveTheSameAnswer) {
    const std::vector<Node> nodes{{"a", {}}, {"b", {"a"}}, {"c", {}, State::Failed}, {"d", {"c"}}};
    const auto              first  = compute_ready(view_of(nodes));
    const auto              second = compute_ready(view_of(nodes));
    EXPECT_EQ(first.ready, second.ready);
    EXPECT_EQ(first.cascade_failed, second.cascade_failed);
}
