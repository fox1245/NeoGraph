#include <neograph/program/sqlite_transition_store.h>

#include "canonical_json.h"
#include "node_retry_budget.h"
#include "command_publication_head.h"
#include "transition_validator.h"
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

using detail::transition::Borrowed;

[[noreturn]] void throw_sqlite(sqlite3* db, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " +
                             (db ? sqlite3_errmsg(db) : "SQLite unavailable"));
}

void exec(sqlite3* db, std::string_view sql) {
    char* error = nullptr;
    if (sqlite3_exec(db, sql.data(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SQLite " + std::string(sql.substr(0, 48)) + ": " + message);
    }
}

class Statement final {
public:
    Statement(sqlite3* db, std::string_view sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr) !=
            SQLITE_OK)
            throw_sqlite(db_, "SQLite prepare");
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { sqlite3_finalize(statement_); }

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind text");
    }

    void bind_blob(int index, std::string_view value) {
        if (sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind blob");
    }

    void bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind null");
    }

    void bind_uint64(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument("Program transition sequence exceeds SQLite integer range");
        if (sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            throw_sqlite(db_, "SQLite bind integer");
    }

    bool step_row() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(db_, "SQLite step");
    }

    void step_done() {
        if (sqlite3_step(statement_) != SQLITE_DONE) throw_sqlite(db_, "SQLite step");
    }

    void reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK) throw_sqlite(db_, "SQLite reset");
        if (sqlite3_clear_bindings(statement_) != SQLITE_OK)
            throw_sqlite(db_, "SQLite clear bindings");
    }

private:
    sqlite3*      db_        = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction final {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (!committed_) {
            char* error = nullptr;
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, &error);
            sqlite3_free(error);
        }
    }

    void commit() {
        exec(db_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3* db_        = nullptr;
    bool     committed_ = false;
};

std::string column_blob(sqlite3_stmt* statement, int index) {
    const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement, index));
    const int size = sqlite3_column_bytes(statement, index);
    if ((!bytes && size != 0) || size < 0)
        throw std::runtime_error("SQLite returned an invalid transition blob");
    return std::string(bytes ? bytes : "", static_cast<std::size_t>(size));
}

std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
    const int size = sqlite3_column_bytes(statement, index);
    if ((!bytes && size != 0) || size < 0)
        throw std::runtime_error("SQLite returned an invalid transition text value");
    return std::string(bytes ? bytes : "", static_cast<std::size_t>(size));
}

std::optional<std::string> column_optional_blob(sqlite3_stmt* statement, int index) {
    if (sqlite3_column_type(statement, index) == SQLITE_NULL) return std::nullopt;
    return column_blob(statement, index);
}

bool table_exists(sqlite3* db, std::string_view name) {
    Statement statement(
        db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1 LIMIT 1");
    statement.bind_text(1, name);
    return statement.step_row();
}

struct StoredHead {
    ProgramRunRecord          run_record;
    ProgramJournalRecord      journal_record;
    std::optional<MigrationPlan> migration_plan;
    std::string               last_publication_bytes;
};

std::optional<StoredHead> load_head(sqlite3* db, std::string_view owner_scope,
                                    std::string_view run_id) {
    Statement statement(
        db, "SELECT run_record_bytes, journal_record_bytes, migration_plan_bytes, "
            "last_publication_bytes FROM program_transition_run_heads_v2 "
            "WHERE owner_scope = ?1 AND run_id = ?2");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;

    const auto migration_plan_bytes = column_optional_blob(statement.get(), 2);
    return StoredHead{ProgramRunRecord::parse(column_blob(statement.get(), 0)),
                      ProgramJournalRecord::parse(column_blob(statement.get(), 1)),
                      migration_plan_bytes
                          ? std::optional<MigrationPlan>(MigrationPlan::parse(*migration_plan_bytes))
                          : std::nullopt,
                       column_blob(statement.get(), 3)};
}

std::optional<ProgramRunLineage> load_current_lineage_head(sqlite3* db,
                                                            std::string_view owner_scope,
                                                            std::string_view lineage_id) {
    Statement statement(
        db, "SELECT head_bytes FROM program_run_lineage_heads_v1 "
            "WHERE owner_scope = ?1 AND lineage_id = ?2");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, lineage_id);
    if (!statement.step_row()) return std::nullopt;
    auto lineage = ProgramRunLineage::parse(column_blob(statement.get(), 0));
    if (lineage.owner_scope() != owner_scope || lineage.lineage_id() != lineage_id)
        throw std::invalid_argument("Stored Program lineage head binding is corrupt");
    return lineage;
}

std::optional<ProgramRunLineage> load_historical_lineage_head(sqlite3* db,
                                                               std::string_view owner_scope,
                                                               std::string_view lineage_id,
                                                               std::string_view head_id) {
    Statement statement(
        db, "SELECT canonical_bytes FROM program_run_lineage_history_v1 "
            "WHERE owner_scope = ?1 AND lineage_id = ?2 AND head_id = ?3");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, lineage_id);
    statement.bind_text(3, head_id);
    if (!statement.step_row()) return std::nullopt;
    auto lineage = ProgramRunLineage::parse(column_blob(statement.get(), 0));
    if (lineage.owner_scope() != owner_scope || lineage.lineage_id() != lineage_id ||
        lineage.id() != head_id)
        throw std::invalid_argument("Stored Program lineage history is corrupt");
    return lineage;
}

std::optional<ProgramRunGeneration> load_generation_record(sqlite3* db,
                                                            std::string_view owner_scope,
                                                            std::string_view lineage_id,
                                                            std::uint64_t generation) {
    Statement statement(
        db, "SELECT generation_id, canonical_bytes FROM program_run_generations_v1 "
            "WHERE owner_scope = ?1 AND lineage_id = ?2 AND generation = ?3");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, lineage_id);
    statement.bind_uint64(3, generation);
    if (!statement.step_row()) return std::nullopt;
    auto value = ProgramRunGeneration::parse(column_blob(statement.get(), 1));
    if (value.owner_scope() != owner_scope || value.lineage_id() != lineage_id ||
        value.generation() != generation || value.id() != column_text(statement.get(), 0))
        throw std::invalid_argument("Stored Program generation binding is corrupt");
    return value;
}

