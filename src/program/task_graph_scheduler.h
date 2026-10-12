// Internal, pure scheduling rules of a published task graph. Not part of the public API.
//
// The expansion scheduler dispatches in waves: it takes a snapshot of the tasks that are ready,
// runs them in chunks, and settles each chunk in launch order. What is ready, and what fails
// because a dependency did, is a function of the durable per-task states alone; it never
// depends on completion timing. This header holds that function so the rule is stated once
// and can be tested without a runtime.
#pragma once

#include <neograph/program/task_graph_fragment.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program::detail {

/// One task as the scheduler sees it: identity, direct dependencies and durable state.
/// `depends_on` must outlive the call.
struct SchedulerTask {
    std::string_view                id;
    const std::vector<std::string>* depends_on = nullptr;
    TaskGraphTaskState              state      = TaskGraphTaskState::Pending;
};

struct ReadySet {
    /// Indices of the tasks that may be dispatched now, in position order.
    std::vector<std::size_t> ready;
    /// Indices of tasks that did not start and fail with P_TASK_DEPENDENCY because a
    /// dependency Failed or was Cancelled, in position order. A task whose dependency is in
    /// this list is in it too (the walk is in position order, which is topological).
    std::vector<std::size_t> cascade_failed;
};

/// Pending and Active tasks whose dependencies are all Completed are ready; those with a
/// Failed or Cancelled dependency cascade. `tasks` must be in canonical (topological) order.
inline ReadySet compute_ready(const std::vector<SchedulerTask>& tasks) {
    std::map<std::string_view, TaskGraphTaskState, std::less<>> state;
    for (const auto& task : tasks) state.emplace(task.id, task.state);

    ReadySet result;
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto& task = tasks[index];
        if (state[task.id] != TaskGraphTaskState::Pending &&
            state[task.id] != TaskGraphTaskState::Active)
            continue;
        bool dependency_failed  = false;
        bool dependency_waiting = false;
        if (task.depends_on) {
            for (const auto& dependency : *task.depends_on) {
                const auto found = state.find(std::string_view(dependency));
                const auto dependency_state =
                    found == state.end() ? TaskGraphTaskState::Pending : found->second;
                if (dependency_state == TaskGraphTaskState::Failed ||
                    dependency_state == TaskGraphTaskState::Cancelled) {
                    dependency_failed = true;
                    break;
                }
                if (dependency_state != TaskGraphTaskState::Completed) dependency_waiting = true;
            }
        }
        if (dependency_failed) {
            state[task.id] = TaskGraphTaskState::Failed;
            result.cascade_failed.push_back(index);
            continue;
        }
        if (!dependency_waiting) result.ready.push_back(index);
    }
    return result;
}

}  // namespace neograph::program::detail
