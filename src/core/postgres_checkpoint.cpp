// PostgresCheckpointStore — see include/neograph/graph/postgres_checkpoint.h
//
// Stage 3 / Sem 3.3: rewritten on libpq directly. The earlier libpqxx
// implementation stopped linking on Ubuntu 24.04 (libpqxx-7.8t64 was
// built against C++17 but its headers reference std::source_location
// overloads that only exist in C++20 — a cross-std ABI split that bit
// NeoGraph's bump to C++20 in 2.0). libpq is the C API libpqxx wraps;
// it has a stable ABI and is already a transitive dep.
//
// The parameter-passing shape is unchanged in spirit: one prepared
// "save everything" statement with a CTE that upserts blobs and the
// checkpoint row in one round-trip. The blob array used to come in
// via libpqxx's vector<T>-to-PG-array conversion; we now pass it as
// a jsonb array and let PG's jsonb_to_recordset unpack it — simpler
// to escape than building a Postgres text-array literal manually.

#include <neograph/graph/postgres_checkpoint.h>

#include <libpq-fe.h>

// PG socket wrapping is platform-split: on POSIX PQsocket returns an
// int fd and asio::posix::stream_descriptor wraps it directly; on
// Windows it returns a SOCKET and we reach for asio::ip::tcp::socket
// which can take over an existing native SOCKET via assign().
#ifdef _WIN32
#  include <asio/ip/tcp.hpp>
#else
#  include <asio/posix/stream_descriptor.hpp>
#endif
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::graph {

// Out-of-line PgConn destructor — keeps libpq-fe.h out of the header.
PgConn::~PgConn() {
    if (raw) PQfinish(raw);
}

namespace {

// ── PGresult RAII + error helpers ─────────────────────────────────────

struct PgResult {
    PGresult* raw = nullptr;
    PgResult() = default;
    explicit PgResult(PGresult* r) : raw(r) {}
    ~PgResult() { if (raw) PQclear(raw); }
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& o) noexcept : raw(o.raw) { o.raw = nullptr; }
    PgResult& operator=(PgResult&& o) noexcept {
        if (raw) PQclear(raw);
        raw = o.raw;
        o.raw = nullptr;
        return *this;
    }
    operator PGresult*() const { return raw; }
};

// A broken connection error — used to signal the with_conn retry path.
struct BrokenConnection : std::runtime_error {
    explicit BrokenConnection(const std::string& what)
        : std::runtime_error(what) {}
};

// Is this a recoverable connection-failure error? Mirrors libpqxx's
// broken_connection detection: SQLSTATE class 08 ("Connection Exception")
// plus a dead-socket PQstatus check. Transient errors in this class
// are safe to retry on a fresh connection; anything else (constraint
// violation, syntax error, permissions) is a logic bug and must not
// masquerade as a transient failure.
bool is_broken_connection(PGresult* res, pg_conn* c) {
    if (PQstatus(c) != CONNECTION_OK) return true;
    if (!res) return true;
    const char* sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
    if (sqlstate && std::strlen(sqlstate) >= 2 &&
        sqlstate[0] == '0' && sqlstate[1] == '8') {
        return true;
    }
    return false;
}

// Throw on non-success result. Distinguishes broken_connection from
// other errors so the with_conn retry layer can handle them differently.
void check_ok(PGresult* res, pg_conn* c, const char* context) {
    ExecStatusType st = PQresultStatus(res);
    if (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK) return;
    std::string msg = context ? context : "pg exec";
    msg += ": ";
    const char* err = PQresultErrorMessage(res);
    if (err && *err) msg += err;
    else msg += PQerrorMessage(c);
    if (is_broken_connection(res, c)) {
        throw BrokenConnection(msg);
    }
    throw std::runtime_error(msg);
}

// ── Minimal wrappers around PQexecParams / PQexecPrepared ─────────────
//
// Both take vectors of std::string (owning) for the parameter values —
// libpq's interface is (int, const char*const* values, const int* lengths,
// const int* formats), always text format, null = (const char*)nullptr.
// Callers build the vector of strings (one per $N) + optional null bitmap.

PgResult exec_params(pg_conn* c,
                     const char* sql,
                     const std::vector<std::string>& params,
                     const std::vector<bool>& nulls = {}) {
    std::vector<const char*> vals(params.size(), nullptr);
    for (size_t i = 0; i < params.size(); ++i) {
        bool is_null = (i < nulls.size() && nulls[i]);
        vals[i] = is_null ? nullptr : params[i].c_str();
    }
    PGresult* r = PQexecParams(c, sql,
                               static_cast<int>(params.size()),
                               /*paramTypes=*/nullptr,
                               vals.data(),
                               /*lengths=*/nullptr,
                               /*formats=*/nullptr,
                               /*resultFormat=*/0);
    check_ok(r, c, "exec_params");
    return PgResult{r};
}

PgResult exec_prepared(pg_conn* c,
                       const char* name,
                       const std::vector<std::string>& params,
                       const std::vector<bool>& nulls = {}) {
    std::vector<const char*> vals(params.size(), nullptr);
    for (size_t i = 0; i < params.size(); ++i) {
        bool is_null = (i < nulls.size() && nulls[i]);
        vals[i] = is_null ? nullptr : params[i].c_str();
    }
    PGresult* r = PQexecPrepared(c, name,
                                 static_cast<int>(params.size()),
                                 vals.data(),
                                 /*lengths=*/nullptr,
                                 /*formats=*/nullptr,
                                 /*resultFormat=*/0);
    check_ok(r, c, "exec_prepared");
    return PgResult{r};
}

// Simple query (no params) via PQexec. Used only for DDL bundles that
// have multiple statements separated by semicolons (PQexecParams
// rejects that shape).
PgResult exec_sql(pg_conn* c, const char* sql) {
    PGresult* r = PQexec(c, sql);
    check_ok(r, c, "exec_sql");
    return PgResult{r};
}

// ── Result-row accessors ──────────────────────────────────────────────
//
// All columns come back as text (we use resultFormat=0). Callers
// convert via these helpers. Null cells return empty / default values
// — matches the previous libpqxx::field::is_null + as<T>() shape.

std::string col_text(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return {};
    return std::string(PQgetvalue(r, row, col), PQgetlength(r, row, col));
}
int64_t col_i64(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return 0;
    return std::stoll(PQgetvalue(r, row, col));
}
int col_int(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return 0;
    return std::stoi(PQgetvalue(r, row, col));
}
size_t col_sz(PGresult* r, int row, int col) {
    if (PQgetisnull(r, row, col)) return 0;
    return static_cast<size_t>(std::stoull(PQgetvalue(r, row, col)));
}

// Column index by name — one-time lookup per query. Previous libpqxx
// code wrote row["checkpoint_id"] repeatedly; this helper mirrors that
// ergonomics without re-scanning the column list on every access.
int col_idx(PGresult* r, const char* name) {
    int i = PQfnumber(r, name);
    if (i < 0) throw std::runtime_error(
        std::string("pg result: column not found: ") + name);
    return i;
}

// ── JSON helpers (unchanged from the libpqxx era) ─────────────────────

std::string to_jsonb_text(const json& j) {
    return j.is_null() ? std::string("null") : j.dump();
}

json parse_jsonb_text(std::string_view sv) {
    if (sv.empty()) return json();
    return json::parse(sv);
}

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

// ── Schema DDL (unchanged) ────────────────────────────────────────────

constexpr const char* kSchemaDDL = R"SQL(
CREATE TABLE IF NOT EXISTS neograph_checkpoints (
    thread_id          TEXT    NOT NULL,
    checkpoint_id      TEXT    NOT NULL,
    parent_id          TEXT    NOT NULL DEFAULT '',
    current_node       TEXT    NOT NULL DEFAULT '',
    next_nodes         JSONB   NOT NULL DEFAULT '[]'::jsonb,
    interrupt_phase    TEXT    NOT NULL DEFAULT 'completed',
    barrier_state      JSONB   NOT NULL DEFAULT '{}'::jsonb,
    channel_versions   JSONB   NOT NULL DEFAULT '{}'::jsonb,
    global_version     BIGINT  NOT NULL DEFAULT 0,
    metadata           JSONB   NOT NULL DEFAULT '{}'::jsonb,
    step               BIGINT  NOT NULL DEFAULT 0,
    timestamp_ms       BIGINT  NOT NULL DEFAULT 0,
    schema_version     INT     NOT NULL DEFAULT 2,
    PRIMARY KEY (thread_id, checkpoint_id)
);

CREATE INDEX IF NOT EXISTS neograph_checkpoints_recent
    ON neograph_checkpoints (thread_id, timestamp_ms DESC, step DESC);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_blobs (
    thread_id  TEXT   NOT NULL,
    channel    TEXT   NOT NULL,
    version    BIGINT NOT NULL,
    blob_data  JSONB  NOT NULL,
    PRIMARY KEY (thread_id, channel, version)
);

CREATE TABLE IF NOT EXISTS neograph_checkpoint_writes (
    thread_id            TEXT   NOT NULL,
    parent_checkpoint_id TEXT   NOT NULL,
    seq                  INT    NOT NULL,
    task_id              TEXT   NOT NULL,
    task_path            TEXT   NOT NULL DEFAULT '',
    node_name            TEXT   NOT NULL DEFAULT '',
    writes_json          JSONB  NOT NULL DEFAULT '[]'::jsonb,
    command_json         JSONB  NOT NULL DEFAULT 'null'::jsonb,
    sends_json           JSONB  NOT NULL DEFAULT '[]'::jsonb,
    step                 BIGINT NOT NULL DEFAULT 0,
    timestamp_ms         BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (thread_id, parent_checkpoint_id, task_id, seq)
);
)SQL";