std::optional<ProgramTransitionPublication> load_generation_initial_publication_record(
    sqlite3* db, std::string_view owner_scope, std::string_view lineage_id,
    std::uint64_t generation) {
    Statement statement(
        db, "SELECT p.generation_id, p.canonical_bytes, g.generation_id FROM "
            "program_run_generation_publications_v1 p JOIN program_run_generations_v1 g "
            "ON g.owner_scope = p.owner_scope AND g.lineage_id = p.lineage_id "
            "AND g.generation = p.generation "
            "WHERE p.owner_scope = ?1 AND p.lineage_id = ?2 AND p.generation = ?3");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, lineage_id);
    statement.bind_uint64(3, generation);
    if (!statement.step_row()) return std::nullopt;
    auto publication = ProgramTransitionPublication::parse(column_blob(statement.get(), 1));
    if (!publication.run_generation || !publication.run_lineage ||
        column_text(statement.get(), 0) != column_text(statement.get(), 2) ||
        publication.run_generation->id() != column_text(statement.get(), 2) ||
        publication.run_generation->owner_scope() != owner_scope ||
        publication.run_generation->lineage_id() != lineage_id ||
        publication.run_generation->generation() != generation) {
        throw std::invalid_argument("Stored Program generation publication binding is corrupt");
    }
    return publication;
}

std::optional<GraphMigrationCapsule> load_graph_migration_capsule_record(
    sqlite3* db, std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) {
    Statement statement(
        db, "SELECT capsule_id, capsule_bytes FROM program_graph_migration_capsules_v1 "
            "WHERE owner_scope = ?1 AND source_run_id = ?2 AND source_lineage_head_id = ?3");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, source_run_id);
    statement.bind_text(3, source_lineage_head_id);
    if (!statement.step_row()) return std::nullopt;
    auto capsule = GraphMigrationCapsule::parse(column_blob(statement.get(), 1));
    if (capsule.owner_scope() != owner_scope || capsule.source_run_id() != source_run_id ||
        capsule.source_lineage_head_id() != source_lineage_head_id ||
        capsule.id() != column_text(statement.get(), 0)) {
        throw std::invalid_argument("Stored graph migration capsule binding is corrupt");
    }
    return capsule;
}

std::optional<ProgramExecutionLease> load_execution_lease_record(
    sqlite3* db, std::string_view owner_scope, std::string_view run_id) {
    Statement statement(
        db, "SELECT lease_id, lease_bytes FROM program_execution_leases_v1 "
            "WHERE owner_scope = ?1 AND run_id = ?2");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;
    auto lease = ProgramExecutionLease::parse(column_blob(statement.get(), 1));
    if (lease.owner_scope() != owner_scope || lease.run_id() != run_id ||
        lease.id() != column_text(statement.get(), 0)) {
        throw std::invalid_argument("Stored Program execution lease binding is corrupt");
    }
    return lease;
}

std::optional<std::string> load_run_lineage_id(sqlite3* db,
                                               std::string_view owner_scope,
                                               std::string_view run_id) {
    Statement statement(
        db, "SELECT lineage_id FROM program_run_lineage_runs_v1 "
            "WHERE owner_scope = ?1 AND run_id = ?2");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;
    return column_text(statement.get(), 0);
}

std::optional<ProgramJavaScriptCommandJournalEntry> load_latest_javascript_command(
    sqlite3* db, std::string_view owner_scope, std::string_view run_id) {
    Statement statement(
        db, "SELECT sequence, coordinate_id, canonical_bytes FROM "
            "program_transition_javascript_command_log_v2 "
            "WHERE owner_scope = ?1 AND run_id = ?2 ORDER BY sequence DESC LIMIT 1");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;
    const auto sequence = sqlite3_column_int64(statement.get(), 0);
    auto entry = ProgramJavaScriptCommandJournalEntry::parse(column_blob(statement.get(), 2));
    if (sequence <= 0 || entry.sequence() != static_cast<std::uint64_t>(sequence) ||
        column_text(statement.get(), 1) != entry.coordinate_id()) {
        throw std::invalid_argument(
            "Stored JavaScript command journal coordinate column mismatch");
    }
    return entry;
}

std::vector<ProgramJavaScriptCommandJournalEntry> load_all_javascript_commands(
    sqlite3* db, std::string_view owner_scope, std::string_view run_id) {
    Statement statement(
        db, "SELECT sequence, coordinate_id, canonical_bytes FROM "
            "program_transition_javascript_command_log_v2 "
            "WHERE owner_scope = ?1 AND run_id = ?2 ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    while (statement.step_row()) {
        const auto sequence = sqlite3_column_int64(statement.get(), 0);
        auto entry = ProgramJavaScriptCommandJournalEntry::parse(column_blob(statement.get(), 2));
        if (sequence <= 0 || entry.sequence() != static_cast<std::uint64_t>(sequence) ||
            column_text(statement.get(), 1) != entry.coordinate_id()) {
            throw std::invalid_argument(
                "Stored JavaScript command journal coordinate column mismatch");
        }
        commands.push_back(std::move(entry));
    }
    if (!commands.empty()) {
        const auto head = load_head(db, owner_scope, run_id);
        if (!head ||
            !detail::transition::valid_command_history_append(head->run_record, {}, commands)) {
            throw std::invalid_argument("Stored JavaScript command journal history is corrupt");
        }
    }
    return commands;
}

std::string context_publication_bytes(const ProgramContextPublication& context) {
    json artifacts = json::array();
    for (const auto& artifact : context.artifacts) {
        artifacts.push_back(detail::parse_json_strict(artifact.serialize_canonical()));
    }
    return detail::canonical_json_bytes(
        json{{"epoch", detail::parse_json_strict(context.epoch.serialize_canonical())},
             {"artifacts", std::move(artifacts)},
             {"assembly_receipt",
              detail::parse_json_strict(context.assembly_receipt.serialize_canonical())}});
}

ProgramContextPublication parse_context_publication_bytes(std::string_view bytes,
                                                           const ProgramRunRecord& run) {
    const auto encoded = detail::parse_json_strict(bytes);
    if (!encoded.is_object()) {
        throw std::invalid_argument("Stored Program context publication must be an object");
    }
    detail::reject_unknown_fields(encoded, "Stored Program context publication",
                                  {"epoch", "artifacts", "assembly_receipt"});
    if (!encoded.contains("epoch") || !encoded.contains("artifacts") ||
        !encoded.contains("assembly_receipt") || !encoded.at("artifacts").is_array()) {
        throw std::invalid_argument("Stored Program context publication is incomplete");
    }
    auto epoch = ContextEpoch::parse(detail::canonical_json_bytes(encoded.at("epoch")));
    std::vector<ContextArtifact> artifacts;
    artifacts.reserve(encoded.at("artifacts").size());
    for (const auto& artifact : encoded.at("artifacts")) {
        artifacts.push_back(ContextArtifact::parse(detail::canonical_json_bytes(artifact)));
    }
    auto receipt = ContextAssemblyReceipt::parse(
        detail::canonical_json_bytes(encoded.at("assembly_receipt")), epoch, artifacts);
    ProgramContextPublication context{std::move(epoch), std::move(artifacts), std::move(receipt)};
    validate_program_context_publication(context, run);
    return context;
}

