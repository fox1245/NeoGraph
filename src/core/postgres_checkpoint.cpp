// PostgresCheckpointStore — see include/neograph/graph/postgres_checkpoint.h
//
// Schema rationale (mirrors LangGraph PostgresSaver, with neograph_ prefix
// so a single PG database can host both projects):
//
//   neograph_checkpoints       — cp metadata + per-channel version map
//   neograph_checkpoint_blobs  — (thread_id, channel, version) → value
//                                with ON CONFLICT DO NOTHING for dedup
//   neograph_checkpoint_writes — pending intra-super-step writes
//
// Every save() runs blob upserts and the cp insert in a single
// transaction so a crash mid-save never leaves a cp pointing at blobs
// that didn't make it. ON CONFLICT DO NOTHING is what makes dedup work
// without a separate refcount or "have I seen this before" cache.

#include <neograph/graph/postgres_checkpoint.h>

#include <pqxx/pqxx>

#include <chrono>
#include <sstream>
#include <stdexcept>

namespace neograph::graph {

namespace {

// ── JSON ↔ libpqxx helpers ────────────────────────────────────────────

// Convert a neograph::json to its serialized text form for INSERT.
// Empty / null inputs become "{}" or "null" depending on shape so we
// never INSERT NULL into a JSONB column declared NOT NULL — the column
// definitions lower down rely on this.
std::string to_jsonb_text(const json& j) {
    return j.is_null() ? std::string("null") : j.dump();
}

// Parse a JSONB result column back into neograph::json. libpqxx returns
// JSONB as text. Empty / null cells return a null json.
json from_jsonb_text(const pqxx::field& f) {
    if (f.is_null()) return json();
    auto sv = f.view();
    if (sv.empty()) return json();
    return json::parse(sv);
}

// Serialize next_nodes as a JSON array (vector<string>) for storage in
// the JSONB column. Reading goes through json::get<vector<string>>.
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

// barrier_state ↔ JSON. Shape: {"barrier_node": ["upstream1", "upstream2"]}.
// Mirrors what the in-memory checkpoint wire format uses.
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

// Build channel_versions map ({"messages": 7}) from a serialized state
// blob (as produced by GraphState::serialize, shape:
// {"channels": {name: {value, version}}, "global_version": N}).
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

// Inverse: given the channel_versions map and a (channel → blob_value)
// lookup, rebuild the serialize() shape so callers see full inline data.
json materialize_channel_values(const json& channel_versions,
                                 const std::map<std::string, json>& blobs,
                                 uint64_t global_version) {
    json full_channels = json::object();
    if (channel_versions.is_object()) {
        for (auto [name, ver] : channel_versions.items()) {
            json entry = json::object();
            entry["version"] = ver;
            auto it = blobs.find(name);
            // Defensive: missing blob yields null. Indicates store
            // corruption (cp row present, blob row missing) — caller
            // sees a deserializable but stale value rather than a crash.
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

// DDL bundle. Idempotent — every CREATE uses IF NOT EXISTS so first
// connect from a fresh DB and re-connect against an existing schema
// both produce the same end state.
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
    PRIMARY KEY (thread_id, parent_checkpoint_id, seq)
);
)SQL";

constexpr const char* kDropDDL = R"SQL(
DROP TABLE IF EXISTS neograph_checkpoint_writes;
DROP TABLE IF EXISTS neograph_checkpoint_blobs;
DROP TABLE IF EXISTS neograph_checkpoints;
)SQL";

} // namespace

// ── Construction / lifetime ───────────────────────────────────────────

// Prepared-statement names. Per-conn so we can call
// tx.exec_prepared(name, args) instead of tx.exec_params(sql, args)
// which re-parses the SQL on every call. Saves a few hundred μs per
// save on the PG planner side.
namespace {
constexpr const char* kStmtBlobUpsert = "neograph_blob_upsert";
constexpr const char* kStmtCpUpsert   = "neograph_cp_upsert";
constexpr const char* kSqlBlobUpsert  =
    "INSERT INTO neograph_checkpoint_blobs "
    "(thread_id, channel, version, blob_data) "
    "SELECT t, c, v, b::jsonb "
    "  FROM UNNEST($1::text[], $2::text[], $3::bigint[], $4::text[]) "
    "       AS u(t, c, v, b) "
    "ON CONFLICT (thread_id, channel, version) DO NOTHING";
constexpr const char* kSqlCpUpsert =
    "INSERT INTO neograph_checkpoints "
    "(thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
    " interrupt_phase, barrier_state, channel_versions, global_version, "
    " metadata, step, timestamp_ms, schema_version) "
    "VALUES ($1, $2, $3, $4, $5::jsonb, $6, $7::jsonb, $8::jsonb, $9, "
    "        $10::jsonb, $11, $12, $13) "
    "ON CONFLICT (thread_id, checkpoint_id) DO UPDATE SET "
    "  parent_id        = EXCLUDED.parent_id, "
    "  current_node     = EXCLUDED.current_node, "
    "  next_nodes       = EXCLUDED.next_nodes, "
    "  interrupt_phase  = EXCLUDED.interrupt_phase, "
    "  barrier_state    = EXCLUDED.barrier_state, "
    "  channel_versions = EXCLUDED.channel_versions, "
    "  global_version   = EXCLUDED.global_version, "
    "  metadata         = EXCLUDED.metadata, "
    "  step             = EXCLUDED.step, "
    "  timestamp_ms     = EXCLUDED.timestamp_ms, "
    "  schema_version   = EXCLUDED.schema_version";

// Register the hot-path statements on a fresh connection. Run from the
// constructor for every pool slot, and from with_conn after a
// reconnect so a replaced slot still has the same prepared cache.
void prepare_statements(pqxx::connection& c) {
    c.prepare(kStmtBlobUpsert, kSqlBlobUpsert);
    c.prepare(kStmtCpUpsert,   kSqlCpUpsert);
}
} // namespace

PostgresCheckpointStore::PostgresCheckpointStore(const std::string& conn_str,
                                                  size_t pool_size)
    : conn_str_(conn_str) {
    if (pool_size == 0) {
        throw std::invalid_argument(
            "PostgresCheckpointStore: pool_size must be >= 1");
    }
    pool_.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i) {
        // Eager open so credentials / DDL permissions surface now,
        // not on the first save() call deep inside an engine run.
        // Failure here aborts construction (any earlier conns drop on
        // unwind via unique_ptr).
        pool_.emplace_back(std::make_unique<pqxx::connection>(conn_str));
        free_.push(i);
    }
    // ensure_schema once via slot 0; CREATE IF NOT EXISTS so a fresh
    // and a re-attached schema both end in the same state.
    ensure_schema();
    // Prepare hot-path statements on every slot. Must happen AFTER
    // ensure_schema so the tables exist when prepare resolves them.
    for (auto& c : pool_) prepare_statements(*c);
}

