#include <neograph/mcp/sqlite_harness_store.h>

#include "harness_journal_internal.h"
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
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

    int     integer(int column) const { return sqlite3_column_int(statement_, column); }
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

bool is_program_identity(std::string_view value) noexcept {
    if (value.size() != 71 || !value.starts_with("sha256:")) return false;
    return std::all_of(value.begin() + 7, value.end(), [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool effect_ids_are_unique(
    const std::vector<program::ProgramEffectOutboxEntry>& old_effects,
    const std::vector<program::ProgramEffectOutboxEntry>& new_effects) {
    std::set<std::string, std::less<>> ids;
    for (const auto& effect : old_effects)
        if (!ids.emplace(effect.effect().effect_id()).second) return false;
    for (const auto& effect : new_effects)
        if (!ids.emplace(effect.effect().effect_id()).second) return false;
    return true;
}

bool valid_effect_outbox_binding(
    const program::ProgramRunRecord& run,
    const std::vector<program::ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;
    const auto pending = run.pending_effect();
    return pending && pending->state() == program::ProgramPendingState::Awaiting &&
           effects.size() == 1 && effects.front().effect() == *pending;
}

std::string program_storage_run_id(std::string_view owner_scope, std::string_view run_id) {
    return "program/" + std::to_string(owner_scope.size()) + "/" + std::string(owner_scope) + "/" +
           std::to_string(run_id.size()) + "/" + std::string(run_id);
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
        for (const auto child : value)
            result.push_back(redacted_json(child, redacted_keys));
        return result;
    }
    return value;
}

}  // namespace

struct SqliteHarnessRecordStore::Impl {
    Impl(const std::string&         db_path,
         std::chrono::milliseconds  busy_timeout,
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
VALUES (1, 5)
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
    owner_scope       TEXT    NOT NULL DEFAULT '',
    bundle_id         TEXT    NOT NULL DEFAULT '',
    program_version_id TEXT   NOT NULL DEFAULT '',
    program_run_id    TEXT    NOT NULL DEFAULT '',
    journal_head      TEXT    NOT NULL DEFAULT '',
    program_publication_json TEXT NOT NULL DEFAULT '',
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
            if (schema_version == 3) {
                exec(R"SQL(
ALTER TABLE neograph_harness_runs
    ADD COLUMN owner_scope TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN bundle_id TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN program_version_id TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN program_run_id TEXT NOT NULL DEFAULT '';
ALTER TABLE neograph_harness_runs
    ADD COLUMN journal_head TEXT NOT NULL DEFAULT '';
UPDATE neograph_harness_schema SET version = 4 WHERE singleton = 1;
)SQL");
                schema_version = 4;
            }
            if (schema_version == 4) {
                exec(R"SQL(
ALTER TABLE neograph_harness_runs
    ADD COLUMN program_publication_json TEXT NOT NULL DEFAULT '';
UPDATE neograph_harness_schema SET version = 5 WHERE singleton = 1;
)SQL");
                schema_version = 5;
            }
            if (schema_version != 5) {
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
CREATE TABLE IF NOT EXISTS neograph_harness_program_journal (
    run_id             TEXT    NOT NULL,
    sequence           INTEGER NOT NULL,
    owner_scope        TEXT    NOT NULL,
    bundle_id          TEXT    NOT NULL,
    program_version_id TEXT    NOT NULL,
    record_id          TEXT    NOT NULL,
    record_json        TEXT    NOT NULL,
    PRIMARY KEY (run_id, sequence),
    UNIQUE (run_id, record_id),
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_events (
    run_id             TEXT    NOT NULL,
    sequence           INTEGER NOT NULL,
    owner_scope        TEXT    NOT NULL,
    bundle_id          TEXT    NOT NULL,
    program_version_id TEXT    NOT NULL,
    record_id          TEXT    NOT NULL,
    record_json        TEXT    NOT NULL,
    PRIMARY KEY (run_id, sequence),
    UNIQUE (run_id, record_id),
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_effects (
    run_id      TEXT    NOT NULL,
    sequence    INTEGER NOT NULL,
    owner_scope TEXT    NOT NULL,
    record_id   TEXT    NOT NULL,
    record_json TEXT    NOT NULL,
    PRIMARY KEY (run_id, sequence),
    UNIQUE (run_id, record_id),
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
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

    sqlite3*                   db = nullptr;
    std::mutex                 mutex;
    SqliteHarnessJournalConfig journal_config;

    static constexpr std::string_view sqlite_transaction_reference =
        "https://www.sqlite.org/lang_transaction.html (fetched 2026-07-31)";
    std::optional<SqliteHarnessProgramFaultPoint> program_fault;
    std::optional<SqliteHarnessProgramFaultPoint> program_crash;
};

class SqliteHarnessProgramTransitionStore final : public program::ProgramTransitionStore {
public:
    SqliteHarnessProgramTransitionStore(std::shared_ptr<SqliteHarnessRecordStore::Impl> impl,
                                        HarnessProgramArtifactRecord                    artifact)
        : impl_(std::move(impl)), artifact_(std::move(artifact)) {}

    std::optional<program::ProgramRunRecord> load(std::string_view owner_scope,
                                                  std::string_view run_id) const override {
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return std::nullopt;
        return wrapper->run_record();
    }

    std::optional<program::ProgramJournalRecord> latest(std::string_view owner_scope,
                                                        std::string_view run_id) const override {
        if (owner_scope.empty() || run_id.empty()) return std::nullopt;
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement       query(impl_->db,
                              "SELECT j.record_json, r.bundle_id, r.program_version_id, "
                                    "j.owner_scope, j.bundle_id, j.program_version_id "
                                    "FROM neograph_harness_program_journal j "
                                    "JOIN neograph_harness_runs r ON r.run_id=j.run_id "
                                    "WHERE r.owner_scope=? AND r.run_id=? AND r.program_run_id<>'' "
                                    "ORDER BY j.sequence DESC LIMIT 1");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program journal read failed");
        auto record = program::ProgramJournalRecord::parse(query.text(0));
        if (record.id != wrapper->run_record().journal_head() || record.run_id != run_id ||
            record.bundle_id != query.text(1) || record.program_version_id != query.text(2) ||
            query.text(3) != owner_scope || query.text(4) != query.text(1) ||
            query.text(5) != query.text(2)) {
            throw std::invalid_argument("Stored Program journal binding is corrupt");
        }
        return record;
    }

    std::vector<program::ProgramEvent> load_events(std::string_view owner_scope,
                                                   std::string_view run_id,
                                                   std::uint64_t    after_sequence) const override {
        if (owner_scope.empty() || run_id.empty()) return {};
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return {};
        if (after_sequence > static_cast<std::uint64_t>(INT64_MAX)) {
            throw std::invalid_argument("Program event sequence is out of SQLite range");
        }
        std::lock_guard lock(impl_->mutex);
        Statement       query(impl_->db,
                              "SELECT e.sequence, e.record_json, r.bundle_id, r.program_version_id, "
                                    "e.owner_scope, e.bundle_id, e.program_version_id "
                                    "FROM neograph_harness_program_events e "
                                    "JOIN neograph_harness_runs r ON r.run_id=e.run_id "
                                    "WHERE r.owner_scope=? AND r.run_id=? AND e.sequence>? "
                                    "ORDER BY e.sequence");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        query.bind_int64(3, static_cast<std::int64_t>(after_sequence));
        std::vector<program::ProgramEvent> values;
        while (true) {
            const auto result = query.step();
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program event read failed");
            auto event = program::ProgramEvent::parse(query.text(1));
            if (event.sequence != static_cast<std::uint64_t>(query.int64(0)) ||
                event.run_id != run_id || event.bundle_id != query.text(2) ||
                event.program_version_id != query.text(3) || query.text(4) != owner_scope ||
                query.text(5) != query.text(2) || query.text(6) != query.text(3)) {
                throw std::invalid_argument("Stored Program event binding is corrupt");
            }
            values.push_back(std::move(event));
        }
        return values;
    }

    std::vector<program::ProgramEffectOutboxEntry> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t    after_sequence) const override {
        if (owner_scope.empty() || run_id.empty()) return {};
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return {};
        if (after_sequence > static_cast<std::uint64_t>(INT64_MAX)) {
            throw std::invalid_argument("Program effect sequence is out of SQLite range");
        }
        std::lock_guard lock(impl_->mutex);
        Statement       query(impl_->db,
                              "SELECT e.sequence, e.record_json, e.owner_scope "
                                    "FROM neograph_harness_program_effects e "
                                    "JOIN neograph_harness_runs r ON r.run_id=e.run_id "
                                    "WHERE r.owner_scope=? AND r.run_id=? AND e.sequence>? "
                                    "ORDER BY e.sequence");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        query.bind_int64(3, static_cast<std::int64_t>(after_sequence));
        std::vector<program::ProgramEffectOutboxEntry> values;
        while (true) {
            const auto result = query.step();
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program effect read failed");
            auto effect = program::ProgramEffectOutboxEntry::parse(query.text(1));
            if (effect.sequence() != static_cast<std::uint64_t>(query.int64(0)) ||
                query.text(2) != owner_scope) {
                throw std::invalid_argument("Stored Program effect binding is corrupt");
            }
            values.push_back(std::move(effect));
        }
        return values;
    }

    std::optional<program::MigrationPlan> load_migration_plan(
        std::string_view owner_scope, std::string_view run_id) const override {
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement       query(
            impl_->db,
            "SELECT program_publication_json FROM neograph_harness_runs "
                  "WHERE owner_scope=? AND run_id=? AND program_run_id<>''");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program migration read failed");
        if (query.text(0).empty()) {
            throw std::invalid_argument("Stored Program migration publication is missing");
        }
        const auto publication = program::ProgramTransitionPublication::parse(query.text(0));
        if (publication.run_record.run_id() != run_id ||
            publication.run_record.owner_scope() != owner_scope) {
            throw std::invalid_argument("Stored Program migration publication binding is corrupt");
        }
        return publication.migration_plan;
    }

    program::ProgramTransitionPublishResult compare_publish(
        std::string_view                      owner_scope,
        std::string_view                      expected_journal_head,
        program::ProgramTransitionPublication publication) override {
        std::string                            publication_bytes;
        std::optional<HarnessProgramRunRecord> wrapper;
        if (!expected_journal_head.empty() && !is_program_identity(expected_journal_head)) {
            return program::ProgramTransitionPublishResult::Conflict;
        }
        try {
            if (!valid_effect_outbox_binding(publication.run_record, publication.effects)) {
                return program::ProgramTransitionPublishResult::Conflict;
            }
            publication_bytes = publication.serialize_canonical();
            wrapper.emplace(HarnessProgramRunRecord::create(artifact_, publication.run_record,
                                                            artifact_.legacy_projection()));
            if (publication.journal_record.sequence > static_cast<std::uint64_t>(INT64_MAX)) {
                return program::ProgramTransitionPublishResult::Conflict;
            }
            for (const auto& event : publication.events) {
                if (event.sequence > static_cast<std::uint64_t>(INT64_MAX)) {
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }
            for (const auto& effect : publication.effects) {
                if (effect.sequence() > static_cast<std::uint64_t>(INT64_MAX)) {
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }
        } catch (const std::invalid_argument&) {
            return program::ProgramTransitionPublishResult::Conflict;
        }
        if (publication.run_record.owner_scope() != owner_scope) {
            return program::ProgramTransitionPublishResult::Conflict;
        }

        const auto      run_id             = publication.run_record.run_id();
        const auto      storage_run_id     = program_storage_run_id(owner_scope, run_id);
        const auto      serialized_wrapper = wrapper->serialize().dump();
        std::lock_guard lock(impl_->mutex);
        impl_->exec("BEGIN IMMEDIATE;");
        try {
            maybe_fail(SqliteHarnessProgramFaultPoint::AfterBegin);
            const auto persisted_artifact = load_artifact_locked(artifact_.artifact_id());
            if (!persisted_artifact ||
                persisted_artifact->serialize().dump() != artifact_.serialize().dump()) {
                impl_->exec("ROLLBACK;");
                return program::ProgramTransitionPublishResult::Conflict;
            }

            Statement current(impl_->db,
                              "SELECT owner_scope, artifact_id, bundle_id, program_version_id, "
                              "program_run_id, journal_head, record_json, program_publication_json "
                              "FROM neograph_harness_runs WHERE run_id=?");
            current.bind_text(1, storage_run_id);
            const auto current_result = current.step();
            const bool exists         = current_result == SQLITE_ROW;
            if (current_result != SQLITE_ROW && current_result != SQLITE_DONE) {
                throw_sqlite_error(impl_->db, "Program run CAS read failed");
            }
            if (exists && current.text(4).empty()) {
                impl_->exec("ROLLBACK;");
                return program::ProgramTransitionPublishResult::Conflict;
            }
            if (exists) {
                try {
                    const auto previous_publication =
                        program::ProgramTransitionPublication::parse(current.text(7));
                    if (previous_publication.migration_plan) {
                        if (publication.migration_plan &&
                            publication.migration_plan->id() !=
                                previous_publication.migration_plan->id()) {
                            impl_->exec("ROLLBACK;");
                            return program::ProgramTransitionPublishResult::Conflict;
                        }
                        publication.migration_plan = previous_publication.migration_plan;
                        publication_bytes = publication.serialize_canonical();
                    }
                } catch (const std::invalid_argument&) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }
            if (exists) {
                const auto stored_wrapper =
                    HarnessProgramRunRecord::parse(json::parse(current.text(6)));
                if (stored_wrapper.run_record().run_id() != run_id) {
                    throw std::invalid_argument("Stored Program run alias is corrupt");
                }
                if (current.text(0) != stored_wrapper.owner_scope() ||
                    current.text(1) != stored_wrapper.artifact_id() ||
                    current.text(2) != stored_wrapper.run_record().bundle_id() ||
                    current.text(3) != stored_wrapper.run_record().program_version_id() ||
                    current.text(4) != stored_wrapper.run_record().id() ||
                    current.text(5) != stored_wrapper.run_record().journal_head()) {
                    throw std::invalid_argument("Stored Program run SQLite binding is corrupt");
                }
                stored_wrapper.validate_artifact(artifact_);
                validate_publication_locked(stored_wrapper, current.text(5), current.text(7));
            }
            if (exists && current.text(7) == publication_bytes) {
                impl_->exec("ROLLBACK;");
                return expected_journal_head == publication.journal_record.previous_id
                           ? program::ProgramTransitionPublishResult::AlreadyPresent
                           : program::ProgramTransitionPublishResult::Conflict;
            }

            const auto new_terminal = publication.run_record.terminal_result();
            const bool publishes_terminal_event =
                !publication.events.empty() &&
                publication.events.back().kind == program::ProgramEventKind::Terminal;

            if (!exists) {
                if (!expected_journal_head.empty() ||
                    !publication.journal_record.previous_id.empty() ||
                    publication.journal_record.sequence != 1 || publication.events.empty() ||
                    publication.events.front().kind != program::ProgramEventKind::Started ||
                    publication.run_record.event_sequence() != publication.events.size() ||
                    publication.run_record.effect_sequence() != publication.effects.size() ||
                    publication.events.front().sequence != 1 ||
                    (!publication.effects.empty() && publication.effects.front().sequence() != 1) ||
                    (new_terminal && !publishes_terminal_event) ||
                    (publication.run_record.fork_receipt() && !publication.migration_plan)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            } else {
                if (current.text(0) != owner_scope || current.text(1) != artifact_.artifact_id() ||
                    current.text(2) != artifact_.bundle().id() ||
                    current.text(3) != artifact_.version().id() ||
                    current.text(5) != expected_journal_head ||
                    publication.journal_record.previous_id != expected_journal_head) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                const auto previous_wrapper =
                    HarnessProgramRunRecord::parse(json::parse(current.text(6)));
                previous_wrapper.validate_artifact(artifact_);
                const auto previous_run = previous_wrapper.run_record();
                const auto old_terminal = previous_run.terminal_result();
                const auto old_fork     = previous_run.fork_receipt();
                const auto new_fork     = publication.run_record.fork_receipt();
                const bool same_fork    = old_fork.has_value() == new_fork.has_value() &&
                                       (!old_fork || old_fork->id() == new_fork->id());
                const auto old_state = previous_run.continuation().state;
                const bool old_is_final =
                    old_state != program::ContinuationState::Running &&
                    old_state != program::ContinuationState::Interrupted &&
                    old_state != program::ContinuationState::AmbiguousEffect;
                if ((old_terminal && old_is_final && !new_terminal) ||
                    (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
                     !publishes_terminal_event)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                if (publication.events.empty() && publication.effects.empty()) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                Statement previous_journal(
                    impl_->db,
                    "SELECT record_json FROM neograph_harness_program_journal "
                    "WHERE run_id=? ORDER BY sequence DESC LIMIT 1");
                previous_journal.bind_text(1, storage_run_id);
                if (previous_journal.step() != SQLITE_ROW) {
                    throw std::invalid_argument("Stored Program run has no causal journal head");
                }
                const auto previous =
                    program::ProgramJournalRecord::parse(previous_journal.text(0));
                std::vector<program::ProgramEffectOutboxEntry> previous_effects;
                Statement previous_effect_rows(
                    impl_->db,
                    "SELECT record_json FROM neograph_harness_program_effects "
                          "WHERE run_id=? ORDER BY sequence");
                previous_effect_rows.bind_text(1, storage_run_id);
                while (true) {
                    const auto effect_result = previous_effect_rows.step();
                    if (effect_result == SQLITE_DONE) break;
                    if (effect_result != SQLITE_ROW) {
                        throw_sqlite_error(impl_->db, "Program effect history read failed");
                    }
                    previous_effects.push_back(
                        program::ProgramEffectOutboxEntry::parse(previous_effect_rows.text(0)));
                }
                if (!effect_ids_are_unique(previous_effects, publication.effects)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                if (previous.id != expected_journal_head ||
                    !program::is_valid_program_journal_transition(previous,
                                                                  publication.journal_record) ||
                    publication.run_record.created_at_ms() != previous_run.created_at_ms() ||
                    publication.run_record.updated_at_ms() < previous_run.updated_at_ms() ||
                    publication.run_record.binding_fingerprint() !=
                        previous_run.binding_fingerprint() ||
                    !same_fork ||
                    publication.run_record.recorded_binding_set_fingerprint() !=
                        previous_run.recorded_binding_set_fingerprint() ||
                    publication.run_record.invocation() != previous_run.invocation() ||
                    publication.run_record.event_sequence() !=
                        previous_run.event_sequence() + publication.events.size() ||
                    publication.run_record.effect_sequence() !=
                        previous_run.effect_sequence() + publication.effects.size() ||
                    (!publication.events.empty() &&
                     publication.events.front().sequence != previous_run.event_sequence() + 1) ||
                    (!publication.effects.empty() && publication.effects.front().sequence() !=
                                                         previous_run.effect_sequence() + 1)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }

            if (!exists) {
                Statement insert(
                    impl_->db,
                    "INSERT INTO neograph_harness_runs "
                    "(run_id, artifact_id, revision_digest, protocol_version, profile, status, "
                    "source_run_id, source_checkpoint_id, owner_scope, bundle_id, "
                    "program_version_id, program_run_id, journal_head, "
                    "program_publication_json, record_json, updated_at_ms) "
                    "VALUES (?, ?, ?, 'program-v1', ?, ?, '', '', ?, ?, ?, ?, ?, ?, ?, ?)");
                insert.bind_text(1, storage_run_id);
                insert.bind_text(2, artifact_.artifact_id());
                insert.bind_text(3, artifact_.version().id());
                insert.bind_text(4, artifact_.version().admission_profile().fingerprint());
                insert.bind_text(
                    5,
                    std::string(program::to_string(publication.run_record.continuation().state)));
                insert.bind_text(6, std::string(owner_scope));
                insert.bind_text(7, artifact_.bundle().id());
                insert.bind_text(8, artifact_.version().id());
                insert.bind_text(9, publication.run_record.id());
                insert.bind_text(10, publication.run_record.journal_head());
                insert.bind_text(11, publication_bytes);
                insert.bind_text(12, serialized_wrapper);
                insert.bind_int64(13, publication.run_record.updated_at_ms());
                if (insert.step() != SQLITE_DONE) {
                    throw_sqlite_error(impl_->db, "Program run insert failed");
                }
            } else {
                Statement update(
                    impl_->db,
                    "UPDATE neograph_harness_runs SET status=?, program_run_id=?, "
                    "journal_head=?, program_publication_json=?, record_json=?, updated_at_ms=? "
                    "WHERE run_id=? AND owner_scope=? AND artifact_id=? AND bundle_id=? "
                    "AND program_version_id=? AND program_run_id=? AND journal_head=?");
                update.bind_text(
                    1,
                    std::string(program::to_string(publication.run_record.continuation().state)));
                update.bind_text(2, publication.run_record.id());
                update.bind_text(3, publication.run_record.journal_head());
                update.bind_text(4, publication_bytes);
                update.bind_text(5, serialized_wrapper);
                update.bind_int64(6, publication.run_record.updated_at_ms());
                update.bind_text(7, storage_run_id);
                update.bind_text(8, std::string(owner_scope));
                update.bind_text(9, artifact_.artifact_id());
                update.bind_text(10, artifact_.bundle().id());
                update.bind_text(11, artifact_.version().id());
                update.bind_text(12, current.text(4));
                update.bind_text(13, std::string(expected_journal_head));
                if (update.step() != SQLITE_DONE) {
                    throw_sqlite_error(impl_->db, "Program run update failed");
                }
                if (sqlite3_changes(impl_->db) != 1) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }
            maybe_fail(SqliteHarnessProgramFaultPoint::AfterRunWrite);

            insert_journal(storage_run_id, owner_scope, publication.journal_record);
            maybe_fail(SqliteHarnessProgramFaultPoint::AfterJournalWrite);
            for (const auto& event : publication.events) {
                insert_event(storage_run_id, owner_scope, event);
            }
            maybe_fail(SqliteHarnessProgramFaultPoint::AfterEventWrite);
            for (const auto& effect : publication.effects) {
                insert_effect(storage_run_id, owner_scope, effect);
            }
            maybe_fail(SqliteHarnessProgramFaultPoint::AfterEffectWrite);
            maybe_fail(SqliteHarnessProgramFaultPoint::BeforeCommit);
            impl_->exec("COMMIT;");
            return program::ProgramTransitionPublishResult::Published;
        } catch (...) {
            try {
                impl_->exec("ROLLBACK;");
            } catch (...) {}
            throw;
        }
    }

private:
    void maybe_fail(SqliteHarnessProgramFaultPoint point) {
        if (impl_->program_crash && *impl_->program_crash == point) {
            impl_->program_crash.reset();
            std::raise(SIGKILL);
            std::abort();
        }
        if (impl_->program_fault && *impl_->program_fault == point) {
            impl_->program_fault.reset();
            throw std::runtime_error("injected SQLite Program transition failure");
        }
    }

    std::optional<HarnessProgramArtifactRecord> load_artifact_locked(
        const std::string& artifact_id) const {
        Statement query(impl_->db,
                        "SELECT record_json FROM neograph_harness_artifacts "
                        "WHERE artifact_id=?");
        query.bind_text(1, artifact_id);
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program artifact read failed");
        return HarnessProgramArtifactRecord::parse(json::parse(query.text(0)));
    }

    void validate_publication_locked(const HarnessProgramRunRecord& wrapper,
                                     std::string_view               row_journal_head,
                                     std::string_view               publication_bytes) const {
        const auto  publication = program::ProgramTransitionPublication::parse(publication_bytes);
        const auto& run         = wrapper.run_record();
        const auto  storage_run_id = program_storage_run_id(run.owner_scope(), run.run_id());
        if (publication.serialize_canonical() != publication_bytes ||
            publication.run_record.serialize_canonical() != run.serialize_canonical() ||
            publication.journal_record.id != row_journal_head ||
            run.journal_head() != row_journal_head) {
            throw std::invalid_argument("Stored Program publication binding is corrupt");
        }

        Statement journal(impl_->db,
                          "SELECT record_json FROM neograph_harness_program_journal "
                          "WHERE run_id=? AND sequence=? AND record_id=? "
                          "AND owner_scope=? AND bundle_id=? AND program_version_id=?");
        journal.bind_text(1, storage_run_id);
        journal.bind_int64(2, static_cast<std::int64_t>(publication.journal_record.sequence));
        journal.bind_text(3, publication.journal_record.id);
        journal.bind_text(4, run.owner_scope());
        journal.bind_text(5, run.bundle_id());
        journal.bind_text(6, run.program_version_id());
        if (journal.step() != SQLITE_ROW ||
            journal.text(0) != publication.journal_record.serialize_canonical()) {
            throw std::invalid_argument("Stored Program publication journal is missing");
        }

        for (const auto& event : publication.events) {
            Statement stored(impl_->db,
                             "SELECT record_json FROM neograph_harness_program_events "
                             "WHERE run_id=? AND sequence=? AND record_id=? "
                             "AND owner_scope=? AND bundle_id=? AND program_version_id=?");
            stored.bind_text(1, storage_run_id);
            stored.bind_int64(2, static_cast<std::int64_t>(event.sequence));
            stored.bind_text(3, event.id);
            stored.bind_text(4, run.owner_scope());
            stored.bind_text(5, run.bundle_id());
            stored.bind_text(6, run.program_version_id());
            if (stored.step() != SQLITE_ROW || stored.text(0) != event.serialize_canonical()) {
                throw std::invalid_argument("Stored Program publication event is missing");
            }
        }
        for (const auto& effect : publication.effects) {
            Statement stored(impl_->db,
                             "SELECT record_json FROM neograph_harness_program_effects "
                             "WHERE run_id=? AND sequence=? AND record_id=? AND owner_scope=?");
            stored.bind_text(1, storage_run_id);
            stored.bind_int64(2, static_cast<std::int64_t>(effect.sequence()));
            stored.bind_text(3, effect.id());
            stored.bind_text(4, run.owner_scope());
            if (stored.step() != SQLITE_ROW || stored.text(0) != effect.serialize_canonical()) {
                throw std::invalid_argument("Stored Program publication effect is missing");
            }
        }

        const auto require_count = [&](const char* table, std::uint64_t expected) {
            const std::string sql =
                std::string("SELECT COUNT(*) FROM ") + table + " WHERE run_id=?";
            Statement count(impl_->db, sql.c_str());
            count.bind_text(1, storage_run_id);
            if (count.step() != SQLITE_ROW ||
                static_cast<std::uint64_t>(count.int64(0)) != expected) {
                throw std::invalid_argument("Stored Program publication sequence is incomplete");
            }
        };
        require_count("neograph_harness_program_journal", publication.journal_record.sequence);
        require_count("neograph_harness_program_events", run.event_sequence());
        require_count("neograph_harness_program_effects", run.effect_sequence());
    }

    std::optional<HarnessProgramRunRecord> load_wrapper(std::string_view owner_scope,
                                                        std::string_view run_id) const {
        if (owner_scope.empty() || run_id.empty()) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement       query(
            impl_->db,
            "SELECT record_json, artifact_id, owner_scope, bundle_id, "
                  "program_version_id, program_run_id, journal_head, program_publication_json "
                  "FROM neograph_harness_runs "
                  "WHERE owner_scope=? AND run_id=? AND program_run_id<>''");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program run read failed");
        auto wrapper = HarnessProgramRunRecord::parse(json::parse(query.text(0)));
        if (wrapper.run_record().run_id() != run_id) {
            throw std::invalid_argument("Stored Program run alias is corrupt");
        }
        if (wrapper.artifact_id() != query.text(1) || wrapper.owner_scope() != query.text(2) ||
            wrapper.run_record().bundle_id() != query.text(3) ||
            wrapper.run_record().program_version_id() != query.text(4) ||
            wrapper.run_record().id() != query.text(5) ||
            wrapper.run_record().journal_head() != query.text(6)) {
            throw std::invalid_argument("Stored Program run alias binding is corrupt");
        }
        const auto artifact = load_artifact_locked(wrapper.artifact_id());
        if (!artifact) throw std::invalid_argument("Stored Program run artifact is missing");
        wrapper.validate_artifact(*artifact);
        validate_publication_locked(wrapper, query.text(6), query.text(7));
        return wrapper;
    }

    void insert_journal(const std::string&                   run_id,
                        std::string_view                     owner_scope,
                        const program::ProgramJournalRecord& record) {
        Statement insert(
            impl_->db,
            "INSERT INTO neograph_harness_program_journal "
            "(run_id, sequence, owner_scope, bundle_id, program_version_id, record_id, "
            "record_json) VALUES (?, ?, ?, ?, ?, ?, ?)");
        insert.bind_text(1, run_id);
        insert.bind_int64(2, static_cast<std::int64_t>(record.sequence));
        insert.bind_text(3, std::string(owner_scope));
        insert.bind_text(4, record.bundle_id);
        insert.bind_text(5, record.program_version_id);
        insert.bind_text(6, record.id);
        insert.bind_text(7, record.serialize_canonical());
        if (insert.step() != SQLITE_DONE) {
            throw_sqlite_error(impl_->db, "Program journal insert failed");
        }
    }

    void insert_event(const std::string&           run_id,
                      std::string_view             owner_scope,
                      const program::ProgramEvent& event) {
        Statement insert(
            impl_->db,
            "INSERT INTO neograph_harness_program_events "
            "(run_id, sequence, owner_scope, bundle_id, program_version_id, record_id, "
            "record_json) VALUES (?, ?, ?, ?, ?, ?, ?)");
        insert.bind_text(1, run_id);
        insert.bind_int64(2, static_cast<std::int64_t>(event.sequence));
        insert.bind_text(3, std::string(owner_scope));
        insert.bind_text(4, event.bundle_id);
        insert.bind_text(5, event.program_version_id);
        insert.bind_text(6, event.id);
        insert.bind_text(7, event.serialize_canonical());
        if (insert.step() != SQLITE_DONE) {
            throw_sqlite_error(impl_->db, "Program event insert failed");
        }
    }

    void insert_effect(const std::string&                       run_id,
                       std::string_view                         owner_scope,
                       const program::ProgramEffectOutboxEntry& effect) {
        Statement insert(impl_->db,
                         "INSERT INTO neograph_harness_program_effects "
                         "(run_id, sequence, owner_scope, record_id, record_json) "
                         "VALUES (?, ?, ?, ?, ?)");
        insert.bind_text(1, run_id);
        insert.bind_int64(2, static_cast<std::int64_t>(effect.sequence()));
        insert.bind_text(3, std::string(owner_scope));
        insert.bind_text(4, effect.id());
        insert.bind_text(5, effect.serialize_canonical());
        if (insert.step() != SQLITE_DONE) {
            throw_sqlite_error(impl_->db, "Program effect insert failed");
        }
    }

    std::shared_ptr<SqliteHarnessRecordStore::Impl> impl_;
    HarnessProgramArtifactRecord                    artifact_;
};

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string& db_path)
    : SqliteHarnessRecordStore(db_path, std::chrono::seconds(5), {}) {}

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string&        db_path,
                                                   std::chrono::milliseconds busy_timeout)
    : SqliteHarnessRecordStore(db_path, busy_timeout, {}) {}

SqliteHarnessRecordStore::SqliteHarnessRecordStore(const std::string&         db_path,
                                                   std::chrono::milliseconds  busy_timeout,
                                                   SqliteHarnessJournalConfig journal_config)
    : impl_(std::make_shared<Impl>(db_path, busy_timeout, std::move(journal_config))) {}

SqliteHarnessRecordStore::~SqliteHarnessRecordStore() = default;

#ifdef NEOGRAPH_TESTING
void SqliteHarnessRecordStore::fail_next_program_transition_for_testing(
    SqliteHarnessProgramFaultPoint point) {
    std::lock_guard lock(impl_->mutex);
    impl_->program_fault = point;
}

void SqliteHarnessRecordStore::crash_next_program_transition_for_testing(
    SqliteHarnessProgramFaultPoint point) {
    std::lock_guard lock(impl_->mutex);
    impl_->program_crash = point;
}
#endif

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
    const auto revision_digest      = record.value("revision_digest", "");
    const auto protocol_version     = record.value("protocol_version", "");
    const auto profile              = record.value("profile", "");
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
              "excluded.source_checkpoint_id) "
              "AND neograph_harness_runs.program_run_id=''");
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
        if (!event.contains(key) || !event[key].is_string() ||
            event[key].get<std::string>().empty()) {
            throw std::invalid_argument(std::string("Harness journal event requires ") + key);
        }
        return event[key].get<std::string>();
    };
    const auto run_id           = required_string("run_id");
    const auto artifact_id      = required_string("artifact_id");
    const auto revision_digest  = required_string("revision_digest");
    const auto protocol_version = required_string("protocol_version");
    const auto profile          = required_string("profile");
    const auto event_type       = required_string("event_type");
    validate_id(run_id);
    validate_id(artifact_id);

    json payload = event.value("payload", json::object());
    if (impl_->journal_config.mode == HarnessJournalPayloadMode::METADATA_ONLY) {
        payload = json::object();
    } else if (impl_->journal_config.mode == HarnessJournalPayloadMode::REDACTED) {
        std::set<std::string> keys;
        for (const auto& key : impl_->journal_config.redacted_keys)
            keys.insert(lowercase(key));
        payload = redacted_json(payload, keys);
    }

    std::lock_guard lock(impl_->mutex);
    impl_->exec("BEGIN IMMEDIATE;");
    try {
        Statement next(impl_->db,
                       "SELECT COALESCE(MAX(sequence), 0) + 1 "
                       "FROM neograph_harness_journal WHERE run_id = ?");
        next.bind_text(1, run_id);
        if (next.step() != SQLITE_ROW)
            throw_sqlite_error(impl_->db, "journal sequence read failed");
        const auto sequence = next.int64(0);

        Statement insert(
            impl_->db,
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
                                                        std::size_t        after_sequence,
                                                        std::size_t        limit) {
    validate_id(run_id);
    if (limit == 0 || limit > 10000 || after_sequence > static_cast<std::size_t>(INT64_MAX)) {
        throw std::invalid_argument("Harness journal pagination is out of range");
    }
    std::lock_guard lock(impl_->mutex);
    Statement       list(impl_->db,
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
                "AND candidate.program_run_id='' "
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
                "WHERE COALESCE(json_extract(artifact.record_json, '$.format'), '') "
                "<> 'neograph-harness-program-adapter-artifact' "
                "AND NOT EXISTS (SELECT 1 FROM neograph_harness_runs AS run "
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

std::shared_ptr<program::ProgramTransitionStore> SqliteHarnessRecordStore::bind_program_transitions(
    HarnessProgramArtifactRecord artifact) {
    const auto stored = load_artifact(artifact.artifact_id());
    if (!stored) {
        throw std::invalid_argument(
            "Harness Program transition binding requires a persisted adapter artifact");
    }
    const auto reparsed = HarnessProgramArtifactRecord::parse(*stored);
    if (reparsed.serialize().dump() != artifact.serialize().dump()) {
        throw std::invalid_argument("Harness Program transition artifact binding mismatch");
    }
    return std::make_shared<SqliteHarnessProgramTransitionStore>(impl_, std::move(artifact));
}

std::optional<HarnessProgramRunRecord> SqliteHarnessRecordStore::resolve_program_run(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    Statement       query(impl_->db,
                          "SELECT r.record_json, r.artifact_id, a.record_json, r.owner_scope, "
                                "r.bundle_id, r.program_version_id, r.program_run_id, r.journal_head, "
                                "r.program_publication_json "
                                "FROM neograph_harness_runs r "
                                "JOIN neograph_harness_artifacts a ON a.artifact_id=r.artifact_id "
                                "WHERE r.owner_scope=? AND r.run_id=? AND r.program_run_id<>''");
    query.bind_text(1, std::string(owner_scope));
    query.bind_text(2, program_storage_run_id(owner_scope, run_id));
    const auto result = query.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program run alias read failed");
    auto wrapper  = HarnessProgramRunRecord::parse(json::parse(query.text(0)));
    auto artifact = HarnessProgramArtifactRecord::parse(json::parse(query.text(2)));
    if (wrapper.run_record().run_id() != run_id) {
        throw std::invalid_argument("Stored Program run alias is corrupt");
    }
    if (wrapper.artifact_id() != query.text(1) || wrapper.owner_scope() != query.text(3) ||
        wrapper.run_record().bundle_id() != query.text(4) ||
        wrapper.run_record().program_version_id() != query.text(5) ||
        wrapper.run_record().id() != query.text(6) ||
        wrapper.run_record().journal_head() != query.text(7)) {
        throw std::invalid_argument("Stored Program run alias does not match SQLite binding");
    }
    const auto publication = program::ProgramTransitionPublication::parse(query.text(8));
    if (publication.serialize_canonical() != query.text(8) ||
        publication.run_record.serialize_canonical() !=
            wrapper.run_record().serialize_canonical() ||
        publication.journal_record.id != query.text(7)) {
        throw std::invalid_argument("Stored Program run publication is corrupt");
    }
    wrapper.validate_artifact(artifact);
    return wrapper;
}
}  // namespace neograph::mcp