std::vector<ProgramContextPublication> load_context_publication_history(
    sqlite3* db, std::string_view owner_scope, std::string_view run_id,
    std::uint64_t after_sequence = 0) {
    const auto head = load_head(db, owner_scope, run_id);
    if (!head) return {};
    Statement statement(
        db, "SELECT sequence, canonical_bytes FROM program_transition_context_log_v1 "
            "WHERE owner_scope = ?1 AND run_id = ?2 AND sequence > ?3 ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_uint64(3, after_sequence);
    std::vector<ProgramContextPublication> result;
    while (statement.step_row()) {
        const auto stored_sequence = sqlite3_column_int64(statement.get(), 0);
        if (stored_sequence <= 0) {
            throw std::invalid_argument("Stored Program context sequence is corrupt");
        }
        auto context = parse_context_publication_bytes(column_blob(statement.get(), 1), head->run_record);
        if (context.epoch.sequence() != static_cast<std::uint64_t>(stored_sequence)) {
            throw std::invalid_argument("Stored Program context sequence column mismatch");
        }
        result.push_back(std::move(context));
    }
    return result;
}

bool valid_context_history(const std::vector<ProgramContextPublication>& history) {
    std::vector<ProgramContextPublication> prefix;
    prefix.reserve(history.size());
    for (const auto& context : history) {
        if (!is_valid_program_context_history_append(prefix, context)) return false;
        prefix.push_back(context);
    }
    return true;
}

std::vector<HookOutboxEntry> load_hook_outbox_heads(sqlite3* db, std::string_view owner_scope,
                                                      std::string_view run_id,
                                                      const ProgramRunRecord& run) {
    Statement statement(db, "SELECT invocation_id, canonical_bytes FROM "
                          "program_transition_hook_outbox_log_v1 WHERE owner_scope=?1 AND run_id=?2 "
                          "ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope); statement.bind_text(2, run_id);
    std::map<std::string, HookOutboxEntry, std::less<>> heads;
    while (statement.step_row()) {
        auto entry = HookOutboxEntry::parse(column_blob(statement.get(), 1));
        if (column_text(statement.get(), 0) != entry.data().invocation.id())
            throw std::invalid_argument("Stored Program hook invocation binding is corrupt");
        std::vector<HookOutboxEntry> current;
        for (const auto& [_, head] : heads) current.push_back(head);
        if (!is_valid_program_hook_history_append(current, {entry}, run))
            throw std::invalid_argument("Stored Program hook outbox transition is corrupt");
        heads.insert_or_assign(entry.data().invocation.id(), std::move(entry));
    }
    std::vector<HookOutboxEntry> result;
    for (auto& [_, entry] : heads) result.push_back(std::move(entry));
    return result;
}

std::vector<ProgramChildSynthesisRecord> load_synthesis_heads(sqlite3*         db,
                                                              std::string_view owner_scope,
                                                              std::string_view run_id) {
    std::vector<ProgramChildSynthesisRecord> heads;
    Statement                                rows(
        db,
        "SELECT proposal_id, head_id, canonical_bytes FROM program_transition_synthesis_log_v1 "
                                       "WHERE owner_scope=?1 AND run_id=?2 ORDER BY sequence ASC");
    rows.bind_text(1, owner_scope);
    rows.bind_text(2, run_id);
    while (rows.step_row()) {
        auto entry = ProgramChildSynthesisRecord::parse(column_blob(rows.get(), 2));
        if (entry.data().proposal.id() != column_text(rows.get(), 0) ||
            entry.id() != column_text(rows.get(), 1))
            throw std::invalid_argument("Stored child synthesis key is corrupt");
        if (entry.data().proposal.data().owner_scope != owner_scope ||
            entry.data().parent.run.run_id() != run_id ||
            !is_valid_program_child_synthesis_append(heads, entry))
            throw std::invalid_argument("Stored child synthesis history is corrupt");
        auto found = std::find_if(heads.begin(), heads.end(), [&](const auto& head) {
            return head.data().proposal.id() == entry.data().proposal.id();
        });
        if (found == heads.end())
            heads.push_back(std::move(entry));
        else
            *found = std::move(entry);
    }
    return heads;
}

// The stored state as the transition validator reads it, inside the caller's transaction.
class SqliteReads final : public detail::transition::TransitionReads {
public:
    SqliteReads(sqlite3* db, std::string_view owner_scope) : db_(db), owner_scope_(owner_scope) {}

    std::optional<GraphMigrationCapsule> graph_migration_capsule(
        std::string_view run_id, std::string_view lineage_head_id) override {
        return load_graph_migration_capsule_record(db_, owner_scope_, run_id, lineage_head_id);
    }
    bool effect_id_published(std::string_view run_id, std::string_view effect_id) override {
        Statement statement(
            db_, "SELECT 1 FROM program_transition_effect_log_v2 "
                 "WHERE owner_scope = ?1 AND run_id = ?2 AND effect_id = ?3 LIMIT 1");
        statement.bind_text(1, owner_scope_);
        statement.bind_text(2, run_id);
        statement.bind_text(3, effect_id);
        return statement.step_row();
    }
    Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>> command_log(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>>(
            load_all_javascript_commands(db_, owner_scope_, run_id));
    }
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(
        std::string_view run_id) override {
        return load_latest_javascript_command(db_, owner_scope_, run_id);
    }
    std::optional<ProgramRunRecord> run_record(std::string_view run_id, Lock) override {
        auto head = load_head(db_, owner_scope_, run_id);
        if (!head) return std::nullopt;
        return std::move(head->run_record);
    }
    std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id, Lock) override {
        return load_current_lineage_head(db_, owner_scope_, lineage_id);
    }
    std::optional<ProgramRunGeneration> generation(std::string_view lineage_id,
                                                   std::uint64_t    ordinal) override {
        return load_generation_record(db_, owner_scope_, lineage_id, ordinal);
    }
    std::optional<std::string> run_lineage_id(std::string_view run_id) override {
        return load_run_lineage_id(db_, owner_scope_, run_id);
    }
    Borrowed<std::vector<ProgramContextPublication>> context_history(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramContextPublication>>(
            load_context_publication_history(db_, owner_scope_, run_id));
    }
    bool context_history_is_intact(const std::vector<ProgramContextPublication>& history) override {
        return valid_context_history(history);
    }
    Borrowed<std::vector<ProgramChildSynthesisRecord>> synthesis_heads(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramChildSynthesisRecord>>(
            load_synthesis_heads(db_, owner_scope_, run_id));
    }
    Borrowed<std::vector<HookOutboxEntry>> hook_heads(std::string_view       run_id,
                                                      const ProgramRunRecord& run) override {
        return Borrowed<std::vector<HookOutboxEntry>>(
            load_hook_outbox_heads(db_, owner_scope_, run_id, run));
    }

private:
    sqlite3*         db_;
    std::string_view owner_scope_;
};


void create_v2_schema(sqlite3* db) {
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_run_heads_v2 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
             "run_record_bytes BLOB NOT NULL, journal_record_bytes BLOB NOT NULL, "
             "migration_plan_bytes BLOB, last_publication_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, run_id))");
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_event_log_v2 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence INTEGER NOT NULL, "
             "canonical_bytes BLOB NOT NULL, PRIMARY KEY(owner_scope, run_id, sequence), "
             "FOREIGN KEY(owner_scope, run_id) REFERENCES "
             "program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_effect_log_v2 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence INTEGER NOT NULL, "
             "effect_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, run_id, sequence), UNIQUE(owner_scope, run_id, effect_id), "
             "FOREIGN KEY(owner_scope, run_id) REFERENCES "
             "program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_javascript_command_log_v2 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence INTEGER NOT NULL, "
             "coordinate_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, run_id, sequence), "
              "FOREIGN KEY(owner_scope, run_id) REFERENCES "
               "program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_context_log_v1 ("
              "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence INTEGER NOT NULL, "
              "canonical_bytes BLOB NOT NULL, PRIMARY KEY(owner_scope, run_id, sequence), "
              "FOREIGN KEY(owner_scope, run_id) REFERENCES "
              "program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db,
         "CREATE TABLE IF NOT EXISTS program_transition_synthesis_log_v1 ("
         "sequence INTEGER PRIMARY KEY AUTOINCREMENT, owner_scope TEXT NOT NULL, run_id TEXT NOT "
         "NULL, "
         "proposal_id TEXT NOT NULL, head_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
         "UNIQUE(owner_scope, run_id, head_id), FOREIGN KEY(owner_scope, run_id) "
         "REFERENCES program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db,
         "CREATE INDEX IF NOT EXISTS program_transition_synthesis_run_v1 ON "
         "program_transition_synthesis_log_v1(owner_scope, run_id, sequence)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_transition_hook_outbox_log_v1 ("
             "sequence INTEGER PRIMARY KEY AUTOINCREMENT, owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
             "invocation_id TEXT NOT NULL, head_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
             "FOREIGN KEY(owner_scope, run_id) REFERENCES program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE)");
    exec(db, "CREATE INDEX IF NOT EXISTS program_transition_hook_outbox_log_run_v1 "
             "ON program_transition_hook_outbox_log_v1(owner_scope, run_id, sequence)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_run_lineage_heads_v1 ("
             "owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, head_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, lineage_id))");
    exec(db, "CREATE TABLE IF NOT EXISTS program_run_generations_v1 ("
             "owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, generation INTEGER NOT NULL, "
             "generation_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, lineage_id, generation), "
             "UNIQUE(owner_scope, lineage_id, generation_id), "
             "FOREIGN KEY(owner_scope, lineage_id) REFERENCES "
             "program_run_lineage_heads_v1(owner_scope, lineage_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_run_generation_publications_v1 ("
             "owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, generation INTEGER NOT NULL, "
             "generation_id TEXT NOT NULL, canonical_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, lineage_id, generation), "
             "FOREIGN KEY(owner_scope, lineage_id, generation) REFERENCES "
             "program_run_generations_v1(owner_scope, lineage_id, generation) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_run_lineage_history_v1 ("
             "owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, head_id TEXT NOT NULL, "
             "canonical_bytes BLOB NOT NULL, PRIMARY KEY(owner_scope, lineage_id, head_id), "
             "FOREIGN KEY(owner_scope, lineage_id) REFERENCES "
             "program_run_lineage_heads_v1(owner_scope, lineage_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_run_lineage_runs_v1 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, lineage_id TEXT NOT NULL, "
             "PRIMARY KEY(owner_scope, run_id), "
             "FOREIGN KEY(owner_scope, lineage_id) REFERENCES "
             "program_run_lineage_heads_v1(owner_scope, lineage_id) ON DELETE CASCADE)");
    exec(db, "CREATE TABLE IF NOT EXISTS program_graph_migration_capsules_v1 ("
             "owner_scope TEXT NOT NULL, source_run_id TEXT NOT NULL, "
             "source_lineage_head_id TEXT NOT NULL, capsule_id TEXT NOT NULL, "
             "capsule_bytes BLOB NOT NULL, "
             "PRIMARY KEY(owner_scope, source_run_id, source_lineage_head_id))");
    exec(db, "CREATE TABLE IF NOT EXISTS program_execution_leases_v1 ("
             "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, lease_id TEXT NOT NULL, "
             "lease_bytes BLOB NOT NULL, PRIMARY KEY(owner_scope, run_id))");
}

