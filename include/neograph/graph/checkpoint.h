/**
 * @file graph/checkpoint.h
 * @brief Checkpoint system for graph execution state persistence and time-travel.
 *
 * Provides the Checkpoint data structure and the CheckpointStore interface
 * for saving and loading execution state snapshots. Used for HITL (Human-in-the-Loop)
 * interrupt/resume, time-travel debugging, and thread forking.
 */
#pragma once

#include <neograph/graph/types.h>
#include <optional>
#include <mutex>
#include <map>
#include <vector>
#include <string_view>
#include <chrono>

namespace neograph::graph {

/// Current Checkpoint layout version. Bump whenever the on-wire schema
/// changes in a way that would break a naive load of an older blob.
///
/// Version log:
///   1 — first versioned format. `next_nodes` is `vector<string>`
///       (previously a single `next_node` string; the string form is
///       unversioned and predates this constant).
constexpr int CHECKPOINT_SCHEMA_VERSION = 1;

/// Phase at which a Checkpoint was produced. Drives resume semantics —
/// `Before` means "re-enter before the target node runs", `After` /
/// `Completed` means "routing has already happened, advance from the
/// stored next_nodes", `NodeInterrupt` means "a node threw
/// NodeInterrupt mid-execution", `Updated` means "user patched state
/// out-of-band via update_state()".
enum class CheckpointPhase {
    Before,         ///< Saved just before an interrupt_before node fires.
    After,          ///< Saved just after an interrupt_after node completed.
    Completed,      ///< Saved at end of super-step (normal cadence).
    NodeInterrupt,  ///< Saved when a node threw NodeInterrupt.
    Updated         ///< Saved by update_state() injecting state externally.
};

/// @brief Canonical wire / log string for a CheckpointPhase.
///
/// The returned value is the same as the legacy stringly-typed phase
/// so persistent stores serializing with to_string() produce identical
/// blobs to pre-enum NeoGraph.
const char* to_string(CheckpointPhase phase);

/// @brief Parse a phase string back to the enum.
///
/// Useful for deserializing checkpoints from persistent stores. Unknown
/// strings throw std::invalid_argument — deliberate, because silent
/// fallback would mask wire-format drift.
CheckpointPhase parse_checkpoint_phase(std::string_view s);

/**
 * @brief Serialized snapshot of graph execution state at a single super-step.
 *
 * Each checkpoint captures the complete state of a graph execution,
 * including all channel values, the active node, and the next node
 * to execute. Checkpoints form a linked list via parent_id for
 * time-travel navigation.
 */
struct Checkpoint {
    std::string id;                ///< Unique checkpoint ID (UUID v4).
    std::string thread_id;         ///< Conversation/session identifier.
    json        channel_values;    ///< Serialized channel data.
    json        channel_versions;  ///< Per-channel version counters.
    std::string parent_id;         ///< Previous checkpoint ID (for time-travel chain).
    std::string current_node;      ///< Node that was active at checkpoint time.
    /// Nodes to execute on resume. With signal dispatch, a super-step can
    /// end with multiple nodes ready simultaneously (parallel fan-out,
    /// multiple conditional branches activating together); storing only
    /// one would silently drop siblings.
    std::vector<std::string> next_nodes;
    CheckpointPhase interrupt_phase = CheckpointPhase::Completed;  ///< Phase at which this cp was produced.
    json        metadata;          ///< User-defined metadata.
    int64_t     step;              ///< Super-step number.
    int64_t     timestamp;         ///< Unix epoch milliseconds.
    /// Layout version of this record. Persistent CheckpointStore impls
    /// should write it and inspect it on load: a value of 0 on a
    /// deserialized blob means "pre-versioned format" and may require
    /// migration (e.g. promoting a single next_node field into a one-
    /// element next_nodes vector). In-memory checkpoints created
    /// through the engine always carry CHECKPOINT_SCHEMA_VERSION.
    int         schema_version = CHECKPOINT_SCHEMA_VERSION;