constexpr const char* kDropDDL = R"SQL(
DROP TABLE IF EXISTS neograph_checkpoint_writes;
DROP TABLE IF EXISTS neograph_checkpoint_blobs;
DROP TABLE IF EXISTS neograph_checkpoints;
)SQL";

// ── save-all statement ────────────────────────────────────────────────
//
// CTE-driven — one statement, one round-trip. The blob payload is a
// JSON array of {t,c,v,b} rows; jsonb_to_recordset unpacks it inside
// PG. That's a simpler escape path than building a PG text-array
// literal for vector<string> in C++.
//
// Sent via PQexecParams (not a server-side prepared statement) so
// drop_schema doesn't have to worry about stale cached plans: each
// save() round-trip re-parses, which costs ~100µs vs a ~10ms commit
// fsync — statistical noise on the hot path. The earlier libpqxx
// version used a prepared statement mainly because PQprepare fit
// libpqxx's ergonomics; the libpq rewrite can skip it.

constexpr const char* kSqlSaveAll = R"SQL(
WITH blob_ins AS (
    INSERT INTO neograph_checkpoint_blobs
        (thread_id, channel, version, blob_data)
    SELECT t, c, v, b::jsonb
      FROM jsonb_to_recordset($1::jsonb) AS x(t text, c text, v bigint, b text)
    ON CONFLICT (thread_id, channel, version) DO NOTHING
    RETURNING 1
)
INSERT INTO neograph_checkpoints
    (thread_id, checkpoint_id, parent_id, current_node, next_nodes,
     interrupt_phase, barrier_state, channel_versions, global_version,
     metadata, step, timestamp_ms, schema_version)
