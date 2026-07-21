#include <neograph/mcp/sqlite_harness_store.h>

#include "harness_journal_internal.h"
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::mcp {
namespace {

[[noreturn]] void throw_sqlite_error(sqlite3* db, const char* operation) {
    throw std::runtime_error(std::string("SqliteHarnessRecordStore: ") + operation + ": " +
                             (db ? sqlite3_errmsg(db) : "out of memory"));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw_sqlite_error(db, "prepare failed");
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&)            = delete;
    Statement& operator=(const Statement&) = delete;

    void bind_text(int index, const std::string& value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind failed");
        }
    }

    void bind_int64(int index, int64_t value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw_sqlite_error(db_, "bind failed");
        }
    }

    int step() { return sqlite3_step(statement_); }

    std::string text(int column) const {
        const auto* value = sqlite3_column_text(statement_, column);
        if (!value) return {};
        return {reinterpret_cast<const char*>(value),
                static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
    }

    int integer(int column) const { return sqlite3_column_int(statement_, column); }
    int64_t int64(int column) const { return sqlite3_column_int64(statement_, column); }

private:
    sqlite3*      db_        = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void validate_id(const std::string& id) {
    if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' || character == '_';
        })) {
        throw std::invalid_argument("invalid Harness record identifier");
    }
}

int64_t unix_millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

json redacted_json(const json& value, const std::set<std::string>& redacted_keys) {
    if (value.is_object()) {
        json result = json::object();
        for (const auto [key, child] : value.items()) {
            if (redacted_keys.contains(lowercase(key))) {
                result[key] = detail::kHarnessRedactedMarker;
            } else {
                result[key] = redacted_json(child, redacted_keys);
            }
        }
        return result;
    }
    if (value.is_array()) {
        json result = json::array();
        for (const auto child : value) result.push_back(redacted_json(child, redacted_keys));
        return result;
    }
    return value;
}

}  // namespace

struct SqliteHarnessRecordStore::Impl {
    Impl(const std::string&              db_path,
         std::chrono::milliseconds       busy_timeout,
         SqliteHarnessJournalConfig journal_config_value)
        : journal_config(std::move(journal_config_value)) {
        if (db_path.empty()) {
            throw std::invalid_argument("SqliteHarnessRecordStore path must not be empty");
        }
        if (busy_timeout.count() < 0 || busy_timeout.count() > std::numeric_limits<int>::max()) {
            throw std::invalid_argument("SqliteHarnessRecordStore busy timeout is out of range");
        }
        if (sqlite3_open_v2(db_path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = std::string("SqliteHarnessRecordStore: open failed: ") +
                                        (db ? sqlite3_errmsg(db) : "out of memory");
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error(message);
        }

        try {
            // SQLite documents that this handler waits for transient locks until
            // the configured budget is exhausted.
            // https://www.sqlite.org/c3ref/busy_timeout.html (fetched 2026-07-21)
            if (sqlite3_busy_timeout(db, static_cast<int>(busy_timeout.count())) != SQLITE_OK) {
                throw_sqlite_error(db, "cannot configure busy timeout");
            }
            exec("PRAGMA journal_mode=WAL;");
            exec("PRAGMA foreign_keys=ON;");
            exec("PRAGMA synchronous=NORMAL;");
            ensure_schema();
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }

    ~Impl() { sqlite3_close(db); }

    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = "SqliteHarnessRecordStore: SQL execution failed";
            if (error) {
                message += ": ";
                message += error;
                sqlite3_free(error);
            }
            throw std::runtime_error(std::move(message));
        }
    }

