/**
 * @file graph/scheduler.h
 * @brief Pure routing / next-step-planning logic extracted from GraphEngine.
 *
 * Scheduler owns the graph topology (edges + conditional edges) and
 * answers two questions:
 *   1. Given the current state, what are the successors of a given node?
 *   2. Given the nodes that just ran (and their routing signals), what
 *      is the next super-step's ready set?
 *
 * It has NO knowledge of threading, checkpointing, retries, or HITL —
 * every one of those concerns stays in GraphEngine. The upside is that
 * routing-correctness tests (signal dispatch rules, Command/Send
 * override semantics, END_NODE termination) can be written against
 * Scheduler alone, without spinning up an engine or a graph of real
 * nodes. That matters because the scheduling rules are where the
 * predecessor-map → signal-dispatch refactor lived, and the class of
 * bug we hit today (XOR/AND conflation, dropped siblings) is best
 * caught by pure routing tests rather than by end-to-end runs.
 */
#pragma once

#include <neograph/graph/types.h>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neograph::graph {

class GraphState;

/**
 * @brief Per-node routing signal emitted by this super-step.
 *
 * One StepRouting per node that just executed. `command_goto` is set
 * iff the node returned a `Command` with a non-empty goto_node; the
 * Scheduler uses it to override regular edge routing. `command_goto`
 * == `END_NODE` terminates the graph.
 */
struct StepRouting {
    std::string node_name;
    std::optional<std::string> command_goto;
};

/**
 * @brief Plan for the next super-step.
 *
 * `ready` is the deduped list of nodes to execute (insertion-ordered
 * through std::set so parallel fan-in joins only once). `hit_end` is
 * set when any routing signal reached `END_NODE`. `winning_command_goto`
 * records the Command.goto_node that took over (if any) — purely for
 * debug streaming; the Scheduler itself does not use it downstream.
 */
struct NextStepPlan {
    std::vector<std::string> ready;
    bool hit_end = false;
    std::optional<std::string> winning_command_goto;
};

/// Per-barrier signal bookkeeping. Maps barrier node name → set of
/// upstream node names that have signaled it so far. The caller (engine)
/// owns this map and threads it through successive plan_next_step calls
/// so partial signal counts accumulate across super-steps.
///
/// Entries are cleared by the Scheduler when a barrier fires (so the
/// next round of signals starts fresh), and created lazily on first
/// signal.
using BarrierState = std::map<std::string, std::set<std::string>>;

/// Static barrier declarations extracted at graph-compile time. Maps
/// barrier node name → set of upstream node names that must ALL signal
/// before the barrier fires. Immutable after construction.
using BarrierSpecs = std::map<std::string, std::set<std::string>>;

/**
 * @brief Immutable routing oracle for a compiled graph.
 *
 * Scheduler is constructed once per engine compile and shared across
 * all runs on that engine. It reads (but never mutates) the edge
 * vectors passed at construction, so it is inherently thread-safe for
 * concurrent `plan_next_step` / `resolve_next_nodes` calls — provided
 * the caller-supplied `GraphState` is read-thread-safe (which
 * `GraphState` guarantees via its shared_mutex).
 */
class Scheduler {
public:
    /**
     * @brief Bind the scheduler to the graph's edge topology.
     * @param edges Regular (unconditional) edges.
     * @param conditional_edges Edges that route on a runtime condition.
     * @param barrier_specs Optional declaration of barrier nodes and
     *        their required upstream signal sets. Copied by value so
     *        caller ownership is simple.
     *
     * `edges` and `conditional_edges` must out-live the Scheduler —
     * typical usage has GraphEngine own them and pass references.
     */
    Scheduler(const std::vector<Edge>& edges,
              const std::vector<ConditionalEdge>& conditional_edges,
              BarrierSpecs barrier_specs = {});

    /**
     * @brief Resolve the direct successors of a node.
     *
     * If a conditional edge originates from `current`, the condition
     * is evaluated against `state` and the matching route is returned
     * as a single-element vector. If the route key is missing from the
     * routes map, the LAST entry (by map ordering) is used as a
     * fallback — matching the pre-refactor engine behavior.
     *
     * Otherwise, every regular edge with `from == current` contributes
     * its `to` to the result. If no edge matches at all, `{END_NODE}`
     * is returned so callers can uniformly check for termination.
     *
     * @param current Node whose successors to resolve.
     * @param state Snapshot used to evaluate any conditional-edge
     *              predicate originating from `current`.
     * @return Vector of direct successor node names, or `{END_NODE}`.
     */
    std::vector<std::string> resolve_next_nodes(
        const std::string& current, const GraphState& state) const;

