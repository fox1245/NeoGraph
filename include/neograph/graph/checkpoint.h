/**
 * @file graph/checkpoint.h
 * @brief Checkpoint system for graph execution state persistence and time-travel.
 *
 * Provides the Checkpoint data structure and the CheckpointStore interface
 * for saving and loading execution state snapshots. Used for HITL (Human-in-the-Loop)
 * interrupt/resume, time-travel debugging, and thread forking.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/types.h>

#include <asio/awaitable.hpp>

#include <cstdint>

#include <memory>
#include <optional>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <chrono>

namespace neograph::graph {
class NativeGraphCheckpoint;
struct NodeResult;
struct ChannelWrite;
namespace detail { class ManagedBudgetJournalAccess; }

struct ManagedBudgetLeaseScope {
    std::string owner_scope;
    std::string thread_id;
    std::string storage_thread_id;
    std::string graph_identity;
    std::uint64_t original_ceiling = 0;
    std::optional<std::int64_t> original_deadline_ticks;
    std::string deadline_clock_identity;
};

/// Store-issued ownership; JSON or an archived observation cannot create it.
class NEOGRAPH_API OwnedManagedBudgetLease final {
public:
    const ManagedBudgetLeaseScope& scope() const noexcept;
    const std::string& actor_id() const noexcept;
    const std::string& bank_generation() const noexcept;
    std::uint64_t revision() const noexcept;
    std::string head_checkpoint_id() const;
    std::string head_commitment() const;
    const std::string& execution_thread_id() const noexcept;
    const std::string& execution_storage_thread_id() const noexcept;
private:
    struct Impl;
    explicit OwnedManagedBudgetLease(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend class detail::ManagedBudgetJournalAccess;
};

/// Actual write-ahead claim, bound to one original generation and owned actor.
class NEOGRAPH_API ManagedBudgetEffectReceipt final {
public:
    ManagedBudgetEffectReceipt() = default;
    bool active() const noexcept;
    const std::string& effect_id() const;
    std::uint64_t claim_amount() const;
    const std::string& request_digest() const;
private:
    struct Impl;
    explicit ManagedBudgetEffectReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
    friend class detail::ManagedBudgetJournalAccess;
};

/// Current Checkpoint layout version. Bump whenever the on-wire schema
/// changes in a way that would break a naive load of an older blob.
///
/// Version log:
///   1 — first versioned format. `next_nodes` is `vector<string>`
///       (previously a single `next_node` string; the string form is
///       unversioned and predates this constant).
///   2 — added `barrier_state`: per-barrier accumulator of upstream
///       signals received so far. v1 blobs deserialize with an empty
///       map, which is safe — a barrier that had partial signals under
///       v1 would have lost them anyway (the pre-v2 contract), so the
///       v2 engine simply resumes with zero accumulated signals and
///       waits for the full set again.
// Unsigned + fixed-width: schema versions are non-negative and a
// platform-variable `int` width is wrong for a value persisted to
// disk and round-tripped through JSON. Old `int` left the door open
// to a `-1` sentinel value that wasn't documented anywhere.
// v3 (#91): pending-write records may carry a "mode" field ("overwrite").
// Absent means Reduce, so a v2 blob loads unchanged — same tolerant shape as the
// v1 -> v2 barrier_state addition. Nothing rejects an older version on read; the
// number records what the writer was capable of.
// v4 (#235): child checkpoints may carry the cumulative subgraph write journal
// in metadata. Standalone checkpoints remain readable as before; resuming a
// captured subgraph from an older checkpoint fails explicitly because an
// arbitrary custom reducer cannot reconstruct its historical deltas.
constexpr std::uint32_t CHECKPOINT_SCHEMA_VERSION = 4;

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
NEOGRAPH_API const char* to_string(CheckpointPhase phase);

/// @brief Parse a phase string back to the enum.
///
/// Useful for deserializing checkpoints from persistent stores. Unknown
/// strings throw std::invalid_argument — deliberate, because silent
/// fallback would mask wire-format drift.
NEOGRAPH_API CheckpointPhase parse_checkpoint_phase(std::string_view s);

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
    /// Per-barrier accumulator: each entry maps a declared barrier node
    /// to the set of upstreams that have signaled it so far. Persists
    /// across super-steps for barriers that haven't yet reached their
    /// `wait_for` set. The Scheduler clears an entry when its barrier
    /// fires, so this map only ever contains in-flight (partial) state.
    ///
    /// Shape matches `BarrierState` from scheduler.h; kept as raw map
    /// here to avoid pulling the scheduler header into every checkpoint
    /// consumer.
    std::map<std::string, std::set<std::string>> barrier_state;
    json        metadata;          ///< User-defined metadata.
    /// Original trusted C++ custody only; never reconstructed from checkpoint JSON.
    std::shared_ptr<const NativeGraphCheckpoint> native_history;
    std::shared_ptr<const std::vector<ChannelWrite>> native_subgraph_writes;
    int64_t     step;              ///< Super-step number.
    int64_t     timestamp;         ///< Unix epoch milliseconds.
    /// Layout version of this record. Persistent CheckpointStore impls
    /// should write it and inspect it on load: a value of 0 on a
    /// deserialized blob means "pre-versioned format" and may require
    /// migration (e.g. promoting a single next_node field into a one-
    /// element next_nodes vector). In-memory checkpoints created
    /// through the engine always carry CHECKPOINT_SCHEMA_VERSION.
    std::uint32_t schema_version = CHECKPOINT_SCHEMA_VERSION;

    /**
     * @brief Generate a new UUID v4 string.
     * @return A random UUID v4 string (e.g., "550e8400-e29b-41d4-a716-446655440000").
     */
    static NEOGRAPH_API std::string generate_id();
};

