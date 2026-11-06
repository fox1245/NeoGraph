#include <neograph/program/postgres_transition_store.h>

#include "canonical_json.h"
#include "node_retry_budget.h"
#include "command_publication_head.h"
#include "own_head_hints.h"
#include "publication_plan.h"
#include "run_record_cache.h"
#include "transition_validator.h"

#include <libpq-fe.h>

#include "postgres_pipeline.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>
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

// The statements the calling thread has sent on the connection it holds, counted where they are sent. Set while the
// thread holds a store's connection lock (ConnectionLock below).
inline std::atomic<std::uint64_t>*& statement_counter() noexcept {
    thread_local std::atomic<std::uint64_t>* counter = nullptr;
    return counter;
}
inline void note_statement() noexcept {
    if (auto* counter = statement_counter()) counter->fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void throw_pg(PGconn* connection, std::string_view operation) {
    std::string message(operation);
    message += ": ";
    if (connection) {
        const auto* detail = PQerrorMessage(connection);
        if (detail && *detail) message += detail;
    }
    throw std::runtime_error(std::move(message));
}

// A statement the server refused. `sqlstate` is the code the server reported (empty when it reported none).
class PgError final : public std::runtime_error {
public:
    PgError(const std::string& message, ExecStatusType status, std::string sqlstate, std::string constraint = {})
        : std::runtime_error(message),
          status_(status),
          sqlstate_(std::move(sqlstate)),
          constraint_(std::move(constraint)) {}
    ExecStatusType status() const noexcept { return status_; }
    const std::string& sqlstate() const noexcept { return sqlstate_; }
    /// The constraint the statement violated (empty when the error names none).
    const std::string& constraint() const noexcept { return constraint_; }

private:
    ExecStatusType status_;
    std::string    sqlstate_;
    std::string    constraint_;
};

// A compare-and-swap statement that changed no row (or more than one) inserts a row into an always-empty table whose
// check constraint refuses it: the server aborts the transaction, so a COMMIT sent after the statement in the same
// pipeline is skipped and nothing of the publication is committed.
constexpr const char* CAS_GUARD_TABLE      = "neograph_program_transition_cas_guard_v1";
constexpr const char* CAS_GUARD_CONSTRAINT = "neograph_program_transition_cas_miss";

// SQLSTATE of a unique violation: another writer inserted the row a statement inserts.
constexpr const char* UNIQUE_VIOLATION = "23505";

// Whether `error` is a statement error answered by the server on a connection that is still open.
bool is_server_statement_error(PGconn* connection, const PgError& error) noexcept {
    detail::consume_available_input(connection);
    return PQstatus(connection) == CONNECTION_OK && error.status() == PGRES_FATAL_ERROR && !error.sqlstate().empty();
}

void check_result(PGconn* connection, const PgResult& result, std::string_view operation) {
    if (!result.get()) throw_pg(connection, operation);
    const auto status = PQresultStatus(result.get());
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) return;
    std::string message(operation);
    message += ": ";
    const auto* detail = PQresultErrorMessage(result.get());
    if (detail && *detail) message += detail;
    const auto* sqlstate   = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
    const auto* constraint = PQresultErrorField(result.get(), PG_DIAG_CONSTRAINT_NAME);
    throw PgError(message, status, sqlstate ? sqlstate : "", constraint ? constraint : "");
}

PgResult exec_sql(PGconn* connection, std::string_view sql) {
    note_statement();
    PgResult result(PQexec(connection, std::string(sql).c_str()));
    check_result(connection, result, "PostgreSQL transition command");
    return result;
}

PgResult exec_params(PGconn* connection, std::string_view sql,
                     const std::vector<std::string>& params) {
    note_statement();
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) values.push_back(param.c_str());
    PgResult result(PQexecParams(connection, std::string(sql).c_str(),
                                 static_cast<int>(values.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0));
    check_result(connection, result, "PostgreSQL transition parameterized command");
    return result;
}

// One statement and its parameters (an absent parameter is SQL NULL). A statement of a write phase may state what the
// server must answer for it to count as done.
struct SqlStatement {
    std::string                             sql;
    std::vector<std::optional<std::string>> params;
    /// Not empty: the statement is a compare-and-swap that carries the guard of cas_guarded(); this is the error a
    /// miss is reported as.
    std::string cas_failure;
    /// Not empty: the command tag the server must answer (the tag of a COMMIT that did not roll back is "COMMIT").
    std::string command_tag;
};

// `target` is a data-modifying statement with RETURNING; the result is the statement that runs it and refuses, on the
// server, to go on unless it changed exactly one row.
std::string cas_guarded(std::string_view target) {
    return "WITH changed AS (" + std::string(target) + " RETURNING 1) INSERT INTO " + CAS_GUARD_TABLE +
           "(ok) SELECT false WHERE (SELECT count(*) FROM changed) <> 1";
}

// The answer of a statement that was sent is as the statement says it must be.
void verify_statement_result(PGconn* connection, const SqlStatement& statement, const PgResult& result) {
    try {
        check_result(connection, result, "PostgreSQL transition statement");
    } catch (const PgError& error) {
        if (!statement.cas_failure.empty() && error.constraint() == CAS_GUARD_CONSTRAINT)
            throw PgError(statement.cas_failure, error.status(), error.sqlstate(), error.constraint());
        throw;
    }
    if (!statement.command_tag.empty()) {
        const auto* tag = PQcmdStatus(result.get());
        if (!tag || statement.command_tag != tag)
            throw std::runtime_error("PostgreSQL transition statement did not complete as " + statement.command_tag);
    }
}

// The parameters as libpq takes them: a null pointer is NULL.
std::vector<const char*> statement_values(const SqlStatement& statement) {
    std::vector<const char*> values;
    values.reserve(statement.params.size());
    for (const auto& param : statement.params) values.push_back(param ? param->c_str() : nullptr);
    return values;
}

PgResult exec_statement(PGconn* connection, const SqlStatement& statement,
                        const std::function<void(int)>* after_send = nullptr, bool fail_send = false) {
    note_statement();
    const auto values = statement_values(statement);
    if ((after_send && *after_send) || fail_send) {
        if (fail_send && !(after_send && *after_send))
            throw_pg(connection, "PostgreSQL transition statement");
        if (PQsendQueryParams(connection, statement.sql.c_str(), static_cast<int>(values.size()), nullptr,
                              values.empty() ? nullptr : values.data(), nullptr, nullptr, 0) != 1)
            throw_pg(connection, "PostgreSQL transition statement");
        while (const int flushed = PQflush(connection)) {
            if (flushed < 0) throw_pg(connection, "PostgreSQL transition statement");
        }
        if (after_send && *after_send) {
            (*after_send)(PQsocket(connection));
            detail::consume_available_input(connection);
            if (PQstatus(connection) == CONNECTION_BAD) throw_pg(connection, "PostgreSQL transition statement");
        }
        if (fail_send) throw_pg(connection, "PostgreSQL transition statement");
        PgResult last(nullptr);
        while (PGresult* raw = PQgetResult(connection)) last = PgResult(raw);
        verify_statement_result(connection, statement, last);
        return last;
    }
    PgResult   result(PQexecParams(connection, statement.sql.c_str(), static_cast<int>(values.size()), nullptr,
                                   values.empty() ? nullptr : values.data(), nullptr, nullptr, 0));
    verify_statement_result(connection, statement, result);
    return result;
}

// Sends `statements` in order and returns their results. One at a time when `pipeline` is false: each statement is
// answered and checked before the next is sent. In pipeline mode all are sent before any answer is read, the first
// statement that fails decides the error (every statement after it was aborted by the server), and a statement's own
// check runs when its answer is read.
std::vector<PgResult> run_statements(PGconn* connection, const std::vector<SqlStatement>& statements,
                                     bool pipeline, detail::PublicationTestHooks* hooks = nullptr,
                                     const std::function<void(int)>* after_send = nullptr,
                                     bool fail_send = false) {
    std::vector<PgResult> results;
    results.reserve(statements.size());
#ifdef LIBPQ_HAS_PIPELINING
    if (pipeline) {
        {
            detail::PgPipeline queue(connection, hooks ? &hooks->fail_pipeline_leave : nullptr);
            for (const auto& statement : statements) {
                note_statement();
                queue.send(statement.sql, statement_values(statement));
            }
            for (auto& result : queue.finish(after_send, fail_send)) results.emplace_back(result.release());
        }
        for (std::size_t index = 0; index < statements.size(); ++index)
            verify_statement_result(connection, statements[index], results[index]);
        return results;
    }
#else
    (void)pipeline;
    (void)hooks;
#endif
    for (std::size_t index = 0; index < statements.size(); ++index) {
        const bool last = index + 1 == statements.size();
        results.push_back(exec_statement(connection, statements[index], last ? after_send : nullptr,
                                         last && fail_send));
    }
    return results;
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

bool is_pipeline_off(PGconn* connection) noexcept {
#ifdef LIBPQ_HAS_PIPELINING
    return PQpipelineStatus(connection) == PQ_PIPELINE_OFF;
#else
    (void)connection;
    return true;
#endif
}

// Connected, idle outside any transaction, out of pipeline mode and blocking: the state every call expects to find.
bool is_clean_idle_connection(PGconn* connection) noexcept {
    return connection && PQstatus(connection) == CONNECTION_OK && PQtransactionStatus(connection) == PQTRANS_IDLE &&
           is_pipeline_off(connection) && PQisnonblocking(connection) == 0;
}

// The server process of a connection's current session, identified by pid and backend_start from pg_stat_activity so
// a reused pid is not mistaken for the same session. Read when a session opens or is reset, without counting as a
// transition statement.
struct SessionIdentity {
    int         pid = 0;
    std::string backend_start;

    bool known() const noexcept { return pid > 0 && !backend_start.empty(); }
};

SessionIdentity read_session_identity(PGconn* connection) noexcept {
    if (!is_clean_idle_connection(connection)) return {};
    const int pid = PQbackendPID(connection);
    if (pid <= 0) return {};
    const auto  pid_text = std::to_string(pid);
    const char* values[] = {pid_text.c_str()};
    PgResult    result(PQexecParams(connection, "SELECT backend_start FROM pg_stat_activity WHERE pid = $1", 1,
                                    nullptr, values, nullptr, nullptr, 0));
    if (!result.get() || PQresultStatus(result.get()) != PGRES_TUPLES_OK || PQntuples(result.get()) != 1 ||
        PQnfields(result.get()) != 1 || PQgetisnull(result.get(), 0, 0))
        return {};
    std::string started(PQgetvalue(result.get(), 0, 0), static_cast<std::size_t>(PQgetlength(result.get(), 0, 0)));
    if (started.empty()) return {};
    return SessionIdentity{pid, std::move(started)};
}

// Starts the session over: the server ends the old session's transaction and releases its locks.
void reset_connection(PGconn* connection, SessionIdentity* identity = nullptr) noexcept {
    if (!connection) {
        if (identity) *identity = {};
        return;
    }
    PQreset(connection);
    (void)PQsetnonblocking(connection, 0);
    if (identity) *identity = read_session_identity(connection);
}

class Transaction final {
public:
    explicit Transaction(PGconn* connection, SessionIdentity* identity = nullptr)
        : connection_(connection), identity_(identity) {
        exec_sql(connection_, "BEGIN");
    }
    /// The BEGIN is the first statement of a batch the caller sends next. The transaction is rolled back on every exit
    /// that did not commit, whether or not the BEGIN reached the server.
    struct BegunByBatch {};
    Transaction(PGconn* connection, BegunByBatch, SessionIdentity* identity = nullptr)
        : connection_(connection), identity_(identity) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    // A connection still in pipeline mode refuses a ROLLBACK, and one that is not idle afterwards would carry its
    // transaction and locks into the store's next call: either is reset instead.
    ~Transaction() {
        if (!committed_) {
            if (!is_pipeline_off(connection_)) {
                reset_connection(connection_, identity_);
                return;
            }
            try {
                exec_sql(connection_, "ROLLBACK");
            } catch (...) {
            }
        }
        if (!is_clean_idle_connection(connection_)) reset_connection(connection_, identity_);
    }
    void commit(const std::function<void(int)>* after_send = nullptr, bool fail_send = false) {
        (void)exec_statement(connection_, {"COMMIT", {}, {}, "COMMIT"}, after_send, fail_send);
        committed_ = true;
    }
    /// A batch whose last statement is COMMIT was answered as a commit.
    void committed_by_batch() noexcept { committed_ = true; }
    /// The session was reset after a lost connection, ending this transaction on the old session.
    void ended_by_reset() noexcept { committed_ = true; }

private:
    PGconn*          connection_ = nullptr;
    SessionIdentity* identity_   = nullptr;
    bool             committed_  = false;
};

SqlStatement lock_owner_statement(std::string_view owner_scope) {
    return {"SELECT pg_advisory_xact_lock(hashtextextended($1, 0))", {std::string(owner_scope)}, {}, {}};
}

void lock_owner(PGconn* connection, std::string_view owner_scope) {
    (void)exec_statement(connection, lock_owner_statement(owner_scope));
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

// One column of a run head, for a caller that needs only that column. An absent row reads as absent and
// drops the parsed-record cache entry, as load_head does.
PgResult select_head_column(PGconn* connection, std::string_view owner_scope, std::string_view run_id,
                            const char* column, detail::RunRecordCache* cache) {
    auto result = exec_params(
        connection,
        std::string("SELECT ") + column +
            " FROM neograph_program_transition_run_heads_v2 WHERE owner_scope = $1 AND run_id = $2",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0 && cache) cache->erase(owner_scope, run_id);
    if (PQntuples(result.get()) > 1 || (PQntuples(result.get()) == 1 && PQnfields(result.get()) != 1))
        throw std::invalid_argument("Stored PostgreSQL Program transition head shape is invalid");
    return result;
}

std::optional<ProgramRunRecord> load_head_run_record(PGconn* connection, std::string_view owner_scope,
                                                     std::string_view run_id,
                                                     detail::RunRecordCache* cache) {
    const auto result = select_head_column(connection, owner_scope, run_id, "run_record_bytes", cache);
    if (PQntuples(result.get()) == 0) return std::nullopt;
    return parse_run_record(cache, owner_scope, run_id, row_text(result, 0, 0));
}

std::optional<ProgramJournalRecord> load_head_journal_record(PGconn* connection,
                                                             std::string_view owner_scope,
                                                             std::string_view run_id,
                                                             detail::RunRecordCache* cache) {
    const auto result = select_head_column(connection, owner_scope, run_id, "journal_record_bytes", cache);
    if (PQntuples(result.get()) == 0) return std::nullopt;
    return ProgramJournalRecord::parse(row_text(result, 0, 0));
}

std::optional<MigrationPlan> load_head_migration_plan(PGconn* connection, std::string_view owner_scope,
                                                      std::string_view run_id,
                                                      detail::RunRecordCache* cache) {
    const auto result = select_head_column(connection, owner_scope, run_id, "migration_plan_bytes", cache);
    if (PQntuples(result.get()) == 0) return std::nullopt;
    const auto migration = row_optional_text(result, 0, 0);
    return migration ? std::optional<MigrationPlan>(MigrationPlan::parse(*migration)) : std::nullopt;
}

ProgramExecutionLease parse_execution_lease(std::string_view owner_scope, std::string_view run_id,
                                            std::string_view lease_id, std::string_view lease_bytes) {
    auto lease = ProgramExecutionLease::parse(lease_bytes);
    if (lease.owner_scope() != owner_scope || lease.run_id() != run_id || lease.id() != lease_id)
        throw std::invalid_argument("Stored PostgreSQL execution lease is corrupt");
    return lease;
}

struct StoredHeadAndLease {
    std::optional<StoredHead>            head;
    std::optional<ProgramExecutionLease> lease;
};

// The run head, locked, and the execution lease of the run, in one statement. Every write of a lease is a
// publication of its run, which holds this head's lock and the owner lock; a lease row references its head, so a
// run without a head has no lease.
StoredHeadAndLease load_head_and_lease_for_update(PGconn* connection, std::string_view owner_scope,
                                                  std::string_view run_id,
                                                  detail::RunRecordCache* cache) {
    auto result = exec_params(
        connection,
        "SELECT h.run_record_bytes, h.journal_record_bytes, h.migration_plan_bytes, "
        "h.last_publication_bytes, l.lease_id, l.lease_bytes "
        "FROM neograph_program_transition_run_heads_v2 h "
        "LEFT JOIN neograph_program_execution_leases_v1 l "
        "ON l.owner_scope = h.owner_scope AND l.run_id = h.run_id "
        "WHERE h.owner_scope = $1 AND h.run_id = $2 FOR UPDATE OF h",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) {
        if (cache) cache->erase(owner_scope, run_id);
        return {};
    }
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 6)
        throw std::invalid_argument("Stored PostgreSQL Program transition head shape is invalid");
    auto migration = row_optional_text(result, 0, 2);
    StoredHeadAndLease loaded;
    loaded.head = StoredHead{
        parse_run_record(cache, owner_scope, run_id, row_text(result, 0, 0)),
        ProgramJournalRecord::parse(row_text(result, 0, 1)),
        migration ? std::optional<MigrationPlan>(MigrationPlan::parse(*migration)) : std::nullopt,
        row_text(result, 0, 3)};
    if (!PQgetisnull(result.get(), 0, 4))
        loaded.lease = parse_execution_lease(owner_scope, run_id, row_text(result, 0, 4),
                                             row_text(result, 0, 5));
    return loaded;
}

ProgramRunLineage parse_lineage_head(std::string_view bytes, std::string_view owner_scope,
                                     std::string_view lineage_id) {
    auto lineage = ProgramRunLineage::parse(bytes);
    if (lineage.owner_scope() != owner_scope || lineage.lineage_id() != lineage_id)
        throw std::invalid_argument("Stored PostgreSQL Program lineage head binding is corrupt");
    return lineage;
}

// The lineage a run belongs to and that lineage's head, in one statement (optionally locking the head). `id` is
// empty when the run belongs to no lineage; `head` is empty when the association names a lineage with no head.
struct RunLineageRead {
    std::optional<std::string>       id;
    std::optional<ProgramRunLineage> head;
};

RunLineageRead load_run_lineage_and_head(PGconn* connection, std::string_view owner_scope,
                                         std::string_view run_id, bool lock) {
    // A row lock cannot be taken on the nullable side of an outer join: the locking form reads the head in a
    // locking common table expression instead.
    auto result = exec_params(
        connection,
        lock ? "WITH r AS (SELECT owner_scope, lineage_id FROM neograph_program_run_lineage_runs_v1 "
               "WHERE owner_scope = $1 AND run_id = $2), "
               "h AS (SELECT lh.lineage_id, lh.head_bytes FROM neograph_program_run_lineage_heads_v1 lh "
               "JOIN r ON lh.owner_scope = r.owner_scope AND lh.lineage_id = r.lineage_id FOR UPDATE OF lh) "
               "SELECT r.lineage_id, h.head_bytes FROM r LEFT JOIN h ON h.lineage_id = r.lineage_id"
             : "SELECT r.lineage_id, h.head_bytes FROM neograph_program_run_lineage_runs_v1 r "
               "LEFT JOIN neograph_program_run_lineage_heads_v1 h "
               "ON h.owner_scope = r.owner_scope AND h.lineage_id = r.lineage_id "
               "WHERE r.owner_scope = $1 AND r.run_id = $2",
        {std::string(owner_scope), std::string(run_id)});
    if (PQntuples(result.get()) == 0) return {};
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 2)
        throw std::invalid_argument("Stored PostgreSQL run-lineage shape is invalid");
    RunLineageRead read;
    read.id = row_text(result, 0, 0);
    if (const auto bytes = row_optional_text(result, 0, 1))
        read.head = parse_lineage_head(*bytes, owner_scope, *read.id);
    return read;
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
    return parse_lineage_head(row_text(result, 0, 0), owner_scope, lineage_id);
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
    std::uint64_t generation, detail::GenerationCache* cache = nullptr) {
    if (cache) {
        if (auto cached = cache->get(owner_scope, lineage_id, generation)) return cached;
    }
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
    if (cache) cache->put(owner_scope, lineage_id, generation, value);
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
    return parse_execution_lease(owner_scope, run_id, row_text(result, 0, 0), row_text(result, 0, 1));
}

