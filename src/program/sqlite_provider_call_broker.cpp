#include <neograph/program/sqlite_provider_call_broker.h>

#include "../core/canonical_json.h"

#include <sqlite3.h>

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {
constexpr std::string_view domain = "program-core-provider-call/v1";

[[noreturn]] void sql_error(sqlite3* db) {
    throw std::runtime_error(std::string("Program provider journal SQLite error: ") + sqlite3_errmsg(db));
}
void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) sql_error(db);
}
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            const auto error = std::string("Program provider journal SQLite prepare: ") +
                               sqlite3_errmsg(db);
            sqlite3_finalize(stmt_);
            throw std::runtime_error(error);
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    void bind(int index, std::string_view value) {
        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            throw std::length_error("Program provider journal field is too large");
        if (sqlite3_bind_text(stmt_, index, value.empty() ? "" : value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            sql_error(db_);
    }
    bool next() {
        const auto result = sqlite3_step(stmt_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        sql_error(db_);
    }
    void done() {
        if (next()) throw std::runtime_error("Unexpected SQLite journal row");
    }
    std::string column(int index) const {
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, index));
        const int size = sqlite3_column_bytes(stmt_, index);
        if (!data) throw std::runtime_error("Corrupt Program provider journal field");
        return {data, static_cast<size_t>(size)};
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};

std::string hash(std::string_view purpose, const json& value) {
    return detail::sha256_identity(domain, purpose, detail::canonical_json_bytes(value));
}
json completion_json(const ChatCompletion& completion) {
    json message;
    to_json(message, completion.message);
    json artifacts = json::array();
    for (const auto& artifact : completion.artifacts) {
        artifacts.push_back({{"kind", artifact.kind}, {"mime_type", artifact.mime_type},
                             {"base64_data", artifact.base64_data}, {"url", artifact.url},
                             {"file_id", artifact.file_id}, {"metadata", artifact.metadata}});
    }
    return {{"message", std::move(message)}, {"artifacts", std::move(artifacts)},
            {"stop_reason", completion.stop_reason},
            {"usage", {{"prompt_tokens", completion.usage.prompt_tokens},
                       {"completion_tokens", completion.usage.completion_tokens},
                       {"total_tokens", completion.usage.total_tokens},
                       {"cached_prompt_tokens", completion.usage.cached_prompt_tokens},
                       {"reasoning_tokens", completion.usage.reasoning_tokens}}}};
}
ChatCompletion parse_completion(std::string_view canonical) {
    auto value = detail::parse_json_strict(canonical);
    // Legacy receipts omitted media entirely, so absence cannot prove an empty
    // artifact result. Fail closed rather than inventing a successful replay.
    if (!value.contains("artifacts"))
        throw std::runtime_error(
            "Legacy Program provider completion lacks artifact evidence; replay unavailable");
    if (!value.at("artifacts").is_array())
        throw std::runtime_error("Corrupt Program provider completion artifacts");
    ChatCompletion completion;
    from_json(value.at("message"), completion.message);
    completion.artifacts.reserve(value.at("artifacts").size());
    for (const auto& artifact : value.at("artifacts")) {
        completion.artifacts.push_back({
            artifact.at("kind").get<std::string>(),
            artifact.at("mime_type").get<std::string>(),
            artifact.at("base64_data").get<std::string>(),
            artifact.at("url").get<std::string>(),
            artifact.at("file_id").get<std::string>(),
            artifact.at("metadata")});
    }
    completion.stop_reason = value.at("stop_reason").get<std::string>();
    const auto& usage = value.at("usage");
    completion.usage.prompt_tokens = usage.at("prompt_tokens").get<int>();
    completion.usage.completion_tokens = usage.at("completion_tokens").get<int>();
    completion.usage.total_tokens = usage.at("total_tokens").get<int>();
    completion.usage.cached_prompt_tokens = usage.at("cached_prompt_tokens").get<int>();
    completion.usage.reasoning_tokens = usage.at("reasoning_tokens").get<int>();
    if (detail::canonical_json_bytes(completion_json(completion)) != canonical)
        throw std::runtime_error("Corrupt Program provider completion");
    return completion;
}
std::string request_digest(const CompletionParams& params, bool streaming) {
    json messages = json::array();
    for (const auto& message : params.messages) {
        json encoded;
        to_json(encoded, message);
        messages.push_back(std::move(encoded));
    }
    json tools = json::array();
    for (const auto& tool : params.tools)
        tools.push_back({{"name", tool.name}, {"description", tool.description},
                         {"parameters", tool.parameters}});
    // Cancellation is transport control, not request content. All per-call
    // fields, including the callback mode, are bound to the immutable slot.
    return hash("request", {{"model", params.model}, {"messages", std::move(messages)},
                            {"tools", std::move(tools)}, {"temperature", params.temperature},
                            {"max_tokens", params.max_tokens},
                            {"extra_fields", params.extra_fields},
                            {"timeout_seconds", params.timeout_seconds},
                            {"streaming", streaming}});
}
std::string dispatch_id(const ProgramCoreProviderCallContext& context,
                        const graph::ProviderCallIdentity& call) {
    return hash("slot", {{"owner_scope", std::string(context.owner_scope)},
                         {"program_version_id", std::string(context.program_version_id)},
                         {"run_id", std::string(context.run_id)},
                         {"operation_id", std::string(context.operation_id)},
                         {"thread_id", call.thread_id}, {"task_id", call.task_id},
                         {"node_name", call.node_name}, {"call_ordinal", call.call_ordinal}});
}
void validate_context(const ProgramCoreProviderCallContext& context) {
    if (context.owner_scope.empty() || context.program_version_id.empty() ||
        context.run_id.empty() || context.operation_id.empty() || !context.attempt)
        throw std::invalid_argument("Program provider journal requires exact Program scope and attempt");
}
}  // namespace

