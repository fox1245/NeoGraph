#include <neograph/program/postgres_transition_store.h>

#include "canonical_json.h"

#include <libpq-fe.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

class PgResult final {
public:
    explicit PgResult(PGresult* result) : result_(result) {}
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}
    PgResult& operator=(PgResult&& other) noexcept {
        if (this != &other) {
            clear();
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }
    ~PgResult() { clear(); }

    PGresult* get() const noexcept { return result_; }

private:
    void clear() noexcept {
        if (result_) PQclear(result_);
        result_ = nullptr;
    }
    PGresult* result_ = nullptr;
};

[[noreturn]] void throw_pg(PGconn* connection, std::string_view operation) {
    std::string message(operation);
    message += ": ";
    if (connection) {
        const auto* detail = PQerrorMessage(connection);
        if (detail && *detail) message += detail;
    }
    throw std::runtime_error(std::move(message));
}

void check_result(PGconn* connection, const PgResult& result, std::string_view operation) {
    if (!result.get()) throw_pg(connection, operation);
    const auto status = PQresultStatus(result.get());
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) return;
    std::string message(operation);
    message += ": ";
    const auto* detail = PQresultErrorMessage(result.get());
    if (detail && *detail) message += detail;
    throw std::runtime_error(std::move(message));
}

PgResult exec_sql(PGconn* connection, std::string_view sql) {
    PgResult result(PQexec(connection, std::string(sql).c_str()));
    check_result(connection, result, "PostgreSQL transition command");
    return result;
}

PgResult exec_params(PGconn* connection, std::string_view sql,
                     const std::vector<std::string>& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) values.push_back(param.c_str());
    PgResult result(PQexecParams(connection, std::string(sql).c_str(),
                                 static_cast<int>(values.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0));
    check_result(connection, result, "PostgreSQL transition parameterized command");
    return result;
}

std::string row_text(const PgResult& result, int row, int column) {
    if (PQgetisnull(result.get(), row, column))
        throw std::invalid_argument("Stored PostgreSQL Program transition field is null");
    return std::string(PQgetvalue(result.get(), row, column),
                       static_cast<std::size_t>(PQgetlength(result.get(), row, column)));
}

std::optional<std::string> row_optional_text(const PgResult& result, int row, int column) {
    if (PQgetisnull(result.get(), row, column)) return std::nullopt;
    return row_text(result, row, column);
}

std::uint64_t parse_u64(std::string_view value, std::string_view field) {
    if (value.empty() || value.front() == '-')
        throw std::invalid_argument("Stored PostgreSQL Program transition " +
                                    std::string(field) + " is invalid");
    std::size_t consumed = 0;
    const auto result = std::stoull(std::string(value), &consumed);
    if (consumed != value.size())
        throw std::invalid_argument("Stored PostgreSQL Program transition " +
                                    std::string(field) + " is invalid");
    return result;
}

std::uint64_t affected_rows(const PgResult& result) {
    const auto* value = PQcmdTuples(result.get());
    if (!value || !*value) return 0;
    return std::stoull(value);
}

