#include <neograph/program/postgres_transition_store.h>

#include "canonical_json.h"
#include "node_retry_budget.h"
#include "command_publication_head.h"
#include "run_record_cache.h"
#include "transition_validator.h"

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

using detail::transition::Borrowed;

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

ProgramRunRecord parse_run_record(detail::RunRecordCache* cache,
                                  std::string_view        owner_scope,
                                  std::string_view        run_id,
                                  std::string_view        stored_bytes) {
    if (cache) return cache->parse(owner_scope, run_id, stored_bytes);
    auto parsed = ProgramRunRecord::parse(stored_bytes);
    if (parsed.owner_scope() != owner_scope || parsed.run_id() != run_id) {
        throw std::invalid_argument("Stored Program run record key binding is corrupt");
    }
    return parsed;
}

std::optional<StoredHead> load_head(PGconn*                 connection,
                                    std::string_view        owner_scope,
                                    std::string_view        run_id,
                                    bool                    lock  = false,
                                    detail::RunRecordCache* cache = nullptr) {
    auto result = exec_params(
        connection,
        std::string("SELECT run_record_bytes, journal_record_bytes, migration_plan_bytes, "
                    "last_publication_bytes FROM neograph_program_transition_run_heads_v2 "
                    "WHERE owner_scope = $1 AND run_id = $2") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) {
        if (cache) cache->erase(owner_scope, run_id);
        return std::nullopt;
    }
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 4)
        throw std::invalid_argument("Stored PostgreSQL Program transition head shape is invalid");
    auto migration = row_optional_text(result, 0, 2);
    return StoredHead{
        parse_run_record(cache, owner_scope, run_id, row_text(result, 0, 0)),
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
        "SELECT sequence, coordinate_id, canonical_bytes FROM "
        "neograph_program_transition_javascript_command_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence DESC LIMIT 1",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 3)
        throw std::invalid_argument("Stored PostgreSQL JavaScript command shape is invalid");
    const auto sequence = parse_u64(row_text(result, 0, 0), "command sequence");
    auto entry = ProgramJavaScriptCommandJournalEntry::parse(row_text(result, 0, 2));
    if (entry.sequence() != sequence || entry.coordinate_id() != row_text(result, 0, 1))
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
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    detail::RunRecordCache* cache = nullptr) {
    const auto head = load_head(connection, owner_scope, run_id, false, cache);
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

std::vector<ProgramChildSynthesisRecord> load_synthesis_heads(PGconn*          connection,
                                                              std::string_view owner_scope,
                                                              std::string_view run_id) {
    std::vector<ProgramChildSynthesisRecord> heads;
    auto                                     rows = exec_params(connection,
                                                                "SELECT proposal_id, head_id, canonical_bytes FROM "
                                                                                                    "neograph_program_transition_synthesis_log_v1 "
                                                                                                    "WHERE owner_scope=$1 AND run_id=$2 ORDER BY sequence ASC",
                                                                {std::string(owner_scope), std::string(run_id)});
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        auto entry = ProgramChildSynthesisRecord::parse(row_text(rows, row, 2));
        if (entry.data().proposal.id() != row_text(rows, row, 0) ||
            entry.id() != row_text(rows, row, 1))
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

std::vector<ProgramJavaScriptCommandJournalEntry> load_all_javascript_commands(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    detail::RunRecordCache* cache = nullptr) {
    auto rows = exec_params(
        connection,
        "SELECT sequence, coordinate_id, canonical_bytes FROM "
        "neograph_program_transition_javascript_command_log_v2 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        const auto sequence = parse_u64(row_text(rows, row, 0), "command sequence");
        auto entry = ProgramJavaScriptCommandJournalEntry::parse(row_text(rows, row, 2));
        if (entry.sequence() != sequence || entry.coordinate_id() != row_text(rows, row, 1))
            throw std::invalid_argument("Stored PostgreSQL JavaScript command is corrupt");
        result.push_back(std::move(entry));
    }
    if (!result.empty()) {
        const auto head = load_head(connection, owner_scope, run_id, false, cache);
        if (!head ||
            !detail::transition::valid_command_history_append(head->run_record, {}, result))
            throw std::invalid_argument("Stored PostgreSQL JavaScript command history is corrupt");
    }
    return result;
}

// The stored state as the transition validator reads it, inside the caller's transaction,
// which already holds the advisory lock of the owner scope. A read the validator marks
// Lock::Update is a FOR UPDATE read.
class PostgresReads final : public detail::transition::TransitionReads {
public:
    PostgresReads(PGconn* connection, std::string_view owner_scope, detail::RunRecordCache* cache)
        : connection_(connection), owner_scope_(owner_scope), cache_(cache) {}

    std::optional<GraphMigrationCapsule> graph_migration_capsule(
        std::string_view run_id, std::string_view lineage_head_id) override {
        return load_graph_migration_capsule_record(connection_, owner_scope_, run_id,
                                                   lineage_head_id);
    }
    bool effect_id_published(std::string_view run_id, std::string_view effect_id) override {
        auto result = exec_params(
            connection_,
            "SELECT 1 FROM neograph_program_transition_effect_log_v2 "
            "WHERE owner_scope = $1 AND run_id = $2 AND effect_id = $3 LIMIT 1",
            {std::string(owner_scope_), std::string(run_id), std::string(effect_id)});
        return PQntuples(result.get()) != 0;
    }
    Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>> command_log(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>>(
            load_all_javascript_commands(connection_, owner_scope_, run_id, cache_));
    }
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(
        std::string_view run_id) override {
        return load_latest_javascript_command(connection_, owner_scope_, run_id);
    }
    std::optional<ProgramRunRecord> run_record(std::string_view run_id, Lock lock) override {
        auto head = load_head(connection_, owner_scope_, run_id, lock == Lock::Update, cache_);
        if (!head) return std::nullopt;
        return std::move(head->run_record);
    }
    std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id,
                                                  Lock             lock) override {
        return load_current_lineage_head(connection_, owner_scope_, lineage_id,
                                         lock == Lock::Update);
    }
    std::optional<ProgramRunGeneration> generation(std::string_view lineage_id,
                                                   std::uint64_t    ordinal) override {
        return load_generation_record(connection_, owner_scope_, lineage_id, ordinal);
    }
    std::optional<std::string> run_lineage_id(std::string_view run_id) override {
        return load_run_lineage_id(connection_, owner_scope_, run_id);
    }
    Borrowed<std::vector<ProgramContextPublication>> context_history(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramContextPublication>>(
            load_context_publication_history(connection_, owner_scope_, run_id, cache_));
    }
    bool context_history_is_intact(const std::vector<ProgramContextPublication>& history) override {
        return valid_context_history(history);
    }
    Borrowed<std::vector<ProgramChildSynthesisRecord>> synthesis_heads(
        std::string_view run_id) override {
        return Borrowed<std::vector<ProgramChildSynthesisRecord>>(
            load_synthesis_heads(connection_, owner_scope_, run_id));
    }
    Borrowed<std::vector<HookOutboxEntry>> hook_heads(std::string_view       run_id,
                                                      const ProgramRunRecord& run) override {
        return Borrowed<std::vector<HookOutboxEntry>>(
            load_hook_outbox_heads(connection_, owner_scope_, run_id, run));
    }

private:
    PGconn*                 connection_;
    std::string_view        owner_scope_;
    detail::RunRecordCache* cache_;
};


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
                 std::string_view run_id_view,
                 const std::string& run_bytes,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    const auto journal_bytes = journal_record.serialize_canonical();
    const auto migration_bytes = migration_plan
        ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt;
    const std::string owner(owner_scope);
    const std::string run_id(run_id_view);
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
                 std::string_view run_id_view,
                 const std::string& run_bytes,
                 const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan,
                 std::string_view last_publication_bytes) {
    const auto journal_bytes = journal_record.serialize_canonical();
    const auto migration_bytes = migration_plan
        ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt;
    const std::string owner(owner_scope);
    const std::string run_id(run_id_view);
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

void append_synthesis_records(PGconn*                                         connection,
                              std::string_view                                owner_scope,
                              std::string_view                                run_id,
                              const std::vector<ProgramChildSynthesisRecord>& entries) {
    for (const auto& entry : entries)
        (void)exec_params(connection,
                          "INSERT INTO "
                          "neograph_program_transition_synthesis_log_v1(owner_scope,run_id,"
                          "proposal_id,head_id,canonical_bytes) VALUES($1,$2,$3,$4,$5)",
                          {std::string(owner_scope), std::string(run_id),
                           entry.data().proposal.id(), entry.id(), entry.serialize_canonical()});
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
CREATE TABLE IF NOT EXISTS neograph_program_transition_synthesis_log_v1 (
    sequence BIGSERIAL PRIMARY KEY, owner_scope TEXT NOT NULL, run_id TEXT NOT NULL,
    proposal_id TEXT NOT NULL, head_id TEXT NOT NULL, canonical_bytes TEXT NOT NULL,
    UNIQUE(owner_scope, run_id, head_id), FOREIGN KEY(owner_scope, run_id)
        REFERENCES neograph_program_transition_run_heads_v2(owner_scope, run_id) ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS neograph_program_transition_synthesis_run_v1
    ON neograph_program_transition_synthesis_log_v1(owner_scope, run_id, sequence);
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
    Impl(std::string value, Options options)
        : connection_string(std::move(value)),
          run_record_cache(options.run_record_cache_capacity) {}
    ~Impl() {
        if (connection) PQfinish(connection);
    }
    std::string                    connection_string;
    std::string                    coordination_key;
    PGconn*                        connection = nullptr;
    mutable detail::RunRecordCache run_record_cache;
    mutable std::mutex             mutex;
};

PostgreSQLProgramTransitionStore::PostgreSQLProgramTransitionStore(std::string connection_string)
    : PostgreSQLProgramTransitionStore(std::move(connection_string), Options{}) {}

PostgreSQLProgramTransitionStore::PostgreSQLProgramTransitionStore(std::string connection_string,
                                                                   Options     options)
    : impl_(std::make_unique<Impl>(std::move(connection_string), options)) {
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

std::optional<ProgramRunPublicationHead>
PostgreSQLProgramTransitionStore::load_run_publication_head_sync(std::string_view owner,
                                                           std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    // One statement observes the run record and journal head under one MVCC snapshot. Unlike
    // load() followed by latest() it reads once and parses no migration plan or publication bytes.
    auto rows = exec_params(impl_->connection,
        "SELECT run_record_bytes, journal_record_bytes FROM neograph_program_transition_run_heads_v2 "
        "WHERE owner_scope = $1 AND run_id = $2", {std::string(owner), std::string(run_id)});
    if (PQntuples(rows.get()) == 0) {
        impl_->run_record_cache.erase(owner, run_id);
        return std::nullopt;
    }
    if (PQntuples(rows.get()) != 1 || PQnfields(rows.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL run publication head shape is invalid");
    ProgramRunPublicationHead head{
        impl_->run_record_cache.parse(owner, run_id, row_text(rows, 0, 0)),
        ProgramJournalRecord::parse(row_text(rows, 0, 1))};
    detail::validate_run_publication_head(head.run_record, head.journal_record, owner, run_id);
    return head;
}

std::optional<ProgramCommandPublicationHead>
PostgreSQLProgramTransitionStore::load_command_publication_head_sync(std::string_view owner,
                                                               std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    // One statement observes the head and tail under the same MVCC snapshot.
    auto rows = exec_params(impl_->connection,
        "SELECT h.run_record_bytes, h.journal_record_bytes, c.sequence, c.coordinate_id, "
        "c.canonical_bytes FROM neograph_program_transition_run_heads_v2 h "
        "LEFT JOIN neograph_program_transition_javascript_command_log_v2 c "
        "ON c.owner_scope = h.owner_scope AND c.run_id = h.run_id AND c.sequence = "
        "(SELECT MAX(t.sequence) FROM neograph_program_transition_javascript_command_log_v2 t "
        "WHERE t.owner_scope = h.owner_scope AND t.run_id = h.run_id) "
        "WHERE h.owner_scope = $1 AND h.run_id = $2", {std::string(owner), std::string(run_id)});
    if (PQntuples(rows.get()) == 0) {
        impl_->run_record_cache.erase(owner, run_id);
        return std::nullopt;
    }
    if (PQntuples(rows.get()) != 1 || PQnfields(rows.get()) != 5)
        throw std::invalid_argument("Stored PostgreSQL command publication head shape is invalid");
    ProgramCommandPublicationHead head{
        impl_->run_record_cache.parse(owner, run_id, row_text(rows, 0, 0)),
        ProgramJournalRecord::parse(row_text(rows, 0, 1)), std::nullopt};
    if (!PQgetisnull(rows.get(), 0, 2)) {
        head.latest_command = ProgramJavaScriptCommandJournalEntry::parse(row_text(rows, 0, 4));
        if (head.latest_command->sequence() != parse_u64(row_text(rows, 0, 2), "command sequence") ||
            head.latest_command->coordinate_id() != row_text(rows, 0, 3)) {
            throw std::invalid_argument("Stored PostgreSQL command publication head is corrupt");
        }
    }
    detail::validate_command_publication_head(head, owner, run_id);
    return head;
}

std::optional<ProgramRunRecord> PostgreSQLProgramTransitionStore::load_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id, false, &impl_->run_record_cache);
    return head ? std::optional<ProgramRunRecord>(head->run_record) : std::nullopt;
}

std::optional<ProgramJournalRecord> PostgreSQLProgramTransitionStore::latest_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id, false, &impl_->run_record_cache);
    return head ? std::optional<ProgramJournalRecord>(head->journal_record) : std::nullopt;
}

std::vector<ProgramEvent> PostgreSQLProgramTransitionStore::load_events_sync(
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

std::vector<ProgramEffectOutboxEntry> PostgreSQLProgramTransitionStore::load_effects_sync(
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
PostgreSQLProgramTransitionStore::load_javascript_commands_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    std::lock_guard lock(impl_->mutex);
    const auto all = load_all_javascript_commands(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
    std::vector<ProgramJavaScriptCommandJournalEntry> result;
    for (const auto& entry : all) {
        if (entry.sequence() > after_sequence) result.push_back(entry);
    }
    return result;
}

std::vector<ProgramContextPublication>
PostgreSQLProgramTransitionStore::load_context_publications_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto all = load_context_publication_history(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
    if (!valid_context_history(all))
        throw std::invalid_argument("Stored PostgreSQL context history is not contiguous");
    std::vector<ProgramContextPublication> result;
    for (const auto& context : all) {
        if (context.epoch.sequence() > after_sequence) result.push_back(context);
    }
    return result;
}

std::vector<ProgramChildSynthesisRecord> PostgreSQLProgramTransitionStore::load_child_syntheses_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    if (!load_head(impl_->connection, owner_scope, run_id, false, &impl_->run_record_cache)) return {};
    return load_synthesis_heads(impl_->connection, owner_scope, run_id);
}

std::vector<HookOutboxEntry> PostgreSQLProgramTransitionStore::load_hook_outbox_entries_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id, false, &impl_->run_record_cache);
    if (!head) return {};
    auto result = load_hook_outbox_heads(impl_->connection, owner_scope, run_id, head->run_record);
    if (!is_valid_program_hook_history_append({}, result, head->run_record))
        throw std::invalid_argument("Stored PostgreSQL hook history is corrupt");
    return result;
}