void insert_lineage_head(sqlite3* db, const ProgramRunLineage& lineage) {
    Statement statement(
        db, "INSERT INTO program_run_lineage_heads_v1"
            "(owner_scope, lineage_id, head_bytes) VALUES(?1, ?2, ?3)");
    statement.bind_text(1, lineage.owner_scope());
    statement.bind_text(2, lineage.lineage_id());
    statement.bind_blob(3, lineage.serialize_canonical());
    statement.step_done();
}

void update_lineage_head(sqlite3* db, const ProgramRunLineage& lineage) {
    Statement statement(
        db, "UPDATE program_run_lineage_heads_v1 SET head_bytes = ?1 "
            "WHERE owner_scope = ?2 AND lineage_id = ?3");
    statement.bind_blob(1, lineage.serialize_canonical());
    statement.bind_text(2, lineage.owner_scope());
    statement.bind_text(3, lineage.lineage_id());
    statement.step_done();
}

void insert_generation(sqlite3* db, const ProgramRunGeneration& generation) {
    Statement statement(
        db, "INSERT INTO program_run_generations_v1"
            "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
            "VALUES(?1, ?2, ?3, ?4, ?5)");
    statement.bind_text(1, generation.owner_scope());
    statement.bind_text(2, generation.lineage_id());
    statement.bind_uint64(3, generation.generation());
    statement.bind_text(4, generation.id());
    statement.bind_blob(5, generation.serialize_canonical());
    statement.step_done();
}

void insert_generation_initial_publication(
    sqlite3* db, const ProgramRunGeneration& generation, std::string_view publication_bytes) {
    Statement statement(
        db, "INSERT INTO program_run_generation_publications_v1"
            "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
            "VALUES(?1, ?2, ?3, ?4, ?5)");
    statement.bind_text(1, generation.owner_scope());
    statement.bind_text(2, generation.lineage_id());
    statement.bind_uint64(3, generation.generation());
    statement.bind_text(4, generation.id());
    statement.bind_blob(5, publication_bytes);
    statement.step_done();
}

void insert_lineage_history(sqlite3* db, const ProgramRunLineage& lineage) {
    Statement statement(
        db, "INSERT INTO program_run_lineage_history_v1"
            "(owner_scope, lineage_id, head_id, canonical_bytes) VALUES(?1, ?2, ?3, ?4)");
    statement.bind_text(1, lineage.owner_scope());
    statement.bind_text(2, lineage.lineage_id());
    statement.bind_text(3, lineage.id());
    statement.bind_blob(4, lineage.serialize_canonical());
    statement.step_done();
}