class Transaction final {
public:
    explicit Transaction(PGconn* connection) : connection_(connection) {
        exec_sql(connection_, "BEGIN");
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (!committed_) {
            try {
                exec_sql(connection_, "ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit() {
        exec_sql(connection_, "COMMIT");
        committed_ = true;
    }

private:
    PGconn* connection_ = nullptr;
    bool committed_ = false;
};

void lock_owner(PGconn* connection, std::string_view owner_scope) {
    (void)exec_params(connection,
                      "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                      {std::string(owner_scope)});
}

struct StoredHead {
    ProgramRunRecord run_record;
    ProgramJournalRecord journal_record;
    std::optional<MigrationPlan> migration_plan;
    std::string last_publication_bytes;
};

std::optional<StoredHead> load_head(PGconn* connection, std::string_view owner_scope,
                                    std::string_view run_id, bool lock = false) {
    auto result = exec_params(
        connection,
        std::string("SELECT run_record_bytes, journal_record_bytes, migration_plan_bytes, "
                    "last_publication_bytes FROM neograph_program_transition_run_heads_v2 "
                    "WHERE owner_scope = $1 AND run_id = $2") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 4)
        throw std::invalid_argument("Stored PostgreSQL Program transition head shape is invalid");
    auto migration = row_optional_text(result, 0, 2);
    return StoredHead{
        ProgramRunRecord::parse(row_text(result, 0, 0)),
        ProgramJournalRecord::parse(row_text(result, 0, 1)),
        migration ? std::optional<MigrationPlan>(MigrationPlan::parse(*migration)) : std::nullopt,
        row_text(result, 0, 3)};
}

std::optional<ProgramRunLineage> load_current_lineage_head(
    PGconn* connection, std::string_view owner_scope, std::string_view lineage_id,
    bool lock = false) {
    auto result = exec_params(
        connection,
        std::string("SELECT head_bytes FROM neograph_program_run_lineage_heads_v1 "
                    "WHERE owner_scope = $1 AND lineage_id = $2") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(lineage_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::invalid_argument("Stored PostgreSQL Program lineage head shape is invalid");
    auto lineage = ProgramRunLineage::parse(row_text(result, 0, 0));
    if (lineage.owner_scope() != owner_scope || lineage.lineage_id() != lineage_id)
        throw std::invalid_argument("Stored PostgreSQL Program lineage head binding is corrupt");
    return lineage;
}

std::optional<ProgramRunLineage> load_historical_lineage_head(
    PGconn* connection, std::string_view owner_scope, std::string_view lineage_id,
    std::string_view head_id) {
    auto result = exec_params(
        connection,
        "SELECT canonical_bytes FROM neograph_program_run_lineage_history_v1 "
        "WHERE owner_scope = $1 AND lineage_id = $2 AND head_id = $3",
        {std::string(owner_scope), std::string(lineage_id), std::string(head_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::invalid_argument("Stored PostgreSQL Program lineage history shape is invalid");
    auto lineage = ProgramRunLineage::parse(row_text(result, 0, 0));
    if (lineage.owner_scope() != owner_scope || lineage.lineage_id() != lineage_id ||
        lineage.id() != head_id)
        throw std::invalid_argument("Stored PostgreSQL Program lineage history is corrupt");
    return lineage;
}

std::optional<ProgramRunGeneration> load_generation_record(
    PGconn* connection, std::string_view owner_scope, std::string_view lineage_id,
    std::uint64_t generation) {
    auto result = exec_params(
        connection,
        "SELECT generation_id, canonical_bytes FROM neograph_program_run_generations_v1 "
        "WHERE owner_scope = $1 AND lineage_id = $2 AND generation = $3",
        {std::string(owner_scope), std::string(lineage_id), std::to_string(generation)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL Program generation shape is invalid");
    auto value = ProgramRunGeneration::parse(row_text(result, 0, 1));
    if (value.owner_scope() != owner_scope || value.lineage_id() != lineage_id ||
        value.generation() != generation || value.id() != row_text(result, 0, 0))
        throw std::invalid_argument("Stored PostgreSQL Program generation binding is corrupt");
    return value;
}

std::optional<ProgramTransitionPublication> load_generation_initial_publication_record(
    PGconn* connection, std::string_view owner_scope, std::string_view lineage_id,
    std::uint64_t generation) {
    auto result = exec_params(
        connection,
        "SELECT generation_id, canonical_bytes "
        "FROM neograph_program_run_generation_publications_v1 "
        "WHERE owner_scope = $1 AND lineage_id = $2 AND generation = $3",
        {std::string(owner_scope), std::string(lineage_id), std::to_string(generation)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL generation publication shape is invalid");
    auto publication = ProgramTransitionPublication::parse(row_text(result, 0, 1));
    if (!publication.run_generation || publication.run_generation->generation() != generation ||
        publication.run_generation->lineage_id() != lineage_id ||
        publication.run_generation->owner_scope() != owner_scope ||
        publication.run_generation->id() != row_text(result, 0, 0))
        throw std::invalid_argument("Stored PostgreSQL generation publication is corrupt");
    return publication;
}

std::optional<GraphMigrationCapsule> load_graph_migration_capsule_record(
    PGconn* connection, std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) {
    auto result = exec_params(
        connection,
        "SELECT capsule_id, capsule_bytes FROM neograph_program_graph_migration_capsules_v1 "
        "WHERE owner_scope = $1 AND source_run_id = $2 AND source_lineage_head_id = $3",
        {std::string(owner_scope), std::string(source_run_id),
         std::string(source_lineage_head_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL graph migration capsule shape is invalid");
    auto capsule = GraphMigrationCapsule::parse(row_text(result, 0, 1));
    if (capsule.owner_scope() != owner_scope || capsule.source_run_id() != source_run_id ||
        capsule.source_lineage_head_id() != source_lineage_head_id ||
        capsule.id() != row_text(result, 0, 0))
        throw std::invalid_argument("Stored PostgreSQL graph migration capsule is corrupt");
    return capsule;
}

std::optional<ProgramExecutionLease> load_execution_lease_record(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    bool lock = false) {
    auto result = exec_params(
        connection,
        std::string("SELECT lease_id, lease_bytes FROM neograph_program_execution_leases_v1 "
                    "WHERE owner_scope = $1 AND run_id = $2") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL execution lease shape is invalid");
    auto lease = ProgramExecutionLease::parse(row_text(result, 0, 1));
    if (lease.owner_scope() != owner_scope || lease.run_id() != run_id ||
        lease.id() != row_text(result, 0, 0))
        throw std::invalid_argument("Stored PostgreSQL execution lease is corrupt");
    return lease;
}

std::optional<std::string> load_run_lineage_id(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id) {
    auto result = exec_params(
        connection,
        "SELECT lineage_id FROM neograph_program_run_lineage_runs_v1 "
        "WHERE owner_scope = $1 AND run_id = $2",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::invalid_argument("Stored PostgreSQL run-lineage shape is invalid");
    return row_text(result, 0, 0);
}

std::optional<ProgramJavaScriptCommandJournalEntry> load_latest_javascript_command(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id) {
    auto result = exec_params(
        connection,
        "SELECT coordinate_id, canonical_bytes FROM "
        "neograph_program_transition_javascript_command_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence DESC LIMIT 1",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    auto entry = ProgramJavaScriptCommandJournalEntry::parse(row_text(result, 0, 1));
    if (entry.coordinate_id() != row_text(result, 0, 0))
        throw std::invalid_argument("Stored PostgreSQL JavaScript command is corrupt");
    return entry;
}

std::string context_publication_bytes(const ProgramContextPublication& context) {
    json artifacts = json::array();
    for (const auto& artifact : context.artifacts)
        artifacts.push_back(detail::parse_json_strict(artifact.serialize_canonical()));
    return detail::canonical_json_bytes(
        json{{"epoch", detail::parse_json_strict(context.epoch.serialize_canonical())},
             {"artifacts", std::move(artifacts)},
             {"assembly_receipt",
              detail::parse_json_strict(context.assembly_receipt.serialize_canonical())}});
}

ProgramContextPublication parse_context_publication_bytes(std::string_view bytes,
                                                           const ProgramRunRecord& run) {
    const auto encoded = detail::parse_json_strict(bytes);
    if (!encoded.is_object())
        throw std::invalid_argument("Stored Program context publication must be an object");
    detail::reject_unknown_fields(encoded, "Stored Program context publication",
                                  {"epoch", "artifacts", "assembly_receipt"});
    if (!encoded.contains("epoch") || !encoded.contains("artifacts") ||
        !encoded.contains("assembly_receipt") || !encoded.at("artifacts").is_array())
        throw std::invalid_argument("Stored Program context publication is incomplete");
    auto epoch = ContextEpoch::parse(detail::canonical_json_bytes(encoded.at("epoch")));
    std::vector<ContextArtifact> artifacts;
    for (const auto& artifact : encoded.at("artifacts"))
        artifacts.push_back(ContextArtifact::parse(detail::canonical_json_bytes(artifact)));
    auto receipt = ContextAssemblyReceipt::parse(
        detail::canonical_json_bytes(encoded.at("assembly_receipt")), epoch, artifacts);
    ProgramContextPublication context{std::move(epoch), std::move(artifacts), std::move(receipt)};
    validate_program_context_publication(context, run);
    return context;
}

std::vector<ProgramContextPublication> load_context_publication_history(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id) {
    const auto head = load_head(connection, owner_scope, run_id);
    if (!head) return {};
    auto rows = exec_params(
        connection,
        "SELECT sequence, canonical_bytes FROM neograph_program_transition_context_log_v1 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    std::vector<ProgramContextPublication> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        const auto sequence = parse_u64(row_text(rows, row, 0), "context sequence");
        auto context = parse_context_publication_bytes(row_text(rows, row, 1), head->run_record);
        if (context.epoch.sequence() != sequence)
            throw std::invalid_argument("Stored PostgreSQL context sequence is corrupt");
        result.push_back(std::move(context));
    }
    return result;
}

bool valid_context_history(const std::vector<ProgramContextPublication>& history) {
    std::vector<ProgramContextPublication> prefix;
    for (const auto& context : history) {
        if (!is_valid_program_context_history_append(prefix, context)) return false;
        prefix.push_back(context);
    }
    return true;
}

std::vector<HookOutboxEntry> load_hook_outbox_heads(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    const ProgramRunRecord& run) {
    auto rows = exec_params(
        connection,
        "SELECT invocation_id, canonical_bytes FROM "
        "neograph_program_transition_hook_outbox_log_v1 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    std::map<std::string, HookOutboxEntry, std::less<>> heads;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        auto entry = HookOutboxEntry::parse(row_text(rows, row, 1));
        if (entry.data().invocation.id() != row_text(rows, row, 0))
            throw std::invalid_argument("Stored PostgreSQL hook invocation is corrupt");
        std::vector<HookOutboxEntry> current;
        for (const auto& [_, head] : heads) current.push_back(head);
        if (!is_valid_program_hook_history_append(current, {entry}, run))
            throw std::invalid_argument("Stored PostgreSQL hook history is corrupt");
        heads.insert_or_assign(entry.data().invocation.id(), std::move(entry));
    }
    std::vector<HookOutboxEntry> result;
    for (auto& [_, entry] : heads) result.push_back(std::move(entry));
    return result;
}

bool has_blocking_hook_obligation(const std::vector<HookOutboxEntry>& entries) noexcept {
    return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        const auto state = entry.data().state;
        const bool pending = state != HookExecutionState::Succeeded &&
            state != HookExecutionState::Cancelled;
        return pending && entry.data().invocation.data().delivery == HookDelivery::BlockingMandatory;
    });
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
    const auto next_children = next.children();
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

bool effect_ids_are_new(PGconn* connection, std::string_view owner_scope,
                        std::string_view run_id,
                        const std::vector<ProgramEffectOutboxEntry>& new_effects) {
    std::set<std::string, std::less<>> ids;
    for (const auto& effect : new_effects) {
        const auto& effect_id = effect.effect().effect_id();
        if (!ids.emplace(effect_id).second) return false;
        auto result = exec_params(
            connection,
            "SELECT 1 FROM neograph_program_transition_effect_log_v2 "
            "WHERE owner_scope = $1 AND run_id = $2 AND effect_id = $3 LIMIT 1",
            {std::string(owner_scope), std::string(run_id), effect_id});
        if (PQntuples(result.get()) != 0) return false;
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
    const ProgramRunRecord& run,
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
            entry.sequence() != expected_sequence + 1)
            return false;
        ++expected_sequence;
        const auto found = std::find_if(prior.rbegin(), prior.rend(), [&](const auto& previous) {
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
        if (!found->pending() || !entry.completed() || !same_command_coordinate(*found, entry))
            return false;
        prior.push_back(entry);
    }
    return true;
}

bool budget_is_empty(const RunBudget& budget) noexcept { return budget == RunBudget{}; }

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
        !terminal_result->at("usage").is_object())
        return std::nullopt;
    const auto& usage = terminal_result->at("usage");
    const auto read = [&](std::string_view key) -> std::optional<std::uint64_t> {
        const std::string owned(key);
        if (!usage.contains(owned) || !usage.at(owned).is_number_unsigned()) return std::nullopt;
        return usage.at(owned).get<std::uint64_t>();
    };
    const auto wall = read("wall_time_ms");
    const auto model = read("model_tokens");
    const auto money = read("monetary_microunits");
    const auto operations = read("program_operations");
    const auto steps = read("core_steps");
    const auto peak = read("peak_concurrency");
    if (!wall || !model || !money || !operations || !steps || !peak ||
        *peak > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    return ProgramUsage{*wall, *model, *money, 0, *steps, static_cast<std::uint32_t>(*peak)};
}

bool checkpoint_event_matches(const std::vector<ProgramEvent>& events,
                              const std::optional<CoreCheckpointIdentity>& checkpoint) {
    if (!checkpoint) return false;
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint == *checkpoint;
    });
}

bool javascript_call_core_checkpoint_matches(
    const ProgramRunRecord& run, const ProgramJavaScriptCommandJournalEntry& pending,
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
        if (command.kind() == JavaScriptCommandKind::Await)
            return matches(JavaScriptCommand::from_json(arguments.at("command")),
                           operation_id + "/await", depth + 1);
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
    const ProgramRunRecord& previous_run, const ProgramJournalRecord& previous_journal,
    const ProgramJournalRecord& next_journal,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const ProgramTransitionPublication& publication,
    const ProgramGraphSafePointEvidence* safe_point_evidence,
    const GraphMigrationCapsule* safe_point_capsule,
    const ProgramExecutionLease* execution_lease) {
    const bool increased = budget_increased(next_journal.remaining_budget,
                                             previous_journal.remaining_budget);
    const bool ordinary = is_valid_program_journal_transition(previous_journal, next_journal);
    const auto completed = std::find_if(publication.commands.begin(), publication.commands.end(),
                                        [](const auto& entry) { return entry.completed(); });
    const bool checkpoint_changed = previous_journal.core_checkpoint != next_journal.core_checkpoint;
    if (completed != publication.commands.end()) {
        if (publication.commands.size() != 1 || !budget_is_empty(next_journal.inflight_reservation))
            return false;
        if (checkpoint_changed &&
            (!checkpoint_event_matches(publication.events, next_journal.core_checkpoint) ||
             !next_journal.core_checkpoint ||
             !javascript_call_core_checkpoint_matches(publication.run_record, *completed,
                                                      *next_journal.core_checkpoint)))
            return false;
        if (budget_is_empty(previous_journal.inflight_reservation)) return ordinary && !increased;
        const auto usage = command_terminal_usage(*completed);
        return usage && is_valid_program_journal_reservation_settlement(previous_journal,
                                                                         next_journal, *usage);
    }
    if (!increased) {
        if (safe_point_evidence && safe_point_capsule && execution_lease)
            return is_valid_program_graph_safe_point_transition(
                previous_run, previous_journal, publication, *safe_point_evidence,
                *safe_point_capsule, *execution_lease);
        return ordinary;
    }
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
    auto usage = publication.run_record.terminal_result()->usage();
    usage.program_operations = 0;
    return is_valid_program_journal_reservation_settlement(previous_journal, next_journal, usage);
}

bool valid_initial_publication(const ProgramTransitionPublication& publication) {
    const auto& journal = publication.journal_record;
    const auto terminal = publication.run_record.terminal_result();
    const bool terminal_event = !publication.events.empty() &&
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

std::vector<ProgramJavaScriptCommandJournalEntry> load_all_javascript_commands(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id) {
    auto rows = exec_params(
        connection,
        "SELECT coordinate_id, canonical_bytes FROM "
        "neograph_program_transition_javascript_command_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        auto entry = ProgramJavaScriptCommandJournalEntry::parse(row_text(rows, row, 1));
        if (entry.coordinate_id() != row_text(rows, row, 0))
            throw std::invalid_argument("Stored PostgreSQL JavaScript command is corrupt");
        result.push_back(std::move(entry));
    }
    return result;
}

bool valid_increment(PGconn* connection, std::string_view owner_scope,
                     const StoredHead& old_head,
                     const ProgramTransitionPublication& next_publication,
                     std::string_view expected_journal_head,
                     const ProgramGraphSafePointEvidence* safe_point_evidence,
                     const GraphMigrationCapsule* safe_point_capsule,
                     const ProgramExecutionLease* execution_lease) {
    const auto& old_run = old_head.run_record;
    const auto& next_run = next_publication.run_record;
    const auto& old_journal = old_head.journal_record;
    const auto& next_journal = next_publication.journal_record;
    const auto old_terminal = old_run.terminal_result();
    const auto new_terminal = next_run.terminal_result();
    const bool terminal_event = !next_publication.events.empty() &&
                                next_publication.events.back().kind == ProgramEventKind::Terminal;
    if ((old_terminal && is_final(old_run.continuation().state) && !new_terminal) ||
        (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
         !terminal_event))
        return false;
    if (old_journal.id != expected_journal_head ||
        next_journal.previous_id != expected_journal_head ||
        next_run.created_at_ms() != old_run.created_at_ms() ||
        next_run.updated_at_ms() < old_run.updated_at_ms() ||
        next_run.binding_fingerprint() != old_run.binding_fingerprint() ||
        !same_fork(old_run, next_run) ||
        next_run.recorded_binding_set_fingerprint() != old_run.recorded_binding_set_fingerprint() ||
        next_run.invocation() != old_run.invocation() ||
        (next_run.exact_checkpoint() == old_run.exact_checkpoint() &&
         next_run.exact_checkpoint_content_id() != old_run.exact_checkpoint_content_id()) ||
        !valid_children_transition(old_run, next_run) ||
        next_run.event_sequence() != old_run.event_sequence() + next_publication.events.size() ||
        next_run.effect_sequence() != old_run.effect_sequence() + next_publication.effects.size())
        return false;
    if (!next_publication.events.empty() &&
        next_publication.events.front().sequence != old_run.event_sequence() + 1)
        return false;
    if (!next_publication.effects.empty() &&
        next_publication.effects.front().sequence() != old_run.effect_sequence() + 1)
        return false;
    if (!effect_ids_are_new(connection, owner_scope, old_run.run_id(), next_publication.effects))
        return false;
    const auto old_commands = load_all_javascript_commands(connection, owner_scope,
                                                          old_run.run_id());
    if (!valid_command_history_append(old_run, old_commands, next_publication.commands) ||
        !valid_command_reservation_transition(old_run, old_journal, next_journal, old_commands,
                                              next_publication, safe_point_evidence,
                                              safe_point_capsule, execution_lease))
        return false;
    if (next_publication.migration_plan &&
        (!old_head.migration_plan ||
         next_publication.migration_plan->id() != old_head.migration_plan->id()))
        return false;
    return true;
}

bool fork_binds_predecessor(const ProgramRunRecord& target,
                            const ProgramRunGeneration& predecessor,
                            const ProgramRunLineage& lineage,
                            const ProgramRunRecord& source) noexcept {
    const auto receipt = target.fork_receipt();
    if (!receipt) return false;
    const auto checkpoint = source.exact_checkpoint();
    const auto resume_binds = [&]() noexcept {
        if (receipt->storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION)
            return true;
        try {
            if (const auto source_pending = source.pending_input()) {
                const auto target_pending = target.pending_input();
                const auto result = target_pending ? target_pending->consumed_result() : std::nullopt;
                if (!target_pending || !result || target.pending_effect() ||
                    !receipt->matches_initial_resume(target_pending->call_id(), *result))
                    return false;
                const auto applied = source_pending->submit(
                    target_pending->call_id(), *result,
                    static_cast<std::uint64_t>(target.created_at_ms()));
                return applied.disposition == ProgramPendingDisposition::Applied &&
                       applied.value == *target_pending;
            }
            if (const auto source_pending = source.pending_effect()) {
                const auto target_pending = target.pending_effect();
                const auto result = target_pending ? target_pending->reconciled_result() : std::nullopt;
                if (!target_pending || !result || target.pending_input() ||
                    !receipt->matches_initial_resume(target_pending->call_id(), *result))
                    return false;
                const auto applied = source_pending->submit(
                    target_pending->call_id(), target_pending->effect_id(), *result,
                    static_cast<std::uint64_t>(target.created_at_ms()));
                return applied.disposition == ProgramPendingDisposition::Applied &&
                       applied.value == *target_pending;
            }
            return !target.pending_input() && !target.pending_effect() &&
                   receipt->initial_resume_binding() &&
                   !receipt->initial_resume_binding()->target_pending_id;
        } catch (const std::exception&) {
            return false;
        }
    };
    return checkpoint && target.run_id() != source.run_id() && source.child_depth() == 0 &&
           target.created_at_ms() >= source.updated_at_ms() &&
           source.invocation().parent_run_id.empty() &&
           source.continuation().state == ContinuationState::Interrupted &&
           lineage.committed_descendant_budget() == RunBudget{} && receipt->compatible() &&
           receipt->owner_scope() == target.owner_scope() &&
           receipt->source_run_id() == predecessor.run_id() &&
           receipt->source_program_version_id() == predecessor.program_version_id() &&
           receipt->source_checkpoint_id() == checkpoint->checkpoint_id &&
           receipt->target_program_version_id() == target.program_version_id() && resume_binds() &&
           source.id() == lineage.active_run_record_id() &&
           source.journal_head() == lineage.active_journal_head();
}

bool fork_allocation_fits(const ProgramRunLineage& previous,
                          const ProgramRunLineage& debited,
                          const RunBudget& target) noexcept {
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
                debited.remaining_budget().max_program_operations, target.max_program_operations) &&
           fits(previous.remaining_budget().max_core_steps,
                debited.remaining_budget().max_core_steps, target.max_core_steps) &&
           fits(previous.remaining_budget().max_dynamic_compiles,
                debited.remaining_budget().max_dynamic_compiles, target.max_dynamic_compiles) &&
           fits(previous.remaining_budget().max_child_depth,
                debited.remaining_budget().max_child_depth, target.max_child_depth) &&
           fits(previous.remaining_budget().max_total_children,
                debited.remaining_budget().max_total_children, target.max_total_children);
}

void insert_lineage_head(PGconn* connection, const ProgramRunLineage& lineage) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_run_lineage_heads_v1"
        "(owner_scope, lineage_id, head_bytes) VALUES($1, $2, $3)",
        {lineage.owner_scope(), lineage.lineage_id(), lineage.serialize_canonical()});
}

void update_lineage_head(PGconn* connection, const ProgramRunLineage& lineage) {
    auto result = exec_params(
        connection,
        "UPDATE neograph_program_run_lineage_heads_v1 SET head_bytes = $1 "
        "WHERE owner_scope = $2 AND lineage_id = $3",
        {lineage.serialize_canonical(), lineage.owner_scope(), lineage.lineage_id()});
    if (affected_rows(result) != 1)
        throw std::runtime_error("PostgreSQL Program lineage head update lost its CAS target");
}

void insert_generation(PGconn* connection, const ProgramRunGeneration& generation) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_run_generations_v1"
        "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
        "VALUES($1, $2, $3, $4, $5)",
        {generation.owner_scope(), generation.lineage_id(),
         std::to_string(generation.generation()), generation.id(), generation.serialize_canonical()});
}

void insert_generation_initial_publication(PGconn* connection,
                                           const ProgramRunGeneration& generation,
                                           std::string_view publication_bytes) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_run_generation_publications_v1"
        "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
        "VALUES($1, $2, $3, $4, $5)",
        {generation.owner_scope(), generation.lineage_id(),
         std::to_string(generation.generation()), generation.id(),
         std::string(publication_bytes)});
}

void insert_lineage_history(PGconn* connection, const ProgramRunLineage& lineage) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_run_lineage_history_v1"
        "(owner_scope, lineage_id, head_id, canonical_bytes) VALUES($1, $2, $3, $4)",
        {lineage.owner_scope(), lineage.lineage_id(), lineage.id(), lineage.serialize_canonical()});
}

void insert_run_lineage(PGconn* connection, std::string_view owner_scope,
                        std::string_view run_id, std::string_view lineage_id) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_run_lineage_runs_v1"
        "(owner_scope, run_id, lineage_id) VALUES($1, $2, $3)",
        {std::string(owner_scope), std::string(run_id), std::string(lineage_id)});
}

void insert_graph_migration_capsule(PGconn* connection,
                                    const GraphMigrationCapsule& capsule) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_graph_migration_capsules_v1"
        "(owner_scope, source_run_id, source_lineage_head_id, capsule_id, capsule_bytes) "
        "VALUES($1, $2, $3, $4, $5)",
        {capsule.owner_scope(), capsule.source_run_id(), capsule.source_lineage_head_id(),
         capsule.id(), capsule.serialize_canonical()});
}

void insert_execution_lease(PGconn* connection, const ProgramExecutionLease& lease) {
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_execution_leases_v1"
        "(owner_scope, run_id, lease_id, lease_bytes) VALUES($1, $2, $3, $4) "
        "ON CONFLICT(owner_scope, run_id) DO UPDATE SET "
        "lease_id = EXCLUDED.lease_id, lease_bytes = EXCLUDED.lease_bytes",
        {lease.owner_scope(), lease.run_id(), lease.id(), lease.serialize_canonical()});
}

void delete_execution_lease(PGconn* connection, const ProgramExecutionLease& lease) {
    auto result = exec_params(
        connection,
        "DELETE FROM neograph_program_execution_leases_v1 "
        "WHERE owner_scope = $1 AND run_id = $2 AND lease_id = $3",
        {lease.owner_scope(), lease.run_id(), lease.id()});
    if (affected_rows(result) != 1)
        throw std::runtime_error("PostgreSQL Program execution lease delete lost its CAS target");
}

void insert_head(PGconn* connection, std::string_view owner_scope,
                 const ProgramRunRecord& run_record,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    const auto run_bytes = run_record.serialize_canonical();
    const auto journal_bytes = journal_record.serialize_canonical();
    const auto migration_bytes = migration_plan
        ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt;
    const std::string owner(owner_scope);
    const std::string run_id(run_record.run_id());
    const std::string publication(last_publication_bytes);
    const char* params[] = {owner.c_str(), run_id.c_str(), run_bytes.c_str(), journal_bytes.c_str(),
                            migration_bytes ? migration_bytes->c_str() : nullptr,
                            publication.c_str()};
    PgResult result(PQexecParams(
        connection,
        "INSERT INTO neograph_program_transition_run_heads_v2"
        "(owner_scope, run_id, run_record_bytes, journal_record_bytes, migration_plan_bytes, "
        "last_publication_bytes) VALUES($1, $2, $3, $4, $5, $6)",
        6, nullptr, params, nullptr, nullptr, 0));
    check_result(connection, result, "PostgreSQL Program transition head insert");
}

void update_head(PGconn* connection, std::string_view owner_scope,
                 const ProgramRunRecord& run_record,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    const auto run_bytes = run_record.serialize_canonical();
    const auto journal_bytes = journal_record.serialize_canonical();
    const auto migration_bytes = migration_plan
        ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt;
    const std::string owner(owner_scope);
    const std::string run_id(run_record.run_id());
    const std::string publication(last_publication_bytes);
    const char* params[] = {run_bytes.c_str(), journal_bytes.c_str(),
                            migration_bytes ? migration_bytes->c_str() : nullptr,
                            publication.c_str(), owner.c_str(), run_id.c_str()};
    PgResult result(PQexecParams(
        connection,
        "UPDATE neograph_program_transition_run_heads_v2 SET run_record_bytes = $1, "
        "journal_record_bytes = $2, migration_plan_bytes = $3, last_publication_bytes = $4 "
        "WHERE owner_scope = $5 AND run_id = $6",
        6, nullptr, params, nullptr, nullptr, 0));
    check_result(connection, result, "PostgreSQL Program transition head update");
    if (affected_rows(result) != 1)
        throw std::runtime_error("PostgreSQL Program transition head update lost its CAS target");
}

void append_events(PGconn* connection, std::string_view owner_scope, std::string_view run_id,
                   const std::vector<ProgramEvent>& events) {
    for (const auto& event : events)
        (void)exec_params(
            connection,
            "INSERT INTO neograph_program_transition_event_log_v2"
            "(owner_scope, run_id, sequence, canonical_bytes) VALUES($1, $2, $3, $4)",
            {std::string(owner_scope), std::string(run_id), std::to_string(event.sequence),
             event.serialize_canonical()});
}

void append_effects(PGconn* connection, std::string_view owner_scope, std::string_view run_id,
                    const std::vector<ProgramEffectOutboxEntry>& effects) {
    for (const auto& effect : effects)
        (void)exec_params(
            connection,
            "INSERT INTO neograph_program_transition_effect_log_v2"
            "(owner_scope, run_id, sequence, effect_id, canonical_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {std::string(owner_scope), std::string(run_id), std::to_string(effect.sequence()),
             effect.effect().effect_id(), effect.serialize_canonical()});
}

void append_javascript_commands(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& commands) {
    for (const auto& command : commands)
        (void)exec_params(
            connection,
            "INSERT INTO neograph_program_transition_javascript_command_log_v2"
            "(owner_scope, run_id, sequence, coordinate_id, canonical_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {std::string(owner_scope), std::string(run_id), std::to_string(command.sequence()),
             command.coordinate_id(), command.serialize_canonical()});
}

void append_context_publication(PGconn* connection, std::string_view owner_scope,
                                std::string_view run_id,
                                const std::optional<ProgramContextPublication>& context) {
    if (!context) return;
    (void)exec_params(
        connection,
        "INSERT INTO neograph_program_transition_context_log_v1"
        "(owner_scope, run_id, sequence, canonical_bytes) VALUES($1, $2, $3, $4)",
        {std::string(owner_scope), std::string(run_id),
         std::to_string(context->epoch.sequence()), context_publication_bytes(*context)});
}

void append_hook_outbox_entries(PGconn* connection, std::string_view owner_scope,
                                std::string_view run_id,
                                const std::vector<HookOutboxEntry>& entries) {
    for (const auto& entry : entries)
        (void)exec_params(
            connection,
            "INSERT INTO neograph_program_transition_hook_outbox_log_v1"
            "(owner_scope, run_id, invocation_id, head_id, canonical_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {std::string(owner_scope), std::string(run_id), entry.data().invocation.id(),
             entry.id(), entry.serialize_canonical()});
}

