#include <neograph/mcp/sqlite_harness_store.h>

#include "harness_journal_internal.h"
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

bool fork_allocation_fits(const program::ProgramRunLineage& previous,
                          const program::ProgramRunLineage& debited,
                          const program::RunBudget&         target) noexcept {
    const auto fits = [](auto before, auto after, auto allocated) {
        return after <= before && allocated == before - after;
    };
    return debited.active_generation() == previous.active_generation() &&
           debited.active_generation_id() == previous.active_generation_id() &&
           debited.active_run_record_id() == previous.active_run_record_id() &&
           debited.active_journal_head() == previous.active_journal_head() &&
           debited.inflight_reservation() == previous.inflight_reservation() &&
           debited.committed_descendant_budget() == previous.committed_descendant_budget() &&
           fits(previous.remaining_budget().wall_time_ms,
                debited.remaining_budget().wall_time_ms, target.wall_time_ms) &&
           fits(previous.remaining_budget().model_tokens,
                debited.remaining_budget().model_tokens, target.model_tokens) &&
           fits(previous.remaining_budget().monetary_microunits,
                debited.remaining_budget().monetary_microunits, target.monetary_microunits) &&
           fits(previous.remaining_budget().max_concurrency,
                debited.remaining_budget().max_concurrency, target.max_concurrency) &&
           fits(previous.remaining_budget().max_program_operations,
                debited.remaining_budget().max_program_operations,
                target.max_program_operations) &&
           fits(previous.remaining_budget().max_core_steps,
                debited.remaining_budget().max_core_steps, target.max_core_steps) &&
           fits(previous.remaining_budget().max_dynamic_compiles,
                debited.remaining_budget().max_dynamic_compiles,
                target.max_dynamic_compiles) &&
           fits(previous.remaining_budget().max_child_depth,
                debited.remaining_budget().max_child_depth, target.max_child_depth) &&
           fits(previous.remaining_budget().max_total_children,
                debited.remaining_budget().max_total_children, target.max_total_children);
}

bool valid_effect_outbox_binding(
    const program::ProgramRunRecord& run,
    const std::vector<program::ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;
    const auto pending = run.pending_effect();
    return pending && pending->state() == program::ProgramPendingState::Awaiting &&
           effects.size() == 1 && effects.front().effect() == *pending;
}
bool same_command_coordinate(const program::ProgramJavaScriptCommandJournalEntry& lhs,
                             const program::ProgramJavaScriptCommandJournalEntry& rhs) {
    return lhs.bundle_id() == rhs.bundle_id() && lhs.command_ordinal() == rhs.command_ordinal() &&
           lhs.coordinate_id() == rhs.coordinate_id() && lhs.command() == rhs.command() &&
           lhs.effect_identity() == rhs.effect_identity();
}