std::optional<MigrationPlan> PostgreSQLProgramTransitionStore::load_migration_plan_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    const auto head = load_head(impl_->connection, owner_scope, run_id, false, &impl_->run_record_cache);
    return head ? head->migration_plan : std::nullopt;
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage_sync(
    std::string_view owner_scope, std::string_view lineage_id) const {
    if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_current_lineage_head(impl_->connection, owner_scope, lineage_id);
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_run_lineage_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto lineage_id = load_run_lineage_id(impl_->connection, owner_scope, run_id);
    if (!lineage_id) return std::nullopt;
    const auto lineage = load_current_lineage_head(impl_->connection, owner_scope, *lineage_id);
    if (!lineage) throw std::invalid_argument("Stored PostgreSQL run-lineage binding is corrupt");
    return lineage;
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage_head_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const {
    if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_historical_lineage_head(impl_->connection, owner_scope, lineage_id, head_id);
}

std::optional<ProgramRunGeneration> PostgreSQLProgramTransitionStore::load_generation_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_record(impl_->connection, owner_scope, lineage_id, generation);
}

std::optional<ProgramTransitionPublication>
PostgreSQLProgramTransitionStore::load_generation_initial_publication_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_generation_initial_publication_record(
        impl_->connection, owner_scope, lineage_id, generation);
}