void create_schema(PGconn* connection) {
    exec_sql(connection, R"SQL(
CREATE TABLE IF NOT EXISTS neograph_program_transition_run_heads_v2 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL,
    run_record_bytes TEXT NOT NULL, journal_record_bytes TEXT NOT NULL,
    migration_plan_bytes TEXT, last_publication_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, run_id));
CREATE TABLE IF NOT EXISTS neograph_program_transition_event_log_v2 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence BIGINT NOT NULL,
    canonical_bytes TEXT NOT NULL, PRIMARY KEY(owner_scope, run_id, sequence),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_transition_effect_log_v2 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence BIGINT NOT NULL,
    effect_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, run_id, sequence), UNIQUE(owner_scope, run_id, effect_id),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_transition_javascript_command_log_v2 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence BIGINT NOT NULL,
    coordinate_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, run_id, sequence),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_transition_context_log_v1 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, sequence BIGINT NOT NULL,
    canonical_bytes TEXT NOT NULL, PRIMARY KEY(owner_scope, run_id, sequence),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_transition_hook_outbox_log_v1 (
    sequence BIGSERIAL PRIMARY KEY, owner_scope TEXT NOT NULL, run_id TEXT NOT NULL,
    invocation_id TEXT NOT NULL, head_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS neograph_program_transition_hook_outbox_run_v1
    ON neograph_program_transition_hook_outbox_log_v1(owner_scope, run_id, sequence);
CREATE TABLE IF NOT EXISTS neograph_program_run_lineage_heads_v1 (
    owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, head_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, lineage_id));