// A stored command from the columns of its row: sequence, coordinate id and canonical bytes.
ProgramJavaScriptCommandJournalEntry parse_command_row(std::string_view sequence_text, std::string_view coordinate_id,
                                                       std::string_view bytes) {
    const auto sequence = parse_u64(sequence_text, "command sequence");
    auto       entry    = ProgramJavaScriptCommandJournalEntry::parse(bytes);
    if (entry.sequence() != sequence || entry.coordinate_id() != coordinate_id)
        throw std::invalid_argument("Stored PostgreSQL JavaScript command is corrupt");
    return entry;
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
    return parse_command_row(row_text(result, 0, 0), row_text(result, 0, 1), row_text(result, 0, 2));
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

std::vector<ProgramContextPublication> load_context_publication_log(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    const ProgramRunRecord& run) {
    auto rows = exec_params(
        connection,
        "SELECT sequence, canonical_bytes FROM neograph_program_transition_context_log_v1 "
        "WHERE owner_scope = $1 AND run_id = $2 ORDER BY sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    std::vector<ProgramContextPublication> result;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        const auto sequence = parse_u64(row_text(rows, row, 0), "context sequence");
        auto context = parse_context_publication_bytes(row_text(rows, row, 1), run);
        if (context.epoch.sequence() != sequence)
            throw std::invalid_argument("Stored PostgreSQL context sequence is corrupt");
        result.push_back(std::move(context));
    }
    return result;
}

std::vector<ProgramContextPublication> load_context_publication_history(
    PGconn* connection, std::string_view owner_scope, std::string_view run_id,
    detail::RunRecordCache* cache = nullptr) {
    const auto run = load_head_run_record(connection, owner_scope, run_id, cache);
    if (!run) return {};
    return load_context_publication_log(connection, owner_scope, run_id, *run);
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

std::vector<ProgramChildSynthesisRecord> synthesis_heads_of_rows(const PgResult&   rows,
                                                                 std::string_view owner_scope,
                                                                 std::string_view run_id) {
    std::vector<ProgramChildSynthesisRecord> heads;
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
        if (PQgetisnull(rows.get(), row, 0)) continue;  // the run has a head and no synthesis rows
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

std::vector<ProgramChildSynthesisRecord> load_synthesis_heads(PGconn*          connection,
                                                              std::string_view owner_scope,
                                                              std::string_view run_id) {
    const auto rows = exec_params(connection,
                                  "SELECT proposal_id, head_id, canonical_bytes FROM "
                                  "neograph_program_transition_synthesis_log_v1 "
                                  "WHERE owner_scope=$1 AND run_id=$2 ORDER BY sequence ASC",
                                  {std::string(owner_scope), std::string(run_id)});
    return synthesis_heads_of_rows(rows, owner_scope, run_id);
}

// The synthesis records of a stored run, or none when the run has no head: one statement reads both.
std::vector<ProgramChildSynthesisRecord> load_synthesis_heads_of_stored_run(PGconn*          connection,
                                                                            std::string_view owner_scope,
                                                                            std::string_view run_id) {
    const auto rows = exec_params(
        connection,
        "SELECT l.proposal_id, l.head_id, l.canonical_bytes "
        "FROM neograph_program_transition_run_heads_v2 h "
        "LEFT JOIN neograph_program_transition_synthesis_log_v1 l "
        "ON l.owner_scope = h.owner_scope AND l.run_id = h.run_id "
        "WHERE h.owner_scope = $1 AND h.run_id = $2 ORDER BY l.sequence ASC",
        {std::string(owner_scope), std::string(run_id)});
    return synthesis_heads_of_rows(rows, owner_scope, run_id);
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
        const auto run = load_head_run_record(connection, owner_scope, run_id, cache);
        if (!run || !detail::transition::valid_command_history_append(*run, {}, result))
            throw std::invalid_argument("Stored PostgreSQL JavaScript command history is corrupt");
    }
    return result;
}

// The stored state as the transition validator reads it, inside the caller's transaction,
// which already holds the advisory lock of the owner scope. A read the validator marks
// Lock::Update is a FOR UPDATE read.
ProgramChildRelationHead parse_relation_head(std::string_view owner_scope, std::string_view parent_run_id,
                                             std::string_view bytes) {
    auto head = ProgramChildRelationHead::parse(bytes);
    if (head.data().owner_scope != owner_scope || head.data().parent_run_id != parent_run_id)
        throw std::invalid_argument("Stored PostgreSQL Program child relation head binding is corrupt");
    return head;
}

std::optional<ProgramChildRelationHead> load_relation_head_record(PGconn*          connection,
                                                                   std::string_view owner_scope,
                                                                   std::string_view parent_run_id,
                                                                   bool             lock = false) {
    auto result = exec_params(
        connection,
        std::string("SELECT head_bytes FROM neograph_program_child_relation_heads_v1 "
                    "WHERE owner_scope = $1 AND parent_run_id = $2") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(parent_run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::invalid_argument("Stored PostgreSQL Program child relation head shape is invalid");
    return parse_relation_head(owner_scope, parent_run_id, row_text(result, 0, 0));
}

ProgramChildRelation parse_relation_row(std::string_view owner_scope, std::string_view parent_run_id,
                                        std::string_view bytes) {
    auto row = ProgramChildRelation::parse(bytes);
    if (row.owner_scope() != owner_scope || row.parent_run_id() != parent_run_id)
        throw std::invalid_argument("Stored PostgreSQL Program child relation binding is corrupt");
    return row;
}

std::optional<ProgramChildRelation> load_relation_row_record(PGconn*          connection,
                                                             std::string_view owner_scope,
                                                             std::string_view parent_run_id,
                                                             std::string_view child_run_id,
                                                             bool             lock = false) {
    auto result = exec_params(
        connection,
        std::string("SELECT row_bytes FROM neograph_program_child_relations_v1 "
                    "WHERE owner_scope = $1 AND parent_run_id = $2 AND child_run_id = $3") +
            (lock ? " FOR UPDATE" : ""),
        {std::string(owner_scope), std::string(parent_run_id), std::string(child_run_id)});
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::invalid_argument("Stored PostgreSQL Program child relation shape is invalid");
    auto row = parse_relation_row(owner_scope, parent_run_id, row_text(result, 0, 0));
    if (row.child_run_id() != child_run_id)
        throw std::invalid_argument("Stored PostgreSQL Program child relation binding is corrupt");
    return row;
}

std::vector<ProgramChildRelation> load_relation_rows(PGconn*          connection,
                                                     std::string_view owner_scope,
                                                     std::string_view parent_run_id,
                                                     bool             open_only) {
    auto result = exec_params(
        connection,
        open_only ? "SELECT row_bytes FROM neograph_program_child_relations_v1 "
                    "WHERE owner_scope = $1 AND parent_run_id = $2 AND open ORDER BY ordinal"
                  : "SELECT row_bytes FROM neograph_program_child_relations_v1 "
                    "WHERE owner_scope = $1 AND parent_run_id = $2 ORDER BY ordinal",
        {std::string(owner_scope), std::string(parent_run_id)});
    std::vector<ProgramChildRelation> rows;
    const int count = PQntuples(result.get());
    rows.reserve(static_cast<std::size_t>(count));
    for (int row = 0; row < count; ++row)
        rows.push_back(parse_relation_row(owner_scope, parent_run_id, row_text(result, row, 0)));
    return rows;
}

class PostgresReads : public detail::transition::TransitionReads {
public:
    PostgresReads(PGconn* connection, std::string_view owner_scope, detail::RunRecordCache* cache,
                  detail::GenerationCache* generations = nullptr)
        : connection_(connection), owner_scope_(owner_scope), cache_(cache), generations_(generations) {}
    virtual ~PostgresReads() = default;

    /// The transaction has read (and locked) the head of `run_id`; `run` is its record, or null when the run has
    /// no head. A read of that run's head inside the same transaction answers from it.
    void know_head(std::string_view run_id, const ProgramRunRecord* run) {
        known_run_id_ = std::string(run_id);
        known_run_    = run;
        has_known_    = true;
    }

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
        const auto& head = stored_head(run_id, lock == Lock::Update);
        if (!head) return std::nullopt;
        return head->run_record;
    }
    std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id,
                                                  Lock             lock) override {
        // The head the run's lineage lookup already read, locked, in this transaction.
        if (lineage_memo_ && lineage_memo_->id == lineage_id) return lineage_memo_->head;
        if (lock == Lock::Update) locking_read_issued();
        return load_current_lineage_head(connection_, owner_scope_, lineage_id,
                                         lock == Lock::Update);
    }
    std::optional<ProgramRunGeneration> generation(std::string_view lineage_id,
                                                   std::uint64_t    ordinal) override {
        return load_generation_record(connection_, owner_scope_, lineage_id, ordinal, generations_);
    }
    std::optional<std::string> run_lineage_id(std::string_view run_id) override {
        // The lineage id and its head come from one statement, the head locked: the validator reads
        // the head of the lineage a run is recorded in right after it learns the id.
        locking_read_issued();
        auto read = load_run_lineage_and_head(connection_, owner_scope_, run_id, true);
        lineage_memo_.reset();
        if (read.id) lineage_memo_ = LineageMemo{*read.id, std::move(read.head)};
        return read.id;
    }
    Borrowed<std::vector<ProgramContextPublication>> context_history(
        std::string_view run_id) override {
        if (has_known_ && known_run_id_ == run_id) {
            if (!known_run_) return Borrowed<std::vector<ProgramContextPublication>>(
                std::vector<ProgramContextPublication>{});
            return Borrowed<std::vector<ProgramContextPublication>>(
                load_context_publication_log(connection_, owner_scope_, run_id, *known_run_));
        }
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
    std::optional<ProgramJournalRecord> journal_record(std::string_view run_id, Lock lock) override {
        const auto& head = stored_head(run_id, lock == Lock::Update);
        if (!head) return std::nullopt;
        return head->journal_record;
    }
    std::optional<ProgramChildRelationHead> relation_head(std::string_view parent_run_id,
                                                          Lock             lock) override {
        // Nothing is written while the transaction validates, and a locked row stays as it was read.
        const bool update = lock == Lock::Update;
        if (const auto found = relation_heads_.find(parent_run_id);
            found != relation_heads_.end() && (found->second.locked || !update))
            return found->second.head;
        if (update) locking_read_issued();
        auto head = load_relation_head_record(connection_, owner_scope_, parent_run_id, update);
        relation_heads_.insert_or_assign(std::string(parent_run_id), RelationHeadMemo{update, head});
        return head;
    }
    std::optional<ProgramChildRelation> relation_row(std::string_view parent_run_id,
                                                      std::string_view child_run_id,
                                                      Lock             lock) override {
        if (lock == Lock::Update) locking_read_issued();
        return load_relation_row_record(connection_, owner_scope_, parent_run_id, child_run_id,
                                        lock == Lock::Update);
    }

protected:
    /// A statement that takes a row lock is about to be sent for a row no earlier read of this transaction locked.
    virtual void locking_read_issued() {}

    struct LineageMemo {
        std::string                      id;
        std::optional<ProgramRunLineage> head;
    };
    // What the transaction has read of a run's head, or of a parent's relation head, and whether it holds the row's
    // lock: the validator reads the same row for several of its checks.
    struct HeadMemo {
        bool                      locked = false;
        std::optional<StoredHead> head;
    };
    struct RelationHeadMemo {
        bool                                    locked = false;
        std::optional<ProgramChildRelationHead> head;
    };
    const std::optional<StoredHead>& stored_head(std::string_view run_id, bool lock) {
        auto found = heads_.find(run_id);
        if (found != heads_.end() && (found->second.locked || !lock)) return found->second.head;
        if (lock) locking_read_issued();
        auto head = load_head(connection_, owner_scope_, run_id, lock, cache_);
        if (found == heads_.end())
            found = heads_.emplace(std::string(run_id), HeadMemo{lock, std::move(head)}).first;
        else
            found->second = HeadMemo{lock, std::move(head)};
        return found->second.head;
    }

    PGconn*                 connection_;
    std::string_view        owner_scope_;
    detail::RunRecordCache* cache_;
    detail::GenerationCache* generations_;
    std::optional<LineageMemo> lineage_memo_;
    std::map<std::string, HeadMemo, std::less<>>         heads_;
    std::map<std::string, RelationHeadMemo, std::less<>> relation_heads_;
    std::string             known_run_id_;
    const ProgramRunRecord* known_run_ = nullptr;
    bool                    has_known_ = false;
};

// ---------------------------------------------------------------------------------------------------------------
// Ordered read groups. A publication names, from the typed fields of its request, the rows it will lock
// (publication_plan.h). They are read in at most four statements, always in this order, each statement locking its
// rows in the order of its ORDER BY (byte order, whatever the database collation): lineage heads, run heads with
// their execution lease and lineage association, relation heads, relation rows. What a group returns seeds the memos
// of BatchedPostgresReads; a row a group did not return is absent, a row no group named is read when the validator
// asks, inside the same transaction, as it always was.
// ---------------------------------------------------------------------------------------------------------------

// A list of ids travels as one JSON text parameter, built by the JSON writer: an id may hold any character.
std::string json_text_array(const std::vector<std::string>& values) {
    auto array = json::array();
    for (const auto& value : values) array.push_back(value);
    return array.dump();
}

enum class ReadGroup : std::uint8_t { LineageHeads, RunHeads, RelationHeads, RelationRows };

struct ReadGroupStatement {
    ReadGroup    group;
    SqlStatement statement;
    /// The run group also returns the latest stored command of the publication's own run.
    bool with_latest_command = false;
};

// `latest_command_of` names the run whose latest command the run group returns too, or is empty: a publication that
// appends commands asks for it.
std::vector<ReadGroupStatement> read_group_statements(std::string_view owner_scope,
                                                      const detail::transition::PublicationLockPlan& plan,
                                                      std::string_view latest_command_of) {
    std::vector<ReadGroupStatement> groups;
    if (!plan.lineage_ids.empty())
        groups.push_back({ReadGroup::LineageHeads,
                          {"SELECT lineage_id, head_bytes FROM neograph_program_run_lineage_heads_v1 "
                           "WHERE owner_scope = $1 AND lineage_id = ANY(ARRAY(SELECT "
                           "jsonb_array_elements_text($2::jsonb))) "
                           "ORDER BY lineage_id COLLATE \"C\" FOR UPDATE",
                           {std::string(owner_scope), json_text_array(plan.lineage_ids)}}});
    if (!plan.run_ids.empty()) {
        const bool with_command = !latest_command_of.empty();
        SqlStatement statement{
            std::string("SELECT h.run_id, h.run_record_bytes, h.journal_record_bytes, h.migration_plan_bytes, "
                        "h.last_publication_bytes, l.lease_id, l.lease_bytes, r.lineage_id") +
                (with_command ? ", c.sequence, c.coordinate_id, c.canonical_bytes " : " ") +
                "FROM neograph_program_transition_run_heads_v2 h "
                "LEFT JOIN neograph_program_execution_leases_v1 l "
                "ON l.owner_scope = h.owner_scope AND l.run_id = h.run_id "
                "LEFT JOIN neograph_program_run_lineage_runs_v1 r "
                "ON r.owner_scope = h.owner_scope AND r.run_id = h.run_id " +
                (with_command ? "LEFT JOIN LATERAL (SELECT sequence, coordinate_id, canonical_bytes "
                                "FROM neograph_program_transition_javascript_command_log_v2 "
                                "WHERE owner_scope = h.owner_scope AND run_id = h.run_id AND run_id = $3 "
                                "ORDER BY sequence DESC LIMIT 1) c ON TRUE "
                              : "") +
                "WHERE h.owner_scope = $1 AND h.run_id = ANY(ARRAY(SELECT jsonb_array_elements_text($2::jsonb))) "
                "ORDER BY h.run_id COLLATE \"C\" FOR UPDATE OF h",
            {std::string(owner_scope), json_text_array(plan.run_ids)}, {}, {}};
        if (with_command) statement.params.emplace_back(std::string(latest_command_of));
        groups.push_back({ReadGroup::RunHeads, std::move(statement), with_command});
    }
    if (!plan.relation_parent_ids.empty())
        groups.push_back({ReadGroup::RelationHeads,
                          {"SELECT parent_run_id, head_bytes FROM neograph_program_child_relation_heads_v1 "
                           "WHERE owner_scope = $1 AND parent_run_id = ANY(ARRAY(SELECT "
                           "jsonb_array_elements_text($2::jsonb))) "
                           "ORDER BY parent_run_id COLLATE \"C\" FOR UPDATE",
                           {std::string(owner_scope), json_text_array(plan.relation_parent_ids)}}});
    if (!plan.relation_rows.empty()) {
        auto rows = json::array();
        for (const auto& [parent, child] : plan.relation_rows) rows.push_back(json{{"p", parent}, {"c", child}});
        groups.push_back({ReadGroup::RelationRows,
                          {"SELECT parent_run_id, child_run_id, row_bytes "
                           "FROM neograph_program_child_relations_v1 "
                           "WHERE owner_scope = $1 AND (parent_run_id, child_run_id) IN "
                           "(SELECT e->>'p', e->>'c' FROM jsonb_array_elements($2::jsonb) e) "
                           "ORDER BY parent_run_id COLLATE \"C\", child_run_id COLLATE \"C\" FOR UPDATE",
                           {std::string(owner_scope), rows.dump()}}});
    }
    return groups;
}

// The reads of one transaction, seeded from the groups. Anything not seeded is read by PostgresReads, and a read
// that takes a lock is counted: the plan did not cover that row.
class BatchedPostgresReads final : public PostgresReads {
public:
    BatchedPostgresReads(PGconn* connection, std::string_view owner_scope, detail::RunRecordCache* cache,
                         detail::GenerationCache* generations, std::atomic<std::uint64_t>* late_locks)
        : PostgresReads(connection, owner_scope, cache, generations), late_locks_(late_locks) {}

    // The rows a group returned are parsed when the validator asks for them, as the sequential path reads them: a
    // stored row the validator never reads, or reads after it has decided, is not parsed. The publication's own run is
    // parsed at once, as every path parses the head it compares against.

    /// A lineage head (locked) as stored, or none.
    void seed_lineage_head(std::string lineage_id, std::optional<std::string> head_bytes) {
        lineage_heads_.insert_or_assign(std::move(lineage_id), std::move(head_bytes));
    }
    /// The columns of a stored run head.
    struct RunColumns {
        std::string                run_record;
        std::string                journal_record;
        std::optional<std::string> migration_plan;
        std::string                last_publication;
    };
    /// A run's head (locked) and the lineage it is recorded in. A run with no head has neither.
    void seed_run(std::string run_id, std::optional<RunColumns> columns, std::optional<std::string> lineage_id,
                  std::optional<ProgramExecutionLease> own_lease, bool own) {
        run_lineages_.insert_or_assign(run_id, std::move(lineage_id));
        if (own) {
            leases_.insert_or_assign(run_id, std::move(own_lease));
            heads_.insert_or_assign(run_id, HeadMemo{true, columns ? std::optional<StoredHead>(parse_head(run_id, *columns))
                                                                   : std::nullopt});
        } else if (columns) {
            run_columns_.insert_or_assign(std::move(run_id), std::move(*columns));
        } else {
            heads_.insert_or_assign(std::move(run_id), HeadMemo{true, std::nullopt});
        }
    }
    /// The columns of the latest stored command of `run_id` (sequence, coordinate id, bytes), or none when it has none.
    struct CommandColumns {
        std::string sequence;
        std::string coordinate_id;
        std::string bytes;
    };
    void seed_latest_command(std::string run_id, std::optional<CommandColumns> columns) {
        latest_command_run_ = std::move(run_id);
        latest_command_     = std::move(columns);
    }
    void seed_relation_head(std::string parent_run_id, std::optional<std::string> head_bytes) {
        relation_head_bytes_.insert_or_assign(std::move(parent_run_id), std::move(head_bytes));
    }
    void seed_relation_row(std::string parent_run_id, std::string child_run_id, std::optional<std::string> row_bytes) {
        relation_row_bytes_.insert_or_assign(std::make_pair(std::move(parent_run_id), std::move(child_run_id)),
                                             std::move(row_bytes));
    }

    /// The head and lease the run group read for `run_id`, which the caller named in the plan.
    const std::optional<StoredHead>& head_of(std::string_view run_id) const { return heads_.at(std::string(run_id)).head; }
    const std::optional<ProgramExecutionLease>& lease_of(std::string_view run_id) const {
        return leases_.at(std::string(run_id));
    }

    std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id, Lock lock) override {
        if (const auto found = lineage_heads_.find(lineage_id); found != lineage_heads_.end()) {
            if (!found->second) return std::nullopt;
            return parse_lineage_head(*found->second, owner_scope_, lineage_id);
        }
        return PostgresReads::lineage_head(lineage_id, lock);
    }
    std::optional<ProgramRunRecord> run_record(std::string_view run_id, Lock lock) override {
        materialize_run(run_id);
        return PostgresReads::run_record(run_id, lock);
    }
    std::optional<ProgramJournalRecord> journal_record(std::string_view run_id, Lock lock) override {
        materialize_run(run_id);
        return PostgresReads::journal_record(run_id, lock);
    }
    std::optional<ProgramChildRelationHead> relation_head(std::string_view parent_run_id, Lock lock) override {
        if (const auto found = relation_head_bytes_.find(parent_run_id); found != relation_head_bytes_.end()) {
            std::optional<ProgramChildRelationHead> head;
            if (found->second) head = parse_relation_head(owner_scope_, parent_run_id, *found->second);
            relation_heads_.insert_or_assign(std::string(parent_run_id), RelationHeadMemo{true, std::move(head)});
            relation_head_bytes_.erase(found);
        }
        return PostgresReads::relation_head(parent_run_id, lock);
    }
    std::optional<std::string> run_lineage_id(std::string_view run_id) override {
        if (const auto found = run_lineages_.find(run_id); found != run_lineages_.end()) return found->second;
        return PostgresReads::run_lineage_id(run_id);
    }
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(std::string_view run_id) override {
        if (latest_command_run_ && *latest_command_run_ == run_id) {
            if (!latest_command_) return std::nullopt;
            return parse_command_row(latest_command_->sequence, latest_command_->coordinate_id,
                                     latest_command_->bytes);
        }
        return PostgresReads::latest_command(run_id);
    }
    std::optional<ProgramChildRelation> relation_row(std::string_view parent_run_id, std::string_view child_run_id,
                                                     Lock lock) override {
        if (const auto found =
                relation_row_bytes_.find(std::make_pair(std::string(parent_run_id), std::string(child_run_id)));
            found != relation_row_bytes_.end()) {
            if (!found->second) return std::nullopt;
            auto row = parse_relation_row(owner_scope_, parent_run_id, *found->second);
            if (row.child_run_id() != child_run_id)
                throw std::invalid_argument("Stored PostgreSQL Program child relation binding is corrupt");
            return row;
        }
        return PostgresReads::relation_row(parent_run_id, child_run_id, lock);
    }

protected:
    void locking_read_issued() override { late_locks_->fetch_add(1, std::memory_order_relaxed); }

private:
    StoredHead parse_head(std::string_view run_id, const RunColumns& columns) const {
        return StoredHead{parse_run_record(cache_, owner_scope_, run_id, columns.run_record),
                          ProgramJournalRecord::parse(columns.journal_record),
                          columns.migration_plan ? std::optional<MigrationPlan>(MigrationPlan::parse(*columns.migration_plan))
                                                 : std::nullopt,
                          columns.last_publication};
    }
    // A head of a run other than the publication's own is parsed the first time the validator reads it.
    void materialize_run(std::string_view run_id) {
        const auto found = run_columns_.find(run_id);
        if (found == run_columns_.end()) return;
        auto head = parse_head(run_id, found->second);
        heads_.insert_or_assign(std::string(run_id), HeadMemo{true, std::move(head)});
        run_columns_.erase(found);
    }

    std::map<std::string, RunColumns, std::less<>>                                  run_columns_;
    std::map<std::string, std::optional<std::string>, std::less<>>                  relation_head_bytes_;
    std::map<std::pair<std::string, std::string>, std::optional<std::string>>       relation_row_bytes_;
    std::atomic<std::uint64_t>*                                                     late_locks_;
    std::optional<std::string>                                                      latest_command_run_;
    std::optional<CommandColumns>                                                   latest_command_;
    std::map<std::string, std::optional<std::string>, std::less<>>                  lineage_heads_;
    std::map<std::string, std::optional<std::string>, std::less<>>                  run_lineages_;
    std::map<std::string, std::optional<ProgramExecutionLease>, std::less<>>        leases_;
};

void check_group_shape(const PgResult& result, int columns, std::string_view what) {
    if (PQnfields(result.get()) != columns)
        throw std::invalid_argument("Stored PostgreSQL Program " + std::string(what) + " group shape is invalid");
}

// Seeds `reads` from the results of the statements of read_group_statements, in the same order. A named row that a
// group did not return is absent. Only the shape of a group is checked here; the content of a row is parsed when the
// validator reads it (BatchedPostgresReads), except the publication's own run head.
void seed_read_groups(std::string_view owner_scope, std::string_view own_run_id,
                      const detail::transition::PublicationLockPlan& plan,
                      const std::vector<ReadGroupStatement>& groups, const std::vector<PgResult>& results,
                      detail::RunRecordCache* cache, BatchedPostgresReads& reads) {
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto& result = results[index];
        const int   count  = PQntuples(result.get());
        switch (groups[index].group) {
        case ReadGroup::LineageHeads: {
            check_group_shape(result, 2, "lineage head");
            std::map<std::string, int, std::less<>> at_row;
            for (int row = 0; row < count; ++row) at_row.emplace(row_text(result, row, 0), row);
            if (at_row.size() != static_cast<std::size_t>(count) || at_row.size() > plan.lineage_ids.size())
                throw std::invalid_argument("Stored PostgreSQL Program lineage head group is invalid");
            for (const auto& id : plan.lineage_ids) {
                const auto at = at_row.find(id);
                reads.seed_lineage_head(id, at == at_row.end() ? std::nullopt
                                                               : std::optional<std::string>(row_text(result, at->second, 1)));
            }
            break;
        }
        case ReadGroup::RunHeads: {
            check_group_shape(result, groups[index].with_latest_command ? 11 : 8, "run head");
            std::map<std::string, int, std::less<>> at_row;
            for (int row = 0; row < count; ++row) at_row.emplace(row_text(result, row, 0), row);
            if (at_row.size() != static_cast<std::size_t>(count) || at_row.size() > plan.run_ids.size())
                throw std::invalid_argument("Stored PostgreSQL Program run head group is invalid");
            for (const auto& run_id : plan.run_ids) {
                const auto at = at_row.find(run_id);
                if (at == at_row.end()) {
                    if (cache) cache->erase(owner_scope, run_id);
                    reads.seed_run(run_id, std::nullopt, std::nullopt, std::nullopt, run_id == own_run_id);
                    continue;
                }
                const int row = at->second;
                BatchedPostgresReads::RunColumns columns{row_text(result, row, 1), row_text(result, row, 2),
                                                         row_optional_text(result, row, 3), row_text(result, row, 4)};
                std::optional<ProgramExecutionLease> lease;
                if (run_id == own_run_id && !PQgetisnull(result.get(), row, 5))
                    lease = parse_execution_lease(owner_scope, run_id, row_text(result, row, 5),
                                                  row_text(result, row, 6));
                reads.seed_run(run_id, std::move(columns), row_optional_text(result, row, 7), std::move(lease),
                               run_id == own_run_id);
            }
            if (groups[index].with_latest_command) {
                // The latest command of the named run: none when the run has no head or no command.
                std::optional<BatchedPostgresReads::CommandColumns> columns;
                const auto at = at_row.find(groups[index].statement.params.back().value());
                if (at != at_row.end() && !PQgetisnull(result.get(), at->second, 8))
                    columns = BatchedPostgresReads::CommandColumns{row_text(result, at->second, 8),
                                                                  row_text(result, at->second, 9),
                                                                  row_text(result, at->second, 10)};
                reads.seed_latest_command(groups[index].statement.params.back().value(), std::move(columns));
            }
            break;
        }
        case ReadGroup::RelationHeads: {
            check_group_shape(result, 2, "relation head");
            std::map<std::string, int, std::less<>> at_row;
            for (int row = 0; row < count; ++row) at_row.emplace(row_text(result, row, 0), row);
            if (at_row.size() != static_cast<std::size_t>(count) || at_row.size() > plan.relation_parent_ids.size())
                throw std::invalid_argument("Stored PostgreSQL Program relation head group is invalid");
            for (const auto& parent : plan.relation_parent_ids) {
                const auto at = at_row.find(parent);
                reads.seed_relation_head(parent, at == at_row.end()
                                                     ? std::nullopt
                                                     : std::optional<std::string>(row_text(result, at->second, 1)));
            }
            break;
        }
        case ReadGroup::RelationRows: {
            check_group_shape(result, 3, "relation row");
            std::map<std::pair<std::string, std::string>, int> at_row;
            for (int row = 0; row < count; ++row)
                at_row.emplace(std::make_pair(row_text(result, row, 0), row_text(result, row, 1)), row);
            if (at_row.size() != static_cast<std::size_t>(count) || at_row.size() > plan.relation_rows.size())
                throw std::invalid_argument("Stored PostgreSQL Program relation row group is invalid");
            for (const auto& key : plan.relation_rows) {
                const auto at = at_row.find(key);
                reads.seed_relation_row(key.first, key.second,
                                        at == at_row.end() ? std::nullopt
                                                           : std::optional<std::string>(row_text(result, at->second, 2)));
            }
            break;
        }
        }
    }
}

// ---- The statements of a publication's write phase. Each part is one statement here (the batched path may merge them);
// the wrappers below run one statement for the callers that write outside a publication.

SqlStatement insert_lineage_head_statement(const ProgramRunLineage& lineage) {
    return {"INSERT INTO neograph_program_run_lineage_heads_v1"
            "(owner_scope, lineage_id, head_bytes) VALUES($1, $2, $3)",
            {lineage.owner_scope(), lineage.lineage_id(), lineage.serialize_canonical()}, {}, {}};
}

SqlStatement update_lineage_head_statement(const ProgramRunLineage& lineage) {
    return {cas_guarded("UPDATE neograph_program_run_lineage_heads_v1 SET head_bytes = $1 "
                        "WHERE owner_scope = $2 AND lineage_id = $3"),
            {lineage.serialize_canonical(), lineage.owner_scope(), lineage.lineage_id()},
            "PostgreSQL Program lineage head update lost its CAS target", {}};
}

SqlStatement insert_generation_statement(const ProgramRunGeneration& generation) {
    return {"INSERT INTO neograph_program_run_generations_v1"
            "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {generation.owner_scope(), generation.lineage_id(), std::to_string(generation.generation()),
             generation.id(), generation.serialize_canonical()},
            {}, {}};
}