void insert_run_lineage(sqlite3* db,
                        std::string_view owner_scope,
                        std::string_view run_id,
                        std::string_view lineage_id) {
    Statement statement(
        db, "INSERT INTO program_run_lineage_runs_v1"
            "(owner_scope, run_id, lineage_id) VALUES(?1, ?2, ?3)");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_text(3, lineage_id);
    statement.step_done();
}

void insert_graph_migration_capsule(sqlite3* db,
                                    const GraphMigrationCapsule& capsule) {
    Statement statement(
        db, "INSERT INTO program_graph_migration_capsules_v1"
            "(owner_scope, source_run_id, source_lineage_head_id, capsule_id, capsule_bytes) "
            "VALUES(?1, ?2, ?3, ?4, ?5)");
    statement.bind_text(1, capsule.owner_scope());
    statement.bind_text(2, capsule.source_run_id());
    statement.bind_text(3, capsule.source_lineage_head_id());
    statement.bind_text(4, capsule.id());
    statement.bind_blob(5, capsule.serialize_canonical());
    statement.step_done();
}

void insert_execution_lease(sqlite3* db, const ProgramExecutionLease& lease) {
    Statement statement(
        db, "INSERT INTO program_execution_leases_v1"
            "(owner_scope, run_id, lease_id, lease_bytes) VALUES(?1, ?2, ?3, ?4) "
            "ON CONFLICT(owner_scope, run_id) DO UPDATE SET "
            "lease_id = excluded.lease_id, lease_bytes = excluded.lease_bytes");
    statement.bind_text(1, lease.owner_scope());
    statement.bind_text(2, lease.run_id());
    statement.bind_text(3, lease.id());
    statement.bind_blob(4, lease.serialize_canonical());
    statement.step_done();
}

void delete_execution_lease(sqlite3* db, const ProgramExecutionLease& lease) {
    Statement statement(
        db, "DELETE FROM program_execution_leases_v1 "
            "WHERE owner_scope = ?1 AND run_id = ?2 AND lease_id = ?3");
    statement.bind_text(1, lease.owner_scope());
    statement.bind_text(2, lease.run_id());
    statement.bind_text(3, lease.id());
    statement.step_done();
    if (sqlite3_changes(db) != 1) {
        throw std::runtime_error("Program execution lease delete lost its compare-and-swap");
    }
}

void insert_head(sqlite3* db, std::string_view owner_scope, const ProgramRunRecord& run_record,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    Statement statement(
        db, "INSERT INTO program_transition_run_heads_v2"
            "(owner_scope, run_id, run_record_bytes, journal_record_bytes, migration_plan_bytes, "
            "last_publication_bytes) VALUES(?1, ?2, ?3, ?4, ?5, ?6)");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_record.run_id());
    statement.bind_blob(3, run_record.serialize_canonical());
    statement.bind_blob(4, journal_record.serialize_canonical());
    if (migration_plan)
        statement.bind_blob(5, migration_plan->serialize_canonical());
    else
        statement.bind_null(5);
    statement.bind_blob(6, last_publication_bytes);
    statement.step_done();
}

void update_head(sqlite3* db, std::string_view owner_scope, const ProgramRunRecord& run_record,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    Statement statement(
        db, "UPDATE program_transition_run_heads_v2 SET run_record_bytes = ?1, "
            "journal_record_bytes = ?2, migration_plan_bytes = ?3, last_publication_bytes = ?4 "
            "WHERE owner_scope = ?5 AND run_id = ?6");
    statement.bind_blob(1, run_record.serialize_canonical());
    statement.bind_blob(2, journal_record.serialize_canonical());
    if (migration_plan)
        statement.bind_blob(3, migration_plan->serialize_canonical());
    else
        statement.bind_null(3);
    statement.bind_blob(4, last_publication_bytes);
    statement.bind_text(5, owner_scope);
    statement.bind_text(6, run_record.run_id());
    statement.step_done();
}

void append_events(sqlite3* db, std::string_view owner_scope, std::string_view run_id,
                   const std::vector<ProgramEvent>& events) {
    if (events.empty()) return;
    Statement statement(
        db, "INSERT INTO program_transition_event_log_v2"
            "(owner_scope, run_id, sequence, canonical_bytes) VALUES(?1, ?2, ?3, ?4)");
    for (const auto& event : events) {
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_uint64(3, event.sequence);
        statement.bind_blob(4, event.serialize_canonical());
        statement.step_done();
        statement.reset();
    }
}

void append_effects(sqlite3* db, std::string_view owner_scope, std::string_view run_id,
                    const std::vector<ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return;
    Statement statement(
        db, "INSERT INTO program_transition_effect_log_v2"
            "(owner_scope, run_id, sequence, effect_id, canonical_bytes) VALUES(?1, ?2, ?3, ?4, ?5)");
    for (const auto& effect : effects) {
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_uint64(3, effect.sequence());
        statement.bind_text(4, effect.effect().effect_id());
        statement.bind_blob(5, effect.serialize_canonical());
        statement.step_done();
        statement.reset();
    }
}

void append_javascript_commands(
    sqlite3* db, std::string_view owner_scope, std::string_view run_id,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& commands) {
    if (commands.empty()) return;
    Statement statement(
        db, "INSERT INTO program_transition_javascript_command_log_v2"
            "(owner_scope, run_id, sequence, coordinate_id, canonical_bytes) "
            "VALUES(?1, ?2, ?3, ?4, ?5)");
    for (const auto& command : commands) {
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_uint64(3, command.sequence());
        statement.bind_text(4, command.coordinate_id());
        statement.bind_blob(5, command.serialize_canonical());
        statement.step_done();
        statement.reset();
    }
}

void append_context_publication(sqlite3* db, std::string_view owner_scope,
                                std::string_view run_id,
                                const std::optional<ProgramContextPublication>& context) {
    if (!context) return;
    Statement statement(
        db, "INSERT INTO program_transition_context_log_v1"
            "(owner_scope, run_id, sequence, canonical_bytes) VALUES(?1, ?2, ?3, ?4)");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_uint64(3, context->epoch.sequence());
    statement.bind_blob(4, context_publication_bytes(*context));
    statement.step_done();
}

void append_synthesis_records(sqlite3*                                        db,
                              std::string_view                                owner_scope,
                              std::string_view                                run_id,
                              const std::vector<ProgramChildSynthesisRecord>& entries) {
    Statement statement(db,
                        "INSERT INTO "
                        "program_transition_synthesis_log_v1(owner_scope,run_id,proposal_id,head_"
                        "id,canonical_bytes) VALUES(?1,?2,?3,?4,?5)");
    for (const auto& entry : entries) {
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_text(3, entry.data().proposal.id());
        statement.bind_text(4, entry.id());
        statement.bind_blob(5, entry.serialize_canonical());
        statement.step_done();
        statement.reset();
    }
}