CREATE TABLE IF NOT EXISTS neograph_program_run_generations_v1 (
    owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, generation BIGINT NOT NULL,
    generation_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, lineage_id, generation),
    UNIQUE(owner_scope, lineage_id, generation_id),
    FOREIGN KEY(owner_scope, lineage_id)
        REFERENCES neograph_program_run_lineage_heads_v1(owner_scope, lineage_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_run_generation_publications_v1 (
    owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, generation BIGINT NOT NULL,
    generation_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, lineage_id, generation),
    FOREIGN KEY(owner_scope, lineage_id, generation)
        REFERENCES neograph_program_run_generations_v1(owner_scope, lineage_id, generation)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_run_lineage_history_v1 (
    owner_scope TEXT NOT NULL, lineage_id TEXT NOT NULL, head_id TEXT NOT NULL,
    canonical_bytes TEXT NOT NULL, PRIMARY KEY(owner_scope, lineage_id, head_id),
    FOREIGN KEY(owner_scope, lineage_id)
        REFERENCES neograph_program_run_lineage_heads_v1(owner_scope, lineage_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_run_lineage_runs_v1 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, lineage_id TEXT NOT NULL,
    PRIMARY KEY(owner_scope, run_id),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE,
    FOREIGN KEY(owner_scope, lineage_id)
        REFERENCES neograph_program_run_lineage_heads_v1(owner_scope, lineage_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_graph_migration_capsules_v1 (
    owner_scope TEXT NOT NULL, source_run_id TEXT NOT NULL,
    source_lineage_head_id TEXT NOT NULL, capsule_id TEXT NOT NULL,
    capsule_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, source_run_id, source_lineage_head_id),
    FOREIGN KEY(owner_scope, source_run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
CREATE TABLE IF NOT EXISTS neograph_program_execution_leases_v1 (
    owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, lease_id TEXT NOT NULL,
    lease_bytes TEXT NOT NULL, PRIMARY KEY(owner_scope, run_id),
    FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id)
        ON DELETE CASCADE);
)SQL");
}

}  // namespace

struct PostgreSQLProgramTransitionStore::Impl {
    explicit Impl(std::string value) : connection_string(std::move(value)) {}
    ~Impl() {
        if (connection) PQfinish(connection);
    }
    std::string connection_string;
    std::string coordination_key;
    PGconn* connection = nullptr;
    mutable std::mutex mutex;
};

PostgreSQLProgramTransitionStore::PostgreSQLProgramTransitionStore(std::string connection_string)
    : impl_(std::make_unique<Impl>(std::move(connection_string))) {
    if (impl_->connection_string.empty())
        throw std::invalid_argument("PostgreSQL Program transition connection string is empty");
    impl_->coordination_key = "postgres-transition:" +
        std::to_string(std::hash<std::string>{}(impl_->connection_string));
    impl_->connection = PQconnectdb(impl_->connection_string.c_str());
    if (!impl_->connection || PQstatus(impl_->connection) != CONNECTION_OK) {
        const auto* detail = impl_->connection ? PQerrorMessage(impl_->connection) : nullptr;
        std::string message = "PostgreSQL Program transition connection failed";
        if (detail && *detail) message += ": " + std::string(detail);
        if (impl_->connection) PQfinish(impl_->connection);
        impl_->connection = nullptr;
        throw std::runtime_error(std::move(message));
    }
    try {
        create_schema(impl_->connection);
    } catch (...) {
        PQfinish(impl_->connection);
        impl_->connection = nullptr;
        throw;
    }
}

PostgreSQLProgramTransitionStore::PostgreSQLProgramTransitionStore(
    PostgreSQLProgramTransitionStore&&) noexcept = default;
PostgreSQLProgramTransitionStore& PostgreSQLProgramTransitionStore::operator=(
    PostgreSQLProgramTransitionStore&&) noexcept = default;
PostgreSQLProgramTransitionStore::~PostgreSQLProgramTransitionStore() = default;

std::string PostgreSQLProgramTransitionStore::process_coordination_key() const {
    return impl_->coordination_key;
}

std::optional<ProgramRunRecord> PostgreSQLProgramTransitionStore::load(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id);
    return head ? std::optional<ProgramRunRecord>(head->run_record) : std::nullopt;
}

