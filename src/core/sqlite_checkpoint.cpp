// SqliteCheckpointStore — see include/neograph/graph/sqlite_checkpoint.h
//
// Payload schema is the same shape as PostgresCheckpointStore (three tables:
// neograph_checkpoints, neograph_checkpoint_blobs, neograph_checkpoint_writes)
// with independent monotonic budget obligations and migration markers, and:
//
//   JSONB   → TEXT       (stored as JSON text; queryable via json1 if needed)
//   BIGINT  → INTEGER    (SQLite's INTEGER is variable-width up to 64-bit)
//   TEXT    → TEXT
//
// Same `INSERT ... ON CONFLICT DO NOTHING` upsert pattern for blob
// dedup (SQLite ≥ 3.24, well below the 3.45 we link against).
//
// Why raw sqlite3 C API instead of a wrapper: zero new dependencies
// (libsqlite3 is already on every Linux distro), the API surface we
// touch is small, and the prepared-statement lifetime is local per
// method so the cleanup boilerplate is bounded.

#include <neograph/graph/sqlite_checkpoint.h>
#include "managed_budget_journal.h"

#include <sqlite3.h>

#include <chrono>
#include <limits>
#include <stdexcept>

namespace neograph::graph {

namespace {

// ── Error helpers ─────────────────────────────────────────────────────

[[noreturn]] void throw_sqlite_error(sqlite3* db, const char* what) {
    std::string msg = "SqliteCheckpointStore: ";
    msg += what;
    msg += ": ";
    msg += sqlite3_errmsg(db);
    throw std::runtime_error(std::move(msg));
}

// Owns a writer transaction so every exceptional exit rolls it back.
// Cleanup is noexcept: a rollback error must not replace the original failure.
class WriteTransaction {
public:
    explicit WriteTransaction(sqlite3* db) : db_(db) {
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw_sqlite_error(db_, "begin immediate failed");
    }
    ~WriteTransaction() noexcept {
        if (active_) (void)sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;

    void commit() {
        if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw_sqlite_error(db_, "commit failed");
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_ = true;
};

// RAII wrapper for sqlite3_stmt. Ensures finalize on every exit path —
// including exceptions thrown mid-binding.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw_sqlite_error(db, "prepare failed");
        }
    }
    ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    sqlite3_stmt* get() const { return stmt_; }