    void ensure_schema() {
        exec("BEGIN IMMEDIATE;");
        try {
            exec(R"SQL(
CREATE TABLE IF NOT EXISTS neograph_harness_schema (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    version   INTEGER NOT NULL
);
INSERT INTO neograph_harness_schema (singleton, version)
VALUES (1, 3)
ON CONFLICT(singleton) DO NOTHING;
CREATE TABLE IF NOT EXISTS neograph_harness_artifacts (
    artifact_id  TEXT PRIMARY KEY,
    record_json  TEXT    NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS neograph_harness_runs (
    run_id           TEXT PRIMARY KEY,
    artifact_id      TEXT    NOT NULL,
    revision_digest  TEXT    NOT NULL DEFAULT '',
    protocol_version TEXT    NOT NULL DEFAULT '',
    profile          TEXT    NOT NULL DEFAULT '',
    status           TEXT    NOT NULL DEFAULT '',
    source_run_id    TEXT    NOT NULL DEFAULT '',
    source_checkpoint_id TEXT NOT NULL DEFAULT '',
    record_json      TEXT    NOT NULL,
    updated_at_ms    INTEGER NOT NULL,
    FOREIGN KEY (artifact_id) REFERENCES neograph_harness_artifacts (artifact_id)
        ON DELETE RESTRICT
);
CREATE INDEX IF NOT EXISTS neograph_harness_runs_artifact
    ON neograph_harness_runs (artifact_id);
)SQL");
            Statement version(db,
                              "SELECT version FROM neograph_harness_schema WHERE singleton = 1");
            if (version.step() != SQLITE_ROW) {
                throw std::runtime_error("SqliteHarnessRecordStore: unsupported schema version");
            }
            auto schema_version = version.integer(0);
            if (schema_version == 1) {
                exec(R"SQL(
ALTER TABLE neograph_harness_runs
    ADD COLUMN revision_digest TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN protocol_version TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN profile TEXT NOT NULL DEFAULT '';
UPDATE neograph_harness_schema SET version = 2 WHERE singleton = 1;
)SQL");
                schema_version = 2;
            }
            if (schema_version == 2) {
                exec(R"SQL(
ALTER TABLE neograph_harness_runs
    ADD COLUMN status TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN source_run_id TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN source_checkpoint_id TEXT NOT NULL DEFAULT '';
)SQL");
                std::vector<std::pair<std::string, json>> records;
                Statement rows(db, "SELECT run_id, record_json FROM neograph_harness_runs");
                while (true) {
                    const auto result = rows.step();
                    if (result == SQLITE_DONE) break;
                    if (result != SQLITE_ROW) throw_sqlite_error(db, "run migration read failed");
                    records.emplace_back(rows.text(0), json::parse(rows.text(1)));
                }
                for (const auto& [run_id, record] : records) {
                    Statement update(db,
                                     "UPDATE neograph_harness_runs SET status=?, "
                                     "source_run_id=?, source_checkpoint_id=? WHERE run_id=?");
                    update.bind_text(1, record.value("status", ""));
                    update.bind_text(2, record.value("source_run_id", ""));
                    update.bind_text(3, record.value("source_checkpoint_id", ""));
                    update.bind_text(4, run_id);
                    if (update.step() != SQLITE_DONE) {
                        throw_sqlite_error(db, "run migration write failed");
                    }
                }
                exec("UPDATE neograph_harness_schema SET version = 3 WHERE singleton = 1;");
                schema_version = 3;
            }
            if (schema_version != 3) {
                throw std::runtime_error("SqliteHarnessRecordStore: unsupported schema version");
            }
            exec(R"SQL(
CREATE INDEX IF NOT EXISTS neograph_harness_runs_status
    ON neograph_harness_runs (status, updated_at_ms);
CREATE INDEX IF NOT EXISTS neograph_harness_runs_source
    ON neograph_harness_runs (source_run_id)
    WHERE source_run_id <> '';
CREATE TABLE IF NOT EXISTS neograph_harness_journal (
    journal_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id           TEXT    NOT NULL,
    sequence         INTEGER NOT NULL,
    artifact_id      TEXT    NOT NULL,
    revision_digest  TEXT    NOT NULL,
    protocol_version TEXT    NOT NULL,
    profile          TEXT    NOT NULL,
    event_type       TEXT    NOT NULL,
    correlation_id   TEXT    NOT NULL DEFAULT '',
    node_id          TEXT    NOT NULL DEFAULT '',
    worker_id        TEXT    NOT NULL DEFAULT '',
    attempt          INTEGER NOT NULL DEFAULT 0,
    payload_json     TEXT    NOT NULL,
    created_at_ms    INTEGER NOT NULL,
    UNIQUE (run_id, sequence),
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
CREATE INDEX IF NOT EXISTS neograph_harness_journal_run
    ON neograph_harness_journal (run_id, sequence);
CREATE INDEX IF NOT EXISTS neograph_harness_journal_correlation
    ON neograph_harness_journal (correlation_id)
    WHERE correlation_id <> '';
)SQL");
            exec("COMMIT;");
        } catch (...) {
            try {
                exec("ROLLBACK;");
            } catch (...) {}
            throw;
        }
    }

    std::optional<json> load(const char* table, const char* id_column, const std::string& id) {
        validate_id(id);
        std::lock_guard   lock(mutex);
        const std::string sql =
            std::string("SELECT record_json FROM ") + table + " WHERE " + id_column + " = ?";
        Statement statement(db, sql.c_str());
        statement.bind_text(1, id);
        const auto result = statement.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(db, "read failed");
        return json::parse(statement.text(0));
    }

    sqlite3*   db = nullptr;
    std::mutex mutex;
    SqliteHarnessJournalConfig journal_config;
};

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string& db_path)
    : SqliteHarnessRecordStore(db_path, std::chrono::seconds(5), {}) {}

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string&        db_path,
                                                    std::chrono::milliseconds busy_timeout)
    : SqliteHarnessRecordStore(db_path, busy_timeout, {}) {}

