/**
 * @file program/invocation.h
 * @brief Transport-neutral, owner-scoped Program invocation contracts.
 *
 * This value contract deliberately contains no transport/session or executor
 * types.  Adapters may map it to their wire envelopes, but the canonical
 * bytes and idempotency decision remain owned by Program.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Terminal states accepted by the invocation contract. */
enum class InvocationTerminalStatus : std::uint8_t {
    Unknown,
    Completed,
    Failed,
    Interrupted,
    Cancelled,
    BudgetExhausted,
    TimedOut,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(InvocationTerminalStatus status) noexcept;
/** Unknown wire values return Unknown; callers must not treat that as success. */
NEOGRAPH_PROGRAM_API InvocationTerminalStatus
invocation_terminal_status_from_string(std::string_view value) noexcept;
NEOGRAPH_PROGRAM_API bool invocation_terminal_status_succeeded(
    InvocationTerminalStatus status) noexcept;
NEOGRAPH_PROGRAM_API bool invocation_terminal_status_failed(
    InvocationTerminalStatus status) noexcept;

/** Severity is also open-ended on the wire; unknown values remain unknown. */
enum class InvocationDiagnosticSeverity : std::uint8_t {
    Unknown,
    Error,
    Warning,
    Info,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(InvocationDiagnosticSeverity severity) noexcept;
NEOGRAPH_PROGRAM_API InvocationDiagnosticSeverity
invocation_diagnostic_severity_from_string(std::string_view value) noexcept;

/** A diagnostic is retained even when its severity is not known locally. */
struct NEOGRAPH_PROGRAM_API InvocationDiagnostic {
    std::string                    code;
    std::string                    message;
    InvocationDiagnosticSeverity   severity = InvocationDiagnosticSeverity::Unknown;
    std::string                    severity_value;
    bool                           blocking = true;
    json                           witness = json::object();

    bool operator==(const InvocationDiagnostic&) const = default;
};

/**
 * Terminal publication is separate from the request. Unknown status values
 * are retained in status_value and classify as failed, never completed.
 */
struct NEOGRAPH_PROGRAM_API InvocationTerminal {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    InvocationTerminalStatus        status = InvocationTerminalStatus::Unknown;
    std::string                     status_value = "unknown";
    std::vector<InvocationDiagnostic> diagnostics;
    json                            output = json::object();

    static InvocationTerminal create(std::string status_value,
                                      json output = json::object(),
                                      std::vector<InvocationDiagnostic> diagnostics = {});
    static InvocationTerminal parse(std::string_view stored_bytes);

    bool succeeded() const noexcept;
    bool failed() const noexcept;
    std::string serialize_canonical() const;

    bool operator==(const InvocationTerminal&) const = default;
};

NEOGRAPH_PROGRAM_API void to_json(json& value, const InvocationDiagnostic& diagnostic);
NEOGRAPH_PROGRAM_API void from_json(const json& value, InvocationDiagnostic& diagnostic);
NEOGRAPH_PROGRAM_API void to_json(json& value, const InvocationTerminal& terminal);
NEOGRAPH_PROGRAM_API void from_json(const json& value, InvocationTerminal& terminal);

/** A transport-neutral artifact witness attached to one invocation. */
struct NEOGRAPH_PROGRAM_API InvocationArtifactReference {
    std::string   artifact_identity;
    std::string   uri;
    std::string   media_type;
    std::uint64_t size_bytes = 0;

    bool operator==(const InvocationArtifactReference&) const = default;
};

NEOGRAPH_PROGRAM_API void to_json(json& value, const InvocationArtifactReference& artifact);
NEOGRAPH_PROGRAM_API void from_json(const json& value, InvocationArtifactReference& artifact);

/**
 * One owner-scoped Program request.  `message_sequence` starts at one and is
 * part of the idempotency payload: a replay with a changed owner, sequence,
 * or request body is a conflict, not a second run.
 */
struct NEOGRAPH_PROGRAM_API RunInvocation {
    static constexpr std::uint32_t PROTOCOL_REVISION = 1;
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    std::uint32_t                       protocol_revision = PROTOCOL_REVISION;
    std::string                         owner_scope;
    std::string                         agent_id;
    std::string                         program_version_id;
    std::string                         run_id;
    std::string                         parent_run_id;
    RunBudget                           budget;
    json                                input = json::object();
    std::uint64_t                       message_sequence = 0;
    std::string                         idempotency_key;
    std::string                         correlation_id;
    std::optional<InvocationArtifactReference> artifact;

    /// Validate ownership, identity, sequence and required request fields.
    void validate() const;
    /// Canonical payload excludes the idempotency key and envelope metadata.
    std::string canonical_payload() const;
    /// Content-addressed identity of the canonical payload.
    std::string canonical_identity() const;
    std::string content_hash() const { return canonical_identity(); }
    std::string id() const { return canonical_identity(); }

    std::string serialize_canonical() const;
    static RunInvocation parse(std::string_view stored_bytes);
    std::string canonical_id() const { return canonical_identity(); }
    static RunInvocation from_canonical(std::string_view stored_bytes) { return parse(stored_bytes); }

    bool operator==(const RunInvocation& other) const;

};

NEOGRAPH_PROGRAM_API void to_json(json& value, const RunInvocation& invocation);
NEOGRAPH_PROGRAM_API void from_json(const json& value, RunInvocation& invocation);

/** Strict result for comparing one idempotency key in an owner mailbox. */
enum class InvocationIdempotencyResult : std::uint8_t {
    Distinct,
    Duplicate,
    Conflict,
};
using IdempotencyResult = InvocationIdempotencyResult;

NEOGRAPH_PROGRAM_API std::string_view to_string(InvocationIdempotencyResult result) noexcept;

/**
 * Compare requests at the durable boundary. Different/empty keys are
 * Distinct. Equal keys require byte-identical canonical payloads; any owner,
 * sequence, identity, budget, input, correlation, or artifact change is a
 * Conflict.
 */
NEOGRAPH_PROGRAM_API InvocationIdempotencyResult compare_invocations(
    const RunInvocation& existing,
    const RunInvocation& incoming);
NEOGRAPH_PROGRAM_API InvocationIdempotencyResult compare_invocation_idempotency(
    const RunInvocation& existing,
    const RunInvocation& incoming);

}  // namespace neograph::program
