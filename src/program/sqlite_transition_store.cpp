#include <neograph/program/sqlite_transition_store.h>

#include "canonical_json.h"
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <functional>
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
    return ProgramRunLineage::parse(column_blob(statement.get(), 0));
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
    return ProgramRunLineage::parse(column_blob(statement.get(), 0));
}

std::optional<ProgramRunGeneration> load_generation_record(sqlite3* db,
                                                            std::string_view owner_scope,
                                                            std::string_view lineage_id,
                                                            std::uint64_t generation) {
    Statement statement(
        db, "SELECT canonical_bytes FROM program_run_generations_v1 "
            "WHERE owner_scope = ?1 AND lineage_id = ?2 AND generation = ?3");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, lineage_id);
    statement.bind_uint64(3, generation);
    if (!statement.step_row()) return std::nullopt;
    return ProgramRunGeneration::parse(column_blob(statement.get(), 0));
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

bool is_final(ContinuationState state) noexcept {
    return state != ContinuationState::Running && state != ContinuationState::Interrupted &&
           state != ContinuationState::AmbiguousEffect;
}

bool same_fork(const ProgramRunRecord& old_run, const ProgramRunRecord& new_run) {
    const auto old_receipt = old_run.fork_receipt();
    const auto new_receipt = new_run.fork_receipt();
    return old_receipt.has_value() == new_receipt.has_value() &&
           (!old_receipt || old_receipt->id() == new_receipt->id());
}

bool same_child_metadata(const ProgramChildRecord& lhs, const ProgramChildRecord& rhs) {
    return lhs.child_run_id == rhs.child_run_id && lhs.link_id == rhs.link_id &&
           lhs.link_receipt == rhs.link_receipt && lhs.invocation == rhs.invocation;
}

bool valid_child_state_transition(ProgramChildState previous, ProgramChildState next) noexcept {
    if (previous == next) return true;
    if (previous == ProgramChildState::Publishing &&
        (next == ProgramChildState::Dispatched || next == ProgramChildState::Completed ||
         next == ProgramChildState::Cancelled || next == ProgramChildState::Failed))
        return true;
    if (previous == ProgramChildState::Dispatched &&
        (next == ProgramChildState::Completed || next == ProgramChildState::Cancelled ||
         next == ProgramChildState::Failed))
        return true;
    return false;
}

bool valid_children_transition(const ProgramRunRecord& previous, const ProgramRunRecord& next) {
    const auto previous_children = previous.children();
    const auto next_children     = next.children();
    for (const auto& child : previous_children) {
        const auto found = std::find_if(next_children.begin(), next_children.end(),
                                        [&](const auto& value) {
                                            return value.child_run_id == child.child_run_id;
                                        });
        if (found == next_children.end() || !same_child_metadata(child, *found) ||
            !valid_child_state_transition(child.state, found->state))
            return false;
    }
    return true;
}

bool effect_ids_are_new(sqlite3* db, std::string_view owner_scope, std::string_view run_id,
                        const std::vector<ProgramEffectOutboxEntry>& new_effects) {
    if (new_effects.empty()) return true;
    std::set<std::string, std::less<>> ids;
    Statement statement(
        db, "SELECT 1 FROM program_transition_effect_log_v2 "
            "WHERE owner_scope = ?1 AND run_id = ?2 AND effect_id = ?3 LIMIT 1");
    for (const auto& effect : new_effects) {
        const auto& effect_id = effect.effect().effect_id();
        if (!ids.emplace(effect_id).second) return false;
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, run_id);
        statement.bind_text(3, effect_id);
        const bool already_present = statement.step_row();
        statement.reset();
        if (already_present) return false;
    }
    return true;
}

bool valid_effect_outbox_binding(const ProgramRunRecord& run,
                                 const std::vector<ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;
    const auto pending = run.pending_effect();
    return pending && pending->state() == ProgramPendingState::Awaiting && effects.size() == 1 &&
           effects.front().effect() == *pending;
}