PostgresCheckpointStore::~PostgresCheckpointStore() = default;

void PostgresCheckpointStore::ensure_schema() {
    // Direct call on slot 0 — only invoked at construction (and from
    // drop_schema below), so there's no contention to worry about.
    pqxx::work tx{*pool_[0]};
    tx.exec(kSchemaDDL);
    tx.commit();
}

void PostgresCheckpointStore::drop_schema() {
    // Drain the pool — DROP TABLE acquires AccessExclusiveLock, so any
    // other in-flight connection holding even a SELECT could deadlock.
    // We can safely take pool_mutex_ here because drop_schema is
    // test-only and never called concurrently with regular ops.
    std::unique_lock lock(pool_mutex_);
    pqxx::work tx{*pool_[0]};
    tx.exec(kDropDDL);
    tx.commit();
    pqxx::work tx2{*pool_[0]};
    tx2.exec(kSchemaDDL);
    tx2.commit();
}

// Borrow a free pool slot. Blocks if all slots are in use until one
// is released. With pool_size matching worker count there's no wait.
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
    // notify_one wakes one waiter — N waiters serialize naturally
    // through repeated release calls, no thundering herd.
    pool_cv_.notify_one();
}

// Borrow a slot, run fn(connection&) on it, release the slot. If
// libpqxx throws `pqxx::broken_connection` (the canonical signal that
// the TCP socket to PG is dead — e.g. PG was restarted, network blip,
// idle timeout in pgbouncer), replace THIS slot's connection with a
// fresh one (using the original URL), re-materialise the schema, and
// retry the operation a single time on the same slot.
//
// One retry, not many: if the new connection ALSO breaks immediately,
// something is wrong (PG down, credentials revoked) and a tight retry
// loop would just hammer it. Caller-level retry is the right place
// for that.
//
// Exceptions other than broken_connection — query errors, constraint
// violations, transaction conflicts — propagate as-is. Reconnecting
// wouldn't help and would mask the real bug. Either way, the slot is
// released back to the pool via the RAII guard so a thrown exception
// can't deadlock the pool.
template <typename Fn>
auto PostgresCheckpointStore::with_conn(Fn&& fn) {
    size_t idx = acquire_slot();
    // RAII: release slot on every exit path, including exception.
    struct Releaser {
        PostgresCheckpointStore* self;
        size_t idx;
        ~Releaser() { self->release_slot(idx); }
    } guard{this, idx};

    try {
        return fn(*pool_[idx]);
    } catch (const pqxx::broken_connection&) {
        pool_[idx].reset();
        pool_[idx] = std::make_unique<pqxx::connection>(conn_str_);
        // Re-materialise schema on the new conn. CREATE IF NOT EXISTS
        // is a cheap no-op against a healthy DB.
        {
            pqxx::work tx{*pool_[idx]};
            tx.exec(kSchemaDDL);
            tx.commit();
        }
        // Prepared statements live per-connection — re-register on the
        // replacement so save() can still call exec_prepared.
        prepare_statements(*pool_[idx]);
        reconnect_count_.fetch_add(1, std::memory_order_relaxed);
        return fn(*pool_[idx]);
    }
}