    /**
     * @brief Generate a new UUID v4 string.
     * @return A random UUID v4 string (e.g., "550e8400-e29b-41d4-a716-446655440000").
     */
    static std::string generate_id();
};

/**
 * @brief Successful node writes recorded within an in-progress super-step.
 *
 * PendingWrite is the fine-grained progress log that lets NeoGraph resume
 * a partially completed super-step after a crash without re-executing
 * nodes that already succeeded. One PendingWrite corresponds to one
 * successful node execution; the engine records it immediately after the
 * node returns, before applying the writes to the shared GraphState.
 *
 * On resume, the engine loads all pending writes attached to the parent
 * checkpoint, replays them into GraphState, and skips any task whose
 * deterministic `task_id` is already present — so partial fan-out failures
 * only cost the failed node's re-execution, not its successful siblings.
 *
 * @see CheckpointStore::put_writes, CheckpointStore::get_writes
 */
struct PendingWrite {
    std::string task_id;        ///< Deterministic per-execution ID (survives replay).
    std::string task_path;      ///< Human-readable path, e.g. "s3:executor_2" or "s3:send[0]:searcher".
    std::string node_name;      ///< Node that produced these writes.
    json        writes;         ///< Serialized ChannelWrite vector (json array of {channel, value}).
    json        command;        ///< Serialized optional Command, or null if the node didn't emit one.
    json        sends;          ///< Serialized Send vector (json array of {target_node, input}); empty if none.
    int64_t     step;           ///< Super-step number this write belongs to.
    int64_t     timestamp;      ///< Unix epoch milliseconds at record time.
};

/**
 * @brief Abstract interface for checkpoint persistence backends.
 *
 * Implement this to store checkpoints in databases, files, or other
 * storage systems. The engine uses this interface for save/load operations.
 *
 * ## Pending writes (fine-grained progress log)
 *
 * The `put_writes` / `get_writes` / `clear_writes` family lets the engine
 * record each successful node execution *within* a super-step, so partial
 * fan-out failures can be resumed without re-running siblings. Custom
 * stores MAY leave the default no-op implementations in place — the engine
 * degrades gracefully to "full super-step replay" semantics, matching the
 * behavior of NeoGraph before this feature existed.
 *
 * Durability requirement: `put_writes` must be durable by the time it
 * returns (flushed to whatever backend the store wraps), because the
 * engine calls `state.apply_writes` only *after* `put_writes` succeeds.
 *
 * @see InMemoryCheckpointStore for a reference implementation.
 */
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    /**
     * @brief Save a checkpoint.
     * @param cp The checkpoint to persist.
     */
    virtual void save(const Checkpoint& cp) = 0;

    /**
     * @brief Load the most recent checkpoint for a thread.
     * @param thread_id Thread identifier.
     * @return The latest checkpoint, or std::nullopt if none exists.
     */
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;

    /**
     * @brief Load a checkpoint by its unique ID.
     * @param id Checkpoint UUID.
     * @return The checkpoint, or std::nullopt if not found.
     */
    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;

    /**
     * @brief List checkpoints for a thread, ordered by timestamp (newest first).
     * @param thread_id Thread identifier.
     * @param limit Maximum number of checkpoints to return (default: 100).
     * @return Vector of checkpoints.
     */
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100) = 0;

    /**
     * @brief Delete all checkpoints for a thread.
     * @param thread_id Thread identifier to delete.
     */
    virtual void delete_thread(const std::string& thread_id) = 0;

    // ── Pending writes (fine-grained progress log) ──────────────────────

    /**
     * @brief Record a successful node execution within an in-progress super-step.
     *
     * Called by the engine immediately after a node returns successfully
     * and *before* its writes are applied to the shared GraphState. The
     * parent_checkpoint_id anchors the pending write to the super-step
     * boundary it was produced under.
     *
     * Default implementation is a no-op so custom stores keep working;
     * such stores fall back to "full super-step replay" on resume.
     *
     * @param thread_id Thread identifier.
     * @param parent_checkpoint_id Checkpoint marking the start of the in-progress super-step.
     * @param write The pending write record to persist.
     */
    virtual void put_writes(const std::string& /*thread_id*/,
                            const std::string& /*parent_checkpoint_id*/,
                            const PendingWrite& /*write*/) {}

    /**
     * @brief Load all pending writes attached to a parent checkpoint.
     *
     * Called by the engine on resume to skip already-completed tasks.
     * Default implementation returns an empty vector.
     *
     * @param thread_id Thread identifier.
     * @param parent_checkpoint_id Checkpoint whose pending writes to load.
     * @return Vector of pending writes, in insertion order.
     */
    virtual std::vector<PendingWrite> get_writes(
        const std::string& /*thread_id*/,
        const std::string& /*parent_checkpoint_id*/) { return {}; }

    /**
     * @brief Discard pending writes for a parent checkpoint once its
     *        successor super-step has been fully committed.
     *
     * Called by the engine *after* the new super-step checkpoint has been
     * durably saved, so pending writes are never cleared while still being
     * the only record of a node's output.
     *
     * @param thread_id Thread identifier.
     * @param parent_checkpoint_id Checkpoint whose pending writes to clear.
     */
    virtual void clear_writes(const std::string& /*thread_id*/,
                              const std::string& /*parent_checkpoint_id*/) {}
};

/**
 * @brief In-memory checkpoint store for testing and single-process use.
 *
 * Stores checkpoints in memory using std::map. Thread-safe via mutex.
 * Not suitable for production use where persistence across restarts is needed.
 */
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write) override;
    std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override;

    /**
     * @brief Get the total number of stored checkpoints (test helper).
     * @return Total checkpoint count across all threads.
     */
    size_t size() const;

    /**
     * @brief Get the number of pending writes for a parent checkpoint (test helper).
     */
    size_t pending_writes_count(const std::string& thread_id,
                                const std::string& parent_checkpoint_id) const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Checkpoint>> by_thread_;
    std::map<std::string, Checkpoint> by_id_;
    // Keyed by (thread_id, parent_checkpoint_id) → ordered list of pending writes
    std::map<std::pair<std::string, std::string>, std::vector<PendingWrite>> pending_;
};

} // namespace neograph::graph
