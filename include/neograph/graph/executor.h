/**
 * @file graph/executor.h
 * @brief NodeExecutor — owns per-super-step node invocation.
 *
 * Extracted from GraphEngine::execute_graph so the three invocation
 * paths that used to be open-coded inside the super-step loop sit
 * behind named methods with their own invariants:
 *
 *   * run_one_async — single-node path. Handles replay lookup, retry,
 *     pending-write recording, state.apply_writes, trace append, and
 *     (on NodeInterrupt) a phase=NodeInterrupt checkpoint save via the
 *     coordinator. Rethrows the interrupt after the save.
 *   * run_parallel_async — fan-out via
 *     `asio::experimental::make_parallel_group`. Records per-worker
 *     pending writes, captures the first worker exception, and
 *     rethrows after every branch has finished. After the barrier,
 *     applies each result's writes + Command.updates to the shared
 *     state.
 *   * run_sends_async — dynamic fan-out. Single-send path runs on the
 *     shared state with retry; multi-send path gives each target an
 *     isolated state copy (fresh init + restore + input apply) and no
 *     retry, preserving the pre-3.0 behavior exactly.
 *
 * The executor owns `execute_node_with_retry_async` — the innermost
 * retry loop with exponential backoff + NodeInterrupt pass-through.
 * Retry policies are resolved per node via a lookup callback supplied
 * by GraphEngine at construction; the executor itself is agnostic
 * about where the policy came from.
 *
 * Thread safety: all fan-out runs on whichever executor `co_await
 * asio::this_coro::executor` yields — typically GraphEngine's owned
 * thread_pool. Multiple concurrent executions against the same
 * NodeExecutor instance are safe iff the underlying GraphNode
 * subclasses are safe.
 *
 * 3.0 removed the sync `run_one`/`run_parallel`/`run_sends` twins and
 * their process-wide Taskflow executor; sync callers drive the async
 * peers via GraphEngine's thread_pool (see engine.cpp).
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/graph/node.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/coordinator.h>
#include <neograph/graph/scheduler.h>

#include <asio/thread_pool.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace neograph::graph {

class GraphState;

/**
 * @brief Stateless-per-call node invocation dispatcher.
 *
 * Holds only static configuration: the node map, the channel-def
 * layout (needed to re-init isolated Send states), and a retry-policy
 * lookup. Every dynamic concern — GraphState, CheckpointCoordinator,
 * replay map, step index, streaming callback — flows in through method
 * parameters so tests can drive individual invocations without
 * reconstructing the surrounding super-step.
 */
class NEOGRAPH_API NodeExecutor {
public:
    using RetryPolicyLookup = std::function<RetryPolicy(const std::string&)>;

    /// @param nodes Map of node_name → GraphNode owned by the engine.
    ///              Must outlive this executor (engine owns both).
    /// @param channel_defs Channel layout used to initialize isolated
    ///                     states for multi-Send execution.
    /// @param retry_policy_for Lookup called for each retry run; the
    ///                         executor has no opinion about fallback
    ///                         behavior — the callback is free to
    ///                         return a default policy.
    /// @param fan_out_pool Optional non-owning pool used for parallel
    ///                    fan-out (run_parallel_async and multi-Send
    ///                    run_sends_async). If nullptr, branches run
    ///                    on the current coroutine's executor — fine
    ///                    for single-thread async callers, but
    ///                    serializes CPU-bound fan-out.
    NodeExecutor(
        const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
        const std::vector<ChannelDef>& channel_defs,
        RetryPolicyLookup retry_policy_for,
        asio::thread_pool* fan_out_pool = nullptr);

    /**
     * @brief Execute a single node in the current super-step.
     *
     * On success: applies writes + command.updates to state, appends
     * node_name to trace, returns the NodeResult. Records a pending
     * write via coord BEFORE state.apply_writes, so a crash between
     * record and apply still leaves a replayable log.
     *
     * On NodeInterrupt: saves a phase=NodeInterrupt checkpoint with
     * barrier_state via coord, then rethrows.
     * On any other exception: propagates after the retry policy is
     * exhausted (execute_node_with_retry_async handles the retry loop
     * internally). Node dispatch and checkpoint I/O flow through
     * co_await so other coroutines on the same executor keep moving.
     */
    asio::awaitable<NodeResult> run_one_async(
        const std::string& node_name,
        int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb,
        StreamMode stream_mode);

    /// Inner retry loop with exponential backoff + NodeInterrupt
    /// short-circuit. Drives node->execute_full_(stream_)async via
    /// co_await and uses asio::steady_timer.async_wait for backoff so
    /// the executor is not frozen during retry waits. NodeInterrupt +
    /// exception semantics preserved bit-for-bit; GCC-13-safe (catch
    /// block captures the exception via std::optional, co_await
    /// happens outside).
    ///
    /// Public so regression tests can drive it directly without
    /// reconstructing a full super-step.
    asio::awaitable<NodeResult> execute_node_with_retry_async(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb,
        StreamMode stream_mode);

    /**
     * @brief Execute all `ready` nodes concurrently via
     * `asio::experimental::make_parallel_group` + wait_for_all.
     *
     * After every branch has finished: if any threw, the first
     * exception is rethrown; otherwise writes + command.updates from
     * each result are applied to the shared state in `ready` order,
     * trace is appended in `ready` order, and results are returned
     * in `ready` order so the caller can pair them with scheduler
     * routing decisions.
     *
     * If the first thrown exception is a NodeInterrupt, the offending
     * node's name is captured and a phase=NodeInterrupt checkpoint is
     * saved with `next_nodes={interrupted_node}` before rethrow —
     * matching run_one_async's behavior so resume re-enters on just
     * the interrupting node (replay skips the siblings that already
     * completed via pending_writes).
     */
    asio::awaitable<std::vector<NodeResult>> run_parallel_async(
        const std::vector<std::string>& ready,
        int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb,
        StreamMode stream_mode);

    /**
     * @brief Execute a list of Send requests accumulated this step.
     *
     * Single-send path: the target runs on the shared state with
     * retry and the Send.input is applied to that state. Multi-send
     * path: each target runs on an isolated state copy (fresh init +
     * restore + apply_input) via
     * `asio::experimental::make_parallel_group` on deferred workers,
     * without retry, and the writes are fanned back into the shared
     * state after wait_for_all.
     *
     * Returns one StepRouting per Send-spawned task, in send order.
     * The caller merges these with the original ready-set's routings
     * before invoking the Scheduler so that per-task `Command.goto`
     * and the Send target's default outgoing edges flow into the next
     * super-step's routing decision (LangGraph parity).
     */
    asio::awaitable<std::vector<StepRouting>> run_sends_async(
        const std::vector<Send>& sends,
        int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb,
        StreamMode stream_mode);

private:
    /// Initialize a clean GraphState using the engine's channel defs.
    /// Used by the multi-send path to build each target's isolated copy.
    void init_state(GraphState& state) const;

    /// Apply a JSON object of channel writes (by key) to an existing
    /// state. Silently skips unknown channel names.
    void apply_input(GraphState& state, const json& input) const;

    const std::map<std::string, std::unique_ptr<GraphNode>>& nodes_;
    const std::vector<ChannelDef>& channel_defs_;
    RetryPolicyLookup retry_policy_for_;
    asio::thread_pool* fan_out_pool_ = nullptr;
};

} // namespace neograph::graph