    /**
     * @brief Nodes directly routed from `__start__` at step 0.
     * @return All `e.to` for edges with `from == START_NODE`, excluding
     *         any that target `END_NODE`.
     */
    std::vector<std::string> plan_start_step() const;

    /**
     * @brief Decide the next ready set from this super-step's routing.
     *
     * Rules:
     *   1. If any StepRouting carries a `command_goto`, that node wins —
     *      ALL regular edge routing from this super-step is discarded.
     *      When multiple Commands fire in the same super-step the LAST
     *      iteration's value wins (matching the engine's historical
     *      last-writer-wins behavior under parallel dispatch; note this
     *      is intrinsically non-deterministic under Taskflow).
     *   2. Otherwise, union `resolve_next_nodes(n, state)` for every n
     *      in `routings`. Any `END_NODE` occurrence trips `hit_end` and
     *      is excluded from `ready`.
     *   3. Deduplicate via std::set so parallel fan-in (two upstream
     *      nodes routing to the same downstream in the same super-step)
     *      executes that downstream exactly once.
     *
     * Note: this does NOT implement an implicit AND-join. A downstream
     * that is signalled across super-steps (asymmetric path lengths)
     * will fire once per reaching super-step — that is the documented
     * contract of signal dispatch.
     *
     * @param routings Signals from nodes that just executed.
     * @param state State snapshot to evaluate conditional edges against.
     */
    NextStepPlan plan_next_step(const std::vector<StepRouting>& routings,
                                const GraphState& state) const;

    /**
     * @brief Convenience overload: pair up ready[i] with results[i].
     *
     * The engine's super-step guarantees `results` is pushed in the
     * same order as `just_ran` in both the single- and parallel-node
     * paths. This overload bakes that pairing — plus the
     * `Command.goto_node` extraction — into the scheduler so the
     * engine doesn't have to restate the invariant at the call site
     * (where forgetting to zip correctly is the kind of mistake today's
     * multi-node checkpoint bug came from).
     *
     * `just_ran` and `results` must be the same size; mismatched
     * lengths throw std::invalid_argument rather than silently
     * truncating.
     *
     * @param just_ran Node names executed in this super-step, in order.
     * @param results NodeResults parallel to `just_ran`.
     * @param state State snapshot for conditional-edge evaluation.
     */
    NextStepPlan plan_next_step(const std::vector<std::string>& just_ran,
                                const std::vector<NodeResult>& results,
                                const GraphState& state) const;

    /**
     * @brief Barrier-aware planning.
     *
     * Identical to the 2-arg form, except any candidate listed in
     * `barrier_specs_` is gated on accumulated upstream signals. When
     * a candidate `b` would normally be added to `ready`, the scheduler:
     *
     *   1. Records every upstream node currently signaling `b` into
     *      `barrier_state[b]`.
     *   2. If `barrier_state[b] ⊇ barrier_specs_[b]`, the barrier has
     *      all required upstream signals → it fires this super-step
     *      (added to ready) and `barrier_state[b]` is CLEARED so the
     *      next round starts fresh (important for graphs that loop
     *      back through the barrier).
     *   3. Otherwise, the candidate is DEFERRED (not added to ready;
     *      signals remain in `barrier_state[b]` for the next call).
     *
     * Engines call this with a persistent `barrier_state` owned across
     * super-steps. Ephemeral callers (unit tests, single-shot routing
     * decisions) can use the 2-arg overload, which ignores
     * `barrier_specs_` entirely — there's no memory to gate against.
     *
     * Note: under the current implementation barrier state is
     * **per-run and in-memory only** — a checkpoint/resume round trip
     * drops all partial signals. See module docs for the follow-up
     * schema-versioned persistence plan.
     */
    NextStepPlan plan_next_step(const std::vector<StepRouting>& routings,
                                const GraphState& state,
                                BarrierState& barrier_state) const;

    /// NodeResult-taking barrier-aware overload.
    NextStepPlan plan_next_step(const std::vector<std::string>& just_ran,
                                const std::vector<NodeResult>& results,
                                const GraphState& state,
                                BarrierState& barrier_state) const;

    /// Read-only view of declared barriers (mainly for tests).
    const BarrierSpecs& barrier_specs() const { return barrier_specs_; }

private:
    const std::vector<Edge>& edges_;
    const std::vector<ConditionalEdge>& conditional_edges_;
    BarrierSpecs barrier_specs_;
};

} // namespace neograph::graph