std::optional<GraphMigrationCapsule>
PostgreSQLProgramTransitionStore::load_graph_migration_capsule_sync(
    std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) const {
    if (owner_scope.empty() || source_run_id.empty() || source_lineage_head_id.empty())
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_graph_migration_capsule_record(
        impl_->connection, owner_scope, source_run_id, source_lineage_head_id);
}

std::optional<ProgramExecutionLease> PostgreSQLProgramTransitionStore::load_execution_lease_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    return load_execution_lease_record(impl_->connection, owner_scope, run_id);
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    std::uint64_t expected_fence,
    ProgramTransitionPublication publication) {
    return compare_publish_impl(owner_scope, expected_journal_head, expected_fence,
                                std::move(publication), nullptr, nullptr, nullptr, nullptr);
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish_execution_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    std::uint64_t expected_fence,
    ProgramTransitionPublication publication,
    std::optional<ProgramExecutionLease> expected_lease,
    std::optional<ProgramExecutionLease> next_lease) {
    return compare_publish_impl(owner_scope, expected_journal_head, expected_fence,
                                std::move(publication), nullptr, nullptr,
                                expected_lease ? &*expected_lease : nullptr,
                                next_lease ? &*next_lease : nullptr);
}

ProgramTransitionPublishResult
PostgreSQLProgramTransitionStore::compare_publish_graph_safe_point_sync(
    std::string_view owner_scope, std::string_view expected_journal_head,
    std::uint64_t expected_fence,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence& evidence,
    const GraphMigrationCapsule& capsule,
    const ProgramExecutionLease& execution_lease) {
    return compare_publish_impl(owner_scope, expected_journal_head, expected_fence,
                                std::move(publication), &evidence, &capsule, &execution_lease,
                                nullptr);
}

