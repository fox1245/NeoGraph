#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace neograph::graph {

Scheduler::Scheduler(const std::vector<Edge>& edges,
                     const std::vector<ConditionalEdge>& conditional_edges,
                     BarrierSpecs barrier_specs)
    : edges_(edges),
      conditional_edges_(conditional_edges),
      barrier_specs_(std::move(barrier_specs)) {}

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

// Shared implementation — `barrier_state` is nullptr when barriers
// are disabled for this call (2-arg overloads). With nullptr, every
// candidate fires regardless of barrier_specs_; with a non-null
// pointer the caller owns the accumulation across super-steps.
static NextStepPlan plan_impl(
    const Scheduler& sch,
    const std::vector<StepRouting>& routings,
    const GraphState& state,
    const BarrierSpecs& barrier_specs,
    BarrierState* barrier_state) {

    NextStepPlan plan;

    // Pass 1: Command.goto override — preempts everything, including
    // barriers. Last-writer-wins under parallel dispatch (Taskflow
    // result ordering is already non-deterministic there).
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

    // Pass 2: build signaler → target map so barrier gating can see
    // WHICH upstreams fired this super-step. Non-barrier targets are
    // still just deduped via std::set semantics.
    std::map<std::string, std::set<std::string>> signals;
    for (const auto& r : routings) {
        for (const auto& next : sch.resolve_next_nodes(r.node_name, state)) {
            if (next == std::string(END_NODE)) {
                plan.hit_end = true;
            } else {
                signals[next].insert(r.node_name);
            }
        }
    }

    // Pass 3: apply barrier gating. For non-barrier targets or calls
    // without caller-owned state, fire normally.
    std::set<std::string> ready_set;
    for (const auto& [target, signalers] : signals) {
        auto spec_it = barrier_specs.find(target);
        const bool is_barrier = spec_it != barrier_specs.end();

        if (!is_barrier || barrier_state == nullptr) {
            ready_set.insert(target);
            continue;
        }

        // Accumulate received signals. Persists across plan_next_step
        // calls because the caller owns the map.
        auto& accum = (*barrier_state)[target];
        for (const auto& s : signalers) accum.insert(s);

        // Fire iff every required upstream has signaled at least once.
        const bool satisfied = std::includes(
            accum.begin(), accum.end(),
            spec_it->second.begin(), spec_it->second.end());
        if (satisfied) {
            ready_set.insert(target);
            accum.clear();  // reset for future loops through the barrier
        }
        // else: deferred — target stays out of ready, accum retained.
    }

    plan.ready.reserve(ready_set.size());
    for (const auto& c : ready_set) plan.ready.push_back(c);
    return plan;
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<StepRouting>& routings,
    const GraphState& state) const {
    return plan_impl(*this, routings, state, barrier_specs_, nullptr);
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<StepRouting>& routings,
    const GraphState& state,
    BarrierState& barrier_state) const {
    return plan_impl(*this, routings, state, barrier_specs_, &barrier_state);
}

// Shared zip helper used by both NodeResult overloads.
static std::vector<StepRouting> zip_routings(
    const std::vector<std::string>& just_ran,
    const std::vector<NodeResult>& results) {

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
    return routings;
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<std::string>& just_ran,
    const std::vector<NodeResult>& results,
    const GraphState& state) const {
    return plan_next_step(zip_routings(just_ran, results), state);
}

NextStepPlan Scheduler::plan_next_step(
    const std::vector<std::string>& just_ran,
    const std::vector<NodeResult>& results,
    const GraphState& state,
    BarrierState& barrier_state) const {
    return plan_next_step(zip_routings(just_ran, results), state, barrier_state);
}

} // namespace neograph::graph