VALUES ($2, $3, $4, $5, $6::jsonb, $7, $8::jsonb, $9::jsonb, $10,
        $11::jsonb, $12, $13, $14)
ON CONFLICT (thread_id, checkpoint_id) DO UPDATE SET
    parent_id        = EXCLUDED.parent_id,
    current_node     = EXCLUDED.current_node,
    next_nodes       = EXCLUDED.next_nodes,
    interrupt_phase  = EXCLUDED.interrupt_phase,
    barrier_state    = EXCLUDED.barrier_state,
    channel_versions = EXCLUDED.channel_versions,
    global_version   = EXCLUDED.global_version,
    metadata         = EXCLUDED.metadata,
    step             = EXCLUDED.step,
    timestamp_ms     = EXCLUDED.timestamp_ms,
    schema_version   = EXCLUDED.schema_version
)SQL";

// Selector used by load_latest / load_by_id / list.
constexpr const char* kSelectCols =
    "thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
    "interrupt_phase, barrier_state, channel_versions, global_version, "
    "metadata, step, timestamp_ms, schema_version";

// Row → Checkpoint-shell converter. `r` is a PGresult holding one row;
// caller passes `row = 0` for single-row results or iterates.
Checkpoint row_to_shell(PGresult* r, int row) {
    Checkpoint cp;
    cp.thread_id        = col_text(r, row, col_idx(r, "thread_id"));
    cp.id               = col_text(r, row, col_idx(r, "checkpoint_id"));
    cp.parent_id        = col_text(r, row, col_idx(r, "parent_id"));
    cp.current_node     = col_text(r, row, col_idx(r, "current_node"));
    cp.next_nodes       = next_nodes_from_json(
        parse_jsonb_text(col_text(r, row, col_idx(r, "next_nodes"))));
    cp.interrupt_phase  = parse_checkpoint_phase(
        col_text(r, row, col_idx(r, "interrupt_phase")));
    cp.barrier_state    = barrier_state_from_json(
        parse_jsonb_text(col_text(r, row, col_idx(r, "barrier_state"))));
    cp.metadata         = parse_jsonb_text(
        col_text(r, row, col_idx(r, "metadata")));
    cp.step             = col_i64(r, row, col_idx(r, "step"));
    cp.timestamp        = col_i64(r, row, col_idx(r, "timestamp_ms"));
    cp.schema_version   = col_int(r, row, col_idx(r, "schema_version"));
    return cp;
}

struct LoadedShell {
    Checkpoint cp;
    json channel_versions;
    int64_t global_version = 0;
};

LoadedShell row_to_loaded(PGresult* r, int row) {
    LoadedShell ls;
    ls.cp = row_to_shell(r, row);
    ls.channel_versions = parse_jsonb_text(
        col_text(r, row, col_idx(r, "channel_versions")));
    ls.global_version = col_i64(r, row, col_idx(r, "global_version"));
    return ls;
}

// Fetch blobs for the given (thread, channel, version) triplets named
// in channel_versions. N round trips (one per channel) — same shape as
// the libpqxx version; bound by channel count which is typically small.
std::map<std::string, json> fetch_blobs(pg_conn* c,
                                         const std::string& thread_id,
                                         const json& channel_versions) {
    std::map<std::string, json> out;
    if (!channel_versions.is_object() || channel_versions.empty()) return out;

    std::vector<std::string> names;
    std::vector<int64_t> versions;
    for (auto [name, ver] : channel_versions.items()) {
        names.push_back(name);
        versions.push_back(ver.get<int64_t>());
    }

    const char* sql =
        "SELECT blob_data FROM neograph_checkpoint_blobs "
        "WHERE thread_id = $1 AND channel = $2 AND version = $3";
    for (size_t i = 0; i < names.size(); ++i) {
        std::vector<std::string> params{
            thread_id, names[i], std::to_string(versions[i])};
        auto res = exec_params(c, sql, params);
        if (PQntuples(res) > 0) {
            out.emplace(names[i],
                parse_jsonb_text(col_text(res, 0, 0)));
        }
    }
    return out;
}

Checkpoint finish_load(pg_conn* c, LoadedShell ls) {
    auto blobs = fetch_blobs(c, ls.cp.thread_id, ls.channel_versions);
    ls.cp.channel_values = materialize_channel_values(
        ls.channel_versions, blobs, static_cast<uint64_t>(ls.global_version));
    return ls.cp;
}

// Wrap a raw connection URL open + status check, throwing on failure.
std::unique_ptr<PgConn> open_conn(const std::string& url) {
    pg_conn* raw = PQconnectdb(url.c_str());
    if (!raw || PQstatus(raw) != CONNECTION_OK) {
        std::string msg = "PostgresCheckpointStore: connection failed: ";
        msg += raw ? PQerrorMessage(raw) : "null PGconn";
        if (raw) PQfinish(raw);
        throw std::runtime_error(msg);
    }
    return std::make_unique<PgConn>(raw);
}

} // namespace

