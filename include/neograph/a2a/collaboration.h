/**
 * @file a2a/collaboration.h
 * @brief Program-owned collaboration envelopes carried by A2A.
 *
 * This is a transport adapter, not another execution engine.  A collaboration
 * link explicitly scopes two independently owned agents; envelopes are
 * idempotent, sequence-bearing, and retain unknown terminal kinds verbatim.
 * A mailbox is a durable-boundary journal that callers persist alongside their
 * local ProgramTransitionStore before acknowledging remote delivery.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/types.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef NEOGRAPH_A2A_PROGRAM
namespace neograph::program {
class ProgramVersion;
struct RunInvocation;
}  // namespace program
#endif

namespace neograph::a2a {

enum class CollaborationLinkState : std::uint8_t {
    Proposed,
    Accepted,
    Revoked,
};

NEOGRAPH_API std::string_view to_string(CollaborationLinkState state) noexcept;
NEOGRAPH_API CollaborationLinkState collaboration_link_state_from_string(std::string_view value);

/** Explicit invitation and capability boundary between two owners. */
struct NEOGRAPH_API CollaborationLinkSpec {
    std::uint32_t schema_version = 2;
    std::string   link_id;
    std::string   sender_owner_scope;
    std::string   receiver_owner_scope;
    std::string   sender_agent_id;
    std::string   receiver_agent_id;
    std::string   task_scope;
    std::vector<std::string> capability_allowlist;
    std::vector<std::string> effect_allowlist;
    std::vector<std::string> artifact_allowlist;
    std::vector<std::string> cancellation_rights;
    /// Explicit protocol actions permitted across this owner boundary.
    std::vector<std::string> message_kind_allowlist;
    std::uint64_t expires_at_unix_ms = 0;
    std::uint32_t max_retries = 0;
    std::uint64_t acknowledgement_timeout_ms = 1;

    bool operator==(const CollaborationLinkSpec&) const = default;
};