    // Bind helpers — index is 1-based per SQLite convention.
    void bind_text(int idx, const std::string& s) {
        if (sqlite3_bind_text(stmt_, idx, s.data(),
                              static_cast<int>(s.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind_text failed");
        }
    }
    void bind_int64(int idx, int64_t v) {
        if (sqlite3_bind_int64(stmt_, idx, v) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind_int64 failed");
        }
    }
    void bind_int(int idx, int v) {
        if (sqlite3_bind_int(stmt_, idx, v) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind_int failed");
        }
    }

    int step() { return sqlite3_step(stmt_); }

    std::string column_text(int idx) const {
        const unsigned char* s = sqlite3_column_text(stmt_, idx);
        if (!s) return {};
        int n = sqlite3_column_bytes(stmt_, idx);
        return std::string(reinterpret_cast<const char*>(s), n);
    }
    int64_t column_int64(int idx) const {
        return sqlite3_column_int64(stmt_, idx);
    }
    int column_int(int idx) const {
        return sqlite3_column_int(stmt_, idx);
    }
    bool column_is_null(int idx) const {
        return sqlite3_column_type(stmt_, idx) == SQLITE_NULL;
    }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// ── JSON ↔ text helpers ───────────────────────────────────────────────

std::string to_text(const json& j) {
    return j.is_null() ? std::string("null") : j.dump();
}

json parse_text(const std::string& s) {
    if (s.empty()) return json();
    return json::parse(s);
}

bool has_managed_budget_obligation(sqlite3* db, const std::string& thread_id) {
    Stmt query(db,
        "SELECT 1 FROM neograph_checkpoint_managed_budget_obligations WHERE thread_id = ?");
    query.bind_text(1, thread_id);
    const int status = query.step();
    if (status == SQLITE_ROW) return true;
    if (status == SQLITE_DONE) return false;
    throw_sqlite_error(db, "managed budget obligation query failed");
}

void record_managed_budget_obligation(sqlite3* db, const std::string& thread_id) {
    Stmt insert(db,
        "INSERT INTO neograph_checkpoint_managed_budget_obligations "
        "(thread_id) VALUES (?) ON CONFLICT (thread_id) DO NOTHING");
    insert.bind_text(1, thread_id);
    if (insert.step() != SQLITE_DONE)
        throw_sqlite_error(db, "managed budget obligation insert failed");
}

json load_managed_budget_head(sqlite3* db, const std::string& thread_id) {
    Stmt query(db,
        "SELECT head_json FROM neograph_checkpoint_managed_budget_heads WHERE thread_id = ?");
    query.bind_text(1, thread_id);
    const int status = query.step();
    if (status == SQLITE_DONE) return nullptr;
    if (status != SQLITE_ROW) throw_sqlite_error(db, "managed budget head query failed");
    return json::parse(query.column_text(0));
}

void save_managed_budget_head(sqlite3* db, const std::string& thread_id, const json& head) {
    Stmt insert(db,
        "INSERT INTO neograph_checkpoint_managed_budget_heads (thread_id, head_json) "
        "VALUES (?, ?) ON CONFLICT (thread_id) DO UPDATE SET head_json = excluded.head_json");
    insert.bind_text(1, thread_id);
    insert.bind_text(2, head.dump());
    if (insert.step() != SQLITE_DONE) throw_sqlite_error(db, "managed budget head write failed");
}

json load_managed_budget_effect(sqlite3* db, const std::string& thread_id,
                                const std::string& generation, const std::string& effect_id) {
    Stmt query(db,
        "SELECT effect_json FROM neograph_checkpoint_managed_budget_effects "
        "WHERE thread_id = ? AND bank_generation = ? AND effect_id = ?");
    query.bind_text(1, thread_id);
    query.bind_text(2, generation);
    query.bind_text(3, effect_id);
    const int status = query.step();
    if (status == SQLITE_DONE) return nullptr;
    if (status != SQLITE_ROW) throw_sqlite_error(db, "managed budget effect query failed");
    return json::parse(query.column_text(0));
}

void save_managed_budget_effect(sqlite3* db, const std::string& thread_id,
                                const std::string& generation, const std::string& effect_id,
                                const json& effect) {
    Stmt insert(db,
        "INSERT INTO neograph_checkpoint_managed_budget_effects "
        "(thread_id, bank_generation, effect_id, effect_json) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (thread_id, bank_generation, effect_id) "
        "DO UPDATE SET effect_json = excluded.effect_json");
    insert.bind_text(1, thread_id);
    insert.bind_text(2, generation);
    insert.bind_text(3, effect_id);
    insert.bind_text(4, effect.dump());
    if (insert.step() != SQLITE_DONE) throw_sqlite_error(db, "managed budget effect write failed");
}

std::string managed_budget_lease_key(const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    if (!lease) throw std::invalid_argument("SqliteCheckpointStore: missing managed budget lease");
    return detail::ManagedBudgetJournalAccess::storage_key(lease->scope());
}

// next_nodes ↔ JSON array (vector<string>).
json next_nodes_to_json(const std::vector<std::string>& v) {
    json arr = json::array();
    for (const auto& s : v) arr.push_back(s);
    return arr;
}
std::vector<std::string> next_nodes_from_json(const json& j) {
    std::vector<std::string> out;
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

// barrier_state ↔ JSON object {"barrier": ["upstream"]}
json barrier_state_to_json(const std::map<std::string, std::set<std::string>>& bs) {
    json obj = json::object();
    for (const auto& [name, set] : bs) {
        json arr = json::array();
        for (const auto& s : set) arr.push_back(s);
        obj[name] = arr;
    }
    return obj;
}
std::map<std::string, std::set<std::string>> barrier_state_from_json(const json& j) {
    std::map<std::string, std::set<std::string>> out;
    if (!j.is_object()) return out;
    for (auto [name, arr] : j.items()) {
        std::set<std::string> set;
        if (arr.is_array()) {
            for (const auto& s : arr) {
                if (s.is_string()) set.insert(s.get<std::string>());
            }
        }
        out.emplace(name, std::move(set));
    }
    return out;
}

json extract_channel_versions(const json& channel_values) {
    json out = json::object();
    if (!channel_values.is_object()) return out;
    if (!channel_values.contains("channels")) return out;
    const auto& chs = channel_values.at("channels");
    if (!chs.is_object()) return out;
    for (const auto& [name, ch] : chs.items()) {
        if (checkpoint_channel_blob_eligible(ch)) {
            out[name] = ch["version"];
        }
    }
    return out;
}

json materialize_channel_values(const json& channel_versions,
                                 const std::map<std::string, json>& blobs,
                                 uint64_t global_version) {
    json full_channels = json::object();
    if (channel_versions.is_object()) {
        for (auto [name, ver] : channel_versions.items()) {
            json entry = json::object();
            entry["version"] = ver;
            auto it = blobs.find(name);
            // Defensive: missing blob → null. Matches PG store semantics.
            entry["value"] = (it != blobs.end()) ? it->second : json();
            full_channels[name] = entry;
        }
    }
    json out = json::object();
    out["channels"] = full_channels;
    out["global_version"] = global_version;
    return out;
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

constexpr const char* kSchemaDDL = R"SQL(
CREATE TABLE IF NOT EXISTS neograph_checkpoints (
    thread_id          TEXT    NOT NULL,
    checkpoint_id      TEXT    NOT NULL,
    parent_id          TEXT    NOT NULL DEFAULT '',
    current_node       TEXT    NOT NULL DEFAULT '',
    next_nodes         TEXT    NOT NULL DEFAULT '[]',
    interrupt_phase    TEXT    NOT NULL DEFAULT 'completed',
    barrier_state      TEXT    NOT NULL DEFAULT '{}',
    channel_versions   TEXT    NOT NULL DEFAULT '{}',
    global_version     INTEGER NOT NULL DEFAULT 0,
    metadata           TEXT    NOT NULL DEFAULT '{}',
    step               INTEGER NOT NULL DEFAULT 0,
    timestamp_ms       INTEGER NOT NULL DEFAULT 0,
    schema_version     INTEGER NOT NULL DEFAULT 2,
    checkpoint_shape   TEXT,
    PRIMARY KEY (thread_id, checkpoint_id)
);

CREATE INDEX IF NOT EXISTS neograph_checkpoints_recent
    ON neograph_checkpoints (thread_id, timestamp_ms DESC, step DESC);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_blobs (
    thread_id  TEXT    NOT NULL,
    channel    TEXT    NOT NULL,
    version    INTEGER NOT NULL,
    blob_data  TEXT    NOT NULL,
    PRIMARY KEY (thread_id, channel, version)
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_writes (
    thread_id            TEXT    NOT NULL,
    parent_checkpoint_id TEXT    NOT NULL,
    seq                  INTEGER NOT NULL,
    task_id              TEXT    NOT NULL,
    task_path            TEXT    NOT NULL DEFAULT '',
    node_name            TEXT    NOT NULL DEFAULT '',
    writes_json          TEXT    NOT NULL DEFAULT '[]',
    command_json         TEXT    NOT NULL DEFAULT 'null',
    sends_json           TEXT    NOT NULL DEFAULT '[]',
    step                 INTEGER NOT NULL DEFAULT 0,
    timestamp_ms         INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (thread_id, parent_checkpoint_id, seq)
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_managed_budget_obligations (
    thread_id TEXT PRIMARY KEY
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_managed_budget_heads (
    thread_id TEXT PRIMARY KEY,
    head_json TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_managed_budget_effects (
    thread_id       TEXT NOT NULL,
    bank_generation TEXT NOT NULL,
    effect_id       TEXT NOT NULL,
    effect_json     TEXT NOT NULL,
    PRIMARY KEY (thread_id, bank_generation, effect_id)
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_schema_migrations (
    migration_id TEXT PRIMARY KEY
);
)SQL";

constexpr const char* kDropDDL = R"SQL(
DROP TABLE IF EXISTS neograph_checkpoint_writes;
DROP TABLE IF EXISTS neograph_checkpoint_blobs;
DROP TABLE IF EXISTS neograph_checkpoints;
)SQL";

constexpr const char* kSelectCols =
    "thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
    "interrupt_phase, barrier_state, channel_versions, global_version, "
    "metadata, step, timestamp_ms, schema_version, checkpoint_shape";

} // namespace

// ── Construction / lifetime ───────────────────────────────────────────

SqliteCheckpointStore::SqliteCheckpointStore(const std::string& db_path)
    : SqliteCheckpointStore(db_path, std::chrono::seconds(5)) {}

SqliteCheckpointStore::SqliteCheckpointStore(
    const std::string& db_path, std::chrono::milliseconds busy_timeout)
    : SqliteCheckpointStore(db_path, busy_timeout, {}) {}

SqliteCheckpointStore::SqliteCheckpointStore(
    const std::string& db_path, std::chrono::milliseconds busy_timeout,
    WriteGuard write_guard) : write_guard_(std::move(write_guard)) {
    if (busy_timeout.count() < 0 ||
        busy_timeout.count() > std::numeric_limits<int>::max()) {
        throw std::invalid_argument("SqliteCheckpointStore: busy timeout is out of range");
    }
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = "SqliteCheckpointStore: open failed: ";
        msg += sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(std::move(msg));
    }
    try {
        // https://www.sqlite.org/c3ref/busy_timeout.html (fetched 2026-07-21)
        // waits through transient writer contention up to the configured budget.
        if (sqlite3_busy_timeout(db_, static_cast<int>(busy_timeout.count())) != SQLITE_OK) {
            throw_sqlite_error(db_, "cannot configure busy timeout");
        }
        // WAL gives us non-blocking readers and durable writes via fsync at
        // checkpoint time. Foreign keys aren't used (we manage references in
        // app code), but enabling them now would future-proof the schema.
        exec_ddl("PRAGMA journal_mode=WAL;");
        exec_ddl("PRAGMA foreign_keys=ON;");
        // A committed write-ahead claim must survive a host crash before IO:
        // NORMAL can lose the latest WAL transactions and reissue currency.
        exec_ddl("PRAGMA synchronous=FULL;");
        ensure_schema();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

SqliteCheckpointStore::~SqliteCheckpointStore() {
    if (db_) sqlite3_close(db_);
}

void SqliteCheckpointStore::exec_ddl(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = "SqliteCheckpointStore: exec failed: ";
        if (err) {
            msg += err;
            sqlite3_free(err);
        }
        throw std::runtime_error(std::move(msg));
    }
}

void SqliteCheckpointStore::ensure_schema() {
    WriteTransaction transaction(db_);
    exec_ddl(kSchemaDDL);
    // Older databases lack the lossless structural residue. This inspects only
    // table metadata, never checkpoint history, and preserves legacy payloads.
    bool has_checkpoint_shape = false;
    {
        Stmt columns(db_, "PRAGMA table_info(neograph_checkpoints)");
        int column_status;
        while ((column_status = columns.step()) == SQLITE_ROW) {
            if (columns.column_text(1) == "checkpoint_shape") has_checkpoint_shape = true;
        }
        if (column_status != SQLITE_DONE) throw_sqlite_error(db_, "checkpoint schema lookup failed");
    }
    if (!has_checkpoint_shape)
        exec_ddl("ALTER TABLE neograph_checkpoints ADD COLUMN checkpoint_shape TEXT;");
    Stmt migration(db_,
        "SELECT 1 FROM neograph_checkpoint_schema_migrations "
        "WHERE migration_id = 'managed_budget_finite_scope_v2'");
    const int status = migration.step();
    if (status != SQLITE_ROW && status != SQLITE_DONE)
        throw_sqlite_error(db_, "managed budget migration lookup failed");
    if (status == SQLITE_DONE) {
        // Backfill once using the saver classifier, not reported unbounded
        // usage. Existing flags and journal obligations are never removed.
        Stmt retained(db_, "SELECT thread_id, metadata FROM neograph_checkpoints");
        int retained_status;
        while ((retained_status = retained.step()) == SQLITE_ROW) {
            bool obligation = true;
            try {
                Checkpoint checkpoint;
                checkpoint.metadata = json::parse(retained.column_text(1));
                restore_checkpoint_storage_envelope(checkpoint);
                obligation = detail::ManagedBudgetJournalAccess::checkpoint_requires_obligation(checkpoint);
            } catch (const std::exception&) {
                // Malformed retained custody is denial-only evidence.
            }
            if (obligation) record_managed_budget_obligation(db_, retained.column_text(0));
        }
        if (retained_status != SQLITE_DONE)
            throw_sqlite_error(db_, "retained managed budget migration failed");
        exec_ddl(
            "INSERT INTO neograph_checkpoint_schema_migrations (migration_id) "
            "VALUES ('managed_budget_finite_scope_v2') ON CONFLICT (migration_id) DO NOTHING");
    }
    transaction.commit();
}

void SqliteCheckpointStore::drop_schema() {
    std::lock_guard lock(db_mutex_);
    if (write_guard_)
        throw std::logic_error("SqliteCheckpointStore: guarded schema cannot be dropped");
    exec_ddl(kDropDDL);
    exec_ddl(kSchemaDDL);
}

// ── save() ────────────────────────────────────────────────────────────

void SqliteCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, cp.thread_id);
    save_locked(cp);
    transaction.commit();
}

void SqliteCheckpointStore::save_locked(const Checkpoint& cp) {
    const auto metadata = checkpoint_storage_metadata(cp);
    const auto shape = checkpoint_storage_shape(cp);
    // Custody denial only: this is not proof that any bank is authentic.
    // Record in the payload transaction, independently of metadata/overwrites.
    if (detail::ManagedBudgetJournalAccess::checkpoint_requires_obligation(cp)) {
        record_managed_budget_obligation(db_, cp.thread_id);
    }
    // 1. Blob upserts.
    if (cp.channel_values.is_object() &&
        cp.channel_values.contains("channels")) {
        const auto& chs = cp.channel_values.at("channels");
        if (chs.is_object()) {
            Stmt blob_ins(db_,
                "INSERT INTO neograph_checkpoint_blobs "
                "(thread_id, channel, version, blob_data) "
                "VALUES (?, ?, ?, ?) "
                "ON CONFLICT (thread_id, channel, version) DO NOTHING");
            for (const auto& [name, ch] : chs.items()) {
                if (!checkpoint_channel_blob_eligible(ch)) continue;
                int64_t ver = ch["version"].get<int64_t>();
                std::string val_text = to_text(ch["value"]);

                sqlite3_reset(blob_ins.get());
                sqlite3_clear_bindings(blob_ins.get());
                blob_ins.bind_text(1, cp.thread_id);
                blob_ins.bind_text(2, name);
                blob_ins.bind_int64(3, ver);
                blob_ins.bind_text(4, val_text);
                if (blob_ins.step() != SQLITE_DONE) {
                    throw_sqlite_error(db_, "blob insert failed");
                }
            }
        }
    }

    // 2. Checkpoint row (upsert on PK).
    json channel_versions = extract_channel_versions(cp.channel_values);
    // This legacy SQL index field is not the original JSON scalar; the shared
    // shape retains its exact presence/type/value, including null or overflow.
    constexpr int64_t global_version = 0;

    Stmt cp_ins(db_,
        "INSERT INTO neograph_checkpoints "
        "(thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
        " interrupt_phase, barrier_state, channel_versions, global_version, "
        " metadata, step, timestamp_ms, schema_version, checkpoint_shape) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (thread_id, checkpoint_id) DO UPDATE SET "
        "  parent_id        = excluded.parent_id, "
        "  current_node     = excluded.current_node, "
        "  next_nodes       = excluded.next_nodes, "
        "  interrupt_phase  = excluded.interrupt_phase, "
        "  barrier_state    = excluded.barrier_state, "
        "  channel_versions = excluded.channel_versions, "
        "  global_version   = excluded.global_version, "
        "  metadata         = excluded.metadata, "
        "  step             = excluded.step, "
        "  timestamp_ms     = excluded.timestamp_ms, "
        "  schema_version   = excluded.schema_version, "
        "  checkpoint_shape = excluded.checkpoint_shape");
    cp_ins.bind_text (1,  cp.thread_id);
    cp_ins.bind_text (2,  cp.id);
    cp_ins.bind_text (3,  cp.parent_id);
    cp_ins.bind_text (4,  cp.current_node);
    cp_ins.bind_text (5,  to_text(next_nodes_to_json(cp.next_nodes)));
    cp_ins.bind_text (6,  std::string(to_string(cp.interrupt_phase)));
    cp_ins.bind_text (7,  to_text(barrier_state_to_json(cp.barrier_state)));
    cp_ins.bind_text (8,  to_text(channel_versions));
    cp_ins.bind_int64(9,  global_version);
    cp_ins.bind_text (10, to_text(metadata));
    cp_ins.bind_int64(11, cp.step);
    cp_ins.bind_int64(12, cp.timestamp);
    cp_ins.bind_int  (13, cp.schema_version);
    cp_ins.bind_text (14, shape.dump());
    if (cp_ins.step() != SQLITE_DONE) {
        throw_sqlite_error(db_, "checkpoint insert failed");
    }
}

// ── load helpers ──────────────────────────────────────────────────────

namespace {

struct LoadedShell {
    Checkpoint cp;
    json channel_versions;
    int64_t global_version = 0;
    json checkpoint_shape;
};

LoadedShell stmt_to_loaded(Stmt& q) {
    LoadedShell ls;
    ls.cp.thread_id        = q.column_text (0);
    ls.cp.id               = q.column_text (1);
    ls.cp.parent_id        = q.column_text (2);
    ls.cp.current_node     = q.column_text (3);
    ls.cp.next_nodes       = next_nodes_from_json(parse_text(q.column_text(4)));
    ls.cp.interrupt_phase  = parse_checkpoint_phase(q.column_text(5));
    ls.cp.barrier_state    = barrier_state_from_json(parse_text(q.column_text(6)));
    ls.channel_versions    = parse_text(q.column_text(7));
    ls.global_version      = q.column_int64(8);
    ls.cp.metadata         = parse_text(q.column_text(9));
    ls.cp.step             = q.column_int64(10);
    ls.cp.timestamp        = q.column_int64(11);
    ls.cp.schema_version   = q.column_int  (12);
    if (!q.column_is_null(13)) ls.checkpoint_shape = json::parse(q.column_text(13));
    return ls;
}

std::map<std::string, json> fetch_blobs(sqlite3* db,
                                         const std::string& thread_id,
                                         const json& channel_versions) {
    std::map<std::string, json> out;
    if (!channel_versions.is_object() || channel_versions.empty()) return out;

    Stmt q(db,
        "SELECT blob_data FROM neograph_checkpoint_blobs "
        "WHERE thread_id = ? AND channel = ? AND version = ?");
    for (auto [name, ver] : channel_versions.items()) {
        sqlite3_reset(q.get());
        sqlite3_clear_bindings(q.get());
        q.bind_text(1, thread_id);
        q.bind_text(2, name);
        q.bind_int64(3, ver.get<int64_t>());
        const int status = q.step();
        if (status == SQLITE_ROW) {
            out.emplace(name, parse_text(q.column_text(0)));
        } else if (status != SQLITE_DONE) {
            throw_sqlite_error(db, "checkpoint blob lookup failed");
        }
    }
    return out;
}

Checkpoint finish_load(sqlite3* db, LoadedShell ls) {
    auto blobs = fetch_blobs(db, ls.cp.thread_id, ls.channel_versions);
    if (ls.checkpoint_shape.is_null()) {
        ls.cp.channel_values = materialize_channel_values(
            ls.channel_versions, blobs, static_cast<uint64_t>(ls.global_version));
    } else {
        restore_checkpoint_storage_shape(ls.cp, ls.checkpoint_shape, blobs);
    }
    restore_checkpoint_storage_envelope(ls.cp);
    return ls.cp;
}

} // namespace

std::optional<Checkpoint> SqliteCheckpointStore::load_latest(
    const std::string& thread_id) {
    std::lock_guard lock(db_mutex_);
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE thread_id = ? "
        "ORDER BY timestamp_ms DESC, step DESC LIMIT 1";
    Stmt q(db_, sql.c_str());
    q.bind_text(1, thread_id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return finish_load(db_, stmt_to_loaded(q));
}

std::optional<Checkpoint> SqliteCheckpointStore::load_by_id(
    const std::string& id) {
    std::lock_guard lock(db_mutex_);
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE checkpoint_id = ? LIMIT 1";
    Stmt q(db_, sql.c_str());
    q.bind_text(1, id);
    if (q.step() != SQLITE_ROW) return std::nullopt;
    return finish_load(db_, stmt_to_loaded(q));
}

std::vector<Checkpoint> SqliteCheckpointStore::list(
    const std::string& thread_id, int limit) {
    std::lock_guard lock(db_mutex_);
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE thread_id = ? "
        "ORDER BY timestamp_ms DESC, step DESC LIMIT ?";
    Stmt q(db_, sql.c_str());
    q.bind_text(1, thread_id);
    q.bind_int(2, limit);
    std::vector<Checkpoint> out;
    while (q.step() == SQLITE_ROW) {
        out.push_back(finish_load(db_, stmt_to_loaded(q)));
    }
    return out;
}

void SqliteCheckpointStore::delete_thread(const std::string& thread_id) {
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, thread_id);
    for (const char* sql : {
        "DELETE FROM neograph_checkpoint_writes WHERE thread_id = ?",
        "DELETE FROM neograph_checkpoint_blobs  WHERE thread_id = ?",
        "DELETE FROM neograph_checkpoints       WHERE thread_id = ?"
    }) {
        Stmt q(db_, sql);
        q.bind_text(1, thread_id);
        if (q.step() != SQLITE_DONE) {
            throw_sqlite_error(db_, "delete_thread step failed");
        }
    }
    transaction.commit();
}

bool SqliteCheckpointStore::requires_managed_budget(const std::string& thread_id) {
    std::lock_guard lock(db_mutex_);
    return has_managed_budget_obligation(db_, thread_id);
}

asio::awaitable<bool> SqliteCheckpointStore::requires_managed_budget_async(
    std::string thread_id) {
    co_return co_await CheckpointStore::requires_managed_budget_async(std::move(thread_id));
}

std::shared_ptr<OwnedManagedBudgetLease> SqliteCheckpointStore::acquire_managed_budget_lease(
    const ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
    const std::string& expected_checkpoint_commitment) {
    const auto key = detail::ManagedBudgetJournalAccess::storage_key(scope);
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, key);
    auto head = load_managed_budget_head(db_, key);
    const bool prior_obligation = has_managed_budget_obligation(db_, key);
    if (!head.is_null() && !expected_checkpoint_id.empty()) {
        const std::string sql = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = ? AND checkpoint_id = ?";
        Stmt source(db_, sql.c_str());
        source.bind_text(1, key);
        source.bind_text(2, expected_checkpoint_id);
        const int status = source.step();
        if (status == SQLITE_DONE)
            throw std::runtime_error("SqliteCheckpointStore: managed budget source checkpoint is missing");
        if (status != SQLITE_ROW) throw_sqlite_error(db_, "managed budget source lookup failed");
        const auto checkpoint = finish_load(db_, stmt_to_loaded(source));
        if (managed_budget_checkpoint_commitment(checkpoint) != expected_checkpoint_commitment)
            throw std::runtime_error("SqliteCheckpointStore: managed budget source commitment mismatch");
    }
    auto lease = detail::ManagedBudgetJournalAccess::acquire(
        head, prior_obligation, scope, expected_checkpoint_id, expected_checkpoint_commitment);
    if (!lease)
        throw std::logic_error("SqliteCheckpointStore: journal did not issue a managed budget lease");
    record_managed_budget_obligation(db_, key);
    save_managed_budget_head(db_, key, head);
    transaction.commit();
    detail::ManagedBudgetJournalAccess::refresh(lease, head);
    return lease;
}

asio::awaitable<std::shared_ptr<OwnedManagedBudgetLease>>
SqliteCheckpointStore::acquire_managed_budget_lease_async(
    ManagedBudgetLeaseScope scope, std::string expected_checkpoint_id,
    std::string expected_checkpoint_commitment) {
    co_return co_await CheckpointStore::acquire_managed_budget_lease_async(
        std::move(scope), std::move(expected_checkpoint_id), std::move(expected_checkpoint_commitment));
}

ManagedBudgetEffectReceipt SqliteCheckpointStore::begin_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const std::string& effect_id,
    std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) {
    const auto key = managed_budget_lease_key(lease);
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, key);
    auto head = load_managed_budget_head(db_, key);
    auto effect = load_managed_budget_effect(db_, key, lease->bank_generation(), effect_id);
    auto receipt = detail::ManagedBudgetJournalAccess::begin(
        head, effect, lease, effect_id, exact_claim_amount, prepared_request_digest);
    save_managed_budget_effect(db_, key, lease->bank_generation(), effect_id, effect);
    save_managed_budget_head(db_, key, head);
    transaction.commit();
    detail::ManagedBudgetJournalAccess::refresh(lease, head);
    return receipt;
}

asio::awaitable<ManagedBudgetEffectReceipt> SqliteCheckpointStore::begin_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, std::string effect_id,
    std::uint64_t exact_claim_amount, std::string prepared_request_digest) {
    co_return co_await CheckpointStore::begin_managed_budget_effect_async(
        std::move(lease), std::move(effect_id), exact_claim_amount, std::move(prepared_request_digest));
}

void SqliteCheckpointStore::settle_managed_budget_effect(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const ManagedBudgetEffectReceipt& effect,
    sp::runtime::Result genuine_outcome, const UsageAccumulator::AuthoritySnapshot& authority) {
    const auto key = managed_budget_lease_key(lease);
    if (!effect.active())
        throw std::invalid_argument("SqliteCheckpointStore: missing managed budget effect receipt");
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, key);
    auto head = load_managed_budget_head(db_, key);
    auto stored_effect = load_managed_budget_effect(db_, key, lease->bank_generation(), effect.effect_id());
    detail::ManagedBudgetJournalAccess::settle(
        head, stored_effect, lease, effect, std::move(genuine_outcome), authority);
    save_managed_budget_effect(db_, key, lease->bank_generation(), effect.effect_id(), stored_effect);
    save_managed_budget_head(db_, key, head);
    transaction.commit();
    detail::ManagedBudgetJournalAccess::refresh(lease, head);
}

asio::awaitable<void> SqliteCheckpointStore::settle_managed_budget_effect_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, ManagedBudgetEffectReceipt effect,
    sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority) {
    co_await CheckpointStore::settle_managed_budget_effect_async(
        std::move(lease), std::move(effect), std::move(genuine_outcome), std::move(authority));
}