// ── Construction / lifetime ───────────────────────────────────────────

PostgresCheckpointStore::PostgresCheckpointStore(const std::string& conn_str,
                                                  size_t pool_size)
    : conn_str_(conn_str) {
    if (pool_size == 0) {
        throw std::invalid_argument(
            "PostgresCheckpointStore: pool_size must be >= 1");
    }
    pool_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        pool_.emplace_back(open_conn(conn_str));
        free_.push(i);
    }
    ensure_schema();
}

PostgresCheckpointStore::~PostgresCheckpointStore() = default;

void PostgresCheckpointStore::ensure_schema() {
    // Direct call on slot 0 — only invoked at construction (and from
    // drop_schema below), so no contention to worry about.
    exec_sql(pool_[0]->raw, kSchemaDDL);
}

void PostgresCheckpointStore::drop_schema() {
    // Drain the pool — DROP TABLE acquires AccessExclusiveLock, so any
    // other in-flight connection holding even a SELECT could deadlock.
    // We can safely take pool_mutex_ here because drop_schema is
    // test-only and never called concurrently with regular ops.
    std::unique_lock lock(pool_mutex_);
    exec_sql(pool_[0]->raw, kDropDDL);
    exec_sql(pool_[0]->raw, kSchemaDDL);
    // No prepared statements to re-register — the libpq rewrite sends
    // save_all via PQexecParams every time, so DROP can't invalidate
    // a cached plan.
}

size_t PostgresCheckpointStore::acquire_slot() {
    std::unique_lock lock(pool_mutex_);
    pool_cv_.wait(lock, [this] { return !free_.empty(); });
    size_t idx = free_.front();
    free_.pop();
    return idx;
}

void PostgresCheckpointStore::release_slot(size_t idx) {
    {
        std::lock_guard lock(pool_mutex_);
        free_.push(idx);
    }
    pool_cv_.notify_one();
}

// Borrow a slot, run fn(PGconn*) on it, release the slot. On a broken-
// connection error, replace the slot's connection with a fresh one
// (using the original URL), re-materialise the schema + prepared
// statements, and retry the operation a single time on the same slot.
// See postgres_checkpoint.h's failure-model docs for the rationale.
template <typename Fn>
auto PostgresCheckpointStore::with_conn(Fn&& fn) {
    size_t idx = acquire_slot();
    struct Releaser {
        PostgresCheckpointStore* self;
        size_t idx;
        ~Releaser() { self->release_slot(idx); }
    } guard{this, idx};

    try {
        return fn(pool_[idx]->raw);
    } catch (const BrokenConnection&) {
        pool_[idx].reset();
        pool_[idx] = open_conn(conn_str_);
        exec_sql(pool_[idx]->raw, kSchemaDDL);
        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        return fn(pool_[idx]->raw);
    }
}

// ── save() ────────────────────────────────────────────────────────────

void PostgresCheckpointStore::save(const Checkpoint& cp) {
    with_conn([&](pg_conn* c) {
        // Build the blob payload as a jsonb array. Each element:
        //   {"t": thread, "c": channel, "v": version, "b": "<json text>"}
        // jsonb_to_recordset on the PG side unpacks it into (text,
        // text, bigint, text) rows and b::jsonb coerces back to jsonb.
        // Empty object list is valid — PG produces zero rows and the
        // blob INSERT becomes a no-op.
        json payload = json::array();
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("channels")) {
            json chs = cp.channel_values["channels"];
            if (chs.is_object()) {
                for (auto [name, ch] : chs.items()) {
                    if (!ch.is_object() || !ch.contains("version")) continue;
                    if (!ch.contains("value")) continue;
                    json row;
                    row["t"] = cp.thread_id;
                    row["c"] = name;
                    row["v"] = ch["version"].get<int64_t>();
                    row["b"] = to_jsonb_text(ch["value"]);
                    payload.push_back(std::move(row));
                }
            }
        }

        json channel_versions = extract_channel_versions(cp.channel_values);
        int64_t global_version = 0;
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("global_version")) {
            global_version = cp.channel_values["global_version"].get<int64_t>();
        }

        std::vector<std::string> params{
            payload.dump(),                                         // $1  blobs jsonb
            cp.thread_id,                                           // $2
            cp.id,                                                  // $3
            cp.parent_id,                                           // $4
            cp.current_node,                                        // $5
            to_jsonb_text(next_nodes_to_json(cp.next_nodes)),       // $6
            std::string(to_string(cp.interrupt_phase)),             // $7
            to_jsonb_text(barrier_state_to_json(cp.barrier_state)), // $8
            to_jsonb_text(channel_versions),                        // $9
            std::to_string(global_version),                         // $10
            to_jsonb_text(cp.metadata),                             // $11
            std::to_string(cp.step),                                // $12
            std::to_string(cp.timestamp),                           // $13
            std::to_string(cp.schema_version),                      // $14
        };
        (void)exec_params(c, kSqlSaveAll, params);
    });
}

// ── load_latest / load_by_id / list / delete_thread ───────────────────