struct SQLiteProgramProviderCallJournal::Impl {
    explicit Impl(const std::string& path) {
        if (path.empty() || path == ":memory:" || path.starts_with("file:"))
            throw std::invalid_argument(
                "Program provider journal requires a durable filesystem path");
        if (sqlite3_open_v2(path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            auto error = std::string("Program provider journal SQLite open failed: ") + sqlite3_errmsg(db);
            sqlite3_close_v2(db);
            throw std::runtime_error(error);
        }
        try {
            execute(db, "PRAGMA journal_mode=WAL");
            execute(db, "PRAGMA synchronous=FULL");
            execute(db, "PRAGMA busy_timeout=5000");
            execute(db, "CREATE TABLE IF NOT EXISTS ng_program_provider_calls("
                        "owner_scope TEXT NOT NULL, dispatch_id TEXT NOT NULL,"
                        "slot TEXT NOT NULL, request_digest TEXT NOT NULL,"
                        "provider_binding_identity TEXT NOT NULL, model TEXT NOT NULL,"
                        "original_attempt TEXT NOT NULL, state TEXT NOT NULL,"
                        "completion TEXT NOT NULL DEFAULT '', outcome_digest TEXT NOT NULL DEFAULT '',"
                        "evidence_identity TEXT NOT NULL DEFAULT '',"
                        "PRIMARY KEY(owner_scope,dispatch_id))");
        } catch (...) {
            sqlite3_close_v2(db);
            throw;
        }
    }
    ~Impl() { sqlite3_close_v2(db); }
    sqlite3* db = nullptr;
    mutable std::mutex mutex;
};

namespace {
struct Stored {
    std::string slot, request, provider, model, attempt, state, completion, outcome, evidence;
};
std::optional<Stored> load(sqlite3* db, std::string_view owner, std::string_view id) {
    Statement query(db, "SELECT slot,request_digest,provider_binding_identity,model,"
                        "original_attempt,state,completion,outcome_digest,evidence_identity "
                        "FROM ng_program_provider_calls WHERE owner_scope=?1 AND dispatch_id=?2");
    query.bind(1, owner);
    query.bind(2, id);
    if (!query.next()) return std::nullopt;
    Stored row{query.column(0), query.column(1), query.column(2), query.column(3),
               query.column(4), query.column(5), query.column(6), query.column(7), query.column(8)};
    if (query.next()) throw std::runtime_error("Duplicate Program provider journal slot");
    const auto slot_value = detail::parse_json_strict(row.slot);
    if (detail::canonical_json_bytes(slot_value) != row.slot ||
        hash("slot", slot_value) != id ||
        slot_value.at("owner_scope").get<std::string>() != owner ||
        !detail::is_sha256_identity(row.request) ||
        !detail::is_sha256_identity(row.provider) ||
        row.model.empty() || row.attempt.empty())
        throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    if (row.state != "dispatched" && row.state != "succeeded" &&
        row.state != "reconciliation_required")
        throw std::runtime_error("Corrupt Program provider journal state");
    if (row.state == "succeeded") {
        if (row.completion.empty() ||
            row.outcome != detail::sha256_identity(domain, "outcome", row.completion))
            throw std::runtime_error("Corrupt Program provider journal outcome");
        (void)parse_completion(row.completion);
    } else if (!row.completion.empty() || !row.outcome.empty() || !row.evidence.empty()) {
        throw std::runtime_error("Corrupt pending Program provider journal outcome");
    }
    return row;
}
void save_outcome(sqlite3* db, std::string_view owner, std::string_view id,
                  std::string_view state, std::string_view completion,
                  std::string_view outcome, std::string_view evidence) {
    Statement update(db, "UPDATE ng_program_provider_calls SET state=?3,completion=?4,"
                         "outcome_digest=?5,evidence_identity=?6 "
                         "WHERE owner_scope=?1 AND dispatch_id=?2 AND state IN ('dispatched','reconciliation_required')");
    update.bind(1, owner); update.bind(2, id); update.bind(3, state);
    update.bind(4, completion); update.bind(5, outcome); update.bind(6, evidence);
    update.done();
    if (sqlite3_changes(db) != 1) throw std::runtime_error("Program provider journal settlement conflict");
}
class BoundBroker final : public graph::ProviderCallBroker {
public:
    BoundBroker(std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl,
                ProgramCoreProviderCallContext context, std::string provider_identity)
        : impl_(std::move(impl)), owner_(context.owner_scope), version_(context.program_version_id),
          run_(context.run_id), operation_(context.operation_id), attempt_(context.attempt),
          provider_identity_(std::move(provider_identity)) {}