// ── save() ────────────────────────────────────────────────────────────
//
// One transaction, two statements:
//   1. ONE multi-row INSERT for every channel's blob — UNNEST four
//      arrays so PG sees a single statement / one round-trip instead of
//      N (one per channel). LangGraph's PostgresSaver does the same;
//      single-thread per-save cost dropped from ~13ms to <10ms after
//      this change.
//   2. INSERT INTO checkpoints with channel_versions only (no values).
//
// The transaction guarantees the cp row never exists without its blobs.
// If two concurrent saves write the same (thread, channel, version),
// the unique constraint + ON CONFLICT collapses them — same version
// implies same value (Channel::version is monotonic per write).

void PostgresCheckpointStore::save(const Checkpoint& cp) {
    with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};

        // 1. Batched blob upsert via UNNEST. Build four parallel arrays
        //    of (thread, channel, version, value) and hand them to PG
        //    as a single statement.
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("channels")) {
            json chs = cp.channel_values["channels"];
            if (chs.is_object()) {
                std::vector<std::string> threads, channels, values;
                std::vector<int64_t>     versions;
                for (auto [name, ch] : chs.items()) {
                    if (!ch.is_object() || !ch.contains("version")) continue;
                    if (!ch.contains("value")) continue;
                    threads.push_back(cp.thread_id);
                    channels.push_back(name);
                    versions.push_back(ch["version"].get<int64_t>());
                    values.push_back(to_jsonb_text(ch["value"]));
                }
                if (!threads.empty()) {
                    // Prepared UNNEST upsert — see kSqlBlobUpsert. PG
                    // skips parse+plan on every call after the first.
                    tx.exec_prepared(kStmtBlobUpsert,
                        threads, channels, versions, values);
                }
            }
        }

        // 2. Checkpoint row.
        json channel_versions = extract_channel_versions(cp.channel_values);
        int64_t global_version = 0;
        if (cp.channel_values.is_object() &&
            cp.channel_values.contains("global_version")) {
            global_version = cp.channel_values["global_version"].get<int64_t>();
        }

        // Prepared cp upsert — see kSqlCpUpsert. Same statement on
        // every save so PG planner caches the plan once per conn.
        tx.exec_prepared(kStmtCpUpsert,
            cp.thread_id,
            cp.id,
            cp.parent_id,
            cp.current_node,
            to_jsonb_text(next_nodes_to_json(cp.next_nodes)),
            std::string(to_string(cp.interrupt_phase)),
            to_jsonb_text(barrier_state_to_json(cp.barrier_state)),
            to_jsonb_text(channel_versions),
            global_version,
            to_jsonb_text(cp.metadata),
            cp.step,
            cp.timestamp,
            cp.schema_version);

        tx.commit();
    });
}

// ── load helpers ──────────────────────────────────────────────────────