void SqliteCheckpointStore::publish_managed_budget_checkpoint(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease, const Checkpoint& checkpoint) {
    const auto key = managed_budget_lease_key(lease);
    if (checkpoint.thread_id != key)
        throw std::invalid_argument("SqliteCheckpointStore: managed checkpoint storage scope mismatch");
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, key);
    {
        Stmt existing(db_,
            "SELECT 1 FROM neograph_checkpoints WHERE thread_id = ? AND checkpoint_id = ?");
        existing.bind_text(1, key);
        existing.bind_text(2, checkpoint.id);
        const int status = existing.step();
        if (status == SQLITE_ROW)
            throw std::runtime_error("SqliteCheckpointStore: managed checkpoint ID was already published");
        if (status != SQLITE_DONE) throw_sqlite_error(db_, "managed checkpoint identity lookup failed");
    }
    auto head = load_managed_budget_head(db_, key);
    detail::ManagedBudgetJournalAccess::publish(head, lease, checkpoint);
    save_locked(checkpoint);
    {
        const std::string sql = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = ? AND checkpoint_id = ?";
        Stmt stored(db_, sql.c_str());
        stored.bind_text(1, key);
        stored.bind_text(2, checkpoint.id);
        if (stored.step() != SQLITE_ROW) throw_sqlite_error(db_, "published checkpoint lookup failed");
        const auto persisted = finish_load(db_, stmt_to_loaded(stored));
        if (managed_budget_checkpoint_commitment(persisted) != managed_budget_checkpoint_commitment(checkpoint))
            throw std::runtime_error("SqliteCheckpointStore: published checkpoint cannot be reconstructed exactly");
    }
    save_managed_budget_head(db_, key, head);
    transaction.commit();
    detail::ManagedBudgetJournalAccess::refresh(lease, head);
}