SqliteHarnessRecordStore::SqliteHarnessRecordStore(
    const std::string& db_path,
    std::chrono::milliseconds busy_timeout,
    SqliteHarnessJournalConfig journal_config)
    : impl_(std::make_unique<Impl>(db_path, busy_timeout, std::move(journal_config))) {}

SqliteHarnessRecordStore::~SqliteHarnessRecordStore() = default;

void SqliteHarnessRecordStore::save_artifact(const std::string& artifact_id, const json& record) {
    validate_id(artifact_id);
    if (record.value("artifact_id", "") != artifact_id) {
        throw std::invalid_argument("Harness artifact record id mismatch");
    }
    const auto      serialized = record.dump();
    std::lock_guard lock(impl_->mutex);
    Statement       insert(impl_->db,
                           "INSERT INTO neograph_harness_artifacts "
                                 "(artifact_id, record_json, created_at_ms) VALUES (?, ?, ?) "
                                 "ON CONFLICT(artifact_id) DO NOTHING");
    insert.bind_text(1, artifact_id);
    insert.bind_text(2, serialized);
    insert.bind_int64(3, unix_millis());
    if (insert.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "artifact write failed");
    if (sqlite3_changes(impl_->db) == 1) return;

    Statement existing(impl_->db,
                       "SELECT record_json FROM neograph_harness_artifacts WHERE artifact_id = ?");
    existing.bind_text(1, artifact_id);
    if (existing.step() != SQLITE_ROW || existing.text(0) != serialized) {
        throw std::invalid_argument("Harness artifacts are immutable");
    }
}

std::optional<json> SqliteHarnessRecordStore::load_artifact(const std::string& artifact_id) {
    return impl_->load("neograph_harness_artifacts", "artifact_id", artifact_id);
}