/** Immutable invitation state. Accept/revoke return a new value. */
class NEOGRAPH_API CollaborationLink final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    static CollaborationLink create(CollaborationLinkSpec spec);
    static CollaborationLink parse(std::string_view stored_bytes);

    CollaborationLink accept(std::string receiver_agent_id, std::string consent_token) const;
    CollaborationLink revoke(std::string actor_agent_id) const;

    CollaborationLinkState state() const noexcept;
    const CollaborationLinkSpec& spec() const noexcept;
    const std::string& consent_fingerprint() const noexcept;
    const std::string& content_hash() const noexcept;
    bool is_expired(std::uint64_t now_unix_ms) const noexcept;
    bool permits_capability(std::string_view capability) const noexcept;
    bool permits_effect(std::string_view effect) const noexcept;
    bool permits_artifact(std::string_view artifact_identity) const noexcept;
    bool permits_message_kind(std::string_view kind) const noexcept;
    bool permits_cancellation(std::string_view actor_agent_id) const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit CollaborationLink(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct NEOGRAPH_API CollaborationArtifactReference {
    std::string artifact_identity;
    std::string uri;
    std::string media_type;
    std::uint64_t size_bytes = 0;

    bool operator==(const CollaborationArtifactReference&) const = default;
};

/**
 * The exact logical message sent over same-runtime mailboxes or A2A.  `kind`
 * remains a string intentionally: an unknown terminal/diagnostic kind is
 * preserved and cannot silently become success.
 */
struct NEOGRAPH_API CollaborationEnvelope {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    std::string link_id;
    std::string sender_owner_scope;
    std::string receiver_owner_scope;
    std::string sender_agent_id;
    std::string receiver_agent_id;
    std::string sender_program_run_id;
    std::string receiver_program_run_id;
    /// Optional admitted ProgramVersion identity. Legacy envelopes leave this empty.
    std::string program_version_id;
    std::string a2a_task_id;
    std::string a2a_context_id;
    std::string message_id;
    std::string correlation_id;
    std::uint64_t sequence = 0;
    std::string kind;
    std::string idempotency_key;
    json payload = json::object();
    std::vector<CollaborationArtifactReference> artifacts;

    static CollaborationEnvelope create(const CollaborationLink& link,
                                         std::string sender_program_run_id,
                                         std::string receiver_program_run_id,
                                         std::string a2a_task_id,
                                         std::string a2a_context_id,
                                         std::string message_id,
                                         std::string correlation_id,
                                         std::uint64_t sequence,
                                         std::string kind,
                                         std::string idempotency_key,
                                         json payload = json::object(),
                                         std::vector<CollaborationArtifactReference> artifacts = {});

#ifdef NEOGRAPH_A2A_PROGRAM
    /// Attach one exact admitted ProgramVersion identity to a collaboration request.
    /// The version bytes remain in the receiver's ProgramCatalog; this identity is
    /// carried so a reconnect cannot silently substitute another version.
    static CollaborationEnvelope bind_program(const CollaborationEnvelope& envelope,
                                              const program::ProgramVersion& version);
#endif

    static CollaborationEnvelope parse(std::string_view stored_bytes);
    std::string serialize_canonical() const;
    std::string content_hash() const;
    bool is_terminal() const noexcept;
};

/// Convert a logical collaboration envelope into an ordinary A2A Message.
NEOGRAPH_API Message collaboration_to_message(const CollaborationEnvelope& envelope);

/// Decode only messages carrying the NeoGraph collaboration metadata envelope.
NEOGRAPH_API CollaborationEnvelope collaboration_from_message(const Message& message);

enum class CollaborationRecordState : std::uint8_t {
    Accepted,
    Acknowledged,
    Canceled,
};

NEOGRAPH_API std::string_view to_string(CollaborationRecordState state) noexcept;
NEOGRAPH_API CollaborationRecordState collaboration_record_state_from_string(std::string_view value);

/** Transport-authenticated owner/agent identity; never serialize credentials. */
struct NEOGRAPH_API CollaborationPeerIdentity {
    std::string owner_scope;
    std::string agent_id;

    bool operator==(const CollaborationPeerIdentity&) const = default;
};

/**
 * Result of authorizing an already-known Program-backed collaboration task.
 * `NotLinked` preserves the legacy A2A task path; `Unauthorized` deliberately
 * does not disclose whether the task, link, or peer identity was wrong.
 */
enum class CollaborationTaskAuthorization : std::uint8_t {
    NotLinked,
    Authorized,
    Unauthorized,
};

struct NEOGRAPH_API CollaborationRecord {
    CollaborationEnvelope envelope;
    CollaborationRecordState state = CollaborationRecordState::Accepted;
    std::string diagnostic;
#ifdef NEOGRAPH_A2A_PROGRAM
    /// Exact Program request retained beside the link journal.  Its invocation
    /// bytes are the canonical transport-neutral Program contract; this
    /// mailbox does not persist a runtime-only invocation projection.
    struct ProgramRequest {
        std::shared_ptr<const program::ProgramVersion> version;
        std::shared_ptr<const program::RunInvocation> invocation;
    };
    std::optional<ProgramRequest> program_request;
#endif
};

enum class CollaborationSubmitResult : std::uint8_t {
    Accepted,
    Duplicate,
    Conflict,
    Rejected,
};

NEOGRAPH_API std::string_view to_string(CollaborationSubmitResult result) noexcept;

/**
 * Owner-scoped idempotent mailbox.  It is deliberately transport-neutral: the
 * A2A adapter submits an envelope here, then persists serialize_canonical()
 * with the local Program transition boundary before acknowledging the peer.
 */
class NEOGRAPH_API CollaborationMailbox final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    CollaborationMailbox(std::string owner_scope, std::string agent_id);
    CollaborationMailbox(CollaborationMailbox&&) noexcept;
    CollaborationMailbox& operator=(CollaborationMailbox&&) noexcept;
    CollaborationMailbox(const CollaborationMailbox&) = delete;
    CollaborationMailbox& operator=(const CollaborationMailbox&) = delete;
    ~CollaborationMailbox();

    static CollaborationMailbox parse(std::string_view stored_bytes);

    const std::string& owner_scope() const noexcept;
    const std::string& agent_id() const noexcept;

    /// Receiver-side consent; a proposed link is not usable before this call.
    void accept_link(CollaborationLink link, std::string consent_token);
    void revoke_link(std::string_view link_id, std::string actor_agent_id);

    CollaborationSubmitResult submit(CollaborationEnvelope envelope);
#ifdef NEOGRAPH_A2A_PROGRAM
    CollaborationSubmitResult submit_program(CollaborationEnvelope envelope,
                                              program::ProgramVersion version,
                                              program::RunInvocation invocation);
    /// Typed overload matching the legacy submit() naming convention.
    CollaborationSubmitResult submit(CollaborationEnvelope envelope,
                                      program::ProgramVersion version,
                                      program::RunInvocation invocation);
    std::optional<CollaborationRecord::ProgramRequest>
    get_program_request(std::string_view idempotency_key) const;
#endif
    bool acknowledge(std::string_view idempotency_key);
    bool cancel(std::string_view link_id,
                std::string_view correlation_id,
                std::string_view actor_agent_id);

    /// Unauthorized/missing links return nullopt without disclosing existence.
    std::optional<CollaborationRecord> get(std::string_view idempotency_key) const;
    /**
     * Verify that a transport-authenticated peer is the accepted sender for
     * one link. Missing, expired, revoked, and mismatched links fail closed.
     */
    bool authenticates_sender(std::string_view link_id,
                              const CollaborationPeerIdentity& peer) const;

    /**
     * Authorize one peer to read or cancel a Program-backed receiver task.
     * An unrelated legacy task returns NotLinked; linked task authorization
     * never leaks which identity or link check failed.
     */
    CollaborationTaskAuthorization authorize_task(
        std::string_view receiver_program_run_id,
        const CollaborationPeerIdentity& peer,
        bool require_cancellation = false) const;

    std::vector<CollaborationRecord> snapshot() const;
    /// Check the receiver-side artifact attenuation for one accepted link.
    /// Missing, revoked, expired, or unauthorized links return false.
    bool permits_artifact(std::string_view link_id,
                          std::string_view artifact_identity) const;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit CollaborationMailbox(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph::a2a