SqlStatement insert_generation_initial_publication_statement(const ProgramRunGeneration& generation,
                                                             std::string_view            publication_bytes) {
    return {"INSERT INTO neograph_program_run_generation_publications_v1"
            "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {generation.owner_scope(), generation.lineage_id(), std::to_string(generation.generation()),
             generation.id(), std::string(publication_bytes)},
            {}, {}};
}

SqlStatement insert_lineage_history_statement(const ProgramRunLineage& lineage) {
    return {"INSERT INTO neograph_program_run_lineage_history_v1"
            "(owner_scope, lineage_id, head_id, canonical_bytes) VALUES($1, $2, $3, $4)",
            {lineage.owner_scope(), lineage.lineage_id(), lineage.id(), lineage.serialize_canonical()}, {}, {}};
}

SqlStatement insert_run_lineage_statement(std::string_view owner_scope, std::string_view run_id,
                                          std::string_view lineage_id) {
    return {"INSERT INTO neograph_program_run_lineage_runs_v1"
            "(owner_scope, run_id, lineage_id) VALUES($1, $2, $3)",
            {std::string(owner_scope), std::string(run_id), std::string(lineage_id)}, {}, {}};
}

SqlStatement insert_graph_migration_capsule_statement(const GraphMigrationCapsule& capsule) {
    return {"INSERT INTO neograph_program_graph_migration_capsules_v1"
            "(owner_scope, source_run_id, source_lineage_head_id, capsule_id, capsule_bytes) "
            "VALUES($1, $2, $3, $4, $5)",
            {capsule.owner_scope(), capsule.source_run_id(), capsule.source_lineage_head_id(), capsule.id(),
             capsule.serialize_canonical()},
            {}, {}};
}

