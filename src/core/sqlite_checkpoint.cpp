// SqliteCheckpointStore — see include/neograph/graph/sqlite_checkpoint.h
//
// Schema is the same shape as PostgresCheckpointStore (three tables:
// neograph_checkpoints, neograph_checkpoint_blobs, neograph_checkpoint_writes)
// with type substitutions for SQLite:
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

#include <sqlite3.h>

#include <chrono>
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
    json chs = channel_values["channels"];
    if (!chs.is_object()) return out;
    for (auto [name, ch] : chs.items()) {
        if (ch.is_object() && ch.contains("version")) {
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
)SQL";

constexpr const char* kDropDDL = R"SQL(
DROP TABLE IF EXISTS neograph_checkpoint_writes;
DROP TABLE IF EXISTS neograph_checkpoint_blobs;
DROP TABLE IF EXISTS neograph_checkpoints;
)SQL";

constexpr const char* kSelectCols =
    "thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
    "interrupt_phase, barrier_state, channel_versions, global_version, "
    "metadata, step, timestamp_ms, schema_version";

} // namespace

// ── Construction / lifetime ───────────────────────────────────────────

SqliteCheckpointStore::SqliteCheckpointStore(const std::string& db_path) {
    if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = "SqliteCheckpointStore: open failed: ";
        msg += sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(std::move(msg));
    }
    // WAL gives us non-blocking readers and durable writes via fsync at
    // checkpoint time. Foreign keys aren't used (we manage references in
    // app code), but enabling them now would future-proof the schema.
    exec_ddl("PRAGMA journal_mode=WAL;");
    exec_ddl("PRAGMA foreign_keys=ON;");
    // synchronous=NORMAL is the WAL-recommended default — fsyncs on
    // checkpoint, not every commit. ~10× faster than FULL with no
    // durability loss for crashes; only OS crashes can lose the last
    // few transactions in WAL. For a CheckpointStore that's a good
    // trade because the engine is the source of truth in RAM until
    // commit anyway.
    exec_ddl("PRAGMA synchronous=NORMAL;");
    ensure_schema();
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
    exec_ddl(kSchemaDDL);
}

void SqliteCheckpointStore::drop_schema() {
    std::lock_guard lock(db_mutex_);
    exec_ddl(kDropDDL);
    exec_ddl(kSchemaDDL);
}

// ── save() ────────────────────────────────────────────────────────────

void SqliteCheckpointStore::save(const Checkpoint& cp) {
    std::lock_guard lock(db_mutex_);

    exec_ddl("BEGIN TRANSACTION;");
    try {
        // 1. Blob upserts.
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("channels")) {
            json chs = cp.channel_values["channels"];
            if (chs.is_object()) {
                Stmt blob_ins(db_,
                    "INSERT INTO neograph_checkpoint_blobs "
                    "(thread_id, channel, version, blob_data) "
                    "VALUES (?, ?, ?, ?) "
                    "ON CONFLICT (thread_id, channel, version) DO NOTHING");
                for (auto [name, ch] : chs.items()) {
                    if (!ch.is_object() || !ch.contains("version")) continue;
                    if (!ch.contains("value")) continue;
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
        int64_t global_version = 0;
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("global_version")) {
            global_version = cp.channel_values["global_version"].get<int64_t>();
        }

        Stmt cp_ins(db_,
            "INSERT INTO neograph_checkpoints "
            "(thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
            " interrupt_phase, barrier_state, channel_versions, global_version, "
            " metadata, step, timestamp_ms, schema_version) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
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
            "  schema_version   = excluded.schema_version");
        cp_ins.bind_text (1,  cp.thread_id);
        cp_ins.bind_text (2,  cp.id);
        cp_ins.bind_text (3,  cp.parent_id);
        cp_ins.bind_text (4,  cp.current_node);
        cp_ins.bind_text (5,  to_text(next_nodes_to_json(cp.next_nodes)));
        cp_ins.bind_text (6,  std::string(to_string(cp.interrupt_phase)));
        cp_ins.bind_text (7,  to_text(barrier_state_to_json(cp.barrier_state)));
        cp_ins.bind_text (8,  to_text(channel_versions));
        cp_ins.bind_int64(9,  global_version);
        cp_ins.bind_text (10, to_text(cp.metadata));
        cp_ins.bind_int64(11, cp.step);
        cp_ins.bind_int64(12, cp.timestamp);
        cp_ins.bind_int  (13, cp.schema_version);
        if (cp_ins.step() != SQLITE_DONE) {
            throw_sqlite_error(db_, "checkpoint insert failed");
        }

        exec_ddl("COMMIT;");
    } catch (...) {
        exec_ddl("ROLLBACK;");
        throw;
    }
}

// ── load helpers ──────────────────────────────────────────────────────

namespace {

struct LoadedShell {
    Checkpoint cp;
    json channel_versions;
    int64_t global_version = 0;
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
        if (q.step() == SQLITE_ROW) {
            out.emplace(name, parse_text(q.column_text(0)));
        }
    }
    return out;
}

Checkpoint finish_load(sqlite3* db, LoadedShell ls) {
    auto blobs = fetch_blobs(db, ls.cp.thread_id, ls.channel_versions);
    ls.cp.channel_values = materialize_channel_values(
        ls.channel_versions, blobs, static_cast<uint64_t>(ls.global_version));
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
    exec_ddl("BEGIN TRANSACTION;");
    try {
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
        exec_ddl("COMMIT;");
    } catch (...) {
        exec_ddl("ROLLBACK;");
        throw;
    }
}

// ── Pending writes ────────────────────────────────────────────────────

void SqliteCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    std::lock_guard lock(db_mutex_);
    exec_ddl("BEGIN TRANSACTION;");
    try {
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

        exec_ddl("COMMIT;");
    } catch (...) {
        exec_ddl("ROLLBACK;");
        throw;
    }
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
    Stmt q(db_,
        "DELETE FROM neograph_checkpoint_writes "
        "WHERE thread_id = ? AND parent_checkpoint_id = ?");
    q.bind_text(1, thread_id);
    q.bind_text(2, parent_checkpoint_id);
    if (q.step() != SQLITE_DONE) {
        throw_sqlite_error(db_, "clear_writes delete failed");
    }
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
