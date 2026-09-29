#include <neograph/program/sqlite_core_tool_grant_store.h>

#include <sqlite3.h>

#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neograph::program {
namespace {

[[noreturn]] void sqlite_error(sqlite3* db, std::string_view operation) {
    throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(db));
}

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        sqlite_error(db, "Core Tool grant transaction");
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            sqlite_error(db_, "Prepare Core Tool grant statement");
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { sqlite3_finalize(stmt_); }

    void text(int index, std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK)
            sqlite_error(db_, "Bind Core Tool grant text");
    }
    void attempt(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
            sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            throw std::invalid_argument("Core Tool grant attempt is out of SQLite range");
    }
    bool row() {
        const auto status = sqlite3_step(stmt_);
        if (status == SQLITE_ROW) return true;
        if (status == SQLITE_DONE) return false;
        sqlite_error(db_, "Read Core Tool grant");
    }
    void done() {
        if (sqlite3_step(stmt_) != SQLITE_DONE)
            sqlite_error(db_, "Write Core Tool grant");
    }
    sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (active_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execute(db_, "COMMIT");
        active_ = false;
    }
private:
    sqlite3* db_;
    bool active_ = true;
};

std::string column_text(sqlite3_stmt* stmt, int index) {
    const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    const auto size = sqlite3_column_bytes(stmt, index);
    if (!value || size < 0) throw std::runtime_error("Corrupt Core Tool grant record");
    return {value, static_cast<std::size_t>(size)};
}

void bind_identity(Statement& stmt, const ProgramCoreToolGrantContext& context) {
    stmt.text(1, context.owner_scope);
    stmt.text(2, context.program_version_id);
    stmt.text(3, context.run_id);
    stmt.text(4, context.operation_id);
    stmt.attempt(5, context.attempt);
    stmt.text(6, context.binding_fingerprint);
}

void validate(const ProgramCoreToolGrantRecord& record) {
    if (record.owner_scope.empty() || record.program_version_id.empty() ||
        record.run_id.empty() || record.operation_id.empty() ||
        record.attempt == 0 || record.binding_fingerprint.empty() ||
        record.grant_id.empty())
        throw std::invalid_argument("Core Tool grant identity must be complete");
}

}  // namespace