SqlStatement insert_execution_lease_statement(const ProgramExecutionLease& lease) {
    return {"INSERT INTO neograph_program_execution_leases_v1"
            "(owner_scope, run_id, lease_id, lease_bytes) VALUES($1, $2, $3, $4) "
            "ON CONFLICT(owner_scope, run_id) DO UPDATE SET "
            "lease_id = EXCLUDED.lease_id, lease_bytes = EXCLUDED.lease_bytes",
            {lease.owner_scope(), lease.run_id(), lease.id(), lease.serialize_canonical()}, {}, {}};
}

SqlStatement delete_execution_lease_statement(const ProgramExecutionLease& lease) {
    return {cas_guarded("DELETE FROM neograph_program_execution_leases_v1 "
                        "WHERE owner_scope = $1 AND run_id = $2 AND lease_id = $3"),
            {lease.owner_scope(), lease.run_id(), lease.id()},
            "PostgreSQL Program execution lease delete lost its CAS target", {}};
}

SqlStatement insert_head_statement(std::string_view owner_scope, std::string_view run_id,
                                   const std::string& run_bytes, const ProgramJournalRecord& journal_record,
                                   const std::optional<MigrationPlan>& migration_plan,
                                   std::string_view last_publication_bytes) {
    return {"INSERT INTO neograph_program_transition_run_heads_v2"
            "(owner_scope, run_id, run_record_bytes, journal_record_bytes, migration_plan_bytes, "
            "last_publication_bytes) VALUES($1, $2, $3, $4, $5, $6)",
            {std::string(owner_scope), std::string(run_id), run_bytes, journal_record.serialize_canonical(),
             migration_plan ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt,
             std::string(last_publication_bytes)},
            {}, {}};
}

SqlStatement update_head_statement(std::string_view owner_scope, std::string_view run_id,
                                   const std::string& run_bytes, const ProgramJournalRecord& journal_record,
                                   const std::optional<MigrationPlan>& migration_plan,
                                   std::string_view last_publication_bytes) {
    return {cas_guarded("UPDATE neograph_program_transition_run_heads_v2 SET run_record_bytes = $1, "
                        "journal_record_bytes = $2, migration_plan_bytes = $3, last_publication_bytes = $4 "
                        "WHERE owner_scope = $5 AND run_id = $6"),
            {run_bytes, journal_record.serialize_canonical(),
             migration_plan ? std::optional<std::string>(migration_plan->serialize_canonical()) : std::nullopt,
             std::string(last_publication_bytes), std::string(owner_scope), std::string(run_id)},
            "PostgreSQL Program transition head update lost its CAS target", {}};
}