NEOGRAPH_API std::string managed_budget_checkpoint_commitment(const Checkpoint& checkpoint);
NEOGRAPH_API bool checkpoint_channel_blob_eligible(const json& channel);
NEOGRAPH_API json checkpoint_storage_shape(const Checkpoint& checkpoint);
NEOGRAPH_API void restore_checkpoint_storage_shape(Checkpoint& checkpoint, const json& shape,
    const std::map<std::string, json>& blobs);

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
    std::shared_ptr<const NodeResult> native_result;
};

/// Preserve the complete durable provider-state envelope without changing store schemas.
NEOGRAPH_API json checkpoint_storage_metadata(const Checkpoint& checkpoint);
NEOGRAPH_API void restore_checkpoint_storage_envelope(Checkpoint& checkpoint);

/**
 * @brief Mandatory synchronous operations for a sync-only backend.
 *
 * Adapt this pure contract with adapt_checkpoint_store() to offload blocking
 * I/O outside the engine executor. The legacy CheckpointStore remains an ABI
 * compatibility facade for existing backends and consumers.
 */
class NEOGRAPH_API CheckpointStoreCore {
public:
    virtual ~CheckpointStoreCore() = default;

    virtual void save(const Checkpoint& cp) = 0;
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;
    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                         int limit = 100) = 0;
    virtual void delete_thread(const std::string& thread_id) = 0;

    /// A persistent denial obligation, never authority to spend or restore.
    /// Unsupported backends fail explicitly; absence must not be guessed.
    virtual bool requires_managed_budget(const std::string& thread_id);
    virtual std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment);
    virtual ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest);
    virtual void settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority);
    virtual void publish_managed_budget_checkpoint(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint);
    virtual void release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
    /// Storage retention only; never currency or native replay authority.
    virtual bool retains_native_checkpoint() const noexcept { return false; }
    virtual void publish_managed_budget_fork(
        const Checkpoint& authenticated_source, const Checkpoint& genuine_shared_bank_fork);
};

/**
 * @brief Canonical coroutine contract for a native async backend.
 *
 * Implement all five operations and pass it through
 * adapt_async_checkpoint_store() for synchronous administration.
 */
class NEOGRAPH_API AsyncCheckpointStore {
public:
    virtual ~AsyncCheckpointStore() = default;

