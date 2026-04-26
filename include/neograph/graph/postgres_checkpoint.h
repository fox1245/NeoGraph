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
 * Stage 3 / Sem 3.3: migrated off libpqxx onto libpq directly. The
 * libpqxx-7.8t64 package on Ubuntu 24.04 has a C++17/C++20 ABI split
 * (link error on `pqxx::conversion_error(..., std::source_location)`)
 * that made it unbuildable against NeoGraph 2.0's C++20 baseline.
 * libpq has a stable C ABI and is what libpqxx wraps internally, so
 * the migration is a net simplification.
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
 * The store owns a **fixed-size pool** of libpq connections sized by
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
 * A broken connection (PG restart, network blip, pgbouncer idle timeout)
 * is detected after each operation via `PQstatus(conn) != CONNECTION_OK`
 * or by matching the `08xxx` SQLSTATE class on an error result. The
 * dead connection is replaced with a fresh one (using the original
 * connection string) and the operation is retried once.
 * `reconnect_count()` exposes the cumulative number of slot
 * replacements for monitoring. Other errors (constraint violations,
 * query errors) propagate as std::runtime_error with the PG server's
 * error message.
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

// Forward-declare libpq's opaque PGconn. Keeps libpq-fe.h out of
// every translation unit that includes this header.
struct pg_conn;

namespace neograph::graph {

/// RAII wrapper around a libpq PGconn. Owns the connection; closes
/// via PQfinish in the destructor. Non-copyable, non-movable to match
/// the pool's slot-index access pattern (pool owns unique_ptr<PgConn>).
struct PgConn {
    pg_conn* raw = nullptr;
    PgConn() = default;
    explicit PgConn(pg_conn* p) : raw(p) {}
    ~PgConn();
    PgConn(const PgConn&) = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&&) = delete;
    PgConn& operator=(PgConn&&) = delete;
};

/**
 * @brief Persistent CheckpointStore backed by PostgreSQL via libpq.
 *
 * Construct with a libpq connection string (e.g.
 * `"postgresql://user:pass@host:5432/dbname"`). The constructor opens
 * the connection eagerly and runs `ensure_schema()` so callers get
 * an immediate failure if credentials or DDL permissions are wrong,
 * rather than a delayed surprise on first save().
 */
class NEOGRAPH_API PostgresCheckpointStore : public CheckpointStore {
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
    /// broken-connection detection. Cumulative across the whole store;
    /// useful for monitoring (e.g. Prometheus gauge) and for tests that
    /// want to assert the retry path fired.
    size_t reconnect_count() const { return reconnect_count_; }

    /// Number of connections in the pool. Useful for benchmarks and
    /// for confirming the pool was sized as expected.
    size_t pool_size() const { return pool_.size(); }

    ~PostgresCheckpointStore() override;

    // Non-copyable, non-movable — mutex would need rebinding on move
    // and connection pool slots are keyed by stable indices.
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

    // ── Async peers (Sem 4 follow-up) ───────────────────────────────────
    //
    // True async: libpq nonblocking mode + asio::posix::stream_descriptor
    // on PQsocket() + co_await on write/read readiness. The io_context
    // stays free for other coroutines during PG's commit fsync, so N
    // concurrent save_async calls across pool slots actually overlap
    // instead of serialising on the main worker thread.
    //
    // Behaviour identical to the sync peers (same retry semantics,
    // same broken-connection auto-replacement, same schema). Only the
    // wire-level wait is non-blocking.

    asio::awaitable<void> save_async(const Checkpoint& cp) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_latest_async(const std::string& thread_id) override;
    asio::awaitable<std::optional<Checkpoint>>
    load_by_id_async(const std::string& id) override;
    asio::awaitable<std::vector<Checkpoint>>
    list_async(const std::string& thread_id, int limit = 100) override;
    asio::awaitable<void>
    delete_thread_async(const std::string& thread_id) override;

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

    /// Async peer of with_conn. `fn(pg_conn*)` must return an
    /// asio::awaitable<T>; the helper co_awaits it, replaces the
    /// slot's connection on BrokenConnection, and retries once.
    /// Defined in the .cpp file — only instantiated from within
    /// the same TU so no explicit instantiation is required.
    template <typename Fn>
    auto with_conn_async(Fn fn) -> decltype(fn(std::declval<pg_conn*>()));

    /// Acquire a free pool slot index (blocks if none available).
    /// MUST be paired with `release_slot`.
    size_t acquire_slot();
    void release_slot(size_t idx);

    /// Original connection string, retained so individual pool slots
    /// can be rebuilt on demand after a broken-connection detection.
    std::string conn_str_;

    /// Fixed-size pool of connections. Indexed; slots are individually
    /// borrowed via the free-index queue.
    std::vector<std::unique_ptr<PgConn>> pool_;

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