asio::awaitable<void> SqliteCheckpointStore::publish_managed_budget_checkpoint_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease, Checkpoint checkpoint) {
    co_await CheckpointStore::publish_managed_budget_checkpoint_async(
        std::move(lease), std::move(checkpoint));
}

void SqliteCheckpointStore::release_managed_budget_lease(
    const std::shared_ptr<OwnedManagedBudgetLease>& lease) {
    const auto key = managed_budget_lease_key(lease);
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, key);
    auto head = load_managed_budget_head(db_, key);
    detail::ManagedBudgetJournalAccess::release(head, lease);
    save_managed_budget_head(db_, key, head);
    transaction.commit();
    detail::ManagedBudgetJournalAccess::refresh(lease, head);
}

asio::awaitable<void> SqliteCheckpointStore::release_managed_budget_lease_async(
    std::shared_ptr<OwnedManagedBudgetLease> lease) {
    co_await CheckpointStore::release_managed_budget_lease_async(std::move(lease));
}

// ── Pending writes ────────────────────────────────────────────────────

void SqliteCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    if (write.native_result) throw std::invalid_argument("Native pending writes require explicit archive serialization");
    std::lock_guard lock(db_mutex_);
    // Acquire the writer before reading seq. A deferred read transaction cannot
    // upgrade its WAL snapshot after a different connection commits, even with
    // busy_timeout; shared Program/chat databases exercise this path frequently.
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, thread_id);
    // Allocate next seq inside the transaction so concurrent puts
    // (well, serialised by db_mutex_, but matching PG semantics
    // anyway) get distinct seqs.
    int next_seq = 0;
    {
        Stmt q(db_,
            "SELECT COALESCE(MAX(seq), -1) + 1 FROM neograph_checkpoint_writes "
            "WHERE thread_id = ? AND parent_checkpoint_id = ?");
        q.bind_text(1, thread_id);
        q.bind_text(2, parent_checkpoint_id);
        if (q.step() != SQLITE_ROW) {
            throw_sqlite_error(db_, "next_seq query failed");
        }
        next_seq = q.column_int(0);
    }

    Stmt ins(db_,
        "INSERT INTO neograph_checkpoint_writes "
        "(thread_id, parent_checkpoint_id, seq, task_id, task_path, "
        " node_name, writes_json, command_json, sends_json, step, "
        " timestamp_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind_text (1,  thread_id);
    ins.bind_text (2,  parent_checkpoint_id);
    ins.bind_int  (3,  next_seq);
    ins.bind_text (4,  write.task_id);
    ins.bind_text (5,  write.task_path);
    ins.bind_text (6,  write.node_name);
    ins.bind_text (7,  to_text(write.writes));
    ins.bind_text (8,  to_text(write.command));
    ins.bind_text (9,  to_text(write.sends));
    ins.bind_int64(10, write.step);
    ins.bind_int64(11, write.timestamp);
    if (ins.step() != SQLITE_DONE) {
        throw_sqlite_error(db_, "put_writes insert failed");
    }

    transaction.commit();
}