struct SQLiteProgramCoreToolGrantStore::Impl {
    explicit Impl(const std::string& path) {
        if (path.empty()) throw std::invalid_argument("Core Tool grant database path is empty");
        if (sqlite3_open_v2(path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string reason = db ? sqlite3_errmsg(db) : "SQLite unavailable";
            if (db) sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("Open Core Tool grant database: " + reason);
        }
        try {
            sqlite3_busy_timeout(db, 5000);
            execute(db, "CREATE TABLE IF NOT EXISTS program_core_tool_grants ("
                        "owner_scope TEXT NOT NULL, program_version_id TEXT NOT NULL, "
                        "run_id TEXT NOT NULL, operation_id TEXT NOT NULL, "
                        "attempt INTEGER NOT NULL, binding_fingerprint TEXT NOT NULL, "
                        "grant_id TEXT NOT NULL, revoked INTEGER NOT NULL DEFAULT 0, "
                        "PRIMARY KEY (owner_scope, program_version_id, run_id, operation_id, attempt), "
                        "CHECK (attempt > 0), CHECK (grant_id <> ''), "
                        "CHECK (revoked IN (0, 1)))");
            execute(db, "CREATE INDEX IF NOT EXISTS program_core_tool_grants_by_grant "
                        "ON program_core_tool_grants (owner_scope, grant_id)");
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }
    ~Impl() { sqlite3_close(db); }
    sqlite3* db = nullptr;
    mutable std::mutex mutex;
};

SQLiteProgramCoreToolGrantStore::SQLiteProgramCoreToolGrantStore(std::string database_path)
    : impl_(std::make_unique<Impl>(database_path)) {}
SQLiteProgramCoreToolGrantStore::~SQLiteProgramCoreToolGrantStore() = default;

ProgramCoreToolGrantAdmission SQLiteProgramCoreToolGrantStore::admit(
    const ProgramCoreToolGrantRecord& record) {
    validate(record);
    std::lock_guard lock(impl_->mutex);
    auto* db = impl_->db;
    Transaction transaction(db);
    Statement same_grant(db, "SELECT program_version_id,run_id,operation_id,"
        "binding_fingerprint,revoked FROM program_core_tool_grants "
        "WHERE owner_scope=?1 AND grant_id=?2 LIMIT 1");
    same_grant.text(1, record.owner_scope);
    same_grant.text(2, record.grant_id);
    if (same_grant.row() &&
        (column_text(same_grant.get(), 0) != record.program_version_id ||
         column_text(same_grant.get(), 1) != record.run_id ||
         column_text(same_grant.get(), 2) != record.operation_id ||
         column_text(same_grant.get(), 3) != record.binding_fingerprint ||
         sqlite3_column_int(same_grant.get(), 4) != 0)) {
        transaction.commit();
        return ProgramCoreToolGrantAdmission::Conflict;
    }
    Statement insert(db, "INSERT OR IGNORE INTO program_core_tool_grants "
        "(owner_scope,program_version_id,run_id,operation_id,attempt,binding_fingerprint,grant_id) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7)");
    insert.text(1, record.owner_scope);
    insert.text(2, record.program_version_id);
    insert.text(3, record.run_id);
    insert.text(4, record.operation_id);
    insert.attempt(5, record.attempt);
    insert.text(6, record.binding_fingerprint);
    insert.text(7, record.grant_id);
    insert.done();
    if (sqlite3_changes(db) == 1) {
        transaction.commit();
        return ProgramCoreToolGrantAdmission::Admitted;
    }
    Statement existing(db, "SELECT binding_fingerprint, grant_id, revoked "
        "FROM program_core_tool_grants WHERE owner_scope=?1 AND program_version_id=?2 "
        "AND run_id=?3 AND operation_id=?4 AND attempt=?5");
    existing.text(1, record.owner_scope);
    existing.text(2, record.program_version_id);
    existing.text(3, record.run_id);
    existing.text(4, record.operation_id);
    existing.attempt(5, record.attempt);
    const bool identical = existing.row() &&
        column_text(existing.get(), 0) == record.binding_fingerprint &&
        column_text(existing.get(), 1) == record.grant_id &&
        sqlite3_column_int(existing.get(), 2) == 0;
    transaction.commit();
    return identical ? ProgramCoreToolGrantAdmission::AlreadyPresent
                     : ProgramCoreToolGrantAdmission::Conflict;
}

std::optional<ProgramCoreToolGrantRecord> SQLiteProgramCoreToolGrantStore::load(
    const ProgramCoreToolGrantContext& context) const {
    std::lock_guard lock(impl_->mutex);
    Statement statement(impl_->db, "SELECT grant_id FROM program_core_tool_grants "
        "WHERE owner_scope=?1 AND program_version_id=?2 AND run_id=?3 "
        "AND operation_id=?4 AND attempt=?5 AND binding_fingerprint=?6 AND revoked=0");
    bind_identity(statement, context);
    if (!statement.row()) return std::nullopt;
    auto grant_id = column_text(statement.get(), 0);
    if (grant_id.empty()) throw std::runtime_error("Corrupt Core Tool grant ID");
    return ProgramCoreToolGrantRecord{
        std::string(context.owner_scope), std::string(context.program_version_id),
        std::string(context.run_id), std::string(context.operation_id),
        context.attempt, std::string(context.binding_fingerprint), std::move(grant_id)};
}

bool SQLiteProgramCoreToolGrantStore::revoke(
    const ProgramCoreToolGrantContext& context) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->db);
    Statement lookup(impl_->db, "SELECT grant_id FROM program_core_tool_grants "
        "WHERE owner_scope=?1 AND program_version_id=?2 AND run_id=?3 "
        "AND operation_id=?4 AND attempt=?5 AND binding_fingerprint=?6 AND revoked=0");
    bind_identity(lookup, context);
    if (!lookup.row()) return false;
    const auto grant_id = column_text(lookup.get(), 0);
    Statement statement(impl_->db, "UPDATE program_core_tool_grants SET revoked=1 "
        "WHERE owner_scope=?1 AND grant_id=?2 AND revoked=0");
    statement.text(1, context.owner_scope);
    statement.text(2, grant_id);
    statement.done();
    const bool updated = sqlite3_changes(impl_->db) > 0;
    transaction.commit();
    return updated;
}

}  // namespace neograph::program