void append_event_statements(std::vector<SqlStatement>& out, std::string_view owner_scope, std::string_view run_id,
                             const std::vector<ProgramEvent>& events) {
    for (const auto& event : events)
        out.push_back({"INSERT INTO neograph_program_transition_event_log_v2"
                       "(owner_scope, run_id, sequence, canonical_bytes) VALUES($1, $2, $3, $4)",
                       {std::string(owner_scope), std::string(run_id), std::to_string(event.sequence),
                        event.serialize_canonical()},
                       {}, {}});
}

void append_effect_statements(std::vector<SqlStatement>& out, std::string_view owner_scope, std::string_view run_id,
                              const std::vector<ProgramEffectOutboxEntry>& effects) {
    for (const auto& effect : effects)
        out.push_back({"INSERT INTO neograph_program_transition_effect_log_v2"
                       "(owner_scope, run_id, sequence, effect_id, canonical_bytes) "
                       "VALUES($1, $2, $3, $4, $5)",
                       {std::string(owner_scope), std::string(run_id), std::to_string(effect.sequence()),
                        effect.effect().effect_id(), effect.serialize_canonical()},
                       {}, {}});
}

void append_javascript_command_statements(std::vector<SqlStatement>& out, std::string_view owner_scope,
                                          std::string_view run_id,
                                          const std::vector<ProgramJavaScriptCommandJournalEntry>& commands) {
    for (const auto& command : commands)
        out.push_back({"INSERT INTO neograph_program_transition_javascript_command_log_v2"
                       "(owner_scope, run_id, sequence, coordinate_id, canonical_bytes) "
                       "VALUES($1, $2, $3, $4, $5)",
                       {std::string(owner_scope), std::string(run_id), std::to_string(command.sequence()),
                        command.coordinate_id(), command.serialize_canonical()},
                       {}, {}});
}

void append_context_statement(std::vector<SqlStatement>& out, std::string_view owner_scope, std::string_view run_id,
                              const std::optional<ProgramContextPublication>& context) {
    if (!context) return;
    out.push_back({"INSERT INTO neograph_program_transition_context_log_v1"
                   "(owner_scope, run_id, sequence, canonical_bytes) VALUES($1, $2, $3, $4)",
                   {std::string(owner_scope), std::string(run_id), std::to_string(context->epoch.sequence()),
                    context_publication_bytes(*context)},
                   {}, {}});
}

void append_synthesis_statements(std::vector<SqlStatement>& out, std::string_view owner_scope,
                                 std::string_view run_id,
                                 const std::vector<ProgramChildSynthesisRecord>& entries) {
    for (const auto& entry : entries)
        out.push_back({"INSERT INTO "
                       "neograph_program_transition_synthesis_log_v1(owner_scope,run_id,"
                       "proposal_id,head_id,canonical_bytes) VALUES($1,$2,$3,$4,$5)",
                       {std::string(owner_scope), std::string(run_id), entry.data().proposal.id(), entry.id(),
                        entry.serialize_canonical()},
                       {}, {}});
}

void append_hook_outbox_statements(std::vector<SqlStatement>& out, std::string_view owner_scope,
                                   std::string_view run_id, const std::vector<HookOutboxEntry>& entries) {
    for (const auto& entry : entries)
        out.push_back({"INSERT INTO neograph_program_transition_hook_outbox_log_v1"
                       "(owner_scope, run_id, invocation_id, head_id, canonical_bytes) "
                       "VALUES($1, $2, $3, $4, $5)",
                       {std::string(owner_scope), std::string(run_id), entry.data().invocation.id(), entry.id(),
                        entry.serialize_canonical()},
                       {}, {}});
}

SqlStatement write_relation_head_statement(const ProgramChildRelationHead& head) {
    return {"INSERT INTO neograph_program_child_relation_heads_v1(owner_scope, parent_run_id, head_bytes) "
            "VALUES($1, $2, $3) ON CONFLICT(owner_scope, parent_run_id) DO UPDATE SET "
            "head_bytes = EXCLUDED.head_bytes",
            {head.data().owner_scope, head.data().parent_run_id, head.serialize_canonical()}, {}, {}};
}

SqlStatement write_relation_row_statement(const ProgramChildRelation& row) {
    return {"INSERT INTO neograph_program_child_relations_v1"
            "(owner_scope, parent_run_id, child_run_id, ordinal, open, row_bytes) "
            "VALUES($1, $2, $3, $4, $5, $6) ON CONFLICT(owner_scope, parent_run_id, child_run_id) "
            "DO UPDATE SET open = EXCLUDED.open, row_bytes = EXCLUDED.row_bytes",
            {row.owner_scope(), row.parent_run_id(), row.child_run_id(), std::to_string(row.ordinal()),
             row.is_open() ? "true" : "false", row.serialize_canonical()},
            {}, {}};
}

// The relation writes a plan names: the head first, which the rows reference.
void relation_write_statements(std::vector<SqlStatement>& out, const std::optional<ProgramChildRelation>& row,
                               const std::optional<ProgramChildRelationHead>& head,
                               const std::optional<ProgramChildRelationHead>& closed_head) {
    if (row && head) {
        out.push_back(write_relation_head_statement(*head));
        out.push_back(write_relation_row_statement(*row));
    }
    if (closed_head) out.push_back(write_relation_head_statement(*closed_head));
}

void exec_write(PGconn* connection, const SqlStatement& statement) { (void)exec_statement(connection, statement); }

void update_lineage_head(PGconn* connection, const ProgramRunLineage& lineage) {
    exec_write(connection, update_lineage_head_statement(lineage));
}

void insert_lineage_history(PGconn* connection, const ProgramRunLineage& lineage) {
    exec_write(connection, insert_lineage_history_statement(lineage));
}

void update_head(PGconn* connection, std::string_view owner_scope, std::string_view run_id,
                 const std::string& run_bytes, const ProgramJournalRecord& journal_record,
                 const std::optional<MigrationPlan>& migration_plan, std::string_view last_publication_bytes) {
    exec_write(connection, update_head_statement(owner_scope, run_id, run_bytes, journal_record, migration_plan,
                                                 last_publication_bytes));
}

void create_schema(PGconn* connection) {
    exec_sql(connection, std::string("CREATE TABLE IF NOT EXISTS ") + CAS_GUARD_TABLE +
                             " (ok BOOLEAN NOT NULL, CONSTRAINT " + CAS_GUARD_CONSTRAINT + " CHECK (ok))");
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
CREATE TABLE IF NOT EXISTS neograph_program_child_relation_heads_v1 (
    owner_scope TEXT NOT NULL, parent_run_id TEXT NOT NULL, head_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, parent_run_id));
CREATE TABLE IF NOT EXISTS neograph_program_child_relations_v1 (
    owner_scope TEXT NOT NULL, parent_run_id TEXT NOT NULL, child_run_id TEXT NOT NULL,
    ordinal BIGINT NOT NULL, open BOOLEAN NOT NULL, row_bytes TEXT NOT NULL,
    PRIMARY KEY(owner_scope, parent_run_id, child_run_id),
    UNIQUE(owner_scope, parent_run_id, ordinal),
    FOREIGN KEY(owner_scope, parent_run_id)
        REFERENCES neograph_program_child_relation_heads_v1(owner_scope, parent_run_id)
        ON DELETE CASCADE);
CREATE INDEX IF NOT EXISTS neograph_program_child_relations_open_v1
    ON neograph_program_child_relations_v1(owner_scope, parent_run_id, open, ordinal);
)SQL");
}

void write_relation_head(PGconn* connection, const ProgramChildRelationHead& head) {
    exec_write(connection, write_relation_head_statement(head));
}

void write_relation_row(PGconn* connection, const ProgramChildRelation& row) {
    exec_write(connection, write_relation_row_statement(row));
}