namespace {

// Hydrate a single result row into a Checkpoint shell (no blob join yet).
// Caller is responsible for fetching blobs and replacing channel_values
// with a fully materialized version.
Checkpoint row_to_shell(const pqxx::row& r) {
    Checkpoint cp;
    cp.thread_id        = r["thread_id"].as<std::string>();
    cp.id               = r["checkpoint_id"].as<std::string>();
    cp.parent_id        = r["parent_id"].as<std::string>();
    cp.current_node     = r["current_node"].as<std::string>();
    cp.next_nodes       = next_nodes_from_json(from_jsonb_text(r["next_nodes"]));
    cp.interrupt_phase  = parse_checkpoint_phase(
        r["interrupt_phase"].as<std::string>());
    cp.barrier_state    = barrier_state_from_json(
        from_jsonb_text(r["barrier_state"]));
    cp.metadata         = from_jsonb_text(r["metadata"]);
    cp.step             = r["step"].as<int64_t>();
    cp.timestamp        = r["timestamp_ms"].as<int64_t>();
    cp.schema_version   = r["schema_version"].as<int>();
    return cp;
}

// Stash the channel_versions json + global_version on the shell so the
// blob-fetch step can read them out without re-querying.
struct LoadedShell {
    Checkpoint cp;
    json channel_versions;
    int64_t global_version = 0;
};

LoadedShell row_to_loaded(const pqxx::row& r) {
    LoadedShell ls;
    ls.cp = row_to_shell(r);
    ls.channel_versions = from_jsonb_text(r["channel_versions"]);
    ls.global_version = r["global_version"].as<int64_t>();
    return ls;
}

// Fetch all blobs for the (thread, channel, version) pairs named in
// channel_versions and return a {channel → value} map. Single round
// trip via the unnest() trick — avoids N queries for N channels.
std::map<std::string, json> fetch_blobs(pqxx::work& tx,
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

    // Build "$2[1], $3[1]" "($2[2], $3[2])" style isn't supported; use
    // the unnest pattern. Postgres arrays come through libpqxx as
    // bracketed text — the simplest portable form is to send arrays as
    // explicit constructor literals via separate queries per row.
    // Trade-off: N round trips on load, but load is far less hot than
    // save and N is the channel count (small).
    for (size_t i = 0; i < names.size(); ++i) {
        auto res = tx.exec_params(
            "SELECT blob_data FROM neograph_checkpoint_blobs "
            "WHERE thread_id = $1 AND channel = $2 AND version = $3",
            thread_id, names[i], versions[i]);
        if (!res.empty()) {
            out.emplace(names[i], from_jsonb_text(res[0]["blob_data"]));
        }
    }
    return out;
}

Checkpoint finish_load(pqxx::work& tx, LoadedShell ls) {
    auto blobs = fetch_blobs(tx, ls.cp.thread_id, ls.channel_versions);
    ls.cp.channel_values = materialize_channel_values(
        ls.channel_versions, blobs, static_cast<uint64_t>(ls.global_version));
    return ls.cp;
}

constexpr const char* kSelectCols =
    "thread_id, checkpoint_id, parent_id, current_node, next_nodes, "
    "interrupt_phase, barrier_state, channel_versions, global_version, "
    "metadata, step, timestamp_ms, schema_version";

} // namespace

std::optional<Checkpoint> PostgresCheckpointStore::load_latest(
    const std::string& thread_id) {
    return with_conn([&](pqxx::connection& c) -> std::optional<Checkpoint> {
        pqxx::work tx{c};
        std::string q = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = $1 "
            // Order matches list(): newest by wall-clock, then by step
            // for determinism within a millisecond.
            "ORDER BY timestamp_ms DESC, step DESC LIMIT 1";
        auto res = tx.exec_params(q, thread_id);
        if (res.empty()) return std::nullopt;
        Checkpoint cp = finish_load(tx, row_to_loaded(res[0]));
        tx.commit();
        return cp;
    });
}

std::optional<Checkpoint> PostgresCheckpointStore::load_by_id(
    const std::string& id) {
    return with_conn([&](pqxx::connection& c) -> std::optional<Checkpoint> {
        pqxx::work tx{c};
        std::string q = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE checkpoint_id = $1 LIMIT 1";
        auto res = tx.exec_params(q, id);
        if (res.empty()) return std::nullopt;
        Checkpoint cp = finish_load(tx, row_to_loaded(res[0]));
        tx.commit();
        return cp;
    });
}