std::optional<ProgramJournalRecord> PostgreSQLProgramTransitionStore::latest(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id);
    return head ? std::optional<ProgramJournalRecord>(head->journal_record) : std::nullopt;
}

std::vector<ProgramEvent> PostgreSQLProgramTransitionStore::load_events(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    auto rows = exec_params(
        impl_->connection,
        "SELECT canonical_bytes FROM neograph_program_transition_event_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 AND sequence > $3 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id), std::to_string(after_sequence)});
    std::vector<ProgramEvent> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row)
        result.push_back(ProgramEvent::parse(row_text(rows, row, 0)));
    return result;
}

std::vector<ProgramEffectOutboxEntry> PostgreSQLProgramTransitionStore::load_effects(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    auto rows = exec_params(
        impl_->connection,
        "SELECT canonical_bytes FROM neograph_program_transition_effect_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 AND sequence > $3 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id), std::to_string(after_sequence)});
    std::vector<ProgramEffectOutboxEntry> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row)
        result.push_back(ProgramEffectOutboxEntry::parse(row_text(rows, row, 0)));
    return result;
}

std::vector<ProgramJavaScriptCommandJournalEntry>
PostgreSQLProgramTransitionStore::load_javascript_commands(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    auto rows = exec_params(
        impl_->connection,
        "SELECT coordinate_id, canonical_bytes FROM "
        "neograph_program_transition_javascript_command_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 AND sequence > $3 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id), std::to_string(after_sequence)});
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        auto entry = ProgramJavaScriptCommandJournalEntry::parse(row_text(rows, row, 1));
        if (entry.coordinate_id() != row_text(rows, row, 0))
            throw std::invalid_argument("Stored PostgreSQL JavaScript command is corrupt");
        result.push_back(std::move(entry));
    }
    return result;
}

