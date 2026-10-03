#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/provider_outcome_codec.h>
#include <neograph/graph/cancel.h>

#include "../core/canonical_json.h"

#include <sqlite3.h>

#include <charconv>
#include <exception>
#include <map>
#include <set>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {
// Keep the logical slot identity stable so a historic uncertain dispatch cannot
// become a fresh call merely because its outcome format changed.
constexpr std::string_view slot_domain = "program-core-provider-call/v1";
constexpr std::string_view outcome_domain = "program-core-provider-outcome/v2";

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
    return detail::sha256_identity(slot_domain, purpose, detail::canonical_json_bytes(value));
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
    explicit Impl(const std::string& path, std::shared_ptr<sp::NativeArchive> native_archive)
        : archive(std::move(native_archive)) {
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
            // Existing rows retain version 1. Never upgrade a lossy historic
            // completion or an uncertain marker into the new lossless meaning.
            const auto add_column = [&](std::string_view name, const char* definition) {
                bool present = false;
                {
                    Statement columns(db, "PRAGMA table_info(ng_program_provider_calls)");
                    while (columns.next()) if (columns.column(1) == name) present = true;
                }
                if (!present) execute(db, definition);
            };
            add_column("encoding_version", "ALTER TABLE ng_program_provider_calls ADD COLUMN encoding_version TEXT NOT NULL DEFAULT '1'");
            add_column("provider_name", "ALTER TABLE ng_program_provider_calls ADD COLUMN provider_name TEXT NOT NULL DEFAULT ''");
            add_column("family", "ALTER TABLE ng_program_provider_calls ADD COLUMN family TEXT NOT NULL DEFAULT ''");
            add_column("mode", "ALTER TABLE ng_program_provider_calls ADD COLUMN mode TEXT NOT NULL DEFAULT ''");
            add_column("original_deadline", "ALTER TABLE ng_program_provider_calls ADD COLUMN original_deadline TEXT NOT NULL DEFAULT ''");
            add_column("logical_scope", "ALTER TABLE ng_program_provider_calls ADD COLUMN logical_scope TEXT NOT NULL DEFAULT ''");
            add_column("claim_amount", "ALTER TABLE ng_program_provider_calls ADD COLUMN claim_amount TEXT NOT NULL DEFAULT ''");
            add_column("committed_amount", "ALTER TABLE ng_program_provider_calls ADD COLUMN committed_amount TEXT NOT NULL DEFAULT ''");
            execute(db, "CREATE INDEX IF NOT EXISTS ng_program_provider_scope ON ng_program_provider_calls(owner_scope,logical_scope)");
        } catch (...) {
            sqlite3_close_v2(db);
            throw;
        }
    }
    ~Impl() { sqlite3_close_v2(db); }
    sqlite3* db = nullptr;
    mutable std::mutex mutex;
    std::shared_ptr<sp::NativeArchive> archive;
    struct AccountedSink {
        std::set<std::string> hydrated_scopes;
    };
    std::map<std::weak_ptr<UsageAccumulator>, AccountedSink,
             std::owner_less<std::weak_ptr<UsageAccumulator>>> accounted;
};