void append_hook_outbox_entries(sqlite3* db, std::string_view owner_scope,
                                std::string_view run_id,
                                const std::vector<HookOutboxEntry>& entries) {
    Statement statement(db, "INSERT INTO program_transition_hook_outbox_log_v1"
                          "(owner_scope, run_id, invocation_id, head_id, canonical_bytes) "
                          "VALUES(?1, ?2, ?3, ?4, ?5)");
    for (const auto& entry : entries) {
        statement.bind_text(1, owner_scope); statement.bind_text(2, run_id);
        statement.bind_text(3, entry.data().invocation.id()); statement.bind_text(4, entry.id());
        statement.bind_blob(5, entry.serialize_canonical()); statement.step_done(); statement.reset();
    }
}

std::string normalize_legacy_snapshot(std::string_view                    owner_scope,
                                      std::string_view                    run_id,
                                      const ProgramTransitionPublication& publication,
                                      std::string_view                    last_publication_bytes) {
    const auto& run = publication.run_record;
    if (run.owner_scope() != owner_scope || run.run_id() != run_id ||
        run.journal_head() != publication.journal_record.id ||
        run.event_sequence() != publication.events.size() ||
        run.effect_sequence() != publication.effects.size())
        throw std::invalid_argument("Legacy Program transition snapshot is inconsistent");

    for (std::size_t index = 0; index < publication.events.size(); ++index)
        if (publication.events[index].sequence != index + 1)
            throw std::invalid_argument("Legacy Program transition events are not contiguous");

    std::set<std::string, std::less<>> effect_ids;
    for (std::size_t index = 0; index < publication.effects.size(); ++index) {
        const auto& effect = publication.effects[index];
        if (effect.sequence() != index + 1 ||
            !effect_ids.emplace(effect.effect().effect_id()).second)
            throw std::invalid_argument("Legacy Program transition effects are inconsistent");
    }

    if (!detail::transition::valid_command_history_append(run, {}, publication.commands))
        throw std::invalid_argument("Legacy Program transition commands are inconsistent");

    const auto last_publication = ProgramTransitionPublication::parse(last_publication_bytes);
    if (last_publication.run_record.owner_scope() != owner_scope ||
        last_publication.run_record.run_id() != run_id ||
        last_publication.journal_record.id != publication.journal_record.id)
        throw std::invalid_argument("Legacy Program transition retry record is inconsistent");

    // Older publication envelopes predate the durable command journal and
    // therefore omit "commands". Parsing treats that legacy shape as an empty
    // journal; reserialization upgrades it to the one canonical byte form used
    // by exact-retry comparisons after migration.
    return last_publication.serialize_canonical();
}

void migrate_legacy_schema(sqlite3* db) {
    if (!table_exists(db, "program_transition_runs")) return;

    {
        Statement statement(
            db, "SELECT owner_scope, run_id, canonical_bytes, last_publication_bytes "
                "FROM program_transition_runs");
        while (statement.step_row()) {
            const auto owner_scope            = column_text(statement.get(), 0);
            const auto run_id                 = column_text(statement.get(), 1);
            const auto canonical_bytes        = column_blob(statement.get(), 2);
            const auto last_publication_bytes = column_blob(statement.get(), 3);
            const auto publication = ProgramTransitionPublication::parse(canonical_bytes);
            const auto normalized_last_publication =
                normalize_legacy_snapshot(owner_scope, run_id, publication, last_publication_bytes);
            insert_head(db, owner_scope, publication.run_record, publication.journal_record,
                        publication.migration_plan, normalized_last_publication);
            append_events(db, owner_scope, run_id, publication.events);
            append_effects(db, owner_scope, run_id, publication.effects);
            append_javascript_commands(db, owner_scope, run_id, publication.commands);
            append_context_publication(db, owner_scope, run_id, publication.context_publication);
            append_hook_outbox_entries(db, owner_scope, run_id, publication.hook_outbox_entries);
        }
    }
    exec(db, "DROP TABLE program_transition_runs");
}

void backfill_generation_initial_publications(sqlite3* db) {
    struct MissingPublication {
        std::string          owner_scope;
        std::string          lineage_id;
        std::uint64_t        generation = 0;
        ProgramRunGeneration record;
    };
    std::vector<MissingPublication> missing;
    {
        Statement statement(
            db, "SELECT g.owner_scope, g.lineage_id, g.generation, g.canonical_bytes FROM "
                "program_run_generations_v1 g LEFT JOIN "
                "program_run_generation_publications_v1 p ON "
                "p.owner_scope = g.owner_scope AND p.lineage_id = g.lineage_id "
                "AND p.generation = g.generation WHERE p.generation IS NULL");
        while (statement.step_row()) {
            const auto stored_ordinal = sqlite3_column_int64(statement.get(), 2);
            if (stored_ordinal <= 0) {
                throw std::invalid_argument("Stored Program generation ordinal is corrupt");
            }
            const auto ordinal = static_cast<std::uint64_t>(stored_ordinal);
            auto       generation =
                ProgramRunGeneration::parse(column_blob(statement.get(), 3));
            if (generation.owner_scope() != column_text(statement.get(), 0) ||
                generation.lineage_id() != column_text(statement.get(), 1) ||
                generation.generation() != ordinal) {
                throw std::invalid_argument("Stored Program generation binding is corrupt");
            }
            missing.push_back(MissingPublication{
                column_text(statement.get(), 0), column_text(statement.get(), 1), ordinal,
                std::move(generation)});
        }
    }
    for (const auto& item : missing) {
        const auto head = load_head(db, item.owner_scope, item.record.run_id());
        if (!head || head->run_record.id() != item.record.initial_run_record_id() ||
            head->journal_record.id != item.record.initial_journal_head()) {
            continue;
        }
        const auto publication = ProgramTransitionPublication::parse(
            head->last_publication_bytes);
        if (!publication.run_generation || !publication.run_lineage ||
            publication.run_generation->id() != item.record.id() ||
            publication.run_record.id() != item.record.initial_run_record_id() ||
            publication.journal_record.id != item.record.initial_journal_head()) {
            continue;
        }
        insert_generation_initial_publication(
            db, item.record, publication.serialize_canonical());
    }
}

void initialize_schema(sqlite3* db) {
    Transaction transaction(db);
    create_v2_schema(db);
    migrate_legacy_schema(db);
    backfill_generation_initial_publications(db);
    transaction.commit();
}

}  // namespace

struct SQLiteProgramTransitionStore::Impl {
    explicit Impl(std::string value) : path(std::move(value)) {}
    ~Impl() {
        if (db) sqlite3_close_v2(db);
    }

    std::string       path;
    std::string       coordination_key;
    sqlite3*          db = nullptr;
    mutable std::mutex mutex;
};