std::optional<Checkpoint> PostgresCheckpointStore::load_latest(
    const std::string& thread_id) {
    return with_conn([&](pg_conn* c) -> std::optional<Checkpoint> {
        std::string sql = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = $1 "
            "ORDER BY timestamp_ms DESC, step DESC LIMIT 1";
        auto res = exec_params(c, sql.c_str(), {thread_id});
        if (PQntuples(res) == 0) return std::nullopt;
        return finish_load(c, row_to_loaded(res, 0));
    });
}

std::optional<Checkpoint> PostgresCheckpointStore::load_by_id(
    const std::string& id) {
    return with_conn([&](pg_conn* c) -> std::optional<Checkpoint> {
        std::string sql = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE checkpoint_id = $1 LIMIT 1";
        auto res = exec_params(c, sql.c_str(), {id});
        if (PQntuples(res) == 0) return std::nullopt;
        return finish_load(c, row_to_loaded(res, 0));
    });
}

std::vector<Checkpoint> PostgresCheckpointStore::list(
    const std::string& thread_id, int limit) {
    return with_conn([&](pg_conn* c) {
        std::string sql = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = $1 "
            "ORDER BY timestamp_ms DESC, step DESC LIMIT $2";
        std::vector<std::string> params{thread_id, std::to_string(limit)};
        auto res = exec_params(c, sql.c_str(), params);
        std::vector<Checkpoint> out;
        int n = PQntuples(res);
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            out.push_back(finish_load(c, row_to_loaded(res, i)));
        }
        return out;
    });
}

void PostgresCheckpointStore::delete_thread(const std::string& thread_id) {
    with_conn([&](pg_conn* c) {
        // Order: writes → blobs → checkpoints. No FKs declared, but
        // this order makes the intent obvious and matches the
        // referential dependency direction.
        (void)exec_params(c,
            "DELETE FROM neograph_checkpoint_writes WHERE thread_id = $1",
            {thread_id});
        (void)exec_params(c,
            "DELETE FROM neograph_checkpoint_blobs WHERE thread_id = $1",
            {thread_id});
        (void)exec_params(c,
            "DELETE FROM neograph_checkpoints WHERE thread_id = $1",
            {thread_id});
    });
}

// ── Pending writes ────────────────────────────────────────────────────

void PostgresCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    with_conn([&](pg_conn* c) {
        // seq allocation + INSERT must be in one transaction so
        // concurrent puts on the same parent don't collide on seq.
        (void)exec_sql(c, "BEGIN");
        try {
            auto seq_res = exec_params(c,
                "SELECT COALESCE(MAX(seq), -1) + 1 AS next_seq "
                "FROM neograph_checkpoint_writes "
                "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
                {thread_id, parent_checkpoint_id});
            int next_seq = col_int(seq_res, 0, 0);

            (void)exec_params(c,
                "INSERT INTO neograph_checkpoint_writes "
                "(thread_id, parent_checkpoint_id, seq, task_id, task_path, "
                " node_name, writes_json, command_json, sends_json, step, "
                " timestamp_ms) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9::jsonb, "
                "        $10, $11)",
                {thread_id, parent_checkpoint_id, std::to_string(next_seq),
                 write.task_id, write.task_path, write.node_name,
                 to_jsonb_text(write.writes),
                 to_jsonb_text(write.command),
                 to_jsonb_text(write.sends),
                 std::to_string(write.step),
                 std::to_string(write.timestamp)});
            (void)exec_sql(c, "COMMIT");
        } catch (...) {
            // Best-effort rollback; ignore errors here since the
            // outer exception is the real one.
            PGresult* r = PQexec(c, "ROLLBACK");
            if (r) PQclear(r);
            throw;
        }
    });
}

std::vector<PendingWrite> PostgresCheckpointStore::get_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    return with_conn([&](pg_conn* c) {
        auto res = exec_params(c,
            "SELECT task_id, task_path, node_name, writes_json, command_json, "
            "       sends_json, step, timestamp_ms "
            "FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2 "
            "ORDER BY seq ASC",
            {thread_id, parent_checkpoint_id});
        std::vector<PendingWrite> out;
        int n = PQntuples(res);
        out.reserve(n);
        int task_id_col   = col_idx(res, "task_id");
        int task_path_col = col_idx(res, "task_path");
        int node_name_col = col_idx(res, "node_name");
        int writes_col    = col_idx(res, "writes_json");
        int command_col   = col_idx(res, "command_json");
        int sends_col     = col_idx(res, "sends_json");
        int step_col      = col_idx(res, "step");
        int ts_col        = col_idx(res, "timestamp_ms");
        for (int i = 0; i < n; ++i) {
            PendingWrite pw;
            pw.task_id   = col_text(res, i, task_id_col);
            pw.task_path = col_text(res, i, task_path_col);
            pw.node_name = col_text(res, i, node_name_col);
            pw.writes    = parse_jsonb_text(col_text(res, i, writes_col));
            pw.command   = parse_jsonb_text(col_text(res, i, command_col));
            pw.sends     = parse_jsonb_text(col_text(res, i, sends_col));
            pw.step      = col_i64(res, i, step_col);
            pw.timestamp = col_i64(res, i, ts_col);
            out.push_back(std::move(pw));
        }
        return out;
    });
}

void PostgresCheckpointStore::clear_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    with_conn([&](pg_conn* c) {
        (void)exec_params(c,
            "DELETE FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
            {thread_id, parent_checkpoint_id});
    });
}

size_t PostgresCheckpointStore::blob_count() {
    return with_conn([&](pg_conn* c) -> size_t {
        auto res = exec_params(c,
            "SELECT COUNT(*) AS n FROM neograph_checkpoint_blobs", {});
        return col_sz(res, 0, 0);
    });
}

