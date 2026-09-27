#include <neograph/sqlite_runtime_stores.h>
#include "canonical_json.h"

#include <sqlite3.h>

#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace neograph {
namespace {

[[noreturn]] void db_error(sqlite3* db, const char* operation) {
    throw std::runtime_error(std::string("SQLite Tool effect ") + operation + ": " + sqlite3_errmsg(db));
}

void sql(sqlite3* db, const char* command) {
    char* error = nullptr;
    if (sqlite3_exec(db, command, nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("SQLite Tool effect: " + message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* command) : db_(db) {
        if (sqlite3_prepare_v2(db, command, -1, &stmt_, nullptr) != SQLITE_OK)
            db_error(db, "prepare");
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, const std::string& value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::length_error("SQLite Tool effect identity exceeds SQLite limit");
        if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) db_error(db_, "bind");
    }
    void integer(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()))
            throw std::length_error("SQLite Tool effect ordinal exceeds SQLite limit");
        if (sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
            db_error(db_, "bind ordinal");
    }
    int step() {
        const int result = sqlite3_step(stmt_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) db_error(db_, "step");
        return result;
    }
    sqlite3_stmt* raw() const { return stmt_; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { sql(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) (void)sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { sql(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

std::string column(sqlite3_stmt* statement, int index) {
    const char* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
    const int length = sqlite3_column_bytes(statement, index);
    if (!bytes || length < 0) throw std::runtime_error("SQLite Tool effect missing receipt binding");
    return {bytes, static_cast<std::size_t>(length)};
}

void slot(Statement& statement, const ToolEffectIdentity& identity) {
    statement.text(1, identity.owner_scope);
    statement.text(2, identity.run_id);
    statement.text(3, identity.thread_id);
    statement.text(4, identity.task_id);
    statement.integer(5, identity.call_ordinal);
}

ToolExecutionResult refusal(ToolTerminalStatus status, const std::string& error) {
    ToolExecutionResult result;
    result.status = status;
    result.error = error;
    result.effect_uncertain = status == ToolTerminalStatus::ReconciliationRequired;
    return result;
}

json encode(const ToolExecutionResult& result) {
    return json{{"status", std::string(to_string(result.status))},
                {"output", result.output}, {"error", result.error},
                {"retryable", result.retryable},
                {"effect_uncertain", result.effect_uncertain},
                {"exit_code", result.exit_code ? json(*result.exit_code) : json(nullptr)},
                {"signal_number", result.signal_number ? json(*result.signal_number) : json(nullptr)},
                {"output_truncated", result.output_truncated}};
}

ToolExecutionResult decode(const std::string& encoded) {
    const auto data = json::parse(encoded);
    ToolExecutionResult result;
    result.status = tool_terminal_status_from_string(data.at("status").get<std::string>());
    result.output = data.at("output").get<std::string>();
    result.error = data.at("error").get<std::string>();
    result.retryable = data.at("retryable").get<bool>();
    result.effect_uncertain = data.at("effect_uncertain").get<bool>();
    if (!data.at("exit_code").is_null()) result.exit_code = data.at("exit_code").get<int>();
    if (!data.at("signal_number").is_null())
        result.signal_number = data.at("signal_number").get<int>();
    result.output_truncated = data.at("output_truncated").get<bool>();
    if (!result.succeeded() || result.effect_uncertain)
        throw std::runtime_error("SQLite Tool effect receipt is not a confirmed completion");
    return result;
}

} // namespace

struct SQLiteToolEffectBroker::Impl {
    struct Binding { std::string executable_id; std::string tool_name; };
    sqlite3* db = nullptr;
    std::mutex mutex;
    std::unordered_map<const Tool*, Binding> bindings;
    ~Impl() { if (db) sqlite3_close(db); }
};

SQLiteToolEffectBroker::SQLiteToolEffectBroker(
    std::string database_path, std::vector<SQLiteToolExecutableBinding> bindings)
    : impl_(std::make_unique<Impl>()) {
    if (database_path.empty() || database_path == ":memory:" ||
        database_path.starts_with("file:"))
        throw std::invalid_argument("Tool effect journal requires a durable filesystem path");
    for (auto& binding : bindings) {
        if (!binding.tool || binding.executable_id.empty() || binding.tool->get_name().empty())
            throw std::invalid_argument("Tool effect executable binding must name a Tool and exact executable");
        if (!impl_->bindings.emplace(binding.tool, Impl::Binding{
                std::move(binding.executable_id), binding.tool->get_name()}).second)
            throw std::invalid_argument("duplicate Tool executable binding");
    }
    if (sqlite3_open_v2(database_path.c_str(), &impl_->db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) db_error(impl_->db, "open");
    if (sqlite3_busy_timeout(impl_->db, 5000) != SQLITE_OK) db_error(impl_->db, "busy timeout");
    sql(impl_->db, "PRAGMA journal_mode=WAL");
    sql(impl_->db, "PRAGMA synchronous=FULL");
    sql(impl_->db, R"(
        CREATE TABLE IF NOT EXISTS neograph_tool_effects (
            owner TEXT NOT NULL, run TEXT NOT NULL, thread TEXT NOT NULL,
            task TEXT NOT NULL, ordinal INTEGER NOT NULL,
            program_version TEXT NOT NULL, operation TEXT NOT NULL, grant_id TEXT NOT NULL,
            binding_fingerprint TEXT NOT NULL, executable TEXT NOT NULL,
            first_attempt INTEGER NOT NULL,
            tool_name TEXT NOT NULL, arguments TEXT NOT NULL,
            receipt TEXT,
            PRIMARY KEY (owner, run, thread, task, ordinal)
        )
    )");
}

SQLiteToolEffectBroker::~SQLiteToolEffectBroker() = default;

asio::awaitable<ToolExecutionResult> SQLiteToolEffectBroker::execute(
    ToolEffectIdentity identity, Tool& tool, json arguments, ToolExecutionContext context) {
    if (identity.owner_scope.empty() || identity.run_id.empty() || identity.thread_id.empty() ||
        identity.task_id.empty() || identity.operation_id.empty() || identity.grant_id.empty())
        co_return refusal(ToolTerminalStatus::Rejected, "Tool effect requires exact host grant and stable slot");
    if (!identity.program_version_id.empty() &&
        (identity.binding_fingerprint.empty() || identity.attempt == 0))
        co_return refusal(ToolTerminalStatus::Rejected,
                         "Program Tool effect requires exact admitted binding and attempt");
    const auto found = impl_->bindings.find(&tool);
    if (found == impl_->bindings.end() || tool.get_name() != found->second.tool_name)
        co_return refusal(ToolTerminalStatus::Rejected, "Tool executable is not bound by the host");
    const auto& binding = found->second;
    auto controller = context.controller ? context.controller : default_tool_execution_controller();
    // Internal controller retries are separate physical dispatches invisible to
    // this one marker. Require one physical attempt even for idempotent tools.
    if (controller->policies()->resolve(tool.get_name()).retry_max_attempts != 1)
        co_return refusal(ToolTerminalStatus::Rejected, "Tool effect controller must not retry within one slot");
    const std::string canonical_arguments = detail::canonical_json_bytes(arguments);

    try {
        std::lock_guard lock(impl_->mutex);
        Transaction tx(impl_->db);
        Statement select(impl_->db, R"(
            SELECT program_version, operation, grant_id, binding_fingerprint,
                   executable, tool_name, arguments, receipt
            FROM neograph_tool_effects WHERE owner=? AND run=? AND thread=? AND task=? AND ordinal=?
        )");
        slot(select, identity);
        if (select.step() == SQLITE_ROW) {
            auto* row = select.raw();
            const bool same = column(row, 0) == identity.program_version_id &&
                column(row, 1) == identity.operation_id && column(row, 2) == identity.grant_id &&
                column(row, 3) == identity.binding_fingerprint &&
                column(row, 4) == binding.executable_id && column(row, 5) == binding.tool_name &&
                column(row, 6) == canonical_arguments;
            if (!same) co_return refusal(ToolTerminalStatus::Rejected,
                                         "Tool effect slot, authority, executable or arguments changed");
            if (sqlite3_column_type(row, 7) != SQLITE_NULL) {
                const auto receipt = column(row, 7);
                tx.commit();
                try {
                    co_return decode(receipt);
                } catch (const std::exception&) {
                    co_return refusal(ToolTerminalStatus::ReconciliationRequired,
                                     "Tool effect stored receipt cannot be verified");
                }
            }
            co_return refusal(ToolTerminalStatus::ReconciliationRequired,
                             "Tool effect dispatched without confirmed receipt; reconcile externally");
        }
        Statement insert(impl_->db, R"(
            INSERT INTO neograph_tool_effects
              (owner, run, thread, task, ordinal, program_version, operation, grant_id,
               binding_fingerprint, executable, tool_name, arguments, first_attempt)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )");
        slot(insert, identity);
        insert.text(6, identity.program_version_id);
        insert.text(7, identity.operation_id);
        insert.text(8, identity.grant_id);
        insert.text(9, identity.binding_fingerprint);
        insert.text(10, binding.executable_id);
        insert.text(11, binding.tool_name);
        insert.text(12, canonical_arguments);
        insert.integer(13, identity.attempt);
        (void)insert.step();
        tx.commit(); // FULL-sync durable before any external effect is dispatched.
    } catch (const std::exception& error) {
        // A failed or ambiguous commit has not dispatched anything. If it
        // actually committed, a later attempt sees the unresolved marker.
        co_return refusal(ToolTerminalStatus::Failed,
                          std::string("Tool effect marker not confirmed: ") + error.what());
    }

    ToolExecutionResult result;
    try {
        result = co_await controller->execute_result_async(tool, std::move(arguments), std::move(context));
    } catch (...) {
        // Including cancellation: the marker remains pending on disk.
        throw;
    }
    if (!result.succeeded() || result.effect_uncertain)
        co_return refusal(ToolTerminalStatus::ReconciliationRequired,
                         "Tool effect outcome after dispatch is unconfirmed");
    try {
        std::lock_guard lock(impl_->mutex);
        Transaction tx(impl_->db);
        Statement settle(impl_->db, R"(
            UPDATE neograph_tool_effects SET receipt=?
            WHERE owner=? AND run=? AND thread=? AND task=? AND ordinal=? AND receipt IS NULL
        )");
        settle.text(1, encode(result).dump());
        settle.text(2, identity.owner_scope);
        settle.text(3, identity.run_id);
        settle.text(4, identity.thread_id);
        settle.text(5, identity.task_id);
        settle.integer(6, identity.call_ordinal);
        (void)settle.step();
        if (sqlite3_changes(impl_->db) != 1)
            throw std::runtime_error("Tool effect receipt missing or already settled");
        tx.commit();
    } catch (const std::exception&) {
        co_return refusal(ToolTerminalStatus::ReconciliationRequired,
                         "Tool effect completed but receipt persistence is unconfirmed");
    }
    co_return result;
}

} // namespace neograph
