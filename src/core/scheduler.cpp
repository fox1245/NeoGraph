#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>

#include <set>
#include <stdexcept>

namespace neograph::graph {

Scheduler::Scheduler(const std::vector<Edge>& edges,
                     const std::vector<ConditionalEdge>& conditional_edges)
    : edges_(edges), conditional_edges_(conditional_edges) {}

std::vector<std::string> Scheduler::resolve_next_nodes(
    const std::string& current, const GraphState& state) const {

    for (const auto& ce : conditional_edges_) {
        if (ce.from == current) {
            auto cond_fn = ConditionRegistry::instance().get(ce.condition);
            auto result  = cond_fn(state);
            auto it = ce.routes.find(result);
            if (it != ce.routes.end()) return {it->second};
            // Fallback: last map entry. Documented contract matches the
            // pre-refactor engine — callers relied on this for default
            // routes keyed like {"foo": ..., "default": ...} where the
            // lexicographically-last key was the fallback.
            return {ce.routes.rbegin()->second};
        }
    }

    std::vector<std::string> successors;
    for (const auto& e : edges_) {
        if (e.from == current) {
            successors.push_back(e.to);
        }
    }

    if (successors.empty()) return {std::string(END_NODE)};
    return successors;
}

std::vector<std::string> Scheduler::plan_start_step() const {
    std::vector<std::string> out;
    for (const auto& e : edges_) {
        if (e.from == std::string(START_NODE) && e.to != std::string(END_NODE)) {
            out.push_back(e.to);
        }
    }
    return out;
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<StepRouting>& routings,
    const GraphState& state) const {

    NextStepPlan plan;

    // Pass 1: scan for Command.goto_node override. Last-writer-wins
    // under parallel dispatch (Taskflow result ordering is already
    // non-deterministic there; preserving the existing behavior
    // rather than inventing a new tie-break).
    std::optional<std::string> command_goto;
    for (const auto& r : routings) {
        if (r.command_goto) command_goto = r.command_goto;
    }

    if (command_goto) {
        plan.winning_command_goto = command_goto;
        if (*command_goto == std::string(END_NODE)) {
            plan.hit_end = true;
        } else {
            plan.ready.push_back(*command_goto);
        }
        return plan;
    }

    // Pass 2: union of direct successors. std::set gives us both
    // dedup (parallel fan-in → single downstream execution this
    // super-step) and deterministic-enough ordering.
    std::set<std::string> candidates;
    for (const auto& r : routings) {
        for (const auto& next : resolve_next_nodes(r.node_name, state)) {
            if (next == std::string(END_NODE)) {
                plan.hit_end = true;
            } else {
                candidates.insert(next);
            }
        }
    }

    plan.ready.reserve(candidates.size());
    for (const auto& c : candidates) plan.ready.push_back(c);
    return plan;
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<std::string>& just_ran,
    const std::vector<NodeResult>& results,
    const GraphState& state) const {

    if (just_ran.size() != results.size()) {
        throw std::invalid_argument(
            "Scheduler::plan_next_step: just_ran and results size mismatch");
    }

    std::vector<StepRouting> routings;
    routings.reserve(just_ran.size());
    for (size_t i = 0; i < just_ran.size(); ++i) {
        StepRouting r;
        r.node_name = just_ran[i];
        if (results[i].command && !results[i].command->goto_node.empty()) {
            r.command_goto = results[i].command->goto_node;
        }
        routings.push_back(std::move(r));
    }
    return plan_next_step(routings, state);
}

} // namespace neograph::graph