// ─────────────────────────────────────────────────────────────────────
// Async path — libpq nonblocking mode driven by asio::posix
// ─────────────────────────────────────────────────────────────────────
//
// Contract mirrors the sync helpers above, but every wait (flush,
// consume) yields via `co_await sock.async_wait(...)`. The connection
// is temporarily flipped to nonblocking for the exchange and flipped
// back on every exit path — so the pool slot remains usable by the
// sync path after the async call returns.
//
// One wire quirk worth noting: asio::posix::stream_descriptor tries to
// close its fd on destruction. We wrap PQsocket(conn) without wanting
// that — the fd belongs to the PGconn, PQfinish closes it. A RAII
// `release()` guard strips the ownership before the descriptor's
// destructor runs, so the fd survives.

namespace {

asio::awaitable<PgResult> exec_params_async(
    pg_conn* c,
    const char* sql,
    const std::vector<std::string>& params,
    const std::vector<bool>& nulls = {}) {

    auto ex = co_await asio::this_coro::executor;

    std::vector<const char*> vals(params.size(), nullptr);
    for (size_t i = 0; i < params.size(); ++i) {
        bool is_null = (i < nulls.size() && nulls[i]);
        vals[i] = is_null ? nullptr : params[i].c_str();
    }

    // Flip to nonblocking for this exchange; restore on every exit
    // (exception, cancellation, normal return) so subsequent sync
    // calls on this slot still work.
    if (PQsetnonblocking(c, 1) != 0) {
        throw std::runtime_error(
            std::string("PQsetnonblocking(1): ") + PQerrorMessage(c));
    }
    struct ModeRestore {
        pg_conn* c;
        ~ModeRestore() { PQsetnonblocking(c, 0); }
    } restore{c};

    int send_r = PQsendQueryParams(c, sql,
                                    static_cast<int>(params.size()),
                                    /*paramTypes=*/nullptr, vals.data(),
                                    /*lengths=*/nullptr,
                                    /*formats=*/nullptr,
                                    /*resultFormat=*/0);
    if (send_r != 1) {
        std::string msg = std::string("PQsendQueryParams: ")
                        + PQerrorMessage(c);
        if (PQstatus(c) != CONNECTION_OK) throw BrokenConnection(msg);
        throw std::runtime_error(msg);
    }

    // Wrap PQsocket without taking ownership — asio would close it
    // on destruction otherwise, and the PGconn needs to keep it
    // across calls. release() before unwind strips the ownership.
#ifdef _WIN32
    // On Windows PQsocket returns int but the underlying handle is
    // a SOCKET (UINT_PTR). Explicit cast back to the asio native
    // handle type silences the narrowing warning and keeps us safe
    // on 64-bit where SOCKET values can exceed INT_MAX.
    asio::ip::tcp::socket sock(ex);
    using NativeSock = asio::ip::tcp::socket::native_handle_type;
    sock.assign(asio::ip::tcp::v4(), static_cast<NativeSock>(PQsocket(c)));
    struct SockRelease {
        asio::ip::tcp::socket& s;
        ~SockRelease() { try { (void)s.release(); } catch (...) {} }
    } rel{sock};
    constexpr auto kWaitWrite = asio::ip::tcp::socket::wait_write;
    constexpr auto kWaitRead  = asio::ip::tcp::socket::wait_read;
#else
    asio::posix::stream_descriptor sock(ex, PQsocket(c));
    struct SockRelease {
        asio::posix::stream_descriptor& s;
        ~SockRelease() { try { s.release(); } catch (...) {} }
    } rel{sock};
    constexpr auto kWaitWrite = asio::posix::stream_descriptor::wait_write;
    constexpr auto kWaitRead  = asio::posix::stream_descriptor::wait_read;
#endif

    // Flush loop: PQflush returns 0 (done), 1 (more to send), -1 (error).
    while (true) {
        int f = PQflush(c);
        if (f == 0) break;
        if (f < 0) {
            std::string msg = std::string("PQflush: ") + PQerrorMessage(c);
            if (PQstatus(c) != CONNECTION_OK) throw BrokenConnection(msg);
            throw std::runtime_error(msg);
        }
        co_await sock.async_wait(kWaitWrite, asio::use_awaitable);
    }

    // Consume loop: PQisBusy returns 1 while results are still on the
    // wire. Wait for readable, pull bytes in via PQconsumeInput, repeat.
    while (PQisBusy(c)) {
        co_await sock.async_wait(kWaitRead, asio::use_awaitable);
        if (PQconsumeInput(c) != 1) {
            std::string msg = std::string("PQconsumeInput: ")
                            + PQerrorMessage(c);
            if (PQstatus(c) != CONNECTION_OK) throw BrokenConnection(msg);
            throw std::runtime_error(msg);
        }
    }

    // Collect the single result (non-pipeline sends produce exactly
    // one). Drain defensively in case the wire had spurious extras.
    PGresult* res = PQgetResult(c);
    while (auto* next = PQgetResult(c)) PQclear(next);
    if (!res) {
        throw std::runtime_error("PQgetResult: no result");
    }
    check_ok(res, c, "exec_params_async");  // throws BrokenConnection or runtime_error
    co_return PgResult{res};
}

} // namespace