std::vector<PendingWrite> SqliteCheckpointStore::get_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(db_mutex_);
    Stmt q(db_,
        "SELECT task_id, task_path, node_name, writes_json, command_json, "
        "       sends_json, step, timestamp_ms "
        "FROM neograph_checkpoint_writes "
        "WHERE thread_id = ? AND parent_checkpoint_id = ? "
        "ORDER BY seq ASC");
    q.bind_text(1, thread_id);
    q.bind_text(2, parent_checkpoint_id);

    std::vector<PendingWrite> out;
    while (q.step() == SQLITE_ROW) {
        PendingWrite pw;
        pw.task_id   = q.column_text(0);
        pw.task_path = q.column_text(1);
        pw.node_name = q.column_text(2);
        pw.writes    = parse_text(q.column_text(3));
        pw.command   = parse_text(q.column_text(4));
        pw.sends     = parse_text(q.column_text(5));
        pw.step      = q.column_int64(6);
        pw.timestamp = q.column_int64(7);
        out.push_back(std::move(pw));
    }
    return out;
}

void SqliteCheckpointStore::clear_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::lock_guard lock(db_mutex_);
    WriteTransaction transaction(db_);
    if (write_guard_) write_guard_(db_, thread_id);
    Stmt q(db_,
        "DELETE FROM neograph_checkpoint_writes "
        "WHERE thread_id = ? AND parent_checkpoint_id = ?");
    q.bind_text(1, thread_id);
    q.bind_text(2, parent_checkpoint_id);
    if (q.step() != SQLITE_DONE) {
        throw_sqlite_error(db_, "clear_writes delete failed");
    }
    transaction.commit();
}

size_t SqliteCheckpointStore::blob_count() {
    std::lock_guard lock(db_mutex_);
    Stmt q(db_, "SELECT COUNT(*) FROM neograph_checkpoint_blobs");
    if (q.step() != SQLITE_ROW) {
        throw_sqlite_error(db_, "blob_count query failed");
    }
    return static_cast<size_t>(q.column_int64(0));
}

} // namespace neograph::graph