bool same_command_coordinate(const ProgramJavaScriptCommandJournalEntry& lhs,
                             const ProgramJavaScriptCommandJournalEntry& rhs) {
    return lhs.bundle_id() == rhs.bundle_id() &&
           lhs.command_ordinal() == rhs.command_ordinal() &&
           lhs.coordinate_id() == rhs.coordinate_id() &&
           detail::canonical_json_bytes(lhs.command().to_json()) ==
               detail::canonical_json_bytes(rhs.command().to_json()) &&
           lhs.effect_identity() == rhs.effect_identity();
}

bool valid_command_history_append(
    const ProgramRunRecord&                                  run,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& new_commands) {
    if (new_commands.empty()) return true;
    auto prior = old_commands;
    std::uint64_t expected_sequence = 0;
    std::uint64_t highest_ordinal = 0;
    for (const auto& existing : prior) {
        if (existing.bundle_id() != run.bundle_id() ||
            existing.sequence() != expected_sequence + 1)
            return false;
        ++expected_sequence;
        highest_ordinal = std::max(highest_ordinal, existing.command_ordinal());
    }
    for (const auto& entry : new_commands) {
        if (entry.bundle_id() != run.bundle_id() ||
            entry.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;
        const auto found = std::find_if(
            prior.rbegin(), prior.rend(), [&](const auto& previous) {
                return previous.command_ordinal() == entry.command_ordinal();
            });
        if (found == prior.rend()) {
            if (entry.command_ordinal() != highest_ordinal + 1 || !entry.pending() ||
                (!prior.empty() && prior.back().pending()))
                return false;
            highest_ordinal = entry.command_ordinal();
            prior.push_back(entry);
            continue;
        }
        if (!found->pending() || !entry.completed() ||
            !same_command_coordinate(*found, entry)) {
            return false;
        }
        prior.push_back(entry);
    }
    return true;
}
bool budget_is_empty(const RunBudget& budget) noexcept {
    return budget == RunBudget{};
}

bool budget_increased(const RunBudget& next, const RunBudget& previous) noexcept {
    return next.wall_time_ms > previous.wall_time_ms || next.model_tokens > previous.model_tokens ||
           next.monetary_microunits > previous.monetary_microunits ||
           next.max_concurrency > previous.max_concurrency ||
           next.max_program_operations > previous.max_program_operations ||
           next.max_core_steps > previous.max_core_steps ||
           next.max_dynamic_compiles > previous.max_dynamic_compiles ||
           next.max_child_depth > previous.max_child_depth ||
           next.max_total_children > previous.max_total_children;
}

std::optional<ProgramUsage> command_terminal_usage(
    const ProgramJavaScriptCommandJournalEntry& entry) {
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
        *peak > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    // Program operations are charged before the resource reservation is staged.
    return ProgramUsage{*wall, *model, *money, 0, *steps, static_cast<std::uint32_t>(*peak)};
}

bool checkpoint_event_matches(const std::vector<ProgramEvent>&             events,
                              const std::optional<CoreCheckpointIdentity>& checkpoint) {
    if (!checkpoint) return false;
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint == *checkpoint;
    });
}

bool javascript_call_core_checkpoint_matches(const ProgramRunRecord&                     run,
                                             const ProgramJavaScriptCommandJournalEntry& pending,
                                             const CoreCheckpointIdentity& checkpoint) {
    const auto thread_for = [&](std::string_view operation_id) {
        std::string identity(run.run_id());
        identity.push_back('\0');
        identity.append(operation_id);
        identity.push_back('\0');
        identity.append(checkpoint.core_generation_id);
        return detail::sha256_identity("program-core-thread/v1", identity);
    };
    std::function<bool(const JavaScriptCommand&, std::string, std::size_t)> matches;
    matches = [&](const JavaScriptCommand& command, std::string operation_id, std::size_t depth) {
        if (depth > 32) return false;
        if (command.kind() == JavaScriptCommandKind::CallCore) {
            const auto arguments = command.arguments();
            return arguments.contains("name") && arguments.at("name").is_string() &&
                   arguments.at("name").get<std::string>() == checkpoint.core_name &&
                   thread_for(operation_id) == checkpoint.core_thread_id;
        }
        const auto arguments = command.arguments();
        if (command.kind() == JavaScriptCommandKind::Await) {
            return matches(JavaScriptCommand::from_json(arguments.at("command")),
                           operation_id + "/await", depth + 1);
        }
        if (command.kind() == JavaScriptCommandKind::Join) {
            const auto& members = arguments.at("members");
            for (std::size_t index = 0; index < members.size(); ++index)
                if (matches(JavaScriptCommand::from_json(members.at(index)),
                            operation_id + "/member/" + std::to_string(index), depth + 1))
                    return true;
        }
        return false;
    };
    return matches(pending.command(),
                   "root.javascript." + std::to_string(pending.command_ordinal()), 0);
}

