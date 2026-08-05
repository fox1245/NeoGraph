/**
 * @file research/evidence_ledger.h
 * @brief Durable, typed coordination boundary for research work.
 *
 * Conversation is deliberately not ledger state.  Workers acquire a bounded
 * lease, publish immutable evidence, and leave disagreement visible for a
 * review or human-decision branch.  Implementations must make every mutation
 * atomic; callers can recover from process death by reacquiring expired work.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <limits>
#include <vector>
#include <utility>

namespace neograph::research {

enum class ResearchTaskKind : std::uint8_t {
    PrimaryExtraction,
    IndependentReview,
    Reproduction,
    Rebuttal,
    Reconciliation,
};

NEOGRAPH_API std::string_view to_string(ResearchTaskKind kind) noexcept;
NEOGRAPH_API ResearchTaskKind research_task_kind_from_string(std::string_view value);

enum class ResearchTaskState : std::uint8_t {
    Ready,
    Leased,
    Published,
    Cancelled,
};

NEOGRAPH_API std::string_view to_string(ResearchTaskState state) noexcept;
NEOGRAPH_API ResearchTaskState research_task_state_from_string(std::string_view value);

enum class EvidencePolarity : std::uint8_t {
    Supports,
    Contradicts,
    Inconclusive,
    NoSupport,
};

NEOGRAPH_API std::string_view to_string(EvidencePolarity polarity) noexcept;
NEOGRAPH_API EvidencePolarity evidence_polarity_from_string(std::string_view value);

enum class SourceLifecycle : std::uint8_t {
    Unclaimed,
    Claimed,
    Extracted,
    Reviewed,
    Corroborated,
    Contradicted,
    Inconclusive,
};

NEOGRAPH_API std::string_view to_string(SourceLifecycle state) noexcept;

enum class ClaimResolutionKind : std::uint8_t {
    Unresolved,
    Corroborated,
    Contradicted,
    Inconclusive,
    ReconciliationRequired,
    HumanDecisionRequired,
};

NEOGRAPH_API std::string_view to_string(ClaimResolutionKind kind) noexcept;

enum class EvidencePublishResult : std::uint8_t {
    Published,
    Duplicate,
};

/**
 * Immutable version-specific source identity.  A title is metadata only; the
 * canonical locator, version, and content hash identify the actual source.
 */
struct NEOGRAPH_API SourceIdentity {
    std::uint32_t schema_version = 1;
    std::string source_id;
    std::string canonical_locator;
    std::string version;
    std::string content_hash;
    std::vector<std::string> aliases;
    json metadata = json::object();

    bool operator==(const SourceIdentity&) const = default;
};

/**
 * One bounded unit of work.  Primary extraction is deduplicated per
 * (source_id, scope); review/reproduction must explicitly name the work they
 * intentionally replicate so that deliberate independence is never mistaken
 * for accidental duplication.
 */
struct NEOGRAPH_API ResearchTaskSpec {
    std::uint32_t schema_version = 1;
    std::string task_id;
    std::string objective_id;
    std::string source_id;
    std::string claim_id;
    std::string owner_scope;
    std::string scope;
    ResearchTaskKind kind = ResearchTaskKind::PrimaryExtraction;
    std::string replication_of_task_id;
    bool intentional_replication = false;
    std::uint64_t lease_duration_ms = 60'000;
    json requirements = json::object();

    bool operator==(const ResearchTaskSpec&) const = default;
};

/** Durable snapshot of one task's current lease or terminal state. */
struct NEOGRAPH_API ResearchTask {
    ResearchTaskSpec spec;
    ResearchTaskState state = ResearchTaskState::Ready;
    std::uint64_t generation = 0;
    std::string lease_id;
    std::string leased_by;
    std::uint64_t lease_expires_at_unix_ms = 0;
    std::string published_artifact_id;

    bool operator==(const ResearchTask&) const = default;
};

/** Caller-supplied idempotency identity for one work claim. */
struct NEOGRAPH_API ResearchLeaseRequest {
    std::string task_id;
    std::string lease_id;
    std::string worker_id;
    std::string owner_scope;
    std::uint64_t now_unix_ms = 0;

    bool operator==(const ResearchLeaseRequest&) const = default;
};

/** Immutable proof that a worker owns the current task generation. */
struct NEOGRAPH_API ResearchTaskLease {
    std::string task_id;
    std::string lease_id;
    std::string worker_id;
    std::string owner_scope;
    std::uint64_t generation = 0;
    std::uint64_t expires_at_unix_ms = 0;

    bool operator==(const ResearchTaskLease&) const = default;
};

/**
 * Typed material result.  `NoSupport` is first-class evidence: it records the
 * exact scope searched rather than globally suppressing a future review.
 * `source_version` and `source_content_hash` bind an observation to the source
 * version admitted by the task.
 */
struct NEOGRAPH_API EvidenceArtifact {
    std::uint32_t schema_version = 1;
    std::string artifact_id;
    std::string task_id;
    std::string source_id;
    std::string claim_id;
    std::string owner_scope;
    std::string source_version;
    std::string source_content_hash;
    EvidencePolarity polarity = EvidencePolarity::Inconclusive;
    std::string evidence_locator;
    std::string observation;
    std::string searched_scope;
    json conditions = json::object();
    json applicability = json::object();
    json uncertainty = json::object();
    std::vector<std::string> limitations;
    std::vector<std::string> contradictions;
    std::string recommended_next_task;
    double confidence = 0.0;
    std::string worker_id;
    std::string model_id;
    std::string tool_id;
    std::string program_version_id;
    std::string program_run_id;
    std::string a2a_message_id;
    json provenance = json::object();