    asio::awaitable<ChatCompletion> invoke(graph::ProviderCallIdentity identity,
                                            std::shared_ptr<Provider> provider,
                                            CompletionParams params,
                                            StreamCallback on_chunk) override {
        if (!provider || identity.owner_scope != owner_ || identity.run_id != run_ ||
            identity.thread_id.empty() || identity.task_id.empty() || identity.node_name.empty() ||
            identity.call_ordinal != 0 || params.model.empty())
            throw std::invalid_argument("Program provider journal invocation scope is invalid");
        const ProgramCoreProviderCallContext context{owner_, version_, run_, operation_, attempt_};
        const auto slot = detail::canonical_json_bytes(json{
            {"owner_scope", owner_}, {"program_version_id", version_}, {"run_id", run_},
            {"operation_id", operation_}, {"thread_id", identity.thread_id},
            {"task_id", identity.task_id}, {"node_name", identity.node_name},
            {"call_ordinal", identity.call_ordinal}});
        const auto id = dispatch_id(context, identity);
        const auto digest = request_digest(params, static_cast<bool>(on_chunk));
        {
            std::lock_guard lock(impl_->mutex);
            Transaction tx(impl_->db);
            const auto existing = load(impl_->db, owner_, id);
            if (existing) {
                if (existing->slot != slot || existing->request != digest ||
                    existing->provider != provider_identity_ || existing->model != params.model)
                    throw std::runtime_error("Program provider journal logical call binding conflict");
                if (existing->state == "succeeded") {
                    tx.commit();
                    // A completed replay does not re-emit stream chunks.
                    co_return parse_completion(existing->completion);
                }
                throw std::runtime_error("Program provider journal dispatch outcome uncertain; reconciliation required");
            }
            Statement insert(impl_->db, "INSERT INTO ng_program_provider_calls "
                            "(owner_scope,dispatch_id,slot,request_digest,provider_binding_identity,"
                            "model,original_attempt,state) VALUES(?1,?2,?3,?4,?5,?6,?7,'dispatched')");
            insert.bind(1, owner_); insert.bind(2, id); insert.bind(3, slot);
            insert.bind(4, digest); insert.bind(5, provider_identity_);
            insert.bind(6, params.model); insert.bind(7, std::to_string(attempt_));
            insert.done();
            tx.commit();  // write-ahead barrier: no transport before durable commit
        }
        ChatCompletion result;
        try {
            result = co_await provider->invoke(params, std::move(on_chunk));
        } catch (...) {
            // A thrown transport/cancellation/callback cannot prove non-delivery.
            // A failed settlement also leaves the already durable dispatched marker.
            try {
                std::lock_guard lock(impl_->mutex);
                Transaction tx(impl_->db);
                save_outcome(impl_->db, owner_, id, "reconciliation_required", "", "", "");
                tx.commit();
            } catch (...) {}
            throw;
        }
        const auto canonical = detail::canonical_json_bytes(completion_json(result));
        const auto outcome = detail::sha256_identity(domain, "outcome", canonical);
        {
            std::lock_guard lock(impl_->mutex);
            Transaction tx(impl_->db);
            save_outcome(impl_->db, owner_, id, "succeeded", canonical, outcome, "");
            tx.commit();
        }
        co_return result;
    }
private:
    std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl_;
    std::string owner_, version_, run_, operation_;
    std::uint64_t attempt_;
    std::string provider_identity_;
};
}  // namespace

SQLiteProgramProviderCallJournal::SQLiteProgramProviderCallJournal(std::string path)
    : impl_(std::make_shared<Impl>(path)) {}
SQLiteProgramProviderCallJournal::~SQLiteProgramProviderCallJournal() = default;

std::string SQLiteProgramProviderCallJournal::logical_call_id(
    const ProgramCoreProviderCallContext& context,
    const graph::ProviderCallIdentity& call) {
    validate_context(context);
    if (call.owner_scope != context.owner_scope || call.run_id != context.run_id ||
        call.thread_id.empty() || call.task_id.empty() || call.node_name.empty() ||
        call.call_ordinal != 0)
        throw std::invalid_argument("Program provider journal logical call scope is invalid");
    return dispatch_id(context, call);
}

ProgramCoreProviderCallBinding SQLiteProgramProviderCallJournal::bind(
    const ProgramCoreProviderCallContext& context, std::string provider_identity) {
    validate_context(context);
    if (!detail::is_sha256_identity(provider_identity))
        throw std::invalid_argument("Program provider binding identity must be a deployment sha256 digest");
    return {std::string(context.owner_scope), std::string(context.program_version_id),
            std::string(context.run_id), std::string(context.operation_id), context.attempt,
            std::make_shared<BoundBroker>(impl_, context, std::move(provider_identity))};
}

std::optional<SQLiteProgramProviderCallJournal::Record>
SQLiteProgramProviderCallJournal::inspect(std::string_view owner, std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto stored = load(impl_->db, owner, id);
    if (!stored) return std::nullopt;
    return Record{stored->state == "succeeded" ? State::Succeeded
                  : stored->state == "dispatched" ? State::Dispatched
                  : State::ReconciliationRequired,
                  stored->request, stored->provider, stored->attempt, stored->outcome};
}

void SQLiteProgramProviderCallJournal::reconcile_success(
    std::string_view owner, std::string_view id, std::string evidence,
    ChatCompletion completion) {
    if (owner.empty() || id.empty() || !detail::is_sha256_identity(evidence))
        throw std::invalid_argument("Program provider reconciliation needs scoped external evidence identity");
    const auto canonical = detail::canonical_json_bytes(completion_json(completion));
    const auto outcome = detail::sha256_identity(domain, "outcome", canonical);
    std::lock_guard lock(impl_->mutex);
    Transaction tx(impl_->db);
    const auto existing = load(impl_->db, owner, id);
    if (!existing) throw std::out_of_range("Program provider reconciliation has no dispatched marker");
    if (existing->state == "succeeded") {
        if (existing->completion != canonical || existing->evidence != evidence)
            throw std::runtime_error("Program provider reconciliation evidence conflict");
    } else {
        save_outcome(impl_->db, owner, id, "succeeded", canonical, outcome, evidence);
    }
    tx.commit();
}
}  // namespace neograph::program