bool valid_command_reservation_transition(
    const ProgramJournalRecord&                              previous_journal,
    const ProgramJournalRecord&                              next_journal,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const ProgramTransitionPublication&                      publication) {
    const bool increased =
        budget_increased(next_journal.remaining_budget, previous_journal.remaining_budget);
    const bool ordinary  = is_valid_program_journal_transition(previous_journal, next_journal);
    const auto completed = std::find_if(publication.commands.begin(), publication.commands.end(),
                                        [](const auto& entry) { return entry.completed(); });
    const bool checkpoint_changed =
        previous_journal.core_checkpoint != next_journal.core_checkpoint;
    if (completed != publication.commands.end()) {
        if (publication.commands.size() != 1 || !budget_is_empty(next_journal.inflight_reservation))
            return false;
        if (checkpoint_changed &&
            (!checkpoint_event_matches(publication.events, next_journal.core_checkpoint) ||
             !next_journal.core_checkpoint ||
             !javascript_call_core_checkpoint_matches(publication.run_record, *completed,
                                                      *next_journal.core_checkpoint))) {
            return false;
        }
        if (budget_is_empty(previous_journal.inflight_reservation)) return ordinary && !increased;
        const auto usage = command_terminal_usage(*completed);
        return usage && is_valid_program_journal_reservation_settlement(previous_journal,
                                                                        next_journal, *usage);
    }
    if (!increased) return ordinary;
    if (!publication.commands.empty() || old_commands.empty() || !old_commands.back().pending() ||
        publication.run_record.continuation().state != ContinuationState::Interrupted ||
        !publication.run_record.terminal_result() ||
        publication.run_record.terminal_result()->status() != ProgramTerminalStatus::Interrupted ||
        !checkpoint_event_matches(publication.events, publication.run_record.exact_checkpoint()) ||
        !publication.run_record.exact_checkpoint() ||
        !javascript_call_core_checkpoint_matches(publication.run_record, old_commands.back(),
                                                 *publication.run_record.exact_checkpoint()) ||
        (previous_journal.core_checkpoint &&
         previous_journal.core_checkpoint == next_journal.core_checkpoint))
        return false;
    auto usage               = publication.run_record.terminal_result()->usage();
    usage.program_operations = 0;
    return is_valid_program_journal_reservation_settlement(previous_journal, next_journal, usage);
}

bool valid_initial_publication(const ProgramTransitionPublication& publication) {
    const auto& journal = publication.journal_record;
    const auto  terminal = publication.run_record.terminal_result();
    const bool  terminal_event = !publication.events.empty() &&
                                 publication.events.back().kind == ProgramEventKind::Terminal;
    return journal.previous_id.empty() && journal.sequence == 1 &&
           budget_is_empty(journal.inflight_reservation) && !publication.events.empty() &&
           publication.events.front().kind == ProgramEventKind::Started &&
           publication.run_record.event_sequence() == publication.events.size() &&
           publication.run_record.effect_sequence() == publication.effects.size() &&
           publication.events.front().sequence == 1 &&
           (publication.effects.empty() || publication.effects.front().sequence() == 1) &&
           valid_command_history_append(publication.run_record, {}, publication.commands) &&
           (!terminal || terminal_event) &&
           (!publication.run_record.fork_receipt() || publication.migration_plan.has_value());
}