// Async peer of with_conn. Template — definition here (same TU as the
// only instantiation points) so the Fn → awaitable<T> inference works
// without explicit instantiation. The retry-on-broken-connection path
// avoids co_await inside a catch block (GCC 13 ICE): the catch marks
// `need_retry = true`, then the retry co_await happens outside the try.
template <typename Fn>
auto PostgresCheckpointStore::with_conn_async(Fn fn)
    -> decltype(fn(std::declval<pg_conn*>())) {
    size_t idx = acquire_slot();
    struct Releaser {
        PostgresCheckpointStore* self;
        size_t idx;
        ~Releaser() { self->release_slot(idx); }
    } guard{this, idx};

    bool need_retry = false;
    // First attempt under try — any co_await happens here, not in catch.
    try {
        co_return co_await fn(pool_[idx]->raw);
    } catch (const BrokenConnection&) {
        need_retry = true;
    }

    // need_retry == true (otherwise co_return above already happened).
    // Replace the slot's connection synchronously (cold path, happens
    // at most once per pool slot per failure — not a throughput hot
    // point) and re-run.
    pool_[idx].reset();
    pool_[idx] = open_conn(conn_str_);
    exec_sql(pool_[idx]->raw, kSchemaDDL);
    reconnect_count_.fetch_add(1, std::memory_order_relaxed);
    co_return co_await fn(pool_[idx]->raw);
}

// ── Async method overrides ────────────────────────────────────────────

asio::awaitable<void> PostgresCheckpointStore::save_async(const Checkpoint& cp) {
    // GCC-13 ICE workaround: build all the params outside the
    // coroutine-awaitable lambda captured below. Nested brace-init
    // inside a coroutine body trips build_special_member_call.
    json payload = json::array();
    if (cp.channel_values.is_object() &&
        cp.channel_values.contains("channels")) {
        json chs = cp.channel_values["channels"];
        if (chs.is_object()) {
            for (auto [name, ch] : chs.items()) {
                if (!ch.is_object() || !ch.contains("version")) continue;
                if (!ch.contains("value")) continue;
                json row;
                row["t"] = cp.thread_id;
                row["c"] = name;
                row["v"] = ch["version"].get<int64_t>();
                row["b"] = to_jsonb_text(ch["value"]);
                payload.push_back(std::move(row));
            }
        }
    }

    json channel_versions = extract_channel_versions(cp.channel_values);
    int64_t global_version = 0;
    if (cp.channel_values.is_object() &&
        cp.channel_values.contains("global_version")) {
        global_version = cp.channel_values["global_version"].get<int64_t>();
    }

    std::vector<std::string> params{
        payload.dump(),
        cp.thread_id,
        cp.id,
        cp.parent_id,
        cp.current_node,
        to_jsonb_text(next_nodes_to_json(cp.next_nodes)),
        std::string(to_string(cp.interrupt_phase)),
        to_jsonb_text(barrier_state_to_json(cp.barrier_state)),
        to_jsonb_text(channel_versions),
        std::to_string(global_version),
        to_jsonb_text(cp.metadata),
        std::to_string(cp.step),
        std::to_string(cp.timestamp),
        std::to_string(cp.schema_version),
    };

    co_await with_conn_async([&](pg_conn* c) -> asio::awaitable<void> {
        (void)co_await exec_params_async(c, kSqlSaveAll, params);
        co_return;
    });
}

asio::awaitable<std::optional<Checkpoint>>
PostgresCheckpointStore::load_latest_async(const std::string& thread_id) {
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE thread_id = $1 "
        "ORDER BY timestamp_ms DESC, step DESC LIMIT 1";
    // GCC 13 ICE workaround: build the param vector outside the
    // coroutine lambda; brace-init inside a coroutine body hits
    // build_special_member_call.
    std::vector<std::string> params{thread_id};

    co_return co_await with_conn_async(
        [&, sql, params](pg_conn* c) -> asio::awaitable<std::optional<Checkpoint>> {
            auto res = co_await exec_params_async(c, sql.c_str(), params);
            if (PQntuples(res) == 0) co_return std::nullopt;
            // Blob fetch still sync on the same connection — keeps
            // the load hot path simple. For a thread with 10+ channels
            // a future pipeline-mode batch could reduce round-trips,
            // but typical channel counts are 2-5 so it's not load-bearing.
            co_return finish_load(c, row_to_loaded(res, 0));
        });
}

asio::awaitable<std::optional<Checkpoint>>
PostgresCheckpointStore::load_by_id_async(const std::string& id) {
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE checkpoint_id = $1 LIMIT 1";
    std::vector<std::string> params{id};

    co_return co_await with_conn_async(
        [&, sql, params](pg_conn* c) -> asio::awaitable<std::optional<Checkpoint>> {
            auto res = co_await exec_params_async(c, sql.c_str(), params);
            if (PQntuples(res) == 0) co_return std::nullopt;
            co_return finish_load(c, row_to_loaded(res, 0));
        });
}

asio::awaitable<std::vector<Checkpoint>>
PostgresCheckpointStore::list_async(const std::string& thread_id, int limit) {
    std::string sql = std::string("SELECT ") + kSelectCols +
        " FROM neograph_checkpoints WHERE thread_id = $1 "
        "ORDER BY timestamp_ms DESC, step DESC LIMIT $2";
    std::vector<std::string> params{thread_id, std::to_string(limit)};

    co_return co_await with_conn_async(
        [&, sql](pg_conn* c) -> asio::awaitable<std::vector<Checkpoint>> {
            auto res = co_await exec_params_async(c, sql.c_str(), params);
            std::vector<Checkpoint> out;
            int n = PQntuples(res);
            out.reserve(n);
            for (int i = 0; i < n; ++i) {
                out.push_back(finish_load(c, row_to_loaded(res, i)));
            }
            co_return out;
        });
}