namespace {
struct Stored {
    std::string slot, request, provider, model, attempt, state, completion, outcome, evidence;
    std::string encoding_version, provider_name, family, mode, deadline;
    std::string scope;
    std::uint64_t claim = 0, committed = 0;
};
std::uint64_t parse_amount(std::string_view value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || (value.size() > 1 && value.front() == '0') ||
        parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("Corrupt Program provider budget authority");
    return result;
}
std::string scope_digest(const json& slot) {
    return hash("scope", {{"owner_scope", slot.at("owner_scope")},
                          {"program_version_id", slot.at("program_version_id")},
                          {"run_id", slot.at("run_id")}, {"operation_id", slot.at("operation_id")}});
}
std::string accounting_key(std::string_view owner, std::string_view id, const Stored& row) {
    return hash("accounting", {{"owner_scope", owner}, {"dispatch_id", id},
                               {"request_digest", row.request}});
}
std::uint64_t committed_amount(const sp::Outcome& outcome, std::uint64_t claim) {
    if (const auto* completion = std::get_if<sp::Completion>(&outcome))
        return completion->attempt.prior_usage_unknown || completion->attempt.transport_internal_resends != 0 ? claim
            : UsageAccumulator::conservative_final_charge(completion->usage).value_or(claim);
    const auto& failure = std::get<sp::Failure>(outcome);
    return provider_failure_proves_not_sent(failure) ? 0 : claim;
}
std::string archive_binding(std::string_view owner, std::string_view id, const Stored& row) {
    return detail::canonical_json_bytes(json{
        {"owner_scope", std::string(owner)}, {"dispatch_id", std::string(id)},
        {"request_digest", row.request}, {"provider_binding_identity", row.provider},
        {"provider_name", row.provider_name}, {"model", row.model}, {"family", row.family},
        {"mode", row.mode}, {"original_attempt", row.attempt}, {"original_deadline", row.deadline},
        {"logical_scope", row.scope}, {"claim_amount", row.claim}, {"committed_amount", row.committed}});
}
sp::runtime::Result restore_outcome(const Stored& row, std::string_view owner,
                                    std::string_view id, const std::shared_ptr<sp::NativeArchive>& archive) {
    const auto value = detail::parse_json_strict(row.completion);
    if (detail::canonical_json_bytes(value) != row.completion)
        throw std::runtime_error("Noncanonical Program provider outcome");
    return provider_codec::decode_outcome(value, archive, archive_binding(owner, id, row));
}
std::optional<Stored> load(sqlite3* db, std::string_view owner, std::string_view id,
                          const std::shared_ptr<sp::NativeArchive>& archive) {
    Statement query(db, "SELECT slot,request_digest,provider_binding_identity,model,"
                        "original_attempt,state,completion,outcome_digest,evidence_identity,"
                        "encoding_version,provider_name,family,mode,original_deadline,"
                        "logical_scope,claim_amount,committed_amount "
                        "FROM ng_program_provider_calls WHERE owner_scope=?1 AND dispatch_id=?2");
    query.bind(1, owner);
    query.bind(2, id);
    if (!query.next()) return std::nullopt;
    if (query.column(9) != "2")
        throw std::runtime_error("Incompatible historic Program provider journal encoding; external reconciliation required");
    Stored row{query.column(0), query.column(1), query.column(2), query.column(3),
               query.column(4), query.column(5), query.column(6), query.column(7), query.column(8),
               query.column(9), query.column(10), query.column(11), query.column(12), query.column(13),
               query.column(14), parse_amount(query.column(15)), parse_amount(query.column(16))};
    if (query.next()) throw std::runtime_error("Duplicate Program provider journal slot");
    const auto slot_value = detail::parse_json_strict(row.slot);
    if (detail::canonical_json_bytes(slot_value) != row.slot ||
        hash("slot", slot_value) != id ||
        slot_value.at("owner_scope").get<std::string>() != owner ||
        !detail::is_sha256_identity(row.request) ||
        !detail::is_sha256_identity(row.provider) ||
        row.model.empty() || row.attempt.empty() || row.provider_name.empty() ||
        row.family.empty() || (row.mode != "collect" && row.mode != "stream") || row.deadline.empty() ||
        (!row.evidence.empty() && !detail::is_sha256_identity(row.evidence)) ||
        row.scope != scope_digest(slot_value))
        throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    if (archive && archive->owner_scope() != owner)
        throw std::runtime_error("Program provider archive owner scope mismatch");
    if (row.state != "dispatched" && row.state != "succeeded" && row.state != "failed" &&
        row.state != "reconciliation_required")
        throw std::runtime_error("Corrupt Program provider journal state");
    if (!row.completion.empty()) {
        if (row.state == "dispatched" ||
            row.outcome != detail::sha256_identity(outcome_domain, "outcome", row.completion))
            throw std::runtime_error("Corrupt Program provider journal outcome");
        const auto encoded = detail::parse_json_strict(row.completion);
        if (detail::canonical_json_bytes(encoded) != row.completion ||
            encoded.at("schema") != provider_codec::outcome_schema ||
            ((row.state == "succeeded") != (encoded.at("kind") == "completion")))
            throw std::runtime_error("Program provider journal terminal outcome mismatch");
    } else if (row.state == "succeeded" || row.state == "failed" ||
               !row.outcome.empty() || !row.evidence.empty() || row.committed != row.claim) {
        throw std::runtime_error("Corrupt pending Program provider journal outcome");
    }
    return row;
}
SQLiteProgramProviderCallJournal::Impl::AccountedSink& accounted_sink(
    SQLiteProgramProviderCallJournal::Impl& impl, const std::shared_ptr<UsageAccumulator>& usage) {
    for (auto entry = impl.accounted.begin(); entry != impl.accounted.end();) {
        if (entry->first.expired()) entry = impl.accounted.erase(entry);
        else ++entry;
    }
    return impl.accounted[std::weak_ptr<UsageAccumulator>(usage)];
}
void hydrate_scope(SQLiteProgramProviderCallJournal::Impl& impl,
                   std::string_view owner, std::string_view scope,
                   const graph::ProviderCallIdentity& identity,
                   std::string_view current_id, const Stored* current,
                   sp::runtime::Result& current_result) {
    if (!identity.usage) {
        if (identity.model_token_budget != 0)
            throw std::invalid_argument("Program provider budget requires an accounting authority");
        return;
    }
    auto& sink = accounted_sink(impl, identity.usage);
    if (sink.hydrated_scopes.contains(std::string(scope))) return;
    Statement rows(impl.db, "SELECT dispatch_id,slot FROM ng_program_provider_calls "
                           "WHERE owner_scope=?1 AND (logical_scope=?2 OR logical_scope='')");
    rows.bind(1, owner);
    rows.bind(2, scope);
    while (rows.next()) {
        const auto id = rows.column(0);
        const auto slot = detail::parse_json_strict(rows.column(1));
        if (scope_digest(slot) != scope) continue;
        std::optional<Stored> loaded;
        const Stored* stored = current && id == current_id ? current : nullptr;
        if (!stored) {
            loaded = load(impl.db, owner, id, impl.archive);
            stored = loaded ? &*loaded : nullptr;
        }
        if (!stored) throw std::runtime_error("Program provider claim disappeared during recovery");
        const auto key = accounting_key(owner, id, *stored);
        sp::runtime::Result restored;
        if (!stored->completion.empty()) {
            restored = restore_outcome(*stored, owner, id, impl.archive);
            if (committed_amount(*restored, stored->claim) != stored->committed)
                throw std::runtime_error("Program provider persisted authority differs from the full outcome");
        }
        if (id == current_id) current_result = restored;
        if (identity.model_token_budget != 0 && stored->claim == 0) {
            bool recoverable = stored->state == "failed";
            if (restored) {
                if (const auto* completion = std::get_if<sp::Completion>(restored.get()))
                    recoverable = !completion->attempt.prior_usage_unknown &&
                        completion->attempt.transport_internal_resends == 0 &&
                        UsageAccumulator::conservative_final_charge(completion->usage).has_value();
            }
            if (!recoverable)
                throw std::runtime_error("Historic unbounded Program provider call lacks recoverable bounded authority");
        }
        // The durable journal, not editable report JSON or the attempt number,
        // is authority. Restore each owner/effect/prepared digest once per sink.
        if (!identity.usage->remember_provider_effect(key)) continue;
        const auto* completion = restored ? std::get_if<sp::Completion>(restored.get()) : nullptr;
        const bool charged = completion && !completion->attempt.prior_usage_unknown &&
            completion->attempt.transport_internal_resends == 0 &&
            UsageAccumulator::conservative_final_charge(completion->usage).has_value();
        if (charged)
            identity.usage->restore_charge(stored->committed);
        else
            identity.usage->restore_reservation(stored->committed);
        // A retained dispatch without an outcome has no captured report.
        if (restored) identity.usage->observe(outcome_usage(*restored));
    }
    sink.hydrated_scopes.insert(std::string(scope));
    if (identity.model_token_budget != 0 &&
        identity.usage->total_tokens_wide() > identity.model_token_budget) {
        if (identity.budget_exhausted) identity.budget_exhausted->store(true, std::memory_order_release);
        if (identity.budget_cancel_token) identity.budget_cancel_token->cancel();
    }
}
void save_outcome(sqlite3* db, std::string_view owner, std::string_view id,
                  std::string_view state, std::string_view completion,
                  std::string_view outcome, std::string_view evidence, std::uint64_t committed) {
    Statement update(db, "UPDATE ng_program_provider_calls SET state=?3,completion=?4,"
                         "outcome_digest=?5,evidence_identity=?6,committed_amount=?7 "
                         "WHERE owner_scope=?1 AND dispatch_id=?2 AND state IN ('dispatched','reconciliation_required')");
    update.bind(1, owner); update.bind(2, id); update.bind(3, state);
    update.bind(4, completion); update.bind(5, outcome); update.bind(6, evidence);
    update.bind(7, std::to_string(committed));
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

    asio::awaitable<sp::runtime::Result> invoke(graph::ProviderCallIdentity identity,
                                               std::shared_ptr<Provider> provider,
                                               ProviderRequest request) override {
        return invoke_owned(impl_, owner_, version_, run_, operation_, attempt_,
                            provider_identity_, std::move(identity), std::move(provider),
                            std::move(request));
    }
private:
    static asio::awaitable<sp::runtime::Result> invoke_owned(
        std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl,
        std::string owner, std::string version, std::string run, std::string operation,
        std::uint64_t attempt, std::string provider_identity,
        graph::ProviderCallIdentity identity, std::shared_ptr<Provider> provider,
        ProviderRequest request) {
        if (!provider || identity.owner_scope != owner || identity.run_id != run ||
            identity.thread_id.empty() || identity.task_id.empty() || identity.node_name.empty())
            throw std::invalid_argument("Program provider journal invocation scope is invalid");
        if (impl->archive && impl->archive->owner_scope() != owner)
            throw std::invalid_argument("Program provider archive owner scope mismatch");
        const auto provider_name = provider->get_name();
        if (provider_name.empty())
            throw std::invalid_argument("Program provider capability identity is empty");
        // Admission and encoding happen exactly once, before any durable
        // dispatched marker. Dropping this handle cannot dispatch transport.
        auto prepared = provider->prepare(std::move(request));
        if (!prepared.valid()) {
            sp::Failure failure;
            if (prepared.error()) failure.error = *prepared.error();
            else {
                failure.error.kind = sp::ErrorKind::InvalidRequest;
                failure.error.safe_message = "Program provider preparation is invalid";
                failure.error.retry_safety = sp::RetrySafety::NotSent;
            }
            co_return std::make_shared<const sp::Outcome>(std::move(failure));
        }
        if (prepared.requires_native_custody() && !impl->archive)
            throw std::invalid_argument("Program provider requires configured trusted native custody");
        if (impl->archive) {
            const auto* descriptor = prepared.admitted_descriptor();
            if (!descriptor || !impl->archive->matches_descriptor(*descriptor))
                throw std::invalid_argument("Program provider native custody admission is incompatible");
        }
        const ProgramCoreProviderCallContext context{owner, version, run, operation, attempt};
        const auto id = dispatch_id(context, identity);
        Stored admitted;
        admitted.slot = detail::canonical_json_bytes(json{
            {"owner_scope", owner}, {"program_version_id", version}, {"run_id", run},
            {"operation_id", operation}, {"thread_id", identity.thread_id},
            {"task_id", identity.task_id}, {"node_name", identity.node_name},
            {"call_ordinal", identity.call_ordinal}});
        admitted.request = Provider::request_digest(prepared);
        admitted.provider = provider_identity;
        admitted.provider_name = provider_name;
        admitted.model = std::string(prepared.model());
        admitted.family = std::string(prepared.family());
        admitted.mode = prepared.mode() == ProviderMode::Stream ? "stream" : "collect";
        admitted.deadline = std::to_string(prepared.deadline().time_since_epoch().count());
        admitted.attempt = std::to_string(attempt);
        admitted.encoding_version = "2";
        admitted.scope = hash("scope", {{"owner_scope", owner}, {"program_version_id", version},
                                        {"run_id", run}, {"operation_id", operation}});
        ProviderBudgetClaim claim;
        {
            std::lock_guard lock(impl->mutex);
            Transaction tx(impl->db);
            const auto existing = load(impl->db, owner, id, impl->archive);
            if (existing && (existing->slot != admitted.slot || existing->request != admitted.request ||
                    existing->provider != admitted.provider || existing->model != admitted.model ||
                    existing->provider_name != admitted.provider_name ||
                    existing->family != admitted.family || existing->mode != admitted.mode))
                throw std::runtime_error("Program provider journal logical call binding conflict");
            sp::runtime::Result restored;
            hydrate_scope(*impl, owner, admitted.scope, identity, id,
                          existing ? &*existing : nullptr, restored);
            if (existing) {
                if (!existing->completion.empty()) {
                    if (!restored) restored = restore_outcome(*existing, owner, id, impl->archive);
                    if (committed_amount(*restored, existing->claim) != existing->committed)
                        throw std::runtime_error("Program provider persisted authority differs from the full outcome");
                    tx.commit();
                    // Replay preserves the entire outcome and emits no events.
                    co_return restored;
                }
                throw std::runtime_error("Program provider journal dispatch outcome uncertain; reconciliation required");
            }
            try {
                claim = reserve_provider_dispatch(prepared, ProviderDispatchBudget{
                    identity.usage, identity.model_token_budget,
                    identity.budget_exhausted, identity.budget_cancel_token});
            } catch (const ProviderFailure& error) {
                co_return error.outcome();
            }
            admitted.claim = claim.amount();
            admitted.committed = admitted.claim;
            Statement insert(impl->db, "INSERT INTO ng_program_provider_calls "
                             "(owner_scope,dispatch_id,slot,request_digest,provider_binding_identity,"
                             "model,original_attempt,state,encoding_version,provider_name,family,mode,original_deadline,"
                             "logical_scope,claim_amount,committed_amount)"
                             " VALUES(?1,?2,?3,?4,?5,?6,?7,'dispatched','2',?8,?9,?10,?11,?12,?13,?14)");
            insert.bind(1, owner); insert.bind(2, id); insert.bind(3, admitted.slot);
            insert.bind(4, admitted.request); insert.bind(5, admitted.provider);
            insert.bind(6, admitted.model); insert.bind(7, admitted.attempt);
            insert.bind(8, admitted.provider_name); insert.bind(9, admitted.family);
            insert.bind(10, admitted.mode); insert.bind(11, admitted.deadline);
            insert.bind(12, admitted.scope); insert.bind(13, std::to_string(admitted.claim));
            insert.bind(14, std::to_string(admitted.committed));
            insert.done();
            tx.commit();  // write-ahead barrier: no transport before durable commit
            claim.mark_dispatched();
            if (identity.usage && !identity.usage->remember_provider_effect(accounting_key(owner, id, admitted)))
                throw std::runtime_error("Program provider effect was accounted without its expected durable receipt");
        }
        sp::runtime::Result result;
        std::exception_ptr observer_error;
        try {
            result = co_await provider->dispatch_async(std::move(prepared));
            if (!result) throw std::runtime_error("Program provider returned no owned outcome");
        } catch (const ProviderOutcomeError& error) {
            result = error.outcome();
            observer_error = std::current_exception();
            if (!result) throw;
        } catch (...) {
            // No exception proves non-delivery. The immutable marker remains
            // authoritative even when this best-effort annotation fails.
            try {
                std::lock_guard lock(impl->mutex);
                Transaction tx(impl->db);
                save_outcome(impl->db, owner, id, "reconciliation_required", "", "", "", admitted.claim);
                tx.commit();
            } catch (...) {}
            throw;
        }
        if (claim.active()) admitted.committed = claim.settle(result);
        else {
            admitted.committed = committed_amount(*result, 0);
            if (identity.usage) {
                if (const auto* completion = std::get_if<sp::Completion>(result.get()))
                    identity.usage->add(completion->usage);
                else identity.usage->observe(std::get<sp::Failure>(*result).partial.usage);
            }
        }
        try {
            const auto canonical = detail::canonical_json_bytes(provider_codec::encode_outcome(
                *result, impl->archive, archive_binding(owner, id, admitted)));
            const auto outcome = detail::sha256_identity(outcome_domain, "outcome", canonical);
            const auto* failure = std::get_if<sp::Failure>(result.get());
            const auto state = !failure ? "succeeded"
                : provider_failure_proves_not_sent(*failure) ? "failed" : "reconciliation_required";
            std::lock_guard lock(impl->mutex);
            Transaction tx(impl->db);
            save_outcome(impl->db, owner, id, state, canonical, outcome, "", admitted.committed);
            tx.commit();
        } catch (...) {
            // Preserve the actual model result and original storage cause;
            // the already-durable marker remains nonredispatchable.
            throw ProgramProviderOutcomePersistenceError(
                result, std::current_exception(), observer_error);
        }
        if (observer_error) std::rethrow_exception(observer_error);
        co_return result;
    }
    std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl_;
    std::string owner_, version_, run_, operation_;
    std::uint64_t attempt_;
    std::string provider_identity_;
};
}  // namespace