std::vector<ProgramContextPublication>
PostgreSQLProgramTransitionStore::load_context_publications(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto all = load_context_publication_history(impl_->connection, owner_scope, run_id);
    if (!valid_context_history(all))
        throw std::invalid_argument("Stored PostgreSQL context history is not contiguous");
    std::vector<ProgramContextPublication> result;
    for (const auto& context : all) {
        if (context.epoch.sequence() > after_sequence) result.push_back(context);
    }
    return result;
}

std::vector<HookOutboxEntry> PostgreSQLProgramTransitionStore::load_hook_outbox_entries(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id);
    if (!head) return {};
    auto result = load_hook_outbox_heads(impl_->connection, owner_scope, run_id, head->run_record);
    if (!is_valid_program_hook_history_append({}, result, head->run_record))
        throw std::invalid_argument("Stored PostgreSQL hook history is corrupt");
    return result;
}

std::optional<MigrationPlan> PostgreSQLProgramTransitionStore::load_migration_plan(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id);
    return head ? head->migration_plan : std::nullopt;
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage(
    std::string_view owner_scope, std::string_view lineage_id) const {
    if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_current_lineage_head(impl_->connection, owner_scope, lineage_id);
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_run_lineage(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto lineage_id = load_run_lineage_id(impl_->connection, owner_scope, run_id);
    if (!lineage_id) return std::nullopt;
    const auto lineage = load_current_lineage_head(impl_->connection, owner_scope, *lineage_id);
    if (!lineage) throw std::invalid_argument("Stored PostgreSQL run-lineage binding is corrupt");
    return lineage;
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage_head(
    std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const {
    if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_historical_lineage_head(impl_->connection, owner_scope, lineage_id, head_id);
}

std::optional<ProgramRunGeneration> PostgreSQLProgramTransitionStore::load_generation(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_record(impl_->connection, owner_scope, lineage_id, generation);
}

std::optional<ProgramTransitionPublication>
PostgreSQLProgramTransitionStore::load_generation_initial_publication(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_initial_publication_record(
        impl_->connection, owner_scope, lineage_id, generation);
}

std::optional<GraphMigrationCapsule>
PostgreSQLProgramTransitionStore::load_graph_migration_capsule(
    std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) const {
    if (owner_scope.empty() || source_run_id.empty() || source_lineage_head_id.empty())
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_graph_migration_capsule_record(
        impl_->connection, owner_scope, source_run_id, source_lineage_head_id);
}

std::optional<ProgramExecutionLease> PostgreSQLProgramTransitionStore::load_execution_lease(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_execution_lease_record(impl_->connection, owner_scope, run_id);
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication) {
    return compare_publish_impl(owner_scope, expected_journal_head, std::move(publication),
                                nullptr, nullptr, nullptr, nullptr);
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish_execution(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    std::optional<ProgramExecutionLease> expected_lease,
    std::optional<ProgramExecutionLease> next_lease) {
    return compare_publish_impl(owner_scope, expected_journal_head, std::move(publication),
                                nullptr, nullptr,
                                expected_lease ? &*expected_lease : nullptr,
                                next_lease ? &*next_lease : nullptr);
}

ProgramTransitionPublishResult
PostgreSQLProgramTransitionStore::compare_publish_graph_safe_point(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence& evidence,
    const GraphMigrationCapsule& capsule,
    const ProgramExecutionLease& execution_lease) {
    return compare_publish_impl(owner_scope, expected_journal_head, std::move(publication),
                                &evidence, &capsule, &execution_lease, nullptr);
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish_impl(
    std::string_view owner_scope, std::string_view expected_journal_head,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence* safe_point_evidence,
    const GraphMigrationCapsule* safe_point_capsule,
    const ProgramExecutionLease* expected_lease,
    const ProgramExecutionLease* next_lease) {
    std::string publication_bytes;
    try {
        if (publication.run_record.owner_scope() != owner_scope ||
            static_cast<bool>(safe_point_evidence) != static_cast<bool>(safe_point_capsule) ||
            (safe_point_evidence && (!expected_lease || next_lease)))
            throw std::invalid_argument("Program transition owner scope mismatch");
        if (!valid_effect_outbox_binding(publication.run_record, publication.effects))
            return ProgramTransitionPublishResult::Conflict;
        publication_bytes = publication.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return ProgramTransitionPublishResult::Conflict;
    }

    const std::string run_id = publication.run_record.run_id();
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    lock_owner(impl_->connection, owner_scope);
    const auto current = load_head(impl_->connection, owner_scope, run_id, true);
    const auto current_lease = load_execution_lease_record(
        impl_->connection, owner_scope, run_id, true);
    if (safe_point_evidence && !current) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    if (current && current->last_publication_bytes == publication_bytes) {
        if (safe_point_capsule) {
            const auto stored = load_graph_migration_capsule_record(
                impl_->connection, owner_scope, run_id,
                safe_point_capsule->source_lineage_head_id());
            if (!stored || stored->id() != safe_point_capsule->id()) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        const bool lease_result_matches = next_lease
            ? current_lease && current_lease->id() == next_lease->id()
            : !expected_lease || !current_lease;
        if (!lease_result_matches) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
        const auto result = expected_journal_head == publication.journal_record.previous_id
            ? ProgramTransitionPublishResult::AlreadyPresent
            : ProgramTransitionPublishResult::Conflict;
        transaction.commit();
        return result;
    }

    if (expected_lease) {
        if (!current_lease || current_lease->id() != expected_lease->id() || !current ||
            expected_lease->owner_scope() != owner_scope || expected_lease->run_id() != run_id ||
            expected_lease->attempt() != current->run_record.continuation().attempt ||
            expected_lease->program_version_id() != current->run_record.program_version_id() ||
            expected_lease->bundle_id() != current->run_record.bundle_id()) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (current_lease) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    if (next_lease && !does_program_execution_lease_bind(*next_lease, publication)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    if (!current && publication.fork_source_lineage) {
        const auto fork = publication.run_record.fork_receipt();
        if (!fork ||
            fork->storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    if (!current) {
        if (!expected_journal_head.empty() || !valid_initial_publication(publication)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    } else if (!valid_increment(impl_->connection, owner_scope, *current, publication,
                                expected_journal_head, safe_point_evidence,
                                safe_point_capsule, expected_lease)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto context_history = load_context_publication_history(
        impl_->connection, owner_scope, run_id);
    if (!valid_context_history(context_history) ||
        !is_valid_program_context_history_append(context_history,
                                                 publication.context_publication)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    const auto hook_heads = load_hook_outbox_heads(
        impl_->connection, owner_scope, run_id,
        current ? current->run_record : publication.run_record);
    if (!is_valid_program_hook_history_append(hook_heads, publication.hook_outbox_entries,
                                              publication.run_record)) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto run_lineage_id = load_run_lineage_id(impl_->connection, owner_scope, run_id);
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
            impl_->connection, owner_scope, publication.run_lineage->lineage_id(), true);
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
                impl_->connection, owner_scope, publication.run_lineage->lineage_id(),
                publication.run_lineage->active_generation());
            if (!active || !does_program_run_generation_bind(
                               *active, *publication.run_lineage, publication.run_record)) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        if (current_lineage && publication.run_generation) {
            const auto active = load_generation_record(
                impl_->connection, owner_scope, publication.run_lineage->lineage_id(),
                current_lineage->active_generation());
            if (!active || active->child_depth() != publication.run_generation->child_depth()) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
            const auto source = load_head(impl_->connection, owner_scope, active->run_id(), true);
            if (!source) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
            const auto graph_migration = publication.run_generation->graph_migration_receipt();
            const auto source_contexts = load_context_publication_history(
                impl_->connection, owner_scope, active->run_id());
            const auto source_hooks = load_hook_outbox_heads(
                impl_->connection, owner_scope, active->run_id(), source->run_record);
            bool valid_successor = is_valid_program_runtime_state_transfer(
                publication.run_generation->runtime_state_transfer_receipt(), *active,
                *current_lineage, source->run_record, source_contexts, source_hooks,
                *publication.run_generation, publication.run_record,
                publication.context_publication);
            if (valid_successor && graph_migration) {
                const auto command = load_latest_javascript_command(
                    impl_->connection, owner_scope, active->run_id());
                const auto durable_capsule = load_graph_migration_capsule_record(
                    impl_->connection, owner_scope, active->run_id(), current_lineage->id());
                valid_successor = durable_capsule && next_lease && !command &&
                    publication.commands.empty() && publication.effects.empty() &&
                    publication.events.size() == 1 &&
                    does_program_graph_migration_started_event_bind(
                        publication.events.front(), publication.run_record) &&
                    publication.run_record.event_sequence() == 1 && publication.migration_plan &&
                    is_valid_program_graph_migration_transition(
                        *active, *current_lineage, source->run_record, *durable_capsule,
                        *publication.migration_plan, *publication.run_generation,
                        *publication.run_lineage, publication.run_record);
            } else if (valid_successor) {
                const auto checkpoint = load_latest_javascript_command(
                    impl_->connection, owner_scope, active->run_id());
                valid_successor = checkpoint && is_valid_program_replacement_transition(
                    *active, *current_lineage, source->run_record, *checkpoint,
                    *publication.run_generation, *publication.run_lineage,
                    publication.run_record);
            }
            if (!valid_successor) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        if (publication.run_generation && current_lineage &&
            load_generation_record(impl_->connection, owner_scope,
                                   publication.run_lineage->lineage_id(),
                                   publication.run_generation->generation())) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    std::optional<ProgramRunLineage> current_fork_source;
    if (publication.fork_source_lineage) {
        current_fork_source = load_current_lineage_head(
            impl_->connection, owner_scope, publication.fork_source_lineage->lineage_id(), true);
        if (!current_fork_source || !is_valid_program_run_lineage_transition(
                                        *current_fork_source,
                                        *publication.fork_source_lineage) ||
            !fork_allocation_fits(*current_fork_source, *publication.fork_source_lineage,
                                  publication.journal_record.remaining_budget)) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
        const auto active = load_generation_record(
            impl_->connection, owner_scope, current_fork_source->lineage_id(),
            current_fork_source->active_generation());
        const auto source = active
            ? load_head(impl_->connection, owner_scope, active->run_id(), true)
            : std::nullopt;
        const auto source_contexts = active
            ? load_context_publication_history(impl_->connection, owner_scope, active->run_id())
            : std::vector<ProgramContextPublication>{};
        if (!active || !source || has_blocking_hook_obligation(load_hook_outbox_heads(
                                      impl_->connection, owner_scope, active->run_id(),
                                      source->run_record)) ||
            !is_valid_program_runtime_context_clone(
                source_contexts, publication.run_record, publication.context_publication) ||
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
        update_head(impl_->connection, owner_scope, publication.run_record,
                    publication.journal_record, migration_plan, publication_bytes);
    else
        insert_head(impl_->connection, owner_scope, publication.run_record,
                    publication.journal_record, migration_plan, publication_bytes);
    append_events(impl_->connection, owner_scope, run_id, publication.events);
    append_effects(impl_->connection, owner_scope, run_id, publication.effects);
    append_javascript_commands(impl_->connection, owner_scope, run_id, publication.commands);
    append_context_publication(impl_->connection, owner_scope, run_id,
                               publication.context_publication);
    append_hook_outbox_entries(impl_->connection, owner_scope, run_id,
                               publication.hook_outbox_entries);
    if (publication.run_lineage) {
        if (current_lineage)
            update_lineage_head(impl_->connection, *publication.run_lineage);
        else
            insert_lineage_head(impl_->connection, *publication.run_lineage);
        insert_lineage_history(impl_->connection, *publication.run_lineage);
        if (publication.run_generation) {
            insert_generation(impl_->connection, *publication.run_generation);
            insert_generation_initial_publication(
                impl_->connection, *publication.run_generation, publication_bytes);
        }
        if (!run_lineage_id)
            insert_run_lineage(impl_->connection, owner_scope, run_id,
                               publication.run_lineage->lineage_id());
    }
    if (publication.fork_source_lineage) {
        update_lineage_head(impl_->connection, *publication.fork_source_lineage);
        insert_lineage_history(impl_->connection, *publication.fork_source_lineage);
    }
    if (safe_point_capsule) insert_graph_migration_capsule(impl_->connection, *safe_point_capsule);
    if (next_lease)
        insert_execution_lease(impl_->connection, *next_lease);
    else if (expected_lease)
        delete_execution_lease(impl_->connection, *expected_lease);
    transaction.commit();
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