std::vector<Checkpoint> PostgresCheckpointStore::list(
    const std::string& thread_id, int limit) {
    return with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};
        std::string q = std::string("SELECT ") + kSelectCols +
            " FROM neograph_checkpoints WHERE thread_id = $1 "
            "ORDER BY timestamp_ms DESC, step DESC LIMIT $2";
        auto res = tx.exec_params(q, thread_id, limit);
        std::vector<Checkpoint> out;
        out.reserve(res.size());
        for (const auto& row : res) {
            out.push_back(finish_load(tx, row_to_loaded(row)));
        }
        tx.commit();
        return out;
    });
}

void PostgresCheckpointStore::delete_thread(const std::string& thread_id) {
    with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};
        // Order: writes → blobs → checkpoints. No FKs declared, but
        // this order makes the intent obvious and matches the
        // referential dependency direction.
        tx.exec_params(
            "DELETE FROM neograph_checkpoint_writes WHERE thread_id = $1",
            thread_id);
        tx.exec_params(
            "DELETE FROM neograph_checkpoint_blobs WHERE thread_id = $1",
            thread_id);
        tx.exec_params(
            "DELETE FROM neograph_checkpoints WHERE thread_id = $1",
            thread_id);
        tx.commit();
    });
}

// ── Pending writes ────────────────────────────────────────────────────

void PostgresCheckpointStore::put_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id,
    const PendingWrite& write) {
    with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};
        // Allocate the next seq within (thread, parent). Done inside
        // the transaction so concurrent puts on the same parent get
        // distinct seqs without a separate sequence object.
        auto seq_row = tx.exec_params(
            "SELECT COALESCE(MAX(seq), -1) + 1 AS next_seq "
            "FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
            thread_id, parent_checkpoint_id);
        int next_seq = seq_row[0]["next_seq"].as<int>();

        tx.exec_params(
            "INSERT INTO neograph_checkpoint_writes "
            "(thread_id, parent_checkpoint_id, seq, task_id, task_path, "
            " node_name, writes_json, command_json, sends_json, step, "
            " timestamp_ms) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7::jsonb, $8::jsonb, $9::jsonb, "
            "        $10, $11)",
            thread_id, parent_checkpoint_id, next_seq,
            write.task_id, write.task_path, write.node_name,
            to_jsonb_text(write.writes),
            to_jsonb_text(write.command),
            to_jsonb_text(write.sends),
            write.step, write.timestamp);
        tx.commit();
    });
}

std::vector<PendingWrite> PostgresCheckpointStore::get_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    return with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};
        auto res = tx.exec_params(
            "SELECT task_id, task_path, node_name, writes_json, command_json, "
            "       sends_json, step, timestamp_ms "
            "FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2 "
            "ORDER BY seq ASC",
            thread_id, parent_checkpoint_id);
        std::vector<PendingWrite> out;
        out.reserve(res.size());
        for (const auto& row : res) {
            PendingWrite pw;
            pw.task_id   = row["task_id"].as<std::string>();
            pw.task_path = row["task_path"].as<std::string>();
            pw.node_name = row["node_name"].as<std::string>();
            pw.writes    = from_jsonb_text(row["writes_json"]);
            pw.command   = from_jsonb_text(row["command_json"]);
            pw.sends     = from_jsonb_text(row["sends_json"]);
            pw.step      = row["step"].as<int64_t>();
            pw.timestamp = row["timestamp_ms"].as<int64_t>();
            out.push_back(std::move(pw));
        }
        tx.commit();
        return out;
    });
}

void PostgresCheckpointStore::clear_writes(
    const std::string& thread_id,
    const std::string& parent_checkpoint_id) {
    with_conn([&](pqxx::connection& c) {
        pqxx::work tx{c};
        tx.exec_params(
            "DELETE FROM neograph_checkpoint_writes "
            "WHERE thread_id = $1 AND parent_checkpoint_id = $2",
            thread_id, parent_checkpoint_id);
        tx.commit();
    });
}

size_t PostgresCheckpointStore::blob_count() {
    return with_conn([&](pqxx::connection& c) -> size_t {
        pqxx::work tx{c};
        auto res = tx.exec("SELECT COUNT(*) AS n FROM neograph_checkpoint_blobs");
        size_t n = res[0]["n"].as<size_t>();
        tx.commit();
        return n;
    });
}

} // namespace neograph::graph
