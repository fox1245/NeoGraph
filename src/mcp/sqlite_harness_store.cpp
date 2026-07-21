#include <neograph/mcp/sqlite_harness_store.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
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

}  // namespace

struct SqliteHarnessRecordStore::Impl {
    Impl(const std::string& db_path, std::chrono::milliseconds busy_timeout) {
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
        // Version 1 is the first shipped schema. A future bump must migrate
        // older versions inside this transaction before updating the singleton;
        // rejecting an unknown version is safer than opening it partially.
        exec("BEGIN IMMEDIATE;");
        try {
            exec(R"SQL(
CREATE TABLE IF NOT EXISTS neograph_harness_schema (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    version   INTEGER NOT NULL
);
INSERT INTO neograph_harness_schema (singleton, version)
VALUES (1, 1)
ON CONFLICT(singleton) DO NOTHING;
CREATE TABLE IF NOT EXISTS neograph_harness_artifacts (
    artifact_id  TEXT PRIMARY KEY,
    record_json  TEXT    NOT NULL,
    created_at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS neograph_harness_runs (
    run_id        TEXT PRIMARY KEY,
    artifact_id   TEXT    NOT NULL,
    record_json   TEXT    NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    FOREIGN KEY (artifact_id) REFERENCES neograph_harness_artifacts (artifact_id)
        ON DELETE RESTRICT
);
CREATE INDEX IF NOT EXISTS neograph_harness_runs_artifact
    ON neograph_harness_runs (artifact_id);
)SQL");
            Statement version(db,
                              "SELECT version FROM neograph_harness_schema WHERE singleton = 1");
            if (version.step() != SQLITE_ROW || version.integer(0) != 1) {
                throw std::runtime_error("SqliteHarnessRecordStore: unsupported schema version");
            }
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
};

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string& db_path)
    : SqliteHarnessRecordStore(db_path, std::chrono::seconds(5)) {}

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string&        db_path,
                                                   std::chrono::milliseconds busy_timeout)
    : impl_(std::make_unique<Impl>(db_path, busy_timeout)) {}

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
    std::lock_guard lock(impl_->mutex);
    Statement       save(impl_->db,
                         "INSERT INTO neograph_harness_runs "
                               "(run_id, artifact_id, record_json, updated_at_ms) VALUES (?, ?, ?, ?) "
                               "ON CONFLICT(run_id) DO UPDATE SET "
                               "record_json=excluded.record_json, updated_at_ms=excluded.updated_at_ms "
                               "WHERE neograph_harness_runs.artifact_id=excluded.artifact_id");
    save.bind_text(1, run_id);
    save.bind_text(2, artifact_id);
    save.bind_text(3, record.dump());
    save.bind_int64(4, unix_millis());
    if (save.step() != SQLITE_DONE) throw_sqlite_error(impl_->db, "run write failed");
    if (sqlite3_changes(impl_->db) == 0) {
        throw std::invalid_argument("Harness run revision binding is immutable");
    }
}

std::optional<json> SqliteHarnessRecordStore::load_run(const std::string& run_id) {
    return impl_->load("neograph_harness_runs", "run_id", run_id);
}

}  // namespace neograph::mcp