SQLiteProgramTransitionStore::SQLiteProgramTransitionStore(std::string database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {
    if (impl_->path.empty())
        throw std::invalid_argument("SQLite ProgramTransitionStore path must not be empty");

    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(impl_->path.c_str(), &impl_->db, flags, nullptr) != SQLITE_OK)
        throw_sqlite(impl_->db, "SQLite open");

    try {
        if (const auto* filename = sqlite3_db_filename(impl_->db, "main");
            filename && *filename) {
            std::error_code error;
            auto canonical = std::filesystem::weakly_canonical(filename, error);
            if (error) canonical = std::filesystem::absolute(filename, error).lexically_normal();
            if (!error) {
                impl_->coordination_key =
                    "neograph-program-sqlite:" + canonical.generic_string();
            }
        }
        // https://www.sqlite.org/c3ref/busy_timeout.html (fetched 2026-08-06)
        // Serialize concurrent SQLite writers instead of surfacing transient SQLITE_BUSY.
        if (sqlite3_busy_timeout(impl_->db, 5000) != SQLITE_OK)
            throw_sqlite(impl_->db, "SQLite configure busy timeout");
        exec(impl_->db, "PRAGMA foreign_keys = ON");
        initialize_schema(impl_->db);
    } catch (...) {
        sqlite3_close_v2(impl_->db);
        impl_->db = nullptr;
        throw;
    }
}

SQLiteProgramTransitionStore::SQLiteProgramTransitionStore(SQLiteProgramTransitionStore&&) noexcept =
    default;
SQLiteProgramTransitionStore&
SQLiteProgramTransitionStore::operator=(SQLiteProgramTransitionStore&&) noexcept = default;
SQLiteProgramTransitionStore::~SQLiteProgramTransitionStore() = default;

std::string SQLiteProgramTransitionStore::process_coordination_key() const {
    return impl_->coordination_key;
}

std::optional<ProgramRunPublicationHead>
SQLiteProgramTransitionStore::load_run_publication_head_sync(std::string_view owner,
                                                       std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(impl_->db,
        "SELECT run_record_bytes, journal_record_bytes FROM program_transition_run_heads_v2 "
        "WHERE owner_scope = ?1 AND run_id = ?2");
    statement.bind_text(1, owner);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;
    ProgramRunPublicationHead head{
        ProgramRunRecord::parse(column_blob(statement.get(), 0)),
        ProgramJournalRecord::parse(column_blob(statement.get(), 1))};
    detail::validate_run_publication_head(head.run_record, head.journal_record, owner, run_id);
    return head;
}

std::optional<ProgramCommandPublicationHead>
SQLiteProgramTransitionStore::load_command_publication_head_sync(std::string_view owner,
                                                           std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    // One statement pins a read snapshot even when another connection publishes.
    // The existing (owner, run, sequence) primary key serves both bounded lookups.
    Statement statement(impl_->db,
        "SELECT h.run_record_bytes, h.journal_record_bytes, c.sequence, c.coordinate_id, "
        "c.canonical_bytes FROM program_transition_run_heads_v2 h "
        "LEFT JOIN program_transition_javascript_command_log_v2 c "
        "ON c.owner_scope = h.owner_scope AND c.run_id = h.run_id AND c.sequence = "
        "(SELECT MAX(t.sequence) FROM program_transition_javascript_command_log_v2 t "
        "WHERE t.owner_scope = h.owner_scope AND t.run_id = h.run_id) "
        "WHERE h.owner_scope = ?1 AND h.run_id = ?2");
    statement.bind_text(1, owner);
    statement.bind_text(2, run_id);
    if (!statement.step_row()) return std::nullopt;
    ProgramCommandPublicationHead head{
        ProgramRunRecord::parse(column_blob(statement.get(), 0)),
        ProgramJournalRecord::parse(column_blob(statement.get(), 1)), std::nullopt};
    if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) {
        head.latest_command = ProgramJavaScriptCommandJournalEntry::parse(column_blob(statement.get(), 4));
        const auto sequence = sqlite3_column_int64(statement.get(), 2);
        if (sequence <= 0 || head.latest_command->sequence() != static_cast<std::uint64_t>(sequence) ||
            head.latest_command->coordinate_id() != column_text(statement.get(), 3)) {
            throw std::invalid_argument("Stored JavaScript command publication head is corrupt");
        }
    }
    detail::validate_command_publication_head(head, owner, run_id);
    return head;
}

std::optional<ProgramRunRecord> SQLiteProgramTransitionStore::load_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->run_record;
}

std::optional<ProgramJournalRecord> SQLiteProgramTransitionStore::latest_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->journal_record;
}

std::vector<ProgramEvent> SQLiteProgramTransitionStore::load_events_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(
        impl_->db, "SELECT canonical_bytes FROM program_transition_event_log_v2 "
                   "WHERE owner_scope = ?1 AND run_id = ?2 AND sequence > ?3 "
                   "ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_uint64(3, after_sequence);

    std::vector<ProgramEvent> result;
    while (statement.step_row()) result.push_back(ProgramEvent::parse(column_blob(statement.get(), 0)));
    return result;
}

std::vector<ProgramEffectOutboxEntry> SQLiteProgramTransitionStore::load_effects_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(
        impl_->db, "SELECT canonical_bytes FROM program_transition_effect_log_v2 "
                   "WHERE owner_scope = ?1 AND run_id = ?2 AND sequence > ?3 "
                   "ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_uint64(3, after_sequence);

    std::vector<ProgramEffectOutboxEntry> result;
    while (statement.step_row())
        result.push_back(ProgramEffectOutboxEntry::parse(column_blob(statement.get(), 0)));
    return result;
}

std::vector<ProgramJavaScriptCommandJournalEntry>
SQLiteProgramTransitionStore::load_javascript_commands_sync(std::string_view owner_scope,
                                                       std::string_view run_id,
                                                       std::uint64_t    after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    const auto all = load_all_javascript_commands(impl_->db, owner_scope, run_id);
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    for (const auto& entry : all) {
        if (entry.sequence() > after_sequence) result.push_back(entry);
    }
    return result;
}

std::vector<ProgramContextPublication> SQLiteProgramTransitionStore::load_context_publications_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto all = load_context_publication_history(impl_->db, owner_scope, run_id);
    if (!valid_context_history(all)) {
        throw std::invalid_argument("Stored Program context history is not contiguous");
    }
    std::vector<ProgramContextPublication> result;
    for (const auto& context : all) {
        if (context.epoch.sequence() > after_sequence) result.push_back(context);
    }
    return result;
}
std::vector<ProgramChildSynthesisRecord> SQLiteProgramTransitionStore::load_child_syntheses_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    if (!load_head(impl_->db, owner_scope, run_id)) return {};
    return load_synthesis_heads(impl_->db, owner_scope, run_id);
}

std::vector<HookOutboxEntry> SQLiteProgramTransitionStore::load_hook_outbox_entries_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return {};
    auto entries = load_hook_outbox_heads(impl_->db, owner_scope, run_id, head->run_record);
    if (!is_valid_program_hook_history_append({}, entries, head->run_record))
        throw std::invalid_argument("Stored Program hook outbox history is corrupt");
    return entries;
}