bool valid_increment(sqlite3* db, std::string_view owner_scope,
                     const StoredHead& old_head,
                     const ProgramTransitionPublication& next_publication,
                     std::string_view expected_journal_head) {
    const auto& old_run      = old_head.run_record;
    const auto& next_run     = next_publication.run_record;
    const auto& old_journal  = old_head.journal_record;
    const auto& next_journal = next_publication.journal_record;
    const auto  old_terminal = old_run.terminal_result();
    const auto  new_terminal = next_run.terminal_result();
    const bool  terminal_event = !next_publication.events.empty() &&
                                 next_publication.events.back().kind == ProgramEventKind::Terminal;

    if ((old_terminal && is_final(old_run.continuation().state) && !new_terminal) ||
        (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
         !terminal_event))
        return false;
    if (old_journal.id != expected_journal_head || next_journal.previous_id != expected_journal_head ||
        next_run.created_at_ms() != old_run.created_at_ms() ||
        next_run.updated_at_ms() < old_run.updated_at_ms() ||
        next_run.binding_fingerprint() != old_run.binding_fingerprint() ||
        !same_fork(old_run, next_run) ||
        next_run.recorded_binding_set_fingerprint() != old_run.recorded_binding_set_fingerprint() ||
        next_run.invocation() != old_run.invocation() || !valid_children_transition(old_run, next_run) ||
        next_run.event_sequence() != old_run.event_sequence() + next_publication.events.size() ||
        next_run.effect_sequence() != old_run.effect_sequence() + next_publication.effects.size())
        return false;
    if (!next_publication.events.empty() &&
        next_publication.events.front().sequence != old_run.event_sequence() + 1)
        return false;
    if (!next_publication.effects.empty() &&
        next_publication.effects.front().sequence() != old_run.effect_sequence() + 1)
        return false;
    if (!effect_ids_are_new(db, owner_scope, old_run.run_id(), next_publication.effects))
        return false;
    std::vector<ProgramJavaScriptCommandJournalEntry> old_commands;
    {
        Statement statement(
            db, "SELECT coordinate_id, canonical_bytes FROM "
                "program_transition_javascript_command_log_v2 "
                "WHERE owner_scope = ?1 AND run_id = ?2 ORDER BY sequence ASC");
        statement.bind_text(1, owner_scope);
        statement.bind_text(2, old_run.run_id());
        while (statement.step_row()) {
            auto entry = ProgramJavaScriptCommandJournalEntry::parse(
                column_blob(statement.get(), 1));
            if (column_text(statement.get(), 0) != entry.coordinate_id()) return false;
            old_commands.push_back(std::move(entry));
        }
    }
    if (!valid_command_history_append(old_run, old_commands, next_publication.commands) ||
        !valid_command_reservation_transition(old_journal, next_journal, old_commands,
                                              next_publication))
        return false;
    if (next_publication.migration_plan &&
        (!old_head.migration_plan ||
         next_publication.migration_plan->id() != old_head.migration_plan->id()))
        return false;
    return true;
}

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
}

bool fork_binds_predecessor(const ProgramRunRecord&     target,
                            const ProgramRunGeneration& predecessor,
                            const ProgramRunLineage&    lineage,
                            const ProgramRunRecord&     source) noexcept {
    const auto receipt = target.fork_receipt();
    if (!receipt) return true;
    const auto checkpoint = source.exact_checkpoint();
    return checkpoint && target.run_id() != source.run_id() && source.child_depth() == 0 &&
           source.invocation().parent_run_id.empty() &&
           source.continuation().state == ContinuationState::Interrupted &&
           lineage.committed_descendant_budget() == RunBudget{} && receipt->compatible() &&
           receipt->owner_scope() == target.owner_scope() &&
           receipt->source_run_id() == predecessor.run_id() &&
           receipt->source_program_version_id() == predecessor.program_version_id() &&
           receipt->source_checkpoint_id() == checkpoint->checkpoint_id &&
           receipt->target_program_version_id() == target.program_version_id() &&
           source.id() == lineage.active_run_record_id() &&
           source.journal_head() == lineage.active_journal_head();
}

