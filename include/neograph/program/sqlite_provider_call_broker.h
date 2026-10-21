#pragma once

#include <neograph/graph/configuration_failure.h>
#include <neograph/program/core_provider_call.h>
#include <neograph/program/result.h>
#include <neograph/types.h>
#include <neograph/provider.h>
#include <core/native_archive.h>
#include <runtime/client.h>

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <utility>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

/** A fork reached a provider call its source already journaled at the same
 * position, and could neither adopt the source's record nor show that the call
 * differs, so sending it would likely pay for it again (#543). It is raised
 * before anything is sent, and nothing is journaled. A host that accepts the
 * second payment sets RuntimeConfig::fork_may_resend_provider_calls; the fork
 * then sends the call and reports P_FORK_PROVIDER_CALL_RESENT in
 * ProgramHandle::notices(). Inside a Core node it is NG.ConfigError: it is
 * never retried and never routed.
 */
class NEOGRAPH_PROGRAM_API ProgramProviderForkResendRefusedError final
    : public std::runtime_error,
      public graph::ConfigurationFailure {
public:
    enum class Reason : std::uint8_t {
        /// The source's record predates the content identity (it was written by 0.16.0
        /// or earlier), and its dispatch digest differs from this call's: the call
        /// may be the same under another descriptor policy, or a different one.
        Unverifiable,
        /// The source's record was written by an encoder other than this build's and
        /// its content differs: the request may have changed, or only its encoding.
        EncoderDiffers,
        /// The content is the same, but the source journaled the call under another
        /// provider deployment.
        DeploymentDiffers,
    };
    ProgramProviderForkResendRefusedError(Reason reason, std::string call_id,
                                          std::string source_run_id, std::string source_call_id);
    Reason             reason() const noexcept { return reason_; }
    /// The fork's logical call id (SQLiteProgramProviderCallJournal::logical_call_id).
    const std::string& call_id() const noexcept { return call_id_; }
    const std::string& source_run_id() const noexcept { return source_run_id_; }
    /// The source's logical call id of the record that could not be adopted.
    const std::string& source_call_id() const noexcept { return source_call_id_; }
private:
    Reason      reason_;
    std::string call_id_, source_run_id_, source_call_id_;
};

/** A resumed Program reached a chunked upload that cannot continue (#541). The call
 * that opened the upload's session is journaled, but its answer holds a value no
 * journal stores (a Google resumable upload's session URL, which lets anyone write
 * the file without the API key), so the journal cannot give the session back. The
 * journal also holds a later call of the same node, a part sent into the session or
 * one that may have been, so the upload cannot be sent from its start under the
 * same call positions either: those positions are bound to the first session. It is
 * raised before anything is sent, and nothing is journaled. The upload has to be
 * sent again: from a new run, or by a node that catches this error and uploads at
 * call ordinals the journal has not used. Inside a Core node it is NG.ConfigError:
 * the journal would refuse the same call again, so it is never retried and never
 * routed.
 */
class NEOGRAPH_PROGRAM_API ProgramProviderUploadResendRequiredError final
    : public std::runtime_error,
      public graph::ConfigurationFailure {
public:
    ProgramProviderUploadResendRequiredError(std::string call_id, std::string later_call_id);
    /// The logical call id (SQLiteProgramProviderCallJournal::logical_call_id) of the call that opened the upload.
    const std::string& call_id() const noexcept { return call_id_; }
    /// The logical call id of the first later call of the same node that the journal holds.
    const std::string& later_call_id() const noexcept { return later_call_id_; }
private:
    std::string call_id_, later_call_id_;
};

/** Host-owned SQLite dispatch journal. Share the journal across resolver calls/runs.
 * A marker means transport MAY have occurred; it does not prove provider receipt
 * or exactly-once execution. Pending/uncertain calls require external reconciliation.
 * A runtime whose resolver returns this journal's brokers should set
 * RuntimeConfig::core_provider_calls_journaled, because only a durable journal lets
 * a fork replay the provider calls of the paused invocation.
 *
 * Call identity (#543). A call is named by its position (owner, Program version, run,
 * operation, Core thread, task, node and ordinal) and by its request content
 * (Provider::request_content_digest): what the request asks for, not the
 * SchemaProvider descriptor policy that admitted it. The policy's identity
 * (Provider::request_policy_identity) is recorded beside the content. A resume,
 * a crash recovery or a fork that finds the same content under another policy
 * identity replays or adopts the record, notes the change durably
 * (policy_changes()) and reports P_PROVIDER_CALL_POLICY_CHANGED on the run's
 * handle. A different content is a binding conflict on resume and, in a fork, a
 * new call. Rows written before this identity (0.16.0 and earlier; identity
 * version 1) hold only a dispatch digest that includes the policy, so they match
 * only while the policy is unchanged; a fork that cannot tell such a call from a
 * new one is refused (ProgramProviderForkResendRefusedError).
 *
 * Chunked uploads (#541). The answer to the call that opens an upload's session
 * (PreparedProviderRequest::opens_upload_session()) may carry a value that only the
 * live call may hold: a Google resumable upload's URL, which lets anyone write the
 * file without the API key (Completion::secret). The journal never stores it, and
 * the row says that its answer is short of it (withheld_secret). A resume that
 * finds such a row, or an opening whose answer is not known, cannot give the
 * session back. Where the journal holds no later call of the node's task, nothing
 * was written into the session and opening one charges nothing, so the call is
 * sent again; the row is not rewritten, and the handle carries
 * P_PROVIDER_UPLOAD_RESTARTED. Where it holds one, in any state, the later calls
 * name the first session by the digest of its URL, and the resume fails with
 * ProgramProviderUploadResendRequiredError before anything is sent. A fork sends
 * the opening again, since the source's session is the source's alone. An opening
 * that reconcile_success() settled has no secret to withhold and replays without
 * the session's URL: the upload cannot be continued, and is sent again.
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
        /// #543. 2: request_digest names the content and the policy together, and
        /// content_digest, policy_identity and producer are set. 1: a row written
        /// by 0.16.0 or earlier, whose request_digest is the dispatch digest
        /// (Provider::request_digest) and whose three members are empty.
        std::uint32_t identity_version = 2;
        /// Provider::request_content_digest of the journaled call.
        std::string content_digest;
        /// Provider::request_policy_identity of the policy that admitted it.
        std::string policy_identity;
        /// The NeoGraph build that encoded the request.
        std::string producer;
    };
    std::optional<Record> inspect(std::string_view owner_scope,
                                  std::string_view dispatch_id) const;

    /// A time a call was matched to its record under another descriptor policy
    /// identity than the record names (#543).
    struct PolicyChange {
        enum class Via : std::uint8_t {
            /// The call itself was found again: a resume or a crash recovery.
            Replay,
            /// A fork adopted its source's record of the call.
            Adoption,
        };
        std::string   journaled_policy_identity;
        std::string   observed_policy_identity;
        std::uint64_t attempt = 0;
        Via           via     = Via::Replay;
    };
    /// The policy changes noted for one call, oldest first. A call that was found
    /// again under the same policy has none.
    std::vector<PolicyChange> policy_changes(std::string_view owner_scope,
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
