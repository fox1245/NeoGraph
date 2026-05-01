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
 * first connect via `CREATE TABLE IF NOT EXISTS`.
 */
class NEOGRAPH_API SqliteCheckpointStore : public CheckpointStore {
public:
    /// @param db_path Filesystem path or ":memory:". Anything sqlite3_open accepts.
    /// @throws std::runtime_error on open or DDL failure.
    explicit SqliteCheckpointStore(const std::string& db_path);

    ~SqliteCheckpointStore() override;

    // sqlite3* is a unique resource — no copying or moving.
    SqliteCheckpointStore(const SqliteCheckpointStore&) = delete;
    SqliteCheckpointStore& operator=(const SqliteCheckpointStore&) = delete;

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

    /// Drop all `neograph_*` tables and recreate them. Test-only utility.
    /// Useful in fixtures that want a clean slate per test case.
    void drop_schema();

    /// Number of distinct channel-value blobs currently held. Mirrors
    /// `InMemoryCheckpointStore::blob_count()` and
    /// `PostgresCheckpointStore::blob_count()` so dedup tests can be
    /// written against the abstract interface uniformly.
    size_t blob_count();

private:
    void ensure_schema();
    void exec_ddl(const char* sql);

    /// RAII helper closes the connection on destruction. We hold the
    /// raw pointer so forward-declaration in the header works.
    sqlite3* db_ = nullptr;
    std::mutex db_mutex_;
};

} // namespace neograph::graph