void SqliteHarnessRecordStore::save_run(const std::string& run_id, const json& record) {
    validate_id(run_id);
    if (record.value("run_id", "") != run_id || !record.contains("artifact_id") ||
        !record["artifact_id"].is_string()) {
        throw std::invalid_argument("Harness run record id or artifact id mismatch");
    }
    const auto artifact_id = record["artifact_id"].get<std::string>();
    validate_id(artifact_id);
    const auto revision_digest = record.value("revision_digest", "");
    const auto protocol_version = record.value("protocol_version", "");
    const auto profile = record.value("profile", "");
    const auto status               = record.value("status", "");
    const auto source_run_id        = record.value("source_run_id", "");
    const auto source_checkpoint_id = record.value("source_checkpoint_id", "");
    if (!source_run_id.empty()) {
        validate_id(source_run_id);
        if (source_run_id == run_id) {
            throw std::invalid_argument("Harness run cannot reference itself as a source");
        }
    }
    if (!source_checkpoint_id.empty()) validate_id(source_checkpoint_id);
    std::lock_guard lock(impl_->mutex);
    Statement       save(
        impl_->db,
        "INSERT INTO neograph_harness_runs "
              "(run_id, artifact_id, revision_digest, protocol_version, profile, "
              "status, source_run_id, source_checkpoint_id, record_json, "
              "updated_at_ms) SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
              "WHERE ?='' OR EXISTS (SELECT 1 FROM neograph_harness_runs "
              "WHERE run_id=?) "
              "ON CONFLICT(run_id) DO UPDATE SET "
              "revision_digest=CASE WHEN neograph_harness_runs.revision_digest='' "
              "THEN excluded.revision_digest ELSE neograph_harness_runs.revision_digest END, "
              "protocol_version=CASE WHEN neograph_harness_runs.protocol_version='' "
              "THEN excluded.protocol_version ELSE neograph_harness_runs.protocol_version END, "
              "profile=CASE WHEN neograph_harness_runs.profile='' THEN excluded.profile "
              "ELSE neograph_harness_runs.profile END, "
              "status=excluded.status, "
              "source_run_id=CASE WHEN neograph_harness_runs.source_run_id='' "
              "THEN excluded.source_run_id ELSE neograph_harness_runs.source_run_id END, "
              "source_checkpoint_id=CASE WHEN "
              "neograph_harness_runs.source_checkpoint_id='' "
              "THEN excluded.source_checkpoint_id ELSE "
              "neograph_harness_runs.source_checkpoint_id END, "
              "record_json=excluded.record_json, updated_at_ms=excluded.updated_at_ms "
              "WHERE neograph_harness_runs.artifact_id=excluded.artifact_id "
              "AND (neograph_harness_runs.revision_digest='' OR "
              "neograph_harness_runs.revision_digest=excluded.revision_digest) "
              "AND (neograph_harness_runs.protocol_version='' OR "
              "neograph_harness_runs.protocol_version=excluded.protocol_version) "
              "AND (neograph_harness_runs.profile='' OR "
              "neograph_harness_runs.profile=excluded.profile) "
              "AND (neograph_harness_runs.source_run_id='' OR "
              "neograph_harness_runs.source_run_id=excluded.source_run_id) "
              "AND (neograph_harness_runs.source_checkpoint_id='' OR "
              "neograph_harness_runs.source_checkpoint_id="
              "excluded.source_checkpoint_id)");
    save.bind_text(1, run_id);
    save.bind_text(2, artifact_id);
    save.bind_text(3, revision_digest);
    save.bind_text(4, protocol_version);
    save.bind_text(5, profile);
    save.bind_text(6, status);
    save.bind_text(7, source_run_id);
    save.bind_text(8, source_checkpoint_id);
    save.bind_text(9, record.dump());
    save.bind_int64(10, unix_millis());
    save.bind_text(11, source_run_id);
    save.bind_text(12, source_run_id);
    if (save.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "run write failed");
    if (sqlite3_changes(impl_->db) == 0) {
        throw std::invalid_argument(
            "Harness run binding is immutable or its source run is unavailable");
    }
}

std::optional<json> SqliteHarnessRecordStore::load_run(const std::string& run_id) {
    return impl_->load("neograph_harness_runs", "run_id", run_id);
}