    virtual asio::awaitable<void> save_async(const Checkpoint& cp) = 0;
    virtual asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) = 0;
    virtual asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) = 0;
    virtual asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit = 100) = 0;
    virtual asio::awaitable<void>
    delete_thread_async(const std::string& thread_id) = 0;

    virtual asio::awaitable<bool> requires_managed_budget_async(std::string thread_id);
    virtual asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> acquire_managed_budget_lease_async(
        ManagedBudgetLeaseScope scope, std::string expected_checkpoint_id,
        std::string expected_checkpoint_commitment);
    virtual asio::awaitable<ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest);
    virtual asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
        sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority);
    virtual asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint);
    virtual asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease);
    virtual bool retains_native_checkpoint() const noexcept { return false; }
    virtual asio::awaitable<void> publish_managed_budget_fork_async(
        Checkpoint authenticated_source, Checkpoint genuine_shared_bank_fork);
};

/**
 * @brief Optional fine-grained pending-writes capability.
 */
class NEOGRAPH_API PendingWritesCheckpointStore {
public:
    virtual ~PendingWritesCheckpointStore() = default;

    virtual void put_writes(const std::string& thread_id,
                            const std::string& parent_checkpoint_id,
                            const PendingWrite& write) = 0;
    virtual std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) = 0;
    virtual void clear_writes(const std::string& thread_id,
                              const std::string& parent_checkpoint_id) = 0;
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
 * ## Thread safety
 *
 * **Individual ops are atomic; cross-op sequencing is the caller's
 * responsibility.** Every backend in-tree (`InMemoryCheckpointStore`
 * mutex-guards each call; `PostgresCheckpointStore` /
 * `SqliteCheckpointStore` use per-call transactions) guarantees that
 * a single `save_checkpoint` / `load_latest` / `put_writes` /
 * `delete_thread` call observes a consistent snapshot — no torn
 * reads, no half-applied writes. They do NOT serialize *sequences*
 * of calls for the same `thread_id`. Concurrent runs against the same
 * `thread_id` therefore exhibit last-writer-wins on `save_checkpoint`
 * and last-saver visibility on subsequent `load_latest` — see
 * `GraphEngine`'s class-level Thread safety section for the engine-
 * side contract. Backend authors implementing this interface MUST
 * preserve the per-call-atomic invariant; callers needing
 * cross-op atomicity (e.g. compare-and-set on the latest checkpoint)
 * must wrap the relevant call sequence behind their own external
 * mutex, or use the engine's `cancel_token` to drain in-flight runs
 * before issuing an admin op.
 *
 * @see InMemoryCheckpointStore for a reference implementation.
 */
