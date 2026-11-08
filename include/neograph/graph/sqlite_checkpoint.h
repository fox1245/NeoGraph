/**
 * @file graph/sqlite_checkpoint.h
 * @brief SQLite-backed CheckpointStore — single-file persistence for embedded
 *        and single-process deployments.
 *
 * Mirrors `PostgresCheckpointStore`'s schema and semantics so swapping the
 * backend is a one-line change at the call site:
 *
 *   auto store = std::make_shared<SqliteCheckpointStore>("/var/lib/neograph.db");
 *
 * vs. the Postgres variant:
 *
 *   auto store = std::make_shared<PostgresCheckpointStore>(
 *       "postgresql://user:pass@host/db");
 *
 * Both implement the same `CheckpointStore` interface with identical
 * dedup behaviour: channel values are stored once per (thread, channel,
 * version) triple via `INSERT ... ON CONFLICT DO NOTHING`. The schema
 * uses the same `neograph_*` table prefix so a sqlite_dump → psql
 * import migration path is conceivable (though not implemented).
 *
 * ## When to pick SQLite over Postgres
 *
 * SQLite wins when:
 *   - The deployment is single-process (no multi-host coordination).
 *   - The target is embedded / edge / a desktop CLI tool — no DB
 *     server to provision.
 *   - Operational simplicity beats raw concurrency: SQLite serialises
 *     writers but is faster than Postgres for low-throughput durable
 *     state because there's no network hop.
 *
 * Postgres wins when multiple agent processes share state, when
 * checkpoint volume is high enough that WAL + fsync cost matters, or
 * when an external operator wants to inspect/manage state with their
 * existing PG tooling.
 *
 * ## Concurrency
 *
 * The connection is wrapped in a mutex; every public method holds it
 * for the duration of its work. SQLite's own thread-safety mode is set
 * to "serialized" by default in libsqlite3, so this mutex is belt-and-
 * suspenders against concurrent statement preparation. WAL journal
 * mode is enabled at construction so reads don't block other reads.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

// Forward-declare so this header doesn't drag the sqlite3 C header
// into every TU that includes it.
struct sqlite3;

namespace neograph::graph {

/**
 * @brief Persistent CheckpointStore backed by a SQLite database file.
 *
 * Construct with a filesystem path. ":memory:" works too for tests.
 * The DB file is created if missing; the schema is materialised on
 * first connect via `CREATE TABLE IF NOT EXISTS`. A thread's checkpoints
 * are ordered by save (`save_seq`), as in the PostgreSQL store.
 */
class NEOGRAPH_API SqliteCheckpointStore : public CheckpointStore {
public:
    // Called after BEGIN IMMEDIATE has acquired the SQLite writer and before
    // any checkpoint mutation. Throw to roll the transaction back. Runs while
    // the store's write lock (`db_mutex_`) is held; the guard may inspect the
    // passed `sqlite3*` connection directly without starting another
    // transaction, and must not call back into the store.
    using WriteGuard = std::function<void(sqlite3*, const std::string& thread_id)>;
    // Called inside raise_fence while the store's write lock (`db_mutex_`) and
    // BEGIN IMMEDIATE transaction are held; must not call back into the store.
    using RaiseGuard = std::function<void(const std::string& owner_scope,
                                          const std::string& run_id,
                                          std::uint64_t min_fence)>;
    // Called before acquiring db_mutex_ / BEGIN IMMEDIATE on a checkpoint write.
    using BeforeWriteHook = std::function<void(const std::string& thread_id,
                                               const CheckpointWriteFence& fence)>;
    /// @param db_path Filesystem path or ":memory:". Anything sqlite3_open accepts.
    /// @throws std::runtime_error on open or DDL failure.
    explicit SqliteCheckpointStore(const std::string& db_path);
    /// Configure how long writes wait for a competing SQLite writer, and how
    /// long opening waits for another store or process opening or migrating
    /// the same database at the same moment.
    SqliteCheckpointStore(const std::string& db_path,
                          std::chrono::milliseconds busy_timeout);
    SqliteCheckpointStore(const std::string& db_path,
                          std::chrono::milliseconds busy_timeout,
                          WriteGuard write_guard);
    SqliteCheckpointStore(const std::string& db_path,
                          std::chrono::milliseconds busy_timeout,
                          WriteGuard write_guard,
                          RaiseGuard raise_guard);