bool fork_allocation_fits(const ProgramRunLineage& previous,
                          const ProgramRunLineage& debited,
                          const RunBudget&         target) noexcept {
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
        }
    }
    exec(db, "DROP TABLE program_transition_runs");
}

void initialize_schema(sqlite3* db) {
    Transaction transaction(db);
    create_v2_schema(db);
    migrate_legacy_schema(db);
    transaction.commit();
}

}  // namespace

struct SQLiteProgramTransitionStore::Impl {
    explicit Impl(std::string value) : path(std::move(value)) {}
    ~Impl() {
        if (db) sqlite3_close_v2(db);
    }

    std::string       path;
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

std::optional<ProgramRunRecord> SQLiteProgramTransitionStore::load(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->run_record;
}

std::optional<ProgramJournalRecord> SQLiteProgramTransitionStore::latest(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->journal_record;
}

std::vector<ProgramEvent> SQLiteProgramTransitionStore::load_events(
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

std::vector<ProgramEffectOutboxEntry> SQLiteProgramTransitionStore::load_effects(
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
SQLiteProgramTransitionStore::load_javascript_commands(std::string_view owner_scope,
                                                       std::string_view run_id,
                                                       std::uint64_t    after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(
        impl_->db, "SELECT coordinate_id, canonical_bytes FROM "
                   "program_transition_javascript_command_log_v2 "
                   "WHERE owner_scope = ?1 AND run_id = ?2 AND sequence > ?3 "
                   "ORDER BY sequence ASC");
    statement.bind_text(1, owner_scope);
    statement.bind_text(2, run_id);
    statement.bind_uint64(3, after_sequence);
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    while (statement.step_row()) {
        auto entry = ProgramJavaScriptCommandJournalEntry::parse(column_blob(statement.get(), 1));
        if (column_text(statement.get(), 0) != entry.coordinate_id()) {
            throw std::invalid_argument(
                "Stored JavaScript command journal coordinate column mismatch");
        }
        result.push_back(std::move(entry));
    }
    return result;
}

std::optional<MigrationPlan> SQLiteProgramTransitionStore::load_migration_plan(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->db, owner_scope, run_id);
    if (!head) return std::nullopt;
    return head->migration_plan;
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_lineage(
    std::string_view owner_scope, std::string_view lineage_id) const {
    if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_current_lineage_head(impl_->db, owner_scope, lineage_id);
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_run_lineage(
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

std::optional<ProgramRunGeneration> SQLiteProgramTransitionStore::load_generation(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_record(impl_->db, owner_scope, lineage_id, generation);
}

std::optional<ProgramRunLineage> SQLiteProgramTransitionStore::load_lineage_head(
    std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const {
    if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_historical_lineage_head(impl_->db, owner_scope, lineage_id, head_id);
}

ProgramTransitionPublishResult SQLiteProgramTransitionStore::compare_publish(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication) {
    std::string publication_bytes;
    try {
        if (publication.run_record.owner_scope() != owner_scope)
            throw std::invalid_argument("Program transition owner scope does not match the publication");
        if (!valid_effect_outbox_binding(publication.run_record, publication.effects))
            return ProgramTransitionPublishResult::Conflict;
        publication_bytes = publication.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return ProgramTransitionPublishResult::Conflict;
    }

    const std::string run_id = publication.run_record.run_id();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    const auto current = load_head(impl_->db, owner_scope, run_id);

    if (current && current->last_publication_bytes == publication_bytes) {
        const auto result = expected_journal_head == publication.journal_record.previous_id
                                ? ProgramTransitionPublishResult::AlreadyPresent
                                : ProgramTransitionPublishResult::Conflict;
        transaction.commit();
        return result;
    }

    if (!current) {
        if (!expected_journal_head.empty() || !valid_initial_publication(publication)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (!valid_increment(impl_->db, owner_scope, *current, publication,
                                expected_journal_head)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto run_lineage_id = load_run_lineage_id(impl_->db, owner_scope, run_id);
    const bool adopting_legacy_lineage = current && !run_lineage_id && publication.run_lineage;
    if (current && run_lineage_id) {
        if (!publication.run_lineage ||
            *run_lineage_id != publication.run_lineage->lineage_id()) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (run_lineage_id) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    std::optional<ProgramRunLineage> current_lineage;
    if (publication.run_lineage) {
        current_lineage = load_current_lineage_head(
            impl_->db, owner_scope, publication.run_lineage->lineage_id());
        if (!current_lineage) {
            if (!publication.run_generation ||
                !is_valid_program_run_lineage_initial(*publication.run_lineage,
                                                      *publication.run_generation)) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        } else if (adopting_legacy_lineage) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        } else if (!is_valid_program_run_lineage_transition(
                       *current_lineage, *publication.run_lineage,
                       publication.run_generation)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
        if (current_lineage && !publication.run_generation) {
            const auto active = load_generation_record(
                impl_->db, owner_scope, publication.run_lineage->lineage_id(),
                publication.run_lineage->active_generation());
            if (!active || !does_program_run_generation_bind(
                               *active, *publication.run_lineage, publication.run_record)) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        if (current_lineage && publication.run_generation) {
            const auto active = load_generation_record(
                impl_->db, owner_scope, publication.run_lineage->lineage_id(),
                current_lineage->active_generation());
            if (!active || active->child_depth() != publication.run_generation->child_depth()) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
            const auto source = active ? load_head(impl_->db, owner_scope, active->run_id())
                                       : std::nullopt;
            if (!source ||
                !fork_binds_predecessor(publication.run_record, *active, *current_lineage,
                                        source->run_record)) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        if (publication.run_generation && current_lineage &&
            load_generation_record(impl_->db, owner_scope,
                                   publication.run_lineage->lineage_id(),
                                   publication.run_generation->generation())) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    std::optional<ProgramRunLineage> current_fork_source;
    if (publication.fork_source_lineage) {
        current_fork_source = load_current_lineage_head(
            impl_->db, owner_scope, publication.fork_source_lineage->lineage_id());
        if (!current_fork_source ||
            !is_valid_program_run_lineage_transition(
                *current_fork_source, *publication.fork_source_lineage) ||
            !fork_allocation_fits(*current_fork_source, *publication.fork_source_lineage,
                                  publication.journal_record.remaining_budget)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
        const auto active = load_generation_record(
            impl_->db, owner_scope, current_fork_source->lineage_id(),
            current_fork_source->active_generation());
        const auto source = active ? load_head(impl_->db, owner_scope, active->run_id())
                                   : std::nullopt;
        if (!active || !source ||
            !fork_binds_predecessor(publication.run_record, *active, *current_fork_source,
                                    source->run_record)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (!current && publication.run_record.fork_receipt()) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto& migration_plan = current ? current->migration_plan : publication.migration_plan;
    if (current)
        update_head(impl_->db, owner_scope, publication.run_record, publication.journal_record,
                    migration_plan, publication_bytes);
    else
        insert_head(impl_->db, owner_scope, publication.run_record, publication.journal_record,
                    migration_plan, publication_bytes);
    append_events(impl_->db, owner_scope, run_id, publication.events);
    append_effects(impl_->db, owner_scope, run_id, publication.effects);
    append_javascript_commands(impl_->db, owner_scope, run_id, publication.commands);
    if (publication.run_lineage) {
        if (current_lineage)
            update_lineage_head(impl_->db, *publication.run_lineage);
        else
            insert_lineage_head(impl_->db, *publication.run_lineage);
        insert_lineage_history(impl_->db, *publication.run_lineage);
        if (publication.run_generation)
            insert_generation(impl_->db, *publication.run_generation);
        if (!run_lineage_id)
            insert_run_lineage(impl_->db, owner_scope, run_id,
                               publication.run_lineage->lineage_id());
    }
    if (publication.fork_source_lineage) {
        update_lineage_head(impl_->db, *publication.fork_source_lineage);
        insert_lineage_history(impl_->db, *publication.fork_source_lineage);
    }
    transaction.commit();
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
