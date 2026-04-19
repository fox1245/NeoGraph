/**
 * @file graph/postgres_checkpoint.h
 * @brief PostgreSQL-backed CheckpointStore — durable, multi-process,
 *        blob-deduplicated, connection-pooled.
 *
 * Mirrors LangGraph's PostgresSaver schema so a single PG database can
 * host NeoGraph and LangGraph state side-by-side (NeoGraph uses a
 * `neograph_` table prefix to avoid collisions with LangGraph's
 * unprefixed tables, which carry a different column shape).
 *
 * ## Schema
 *
 * Three tables, all auto-created on first use via `ensure_schema()`:
 *
 *   - `neograph_checkpoints` — cp metadata + per-channel version map.
 *     Channel VALUES are NOT stored here (just versions).
 *   - `neograph_checkpoint_blobs` — `(thread_id, channel, version) →
 *     value`. Identical (channel, version) writes collapse via
 *     `ON CONFLICT DO NOTHING`, so a long-running thread that touches
 *     only a few channels per super-step pays linear blob storage in
 *     distinct writes, not in steps × channels.
 *   - `neograph_checkpoint_writes` — pending intra-super-step writes,
 *     keyed `(thread_id, parent_checkpoint_id, task_id, seq)`.
 *
 * ## Concurrency — connection pool
 *
 * The store owns a **fixed-size pool** of libpqxx connections sized by
 * the constructor's `pool_size` parameter (default 8). Each method
 * acquires one connection for its work and releases it back to the
 * pool when done. With N workers ≤ pool_size, there is no
 * serialisation: each worker commits in parallel, scaling roughly
 * linearly until PG itself (WAL fsync, lock contention) becomes the
 * limit.
 *
 * Why a pool and not a single mutex'd connection: PG commit is
 * dominated by `synchronous_commit=on` WAL fsync (~10ms per commit).
 * One connection serializes those into a sequential queue and caps
 * throughput at ~70 saves/sec. A pool of 8 lets 8 commits flush
 * concurrently — measured ~6× speedup at 8 worker threads.
 *
 * Pool sizing guidance:
 *   - Embedded / single-process: `pool_size = 1` is fine.
 *   - Server / multi-tenant: match your worker thread count, capped
 *     at `max_connections` on the PG side (PG default 100; pgbouncer
 *     in front is recommended for very large pools).
 *
 * ## Failure model
 *
 * `pqxx::broken_connection` (PG restart, network blip, pgbouncer idle
 * timeout) is caught per-slot: the dead connection is replaced with a
 * fresh one (using the original connection string) and the operation
 * is retried once. `reconnect_count()` exposes the cumulative number
 * of slot replacements for monitoring. Other libpqxx exceptions
 * (constraint violations, query errors) propagate as-is.
 */
#pragma once

#include <neograph/graph/checkpoint.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

// Forward-declare libpqxx types so this header doesn't drag pqxx into
// every translation unit that includes it.
namespace pqxx { class connection; }

namespace neograph::graph {

/**
 * @brief Persistent CheckpointStore backed by PostgreSQL via libpqxx.
 *
 * Construct with a libpq connection string (e.g.
 * `"postgresql://user:pass@host:5432/dbname"`). The constructor opens
 * the connection eagerly and runs `ensure_schema()` so callers get
 * an immediate failure if credentials or DDL permissions are wrong,
 * rather than a delayed surprise on first save().
 */
class PostgresCheckpointStore : public CheckpointStore {
public:
    /// @param conn_str libpq connection string. Anything libpq accepts.
    /// @param pool_size Number of connections to open eagerly. Defaults
    ///        to 8 — a sensible match for typical small server worker
    ///        pools. Set to 1 for embedded / single-thread use to save
    ///        one PG backend per store. Must be >= 1.
    /// @throws std::runtime_error on connection or DDL failure.
    explicit PostgresCheckpointStore(const std::string& conn_str,
                                      size_t pool_size = 8);

    /// Number of times a pool slot's connection was replaced after a
    /// `pqxx::broken_connection` failure. Cumulative across the whole
    /// store; useful for monitoring (e.g. Prometheus gauge) and for
    /// tests that want to assert the retry path fired.
    size_t reconnect_count() const { return reconnect_count_; }

    /// Number of connections in the pool. Useful for benchmarks and
    /// for confirming the pool was sized as expected.
    size_t pool_size() const { return pool_.size(); }

    ~PostgresCheckpointStore() override;

    // Non-copyable, non-movable — pqxx::connection isn't copyable and the
    // mutex would need rebinding on move.
    PostgresCheckpointStore(const PostgresCheckpointStore&) = delete;
    PostgresCheckpointStore& operator=(const PostgresCheckpointStore&) = delete;

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

    /// Drop all `neograph_*` tables. Test-only utility — destroys data.
    /// Useful in test fixtures that want a clean slate per test case.
    void drop_schema();

    /// Test helper: count rows in `neograph_checkpoint_blobs`. Lets
    /// integration tests verify blob dedup across saves the same way
    /// `InMemoryCheckpointStore::blob_count()` does.
    size_t blob_count();

private:
    void ensure_schema();

    /// Execute a closure with the connection mutex held. Centralises
    /// the lock so save/load/etc. don't each repeat the boilerplate.
    template <typename Fn>
    auto with_conn(Fn&& fn);

    /// Acquire a free pool slot index (blocks if none available).
    /// MUST be paired with `release_slot`.
    size_t acquire_slot();
    void release_slot(size_t idx);

    /// Original connection string, retained so individual pool slots
    /// can be rebuilt on demand after a `pqxx::broken_connection`.
    std::string conn_str_;

    /// Fixed-size pool of connections. Indexed; slots are individually
    /// borrowed via the free-index queue.
    std::vector<std::unique_ptr<pqxx::connection>> pool_;

    /// Free slot indices, drained on acquire and refilled on release.
    /// Guarded by `pool_mutex_`; callers wait on `pool_cv_` when empty.
    std::queue<size_t> free_;
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;

    /// Cumulative count of broken-connection-driven slot replacements.
    /// Atomic so monitoring threads can read without taking the pool
    /// mutex.
    std::atomic<size_t> reconnect_count_{0};
};

} // namespace neograph::graph