    bool operator==(const EvidenceArtifact&) const = default;
};

/** Deterministic planner result derived from committed evidence only. */
struct NEOGRAPH_API ClaimResolution {
    std::string claim_id;
    ClaimResolutionKind kind = ClaimResolutionKind::Unresolved;
    std::vector<std::string> supporting_artifact_ids;
    std::vector<std::string> contradicting_artifact_ids;
    std::vector<std::string> inconclusive_artifact_ids;
    std::optional<ResearchTaskKind> recommended_next_task;

    bool operator==(const ClaimResolution&) const = default;
};

/**
 * Durable coordination API.  Implementations must serialize competing lease,
 * publication, and expiry transitions.  The API intentionally contains no
 * scheduler policy: callers choose what to lease; this ledger ensures their
 * evidence and ownership decisions remain recoverable and non-overwriting.
 */
class NEOGRAPH_API EvidenceLedger {
public:
    virtual ~EvidenceLedger() = default;

    virtual void register_source(SourceIdentity source) = 0;
    [[nodiscard]] virtual std::optional<SourceIdentity>
    source(std::string_view source_id) const = 0;

    virtual void create_task(ResearchTaskSpec task) = 0;
    [[nodiscard]] virtual std::optional<ResearchTask>
    task(std::string_view owner_scope, std::string_view task_id) const = 0;

    /** Returns owner-scoped task snapshots in deterministic task-id order. */
    [[nodiscard]] virtual std::vector<ResearchTask>
    tasks(std::string_view owner_scope, bool include_terminal) const = 0;
    /**
     * Atomically claims a ready/expired task.  Returns nullopt when another
     * unexpired worker holds it or it has reached a terminal state.  Repeating
     * the exact active lease request returns the same lease.
     */
    [[nodiscard]] virtual std::optional<ResearchTaskLease>
    acquire_lease(ResearchLeaseRequest request) = 0;

    /** Extends only the exact live lease generation. */
    [[nodiscard]] virtual bool renew_lease(const ResearchTaskLease& lease,
                                           std::uint64_t now_unix_ms) = 0;

    /** Makes this owner's expired leases ready for reassignment. */
    [[nodiscard]] virtual std::vector<std::string>
    expire_leases(std::string_view owner_scope, std::uint64_t now_unix_ms) = 0;

    /**
     * Atomically commits evidence and terminally publishes the claimed task.
     * A retried identical publication is idempotent; a different artifact for
     * an already published task is rejected rather than last-writer-wins.
     */
    [[nodiscard]] virtual EvidencePublishResult
    publish(const ResearchTaskLease& lease, EvidenceArtifact artifact,
            std::uint64_t now_unix_ms) = 0;

    [[nodiscard]] virtual std::vector<EvidenceArtifact>
    artifacts_for_claim(std::string_view owner_scope, std::string_view claim_id) const = 0;
    [[nodiscard]] virtual SourceLifecycle source_lifecycle(std::string_view owner_scope,
                                                            std::string_view source_id,
                                                            std::uint64_t now_unix_ms) const = 0;
    [[nodiscard]] virtual ClaimResolution
    resolve_claim(std::string_view owner_scope, std::string_view claim_id) const = 0;
};

/**
 * Owner-scoped scheduling budget for a durable research board.  Costs and
 * information values come from task.requirements; absent values default to
 * one.  The board never mutates a task's requirements while scheduling.
 */
struct NEOGRAPH_API ResearchTaskBoardBudget {
    std::uint32_t max_active_leases = 1;
    std::uint64_t max_cost_microunits = std::numeric_limits<std::uint64_t>::max();
};

/**
 * Deterministic scheduler layered on EvidenceLedger.
 *
 * Selection is durable because the ledger is the source of task state: the
 * board only ranks owner-scoped snapshots, then claims one through the
 * ledger's atomic lease CAS.  A crash therefore leaves an expiring lease,
 * not an in-memory queue entry.  `information_value / cost` is the primary
 * score, with an explicit bounded priority tie-breaker.
 */
class NEOGRAPH_API ResearchTaskBoard final {
public:
    explicit ResearchTaskBoard(EvidenceLedger& ledger) noexcept : ledger_(ledger) {}

    ResearchTaskBoard(const ResearchTaskBoard&) = delete;
    ResearchTaskBoard& operator=(const ResearchTaskBoard&) = delete;

    void submit(ResearchTaskSpec task) { ledger_.create_task(std::move(task)); }

    [[nodiscard]] std::optional<ResearchTaskLease> acquire_next(
        std::string_view owner_scope, std::string_view worker_id,
        std::string_view board_id, std::uint64_t now_unix_ms,
        ResearchTaskBoardBudget budget = {}) const;

    [[nodiscard]] EvidencePublishResult publish(
        const ResearchTaskLease& lease, EvidenceArtifact artifact,
        std::uint64_t now_unix_ms) const {
        return ledger_.publish(lease, std::move(artifact), now_unix_ms);
    }

    [[nodiscard]] std::vector<ResearchTask>
    snapshot(std::string_view owner_scope) const {
        return ledger_.tasks(owner_scope, true);
    }

private:
    EvidenceLedger& ledger_;
};

} // namespace neograph::research