std::optional<MigrationPlan> SQLiteProgramTransitionStore::load_migration_plan_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->migration_plan;
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_lineage_sync(
    std::string_view owner_scope, std::string_view lineage_id) const {
    if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_current_lineage_head(impl_->db, owner_scope, lineage_id);
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_run_lineage_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto lineage_id = load_run_lineage_id(impl_->db, owner_scope, run_id);
    if (!lineage_id) return std::nullopt;
    const auto lineage = load_current_lineage_head(impl_->db, owner_scope, *lineage_id);
    if (!lineage)
        throw std::invalid_argument("Stored Program run lineage association is corrupt");
    return lineage;
}

std::optional<ProgramRunGeneration> SQLiteProgramTransitionStore::load_generation_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_record(impl_->db, owner_scope, lineage_id, generation);
}

std::optional<ProgramTransitionPublication>
SQLiteProgramTransitionStore::load_generation_initial_publication_sync(
    std::string_view owner_scope, std::string_view lineage_id,
    std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_initial_publication_record(
        impl_->db, owner_scope, lineage_id, generation);
}

std::optional<GraphMigrationCapsule>
SQLiteProgramTransitionStore::load_graph_migration_capsule_sync(
    std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) const {
    if (owner_scope.empty() || source_run_id.empty() || source_lineage_head_id.empty())
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_graph_migration_capsule_record(
        impl_->db, owner_scope, source_run_id, source_lineage_head_id);
}
std::optional<ProgramExecutionLease>
SQLiteProgramTransitionStore::load_execution_lease_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_execution_lease_record(impl_->db, owner_scope, run_id);
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_lineage_head_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const {
    if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_historical_lineage_head(impl_->db, owner_scope, lineage_id, head_id);
}

ProgramTransitionPublishResult SQLiteProgramTransitionStore::compare_publish_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication) {
    return compare_publish_impl(owner_scope, expected_journal_head,
                                std::move(publication), nullptr, nullptr, nullptr, nullptr);
}

ProgramTransitionPublishResult
SQLiteProgramTransitionStore::compare_publish_execution_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    std::optional<ProgramExecutionLease> expected_lease,
    std::optional<ProgramExecutionLease> next_lease) {
    return compare_publish_impl(
        owner_scope, expected_journal_head, std::move(publication), nullptr, nullptr,
        expected_lease ? &*expected_lease : nullptr,
        next_lease ? &*next_lease : nullptr);
}

ProgramTransitionPublishResult
SQLiteProgramTransitionStore::compare_publish_graph_safe_point_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence& evidence,
    const GraphMigrationCapsule& capsule,
    const ProgramExecutionLease& execution_lease) {
    return compare_publish_impl(owner_scope, expected_journal_head,
                                std::move(publication), &evidence, &capsule,
                                &execution_lease, nullptr);
}

ProgramTransitionPublishResult SQLiteProgramTransitionStore::compare_publish_impl(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence* safe_point_evidence,
    const GraphMigrationCapsule* safe_point_capsule,
    const ProgramExecutionLease* expected_lease,
    const ProgramExecutionLease* next_lease) {
    const auto publication_bytes = detail::transition::prepare_transition(
        owner_scope, expected_journal_head, publication, safe_point_evidence, safe_point_capsule,
        expected_lease, next_lease);
    if (!publication_bytes) return ProgramTransitionPublishResult::Conflict;

    const std::string run_id = publication.run_record.run_id();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    const auto current = load_head(impl_->db, owner_scope, run_id);
    const auto current_lease = load_execution_lease_record(impl_->db, owner_scope, run_id);

    std::optional<detail::transition::CurrentHead> head;
    if (current)
        head.emplace(detail::transition::CurrentHead{current->run_record, current->journal_record,
                                                     current->migration_plan,
                                                     current->last_publication_bytes});
    SqliteReads                        reads(impl_->db, owner_scope);
    detail::transition::TransitionPlan plan;
    switch (detail::transition::validate_transition(
        {owner_scope, expected_journal_head, publication, *publication_bytes, safe_point_evidence,
         safe_point_capsule, expected_lease, next_lease},
        head ? &*head : nullptr, current_lease ? &*current_lease : nullptr, reads, plan)) {
    case detail::transition::TransitionVerdict::Conflict:
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    case detail::transition::TransitionVerdict::AlreadyPresent:
        transaction.commit();
        return ProgramTransitionPublishResult::AlreadyPresent;
    case detail::transition::TransitionVerdict::Proceed:
        break;
    }

    const auto& migration_plan = current ? current->migration_plan : publication.migration_plan;
    if (current)
        update_head(impl_->db, owner_scope, publication.run_record, publication.journal_record,
                    migration_plan, *publication_bytes);
    else
        insert_head(impl_->db, owner_scope, publication.run_record, publication.journal_record,
                    migration_plan, *publication_bytes);
    append_events(impl_->db, owner_scope, run_id, publication.events);
    append_effects(impl_->db, owner_scope, run_id, publication.effects);
    append_javascript_commands(impl_->db, owner_scope, run_id, publication.commands);
    append_context_publication(impl_->db, owner_scope, run_id, publication.context_publication);
    append_synthesis_records(impl_->db, owner_scope, run_id, publication.child_synthesis_records);
    append_hook_outbox_entries(impl_->db, owner_scope, run_id, publication.hook_outbox_entries);
    if (publication.run_lineage) {
        if (plan.lineage_head_exists)
            update_lineage_head(impl_->db, *publication.run_lineage);
        else
            insert_lineage_head(impl_->db, *publication.run_lineage);
        insert_lineage_history(impl_->db, *publication.run_lineage);
        if (publication.run_generation) {
            insert_generation(impl_->db, *publication.run_generation);
            insert_generation_initial_publication(
                impl_->db, *publication.run_generation, *publication_bytes);
        }
        if (!plan.run_lineage_recorded)
            insert_run_lineage(impl_->db, owner_scope, run_id,
                               publication.run_lineage->lineage_id());
    }
    if (publication.fork_source_lineage) {
        update_lineage_head(impl_->db, *publication.fork_source_lineage);
        insert_lineage_history(impl_->db, *publication.fork_source_lineage);
    }
    if (publication.recorded_replay_source_lineage) {
        update_lineage_head(impl_->db, *publication.recorded_replay_source_lineage);
        insert_lineage_history(impl_->db, *publication.recorded_replay_source_lineage);
    }
    if (safe_point_capsule) insert_graph_migration_capsule(impl_->db, *safe_point_capsule);
    if (next_lease)
        insert_execution_lease(impl_->db, *next_lease);
    else if (expected_lease)
        delete_execution_lease(impl_->db, *expected_lease);
    transaction.commit();
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