/// @note This legacy interface is retained through the pre-v1 ABI window.
///       New sync backends implement CheckpointStoreCore and call
///       adapt_checkpoint_store(); native async backends implement
///       AsyncCheckpointStore and call adapt_async_checkpoint_store().
///       Legacy sync defaults fail explicitly, while async defaults offload
///       sync operations. No sync default calls its async peer.
class NEOGRAPH_API CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    // ── Sync API ────────────────────────────────────────────────────────
    //
    // Legacy sync facade. Override sync operations or explicitly adapt a
    // native AsyncCheckpointStore. Missing sync operations fail explicitly;
    // async defaults offload sync-only backends to a bounded worker pool.

    /**
     * @brief Save a checkpoint.
     * @param cp The checkpoint to persist.
     */
    virtual void save(const Checkpoint& cp);

    /**
     * @brief Load the most recent checkpoint for a thread.
     * @param thread_id Thread identifier.
     * @return The latest checkpoint, or std::nullopt if none exists.
     */
    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id);

    /**
     * @brief Load a checkpoint by its unique ID.
     * @param id Checkpoint UUID.
     * @return The checkpoint, or std::nullopt if not found.
     */
    virtual std::optional<Checkpoint> load_by_id(const std::string& id);

    /**
     * @brief List checkpoints for a thread, ordered by timestamp (newest first).
     * @param thread_id Thread identifier.
     * @param limit Maximum number of checkpoints to return (default: 100).
     * @return Vector of checkpoints.
     */
    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100);

    /**
     * @brief Delete all checkpoints for a thread.
     * @param thread_id Thread identifier to delete.
     */
    virtual void delete_thread(const std::string& thread_id);

    // Async defaults offload the matching synchronous operation instead of
    // blocking the caller's Asio executor. Async-native stores should override
    // these methods to avoid the blocking pool entirely.

    virtual asio::awaitable<void> save_async(const Checkpoint& cp);
    virtual asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id);
    virtual asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id);
    virtual asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit = 100);
    virtual asio::awaitable<void>
    delete_thread_async(const std::string& thread_id);

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

    // Async peers for the pending-writes API. Each defaults to calling
    // the matching sync method (which itself is a no-op for stores that
    // don't override it), so existing custom stores keep working.

    virtual asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write);
    virtual asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);
    virtual asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id);

    /// Whether this trusted checkpoint namespace/thread has ever held an
    /// active standalone managed bank. Saving stripped state or deleting
    /// checkpoints must not clear the obligation. This flag grants no currency;
    /// the original complete bank must still authenticate before restoration.
    /// Unsupported backends throw rather than authorize an absent bank.
    virtual bool requires_managed_budget(const std::string& thread_id);
    virtual asio::awaitable<bool> requires_managed_budget_async(std::string thread_id);

    virtual std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment);
    virtual asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> acquire_managed_budget_lease_async(
        ManagedBudgetLeaseScope scope, std::string expected_checkpoint_id,
        std::string expected_checkpoint_commitment);
    virtual ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest);
    virtual asio::awaitable<ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest);
    virtual void settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority);
    virtual asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
        sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority);
    virtual void publish_managed_budget_checkpoint(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint);
    virtual asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint);
    virtual void release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
    virtual asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease);
    /// Describes real local C++ custody, not a financial admission/grant.
    virtual bool retains_native_checkpoint() const noexcept { return false; }
    virtual void publish_managed_budget_fork(
        const Checkpoint& authenticated_source, const Checkpoint& genuine_shared_bank_fork);
    virtual asio::awaitable<void> publish_managed_budget_fork_async(
        Checkpoint authenticated_source, Checkpoint genuine_shared_bank_fork);
};

/**
 * @brief Adapt split checkpoint capabilities to the legacy engine contract.
 *
 * The returned object keeps @p core alive. If the same implementation also
 * derives from AsyncCheckpointStore or PendingWritesCheckpointStore, the
 * adapter detects and delegates those optional capabilities. If it already
 * implements CheckpointStore, the original shared object is returned. Without
 * AsyncCheckpointStore, async calls run the synchronous core operation on the
 * shared bounded blocking pool and resume on the caller's executor;
 * I/O-bound backends should implement the async capability.
 *
 * @throws std::invalid_argument If core is null.
 */
NEOGRAPH_API std::shared_ptr<CheckpointStore>
adapt_checkpoint_store(std::shared_ptr<CheckpointStoreCore> core);

/** Explicit sync facade for an async-native backend. The adapter retains the
 * backend and forwards engine async operations without a blocking worker;
 * synchronous administration drives them via run_sync. Pending-write storage
 * is optional via PendingWritesCheckpointStore.
 */
NEOGRAPH_API std::shared_ptr<CheckpointStore>
adapt_async_checkpoint_store(std::shared_ptr<AsyncCheckpointStore> backend);

/**
 * @brief In-memory checkpoint store for testing and single-process use.
 *
 * Stores checkpoints in memory using std::map. Thread-safe via mutex.
 * Not suitable for production use where persistence across restarts is needed.
 *
 * ## Incremental storage (channel-blob deduplication)
 *
 * Channel values are deduplicated internally by `(thread_id, channel,
 * version)` — every write bumps `Channel::version`, so the same value at
 * the same version across multiple checkpoints is stored exactly once.
 * In a typical run only one or two channels change per super-step, so a
 * 1000-step session pays roughly `(channels + steps) × value_size`
 * instead of `channels × steps × value_size`.
 *
 * The dedup is purely an internal storage optimization: callers always
 * receive (and pass in) Checkpoints whose `channel_values` carry full
 * inline data. Engine code, tests, and migration paths are unaffected.
 *
 * Persistent CheckpointStore implementations are encouraged to apply
 * the same pattern in their own backends (e.g. a `(channel, version) →
 * blob` table referenced from a `(checkpoint_id, channel) → version`
 * table) to get the same on-disk savings.
 */