SQLiteProgramProviderCallJournal::SQLiteProgramProviderCallJournal(
    std::string path, std::shared_ptr<sp::NativeArchive> archive)
    : impl_(std::make_shared<Impl>(path, std::move(archive))) {}
SQLiteProgramProviderCallJournal::~SQLiteProgramProviderCallJournal() = default;

std::string SQLiteProgramProviderCallJournal::logical_call_id(
    const ProgramCoreProviderCallContext& context,
    const graph::ProviderCallIdentity& call) {
    validate_context(context);
    if (call.owner_scope != context.owner_scope || call.run_id != context.run_id ||
        call.thread_id.empty() || call.task_id.empty() || call.node_name.empty())
        throw std::invalid_argument("Program provider journal logical call scope is invalid");
    return dispatch_id(context, call);
}

ProgramCoreProviderCallBinding SQLiteProgramProviderCallJournal::bind(
    const ProgramCoreProviderCallContext& context, std::string provider_identity) {
    validate_context(context);
    if (impl_->archive && impl_->archive->owner_scope() != context.owner_scope)
        throw std::invalid_argument("Program provider archive owner scope mismatch");
    if (!detail::is_sha256_identity(provider_identity))
        throw std::invalid_argument("Program provider binding identity must be a deployment sha256 digest");
    return {std::string(context.owner_scope), std::string(context.program_version_id),
            std::string(context.run_id), std::string(context.operation_id), context.attempt,
            std::make_shared<BoundBroker>(impl_, context, std::move(provider_identity))};
}