// A database written before the children left the run record holds them inside the record. One pass at open turns
// them into relation rows (the record is not rewritten: its id binds lineage, generations and receipts), and a marker
// says the pass is done. The pass runs under an advisory lock, so stores that open at once migrate once. A parent with
// several stored generations is read from its newest record.
void materialize_legacy_relations(PGconn* connection) {
    Transaction transaction(connection);
    (void)exec_params(connection, "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                      {"neograph_program_child_relation_migration_v1"});
    exec_sql(connection,
             "CREATE TABLE IF NOT EXISTS neograph_program_child_relation_migration_v1 (done BOOLEAN NOT NULL)");
    {
        const auto marker = exec_params(
            connection, "SELECT COUNT(*) FROM neograph_program_child_relation_migration_v1", {});
        if (row_text(marker, 0, 0) != "0") {
            transaction.commit();
            return;
        }
    }
    struct Found {
        std::int64_t                           updated_at_ms = 0;
        detail::transition::LegacyRelationPlan plan;
    };
    std::map<std::pair<std::string, std::string>, Found> newest;
    {
        const auto scan = exec_params(
            connection, "SELECT run_record_bytes FROM neograph_program_transition_run_heads_v2", {});
        for (int index = 0; index < PQntuples(scan.get()); ++index) {
            const auto record = ProgramRunRecord::parse(row_text(scan, index, 0));
            auto       plan   = detail::transition::plan_legacy_relations(record);
            if (!plan) continue;
            const auto key = std::make_pair(record.owner_scope(), record.logical_run_id());
            const auto at  = newest.find(key);
            if (at == newest.end() || at->second.updated_at_ms <= record.updated_at_ms())
                newest.insert_or_assign(key, Found{record.updated_at_ms(), std::move(*plan)});
        }
    }
    for (const auto& [key, found] : newest) {
        const auto present = exec_params(
            connection,
            "SELECT COUNT(*) FROM neograph_program_child_relation_heads_v1 WHERE owner_scope = $1 AND parent_run_id = $2",
            {key.first, key.second});
        if (row_text(present, 0, 0) != "0") continue;
        write_relation_head(connection, found.plan.head);
        for (const auto& row : found.plan.rows) write_relation_row(connection, row);
    }
    exec_sql(connection, "INSERT INTO neograph_program_child_relation_migration_v1(done) VALUES(TRUE)");
    transaction.commit();
}

// The writes a relation plan names, in the transaction that validated it: the head first, which the rows reference.
void apply_relation_writes(PGconn*                                          connection,
                           const std::optional<ProgramChildRelation>&       row,
                           const std::optional<ProgramChildRelationHead>&   head,
                           const std::optional<ProgramChildRelationHead>&   closed_head) {
    std::vector<SqlStatement> statements;
    relation_write_statements(statements, row, head, closed_head);
    for (const auto& statement : statements) exec_write(connection, statement);
}

// What one publication call carries into the transaction.
struct PublishArgs {
    std::string_view                     owner_scope;
    std::string_view                     expected_journal_head;
    std::uint64_t                        expected_fence;
    const ProgramTransitionPublication&  publication;
    const std::string&                   publication_bytes;
    const ProgramGraphSafePointEvidence* safe_point_evidence;
    const GraphMigrationCapsule*         safe_point_capsule;
    const ProgramExecutionLease*         expected_lease;
    const ProgramExecutionLease*         next_lease;
};

// The writes of a validated publication, one statement per part, in the order that keeps every reference satisfied.
std::vector<SqlStatement> publication_write_statements(const PublishArgs& call, const std::string& run_id,
                                                       const std::string&                 run_record_bytes,
                                                       const std::optional<StoredHead>&   current,
                                                       const detail::transition::TransitionPlan& plan) {
    const auto&               owner_scope    = call.owner_scope;
    const auto&               publication    = call.publication;
    const auto&               migration_plan = current ? current->migration_plan : publication.migration_plan;
    std::vector<SqlStatement> out;
    out.push_back(current ? update_head_statement(owner_scope, run_id, run_record_bytes, publication.journal_record,
                                                  migration_plan, call.publication_bytes)
                          : insert_head_statement(owner_scope, run_id, run_record_bytes, publication.journal_record,
                                                  migration_plan, call.publication_bytes));
    append_event_statements(out, owner_scope, run_id, publication.events);
    append_effect_statements(out, owner_scope, run_id, publication.effects);
    append_javascript_command_statements(out, owner_scope, run_id, publication.commands);
    append_context_statement(out, owner_scope, run_id, publication.context_publication);
    append_synthesis_statements(out, owner_scope, run_id, publication.child_synthesis_records);
    append_hook_outbox_statements(out, owner_scope, run_id, publication.hook_outbox_entries);
    if (publication.run_lineage) {
        out.push_back(plan.lineage_head_exists ? update_lineage_head_statement(*publication.run_lineage)
                                               : insert_lineage_head_statement(*publication.run_lineage));
        out.push_back(insert_lineage_history_statement(*publication.run_lineage));
        if (publication.run_generation) {
            out.push_back(insert_generation_statement(*publication.run_generation));
            out.push_back(insert_generation_initial_publication_statement(*publication.run_generation,
                                                                          call.publication_bytes));
        }
        if (!plan.run_lineage_recorded)
            out.push_back(insert_run_lineage_statement(owner_scope, run_id, publication.run_lineage->lineage_id()));
    }
    if (publication.fork_source_lineage) {
        out.push_back(update_lineage_head_statement(*publication.fork_source_lineage));
        out.push_back(insert_lineage_history_statement(*publication.fork_source_lineage));
    }
    if (publication.recorded_replay_source_lineage) {
        out.push_back(update_lineage_head_statement(*publication.recorded_replay_source_lineage));
        out.push_back(insert_lineage_history_statement(*publication.recorded_replay_source_lineage));
    }
    relation_write_statements(out, plan.relation_row, plan.relation_head, plan.closed_relation_head);
    if (call.safe_point_capsule) out.push_back(insert_graph_migration_capsule_statement(*call.safe_point_capsule));
    if (call.next_lease)
        out.push_back(insert_execution_lease_statement(*call.next_lease));
    else if (call.expected_lease)
        out.push_back(delete_execution_lease_statement(*call.expected_lease));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// One statement for the writes of the common publication shapes. The parts a publication has (typed flags) pick the
// fragments; each fragment is a data-modifying common table expression, and the server runs them all, to completion,
// as one statement. Rows of a log arrive as one JSON array parameter built by the JSON writer: the canonical bytes
// travel as a string and are inserted from it unchanged. A publication that has any part not covered here is written
// as one statement per part.
// ---------------------------------------------------------------------------------------------------------------

// The parts of a publication that one statement covers.
struct SingleWriteParts {
    bool head_exists          = false;  // UPDATE of the run head; else INSERT of a new run
    bool events               = false;
    bool effects              = false;
    bool commands             = false;
    bool lineage              = false;  // the publication's lineage head and its history row
    bool lineage_head_exists  = false;  // UPDATE of the lineage head; else INSERT
    bool generation           = false;
    bool run_lineage          = false;  // the association of the run with its lineage is inserted
    bool relation             = false;  // the relation head and row of a spawn or a completion
    bool closed_relation_head = false;
};

// The parts, or nothing when the publication has a part that is written as its own statement: a context publication,
// synthesis records, hook entries, a graph migration capsule, an execution lease write, a fork or replay source, or
// two relation writes of the same head (one statement cannot change a row twice).
std::optional<SingleWriteParts> single_write_parts(const PublishArgs& call, const std::optional<StoredHead>& current,
                                                   const detail::transition::TransitionPlan& plan) {
    const auto& publication = call.publication;
    if (publication.context_publication || !publication.child_synthesis_records.empty() ||
        !publication.hook_outbox_entries.empty() || call.safe_point_capsule || call.next_lease ||
        call.expected_lease || publication.fork_source_lineage || publication.recorded_replay_source_lineage)
        return std::nullopt;
    if (plan.relation_head && plan.closed_relation_head &&
        plan.relation_head->data().parent_run_id == plan.closed_relation_head->data().parent_run_id)
        return std::nullopt;
    SingleWriteParts parts;
    parts.head_exists          = current.has_value();
    parts.events               = !publication.events.empty();
    parts.effects              = !publication.effects.empty();
    parts.commands             = !publication.commands.empty();
    parts.lineage              = publication.run_lineage.has_value();
    parts.lineage_head_exists  = plan.lineage_head_exists;
    parts.generation           = publication.run_lineage && publication.run_generation;
    parts.run_lineage          = publication.run_lineage && !plan.run_lineage_recorded;
    parts.relation             = plan.relation_row && plan.relation_head;
    parts.closed_relation_head = plan.closed_relation_head.has_value();
    return parts;
}

// Numbers the parameters of one statement in the order they are added.
class ParameterList {
public:
    explicit ParameterList(std::vector<std::optional<std::string>>& values) : values_(values) {}
    std::string add(std::optional<std::string> value, const char* type) {
        values_.push_back(std::move(value));
        return "$" + std::to_string(values_.size()) + "::" + type;
    }

private:
    std::vector<std::optional<std::string>>& values_;
};

SqlStatement single_write_statement(const SingleWriteParts& parts, const PublishArgs& call, const std::string& run_id,
                                    const std::string&                         run_record_bytes,
                                    const std::optional<StoredHead>&           current,
                                    const detail::transition::TransitionPlan&  plan) {
    const auto&               publication    = call.publication;
    const auto&               migration_plan = current ? current->migration_plan : publication.migration_plan;
    SqlStatement              statement;
    ParameterList             parameters(statement.params);
    std::vector<std::string>  fragments;
    std::vector<std::string>  changed;  // the fragments whose row count must be one
    const auto owner = parameters.add(std::string(call.owner_scope), "text");
    const auto run   = parameters.add(run_id, "text");

    // The run head.
    {
        const auto record   = parameters.add(run_record_bytes, "text");
        const auto journal  = parameters.add(publication.journal_record.serialize_canonical(), "text");
        const auto plan_txt = parameters.add(migration_plan ? std::optional<std::string>(migration_plan->serialize_canonical())
                                                            : std::nullopt, "text");
        const auto last     = parameters.add(call.publication_bytes, "text");
        if (parts.head_exists) {
            fragments.push_back("head AS (UPDATE neograph_program_transition_run_heads_v2 SET run_record_bytes = " +
                                record + ", journal_record_bytes = " + journal + ", migration_plan_bytes = " + plan_txt +
                                ", last_publication_bytes = " + last + " WHERE owner_scope = " + owner +
                                " AND run_id = " + run + " RETURNING 1)");
            changed.push_back("head");
        } else {
            fragments.push_back("head AS (INSERT INTO neograph_program_transition_run_heads_v2(owner_scope, run_id, "
                                "run_record_bytes, journal_record_bytes, migration_plan_bytes, last_publication_bytes) "
                                "VALUES(" + owner + ", " + run + ", " + record + ", " + journal + ", " + plan_txt +
                                ", " + last + ") RETURNING 1)");
        }
    }

    // The rows of a log: one array parameter, one element per row.
    if (parts.events) {
        auto rows = json::array();
        for (const auto& event : publication.events)
            rows.push_back(json{{"s", event.sequence}, {"b", event.serialize_canonical()}});
        fragments.push_back("events AS (INSERT INTO neograph_program_transition_event_log_v2"
                            "(owner_scope, run_id, sequence, canonical_bytes) SELECT " + owner + ", " + run +
                            ", (e->>'s')::bigint, e->>'b' FROM jsonb_array_elements(" +
                            parameters.add(rows.dump(), "jsonb") + ") AS e)");
    }
    if (parts.effects) {
        auto rows = json::array();
        for (const auto& effect : publication.effects)
            rows.push_back(json{{"s", effect.sequence()}, {"i", effect.effect().effect_id()},
                                {"b", effect.serialize_canonical()}});
        fragments.push_back("effects AS (INSERT INTO neograph_program_transition_effect_log_v2"
                            "(owner_scope, run_id, sequence, effect_id, canonical_bytes) SELECT " + owner + ", " + run +
                            ", (e->>'s')::bigint, e->>'i', e->>'b' FROM jsonb_array_elements(" +
                            parameters.add(rows.dump(), "jsonb") + ") AS e)");
    }
    if (parts.commands) {
        auto rows = json::array();
        for (const auto& command : publication.commands)
            rows.push_back(json{{"s", command.sequence()}, {"c", command.coordinate_id()},
                                {"b", command.serialize_canonical()}});
        fragments.push_back("commands AS (INSERT INTO neograph_program_transition_javascript_command_log_v2"
                            "(owner_scope, run_id, sequence, coordinate_id, canonical_bytes) SELECT " + owner + ", " +
                            run + ", (e->>'s')::bigint, e->>'c', e->>'b' FROM jsonb_array_elements(" +
                            parameters.add(rows.dump(), "jsonb") + ") AS e)");
    }

    // The lineage head, its history row, its first generation, and the run's association with it.
    if (parts.lineage) {
        const auto& lineage       = *publication.run_lineage;
        const auto  lineage_owner = parameters.add(lineage.owner_scope(), "text");
        const auto  lineage_id    = parameters.add(lineage.lineage_id(), "text");
        const auto  head_bytes    = parameters.add(lineage.serialize_canonical(), "text");
        if (parts.lineage_head_exists) {
            fragments.push_back("lineage AS (UPDATE neograph_program_run_lineage_heads_v1 SET head_bytes = " +
                                head_bytes + " WHERE owner_scope = " + lineage_owner + " AND lineage_id = " +
                                lineage_id + " RETURNING 1)");
            changed.push_back("lineage");
        } else {
            fragments.push_back("lineage AS (INSERT INTO neograph_program_run_lineage_heads_v1"
                                "(owner_scope, lineage_id, head_bytes) VALUES(" + lineage_owner + ", " + lineage_id +
                                ", " + head_bytes + ") RETURNING 1)");
        }
        fragments.push_back("lineage_history AS (INSERT INTO neograph_program_run_lineage_history_v1"
                            "(owner_scope, lineage_id, head_id, canonical_bytes) VALUES(" + lineage_owner + ", " +
                            lineage_id + ", " + parameters.add(lineage.id(), "text") + ", " + head_bytes + "))");
        if (parts.generation) {
            const auto& generation     = *publication.run_generation;
            const auto  generation_no  = parameters.add(std::to_string(generation.generation()), "bigint");
            const auto  generation_id  = parameters.add(generation.id(), "text");
            fragments.push_back("generation AS (INSERT INTO neograph_program_run_generations_v1"
                                "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) VALUES(" +
                                owner + ", " + lineage_id + ", " + generation_no + ", " + generation_id + ", " +
                                parameters.add(generation.serialize_canonical(), "text") + "))");
            fragments.push_back("generation_publication AS (INSERT INTO "
                                "neograph_program_run_generation_publications_v1"
                                "(owner_scope, lineage_id, generation, generation_id, canonical_bytes) VALUES(" +
                                owner + ", " + lineage_id + ", " + generation_no + ", " + generation_id + ", " +
                                parameters.add(call.publication_bytes, "text") + "))");
        }
        if (parts.run_lineage)
            fragments.push_back("run_lineage AS (INSERT INTO neograph_program_run_lineage_runs_v1"
                                "(owner_scope, run_id, lineage_id) VALUES(" + owner + ", " + run + ", " + lineage_id +
                                "))");
    }

    // The relation rows: the head first in meaning (a row references it), the same upserts as the per-part writes.
    const auto relation_head = [&](const ProgramChildRelationHead& head, const char* name) {
        fragments.push_back(std::string(name) + " AS (INSERT INTO neograph_program_child_relation_heads_v1"
                            "(owner_scope, parent_run_id, head_bytes) VALUES(" +
                            parameters.add(head.data().owner_scope, "text") + ", " +
                            parameters.add(head.data().parent_run_id, "text") + ", " +
                            parameters.add(head.serialize_canonical(), "text") +
                            ") ON CONFLICT(owner_scope, parent_run_id) DO UPDATE SET head_bytes = EXCLUDED.head_bytes)");
    };
    if (parts.relation) {
        relation_head(*plan.relation_head, "relation_head");
        const auto& row = *plan.relation_row;
        fragments.push_back("relation_row AS (INSERT INTO neograph_program_child_relations_v1"
                            "(owner_scope, parent_run_id, child_run_id, ordinal, open, row_bytes) VALUES(" +
                            parameters.add(row.owner_scope(), "text") + ", " +
                            parameters.add(row.parent_run_id(), "text") + ", " +
                            parameters.add(row.child_run_id(), "text") + ", " +
                            parameters.add(std::to_string(row.ordinal()), "bigint") + ", " +
                            parameters.add(row.is_open() ? "true" : "false", "boolean") + ", " +
                            parameters.add(row.serialize_canonical(), "text") +
                            ") ON CONFLICT(owner_scope, parent_run_id, child_run_id) DO UPDATE SET "
                            "open = EXCLUDED.open, row_bytes = EXCLUDED.row_bytes)");
    }
    if (parts.closed_relation_head) relation_head(*plan.closed_relation_head, "closed_relation_head");

    // A compare-and-swap target that was not changed exactly once aborts the statement on the server (cas_guarded).
    if (!changed.empty()) {
        std::string miss;
        for (std::size_t index = 0; index < changed.size(); ++index)
            miss += (index ? " OR " : "") + std::string("(SELECT count(*) FROM ") + changed[index] + ") <> 1";
        fragments.push_back(std::string("cas AS (INSERT INTO ") + CAS_GUARD_TABLE + "(ok) SELECT false WHERE " + miss +
                            ")");
        statement.cas_failure = "PostgreSQL Program transition write lost its CAS target";
    }

    statement.sql = "WITH ";
    for (std::size_t index = 0; index < fragments.size(); ++index)
        statement.sql += (index ? ", " : "") + fragments[index];
    statement.sql += " SELECT 1";
    return statement;
}

}  // namespace

struct PostgreSQLProgramTransitionStore::Impl {
    Impl(std::string value, Options opts)
        : connection_string(std::move(value)),
          options(opts),
          run_record_cache(opts.run_record_cache_capacity),
          own_heads(opts.own_head_hint_capacity),
          generations(opts.generation_cache_capacity),
          pipeline_enabled(opts.use_pipeline && detail::pipeline_mode_supported()) {}
    ~Impl() {
        if (connection) PQfinish(connection);
    }
    std::string                    connection_string;
    std::string                    coordination_key;
    Options                        options;
    PGconn*                        connection = nullptr;
    SessionIdentity                session_identity;
    mutable detail::RunRecordCache run_record_cache;
    mutable detail::OwnHeadHints   own_heads;
    mutable detail::GenerationCache generations;
    mutable std::mutex             mutex;
    /// Whether a batch of statements goes out in one pipeline: the option, and a libpq that has pipeline mode.
    bool                           pipeline_enabled = false;
    mutable std::mutex                                     hooks_mutex;
    std::shared_ptr<detail::PublicationTestHooks>          hooks;
    /// Statements sent on the connection, and locking reads a publication's plan did not cover.
    mutable std::atomic<std::uint64_t> statements{0};
    mutable std::atomic<std::uint64_t> late_locks{0};

    /// Holds the connection for one call, and counts the statements the thread sends meanwhile.
    class ConnectionLock final {
    public:
        explicit ConnectionLock(Impl& impl) : lock_(impl.mutex), previous_(statement_counter()) {
            statement_counter() = &impl.statements;
        }
        ConnectionLock(const ConnectionLock&) = delete;
        ConnectionLock& operator=(const ConnectionLock&) = delete;
        ~ConnectionLock() { statement_counter() = previous_; }

    private:
        std::lock_guard<std::mutex> lock_;
        std::atomic<std::uint64_t>* previous_;
    };

    struct ValidatedAttempt {
        detail::transition::TransitionVerdict verdict = detail::transition::TransitionVerdict::Conflict;
        std::optional<StoredHead>             current;
        detail::transition::TransitionPlan    plan;
    };

    ValidatedAttempt read_and_validate_sequential(const PublishArgs& call, const std::string& run_id,
                                                  const std::optional<ProgramOwnHeadHint>& previous_hint,
                                                  detail::PublicationTestHooks* test_hooks);
    ValidatedAttempt read_and_validate_batched(const PublishArgs& call, const std::string& run_id,
                                               const std::optional<ProgramOwnHeadHint>& previous_hint,
                                               detail::PublicationTestHooks* test_hooks);
    /// Checks the outcome of a publication once on a new session after the connection was lost while sending or
    /// awaiting its COMMIT. Returns Published or Conflict when the validator decides that outcome, or nullopt when
    /// the validator returns Proceed (the publication did not commit, so the caller rethrows the commit failure);
    /// throws ProgramTransitionOutcomeUnknown when the check itself cannot decide.
    std::optional<ProgramTransitionPublishResult> recheck_after_lost_commit(
        const PublishArgs& call, const std::string& run_id, const std::string& run_record_bytes,
        const std::optional<ProgramOwnHeadHint>& previous_hint, bool run_existed, SessionIdentity old_session,
        detail::PublicationTestHooks* test_hooks);

    ProgramTransitionPublishResult publish_sequential(const PublishArgs& call);
    ProgramTransitionPublishResult publish_batched(const PublishArgs& call);
    /// One attempt of publish_batched; empty when a row the validator saw as absent was inserted by another writer
    /// before this one wrote it (the transaction was rolled back).
    std::optional<ProgramTransitionPublishResult> publish_batched_attempt(
        const PublishArgs& call, const std::string& run_id, const std::optional<ProgramOwnHeadHint>& previous_hint);
    /// The caches and hints a published head updates, after its transaction committed.
    void note_published(const PublishArgs& call, const std::string& run_id, const std::string& run_record_bytes,
                        const std::optional<ProgramOwnHeadHint>& previous_hint, bool run_existed);
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
        materialize_legacy_relations(impl_->connection);
        impl_->session_identity = read_session_identity(impl_->connection);
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

std::optional<ProgramOwnHeadHint> PostgreSQLProgramTransitionStore::own_head_hint(
    std::string_view owner_scope, std::string_view run_id) const {
    return impl_->own_heads.get(owner_scope, run_id);
}

void PostgreSQLProgramTransitionStore::forget_own_head(std::string_view owner_scope,
                                                       std::string_view run_id) const noexcept {
    impl_->own_heads.erase(owner_scope, run_id);
}

std::optional<ProgramRunPublicationHead>
PostgreSQLProgramTransitionStore::load_run_publication_head_sync(std::string_view owner,
                                                           std::string_view run_id) const {
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
    return load_head_run_record(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
}

std::optional<ProgramJournalRecord> PostgreSQLProgramTransitionStore::latest_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    Impl::ConnectionLock lock(*impl_);
    return load_head_journal_record(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
}

std::vector<ProgramEvent> PostgreSQLProgramTransitionStore::load_events_sync(
    std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const {
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
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
    Impl::ConnectionLock lock(*impl_);
    return load_synthesis_heads_of_stored_run(impl_->connection, owner_scope, run_id);
}

std::vector<HookOutboxEntry> PostgreSQLProgramTransitionStore::load_hook_outbox_entries_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return {};
    Impl::ConnectionLock lock(*impl_);
    const auto run = load_head_run_record(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
    if (!run) return {};
    auto result = load_hook_outbox_heads(impl_->connection, owner_scope, run_id, *run);
    if (!is_valid_program_hook_history_append({}, result, *run))
        throw std::invalid_argument("Stored PostgreSQL hook history is corrupt");
    return result;
}

void PostgreSQLProgramTransitionStore::verify_stored_history_sync(std::string_view owner_scope,
                                                                  std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return;
    Impl::ConnectionLock lock(*impl_);
    const auto all =
        load_context_publication_history(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
    if (!valid_context_history(all))
        throw std::invalid_argument("Stored PostgreSQL context history is not contiguous");
    const auto run = load_head_run_record(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
    if (!run) return;
    const auto result = load_hook_outbox_heads(impl_->connection, owner_scope, run_id, *run);
    if (!is_valid_program_hook_history_append({}, result, *run))
        throw std::invalid_argument("Stored PostgreSQL hook history is corrupt");
}

std::optional<MigrationPlan> PostgreSQLProgramTransitionStore::load_migration_plan_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    Impl::ConnectionLock lock(*impl_);
    return load_head_migration_plan(impl_->connection, owner_scope, run_id, &impl_->run_record_cache);
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage_sync(
    std::string_view owner_scope, std::string_view lineage_id) const {
    if (owner_scope.empty() || lineage_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_current_lineage_head(impl_->connection, owner_scope, lineage_id);
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_run_lineage_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    auto read = load_run_lineage_and_head(impl_->connection, owner_scope, run_id, false);
    if (!read.id) return std::nullopt;
    if (!read.head) throw std::invalid_argument("Stored PostgreSQL run-lineage binding is corrupt");
    return std::move(read.head);
}

std::optional<ProgramRunLineage> PostgreSQLProgramTransitionStore::load_lineage_head_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const {
    if (owner_scope.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_historical_lineage_head(impl_->connection, owner_scope, lineage_id, head_id);
}

std::optional<ProgramRunGeneration> PostgreSQLProgramTransitionStore::load_generation_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_generation_record(impl_->connection, owner_scope, lineage_id, generation,
                                  &impl_->generations);
}

std::optional<ProgramTransitionPublication>
PostgreSQLProgramTransitionStore::load_generation_initial_publication_sync(
    std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner_scope.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_generation_initial_publication_record(
        impl_->connection, owner_scope, lineage_id, generation);
}

std::optional<GraphMigrationCapsule>
PostgreSQLProgramTransitionStore::load_graph_migration_capsule_sync(
    std::string_view owner_scope, std::string_view source_run_id,
    std::string_view source_lineage_head_id) const {
    if (owner_scope.empty() || source_run_id.empty() || source_lineage_head_id.empty())
        return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_graph_migration_capsule_record(
        impl_->connection, owner_scope, source_run_id, source_lineage_head_id);
}

std::optional<ProgramExecutionLease> PostgreSQLProgramTransitionStore::load_execution_lease_sync(
    std::string_view owner_scope, std::string_view run_id) const {
    if (owner_scope.empty() || run_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_execution_lease_record(impl_->connection, owner_scope, run_id);
}

std::optional<ProgramChildRelationHead> PostgreSQLProgramTransitionStore::load_child_relation_head_sync(
    std::string_view owner_scope, std::string_view parent_run_id) const {
    if (owner_scope.empty() || parent_run_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_relation_head_record(impl_->connection, owner_scope, parent_run_id);
}

std::optional<ProgramChildRelation> PostgreSQLProgramTransitionStore::load_child_relation_sync(
    std::string_view owner_scope, std::string_view parent_run_id, std::string_view child_run_id) const {
    if (owner_scope.empty() || parent_run_id.empty() || child_run_id.empty()) return std::nullopt;
    Impl::ConnectionLock lock(*impl_);
    return load_relation_row_record(impl_->connection, owner_scope, parent_run_id, child_run_id);
}

std::vector<ProgramChildRelation> PostgreSQLProgramTransitionStore::load_child_relations_sync(
    std::string_view owner_scope, std::string_view parent_run_id, bool open_only) const {
    if (owner_scope.empty() || parent_run_id.empty()) return {};
    Impl::ConnectionLock lock(*impl_);
    return load_relation_rows(impl_->connection, owner_scope, parent_run_id, open_only);
}

ProgramChildRelationOutcome PostgreSQLProgramTransitionStore::insert_child_relation_sync(
    std::string_view owner_scope, std::string_view parent_run_id, std::uint64_t expected_parent_fence,
    ProgramChildRecord child) {
    if (owner_scope.empty() || parent_run_id.empty())
        return ProgramChildRelationOutcome{ProgramChildRelationStatus::Refused,
                                           ProgramChildRelationRefusal::Invalid, std::nullopt,
                                           std::nullopt};
    Impl::ConnectionLock lock(*impl_);
    Transaction     transaction(impl_->connection, &impl_->session_identity);
    lock_owner(impl_->connection, owner_scope);
    PostgresReads   reads(impl_->connection, owner_scope, &impl_->run_record_cache);
    auto            planned = detail::transition::plan_relation_insert(
        reads, owner_scope, parent_run_id, expected_parent_fence, child, false);
    if (planned.status == ProgramChildRelationStatus::Applied)
        apply_relation_writes(impl_->connection, planned.row, planned.head, std::nullopt);
    transaction.commit();
    return ProgramChildRelationOutcome{planned.status, planned.refusal, std::move(planned.row),
                                       std::move(planned.head)};
}

ProgramChildRelationOutcome PostgreSQLProgramTransitionStore::update_child_relation_sync(
    std::string_view owner_scope, std::string_view parent_run_id, std::uint64_t expected_parent_fence,
    std::optional<std::uint64_t> expected_row_version, ProgramChildRecord next) {
    if (owner_scope.empty() || parent_run_id.empty())
        return ProgramChildRelationOutcome{ProgramChildRelationStatus::Refused,
                                           ProgramChildRelationRefusal::Invalid, std::nullopt,
                                           std::nullopt};
    Impl::ConnectionLock lock(*impl_);
    Transaction     transaction(impl_->connection, &impl_->session_identity);
    lock_owner(impl_->connection, owner_scope);
    PostgresReads   reads(impl_->connection, owner_scope, &impl_->run_record_cache);
    auto            planned = detail::transition::plan_relation_update(
        reads, owner_scope, parent_run_id, expected_parent_fence, expected_row_version, next, nullptr);
    if (planned.status == ProgramChildRelationStatus::Applied)
        apply_relation_writes(impl_->connection, planned.row, planned.head, std::nullopt);
    transaction.commit();
    return ProgramChildRelationOutcome{planned.status, planned.refusal, std::move(planned.row),
                                       std::move(planned.head)};
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
    // A takeover moves the run's head and its lineage head from under every hint of them.
    impl_->own_heads.erase(owner_scope, run_id);
    Impl::ConnectionLock lock(*impl_);
    Transaction transaction(impl_->connection, &impl_->session_identity);
    lock_owner(impl_->connection, owner_scope);
    const auto current =
        load_head(impl_->connection, owner_scope, run_id, true, &impl_->run_record_cache);
    if (!current) {
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    }
    std::optional<ProgramRunLineage> current_lineage;
    {
        auto read = load_run_lineage_and_head(impl_->connection, owner_scope, run_id, true);
        if (read.id) {
            if (!read.head) {
                transaction.commit();
                return ProgramTransitionPublishResult::Conflict;
            }
            current_lineage = std::move(read.head);
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
    if (takeover->run_lineage) impl_->own_heads.forget_lineage(owner_scope, takeover->run_lineage->lineage_id());
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
        owner_scope, expected_journal_head, expected_fence, publication, safe_point_evidence, safe_point_capsule,
        expected_lease, next_lease);
    if (!publication_bytes) return ProgramTransitionPublishResult::Conflict;
    const PublishArgs call{owner_scope, expected_journal_head, expected_fence, publication, *publication_bytes,
                           safe_point_evidence, safe_point_capsule, expected_lease, next_lease};
    return impl_->options.batched_publication ? impl_->publish_batched(call) : impl_->publish_sequential(call);
}

std::uint64_t PostgreSQLProgramTransitionStore::late_lock_count() const noexcept {
    return impl_->late_locks.load(std::memory_order_relaxed);
}

std::uint64_t PostgreSQLProgramTransitionStore::statements_sent() const noexcept {
    return impl_->statements.load(std::memory_order_relaxed);
}

std::vector<int> PostgreSQLProgramTransitionStore::connection_backend_pids() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->connection || PQstatus(impl_->connection) != CONNECTION_OK) return {};
    return {PQbackendPID(impl_->connection)};
}

void PostgreSQLProgramTransitionStore::Impl::note_published(
    const PublishArgs& call, const std::string& run_id, const std::string& run_record_bytes,
    const std::optional<ProgramOwnHeadHint>& previous_hint, bool run_existed) {
    const auto& publication = call.publication;
    // A stored generation never changes, and this publication stored it: the next read of it needs no statement.
    if (publication.run_generation)
        generations.put(call.owner_scope, publication.run_generation->lineage_id(),
                        publication.run_generation->generation(), *publication.run_generation);
    // A record without a terminal result and without runtime-only state is exactly what its stored bytes parse to: the
    // next read of the run finds it cached. A terminal result may carry state that its bytes do not (a live provider
    // cause), so a record that has one is dropped and the next read caches what the stored bytes parse to.
    if (publication.run_record.terminal_result() || publication.run_record.has_runtime_overlay())
        run_record_cache.erase(call.owner_scope, run_id);
    else
        run_record_cache.put(call.owner_scope, run_id, run_record_bytes, publication.run_record);
    detail::record_published_head(own_heads, call.owner_scope, run_id, call.expected_journal_head, previous_hint,
                                  publication, run_existed);
}

PostgreSQLProgramTransitionStore::Impl::ValidatedAttempt
PostgreSQLProgramTransitionStore::Impl::read_and_validate_sequential(
    const PublishArgs& call, const std::string& run_id, const std::optional<ProgramOwnHeadHint>& previous_hint,
    detail::PublicationTestHooks* test_hooks) {
    const auto& owner_scope = call.owner_scope;
    const auto& publication = call.publication;
    if (!(test_hooks && test_hooks->skip_owner_lock)) lock_owner(connection, owner_scope);
    auto locked = load_head_and_lease_for_update(connection, owner_scope, run_id, &run_record_cache);
    if (test_hooks && test_hooks->after_reads) test_hooks->after_reads();
    const auto& current       = locked.head;
    const auto& current_lease = locked.lease;

    std::optional<detail::transition::CurrentHead> head;
    if (current)
        head.emplace(detail::transition::CurrentHead{current->run_record, current->journal_record,
                                                     current->migration_plan,
                                                     current->last_publication_bytes});
    PostgresReads reads(connection, owner_scope, &run_record_cache, &generations);
    reads.know_head(run_id, current ? &current->run_record : nullptr);
    const bool stored_history_vouched =
        current && previous_hint &&
        previous_hint->journal_record.id == current->journal_record.id;
    ValidatedAttempt out;
    out.verdict = detail::transition::validate_transition(
        {owner_scope, call.expected_journal_head, call.expected_fence, publication, call.publication_bytes,
         call.safe_point_evidence, call.safe_point_capsule, call.expected_lease, call.next_lease,
         stored_history_vouched},
        head ? &*head : nullptr, current_lease ? &*current_lease : nullptr, reads, out.plan);
    out.current = std::move(locked.head);
    return out;
}

PostgreSQLProgramTransitionStore::Impl::ValidatedAttempt
PostgreSQLProgramTransitionStore::Impl::read_and_validate_batched(
    const PublishArgs& call, const std::string& run_id, const std::optional<ProgramOwnHeadHint>& previous_hint,
    detail::PublicationTestHooks* test_hooks) {
    const auto& owner_scope = call.owner_scope;
    const auto& publication = call.publication;
    detail::transition::TransitionRequest request{
        owner_scope, call.expected_journal_head, call.expected_fence, publication, call.publication_bytes,
        call.safe_point_evidence, call.safe_point_capsule, call.expected_lease, call.next_lease, false};
    const auto lock_plan = detail::transition::plan_publication_locks(request);
    const auto groups    = read_group_statements(
        owner_scope, lock_plan, publication.commands.empty() ? std::string_view() : std::string_view(run_id));

    // BEGIN, the owner lock and the read groups: one pipeline, or one statement after another.
    std::vector<SqlStatement> first_batch;
    first_batch.push_back({"BEGIN", {}, {}, "BEGIN"});
    if (!(test_hooks && test_hooks->skip_owner_lock)) first_batch.push_back(lock_owner_statement(owner_scope));
    const auto group_offset = first_batch.size();
    for (const auto& group : groups) first_batch.push_back(group.statement);
    auto first_results = run_statements(connection, first_batch, pipeline_enabled, test_hooks);
    std::vector<PgResult> results;
    results.reserve(groups.size());
    for (std::size_t index = group_offset; index < first_results.size(); ++index)
        results.push_back(std::move(first_results[index]));
    BatchedPostgresReads reads(connection, owner_scope, &run_record_cache, &generations, &late_locks);
    seed_read_groups(owner_scope, run_id, lock_plan, groups, results, &run_record_cache, reads);
    if (test_hooks && test_hooks->after_reads) test_hooks->after_reads();

    const auto& current       = reads.head_of(run_id);
    const auto& current_lease = reads.lease_of(run_id);
    std::optional<detail::transition::CurrentHead> head;
    if (current)
        head.emplace(detail::transition::CurrentHead{current->run_record, current->journal_record,
                                                     current->migration_plan,
                                                     current->last_publication_bytes});
    reads.know_head(run_id, current ? &current->run_record : nullptr);
    request.stored_history_vouched =
        current && previous_hint &&
        previous_hint->journal_record.id == current->journal_record.id;
    ValidatedAttempt out;
    out.verdict = detail::transition::validate_transition(
        request, head ? &*head : nullptr, current_lease ? &*current_lease : nullptr, reads, out.plan);
    out.current = current;
    return out;
}

std::optional<ProgramTransitionPublishResult> PostgreSQLProgramTransitionStore::Impl::recheck_after_lost_commit(
    const PublishArgs& call, const std::string& run_id, const std::string& run_record_bytes,
    const std::optional<ProgramOwnHeadHint>& previous_hint, bool run_existed, SessionIdentity old_session,
    detail::PublicationTestHooks* test_hooks) {
    reset_connection(connection, &session_identity);
    if (!is_clean_idle_connection(connection) || !old_session.known() ||
        (test_hooks && test_hooks->unknown_session_identity))
        throw ProgramTransitionOutcomeUnknown(
            "PostgreSQL Program transition outcome cannot be checked without a live replacement session and previous "
            "session identity");
    try {
        const char* terminate_sql =
            (test_hooks && test_hooks->fail_recheck_terminate)
                ? "SELECT pg_terminate_backend(pid) FROM neograph_program_no_such_table_v1 "
                  "WHERE pid = $1 AND backend_start = $2"
                : "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE pid = $1 AND backend_start = $2";
        (void)exec_params(connection, terminate_sql, {std::to_string(old_session.pid), old_session.backend_start});
    } catch (...) {
        if (!is_clean_idle_connection(connection)) reset_connection(connection, &session_identity);
        throw ProgramTransitionOutcomeUnknown(
            "PostgreSQL Program transition outcome check could not fence the previous session");
    }

    ValidatedAttempt validated;
    try {
        if (options.batched_publication) {
            Transaction recheck(connection, Transaction::BegunByBatch{}, &session_identity);
            validated = read_and_validate_batched(call, run_id, previous_hint, nullptr);
            recheck.commit();
        } else {
            Transaction recheck(connection, &session_identity);
            validated = read_and_validate_sequential(call, run_id, previous_hint, nullptr);
            recheck.commit();
        }
    } catch (...) {
        throw ProgramTransitionOutcomeUnknown(
            "PostgreSQL Program transition outcome check failed while reading stored state");
    }

    switch (validated.verdict) {
    case detail::transition::TransitionVerdict::AlreadyPresent:
        note_published(call, run_id, run_record_bytes, previous_hint, run_existed);
        return ProgramTransitionPublishResult::Published;
    case detail::transition::TransitionVerdict::Conflict:
        return ProgramTransitionPublishResult::Conflict;
    case detail::transition::TransitionVerdict::Proceed:
        return std::nullopt;
    }
    return std::nullopt;
}

// One statement at a time, each answered before the next is sent: the behaviour the batched path is checked against.
ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::Impl::publish_sequential(const PublishArgs& call) {
    const auto&                                   owner_scope = call.owner_scope;
    const auto&                                   publication = call.publication;
    const std::string                             run_id      = publication.run_record.run_id();
    std::shared_ptr<detail::PublicationTestHooks> test_hooks;
    {
        std::lock_guard hook_lock(hooks_mutex);
        test_hooks = hooks;
    }
    // Only a publication that publishes leaves a hint of the run: every other way out forgets it.
    const auto previous_hint = own_heads.get(owner_scope, run_id);
    own_heads.erase(owner_scope, run_id);
    ConnectionLock lock(*this);
    const auto     old_session = session_identity;
    Transaction    transaction(connection, &session_identity);
    const auto     validated = read_and_validate_sequential(call, run_id, previous_hint, test_hooks.get());
    switch (validated.verdict) {
    case detail::transition::TransitionVerdict::Conflict:
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    case detail::transition::TransitionVerdict::AlreadyPresent:
        transaction.commit();
        return ProgramTransitionPublishResult::AlreadyPresent;
    case detail::transition::TransitionVerdict::Proceed:
        break;
    }
    if (test_hooks && test_hooks->before_write) test_hooks->before_write();

    const auto run_record_bytes = publication.run_record.serialize_canonical();
    if (test_hooks) {
        for (const auto& sql : test_hooks->extra_write_statements)
            exec_write(connection, SqlStatement{sql, {}, {}, {}});
        if (test_hooks->fail_write)
            exec_write(connection,
                       SqlStatement{"INSERT INTO neograph_program_transition_run_heads_v2(owner_scope, run_id) "
                                    "VALUES(NULL, NULL)",
                                    {}, {}, {}});
    }
    for (const auto& statement :
         publication_write_statements(call, run_id, run_record_bytes, validated.current, validated.plan))
        exec_write(connection, statement);
    if (test_hooks && test_hooks->before_commit) test_hooks->before_commit();
    try {
        transaction.commit(test_hooks ? &test_hooks->after_send : nullptr,
                           test_hooks && test_hooks->fail_commit_send);
    } catch (const PgError& error) {
        if (is_server_statement_error(connection, error)) throw;
        transaction.ended_by_reset();
        if (const auto outcome = recheck_after_lost_commit(call, run_id, run_record_bytes, previous_hint,
                                                           validated.current.has_value(), old_session,
                                                           test_hooks.get()))
            return *outcome;
        throw;
    } catch (...) {
        transaction.ended_by_reset();
        if (const auto outcome = recheck_after_lost_commit(call, run_id, run_record_bytes, previous_hint,
                                                           validated.current.has_value(), old_session,
                                                           test_hooks.get()))
            return *outcome;
        throw;
    }
    note_published(call, run_id, run_record_bytes, previous_hint, validated.current.has_value());
    return ProgramTransitionPublishResult::Published;
}

void PostgreSQLProgramTransitionStore::set_publication_test_hooks(std::shared_ptr<detail::PublicationTestHooks> hooks) {
    std::lock_guard lock(impl_->hooks_mutex);
    impl_->hooks = std::move(hooks);
}

// The rows the publication will lock are read in ordered groups before the validator runs, and seed its memos; the
// validator is the sequential path's, and the writes are the same statements.
ProgramTransitionPublishResult PostgreSQLProgramTransitionStore::Impl::publish_batched(const PublishArgs& call) {
    const std::string run_id = call.publication.run_record.run_id();
    // Only a publication that publishes leaves a hint of the run: every other way out forgets it.
    const auto previous_hint = own_heads.get(call.owner_scope, run_id);
    own_heads.erase(call.owner_scope, run_id);
    // A row that the validator saw as absent and another writer inserted before this one wrote it is a lost race: the
    // transaction rolls back and the publication is decided again against what is stored, as often as
    // Options::lost_race_replans allows; the loss after that is a Conflict.
    for (std::size_t attempt = 0; attempt <= options.lost_race_replans; ++attempt)
        if (const auto result = publish_batched_attempt(call, run_id, previous_hint)) return *result;
    return ProgramTransitionPublishResult::Conflict;
}

std::optional<ProgramTransitionPublishResult> PostgreSQLProgramTransitionStore::Impl::publish_batched_attempt(
    const PublishArgs& call, const std::string& run_id, const std::optional<ProgramOwnHeadHint>& previous_hint) {
    const auto&                                   publication = call.publication;
    std::shared_ptr<detail::PublicationTestHooks> test_hooks;
    {
        std::lock_guard hook_lock(hooks_mutex);
        test_hooks = hooks;
    }
    ConnectionLock lock(*this);
    const auto     old_session = session_identity;
    Transaction    transaction(connection, Transaction::BegunByBatch{}, &session_identity);
    const auto     validated = read_and_validate_batched(call, run_id, previous_hint, test_hooks.get());
    switch (validated.verdict) {
    case detail::transition::TransitionVerdict::Conflict:
        transaction.commit();
        return ProgramTransitionPublishResult::Conflict;
    case detail::transition::TransitionVerdict::AlreadyPresent:
        transaction.commit();
        return ProgramTransitionPublishResult::AlreadyPresent;
    case detail::transition::TransitionVerdict::Proceed:
        break;
    }
    if (test_hooks && test_hooks->before_write) test_hooks->before_write();

    const auto&               current          = validated.current;
    const auto&               plan             = validated.plan;
    const auto                run_record_bytes = publication.run_record.serialize_canonical();
    std::vector<SqlStatement> writes;
    if (const auto parts = single_write_parts(call, current, plan))
        writes.push_back(single_write_statement(*parts, call, run_id, run_record_bytes, current, plan));
    else
        writes = publication_write_statements(call, run_id, run_record_bytes, current, plan);
    if (test_hooks && test_hooks->fail_write)
        writes.insert(writes.begin() + 1,
                      SqlStatement{"INSERT INTO neograph_program_transition_run_heads_v2(owner_scope, run_id) "
                                   "VALUES(NULL, NULL)",
                                   {}, {}, {}});
    if (test_hooks && !test_hooks->extra_write_statements.empty()) {
        std::vector<SqlStatement> prefix;
        prefix.reserve(test_hooks->extra_write_statements.size() + writes.size());
        for (const auto& sql : test_hooks->extra_write_statements) prefix.push_back(SqlStatement{sql, {}, {}, {}});
        for (auto& statement : writes) prefix.push_back(std::move(statement));
        writes = std::move(prefix);
    }
    if (test_hooks && test_hooks->before_commit) test_hooks->before_commit();
    writes.push_back({"COMMIT", {}, {}, "COMMIT"});
    // Rows this publication inserts because the validator found them absent: a unique violation on them means another
    // writer inserted the row first.
    const bool inserts_absent_rows =
        !current || (publication.run_lineage && (!plan.lineage_head_exists || !plan.run_lineage_recorded));
    try {
        (void)run_statements(connection, writes, pipeline_enabled, test_hooks.get(),
                             test_hooks ? &test_hooks->after_send : nullptr,
                             test_hooks && test_hooks->fail_commit_send);
    } catch (const PgError& error) {
        if (is_server_statement_error(connection, error)) {
            if (inserts_absent_rows && error.sqlstate() == UNIQUE_VIOLATION) return std::nullopt;
            throw;
        }
        transaction.ended_by_reset();
        if (const auto outcome = recheck_after_lost_commit(call, run_id, run_record_bytes, previous_hint,
                                                           current.has_value(), old_session, test_hooks.get()))
            return *outcome;
        throw;
    } catch (...) {
        transaction.ended_by_reset();
        if (const auto outcome = recheck_after_lost_commit(call, run_id, run_record_bytes, previous_hint,
                                                           current.has_value(), old_session, test_hooks.get()))
            return *outcome;
        throw;
    }
    transaction.committed_by_batch();
    note_published(call, run_id, run_record_bytes, previous_hint, current.has_value());
    return ProgramTransitionPublishResult::Published;
}

}  // namespace neograph::program
