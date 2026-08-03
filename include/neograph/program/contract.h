/**
 * @file program/contract.h
 * @brief Frozen implementation contracts and fail-closed verification evidence.
 *
 * The contract layer is deliberately a value protocol owned by Program.  It
 * records what a planner proposed, what a reviewer froze, and what an
 * independent runner actually observed.  Worker self-reports are retained as
 * evidence but never satisfy an acceptance gate.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class ContractManifestLifecycle : std::uint8_t {
    Proposed,
    Reviewed,
    Frozen,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ContractManifestLifecycle state) noexcept;
NEOGRAPH_PROGRAM_API ContractManifestLifecycle
contract_manifest_lifecycle_from_string(std::string_view value);

struct NEOGRAPH_PROGRAM_API ContractItem {
    std::string id;
    std::string description;

    bool operator==(const ContractItem&) const = default;
};

struct NEOGRAPH_PROGRAM_API ContractAcceptance {
    std::string id;
    std::string description;
    bool        required = true;
    json        expected = json::object();

    bool operator==(const ContractAcceptance&) const = default;
};

struct NEOGRAPH_PROGRAM_API ContractRisk {
    std::string id;
    std::string description;
    std::string mitigation;
    bool        blocking = true;

    bool operator==(const ContractRisk&) const = default;
};

struct NEOGRAPH_PROGRAM_API ContractTestVector {
    std::string id;
    json        input    = json::object();
    json        expected = json::object();

    bool operator==(const ContractTestVector&) const = default;
};

struct NEOGRAPH_PROGRAM_API ContractRetryPolicy {
    std::uint32_t max_attempts          = 1;
    std::uint64_t max_program_operations = 1;
    std::uint64_t max_wall_time_ms       = 1;

    bool operator==(const ContractRetryPolicy&) const = default;
};

/** Mutable planner proposal accepted by ContractManifest::propose(). */
struct NEOGRAPH_PROGRAM_API ContractManifestSpec {
    std::uint32_t schema_version = 1;
    std::string   manifest_id;
    std::string   owner_scope;
    std::string   scope;
    std::vector<std::string> assumptions;
    std::vector<ContractItem> requirements;
    std::vector<std::string> non_goals;
    std::vector<ContractAcceptance> acceptance;
    std::vector<ContractTestVector> fixed_test_vectors;
    std::vector<std::string> independent_oracles;
    std::vector<ContractRisk> risk_register;
    std::vector<std::string> permissions;
    ContractRetryPolicy retry_policy;

    bool operator==(const ContractManifestSpec&) const = default;
};

/** Immutable manifest review recorded before a proposal can be frozen. */
struct NEOGRAPH_PROGRAM_API ContractReview {
    std::string reviewer_id;
    std::string notes;
    bool        approved = false;

    bool operator==(const ContractReview&) const = default;
};

/**
 * A planner manifest.  Every lifecycle transition returns a new value; a
 * worker therefore cannot mutate the frozen contract it receives.
 */
class NEOGRAPH_PROGRAM_API ContractManifest final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ContractManifest propose(ContractManifestSpec spec);
    static ContractManifest parse(std::string_view stored_bytes);

    ContractManifest review(ContractReview review) const;
    ContractManifest freeze() const;

    ContractManifestLifecycle lifecycle() const noexcept;
    const ContractManifestSpec& spec() const noexcept;
    const std::optional<ContractReview>& review_record() const noexcept;
    const std::string& content_hash() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContractManifest(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> impl_;
};

enum class ContractEvidenceKind : std::uint8_t {
    WorkerReport,
    DeterministicRun,
    IndependentOracle,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ContractEvidenceKind kind) noexcept;
NEOGRAPH_PROGRAM_API ContractEvidenceKind contract_evidence_kind_from_string(std::string_view value);

struct NEOGRAPH_PROGRAM_API ContractEvidence {
    std::string         evidence_id;
    std::string         acceptance_id;
    ContractEvidenceKind kind = ContractEvidenceKind::WorkerReport;
    std::string         manifest_hash;
    std::string         program_version_id;
    std::string         workspace_revision;
    std::string         command;
    std::string         toolchain;
    std::string         artifact_hash;
    bool                executed = false;
    bool                passed   = false;
    std::vector<std::string> diagnostics;
    json                details = json::object();

    bool operator==(const ContractEvidence&) const = default;
};

struct NEOGRAPH_PROGRAM_API ContractDiagnostic {
    std::string code;
    std::string message;
    bool        blocking = true;

    bool operator==(const ContractDiagnostic&) const = default;
};

enum class ContractRunStatus : std::uint8_t {
    Frozen,
    Running,
    Verified,
    Published,
    Blocked,
    Failed,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ContractRunStatus status) noexcept;
NEOGRAPH_PROGRAM_API ContractRunStatus contract_run_status_from_string(std::string_view value);

struct NEOGRAPH_PROGRAM_API ContractVerification {
    bool                     publishable = false;
    ContractRunStatus        status      = ContractRunStatus::Blocked;
    std::vector<std::string> missing_acceptance_ids;
    std::vector<std::string> blocking_diagnostics;
    std::vector<std::string> failed_evidence_ids;

    bool operator==(const ContractVerification&) const = default;
};

/**
 * Durable-boundary state machine for one implementation attempt.  It is
 * intentionally independent of a provider or transport: Harness can drive it
 * through Program, MCP, A2A, or a local deterministic runner.
 */
class NEOGRAPH_PROGRAM_API ContractRun final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ContractRun(ContractManifest frozen_manifest);
    static ContractRun parse(std::string_view stored_bytes);

    const ContractManifest& manifest() const noexcept;
    ContractRunStatus       status() const noexcept;
    std::uint32_t            attempt() const noexcept;
    const std::vector<ContractEvidence>& evidence() const noexcept;
    const std::vector<ContractDiagnostic>& diagnostics() const noexcept;
    const std::optional<ContractVerification>& verification() const noexcept;

    /// Start one bounded worker attempt.  Only a frozen manifest may run.
    void begin_attempt();

    /// Store a worker claim without granting it verification authority.
    void record_worker_report(std::string report, bool claims_success);

    /// Record independently observed evidence bound to this manifest.
    void record_evidence(ContractEvidence evidence);
    void record_diagnostic(ContractDiagnostic diagnostic);

    /// Evaluate all required gates; missing/failed evidence becomes blocked.
    ContractVerification verify(std::string_view program_version_id,
                                std::string_view workspace_revision);

    /// Mark the run publishable only after a successful verification.
    void publish();

    /// Canonical checkpoint/recovery representation.
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ContractRun(std::shared_ptr<Impl> impl);

    std::shared_ptr<Impl> impl_;
};

}  // namespace neograph::program
