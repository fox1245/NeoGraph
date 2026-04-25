/**
 * @file graph/coordinator.h
 * @brief CheckpointCoordinator — owns the super-step checkpoint lifecycle.
 *
 * Pulled out of GraphEngine::execute_graph so the three intertwined
 * concerns that used to live there — snapshot write, resume-context
 * load, and pending-writes log — each sit behind named methods with
 * their own invariants. The coordinator is a thin, per-run wrapper
 * over a CheckpointStore + thread_id pair:
 *
 *   * save_super_step: build a Checkpoint, write it, return its id.
 *   * load_for_resume: pull the latest cp plus every pending write, and
 *     return a ResumeContext the engine can apply wholesale.
 *   * record_pending_write / clear_pending_writes: fine-grained progress
 *     log for partial-failure recovery.
 *
 * When the backing store is nullptr or the thread_id is empty, every
 * method is a safe no-op (save returns "", load returns an empty
 * ResumeContext). Callers no longer need to guard on both conditions at
 * every call site.
 *
 * The coordinator does NOT touch admin APIs (get_state, update_state,
 * fork) — those don't have a super-step boundary and continue to use
 * CheckpointStore directly on GraphEngine.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/scheduler.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace neograph::graph {

class GraphState;

/**
 * @brief Context needed to resume execution from the last saved snapshot.
 *
 * Populated by CheckpointCoordinator::load_for_resume(). When `have_cp`
 * is false, the caller should treat this as a "nothing to resume from"
 * signal and fall through to a fresh run.
 */
struct ResumeContext {
    bool have_cp = false;

    std::string checkpoint_id;
    json channel_values;         ///< Serialized GraphState at cp time.
    int start_step = 0;          ///< Phase-adjusted step to re-enter at.
    CheckpointPhase phase = CheckpointPhase::Completed;
    std::vector<std::string> next_nodes;

    /// Partially-completed super-step writes, keyed by task_id. The
    /// engine replays any task whose id is in this map instead of
    /// re-executing the node.
    std::unordered_map<std::string, NodeResult> replay_results;

    /// Barrier accumulators for in-flight AND-joins. Present since
    /// schema v2; empty for v1 blobs.
    BarrierState barrier_state;
};

/**
 * @brief Per-run coordinator for checkpoint lifecycle operations.
 *
 * Value type; construct one at the top of execute_graph and let it go
 * out of scope when the run ends. Holds no state beyond the store
 * handle + thread_id.
 */
class NEOGRAPH_API CheckpointCoordinator {
public:
    /// @param store Checkpoint store (may be nullptr — everything is a no-op).
    /// @param thread_id Per-run thread identifier (may be empty — same effect).
    CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                          std::string thread_id);

    /// @return True iff a non-null store is wired up AND thread_id is non-empty.
    bool enabled() const noexcept { return store_ != nullptr && !thread_id_.empty(); }

    const std::shared_ptr<CheckpointStore>& store() const noexcept { return store_; }
    const std::string& thread_id() const noexcept { return thread_id_; }

    /**
     * @brief Write a super-step snapshot.
     * @return The new checkpoint id, or empty string when disabled.
     */
    std::string save_super_step(const GraphState& state,
                                const std::string& current_node,
                                const std::vector<std::string>& next_nodes,
                                CheckpointPhase phase,
                                int step,
                                const std::string& parent_id,
                                const BarrierState& barrier_state) const;

    /**
     * @brief Load the latest checkpoint + all pending writes attached to it.
     *
     * Computes the phase-adjusted start step: `before` / `node_interrupt`
     * re-enter AT cp.step (the node hadn't finished yet), `after` /
     * `completed` advance by +1 (routing already happened).
     */
    ResumeContext load_for_resume() const;

    /// Async peer of load_for_resume (Sem 3.7.5). Uses
    /// `load_latest_async` + `get_writes_async` on the store so the
    /// resume path doesn't block the io_context's worker.
    asio::awaitable<ResumeContext> load_for_resume_async() const;

    /**
     * @brief Durably record a completed node's writes under parent_cp_id.
     *
     * Must be called BEFORE the writes are applied to GraphState — a
     * crash between record and apply leaves a replayable log that
     * makes resume idempotent.
     */
    void record_pending_write(const std::string& parent_cp_id,
                              const std::string& task_id,
                              const std::string& task_path,
                              const std::string& node_name,
                              const NodeResult& nr,
                              int step) const;

    /**
     * @brief Drop the pending-writes log for a parent cp after its
     *        super-step has been successfully committed.
     *
     * Ordering matters: callers must have called save_super_step for a
     * fresh cp BEFORE invoking this. Clearing before saving would lose
     * data if a crash landed between the two calls.
     */
    void clear_pending_writes(const std::string& parent_cp_id) const;

    // ── Async peers (Stage 3 / Sem 3.6 incremental) ─────────────────────
    //
    // Each routes to the matching CheckpointStore::*_async so the
    // coroutine engine path doesn't block the io_context on
    // checkpoint I/O. Behaviour identical to the sync versions.

    asio::awaitable<std::string> save_super_step_async(
        const GraphState& state,
        const std::string& current_node,
        const std::vector<std::string>& next_nodes,
        CheckpointPhase phase,
        int step,
        const std::string& parent_id,
        const BarrierState& barrier_state) const;

    asio::awaitable<void> record_pending_write_async(
        const std::string& parent_cp_id,
        const std::string& task_id,
        const std::string& task_path,
        const std::string& node_name,
        const NodeResult& nr,
        int step) const;

    asio::awaitable<void> clear_pending_writes_async(
        const std::string& parent_cp_id) const;

private:
    std::shared_ptr<CheckpointStore> store_;
    std::string thread_id_;
};

} // namespace neograph::graph