void SqliteHarnessRecordStore::append_event(const json& event) {
    if (!event.is_object()) throw std::invalid_argument("Harness journal event must be an object");
    const auto required_string = [&](const char* key) {
        if (!event.contains(key) || !event[key].is_string() || event[key].get<std::string>().empty()) {
            throw std::invalid_argument(std::string("Harness journal event requires ") + key);
        }
        return event[key].get<std::string>();
    };
    const auto run_id = required_string("run_id");
    const auto artifact_id = required_string("artifact_id");
    const auto revision_digest = required_string("revision_digest");
    const auto protocol_version = required_string("protocol_version");
    const auto profile = required_string("profile");
    const auto event_type = required_string("event_type");
    validate_id(run_id);
    validate_id(artifact_id);

    json payload = event.value("payload", json::object());
    if (impl_->journal_config.mode == HarnessJournalPayloadMode::METADATA_ONLY) {
        payload = json::object();
    } else if (impl_->journal_config.mode == HarnessJournalPayloadMode::REDACTED) {
        std::set<std::string> keys;
        for (const auto& key : impl_->journal_config.redacted_keys) keys.insert(lowercase(key));
        payload = redacted_json(payload, keys);
    }

    std::lock_guard lock(impl_->mutex);
    impl_->exec("BEGIN IMMEDIATE;");
    try {
        Statement next(impl_->db,
                       "SELECT COALESCE(MAX(sequence), 0) + 1 "
                       "FROM neograph_harness_journal WHERE run_id = ?");
        next.bind_text(1, run_id);
        if (next.step() != SQLITE_ROW) throw_sqlite_error(impl_->db, "journal sequence read failed");
        const auto sequence = next.int64(0);

        Statement insert(impl_->db,
                         "INSERT INTO neograph_harness_journal "
                         "(run_id, sequence, artifact_id, revision_digest, protocol_version, profile, "
                         "event_type, correlation_id, node_id, worker_id, attempt, payload_json, "
                         "created_at_ms) "
                         "SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
                         "FROM neograph_harness_runs WHERE run_id=? AND artifact_id=? "
                         "AND revision_digest=? AND protocol_version=? AND profile=?");
        insert.bind_text(1, run_id);
        insert.bind_int64(2, sequence);
        insert.bind_text(3, artifact_id);
        insert.bind_text(4, revision_digest);
        insert.bind_text(5, protocol_version);
        insert.bind_text(6, profile);
        insert.bind_text(7, event_type);
        insert.bind_text(8, event.value("correlation_id", ""));
        insert.bind_text(9, event.value("node_id", ""));
        insert.bind_text(10, event.value("worker_id", ""));
        insert.bind_int64(11, event.value("attempt", int64_t{0}));
        insert.bind_text(12, payload.dump());
        insert.bind_int64(13, event.value("created_at_ms", unix_millis()));
        insert.bind_text(14, run_id);
        insert.bind_text(15, artifact_id);
        insert.bind_text(16, revision_digest);
        insert.bind_text(17, protocol_version);
        insert.bind_text(18, profile);
        if (insert.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "journal write failed");
        if (sqlite3_changes(impl_->db) != 1) {
            throw std::invalid_argument("Harness journal metadata does not match its run binding");
        }
        impl_->exec("COMMIT;");
    } catch (...) {
        try {
            impl_->exec("ROLLBACK;");
        } catch (...) {}
        throw;
    }
}

std::vector<json> SqliteHarnessRecordStore::list_events(const std::string& run_id,
                                                        std::size_t after_sequence,
                                                        std::size_t limit) {
    validate_id(run_id);
    if (limit == 0 || limit > 10000 || after_sequence > static_cast<std::size_t>(INT64_MAX)) {
        throw std::invalid_argument("Harness journal pagination is out of range");
    }
    std::lock_guard lock(impl_->mutex);
    Statement list(impl_->db,
                   "SELECT sequence, artifact_id, revision_digest, protocol_version, profile, "
                   "event_type, correlation_id, node_id, worker_id, attempt, payload_json, "
                   "created_at_ms FROM neograph_harness_journal "
                   "WHERE run_id=? AND sequence>? ORDER BY sequence LIMIT ?");
    list.bind_text(1, run_id);
    list.bind_int64(2, static_cast<int64_t>(after_sequence));
    list.bind_int64(3, static_cast<int64_t>(limit));
    std::vector<json> events;
    while (true) {
        const auto result = list.step();
        if (result == SQLITE_DONE) break;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "journal read failed");
        json event = {
            {"run_id", run_id},
            {"sequence", list.int64(0)},
            {"artifact_id", list.text(1)},
            {"revision_digest", list.text(2)},
            {"protocol_version", list.text(3)},
            {"profile", list.text(4)},
            {"event_type", list.text(5)},
            {"payload", json::parse(list.text(10))},
            {"created_at_ms", list.int64(11)},
        };
        if (!list.text(6).empty()) event["correlation_id"] = list.text(6);
        if (!list.text(7).empty()) event["node_id"] = list.text(7);
        if (!list.text(8).empty()) event["worker_id"] = list.text(8);
        if (list.int64(9) > 0) event["attempt"] = list.int64(9);
        events.push_back(std::move(event));
    }
    return events;
}

