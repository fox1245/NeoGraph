#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/provider_outcome_codec.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>

#include "../core/canonical_json.h"
#include "../core/sqlite_wal.h"
#include "monetary.h"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <functional>
#include <exception>
#include <map>
#include <set>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifndef NEOGRAPH_VERSION_STR
#define NEOGRAPH_VERSION_STR "unknown"
#endif

namespace neograph::program {
namespace {
// Keep the logical slot identity stable so a historic uncertain dispatch cannot
// become a fresh call merely because its outcome format changed.
constexpr std::string_view slot_domain = "program-core-provider-call/v1";
constexpr std::string_view outcome_domain = "program-core-provider-outcome/v3";
// Rows written before interface 7 hold v2 outcomes under the v2 domain; they are read, not rewritten.
constexpr std::string_view legacy_outcome_domain = "program-core-provider-outcome/v2";

// #543: the build that encodes a request. A fork finds that a call differs from its source's only when the
// same encoder wrote both digests, since an SDK or a built-in prompt that changes can change the bytes of a
// request that did not.
const std::string& producer_identity() {
    static const std::string producer = std::string("neograph/") + NEOGRAPH_VERSION_STR;
    return producer;
}

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
    return neograph::detail::sha256_identity(slot_domain, purpose, neograph::detail::canonical_json_bytes(value));
}
std::string dispatch_id(const ProgramCoreProviderCallContext& context,
                        const graph::ProviderCallIdentity& call) {
    // Shared with the in-process pricing broker; the same bytes as hash("slot", ...).
    return program::detail::provider_call_slot_id(context, call);
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
            // The busy timeout comes first, and the WAL switch retries a
            // concurrent first open of the same file (see sqlite_wal.h).
            execute(db, "PRAGMA busy_timeout=5000");
            if (neograph::detail::sqlite_enable_wal(db, std::chrono::milliseconds(5000)) != SQLITE_OK)
                sql_error(db);
            execute(db, "PRAGMA synchronous=FULL");
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
            // The priced call's ProgramMonetaryRecord (#372): written as the held
            // provisional record with the dispatched marker, replaced by the
            // settled record with the outcome, emptied when nothing was sent.
            add_column("monetary_record", "ALTER TABLE ng_program_provider_calls ADD COLUMN monetary_record TEXT NOT NULL DEFAULT ''");
            // The call's identity (#543). A row written before it keeps identity version 1: its request_digest is
            // the dispatch digest, which includes the descriptor policy, and it has no content, policy or
            // producer. A row written now is version 2.
            add_column("identity_version", "ALTER TABLE ng_program_provider_calls ADD COLUMN identity_version TEXT NOT NULL DEFAULT '1'");
            add_column("content_digest", "ALTER TABLE ng_program_provider_calls ADD COLUMN content_digest TEXT NOT NULL DEFAULT ''");
            add_column("policy_identity", "ALTER TABLE ng_program_provider_calls ADD COLUMN policy_identity TEXT NOT NULL DEFAULT ''");
            add_column("producer", "ALTER TABLE ng_program_provider_calls ADD COLUMN producer TEXT NOT NULL DEFAULT ''");
            // #541: the identity of a secret the outcome's typed result needs and the journal never stores (a
            // resumable upload's session URL). Empty when the outcome has none.
            add_column("withheld_secret", "ALTER TABLE ng_program_provider_calls ADD COLUMN withheld_secret TEXT NOT NULL DEFAULT ''");
            // #541: '1' for a call that opens a chunked upload's session, written with the dispatched marker.
            add_column("opens_upload", "ALTER TABLE ng_program_provider_calls ADD COLUMN opens_upload TEXT NOT NULL DEFAULT ''");
            // The times a call was matched to its record under another policy identity. A row is never rewritten.
            execute(db, "CREATE TABLE IF NOT EXISTS ng_program_provider_call_policy_changes("
                        "owner_scope TEXT NOT NULL, dispatch_id TEXT NOT NULL,"
                        "journaled_policy TEXT NOT NULL, observed_policy TEXT NOT NULL,"
                        "attempt TEXT NOT NULL, via TEXT NOT NULL,"
                        "PRIMARY KEY(owner_scope,dispatch_id,journaled_policy,observed_policy))");
            // #541: the times a resume sent a call that opens an upload's session again, in place of its record. A
            // row is never rewritten: this table says what became of it. `restart` counts a call's restarts from
            // 1; `replaced_state` and `replaced_outcome` are the record's, as the restart found it.
            execute(db, "CREATE TABLE IF NOT EXISTS ng_program_provider_call_restarts("
                        "owner_scope TEXT NOT NULL, dispatch_id TEXT NOT NULL, restart INTEGER NOT NULL,"
                        "attempt TEXT NOT NULL, reason TEXT NOT NULL,"
                        "replaced_state TEXT NOT NULL, replaced_outcome TEXT NOT NULL,"
                        "PRIMARY KEY(owner_scope,dispatch_id,restart))");
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
    std::string monetary;
    // #543. Version 2: `request` names `content` and `policy` together. Version 1: `request` is the dispatch
    // digest of the writer, and the three are empty. `legacy` is the dispatch digest of the call being made.
    std::string identity_version = "2", content, policy, producer;
    // #541: secret_identity() of the secret the outcome carried and the journal did not keep. Empty for none.
    std::string withheld;
    std::string legacy;
    // #541: the call opens a chunked upload's session. For the call being made it is the request's; for a loaded
    // row it is the marker written with the dispatched marker, empty on a row written before it.
    bool opens_upload = false;
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
// The version 2 request_digest: the content and the policy that admitted it, as one name.
std::string identity_of(std::string_view content, std::string_view policy) {
    return hash("identity", {{"content", std::string(content)}, {"policy", std::string(policy)}});
}
// A secret the live call holds, named without being kept (#541).
std::string secret_identity(std::string_view secret) {
    return neograph::detail::sha256_identity("program-core-provider-secret/v1", "secret", secret);
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
    return provider_failure_proves_not_accepted(failure) ? 0 : claim;
}
std::string archive_binding(std::string_view owner, std::string_view id, const Stored& row) {
    return neograph::detail::canonical_json_bytes(json{
        {"owner_scope", std::string(owner)}, {"dispatch_id", std::string(id)},
        {"request_digest", row.request}, {"provider_binding_identity", row.provider},
        {"provider_name", row.provider_name}, {"model", row.model}, {"family", row.family},
        {"mode", row.mode}, {"original_attempt", row.attempt}, {"original_deadline", row.deadline},
        {"logical_scope", row.scope}, {"claim_amount", row.claim}, {"committed_amount", row.committed}});
}
sp::runtime::Result restore_outcome(const Stored& row, std::string_view owner,
                                    std::string_view id, const std::shared_ptr<sp::NativeArchive>& archive) {
    const auto value = neograph::detail::parse_json_strict(row.completion);
    if (neograph::detail::canonical_json_bytes(value) != row.completion)
        throw std::runtime_error("Noncanonical Program provider outcome");
    return provider_codec::decode_outcome(value, archive, archive_binding(owner, id, row));
}
std::optional<Stored> load(sqlite3* db, std::string_view owner, std::string_view id,
                          const std::shared_ptr<sp::NativeArchive>& archive) {
    Statement query(db, "SELECT slot,request_digest,provider_binding_identity,model,"
                        "original_attempt,state,completion,outcome_digest,evidence_identity,"
                        "encoding_version,provider_name,family,mode,original_deadline,"
                        "logical_scope,claim_amount,committed_amount,monetary_record,"
                        "identity_version,content_digest,policy_identity,producer,withheld_secret,opens_upload "
                        "FROM ng_program_provider_calls WHERE owner_scope=?1 AND dispatch_id=?2");
    query.bind(1, owner);
    query.bind(2, id);
    if (!query.next()) return std::nullopt;
    if (query.column(9) != "2")
        throw std::runtime_error("Incompatible historic Program provider journal encoding; external reconciliation required");
    Stored row{query.column(0), query.column(1), query.column(2), query.column(3),
               query.column(4), query.column(5), query.column(6), query.column(7), query.column(8),
               query.column(9), query.column(10), query.column(11), query.column(12), query.column(13),
               query.column(14), parse_amount(query.column(15)), parse_amount(query.column(16)),
               query.column(17), query.column(18), query.column(19), query.column(20),
               query.column(21), query.column(22)};
    const auto marker = query.column(23);
    if (marker != "" && marker != "1") throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    row.opens_upload = marker == "1";
    if (query.next()) throw std::runtime_error("Duplicate Program provider journal slot");
    // A call that names no model, such as a file upload, stores an empty one; the model is compared with the
    // call's own whenever the row is used.
    const auto slot_value = neograph::detail::parse_json_strict(row.slot);
    if (neograph::detail::canonical_json_bytes(slot_value) != row.slot ||
        hash("slot", slot_value) != id ||
        slot_value.at("owner_scope").get<std::string>() != owner ||
        !neograph::detail::is_sha256_identity(row.request) ||
        !neograph::detail::is_sha256_identity(row.provider) ||
        row.attempt.empty() || row.provider_name.empty() ||
        row.family.empty() || (row.mode != "collect" && row.mode != "stream") || row.deadline.empty() ||
        (!row.evidence.empty() && !neograph::detail::is_sha256_identity(row.evidence)) ||
        row.scope != scope_digest(slot_value))
        throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    if (row.identity_version == "1") {
        if (!row.content.empty() || !row.policy.empty() || !row.producer.empty())
            throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    } else if (row.identity_version == "2") {
        if (!neograph::detail::is_sha256_identity(row.content) ||
            !neograph::detail::is_sha256_identity(row.policy) || row.producer.empty() ||
            row.request != identity_of(row.content, row.policy))
            throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    } else {
        throw std::runtime_error("Incompatible Program provider journal identity; external reconciliation required");
    }
    if (!row.withheld.empty() &&
        (!neograph::detail::is_sha256_identity(row.withheld) || row.state != "succeeded"))
        throw std::runtime_error("Corrupt Program provider journal dispatch binding");
    if (archive && archive->owner_scope() != owner)
        throw std::runtime_error("Program provider archive owner scope mismatch");
    if (row.state != "dispatched" && row.state != "succeeded" && row.state != "failed" &&
        row.state != "reconciliation_required")
        throw std::runtime_error("Corrupt Program provider journal state");
    if (!row.completion.empty()) {
        const bool current =
            row.outcome == neograph::detail::sha256_identity(outcome_domain, "outcome", row.completion);
        const bool legacy = !current &&
            row.outcome == neograph::detail::sha256_identity(legacy_outcome_domain, "outcome", row.completion);
        if (row.state == "dispatched" || (!current && !legacy))
            throw std::runtime_error("Corrupt Program provider journal outcome");
        const auto encoded = neograph::detail::parse_json_strict(row.completion);
        if (neograph::detail::canonical_json_bytes(encoded) != row.completion ||
            encoded.at("schema") != (legacy ? provider_codec::legacy_outcome_schema : provider_codec::outcome_schema) ||
            ((row.state == "succeeded") != (encoded.at("kind") == "completion")))
            throw std::runtime_error("Program provider journal terminal outcome mismatch");
    } else if (row.state == "succeeded" || row.state == "failed" ||
               !row.outcome.empty() || !row.evidence.empty() || row.committed != row.claim) {
        throw std::runtime_error("Corrupt pending Program provider journal outcome");
    }
    if (!row.monetary.empty()) {
        // A call proven not accepted (never sent, or declined by a completed
        // HTTP 4xx answer) charges nothing and keeps no record.
        if (row.state == "failed")
            throw std::runtime_error("Corrupt Program provider journal monetary record");
        const auto value = neograph::detail::parse_json_strict(row.monetary);
        if (neograph::detail::canonical_json_bytes(value) != row.monetary)
            throw std::runtime_error("Noncanonical Program provider journal monetary record");
        const auto record = program::detail::decode_monetary_record(value);
        if (record.call_id != id)
            throw std::runtime_error("Program provider journal monetary record names another call");
    }
    return row;
}

std::uint64_t row_attempt(const Stored& row) {
    return parse_amount(row.attempt);
}

std::string monetary_key(std::string_view owner, std::string_view id) {
    return hash("monetary", {{"owner_scope", owner}, {"dispatch_id", id}});
}

std::string encode_monetary(const std::optional<ProgramMonetaryRecord>& record) {
    return record ? neograph::detail::canonical_json_bytes(
                        program::detail::encode_monetary_record(*record))
                  : std::string();
}

// Charge the calls of one operation scope that this attempt journaled before
// a restart, once per account. A call journaled by an earlier attempt was
// settled into the run's remaining budget by that attempt's result, so it is
// only marked as accounted (#372).
void hydrate_monetary_scope(sqlite3* db, std::string_view owner, std::string_view scope,
                            const std::shared_ptr<sp::NativeArchive>& archive,
                            const std::shared_ptr<ProgramMonetaryAccount>& account) {
    if (!account) return;
    if (!account->remember(hash("monetary-scope", {{"owner_scope", owner}, {"scope", scope}})))
        return;
    std::vector<std::string> ids;
    {
        Statement rows(db, "SELECT dispatch_id FROM ng_program_provider_calls "
                           "WHERE owner_scope=?1 AND logical_scope=?2");
        rows.bind(1, owner);
        rows.bind(2, scope);
        while (rows.next()) ids.push_back(rows.column(0));
    }
    for (const auto& id : ids) {
        const auto stored = load(db, owner, id, archive);
        if (!stored) throw std::runtime_error("Program provider claim disappeared during recovery");
        const auto key = monetary_key(owner, id);
        if (stored->monetary.empty() || row_attempt(*stored) != account->attempt()) {
            (void)account->remember(key);
            continue;
        }
        auto record = program::detail::decode_monetary_record(
            neograph::detail::parse_json_strict(stored->monetary));
        record.replayed = true;
        (void)account->restore(key, std::move(record));
    }
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
                   const graph::ProviderCallIdentity& identity, std::uint64_t attempt,
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
        const auto slot = neograph::detail::parse_json_strict(rows.column(1));
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
        // A claim journaled by an earlier Program attempt was settled into the
        // run's remaining budget by that attempt's result, and this attempt's
        // grant already excludes it. Recognise it, never charge it again
        // (#378). Claims this attempt journaled before a restart are charged.
        if (row_attempt(*stored) != attempt) continue;
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
                  std::string_view outcome, std::string_view evidence, std::uint64_t committed,
                  std::optional<std::string_view> monetary = std::nullopt,
                  std::string_view withheld = {}) {
    Statement update(db, monetary
        ? "UPDATE ng_program_provider_calls SET state=?3,completion=?4,"
          "outcome_digest=?5,evidence_identity=?6,committed_amount=?7,withheld_secret=?8,monetary_record=?9 "
          "WHERE owner_scope=?1 AND dispatch_id=?2 AND state IN ('dispatched','reconciliation_required')"
        : "UPDATE ng_program_provider_calls SET state=?3,completion=?4,"
          "outcome_digest=?5,evidence_identity=?6,committed_amount=?7,withheld_secret=?8 "
          "WHERE owner_scope=?1 AND dispatch_id=?2 AND state IN ('dispatched','reconciliation_required')");
    update.bind(1, owner); update.bind(2, id); update.bind(3, state);
    update.bind(4, completion); update.bind(5, outcome); update.bind(6, evidence);
    update.bind(7, std::to_string(committed)); update.bind(8, withheld);
    if (monetary) update.bind(9, *monetary);
    update.done();
    if (sqlite3_changes(db) != 1) throw std::runtime_error("Program provider journal settlement conflict");
}
// #543: how a journaled row relates to the call being made at its position.
enum class Relation {
    Same,           // the same content, admitted by the same policy
    PolicyChanged,  // the same content, admitted by another descriptor policy
    Different,      // another request: its model, family, mode or content differs and the encoder is the same
    Unknown,        // cannot tell: the encoder may differ, or the row predates content identity
};
Relation relation_of(const Stored& row, const Stored& call) {
    // These are the call's own facts, not an encoder's, so a difference proves another request.
    if (row.model != call.model || row.family != call.family || row.mode != call.mode)
        return Relation::Different;
    if (row.identity_version == "2") {
        if (row.content == call.content)
            return row.policy == call.policy ? Relation::Same : Relation::PolicyChanged;
        return row.producer == call.producer ? Relation::Different : Relation::Unknown;
    }
    // A version 1 row names its call by the dispatch digest, which includes the policy: equal digests prove
    // the same call, and unequal ones do not tell a changed policy from a changed request.
    return row.request == call.legacy ? Relation::Same : Relation::Unknown;
}

std::string conflict_text(const Stored& row, const Stored& call, Relation relation) {
    std::string text = "Program provider journal logical call binding conflict";
    if (relation == Relation::Unknown && row.identity_version != "2")
        text += ": the journaled call predates content identity (written by 0.16.0 or earlier) and its dispatch "
                "digest differs, so the request or SchemaProvider's descriptor policy changed";
    else if (relation == Relation::Unknown)
        text += ": the request content differs, and the journaled call was encoded by " + row.producer +
                ", not by " + call.producer;
    else if (relation == Relation::Different)
        text += ": the request content differs";
    return text;
}

// What a call tells the host, kept until its journal write is committed.
using Diagnostics = std::vector<ProgramCoreProviderCallDiagnostic>;

void report(const std::function<void(ProgramCoreProviderCallDiagnostic)>& sink, Diagnostics& pending) noexcept {
    auto reported = std::move(pending);
    pending.clear();
    if (!sink) return;
    for (auto& value : reported) {
        try { sink(std::move(value)); } catch (...) {}
    }
}

// Notes that a call was matched to its record under another descriptor policy identity.
void note_policy_change(sqlite3* db, std::string_view owner, std::string_view id,
                        const Stored& row, const Stored& call, std::uint64_t attempt, bool adoption,
                        std::string_view node, std::string_view source_id, Diagnostics& pending) {
    Statement insert(db, "INSERT OR IGNORE INTO ng_program_provider_call_policy_changes"
                         "(owner_scope,dispatch_id,journaled_policy,observed_policy,attempt,via) "
                         "VALUES(?1,?2,?3,?4,?5,?6)");
    insert.bind(1, owner); insert.bind(2, id); insert.bind(3, row.policy); insert.bind(4, call.policy);
    insert.bind(5, std::to_string(attempt)); insert.bind(6, adoption ? "adoption" : "replay");
    insert.done();
    json witness{{"call_id", std::string(id)}, {"node", std::string(node)},
                 {"via", adoption ? "adoption" : "replay"},
                 {"journaled_policy_identity", row.policy}, {"observed_policy_identity", call.policy}};
    if (adoption) witness["source_call_id"] = std::string(source_id);
    pending.push_back(
        {"P_PROVIDER_CALL_POLICY_CHANGED",
         std::string("A provider call was found again under another SchemaProvider descriptor policy "
                     "identity. Its request content is the same, so the journaled outcome was ") +
             (adoption ? "adopted by the fork." : "replayed."),
         std::move(witness)});
}

// #541: records, before the call is sent, that a resume sends the call that opens an upload's session again in
// place of its record. The record is appended; the row it replaces is not touched.
void note_restart(sqlite3* db, std::string_view owner, std::string_view id, const Stored& row,
                  std::uint64_t attempt, std::string_view reason) {
    Statement insert(db, "INSERT INTO ng_program_provider_call_restarts"
                         "(owner_scope,dispatch_id,restart,attempt,reason,replaced_state,replaced_outcome) "
                         "SELECT ?1,?2,COALESCE(MAX(restart),0)+1,?3,?4,?5,?6 "
                         "FROM ng_program_provider_call_restarts WHERE owner_scope=?1 AND dispatch_id=?2");
    insert.bind(1, owner); insert.bind(2, id); insert.bind(3, std::to_string(attempt));
    insert.bind(4, reason); insert.bind(5, row.state); insert.bind(6, row.outcome);
    insert.done();
}
std::uint64_t restart_count(sqlite3* db, std::string_view owner, std::string_view id) {
    Statement count(db, "SELECT COUNT(*) FROM ng_program_provider_call_restarts "
                        "WHERE owner_scope=?1 AND dispatch_id=?2");
    count.bind(1, owner); count.bind(2, id);
    if (!count.next()) return 0;
    return parse_amount(count.column(0));
}

SQLiteProgramProviderCallJournal::State state_of(std::string_view state) {
    using State = SQLiteProgramProviderCallJournal::State;
    return state == "succeeded" ? State::Succeeded
         : state == "dispatched" ? State::Dispatched
         : state == "failed" ? State::Failed
         : State::ReconciliationRequired;
}

// #541: the first call after `identity` that the journal holds for the same node task, in any state. A node
// that opens a chunked upload's session makes the calls that send its parts after it, with the session it
// holds, so a call the journal holds after the opening may have written into that session. Empty when there
// is none.
std::optional<std::string> later_call(sqlite3* db, std::string_view owner, std::string_view scope,
                                      const graph::ProviderCallIdentity& identity) {
    Statement rows(db, "SELECT dispatch_id,slot FROM ng_program_provider_calls "
                       "WHERE owner_scope=?1 AND (logical_scope=?2 OR logical_scope='')");
    rows.bind(1, owner);
    rows.bind(2, scope);
    std::optional<std::string> first;
    std::uint64_t              first_ordinal = 0;
    while (rows.next()) {
        const auto id   = rows.column(0);
        const auto slot = neograph::detail::parse_json_strict(rows.column(1));
        if (scope_digest(slot) != scope || slot.at("thread_id") != identity.thread_id ||
            slot.at("task_id") != identity.task_id || slot.at("node_name") != identity.node_name)
            continue;
        const auto ordinal = slot.at("call_ordinal").get<std::uint64_t>();
        if (ordinal <= identity.call_ordinal || (first && ordinal >= first_ordinal)) continue;
        first         = id;
        first_ordinal = ordinal;
    }
    return first;
}

const char* to_string(ProgramProviderForkResendRefusedError::Reason reason) noexcept {
    switch (reason) {
        case ProgramProviderForkResendRefusedError::Reason::Unverifiable: return "unverifiable";
        case ProgramProviderForkResendRefusedError::Reason::EncoderDiffers: return "encoder_differs";
        case ProgramProviderForkResendRefusedError::Reason::DeploymentDiffers: return "deployment_differs";
    }
    return "unverifiable";
}

// #405: a fork repeats the provider calls its source's paused invocation made
// before the fork. When the source journaled the same request at the same
// slot (its own run and Core thread), the fork adopts that record: the
// outcome is re-encoded under the fork's dispatch id, the monetary record
// names the fork's call, and the row keeps the source's attempt, so neither
// this attempt nor a restart of it charges the call again. Returns the
// replayed outcome, or nothing when the call is a new one.
//
// #543: the same request is the same content, whatever descriptor policy
// admitted it. A source record that is there but is not shown to differ, and
// cannot be adopted, is a call the fork would send again and pay again: it is
// refused unless the host allows it.
std::optional<sp::runtime::Result> adopt_source_call(
    SQLiteProgramProviderCallJournal::Impl& impl, std::string_view owner, std::string_view id,
    const Stored& admitted, std::string_view operation, const graph::ProviderCallIdentity& identity,
    const ProgramCoreProviderCallReplaySource& source, std::uint64_t attempt, Diagnostics& pending) {
    // The source's thread of the same derivation as this call's.
    std::string           source_id;
    std::optional<Stored> row;
    for (const auto& [target_thread, source_thread] : source.threads) {
        if (target_thread != identity.thread_id) continue;
        auto source_call      = identity;
        source_call.thread_id = source_thread;
        const ProgramCoreProviderCallContext source_context{
            owner, source.program_version_id, source.run_id, operation, 1};
        source_id = dispatch_id(source_context, source_call);
        row       = load(impl.db, owner, source_id, impl.archive);
        if (row) break;
    }
    if (!row || row_attempt(*row) > source.max_attempt) return std::nullopt;
    // #541: a fork opens a session of its own. The source's was named by a URL that only its live call held, and
    // opening one charges nothing, so the fork sends the call whatever became of the source's.
    if (admitted.opens_upload && !(row->state == "succeeded" && row->withheld.empty())) return std::nullopt;
    const auto relation = relation_of(*row, admitted);
    if (relation == Relation::Different) return std::nullopt;
    // A call proven not accepted was not paid for: nothing is sent again.
    if (row->state == "failed") return std::nullopt;
    using Reason = ProgramProviderForkResendRefusedError::Reason;
    std::optional<Reason> refusal;
    if (relation == Relation::Unknown)
        refusal = row->identity_version == "2" ? Reason::EncoderDiffers : Reason::Unverifiable;
    else if (row->provider != admitted.provider || row->provider_name != admitted.provider_name)
        refusal = Reason::DeploymentDiffers;
    if (refusal) {
        if (!source.may_resend)
            throw ProgramProviderForkResendRefusedError(*refusal, std::string(id), source.run_id, source_id);
        pending.push_back(
            {"P_FORK_PROVIDER_CALL_RESENT",
             "The fork sends a provider call that its source had already journaled at the same position, "
             "because the host allows it; the provider may charge it again.",
             json{{"call_id", std::string(id)}, {"node", identity.node_name},
                  {"reason", to_string(*refusal)}, {"source_run_id", source.run_id},
                  {"source_call_id", source_id}}});
        return std::nullopt;
    }
    if (row->state == "dispatched" || row->state == "reconciliation_required")
        throw ProgramProviderDispatchUncertainError(
            "Program provider journal dispatch outcome of the forked call is uncertain; "
            "reconciliation required");
    if (row->state != "succeeded") return std::nullopt;
    auto       result  = restore_outcome(*row, owner, source_id, impl.archive);
    Stored     adopted = admitted;
    adopted.attempt    = row->attempt;
    adopted.claim      = row->claim;
    adopted.committed  = row->committed;
    adopted.deadline   = row->deadline;
    const auto canonical = neograph::detail::canonical_json_bytes(
        provider_codec::encode_outcome(*result, impl.archive, archive_binding(owner, id, adopted)));
    const auto outcome = neograph::detail::sha256_identity(outcome_domain, "outcome", canonical);
    std::string monetary;
    if (!row->monetary.empty()) {
        auto record = program::detail::decode_monetary_record(
            neograph::detail::parse_json_strict(row->monetary));
        record.call_id = std::string(id);
        monetary       = encode_monetary(record);
    }
    Statement insert(impl.db, "INSERT INTO ng_program_provider_calls "
                     "(owner_scope,dispatch_id,slot,request_digest,provider_binding_identity,"
                     "model,original_attempt,state,completion,outcome_digest,evidence_identity,"
                     "encoding_version,provider_name,family,mode,original_deadline,"
                     "logical_scope,claim_amount,committed_amount,monetary_record,"
                     "identity_version,content_digest,policy_identity,producer,opens_upload)"
                     " VALUES(?1,?2,?3,?4,?5,?6,?7,'succeeded',?8,?9,?10,'2',?11,?12,?13,?14,?15,?16,?17,?18,"
                     "'2',?19,?20,?21,?22)");
    insert.bind(1, owner); insert.bind(2, id); insert.bind(3, adopted.slot);
    insert.bind(4, adopted.request); insert.bind(5, adopted.provider);
    insert.bind(6, adopted.model); insert.bind(7, adopted.attempt);
    insert.bind(8, canonical); insert.bind(9, outcome); insert.bind(10, row->evidence);
    insert.bind(11, adopted.provider_name); insert.bind(12, adopted.family);
    insert.bind(13, adopted.mode); insert.bind(14, adopted.deadline);
    insert.bind(15, adopted.scope); insert.bind(16, std::to_string(adopted.claim));
    insert.bind(17, std::to_string(adopted.committed)); insert.bind(18, monetary);
    insert.bind(19, adopted.content); insert.bind(20, adopted.policy); insert.bind(21, adopted.producer);
    insert.bind(22, adopted.opens_upload ? "1" : "");
    insert.done();
    if (relation == Relation::PolicyChanged)
        note_policy_change(impl.db, owner, id, *row, admitted, attempt, true, identity.node_name, source_id,
                           pending);
    return result;
}

class BoundBroker final : public graph::ProviderCallBroker {
public:
    BoundBroker(std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl,
                ProgramCoreProviderCallContext context, std::string provider_identity)
        : impl_(std::move(impl)), owner_(context.owner_scope), version_(context.program_version_id),
          run_(context.run_id), operation_(context.operation_id), attempt_(context.attempt),
          provider_identity_(std::move(provider_identity)),
          monetary_(std::move(context.monetary_account)),
          replay_(std::move(context.replay_source)),
          diagnostic_(std::move(context.diagnostic)) {}

    asio::awaitable<sp::runtime::Result> invoke(graph::ProviderCallIdentity identity,
                                               std::shared_ptr<Provider> provider,
                                               ProviderRequest request) override {
        return invoke_owned(impl_, owner_, version_, run_, operation_, attempt_,
                            provider_identity_, monetary_, replay_, diagnostic_, std::move(identity),
                            std::move(provider), std::move(request));
    }
private:
    static asio::awaitable<sp::runtime::Result> invoke_owned(
        std::shared_ptr<SQLiteProgramProviderCallJournal::Impl> impl,
        std::string owner, std::string version, std::string run, std::string operation,
        std::uint64_t attempt, std::string provider_identity,
        std::shared_ptr<ProgramMonetaryAccount> monetary,
        std::optional<ProgramCoreProviderCallReplaySource> replay,
        std::function<void(ProgramCoreProviderCallDiagnostic)> diagnostic,
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
        // Admission and encoding happen before any durable dispatched marker.
        // Dropping this handle cannot dispatch transport.
        // Price the call first: pinning the route ceiling changes the request,
        // and everything below must see the request that is actually sent.
        auto priced = program::detail::prepare_priced_call(*provider, std::move(request), monetary != nullptr);
        auto prepared = std::move(priced.prepared);
        if (!prepared.valid()) co_return program::detail::preparation_failure_outcome(prepared);
        // An endpoint read (a job poll or download) is never charged and is safe to send again, so it is sent each
        // time rather than journaled: a resumed poll must see the job as it is now, not replay what it was. A
        // priced binding still refuses it below, as it refuses every endpoint call it cannot price.
        if (prepared.is_endpoint_read() && !priced.quote) co_return co_await provider->dispatch_async(std::move(prepared));
        const ProgramCoreProviderCallContext context{owner, version, run, operation, attempt};
        const auto id = dispatch_id(context, identity);
        program::detail::MonetaryHold hold;
        if (priced.quote)
            hold = program::detail::MonetaryHold(
                monetary, std::move(*priced.quote),
                program::detail::MonetaryCallSite{id, operation, identity.node_name}, diagnostic);
        if (prepared.requires_native_custody() && !impl->archive)
            throw std::invalid_argument("Program provider requires configured trusted native custody");
        if (impl->archive) {
            const auto* descriptor = prepared.admitted_descriptor();
            if (!descriptor || !impl->archive->matches_descriptor(*descriptor))
                throw std::invalid_argument("Program provider native custody admission is incompatible");
        }
        Stored admitted;
        admitted.slot = neograph::detail::canonical_json_bytes(json{
            {"owner_scope", owner}, {"program_version_id", version}, {"run_id", run},
            {"operation_id", operation}, {"thread_id", identity.thread_id},
            {"task_id", identity.task_id}, {"node_name", identity.node_name},
            {"call_ordinal", identity.call_ordinal}});
        // The call is named by what it asks for, with the policy that admitted it recorded beside (#543).
        admitted.identity_version = "2";
        admitted.content = Provider::request_content_digest(prepared);
        admitted.policy = Provider::request_policy_identity(prepared);
        admitted.producer = producer_identity();
        admitted.opens_upload = prepared.opens_upload_session();
        admitted.legacy = Provider::request_digest(prepared);
        admitted.request = identity_of(admitted.content, admitted.policy);
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
        Diagnostics pending;
        {
            std::unique_lock lock(impl->mutex);
            Transaction tx(impl->db);
            const auto existing = load(impl->db, owner, id, impl->archive);
            auto relation = Relation::Same;
            if (existing) {
                relation = relation_of(*existing, admitted);
                if (existing->slot != admitted.slot || existing->provider != admitted.provider ||
                    existing->provider_name != admitted.provider_name ||
                    relation == Relation::Different || relation == Relation::Unknown)
                    throw std::runtime_error(conflict_text(*existing, admitted, relation));
            }
            sp::runtime::Result restored;
            hydrate_scope(*impl, owner, admitted.scope, identity, attempt, id,
                          existing ? &*existing : nullptr, restored);
            hydrate_monetary_scope(impl->db, owner, admitted.scope, impl->archive, monetary);
            if (existing) {
                const bool answered = !existing->completion.empty();
                // #541: the call opens a chunked upload's session, and the journal cannot give the session back:
                // its URL, which only the live call held, is not stored, or the answer is not known at all. The
                // session was opened to be written into, so when no call of the node after the opening is
                // journaled nothing was written, and opening a session charges nothing: the call is sent again,
                // and the row stays as it was, with the restart recorded beside it. A later call may have written into the session, and its position
                // is bound to that session, so the upload has to be sent again from the start. A priced or
                // token-budgeted binding refuses every endpoint call before it is journaled, so no such row exists
                // under one today; the conditions keep an upload that is priced one day from skipping its claim.
                if (admitted.opens_upload && !hold.active() && identity.model_token_budget == 0 &&
                    (!answered || !existing->withheld.empty())) {
                    if (auto later = later_call(impl->db, owner, admitted.scope, identity))
                        throw ProgramProviderUploadResendRequiredError(id, std::move(*later));
                    const char* const reason = answered ? "session_not_stored" : "outcome_unknown";
                    // The restart is recorded in this transaction, before the call is sent. The row stays as it was.
                    note_restart(impl->db, owner, id, *existing, attempt, reason);
                    pending.push_back(
                        {"P_PROVIDER_UPLOAD_RESTARTED",
                         "A chunked upload was resumed before any part of it was sent. The journal does not hold "
                         "its session's URL, so the call that opens the upload was sent again and the earlier "
                         "session was left unused.",
                         json{{"call_id", std::string(id)}, {"node", identity.node_name},
                              {"reason", reason}}});
                    tx.commit();
                    lock.unlock();
                    report(diagnostic, pending);
                    co_return co_await provider->dispatch_async(std::move(prepared));
                }
                if (answered) {
                    if (!restored) restored = restore_outcome(*existing, owner, id, impl->archive);
                    if (committed_amount(*restored, existing->claim) != existing->committed)
                        throw std::runtime_error("Program provider persisted authority differs from the full outcome");
                    if (relation == Relation::PolicyChanged)
                        note_policy_change(impl->db, owner, id, *existing, admitted, attempt, false,
                                           identity.node_name, {}, pending);
                    tx.commit();
                    report(diagnostic, pending);
                    // Replay preserves the entire outcome and emits no events.
                    co_return restored;
                }
                throw ProgramProviderDispatchUncertainError(
                    "Program provider journal dispatch outcome uncertain; reconciliation required");
            }
            if (replay) {
                if (auto adopted = adopt_source_call(*impl, owner, id, admitted, operation,
                                                     identity, *replay, attempt, pending)) {
                    tx.commit();
                    report(diagnostic, pending);
                    co_return std::move(*adopted);
                }
            }
            // Monetary preflight: a call that cannot be bounded, or whose hold
            // does not fit, is refused here, before any marker or transport.
            if (auto refused = hold.preflight()) co_return refused;
            try {
                ProviderDispatchBudget budget{identity.usage, identity.model_token_budget,
                                              identity.budget_exhausted, identity.budget_cancel_token};
                budget.model_token_budget_bounded = identity.model_token_budget_bounded;
                claim = reserve_provider_dispatch(prepared, std::move(budget));
            } catch (const ProviderFailure& error) {
                hold.release();
                co_return error.outcome();
            }
            admitted.claim = claim.amount();
            admitted.committed = admitted.claim;
            // The provisional record charges the hold if the outcome is never known.
            admitted.monetary = hold.active()
                ? encode_monetary(program::detail::unknown_outcome_record(
                      hold.quote(), program::detail::MonetaryCallSite{id, operation, identity.node_name},
                      "the outcome is unknown"))
                : std::string();
            Statement insert(impl->db, "INSERT INTO ng_program_provider_calls "
                             "(owner_scope,dispatch_id,slot,request_digest,provider_binding_identity,"
                             "model,original_attempt,state,encoding_version,provider_name,family,mode,original_deadline,"
                             "logical_scope,claim_amount,committed_amount,monetary_record,"
                             "identity_version,content_digest,policy_identity,producer,opens_upload)"
                             " VALUES(?1,?2,?3,?4,?5,?6,?7,'dispatched','2',?8,?9,?10,?11,?12,?13,?14,?15,"
                             "'2',?16,?17,?18,?19)");
            insert.bind(1, owner); insert.bind(2, id); insert.bind(3, admitted.slot);
            insert.bind(4, admitted.request); insert.bind(5, admitted.provider);
            insert.bind(6, admitted.model); insert.bind(7, admitted.attempt);
            insert.bind(8, admitted.provider_name); insert.bind(9, admitted.family);
            insert.bind(10, admitted.mode); insert.bind(11, admitted.deadline);
            insert.bind(12, admitted.scope); insert.bind(13, std::to_string(admitted.claim));
            insert.bind(14, std::to_string(admitted.committed));
            insert.bind(15, admitted.monetary);
            insert.bind(16, admitted.content); insert.bind(17, admitted.policy);
            insert.bind(18, admitted.producer); insert.bind(19, admitted.opens_upload ? "1" : "");
            insert.done();
            tx.commit();  // write-ahead barrier: no transport before durable commit
            report(diagnostic, pending);
            claim.mark_dispatched();
            hold.mark_dispatched();
            hold.remember(monetary_key(owner, id));
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
            // authoritative even when this best-effort annotation fails. The
            // provisional monetary record (the hold) stays the charge.
            hold.settle_unknown("the dispatch raised without an outcome");
            try {
                std::lock_guard lock(impl->mutex);
                Transaction tx(impl->db);
                save_outcome(impl->db, owner, id, "reconciliation_required", "", "", "", admitted.claim);
                tx.commit();
            } catch (...) {}
            // Run control keeps its type. Anything else becomes a typed
            // uncertain outcome, so no retry, from this slot or a fresh one,
            // sends the request again (#364). It keeps the transport's message.
            try {
                throw;
            } catch (const graph::CancelledException&) {
                throw;
            } catch (const graph::NodeInterrupt&) {
                throw;
            } catch (const HookBoundaryBlocked&) {
                throw;
            } catch (const std::exception& error) {
                throw ProgramProviderDispatchUncertainError(error.what(), std::current_exception());
            } catch (...) {
                throw ProgramProviderDispatchUncertainError(
                    "Program provider journal dispatch outcome uncertain; reconciliation required",
                    std::current_exception());
            }
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
        // Settle the money before persisting: the call happened whether or not
        // the journal write below succeeds.
        auto monetary_record = hold.record_for(result);
        const auto monetary_bytes = hold.active() ? std::optional<std::string>(encode_monetary(monetary_record))
                                                  : std::optional<std::string>();
        hold.settle_record(std::move(monetary_record));
        try {
            const auto canonical = neograph::detail::canonical_json_bytes(provider_codec::encode_outcome(
                *result, impl->archive, archive_binding(owner, id, admitted)));
            const auto outcome = neograph::detail::sha256_identity(outcome_domain, "outcome", canonical);
            const auto* failure = std::get_if<sp::Failure>(result.get());
            const auto state = !failure ? "succeeded"
                : provider_failure_proves_not_accepted(*failure) ? "failed" : "reconciliation_required";
            // #541: the secret the opening of a session answers with reaches the live call only. The row says
            // that the answer it holds is short of it.
            const auto* completion = std::get_if<sp::Completion>(result.get());
            const auto withheld = admitted.opens_upload && completion && !completion->secret.empty()
                ? secret_identity(completion->secret) : std::string();
            std::lock_guard lock(impl->mutex);
            Transaction tx(impl->db);
            save_outcome(impl->db, owner, id, state, canonical, outcome, "", admitted.committed,
                         monetary_bytes ? std::optional<std::string_view>(*monetary_bytes)
                                        : std::nullopt,
                         withheld);
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
    std::shared_ptr<ProgramMonetaryAccount> monetary_;
    std::optional<ProgramCoreProviderCallReplaySource> replay_;
    std::function<void(ProgramCoreProviderCallDiagnostic)> diagnostic_;
};
}  // namespace

SQLiteProgramProviderCallJournal::SQLiteProgramProviderCallJournal(
    std::string path, std::shared_ptr<sp::NativeArchive> archive)
    : impl_(std::make_shared<Impl>(path, std::move(archive))) {}
SQLiteProgramProviderCallJournal::~SQLiteProgramProviderCallJournal() = default;

ProgramProviderForkResendRefusedError::ProgramProviderForkResendRefusedError(
    Reason reason, std::string call_id, std::string source_run_id, std::string source_call_id)
    : std::runtime_error(
          std::string("Program provider journal: the fork would send a provider call its source run already "
                      "journaled at the same position, and ") +
          (reason == Reason::DeploymentDiffers
               ? "the source journaled it under another provider deployment"
               : reason == Reason::EncoderDiffers
                     ? "the request content differs from the source's although another NeoGraph build encoded "
                       "the source's, so the request may be the same"
                     : "its journaled dispatch digest predates content identity and differs, so the call may "
                       "be the same under another descriptor policy") +
          "; set RuntimeConfig::fork_may_resend_provider_calls to send it again at the risk of paying twice"),
      reason_(reason), call_id_(std::move(call_id)), source_run_id_(std::move(source_run_id)),
      source_call_id_(std::move(source_call_id)) {}

ProgramProviderUploadResendRequiredError::ProgramProviderUploadResendRequiredError(
    std::string call_id, std::string later_call_id)
    : std::runtime_error(
          "Program provider journal: a chunked upload cannot continue after a resume. The call that opened "
          "its session is journaled without the session's URL, which a journal never stores, and the journal "
          "holds a later call of the same node, which the first session still binds. Send the upload again "
          "from its start, from a new run or at call ordinals the journal has not used"),
      call_id_(std::move(call_id)), later_call_id_(std::move(later_call_id)) {}

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
    auto unpriced             = context;
    unpriced.monetary_account = nullptr;
    return bind_priced(unpriced, std::move(provider_identity));
}

ProgramCoreProviderCallBinding SQLiteProgramProviderCallJournal::bind_priced(
    const ProgramCoreProviderCallContext& context, std::string provider_identity) {
    validate_context(context);
    if (impl_->archive && impl_->archive->owner_scope() != context.owner_scope)
        throw std::invalid_argument("Program provider archive owner scope mismatch");
    if (!neograph::detail::is_sha256_identity(provider_identity))
        throw std::invalid_argument("Program provider binding identity must be a deployment sha256 digest");
    const auto scope = hash("scope", {{"owner_scope", std::string(context.owner_scope)},
                                      {"program_version_id", std::string(context.program_version_id)},
                                      {"run_id", std::string(context.run_id)},
                                      {"operation_id", std::string(context.operation_id)}});
    if (context.monetary_account) {
        // Charge this operation's calls journaled by this attempt before a
        // restart now, even when the restarted Core never calls them again.
        std::lock_guard lock(impl_->mutex);
        hydrate_monetary_scope(impl_->db, context.owner_scope, scope, impl_->archive,
                               context.monetary_account);
    }
    if (context.journaled_usage) {
        // An attempt settled without running again is charged the tokens of
        // its journaled calls here, as its money above (#381).
        std::lock_guard lock(impl_->mutex);
        graph::ProviderCallIdentity identity;
        identity.owner_scope = std::string(context.owner_scope);
        identity.run_id      = std::string(context.run_id);
        identity.usage       = context.journaled_usage;
        sp::runtime::Result unused;
        hydrate_scope(*impl_, context.owner_scope, scope, identity, context.attempt, {}, nullptr,
                      unused);
    }
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
    std::optional<ProgramMonetaryRecord> monetary;
    if (!stored->monetary.empty())
        monetary = program::detail::decode_monetary_record(
            neograph::detail::parse_json_strict(stored->monetary));
    const auto restarts = restart_count(impl_->db, owner, id);
    const auto state = state_of(stored->state);
    return Record{state,
                  stored->request, stored->provider, stored->attempt, stored->outcome,
                  stored->provider_name, stored->model, stored->family, stored->mode,
                  stored->deadline, 2, stored->claim, stored->committed, std::move(monetary),
                  stored->identity_version == "2" ? 2U : 1U, stored->content, stored->policy,
                  stored->producer, stored->opens_upload, static_cast<std::uint32_t>(restarts),
                  restarts != 0 && (state == State::Dispatched || state == State::ReconciliationRequired)};
}

std::vector<SQLiteProgramProviderCallJournal::Restart>
SQLiteProgramProviderCallJournal::restarts(std::string_view owner, std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    Statement rows(impl_->db, "SELECT attempt,reason,replaced_state,replaced_outcome "
                              "FROM ng_program_provider_call_restarts "
                              "WHERE owner_scope=?1 AND dispatch_id=?2 ORDER BY restart");
    rows.bind(1, owner);
    rows.bind(2, id);
    std::vector<Restart> restarts;
    while (rows.next()) {
        const auto reason = rows.column(1);
        const auto state  = rows.column(2);
        if ((reason != "session_not_stored" && reason != "outcome_unknown") ||
            (state != "dispatched" && state != "succeeded" && state != "reconciliation_required"))
            throw std::runtime_error("Corrupt Program provider journal restart");
        restarts.push_back({reason == "session_not_stored" ? Restart::Reason::SessionNotStored
                                                           : Restart::Reason::OutcomeUnknown,
                            parse_amount(rows.column(0)), state_of(state), rows.column(3)});
    }
    return restarts;
}

std::vector<SQLiteProgramProviderCallJournal::PolicyChange>
SQLiteProgramProviderCallJournal::policy_changes(std::string_view owner, std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    Statement rows(impl_->db, "SELECT journaled_policy,observed_policy,attempt,via "
                              "FROM ng_program_provider_call_policy_changes "
                              "WHERE owner_scope=?1 AND dispatch_id=?2 ORDER BY rowid");
    rows.bind(1, owner);
    rows.bind(2, id);
    std::vector<PolicyChange> changes;
    while (rows.next()) {
        const auto via = rows.column(3);
        if (via != "replay" && via != "adoption")
            throw std::runtime_error("Corrupt Program provider journal policy change");
        changes.push_back({rows.column(0), rows.column(1), parse_amount(rows.column(2)),
                           via == "adoption" ? PolicyChange::Via::Adoption : PolicyChange::Via::Replay});
    }
    return changes;
}

void SQLiteProgramProviderCallJournal::reconcile_success(
    std::string_view owner, std::string_view id, std::string evidence,
    sp::runtime::Result completion) {
    if (owner.empty() || id.empty() || !neograph::detail::is_sha256_identity(evidence) ||
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
        // #541: a resume restarts or refuses an upload's opening by itself.
        if (restart_count(impl_->db, owner, id) != 0)
            throw std::runtime_error(
                "Program provider reconciliation refused: a resume sent this call, which opens a chunked "
                "upload's session, again in place of its record. A resume restarts the upload, or refuses it "
                "with ProgramProviderUploadResendRequiredError, so the record needs no reconciliation");
        std::string withheld;
        if (existing->opens_upload) {
            // A session continues by an upload id or by a URL, and a journal keeps no URL.
            const auto  typed    = sp::files::typed_progress(*completion);
            const auto* progress = std::get_if<sp::files::Progress>(&typed);
            if (!progress || (progress->session.id.empty() && progress->session.url.empty()))
                throw std::runtime_error(
                    "Program provider reconciliation refused: this call opens a chunked upload's session, and the "
                    "completion's session names neither an upload id nor a URL, so a resume could not continue it. "
                    "A resume sends the call again, and needs no reconciliation");
            const auto& secret = std::get<sp::Completion>(*completion).secret;
            if (!secret.empty()) withheld = secret_identity(secret);
        }
        auto reconciled = *existing;
        reconciled.committed = committed_amount(*completion, reconciled.claim);
        const auto canonical = neograph::detail::canonical_json_bytes(provider_codec::encode_outcome(
            *completion, impl_->archive, archive_binding(owner, id, reconciled)));
        const auto outcome = neograph::detail::sha256_identity(outcome_domain, "outcome", canonical);
        save_outcome(impl_->db, owner, id, "succeeded", canonical, outcome, evidence, reconciled.committed,
                     std::nullopt, withheld);
    }
    tx.commit();
}
}  // namespace neograph::program
