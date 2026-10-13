#pragma once

#include <neograph/program/core_provider_call.h>
#include <neograph/program/result.h>
#include <neograph/types.h>
#include <neograph/provider.h>
#include <core/native_archive.h>
#include <runtime/client.h>

#include <exception>
#include <stdexcept>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

/** Transport returned this exact outcome, but its durable settlement failed.
 * This is not permission to retry: the dispatched journal marker still stands.
 */
class NEOGRAPH_PROGRAM_API ProgramProviderOutcomePersistenceError final : public ProviderOutcomeError {
public:
    ProgramProviderOutcomePersistenceError(
        sp::runtime::Result outcome, std::exception_ptr cause,
        std::exception_ptr observer_error = {})
        : ProviderOutcomeError("Program provider outcome could not be persisted",
                               std::move(outcome), std::move(cause)),
          observer_error_(std::move(observer_error)) {}
    const std::exception_ptr& observer_error() const noexcept { return observer_error_; }
private:
    std::exception_ptr observer_error_;
};

/** A dispatch may have reached the provider, but no outcome is known: the
 * transport raised instead of returning one, or an earlier dispatch of this
 * logical call is still unresolved. It carries no outcome; when the transport
 * raised, what() is the transport's message and cause() its exception. The
 * journal marker stands: this is
 * not permission to dispatch again, under this call slot or a fresh one, so
 * node retries and failure routes treat it as an uncertain effect
 * (NG.EffectUncertain). Reconcile it with
 * SQLiteProgramProviderCallJournal::reconcile_success.
 */
class NEOGRAPH_PROGRAM_API ProgramProviderDispatchUncertainError final : public ProviderOutcomeError {
public:
    explicit ProgramProviderDispatchUncertainError(std::string message,
                                                   std::exception_ptr cause = {})
        : ProviderOutcomeError(message.c_str(), nullptr, std::move(cause)) {}
};

/** Host-owned SQLite dispatch journal. Share the journal across resolver calls/runs.
 * A marker means transport MAY have occurred; it does not prove provider receipt
 * or exactly-once execution. Pending/uncertain calls require external reconciliation.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramProviderCallJournal final {
public:
    explicit SQLiteProgramProviderCallJournal(
        std::string database_path, std::shared_ptr<sp::NativeArchive> native_archive = {});
    ~SQLiteProgramProviderCallJournal();
    SQLiteProgramProviderCallJournal(const SQLiteProgramProviderCallJournal&) = delete;
    SQLiteProgramProviderCallJournal& operator=(const SQLiteProgramProviderCallJournal&) = delete;

    /** The provider identity is the host's immutable deployment/credential-route digest.
     * It must change when any dispatch authority or deployment changes.
     */
    ProgramCoreProviderCallBinding bind(const ProgramCoreProviderCallContext& context,
                                        std::string provider_binding_identity);
    /** bind() that also prices every call against the context's monetary
     * account, as bind_priced_provider_call() does, and keeps the call's
     * ProgramMonetaryRecord in its row with the outcome. A call this attempt
     * journaled before a restart is charged once, when the restarted attempt
     * binds its operation; a call an earlier attempt journaled was settled by
     * that attempt's result and is never charged again.
     */
    ProgramCoreProviderCallBinding bind_priced(const ProgramCoreProviderCallContext& context,
                                               std::string provider_binding_identity);
    /** Stable across Program attempts; request content is bound to the slot. */
    static std::string logical_call_id(const ProgramCoreProviderCallContext& context,
                                       const graph::ProviderCallIdentity& call);

    enum class State { Dispatched, Succeeded, ReconciliationRequired, Failed };
    struct Record {
        State state;
        std::string request_digest;
        std::string provider_binding_identity;
        std::string original_attempt;
        std::string outcome_digest;
        std::string provider_name;
        std::string model;
        std::string family;
        std::string mode;
        std::string original_deadline;
        std::uint32_t encoding_version;
        /// Conservative authority, not provider-reported usage.
        std::uint64_t claim_amount;
        std::uint64_t committed_amount;
        /// The priced call's record: the held provisional record while the
        /// outcome is unknown, the settled record after it. Empty when the call
        /// was not priced or the provider accepted nothing.
        std::optional<ProgramMonetaryRecord> monetary_record;
    };
    std::optional<Record> inspect(std::string_view owner_scope,
                                  std::string_view dispatch_id) const;

    /** Only after independent provider-side evidence establishes this exact result.
     * The evidence identity is retained, immutable and must not be empty. This
     * method never retries transport; a later invoke replays the verified result.
     */
    void reconcile_success(std::string_view owner_scope, std::string_view dispatch_id,
                           std::string evidence_identity, sp::runtime::Result completion);

    struct Impl;
private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph::program