asio::awaitable<void>
PostgresCheckpointStore::delete_thread_async(const std::string& thread_id) {
    std::vector<std::string> params{thread_id};
    co_await with_conn_async([&, params](pg_conn* c) -> asio::awaitable<void> {
        (void)co_await exec_params_async(c,
            "DELETE FROM neograph_checkpoint_writes WHERE thread_id = $1",
            params);
        (void)co_await exec_params_async(c,
            "DELETE FROM neograph_checkpoint_blobs WHERE thread_id = $1",
            params);
        (void)co_await exec_params_async(c,
            "DELETE FROM neograph_checkpoints WHERE thread_id = $1",
            params);
        co_return;
    });
}

asio::awaitable<void> PostgresCheckpointStore::put_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {

    // Build params outside the coroutine lambda (GCC 13 ICE avoidance).
    std::vector<std::string> insert_params{
        thread_id, parent_checkpoint_id, "",  // $3 = seq — filled per-attempt
        write.task_id, write.task_path, write.node_name,
        to_jsonb_text(write.writes),
        to_jsonb_text(write.command),
        to_jsonb_text(write.sends),
        std::to_string(write.step),
        std::to_string(write.timestamp),
    };

    std::vector<std::string> seq_params{thread_id, parent_checkpoint_id};
    std::vector<std::string> empty_params{};
    co_await with_conn_async([&, insert_params, seq_params, empty_params]
                             (pg_conn* c) mutable
                             -> asio::awaitable<void> {
        // Async BEGIN/COMMIT — matches sync put_writes's seq+INSERT
        // transactionality so concurrent puts on the same parent
        // don't collide.
        (void)co_await exec_params_async(c, "BEGIN", empty_params);
        try {
            auto seq_res = co_await exec_params_async(c,
                "SELECT COALESCE(MAX(seq), -1) + 1 AS next_seq "
                "FROM neograph_checkpoint_writes "
                "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
                seq_params);
            int next_seq = col_int(seq_res, 0, 0);
            insert_params[2] = std::to_string(next_seq);

            (void)co_await exec_params_async(c,
                "INSERT INTO neograph_checkpoint_writes "
                "(thread_id, parent_checkpoint_id, seq, task_id, task_path, "
                " node_name, writes_json, command_json, sends_json, step, "
                " timestamp_ms) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9::jsonb, "
                "        $10, $11)",
                insert_params);

            (void)co_await exec_params_async(c, "COMMIT", empty_params);
        } catch (...) {
            // Best-effort rollback. Can't co_await inside catch (GCC 13
            // ICE), so use the sync exec_sql helper — fast path since
            // connection is still nonblocking-set but PQexec works on
            // either mode.
            PQsetnonblocking(c, 0);
            PGresult* r = PQexec(c, "ROLLBACK");
            if (r) PQclear(r);
            throw;
        }
        co_return;
    });
}

asio::awaitable<std::vector<PendingWrite>>
PostgresCheckpointStore::get_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {

    std::vector<std::string> params{thread_id, parent_checkpoint_id};
    co_return co_await with_conn_async(
        [&, params](pg_conn* c) -> asio::awaitable<std::vector<PendingWrite>> {
            auto res = co_await exec_params_async(c,
                "SELECT task_id, task_path, node_name, writes_json, command_json, "
                "       sends_json, step, timestamp_ms "
                "FROM neograph_checkpoint_writes "
                "WHERE thread_id = $1 AND parent_checkpoint_id = $2 "
                "ORDER BY seq ASC",
                params);
            std::vector<PendingWrite> out;
            int n = PQntuples(res);
            out.reserve(n);
            int task_id_col   = col_idx(res, "task_id");
            int task_path_col = col_idx(res, "task_path");
            int node_name_col = col_idx(res, "node_name");
            int writes_col    = col_idx(res, "writes_json");
            int command_col   = col_idx(res, "command_json");
            int sends_col     = col_idx(res, "sends_json");
            int step_col      = col_idx(res, "step");
            int ts_col        = col_idx(res, "timestamp_ms");
            for (int i = 0; i < n; ++i) {
                PendingWrite pw;
                pw.task_id   = col_text(res, i, task_id_col);
                pw.task_path = col_text(res, i, task_path_col);
                pw.node_name = col_text(res, i, node_name_col);
                pw.writes    = parse_jsonb_text(col_text(res, i, writes_col));
                pw.command   = parse_jsonb_text(col_text(res, i, command_col));
                pw.sends     = parse_jsonb_text(col_text(res, i, sends_col));
                pw.step      = col_i64(res, i, step_col);
                pw.timestamp = col_i64(res, i, ts_col);
                out.push_back(std::move(pw));
            }
            co_return out;
        });
}

asio::awaitable<void> PostgresCheckpointStore::clear_writes_async(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    std::vector<std::string> params{thread_id, parent_checkpoint_id};
    co_await with_conn_async([&, params](pg_conn* c) -> asio::awaitable<void> {
        (void)co_await exec_params_async(c,
            "DELETE FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
            params);
        co_return;
    });
}

} // namespace neograph::graph