    ~SqliteCheckpointStore() override;

    // sqlite3* is a unique resource — no copying or moving.
    SqliteCheckpointStore(const SqliteCheckpointStore&) = delete;
    SqliteCheckpointStore& operator=(const SqliteCheckpointStore&) = delete;

    bool supports_write_fence() const override { return true; }

    /// @note The guard runs while the store's write lock (`db_mutex_`) and
    ///       write transaction are held and must not call back into the store.
    void set_raise_guard(RaiseGuard raise_guard);
    void set_before_write_hook(BeforeWriteHook before_write_hook);

    void save(const Checkpoint& cp) override;
    void save(const Checkpoint& cp, const CheckpointWriteFence& fence) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;
    void delete_thread(const std::string& thread_id,
                       const CheckpointWriteFence& fence) override;

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write) override;
    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const PendingWrite& write,
                    const CheckpointWriteFence& fence) override;
    std::vector<PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override;
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id,
                      const CheckpointWriteFence& fence) override;

    std::uint64_t raise_fence(const std::string& owner_scope,
                              const std::string& run_id,
                              std::uint64_t min_fence) override;
    std::uint64_t load_fence(const std::string& owner_scope,
                             const std::string& run_id) override;

    /// Monotonic denial evidence, independent of retained checkpoint JSON.
    bool requires_managed_budget(const std::string& thread_id) override;
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override;

    std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override;
    std::shared_ptr<OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment, const CheckpointWriteFence& fence) override;
    asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> acquire_managed_budget_lease_async(
        ManagedBudgetLeaseScope scope, std::string expected_checkpoint_id,
        std::string expected_checkpoint_commitment) override;
    asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>> acquire_managed_budget_lease_async(
        ManagedBudgetLeaseScope scope, std::string expected_checkpoint_id,
        std::string expected_checkpoint_commitment, CheckpointWriteFence fence) override;
    ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override;
    ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest,
        const CheckpointWriteFence& fence) override;
    asio::awaitable<ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest) override;
    asio::awaitable<ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest,
        CheckpointWriteFence fence) override;
    void settle_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority) override;
    void settle_managed_budget_effect(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority,
        const CheckpointWriteFence& fence) override;
    asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
        sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority) override;
    asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
        sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority,
        CheckpointWriteFence fence) override;
    void publish_managed_budget_checkpoint(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) override;
    void publish_managed_budget_checkpoint(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint,
        const CheckpointWriteFence& fence) override;
    asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint) override;
    asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint,
        CheckpointWriteFence fence) override;
    void release_managed_budget_lease(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease) override;
    void release_managed_budget_lease(
        const std::shared_ptr<OwnedManagedBudgetLease>& lease,
        const CheckpointWriteFence& fence) override;
    asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease) override;
    asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<OwnedManagedBudgetLease> lease,
        CheckpointWriteFence fence) override;

    /// Drop and recreate checkpoint payload tables. Test-only utility.
    /// Managed-budget obligations, heads, effects and migration markers remain intact.
    void drop_schema();

    /// Number of distinct channel-value blobs currently held. Mirrors
    /// `InMemoryCheckpointStore::blob_count()` and
    /// `PostgresCheckpointStore::blob_count()` so dedup tests can be
    /// written against the abstract interface uniformly.
    size_t blob_count();

private:
    void ensure_schema();
    void exec_ddl(const char* sql);
    /// Caller holds db_mutex_ and the guarded writer transaction.
    void save_locked(const Checkpoint& checkpoint);
    void invoke_before_write_hook(const std::string& thread_id,
                                  const CheckpointWriteFence& fence);
    void invoke_raise_guard(const std::string& owner_scope,
                            const std::string& run_id,
                            std::uint64_t min_fence);

    /// RAII helper closes the connection on destruction. We hold the
    /// raw pointer so forward-declaration in the header works.
    sqlite3* db_ = nullptr;
    std::mutex db_mutex_;
    std::mutex hook_mutex_;
    WriteGuard write_guard_;
    RaiseGuard raise_guard_;
    BeforeWriteHook before_write_hook_;
};

} // namespace neograph::graph