std::optional<SQLiteProgramProviderCallJournal::Record>
SQLiteProgramProviderCallJournal::inspect(std::string_view owner, std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto stored = load(impl_->db, owner, id, impl_->archive);
    if (!stored) return std::nullopt;
    if (!stored->completion.empty()) {
        const auto restored = restore_outcome(*stored, owner, id, impl_->archive);
        if (committed_amount(*restored, stored->claim) != stored->committed)
            throw std::runtime_error("Program provider persisted authority differs from the full outcome");
    }
    return Record{stored->state == "succeeded" ? State::Succeeded
                  : stored->state == "dispatched" ? State::Dispatched
                  : stored->state == "failed" ? State::Failed
                  : State::ReconciliationRequired,
                  stored->request, stored->provider, stored->attempt, stored->outcome,
                  stored->provider_name, stored->model, stored->family, stored->mode,
                  stored->deadline, 2, stored->claim, stored->committed};
}

void SQLiteProgramProviderCallJournal::reconcile_success(
    std::string_view owner, std::string_view id, std::string evidence,
    sp::runtime::Result completion) {
    if (owner.empty() || id.empty() || !detail::is_sha256_identity(evidence) ||
        !completion || !std::holds_alternative<sp::Completion>(*completion))
        throw std::invalid_argument("Program provider reconciliation needs a full completion and scoped external evidence identity");
    std::lock_guard lock(impl_->mutex);
    Transaction tx(impl_->db);
    const auto existing = load(impl_->db, owner, id, impl_->archive);
    if (!existing) throw std::out_of_range("Program provider reconciliation has no dispatched marker");
    if (existing->state == "failed")
        throw std::runtime_error("Program provider reconciliation conflicts with proven non-delivery");
    if (existing->state == "succeeded") {
        const auto restored = restore_outcome(*existing, owner, id, impl_->archive);
        if (provider_codec::observe_outcome(*restored) != provider_codec::observe_outcome(*completion) ||
            existing->evidence != evidence)
            throw std::runtime_error("Program provider reconciliation evidence conflict");
    } else {
        auto reconciled = *existing;
        reconciled.committed = committed_amount(*completion, reconciled.claim);
        const auto canonical = detail::canonical_json_bytes(provider_codec::encode_outcome(
            *completion, impl_->archive, archive_binding(owner, id, reconciled)));
        const auto outcome = detail::sha256_identity(outcome_domain, "outcome", canonical);
        save_outcome(impl_->db, owner, id, "succeeded", canonical, outcome, evidence, reconciled.committed);
    }
    tx.commit();
}
}  // namespace neograph::program