HarnessRetentionResult SqliteHarnessRecordStore::cleanup_retained(
    const HarnessRetentionPolicy& policy) {
    std::set<std::string> protected_runs;
    std::set<std::string> protected_artifacts;
    for (const auto& run_id : policy.protected_run_ids) {
        validate_id(run_id);
        protected_runs.insert(run_id);
    }
    for (const auto& artifact_id : policy.protected_artifact_ids) {
        validate_id(artifact_id);
        protected_artifacts.insert(artifact_id);
    }

    HarnessRetentionResult result;
    std::lock_guard        lock(impl_->mutex);
    impl_->exec("BEGIN IMMEDIATE;");
    try {
        const auto count = [&](const char* table) {
            const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
            Statement         statement(impl_->db, sql.c_str());
            if (statement.step() != SQLITE_ROW) {
                throw_sqlite_error(impl_->db, "retention count failed");
            }
            return static_cast<std::size_t>(statement.int64(0));
        };

        auto run_count = count("neograph_harness_runs");
        while (run_count > policy.max_runs) {
            Statement candidates(
                impl_->db,
                "SELECT candidate.run_id FROM neograph_harness_runs AS candidate "
                "WHERE candidate.status IN "
                "('completed','failed','cancelled','timeout','expired','max_steps_exhausted') "
                "AND NOT EXISTS (SELECT 1 FROM neograph_harness_runs AS dependent "
                "WHERE dependent.source_run_id=candidate.run_id) "
                "ORDER BY candidate.updated_at_ms, candidate.run_id");
            std::string run_id;
            while (true) {
                const auto step = candidates.step();
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    throw_sqlite_error(impl_->db, "retention run scan failed");
                }
                const auto candidate = candidates.text(0);
                if (!protected_runs.contains(candidate)) {
                    run_id = candidate;
                    break;
                }
            }
            if (run_id.empty()) break;

            Statement delete_journal(impl_->db,
                                     "DELETE FROM neograph_harness_journal WHERE run_id=?");
            delete_journal.bind_text(1, run_id);
            if (delete_journal.step() != SQLITE_DONE) {
                throw_sqlite_error(impl_->db, "retention journal delete failed");
            }
            Statement delete_run(impl_->db, "DELETE FROM neograph_harness_runs WHERE run_id=?");
            delete_run.bind_text(1, run_id);
            if (delete_run.step() != SQLITE_DONE) {
                throw_sqlite_error(impl_->db, "retention run delete failed");
            }
            if (sqlite3_changes(impl_->db) != 1) {
                throw std::runtime_error("SqliteHarnessRecordStore: retention lost run candidate");
            }
            result.run_ids.push_back(std::move(run_id));
            --run_count;
        }

        auto artifact_count = count("neograph_harness_artifacts");
        while (artifact_count > policy.max_artifacts) {
            Statement candidates(
                impl_->db,
                "SELECT artifact.artifact_id FROM neograph_harness_artifacts AS artifact "
                "WHERE NOT EXISTS (SELECT 1 FROM neograph_harness_runs AS run "
                "WHERE run.artifact_id=artifact.artifact_id) "
                "ORDER BY artifact.created_at_ms, artifact.artifact_id");
            std::string artifact_id;
            while (true) {
                const auto step = candidates.step();
                if (step == SQLITE_DONE) break;
                if (step != SQLITE_ROW) {
                    throw_sqlite_error(impl_->db, "retention artifact scan failed");
                }
                const auto candidate = candidates.text(0);
                if (!protected_artifacts.contains(candidate)) {
                    artifact_id = candidate;
                    break;
                }
            }
            if (artifact_id.empty()) break;

            Statement delete_artifact(impl_->db,
                                      "DELETE FROM neograph_harness_artifacts WHERE artifact_id=?");
            delete_artifact.bind_text(1, artifact_id);
            if (delete_artifact.step() != SQLITE_DONE) {
                throw_sqlite_error(impl_->db, "retention artifact delete failed");
            }
            if (sqlite3_changes(impl_->db) != 1) {
                throw std::runtime_error(
                    "SqliteHarnessRecordStore: retention lost artifact candidate");
            }
            result.artifact_ids.push_back(std::move(artifact_id));
            --artifact_count;
        }
        impl_->exec("COMMIT;");
    } catch (...) {
        try {
            impl_->exec("ROLLBACK;");
        } catch (...) {}
        throw;
    }
    return result;
}

}  // namespace neograph::mcp