class NEOGRAPH_API InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                 int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    // The exact in-memory concrete type has no I/O boundary, so its async
    // operations stay on the caller executor instead of paying a legacy
    // blocking-pool handoff for each mutex-protected map operation. Derived
    // stores retain the base fallback unless they explicitly override async.
    asio::awaitable<void> save_async(const Checkpoint& cp) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override;
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit = 100) override;
    asio::awaitable<void>
    delete_thread_async(const std::string& thread_id) override;

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write) override;
    std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override;

    asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const PendingWrite& write) override;
    asio::awaitable<std::vector<PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;

    bool requires_managed_budget(const std::string& thread_id) override;
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override;
    std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override;
    ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override;
    void settle_managed_budget_effect(const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority) override;
    void publish_managed_budget_checkpoint(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) override;
    void release_managed_budget_lease(const std::shared_ptr<OwnedManagedBudgetLease>& lease) override;
    bool retains_native_checkpoint() const noexcept override { return true; }
    void publish_managed_budget_fork(
        const Checkpoint& authenticated_source, const Checkpoint& genuine_shared_bank_fork) override;

    /**
     * @brief Get the total number of stored checkpoints (test helper).
     * @return Total checkpoint count across all threads.
     */
    size_t size() const;

    /**
     * @brief Number of distinct channel-value blobs currently held (test helper).
     *
     * Use this to verify dedup: writing N identical-state checkpoints
     * leaves blob_count() at one entry per channel, not N × channels.
     */
    size_t blob_count() const;

    /**
     * @brief Get the number of pending writes for a parent checkpoint (test helper).
     */
    size_t pending_writes_count(const std::string& thread_id,
                                const std::string& parent_checkpoint_id) const;

private:
    /// Strip values out of `cp.channel_values["channels"][n]["value"]`,
    /// store them in `blobs_` keyed by (thread_id, channel, version),
    /// leave the rest of the cp shell intact. Idempotent on the blob
    /// map — duplicate puts at the same key are dropped silently.
    /// MUST be called with `mutex_` held.
    Checkpoint split_blobs_locked(Checkpoint cp);

    /// Inverse of split_blobs_locked: walk the cp's channel pointers
    /// and copy values back from `blobs_` so the returned Checkpoint
    /// looks identical to what the caller originally passed to save().
    /// Channels whose blob is missing get a null `value` (defensive —
    /// indicates either a v1/v2 legacy blob or store corruption).
    /// MUST be called with `mutex_` held.
    Checkpoint join_blobs_locked(Checkpoint cp) const;

    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Checkpoint>> by_thread_;  ///< holds shells (no inline values)
    std::map<std::string, Checkpoint> by_id_;                   ///< holds shells (no inline values)
    /// Deduplicated channel values. A channel value at a given (thread,
    /// channel, version) is identical across every cp that references
    /// it, so a single entry serves all of them.
    std::map<std::tuple<std::string, std::string, uint64_t>, json> blobs_;
    // Keyed by (thread_id, parent_checkpoint_id) → ordered list of pending writes
    std::map<std::pair<std::string, std::string>, std::vector<PendingWrite>> pending_;
    std::unordered_set<std::string> managed_budget_obligations_;
    std::unordered_map<std::string, json> managed_budget_journals_;
    std::map<std::tuple<std::string, std::string, std::string>, json> managed_budget_effects_;
    std::map<std::tuple<std::string, std::string, std::string>, sp::runtime::Result> managed_budget_results_;
    struct ManagedBudgetBranchHead {
        std::string financial_storage_thread_id;
        std::string execution_thread_id;
        std::string checkpoint_id;
        std::string commitment;
        std::shared_ptr<const NativeGraphCheckpoint> native_history;
        bool valid = true;
    };
    const ManagedBudgetBranchHead& authenticate_managed_budget_branch_locked(
        const Checkpoint& source, const std::string& commitment) const;
    void validate_managed_budget_execution_locked(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const json& head) const;
    std::unordered_map<std::string, ManagedBudgetBranchHead> managed_budget_branch_heads_;
};

} // namespace neograph::graph
