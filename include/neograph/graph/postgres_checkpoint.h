/**
 * @file graph/postgres_checkpoint.h
 * @brief PostgreSQL-backed CheckpointStore — durable, multi-process, blob-deduplicated.
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
 * ## Concurrency
 *
 * Each method holds one libpqxx work() transaction for the duration of
 * its call. The connection itself is wrapped in a mutex — libpqxx
 * connections are not thread-safe. Callers that need higher concurrency
 * should construct multiple PostgresCheckpointStore instances (each
 * holds its own connection) and wire them into independent threads.
 *
 * ## Failure model
 *
 * Network or query errors propagate as `std::runtime_error` with the
 * libpqxx error message attached. The store does NOT auto-reconnect;
 * after a failed call the caller may simply retry — libpqxx will
 * re-establish the connection on the next operation if it detects the
 * underlying socket is dead.
 */
#pragma once

#include <neograph/graph/checkpoint.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

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
    /// @throws std::runtime_error on connection or DDL failure.
    explicit PostgresCheckpointStore(const std::string& conn_str);

    /// Number of times the store reconnected and retried after a
    /// `pqxx::broken_connection` failure. Tracked at the store level
    /// (not per-thread) — useful for monitoring (e.g. expose to a
    /// Prometheus gauge) and for tests that want to assert the retry
    /// path actually fired.
    size_t reconnect_count() const { return reconnect_count_; }

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

    /// Original connection string, retained so `with_conn` can rebuild
    /// the connection on demand after a `pqxx::broken_connection`.
    std::string conn_str_;
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex conn_mutex_;
    /// Incremented every time `with_conn` reconnects after a broken
    /// connection. atomic so reads from monitoring threads don't race.
    std::atomic<size_t> reconnect_count_{0};
};

} // namespace neograph::graph