bool valid_command_history_append(
    const program::ProgramRunRecord&                                  run,
    const std::vector<program::ProgramJavaScriptCommandJournalEntry>& old_commands,
    const std::vector<program::ProgramJavaScriptCommandJournalEntry>& new_commands) {
    if (new_commands.empty()) return true;
    auto          prior             = old_commands;
    std::uint64_t expected_sequence = 0;
    std::uint64_t highest_ordinal   = 0;
    for (const auto& existing : prior) {
        if (existing.bundle_id() != run.bundle_id() ||
            existing.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;
        highest_ordinal = std::max(highest_ordinal, existing.command_ordinal());
    }
    for (const auto& entry : new_commands) {
        if (entry.bundle_id() != run.bundle_id() || entry.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;
        const auto found = std::find_if(prior.rbegin(), prior.rend(), [&](const auto& previous) {
            return previous.command_ordinal() == entry.command_ordinal();
        });
        if (found == prior.rend()) {
            if (entry.command_ordinal() != highest_ordinal + 1 || !entry.pending() ||
                (!prior.empty() && prior.back().pending())) {
                return false;
            }
            highest_ordinal = entry.command_ordinal();
            prior.push_back(entry);
            continue;
        }
        if (!found->pending() || !entry.completed() || !same_command_coordinate(*found, entry)) {
            return false;
        }
        prior.push_back(entry);
    }
    return true;
}

bool budget_is_empty(const program::RunBudget& budget) noexcept {
    return budget == program::RunBudget{};
}

bool budget_increased(const program::RunBudget& next, const program::RunBudget& previous) noexcept {
    return next.wall_time_ms > previous.wall_time_ms || next.model_tokens > previous.model_tokens ||
           next.monetary_microunits > previous.monetary_microunits ||
           next.max_concurrency > previous.max_concurrency ||
           next.max_program_operations > previous.max_program_operations ||
           next.max_core_steps > previous.max_core_steps ||
           next.max_dynamic_compiles > previous.max_dynamic_compiles ||
           next.max_child_depth > previous.max_child_depth ||
           next.max_total_children > previous.max_total_children;
}

std::optional<program::ProgramUsage> command_terminal_usage(
    const program::ProgramJavaScriptCommandJournalEntry& entry) {
    const auto terminal_result = entry.terminal_result();
    if (!entry.completed() || !terminal_result || !terminal_result->contains("usage") ||
        !terminal_result->at("usage").is_object()) {
        return std::nullopt;
    }
    const auto& usage = terminal_result->at("usage");
    const auto  read  = [&](std::string_view key) -> std::optional<std::uint64_t> {
        const std::string owned(key);
        if (!usage.contains(owned) || !usage.at(owned).is_number_unsigned()) return std::nullopt;
        return usage.at(owned).get<std::uint64_t>();
    };
    const auto wall       = read("wall_time_ms");
    const auto model      = read("model_tokens");
    const auto money      = read("monetary_microunits");
    const auto operations = read("program_operations");
    const auto steps      = read("core_steps");
    const auto peak       = read("peak_concurrency");
    if (!wall || !model || !money || !operations || !steps || !peak ||
        *peak > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return program::ProgramUsage{*wall, *model, *money,
                                 0,     *steps, static_cast<std::uint32_t>(*peak)};
}

bool checkpoint_event_matches(const std::vector<program::ProgramEvent>&             events,
                              const std::optional<program::CoreCheckpointIdentity>& checkpoint) {
    if (!checkpoint) return false;
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == program::ProgramEventKind::CheckpointPublished &&
               std::get<program::ProgramCheckpointEvent>(event.payload).checkpoint == *checkpoint;
    });
}

bool valid_command_reservation_transition(
    const program::ProgramJournalRecord&                              previous_journal,
    const program::ProgramJournalRecord&                              next_journal,
    const std::vector<program::ProgramJavaScriptCommandJournalEntry>& old_commands,
    const program::ProgramTransitionPublication&                      publication) {
    const bool increased =
        budget_increased(next_journal.remaining_budget, previous_journal.remaining_budget);
    const bool ordinary =
        program::is_valid_program_journal_transition(previous_journal, next_journal);
    const auto completed = std::find_if(publication.commands.begin(), publication.commands.end(),
                                        [](const auto& entry) { return entry.completed(); });
    if (completed != publication.commands.end()) {
        if (publication.commands.size() != 1 ||
            !budget_is_empty(next_journal.inflight_reservation)) {
            return false;
        }
        if (budget_is_empty(previous_journal.inflight_reservation)) return ordinary && !increased;
        const auto usage = command_terminal_usage(*completed);
        return usage && program::is_valid_program_journal_reservation_settlement(
                            previous_journal, next_journal, *usage);
    }
    if (!increased) return ordinary;
    if (!publication.commands.empty() || old_commands.empty() || !old_commands.back().pending() ||
        old_commands.back().command().kind() != program::JavaScriptCommandKind::CallCore ||
        publication.run_record.continuation().state != program::ContinuationState::Interrupted ||
        !publication.run_record.terminal_result() ||
        publication.run_record.terminal_result()->status() !=
            program::ProgramTerminalStatus::Interrupted ||
        !checkpoint_event_matches(publication.events, publication.run_record.exact_checkpoint()) ||
        (previous_journal.core_checkpoint &&
         previous_journal.core_checkpoint == next_journal.core_checkpoint)) {
        return false;
    }
    auto usage               = publication.run_record.terminal_result()->usage();
    usage.program_operations = 0;
    return program::is_valid_program_journal_reservation_settlement(previous_journal, next_journal,
                                                                    usage);
}

program::ContractManifest contract_manifest_for_artifact(
    const HarnessProgramArtifactRecord& artifact) {
    const auto projection = artifact.projection();
    if (!projection.contains("contract_manifest") ||
        !projection.at("contract_manifest").is_object() ||
        !projection.contains("contract_manifest_hash") ||
        !projection.at("contract_manifest_hash").is_string()) {
        throw std::invalid_argument("Harness contract artifact manifest is missing");
    }
    const auto manifest =
        program::ContractManifest::parse(projection.at("contract_manifest").dump());
    if (manifest.lifecycle() != program::ContractManifestLifecycle::Frozen ||
        projection.at("contract_manifest_hash") != manifest.content_hash()) {
        throw std::invalid_argument("Stored Harness contract manifest binding mismatch");
    }
    return manifest;
}

std::string contract_workspace_revision_for_artifact(
    const HarnessProgramArtifactRecord& artifact) {
    const auto projection = artifact.projection();
    if (!projection.contains("workspace_revision")) return artifact.version().id();
    if (!projection.at("workspace_revision").is_string() ||
        projection.at("workspace_revision").get<std::string>().empty()) {
        throw std::invalid_argument("Stored Harness contract workspace revision is invalid");
    }
    return projection.at("workspace_revision").get<std::string>();
}

void validate_contract_run_binding(const HarnessProgramArtifactRecord& artifact,
                                   const program::ProgramRunRecord&     run_record,
                                   const program::ContractRun&           contract_run) {
    if (run_record.owner_scope() != artifact.owner_scope() ||
        run_record.bundle_id() != artifact.bundle().id() ||
        run_record.program_version_id() != artifact.version().id()) {
        throw std::invalid_argument("Stored Harness contract Program binding mismatch");
    }
    if (contract_run.manifest().lifecycle() != program::ContractManifestLifecycle::Frozen) {
        throw std::invalid_argument("Stored Harness contract manifest is not frozen");
    }
    const auto manifest = contract_manifest_for_artifact(artifact);
    if (contract_run.manifest().serialize_canonical() != manifest.serialize_canonical()) {
        throw std::invalid_argument("Stored Harness contract manifest binding mismatch");
    }
    if (contract_run.attempt() > manifest.spec().retry_policy.max_attempts) {
        throw std::invalid_argument("Stored Harness contract attempt exceeds retry policy");
    }
    std::set<std::string, std::less<>> evidence_ids;
    for (const auto& evidence : contract_run.evidence()) {
        if (!evidence_ids.emplace(evidence.evidence_id).second ||
            evidence.manifest_hash != manifest.content_hash()) {
            throw std::invalid_argument("Stored Harness contract evidence lineage is corrupt");
        }
        if (evidence.kind == program::ContractEvidenceKind::WorkerReport) continue;
        if (evidence.program_version_id != run_record.program_version_id() ||
            evidence.run_id != run_record.run_id() ||
            evidence.artifact_hash != run_record.bundle_id()) {
            throw std::invalid_argument("Stored Harness contract evidence Program lineage mismatch");
        }
    }
    const auto status = contract_run.status();
    const auto verification = contract_run.verification();
    switch (status) {
        case program::ContractRunStatus::Frozen:
            if (contract_run.attempt() != 0 || !contract_run.evidence().empty() ||
                !contract_run.diagnostics().empty() || verification) {
                throw std::invalid_argument("Stored Harness frozen contract state is corrupt");
            }
            break;
        case program::ContractRunStatus::Running:
            if (contract_run.attempt() == 0 || verification) {
                throw std::invalid_argument("Stored Harness running contract state is corrupt");
            }
            break;
        case program::ContractRunStatus::Verified:
            if (!verification || verification->status != status || !verification->publishable) {
                throw std::invalid_argument("Stored Harness verified contract state is corrupt");
            }
            break;
        case program::ContractRunStatus::Published:
            if (!verification || verification->status != status || !verification->publishable) {
                throw std::invalid_argument("Stored Harness publication is not verified");
            }
            break;
        case program::ContractRunStatus::Blocked:
        case program::ContractRunStatus::Failed:
            if (!verification || verification->status != status || verification->publishable) {
                throw std::invalid_argument("Stored Harness rejected contract state is corrupt");
            }
            break;
    }
    if ((status == program::ContractRunStatus::Verified ||
         status == program::ContractRunStatus::Published ||
         status == program::ContractRunStatus::Blocked ||
         status == program::ContractRunStatus::Failed) &&
        !run_record.terminal_result()) {
        throw std::invalid_argument("Stored Harness contract terminal state lacks Program result");
    }
    if (verification && status != program::ContractRunStatus::Running &&
        status != program::ContractRunStatus::Frozen) {
        auto replay_json = json::parse(contract_run.serialize_canonical());
        replay_json["status"] = "running";
        replay_json["verification"] = nullptr;
        auto replay = program::ContractRun::parse(replay_json.dump());
        auto expected = replay.verify(run_record.program_version_id(),
                                      run_record.run_id(),
                                      contract_workspace_revision_for_artifact(artifact));
        if (status == program::ContractRunStatus::Published) expected.status = status;
        if (*verification != expected) {
            throw std::invalid_argument("Stored Harness contract verification is corrupt");
        }
    }
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
            if (const auto* filename = sqlite3_db_filename(db, "main");
                filename && *filename) {
                std::error_code error;
                auto canonical = std::filesystem::weakly_canonical(filename, error);
                if (error)
                    canonical = std::filesystem::absolute(filename, error).lexically_normal();
                if (!error) {
                    program_coordination_key =
                        "neograph-harness-sqlite:" + canonical.generic_string();
                }
            }
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
VALUES (1, 7)
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
            if (schema_version == 5) {
                exec(R"SQL(
CREATE TABLE IF NOT EXISTS neograph_harness_contract_runs (
    run_id             TEXT PRIMARY KEY,
    owner_scope        TEXT NOT NULL,
    artifact_id        TEXT NOT NULL,
    bundle_id          TEXT NOT NULL,
    program_version_id TEXT NOT NULL,
    manifest_hash      TEXT NOT NULL,
    record_json        TEXT NOT NULL,
    updated_at_ms      INTEGER NOT NULL,
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
UPDATE neograph_harness_schema SET version = 6 WHERE singleton = 1;
)SQL");
                schema_version = 6;
            }
            if (schema_version == 6) {
                exec(R"SQL(
CREATE TABLE IF NOT EXISTS neograph_harness_program_generation_publications (
    owner_scope    TEXT    NOT NULL,
    lineage_id     TEXT    NOT NULL,
    generation     INTEGER NOT NULL,
    generation_id  TEXT    NOT NULL,
    publication_json TEXT  NOT NULL,
    PRIMARY KEY (owner_scope, lineage_id, generation),
    FOREIGN KEY (owner_scope, lineage_id, generation)
        REFERENCES neograph_harness_program_generations
            (owner_scope, lineage_id, generation)
        ON DELETE RESTRICT
);
UPDATE neograph_harness_schema SET version = 7 WHERE singleton = 1;
)SQL");
                schema_version = 7;
            }
            if (schema_version != 7) {
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
CREATE TABLE IF NOT EXISTS neograph_harness_program_javascript_commands (
    run_id        TEXT    NOT NULL,
    sequence      INTEGER NOT NULL,
    owner_scope   TEXT    NOT NULL,
    bundle_id     TEXT    NOT NULL,
    coordinate_id TEXT    NOT NULL,
    record_id     TEXT    NOT NULL,
    record_json   TEXT    NOT NULL,
    PRIMARY KEY (run_id, sequence),
    UNIQUE (run_id, record_id),
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_lineage_heads (
    owner_scope TEXT NOT NULL,
    lineage_id  TEXT NOT NULL,
    head_id     TEXT NOT NULL,
    record_json TEXT NOT NULL,
    PRIMARY KEY (owner_scope, lineage_id)
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_lineage_history (
    owner_scope TEXT NOT NULL,
    lineage_id  TEXT NOT NULL,
    head_id     TEXT NOT NULL,
    record_json TEXT NOT NULL,
    PRIMARY KEY (owner_scope, lineage_id, head_id),
    FOREIGN KEY (owner_scope, lineage_id)
        REFERENCES neograph_harness_program_lineage_heads (owner_scope, lineage_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_generations (
    owner_scope  TEXT    NOT NULL,
    lineage_id   TEXT    NOT NULL,
    generation   INTEGER NOT NULL,
    generation_id TEXT   NOT NULL,
    record_json  TEXT    NOT NULL,
    PRIMARY KEY (owner_scope, lineage_id, generation),
    UNIQUE (owner_scope, lineage_id, generation_id),
    FOREIGN KEY (owner_scope, lineage_id)
        REFERENCES neograph_harness_program_lineage_heads (owner_scope, lineage_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_generation_publications (
    owner_scope    TEXT    NOT NULL,
    lineage_id     TEXT    NOT NULL,
    generation     INTEGER NOT NULL,
    generation_id  TEXT    NOT NULL,
    publication_json TEXT  NOT NULL,
    PRIMARY KEY (owner_scope, lineage_id, generation),
    FOREIGN KEY (owner_scope, lineage_id, generation)
        REFERENCES neograph_harness_program_generations
            (owner_scope, lineage_id, generation)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_program_lineage_runs (
    owner_scope   TEXT NOT NULL,
    program_run_id TEXT NOT NULL,
    lineage_id    TEXT NOT NULL,
    PRIMARY KEY (owner_scope, program_run_id),
    FOREIGN KEY (owner_scope, lineage_id)
        REFERENCES neograph_harness_program_lineage_heads (owner_scope, lineage_id)
        ON DELETE RESTRICT
);
CREATE TABLE IF NOT EXISTS neograph_harness_contract_runs (
    run_id             TEXT PRIMARY KEY,
    owner_scope        TEXT NOT NULL,
    artifact_id        TEXT NOT NULL,
    bundle_id          TEXT NOT NULL,
    program_version_id TEXT NOT NULL,
    manifest_hash      TEXT NOT NULL,
    record_json        TEXT NOT NULL,
    updated_at_ms      INTEGER NOT NULL,
    FOREIGN KEY (run_id) REFERENCES neograph_harness_runs (run_id)
        ON DELETE RESTRICT
);
)SQL");
            struct MissingGenerationPublication {
                std::string                   owner_scope;
                std::string                   lineage_id;
                std::int64_t                  generation = 0;
                program::ProgramRunGeneration record;
            };
            std::vector<MissingGenerationPublication> missing_publications;
            {
                Statement query(
                    db,
                    "SELECT g.owner_scope, g.lineage_id, g.generation, g.record_json FROM "
                    "neograph_harness_program_generations g LEFT JOIN "
                    "neograph_harness_program_generation_publications p ON "
                    "p.owner_scope=g.owner_scope AND p.lineage_id=g.lineage_id "
                    "AND p.generation=g.generation WHERE p.generation IS NULL");
                while (true) {
                    const auto result = query.step();
                    if (result == SQLITE_DONE) break;
                    if (result != SQLITE_ROW) {
                        throw_sqlite_error(
                            db, "Program generation publication migration read failed");
                    }
                    auto generation =
                        program::ProgramRunGeneration::parse(query.text(3));
                    if (query.int64(2) <= 0 ||
                        generation.owner_scope() != query.text(0) ||
                        generation.lineage_id() != query.text(1) ||
                        generation.generation() !=
                            static_cast<std::uint64_t>(query.int64(2))) {
                        throw std::invalid_argument(
                            "Stored Program generation migration binding is corrupt");
                    }
                    missing_publications.push_back(MissingGenerationPublication{
                        query.text(0), query.text(1), query.int64(2),
                        std::move(generation)});
                }
            }
            for (const auto& item : missing_publications) {
                Statement current(
                    db,
                    "SELECT program_publication_json FROM neograph_harness_runs "
                    "WHERE owner_scope=? AND program_run_id=? AND program_run_id<>''");
                current.bind_text(1, item.owner_scope);
                current.bind_text(2, item.record.initial_run_record_id());
                if (current.step() != SQLITE_ROW || current.text(0).empty()) continue;
                const auto publication =
                    program::ProgramTransitionPublication::parse(current.text(0));
                if (!publication.run_generation || !publication.run_lineage ||
                    publication.run_generation->id() != item.record.id() ||
                    publication.run_record.id() != item.record.initial_run_record_id() ||
                    publication.journal_record.id != item.record.initial_journal_head()) {
                    continue;
                }
                Statement insert(
                    db,
                    "INSERT INTO neograph_harness_program_generation_publications "
                    "(owner_scope, lineage_id, generation, generation_id, publication_json) "
                    "VALUES(?, ?, ?, ?, ?)");
                insert.bind_text(1, item.owner_scope);
                insert.bind_text(2, item.lineage_id);
                insert.bind_int64(3, item.generation);
                insert.bind_text(4, item.record.id());
                insert.bind_text(5, publication.serialize_canonical());
                if (insert.step() != SQLITE_DONE) {
                    throw_sqlite_error(db,
                                       "Program generation publication migration write failed");
                }
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

    sqlite3*                   db = nullptr;
    std::string                program_coordination_key;
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

    std::string process_coordination_key() const override {
        if (!impl_->program_coordination_key.empty()) return impl_->program_coordination_key;
        return "neograph-harness-sqlite-memory:" +
               std::to_string(reinterpret_cast<std::uintptr_t>(impl_.get()));
    }

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
    std::vector<program::ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t    after_sequence) const override {
        if (owner_scope.empty() || run_id.empty()) return {};
        const auto wrapper = load_wrapper(owner_scope, run_id);
        if (!wrapper) return {};
        if (after_sequence > static_cast<std::uint64_t>(INT64_MAX)) {
            throw std::invalid_argument(
                "Program JavaScript command sequence is out of SQLite range");
        }
        std::lock_guard lock(impl_->mutex);
        Statement       query(impl_->db,
                              "SELECT c.sequence, c.record_json, c.owner_scope, c.bundle_id, "
                                    "c.coordinate_id, c.record_id "
                                    "FROM neograph_harness_program_javascript_commands c "
                                    "JOIN neograph_harness_runs r ON r.run_id=c.run_id "
                                    "WHERE r.owner_scope=? AND r.run_id=? AND c.sequence>? "
                                    "ORDER BY c.sequence");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, program_storage_run_id(owner_scope, run_id));
        query.bind_int64(3, static_cast<std::int64_t>(after_sequence));
        std::vector<program::ProgramJavaScriptCommandJournalEntry> values;
        auto expected_sequence = after_sequence;
        while (true) {
            const auto result = query.step();
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                throw_sqlite_error(impl_->db, "Program JavaScript command read failed");
            }
            auto command = program::ProgramJavaScriptCommandJournalEntry::parse(query.text(1));
            if (expected_sequence == std::numeric_limits<std::uint64_t>::max() ||
                command.sequence() != ++expected_sequence ||
                command.sequence() != static_cast<std::uint64_t>(query.int64(0)) ||
                query.text(2) != owner_scope || command.bundle_id() != query.text(3) ||
                command.coordinate_id() != query.text(4) || command.id() != query.text(5)) {
                throw std::invalid_argument("Stored Program JavaScript command binding is corrupt");
            }
            values.push_back(std::move(command));
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

    std::optional<program::ProgramRunLineage> load_lineage(
        std::string_view owner_scope, std::string_view lineage_id) const override {
        if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement query(impl_->db,
                        "SELECT head_id, record_json FROM "
                        "neograph_harness_program_lineage_heads "
                        "WHERE owner_scope=? AND lineage_id=?");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, std::string(lineage_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "Program lineage read failed");
        auto lineage = program::ProgramRunLineage::parse(query.text(1));
        if (lineage.id() != query.text(0) || lineage.owner_scope() != owner_scope ||
            lineage.lineage_id() != lineage_id) {
            throw std::invalid_argument("Stored Program lineage binding is corrupt");
        }
        return lineage;
    }

    std::optional<program::ProgramRunLineage> load_run_lineage(
        std::string_view owner_scope, std::string_view run_id) const override {
        if (owner_scope.empty() || run_id.empty()) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement query(impl_->db,
                        "SELECT r.lineage_id, h.head_id, h.record_json FROM "
                        "neograph_harness_program_lineage_runs r "
                        "JOIN neograph_harness_program_lineage_heads h "
                        "ON h.owner_scope=r.owner_scope AND h.lineage_id=r.lineage_id "
                        "WHERE r.owner_scope=? AND r.program_run_id=?");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, std::string(run_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW)
            throw_sqlite_error(impl_->db, "Program run lineage read failed");
        auto lineage = program::ProgramRunLineage::parse(query.text(2));
        if (lineage.lineage_id() != query.text(0) || lineage.id() != query.text(1) ||
            lineage.owner_scope() != owner_scope) {
            throw std::invalid_argument("Stored Program run lineage association is corrupt");
        }
        return lineage;
    }

    std::optional<program::ProgramRunLineage> load_lineage_head(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::string_view head_id) const override {
        if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
        std::lock_guard lock(impl_->mutex);
        Statement query(impl_->db,
                        "SELECT record_json FROM neograph_harness_program_lineage_history "
                        "WHERE owner_scope=? AND lineage_id=? AND head_id=?");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, std::string(lineage_id));
        query.bind_text(3, std::string(head_id));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW)
            throw_sqlite_error(impl_->db, "Program lineage history read failed");
        auto lineage = program::ProgramRunLineage::parse(query.text(0));
        if (lineage.id() != head_id || lineage.owner_scope() != owner_scope ||
            lineage.lineage_id() != lineage_id) {
            throw std::invalid_argument("Stored Program lineage history binding is corrupt");
        }
        return lineage;
    }

    std::optional<program::ProgramRunGeneration> load_generation(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const override {
        if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
        if (generation > static_cast<std::uint64_t>(INT64_MAX))
            throw std::invalid_argument("Program generation is out of SQLite range");
        std::lock_guard lock(impl_->mutex);
        Statement query(impl_->db,
                        "SELECT generation_id, record_json FROM "
                        "neograph_harness_program_generations "
                        "WHERE owner_scope=? AND lineage_id=? AND generation=?");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, std::string(lineage_id));
        query.bind_int64(3, static_cast<std::int64_t>(generation));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW)
            throw_sqlite_error(impl_->db, "Program generation read failed");
        auto value = program::ProgramRunGeneration::parse(query.text(1));
        if (value.id() != query.text(0) || value.owner_scope() != owner_scope ||
            value.lineage_id() != lineage_id || value.generation() != generation) {
            throw std::invalid_argument("Stored Program generation binding is corrupt");
        }
        return value;
    }

    std::optional<program::ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const override {
        if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
        if (generation > static_cast<std::uint64_t>(INT64_MAX))
            throw std::invalid_argument("Program generation is out of SQLite range");
        std::lock_guard lock(impl_->mutex);
        Statement query(
            impl_->db,
            "SELECT p.generation_id, p.publication_json, g.generation_id FROM "
            "neograph_harness_program_generation_publications p "
            "JOIN neograph_harness_program_generations g "
            "ON g.owner_scope=p.owner_scope AND g.lineage_id=p.lineage_id "
            "AND g.generation=p.generation "
            "WHERE p.owner_scope=? AND p.lineage_id=? AND p.generation=?");
        query.bind_text(1, std::string(owner_scope));
        query.bind_text(2, std::string(lineage_id));
        query.bind_int64(3, static_cast<std::int64_t>(generation));
        const auto result = query.step();
        if (result == SQLITE_DONE) return std::nullopt;
        if (result != SQLITE_ROW)
            throw_sqlite_error(impl_->db, "Program generation publication read failed");
        auto publication = program::ProgramTransitionPublication::parse(query.text(1));
        if (!publication.run_generation || !publication.run_lineage ||
            query.text(0) != query.text(2) ||
            publication.run_generation->id() != query.text(2) ||
            publication.run_generation->owner_scope() != owner_scope ||
            publication.run_generation->lineage_id() != lineage_id ||
            publication.run_generation->generation() != generation) {
            throw std::invalid_argument(
                "Stored Program generation publication binding is corrupt");
        }
        return publication;
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
                                                            artifact_.projection()));
            if (publication.journal_record.sequence > static_cast<std::uint64_t>(INT64_MAX)) {
                return program::ProgramTransitionPublishResult::Conflict;
            }
            if ((publication.run_generation &&
                 publication.run_generation->generation() >
                     static_cast<std::uint64_t>(INT64_MAX)) ||
                (publication.run_lineage &&
                 publication.run_lineage->active_generation() >
                     static_cast<std::uint64_t>(INT64_MAX))) {
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
            for (const auto& command : publication.commands) {
                if (command.sequence() > static_cast<std::uint64_t>(INT64_MAX)) {
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
            if (!exists && publication.fork_source_lineage) {
                const auto fork = publication.run_record.fork_receipt();
                if (!fork || fork->storage_schema_version() <
                                 program::ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            }

            std::optional<std::string> associated_lineage_id;
            {
                Statement association(
                    impl_->db,
                    "SELECT lineage_id FROM neograph_harness_program_lineage_runs "
                    "WHERE owner_scope=? AND program_run_id=?");
                association.bind_text(1, std::string(owner_scope));
                association.bind_text(2, run_id);
                const auto result = association.step();
                if (result == SQLITE_ROW)
                    associated_lineage_id = association.text(0);
                else if (result != SQLITE_DONE)
                    throw_sqlite_error(impl_->db, "Program lineage association read failed");
            }
            if (exists && associated_lineage_id) {
                if (!publication.run_lineage ||
                    *associated_lineage_id != publication.run_lineage->lineage_id()) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            } else if (!exists && associated_lineage_id) {
                throw std::invalid_argument("Stored Program lineage association is orphaned");
            }

            std::optional<program::ProgramRunLineage> current_lineage;
            if (publication.run_lineage) {
                Statement lineage_query(
                    impl_->db,
                    "SELECT head_id, record_json FROM neograph_harness_program_lineage_heads "
                    "WHERE owner_scope=? AND lineage_id=?");
                lineage_query.bind_text(1, std::string(owner_scope));
                lineage_query.bind_text(2, publication.run_lineage->lineage_id());
                const auto lineage_result = lineage_query.step();
                if (lineage_result == SQLITE_ROW) {
                    current_lineage = program::ProgramRunLineage::parse(lineage_query.text(1));
                    if (current_lineage->id() != lineage_query.text(0) ||
                        current_lineage->owner_scope() != owner_scope ||
                        current_lineage->lineage_id() != publication.run_lineage->lineage_id()) {
                        throw std::invalid_argument("Stored Program lineage head binding is corrupt");
                    }
                } else if (lineage_result != SQLITE_DONE) {
                    throw_sqlite_error(impl_->db, "Program lineage CAS read failed");
                }

                const bool adopting_legacy = exists && !associated_lineage_id;
                if (!current_lineage) {
                    if (!publication.run_generation ||
                        !program::is_valid_program_run_lineage_initial(
                            *publication.run_lineage, *publication.run_generation)) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                } else if (adopting_legacy ||
                           !program::is_valid_program_run_lineage_transition(
                               *current_lineage, *publication.run_lineage,
                               publication.run_generation)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }

                if (current_lineage && !publication.run_generation) {
                    Statement generation_query(
                        impl_->db,
                        "SELECT generation_id, record_json FROM "
                        "neograph_harness_program_generations "
                        "WHERE owner_scope=? AND lineage_id=? AND generation=?");
                    generation_query.bind_text(1, std::string(owner_scope));
                    generation_query.bind_text(2, publication.run_lineage->lineage_id());
                    generation_query.bind_int64(
                        3, static_cast<std::int64_t>(publication.run_lineage->active_generation()));
                    if (generation_query.step() != SQLITE_ROW) {
                        throw std::invalid_argument("Stored Program lineage has no active generation");
                    }
                    const auto active =
                        program::ProgramRunGeneration::parse(generation_query.text(1));
                    if (active.id() != generation_query.text(0) ||
                        !program::does_program_run_generation_bind(
                            active, *publication.run_lineage, publication.run_record)) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                }
                if (current_lineage && publication.run_generation) {
                    Statement previous_generation_query(
                        impl_->db,
                        "SELECT record_json FROM neograph_harness_program_generations "
                        "WHERE owner_scope=? AND lineage_id=? AND generation=?");
                    previous_generation_query.bind_text(1, std::string(owner_scope));
                    previous_generation_query.bind_text(2,
                                                        publication.run_lineage->lineage_id());
                    previous_generation_query.bind_int64(
                        3, static_cast<std::int64_t>(current_lineage->active_generation()));
                    if (previous_generation_query.step() != SQLITE_ROW) {
                        throw std::invalid_argument("Stored Program predecessor generation is missing");
                    }
                    const auto previous_generation = program::ProgramRunGeneration::parse(
                        previous_generation_query.text(0));
                    if (previous_generation.child_depth() !=
                        publication.run_generation->child_depth()) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                    const auto source_wrapper =
                        load_wrapper_locked(owner_scope, previous_generation.run_id());
                    if (!source_wrapper) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                    const auto& source = source_wrapper->run_record();
                    Statement checkpoint_query(
                        impl_->db,
                        "SELECT record_json, owner_scope, bundle_id, coordinate_id, record_id "
                        "FROM neograph_harness_program_javascript_commands "
                        "WHERE run_id=? ORDER BY sequence DESC LIMIT 1");
                    checkpoint_query.bind_text(
                        1, program_storage_run_id(owner_scope, previous_generation.run_id()));
                    if (checkpoint_query.step() != SQLITE_ROW) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                    const auto checkpoint =
                        program::ProgramJavaScriptCommandJournalEntry::parse(
                            checkpoint_query.text(0));
                    if (checkpoint_query.text(1) != owner_scope ||
                        checkpoint_query.text(2) != checkpoint.bundle_id() ||
                        checkpoint_query.text(3) != checkpoint.coordinate_id() ||
                        checkpoint_query.text(4) != checkpoint.id() ||
                        !program::is_valid_program_replacement_transition(
                            previous_generation, *current_lineage, source, checkpoint,
                            *publication.run_generation, *publication.run_lineage,
                            publication.run_record)) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                    Statement duplicate_generation(
                        impl_->db,
                        "SELECT 1 FROM neograph_harness_program_generations "
                        "WHERE owner_scope=? AND lineage_id=? AND generation=?");
                    duplicate_generation.bind_text(1, std::string(owner_scope));
                    duplicate_generation.bind_text(2, publication.run_lineage->lineage_id());
                    duplicate_generation.bind_int64(
                        3, static_cast<std::int64_t>(publication.run_generation->generation()));
                    if (duplicate_generation.step() == SQLITE_ROW) {
                        impl_->exec("ROLLBACK;");
                        return program::ProgramTransitionPublishResult::Conflict;
                    }
                }
            }

            std::optional<program::ProgramRunLineage> current_fork_source;
            if (publication.fork_source_lineage) {
                Statement source_lineage_query(
                    impl_->db,
                    "SELECT head_id, record_json FROM neograph_harness_program_lineage_heads "
                    "WHERE owner_scope=? AND lineage_id=?");
                source_lineage_query.bind_text(1, std::string(owner_scope));
                source_lineage_query.bind_text(
                    2, publication.fork_source_lineage->lineage_id());
                if (source_lineage_query.step() != SQLITE_ROW) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                current_fork_source =
                    program::ProgramRunLineage::parse(source_lineage_query.text(1));
                if (current_fork_source->id() != source_lineage_query.text(0) ||
                    !program::is_valid_program_run_lineage_transition(
                        *current_fork_source, *publication.fork_source_lineage) ||
                    !fork_allocation_fits(*current_fork_source,
                                          *publication.fork_source_lineage,
                                          publication.journal_record.remaining_budget)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }

                Statement source_generation_query(
                    impl_->db,
                    "SELECT record_json FROM neograph_harness_program_generations "
                    "WHERE owner_scope=? AND lineage_id=? AND generation=?");
                source_generation_query.bind_text(1, std::string(owner_scope));
                source_generation_query.bind_text(2, current_fork_source->lineage_id());
                source_generation_query.bind_int64(
                    3, static_cast<std::int64_t>(current_fork_source->active_generation()));
                if (source_generation_query.step() != SQLITE_ROW) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                const auto source_generation = program::ProgramRunGeneration::parse(
                    source_generation_query.text(0));
                Statement source_run_query(
                    impl_->db,
                    "SELECT record_json FROM neograph_harness_runs "
                    "WHERE run_id=? AND owner_scope=?");
                source_run_query.bind_text(
                    1, program_storage_run_id(owner_scope, source_generation.run_id()));
                source_run_query.bind_text(2, std::string(owner_scope));
                if (source_run_query.step() != SQLITE_ROW) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                const auto source = HarnessProgramRunRecord::parse(
                    json::parse(source_run_query.text(0))).run_record();
                const auto fork       = publication.run_record.fork_receipt();
                const auto checkpoint = source.exact_checkpoint();
                const auto resume_binds = [&]() noexcept {
                    if (!fork || fork->storage_schema_version() <
                                     program::ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
                        return true;
                    }
                    try {
                        if (const auto source_pending = source.pending_input()) {
                            const auto target_pending = publication.run_record.pending_input();
                            const auto result =
                                target_pending ? target_pending->consumed_result() : std::nullopt;
                            if (!target_pending || !result ||
                                publication.run_record.pending_effect() ||
                                !fork->matches_initial_resume(target_pending->call_id(), *result)) {
                                return false;
                            }
                            const auto applied = source_pending->submit(
                                target_pending->call_id(), *result,
                                static_cast<std::uint64_t>(publication.run_record.created_at_ms()));
                            return applied.disposition ==
                                       program::ProgramPendingDisposition::Applied &&
                                   applied.value == *target_pending;
                        }
                        if (const auto source_pending = source.pending_effect()) {
                            const auto target_pending = publication.run_record.pending_effect();
                            const auto result =
                                target_pending ? target_pending->reconciled_result() : std::nullopt;
                            if (!target_pending || !result ||
                                publication.run_record.pending_input() ||
                                !fork->matches_initial_resume(target_pending->call_id(), *result)) {
                                return false;
                            }
                            const auto applied = source_pending->submit(
                                target_pending->call_id(), target_pending->effect_id(), *result,
                                static_cast<std::uint64_t>(publication.run_record.created_at_ms()));
                            return applied.disposition ==
                                       program::ProgramPendingDisposition::Applied &&
                                   applied.value == *target_pending;
                        }
                        return !publication.run_record.pending_input() &&
                               !publication.run_record.pending_effect() &&
                               fork->initial_resume_binding() &&
                               !fork->initial_resume_binding()->target_pending_id;
                    } catch (const std::exception&) {
                        return false;
                    }
                };
                if (!fork || !checkpoint || source.run_id() == publication.run_record.run_id() ||
                    source.child_depth() != 0 || !source.invocation().parent_run_id.empty() ||
                    publication.run_record.created_at_ms() < source.updated_at_ms() ||
                    source.continuation().state != program::ContinuationState::Interrupted ||
                    current_fork_source->committed_descendant_budget() != program::RunBudget{} ||
                    !fork->compatible() ||
                    fork->owner_scope() != owner_scope ||
                    fork->source_run_id() != source_generation.run_id() ||
                    fork->source_program_version_id() !=
                        source_generation.program_version_id() ||
                    fork->source_checkpoint_id() != checkpoint->checkpoint_id ||
                    fork->target_program_version_id() !=
                        publication.run_record.program_version_id() ||
                    !resume_binds() || source.id() != current_fork_source->active_run_record_id() ||
                    source.journal_head() != current_fork_source->active_journal_head()) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
            } else if (!exists && publication.run_record.fork_receipt()) {
                impl_->exec("ROLLBACK;");
                return program::ProgramTransitionPublishResult::Conflict;
            }

            const auto new_terminal = publication.run_record.terminal_result();
            const bool publishes_terminal_event =
                !publication.events.empty() &&
                publication.events.back().kind == program::ProgramEventKind::Terminal;

            if (!exists) {
                if (!expected_journal_head.empty() ||
                    !publication.journal_record.previous_id.empty() ||
                    publication.journal_record.sequence != 1 ||
                    !budget_is_empty(publication.journal_record.inflight_reservation) ||
                    publication.events.empty() ||
                    publication.events.front().kind != program::ProgramEventKind::Started ||
                    publication.run_record.event_sequence() != publication.events.size() ||
                    publication.run_record.effect_sequence() != publication.effects.size() ||
                    publication.events.front().sequence != 1 ||
                    (!publication.effects.empty() && publication.effects.front().sequence() != 1) ||
                    !valid_command_history_append(publication.run_record, {},
                                                  publication.commands) ||
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
                std::vector<program::ProgramJavaScriptCommandJournalEntry> previous_commands;
                Statement previous_command_rows(impl_->db,
                                                "SELECT record_json FROM "
                                                "neograph_harness_program_javascript_commands "
                                                "WHERE run_id=? ORDER BY sequence");
                previous_command_rows.bind_text(1, storage_run_id);
                while (true) {
                    const auto command_result = previous_command_rows.step();
                    if (command_result == SQLITE_DONE) break;
                    if (command_result != SQLITE_ROW) {
                        throw_sqlite_error(impl_->db,
                                           "Program JavaScript command history read failed");
                    }
                    previous_commands.push_back(
                        program::ProgramJavaScriptCommandJournalEntry::parse(
                            previous_command_rows.text(0)));
                }
                if (!valid_command_history_append(previous_run, previous_commands,
                                                  publication.commands) ||
                    !valid_command_reservation_transition(previous, publication.journal_record,
                                                          previous_commands, publication)) {
                    impl_->exec("ROLLBACK;");
                    return program::ProgramTransitionPublishResult::Conflict;
                }
                if (previous.id != expected_journal_head ||
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
            if (publication.run_lineage) {
                if (current_lineage) {
                    Statement update_lineage(
                        impl_->db,
                        "UPDATE neograph_harness_program_lineage_heads "
                        "SET head_id=?, record_json=? WHERE owner_scope=? AND lineage_id=?");
                    update_lineage.bind_text(1, publication.run_lineage->id());
                    update_lineage.bind_text(2, publication.run_lineage->serialize_canonical());
                    update_lineage.bind_text(3, std::string(owner_scope));
                    update_lineage.bind_text(4, publication.run_lineage->lineage_id());
                    if (update_lineage.step() != SQLITE_DONE || sqlite3_changes(impl_->db) != 1)
                        throw_sqlite_error(impl_->db, "Program lineage head update failed");
                } else {
                    Statement insert_lineage(
                        impl_->db,
                        "INSERT INTO neograph_harness_program_lineage_heads "
                        "(owner_scope, lineage_id, head_id, record_json) VALUES(?, ?, ?, ?)");
                    insert_lineage.bind_text(1, std::string(owner_scope));
                    insert_lineage.bind_text(2, publication.run_lineage->lineage_id());
                    insert_lineage.bind_text(3, publication.run_lineage->id());
                    insert_lineage.bind_text(4, publication.run_lineage->serialize_canonical());
                    if (insert_lineage.step() != SQLITE_DONE)
                        throw_sqlite_error(impl_->db, "Program lineage head insert failed");
                }
                Statement insert_history(
                    impl_->db,
                    "INSERT INTO neograph_harness_program_lineage_history "
                    "(owner_scope, lineage_id, head_id, record_json) VALUES(?, ?, ?, ?)");
                insert_history.bind_text(1, std::string(owner_scope));
                insert_history.bind_text(2, publication.run_lineage->lineage_id());
                insert_history.bind_text(3, publication.run_lineage->id());
                insert_history.bind_text(4, publication.run_lineage->serialize_canonical());
                if (insert_history.step() != SQLITE_DONE)
                    throw_sqlite_error(impl_->db, "Program lineage history insert failed");
                if (publication.run_generation) {
                    Statement insert_generation(
                        impl_->db,
                        "INSERT INTO neograph_harness_program_generations "
                        "(owner_scope, lineage_id, generation, generation_id, record_json) "
                        "VALUES(?, ?, ?, ?, ?)");
                    insert_generation.bind_text(1, std::string(owner_scope));
                    insert_generation.bind_text(2, publication.run_lineage->lineage_id());
                    insert_generation.bind_int64(
                        3, static_cast<std::int64_t>(publication.run_generation->generation()));
                    insert_generation.bind_text(4, publication.run_generation->id());
                    insert_generation.bind_text(5,
                                                publication.run_generation->serialize_canonical());
                    if (insert_generation.step() != SQLITE_DONE)
                        throw_sqlite_error(impl_->db, "Program generation insert failed");
                    Statement insert_generation_publication(
                        impl_->db,
                        "INSERT INTO neograph_harness_program_generation_publications "
                        "(owner_scope, lineage_id, generation, generation_id, publication_json) "
                        "VALUES(?, ?, ?, ?, ?)");
                    insert_generation_publication.bind_text(1, std::string(owner_scope));
                    insert_generation_publication.bind_text(
                        2, publication.run_lineage->lineage_id());
                    insert_generation_publication.bind_int64(
                        3, static_cast<std::int64_t>(publication.run_generation->generation()));
                    insert_generation_publication.bind_text(
                        4, publication.run_generation->id());
                    insert_generation_publication.bind_text(5, publication_bytes);
                    if (insert_generation_publication.step() != SQLITE_DONE) {
                        throw_sqlite_error(impl_->db,
                                           "Program generation publication insert failed");
                    }
                }
                if (!associated_lineage_id) {
                    Statement insert_association(
                        impl_->db,
                        "INSERT INTO neograph_harness_program_lineage_runs "
                        "(owner_scope, program_run_id, lineage_id) VALUES(?, ?, ?)");
                    insert_association.bind_text(1, std::string(owner_scope));
                    insert_association.bind_text(2, run_id);
                    insert_association.bind_text(3, publication.run_lineage->lineage_id());
                    if (insert_association.step() != SQLITE_DONE)
                        throw_sqlite_error(impl_->db, "Program lineage association insert failed");
                }
            }
            if (publication.fork_source_lineage) {
                Statement update_source_lineage(
                    impl_->db,
                    "UPDATE neograph_harness_program_lineage_heads "
                    "SET head_id=?, record_json=? WHERE owner_scope=? AND lineage_id=?");
                update_source_lineage.bind_text(1, publication.fork_source_lineage->id());
                update_source_lineage.bind_text(
                    2, publication.fork_source_lineage->serialize_canonical());
                update_source_lineage.bind_text(3, std::string(owner_scope));
                update_source_lineage.bind_text(
                    4, publication.fork_source_lineage->lineage_id());
                if (update_source_lineage.step() != SQLITE_DONE ||
                    sqlite3_changes(impl_->db) != 1) {
                    throw_sqlite_error(impl_->db, "Program fork source lineage update failed");
                }
                Statement insert_source_history(
                    impl_->db,
                    "INSERT INTO neograph_harness_program_lineage_history "
                    "(owner_scope, lineage_id, head_id, record_json) VALUES(?, ?, ?, ?)");
                insert_source_history.bind_text(1, std::string(owner_scope));
                insert_source_history.bind_text(
                    2, publication.fork_source_lineage->lineage_id());
                insert_source_history.bind_text(3, publication.fork_source_lineage->id());
                insert_source_history.bind_text(
                    4, publication.fork_source_lineage->serialize_canonical());
                if (insert_source_history.step() != SQLITE_DONE)
                    throw_sqlite_error(impl_->db, "Program fork source history insert failed");
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
            for (const auto& command : publication.commands) {
                insert_javascript_command(storage_run_id, owner_scope, command);
            }
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
#if defined(_WIN32)
            std::abort();
#else
            std::raise(SIGKILL);
            std::abort();
#endif
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
        for (const auto& command : publication.commands) {
            Statement stored(impl_->db,
                             "SELECT record_json FROM neograph_harness_program_javascript_commands "
                             "WHERE run_id=? AND sequence=? AND record_id=? AND owner_scope=? "
                             "AND bundle_id=? AND coordinate_id=?");
            stored.bind_text(1, storage_run_id);
            stored.bind_int64(2, static_cast<std::int64_t>(command.sequence()));
            stored.bind_text(3, command.id());
            stored.bind_text(4, run.owner_scope());
            stored.bind_text(5, run.bundle_id());
            stored.bind_text(6, command.coordinate_id());
            if (stored.step() != SQLITE_ROW || stored.text(0) != command.serialize_canonical()) {
                throw std::invalid_argument(
                    "Stored Program publication JavaScript command is missing");
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

    std::optional<HarnessProgramRunRecord> load_wrapper_locked(
        std::string_view owner_scope, std::string_view run_id) const {
        if (owner_scope.empty() || run_id.empty()) return std::nullopt;
        Statement query(
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

    std::optional<HarnessProgramRunRecord> load_wrapper(std::string_view owner_scope,
                                                        std::string_view run_id) const {
        std::lock_guard lock(impl_->mutex);
        return load_wrapper_locked(owner_scope, run_id);
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
    void insert_javascript_command(const std::string&                                   run_id,
                                   std::string_view                                     owner_scope,
                                   const program::ProgramJavaScriptCommandJournalEntry& command) {
        Statement insert(
            impl_->db,
            "INSERT INTO neograph_harness_program_javascript_commands "
            "(run_id, sequence, owner_scope, bundle_id, coordinate_id, record_id, record_json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)");
        insert.bind_text(1, run_id);
        insert.bind_int64(2, static_cast<std::int64_t>(command.sequence()));
        insert.bind_text(3, std::string(owner_scope));
        insert.bind_text(4, command.bundle_id());
        insert.bind_text(5, command.coordinate_id());
        insert.bind_text(6, command.id());
        insert.bind_text(7, command.serialize_canonical());
        if (insert.step() != SQLITE_DONE) {
            throw_sqlite_error(impl_->db, "Program JavaScript command insert failed");
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

void SqliteHarnessRecordStore::save_contract_run(
    const HarnessProgramArtifactRecord& artifact,
    const program::ProgramRunRecord&     run_record,
    const program::ContractRun&           contract_run) {
    validate_contract_run_binding(artifact, run_record, contract_run);
    const auto stored_artifact = load_artifact(artifact.artifact_id());
    if (!stored_artifact || stored_artifact->dump() != artifact.serialize().dump()) {
        throw std::invalid_argument("Harness contract artifact persistence binding mismatch");
    }
    const auto storage_id = program_storage_run_id(run_record.owner_scope(), run_record.run_id());
    const auto serialized = contract_run.serialize_canonical();

    std::lock_guard lock(impl_->mutex);
    impl_->exec("BEGIN IMMEDIATE;");
    try {
        Statement current(
            impl_->db,
            "SELECT owner_scope, artifact_id, bundle_id, program_version_id, program_run_id, "
            "record_json FROM neograph_harness_runs WHERE run_id=? AND program_run_id<>''");
        current.bind_text(1, storage_id);
        if (current.step() != SQLITE_ROW) {
            throw std::invalid_argument("Harness contract requires a persisted Program run");
        }
        auto wrapper = HarnessProgramRunRecord::parse(json::parse(current.text(5)));
        if (current.text(0) != run_record.owner_scope() ||
            current.text(1) != artifact.artifact_id() || current.text(2) != run_record.bundle_id() ||
            current.text(3) != run_record.program_version_id() ||
            current.text(4) != wrapper.run_record().id() ||
            wrapper.run_record().owner_scope() != run_record.owner_scope() ||
            wrapper.run_record().run_id() != run_record.run_id() ||
            wrapper.run_record().bundle_id() != run_record.bundle_id() ||
            wrapper.run_record().program_version_id() != run_record.program_version_id() ||
            wrapper.run_record().binding_fingerprint() != run_record.binding_fingerprint()) {
            throw std::invalid_argument("Harness contract Program run binding is corrupt");
        }
        wrapper.validate_artifact(artifact);
        // The Program transition can advance between the caller's snapshot and
        // this transaction. Validate the contract against the authoritative
        // record instead of requiring byte equality with a stale snapshot.
        validate_contract_run_binding(artifact, wrapper.run_record(), contract_run);

        Statement existing(
            impl_->db,
            "SELECT owner_scope, artifact_id, bundle_id, program_version_id, manifest_hash, "
            "record_json FROM neograph_harness_contract_runs WHERE run_id=?");
        existing.bind_text(1, storage_id);
        const auto existing_result = existing.step();
        if (existing_result != SQLITE_DONE && existing_result != SQLITE_ROW) {
            throw_sqlite_error(impl_->db, "contract run read failed");
        }
        if (existing_result == SQLITE_ROW) {
            if (existing.text(0) != artifact.owner_scope() ||
                existing.text(1) != artifact.artifact_id() ||
                existing.text(2) != run_record.bundle_id() ||
                existing.text(3) != run_record.program_version_id() ||
                existing.text(4) != contract_run.manifest().content_hash()) {
                throw std::invalid_argument("Stored Harness contract SQLite binding is corrupt");
            }
            const auto stored = program::ContractRun::parse(existing.text(5));
            validate_contract_run_binding(artifact, wrapper.run_record(), stored);
            if (stored.serialize_canonical() != existing.text(5)) {
                throw std::invalid_argument("Stored Harness contract canonical bytes are corrupt");
            }
            if (existing.text(5) == serialized) {
                impl_->exec("COMMIT;");
                return;
            }
            Statement update(
                impl_->db,
                "UPDATE neograph_harness_contract_runs SET manifest_hash=?, record_json=?, "
                "updated_at_ms=? WHERE run_id=? AND owner_scope=? AND artifact_id=? "
                "AND bundle_id=? AND program_version_id=?");
            update.bind_text(1, contract_run.manifest().content_hash());
            update.bind_text(2, serialized);
            update.bind_int64(3, unix_millis());
            update.bind_text(4, storage_id);
            update.bind_text(5, artifact.owner_scope());
            update.bind_text(6, artifact.artifact_id());
            update.bind_text(7, run_record.bundle_id());
            update.bind_text(8, run_record.program_version_id());
            if (update.step() != SQLITE_DONE || sqlite3_changes(impl_->db) != 1) {
                throw_sqlite_error(impl_->db, "contract run update failed");
            }
        } else {
            Statement insert(
                impl_->db,
                "INSERT INTO neograph_harness_contract_runs "
                "(run_id, owner_scope, artifact_id, bundle_id, program_version_id, "
                "manifest_hash, record_json, updated_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
            insert.bind_text(1, storage_id);
            insert.bind_text(2, artifact.owner_scope());
            insert.bind_text(3, artifact.artifact_id());
            insert.bind_text(4, run_record.bundle_id());
            insert.bind_text(5, run_record.program_version_id());
            insert.bind_text(6, contract_run.manifest().content_hash());
            insert.bind_text(7, serialized);
            insert.bind_int64(8, unix_millis());
            if (insert.step() != SQLITE_DONE) {
                throw_sqlite_error(impl_->db, "contract run insert failed");
            }
        }
        impl_->exec("COMMIT;");
    } catch (...) {
        try {
            impl_->exec("ROLLBACK;");
        } catch (...) {}
        throw;
    }
}

std::optional<program::ContractRun> SqliteHarnessRecordStore::load_contract_run(
    const HarnessProgramArtifactRecord& artifact,
    const program::ProgramRunRecord&     run_record) const {
    (void)contract_manifest_for_artifact(artifact);
    const auto storage_id = program_storage_run_id(run_record.owner_scope(), run_record.run_id());
    std::lock_guard lock(impl_->mutex);
    Statement query(
        impl_->db,
        "SELECT owner_scope, artifact_id, bundle_id, program_version_id, manifest_hash, "
        "record_json FROM neograph_harness_contract_runs WHERE run_id=?");
    query.bind_text(1, storage_id);
    const auto result = query.step();
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw_sqlite_error(impl_->db, "contract run read failed");
    if (query.text(0) != artifact.owner_scope() || query.text(1) != artifact.artifact_id() ||
        query.text(2) != run_record.bundle_id() ||
        query.text(3) != run_record.program_version_id()) {
        throw std::invalid_argument("Stored Harness contract SQLite binding is corrupt");
    }
    if (query.text(4) != contract_manifest_for_artifact(artifact).content_hash()) {
        throw std::invalid_argument("Stored Harness contract manifest hash is corrupt");
    }
    const auto contract_run = program::ContractRun::parse(query.text(5));
    if (contract_run.serialize_canonical() != query.text(5)) {
        throw std::invalid_argument("Stored Harness contract canonical bytes are corrupt");
    }
    validate_contract_run_binding(artifact, run_record, contract_run);
    return contract_run;
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