ProgramTransitionPublishResult
PostgreSQLProgramTransitionStore::compare_publish_takeover_sync(
    std::string_view owner_scope,
    std::string_view run_id,
    std::string_view expected_journal_head,
    std::uint64_t expected_fence) {
    if (!detail::transition::valid_takeover_request(owner_scope, run_id, expected_journal_head,
                                                    expected_fence))
        return ProgramTransitionPublishResult::Conflict;
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    lock_owner(impl_->connection, owner_scope);
    const auto current =
        load_head(impl_->connection, owner_scope, run_id, true, &impl_->run_record_cache);
    if (!current) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    std::optional<ProgramRunLineage> current_lineage;
    if (const auto lineage_id = load_run_lineage_id(impl_->connection, owner_scope, run_id)) {
        current_lineage =
            load_current_lineage_head(impl_->connection, owner_scope, *lineage_id, true);
        if (!current_lineage) {
            transaction.commit();
            return ProgramTransitionPublishResult::Conflict;
        }
    }
    auto takeover = detail::transition::prepare_takeover(
        owner_scope, run_id, expected_journal_head, expected_fence, current->run_record,
        current->journal_record, current_lineage);
    if (!takeover) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    const auto run_record_bytes = takeover->run_record.serialize_canonical();
    update_head(impl_->connection, owner_scope, run_id, run_record_bytes,
                takeover->journal_record, current->migration_plan, "");
    if (takeover->run_lineage) {
        update_lineage_head(impl_->connection, *takeover->run_lineage);
        insert_lineage_history(impl_->connection, *takeover->run_lineage);
    }
    transaction.commit();
    impl_->run_record_cache.erase(owner_scope, run_id);
    return ProgramTransitionPublishResult::Published;
}

ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::compare_publish_impl(
    std::string_view owner_scope, std::string_view expected_journal_head,
    std::uint64_t expected_fence,
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
    Transaction transaction(impl_->connection);
    lock_owner(impl_->connection, owner_scope);
    const auto current = load_head(impl_->connection, owner_scope, run_id, true, &impl_->run_record_cache);
    const auto current_lease = load_execution_lease_record(
        impl_->connection, owner_scope, run_id, true);

    std::optional<detail::transition::CurrentHead> head;
    if (current)
        head.emplace(detail::transition::CurrentHead{current->run_record, current->journal_record,
                                                     current->migration_plan,
                                                     current->last_publication_bytes});
    PostgresReads                      reads(impl_->connection, owner_scope, &impl_->run_record_cache);
    detail::transition::TransitionPlan plan;
    switch (detail::transition::validate_transition(
        {owner_scope, expected_journal_head, expected_fence, publication, *publication_bytes,
         safe_point_evidence, safe_point_capsule, expected_lease, next_lease},
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

    const auto  run_record_bytes = publication.run_record.serialize_canonical();
    const auto& migration_plan   = current ? current->migration_plan : publication.migration_plan;
    if (current)
        update_head(impl_->connection, owner_scope, run_id, run_record_bytes,
                    publication.journal_record, migration_plan, *publication_bytes);
    else
        insert_head(impl_->connection, owner_scope, run_id, run_record_bytes,
                    publication.journal_record, migration_plan, *publication_bytes);
    append_events(impl_->connection, owner_scope, run_id, publication.events);
    append_effects(impl_->connection, owner_scope, run_id, publication.effects);
    append_javascript_commands(impl_->connection, owner_scope, run_id, publication.commands);
    append_context_publication(impl_->connection, owner_scope, run_id,
                               publication.context_publication);
    append_synthesis_records(impl_->connection, owner_scope, run_id,
                             publication.child_synthesis_records);
    append_hook_outbox_entries(impl_->connection, owner_scope, run_id,
                               publication.hook_outbox_entries);
    if (publication.run_lineage) {
        if (plan.lineage_head_exists)
            update_lineage_head(impl_->connection, *publication.run_lineage);
        else
            insert_lineage_head(impl_->connection, *publication.run_lineage);
        insert_lineage_history(impl_->connection, *publication.run_lineage);
        if (publication.run_generation) {
            insert_generation(impl_->connection, *publication.run_generation);
            insert_generation_initial_publication(
                impl_->connection, *publication.run_generation, *publication_bytes);
        }
        if (!plan.run_lineage_recorded)
            insert_run_lineage(impl_->connection, owner_scope, run_id,
                               publication.run_lineage->lineage_id());
    }
    if (publication.fork_source_lineage) {
        update_lineage_head(impl_->connection, *publication.fork_source_lineage);
        insert_lineage_history(impl_->connection, *publication.fork_source_lineage);
    }
    if (publication.recorded_replay_source_lineage) {
        update_lineage_head(impl_->connection, *publication.recorded_replay_source_lineage);
        insert_lineage_history(impl_->connection, *publication.recorded_replay_source_lineage);
    }
    if (safe_point_capsule) insert_graph_migration_capsule(impl_->connection, *safe_point_capsule);
    if (next_lease)
        insert_execution_lease(impl_->connection, *next_lease);
    else if (expected_lease)
        delete_execution_lease(impl_->connection, *expected_lease);
    transaction.commit();
    // The published record object may carry state that its bytes do not (a live provider cause): drop the
    // entry, and let the next read cache what the stored bytes parse to.
    impl_->run_record_cache.erase(owner_scope, run_id);
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
