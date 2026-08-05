#include <gtest/gtest.h>

#include <neograph/research/sqlite_evidence_ledger.h>

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

using namespace neograph::research;

SourceIdentity make_source(std::string id = "source-1") {
    SourceIdentity source;
    source.source_id = std::move(id);
    source.canonical_locator = "doi:10.1000/example";
    source.version = "2026-08-04";
    source.content_hash = "sha256:source-v1";
    source.aliases = {"https://example.test/paper"};
    source.metadata = {{"title", "Example evidence"}};
    return source;
}

ResearchTaskSpec make_primary_task(std::string id = "extract-1") {
    ResearchTaskSpec task;
    task.task_id = std::move(id);
    task.objective_id = "objective-1";
    task.source_id = "source-1";
    task.claim_id = "claim-1";
    task.owner_scope = "owner-a";
    task.scope = "methods:section-2";
    task.lease_duration_ms = 100;
    task.requirements = {{"extract", "reported measurement"}};
    return task;
}

ResearchTaskSpec make_review_task(std::string id, ResearchTaskKind kind = ResearchTaskKind::IndependentReview) {
    auto task = make_primary_task(std::move(id));
    task.kind = kind;
    task.intentional_replication = true;
    task.replication_of_task_id = "extract-1";
    return task;
}

EvidenceArtifact make_artifact(const ResearchTaskLease& lease,
                               EvidencePolarity polarity = EvidencePolarity::Supports) {
    EvidenceArtifact artifact;
    artifact.artifact_id = "artifact-" + lease.task_id;
    artifact.task_id = lease.task_id;
    artifact.source_id = "source-1";
    artifact.claim_id = "claim-1";
    artifact.owner_scope = lease.owner_scope;
    artifact.source_version = "2026-08-04";
    artifact.source_content_hash = "sha256:source-v1";
    artifact.polarity = polarity;
    artifact.evidence_locator = "page-4:table-2";
    artifact.observation = "Measured result with a bounded confidence interval.";
    artifact.searched_scope = "methods and results, pages 1-9";
    artifact.conditions = {{"dataset", "fixture-v1"}};
    artifact.applicability = {{"population", "fixture"}};
    artifact.uncertainty = {{"sampling", "small-n"}};
    artifact.limitations = {"synthetic fixture"};
    artifact.recommended_next_task = "independent review";
    artifact.confidence = 0.7;
    artifact.worker_id = lease.worker_id;
    artifact.model_id = "model-fixture";
    artifact.tool_id = "extractor-fixture";
    artifact.program_version_id = "program-v1";
    artifact.program_run_id = "run-" + lease.task_id;
    artifact.provenance = {{"worker", lease.worker_id}};
    return artifact;
}

class EvidenceLedgerTest : public ::testing::Test {
protected:
    SqliteEvidenceLedger ledger{":memory:"};

    void SetUp() override { ledger.register_source(make_source()); }

    ResearchTaskLease acquire(std::string task_id, std::string lease_id, std::string worker,
                              std::uint64_t now = 1'000) {
        const auto lease = ledger.acquire_lease(ResearchLeaseRequest{
            std::move(task_id), std::move(lease_id), std::move(worker), "owner-a", now});
        EXPECT_TRUE(lease.has_value());
        return *lease;
    }
};

TEST_F(EvidenceLedgerTest, SourceIdentityIsVersionSpecificAndAliasesDoNotCollapseVersions) {
    const auto by_alias = ledger.source("https://example.test/paper");
    ASSERT_TRUE(by_alias.has_value());
    EXPECT_EQ(by_alias->source_id, "source-1");

    auto conflicting = make_source("source-2");
    conflicting.aliases = {"https://example.test/paper-v2"};
    EXPECT_THROW(ledger.register_source(std::move(conflicting)), std::invalid_argument)
        << "the same locator/version/hash must use one canonical source identity";

    auto distinct_version = make_source("source-2");
    distinct_version.version = "2026-08-05";
    distinct_version.content_hash = "sha256:source-v2";
    distinct_version.aliases = {"https://example.test/paper-v2"};
    EXPECT_NO_THROW(ledger.register_source(std::move(distinct_version)));

    auto source_id_alias_collision = make_source("https://example.test/paper");
    source_id_alias_collision.version = "2026-08-06";
    source_id_alias_collision.content_hash = "sha256:source-v3";
    source_id_alias_collision.aliases = {"https://example.test/paper-v3"};
    EXPECT_THROW(ledger.register_source(std::move(source_id_alias_collision)), std::invalid_argument);

    auto alias_source_id_collision = make_source("source-3");
    alias_source_id_collision.version = "2026-08-07";
    alias_source_id_collision.content_hash = "sha256:source-v4";
    alias_source_id_collision.aliases = {"source-1"};
    EXPECT_THROW(ledger.register_source(std::move(alias_source_id_collision)), std::invalid_argument);
}

TEST_F(EvidenceLedgerTest, DeduplicatesPrimaryExtractionButAdmitsExplicitIndependentReview) {
    ledger.create_task(make_primary_task());
    auto duplicate = make_primary_task("extract-duplicate");
    EXPECT_THROW(ledger.create_task(std::move(duplicate)), std::invalid_argument);

    auto review_not_marked = make_review_task("review-not-marked");
    review_not_marked.intentional_replication = false;
    EXPECT_THROW(ledger.create_task(std::move(review_not_marked)), std::invalid_argument);

    auto review = make_review_task("review-1");
    EXPECT_NO_THROW(ledger.create_task(std::move(review)));
}

TEST_F(EvidenceLedgerTest, ListsOwnerScopedTasksAcrossRestartableStates) {
    ledger.create_task(make_primary_task());
    EXPECT_EQ(ledger.tasks("owner-a", false).size(), 1U);
    const auto lease = acquire("extract-1", "lease-list", "worker-list");
    ASSERT_EQ(ledger.tasks("owner-a", false).size(), 1U);
    ASSERT_EQ(ledger.publish(lease, make_artifact(lease), 1'001),
              EvidencePublishResult::Published);
    EXPECT_TRUE(ledger.tasks("owner-a", false).empty());
    ASSERT_EQ(ledger.tasks("owner-a", true).size(), 1U);
    EXPECT_EQ(ledger.tasks("owner-a", true).front().state, ResearchTaskState::Published);
}

TEST_F(EvidenceLedgerTest, ResearchTaskBoardRanksInformationPerCostAndEnforcesBudget) {
    auto high_cost = make_primary_task("task-high-cost");
    high_cost.scope = "scope-high-cost";
    high_cost.claim_id = "claim-high";
    high_cost.requirements = {{"information_value", 10.0}, {"cost_microunits", 10}};
    ledger.create_task(std::move(high_cost));
    auto high_ratio = make_primary_task("task-high-ratio");
    high_ratio.scope = "scope-high-ratio";
    high_ratio.claim_id = "claim-ratio";
    high_ratio.requirements = {{"information_value", 4.0}, {"cost_microunits", 1}};
    ledger.create_task(std::move(high_ratio));

    ResearchTaskBoard board(ledger);
    const auto first = board.acquire_next("owner-a", "worker-board", "board-v1", 1'000,
                                          ResearchTaskBoardBudget{1, 20});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->task_id, "task-high-ratio");
    ResearchTaskBoard concurrent_board(ledger);
    EXPECT_FALSE(concurrent_board.acquire_next(
        "owner-a", "worker-board-2", "board-v1", 1'001,
        ResearchTaskBoardBudget{1, 20}));
    EXPECT_THROW(
        (void)concurrent_board.acquire_next(
            "owner-a", "worker-board-2", "board-v1", 1'001,
            ResearchTaskBoardBudget{2, 20}),
        std::invalid_argument)
        << "a durable board identity must not silently widen its budget";

    auto first_artifact = make_artifact(*first);
    first_artifact.claim_id = "claim-ratio";
    ASSERT_EQ(board.publish(*first, std::move(first_artifact), 1'002),
              EvidencePublishResult::Published);

    const auto second = board.acquire_next("owner-a", "worker-board", "board-v1", 1'003,
                                           ResearchTaskBoardBudget{1, 20});
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->task_id, "task-high-cost");

}

TEST_F(EvidenceLedgerTest, ExactlyOneWorkerClaimsPrimaryLeaseAndExpiryReassignsIt) {
    ledger.create_task(make_primary_task());
    std::atomic<int> acquired{0};
    std::vector<std::thread> workers;
    workers.reserve(100);
    for (int index = 0; index < 100; ++index) {
        workers.emplace_back([&, index] {
            const auto lease = ledger.acquire_lease(
                ResearchLeaseRequest{"extract-1", "lease-" + std::to_string(index),
                                     "worker-" + std::to_string(index), "owner-a", 1'000});
            if (lease) ++acquired;
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(acquired.load(), 1)
        << "concurrent workers must not receive duplicate primary extraction leases";

    const auto expired = ledger.expire_leases("owner-a", 1'100);
    ASSERT_EQ(expired, std::vector<std::string>{"extract-1"});
    const auto reissued = acquire("extract-1", "lease-reissued", "worker-reissued", 1'101);
    EXPECT_EQ(reissued.generation, 2U);
}
TEST_F(EvidenceLedgerTest, LeaseAndEvidenceVisibilityCannotCrossOwnerBoundaries) {
    ledger.create_task(make_primary_task());

    EXPECT_FALSE(ledger.acquire_lease(
        ResearchLeaseRequest{"extract-1", "lease-other", "worker-other", "owner-b", 1'000}));

    auto other_owner_task = make_primary_task("extract-owner-b");
    other_owner_task.owner_scope = "owner-b";
    ledger.create_task(std::move(other_owner_task));
    const auto other_owner_lease =
        ledger.acquire_lease(ResearchLeaseRequest{"extract-owner-b", "lease-owner-b",
                                                   "worker-owner-b", "owner-b", 1'000});
    ASSERT_TRUE(other_owner_lease.has_value());
    ASSERT_EQ(ledger.publish(*other_owner_lease, make_artifact(*other_owner_lease), 1'001),
              EvidencePublishResult::Published);

    EXPECT_FALSE(ledger.task("owner-a", "extract-owner-b"));
    EXPECT_TRUE(ledger.artifacts_for_claim("owner-a", "claim-1").empty());
    EXPECT_EQ(ledger.artifacts_for_claim("owner-b", "claim-1").size(), 1U);

    const auto lease = acquire("extract-1", "lease-owner", "worker-owner");
    auto forged = lease;
    forged.owner_scope = "owner-b";
    EXPECT_FALSE(ledger.renew_lease(forged, 1'001));
    EXPECT_THROW((void)ledger.publish(forged, make_artifact(forged), 1'001),
                 std::invalid_argument);
}
TEST_F(EvidenceLedgerTest, StaleWorkerCannotPublishAfterLeaseExpiryAndReassignment) {
    ledger.create_task(make_primary_task());
    const auto first = acquire("extract-1", "lease-first", "worker-first");
    ASSERT_EQ(ledger.expire_leases("owner-a", 1'100), std::vector<std::string>{"extract-1"});
    const auto second = acquire("extract-1", "lease-second", "worker-second", 1'101);

    EXPECT_THROW((void)ledger.publish(first, make_artifact(first), 1'102), std::logic_error);
    EXPECT_EQ(ledger.publish(second, make_artifact(second), 1'102),
              EvidencePublishResult::Published);
    EXPECT_EQ(ledger.publish(second, make_artifact(second), 1'103),
              EvidencePublishResult::Duplicate);
}
TEST_F(EvidenceLedgerTest, LeaseRenewalRejectsStaleGenerationAndExactExpiry) {
    ledger.create_task(make_primary_task());
    const auto first = acquire("extract-1", "lease-renew", "worker-renew");

    auto stale_generation = first;
    stale_generation.generation = 0;
    EXPECT_FALSE(ledger.renew_lease(stale_generation, 1'001));
    EXPECT_TRUE(ledger.renew_lease(first, 1'050));
    EXPECT_FALSE(ledger.renew_lease(first, 1'150))
        << "a lease is not live at its exact expiry boundary";

    const auto expired = ledger.expire_leases("owner-a", 1'150);
    ASSERT_EQ(expired, std::vector<std::string>{"extract-1"});
    const auto reassigned = acquire("extract-1", "lease-renew-reassigned",
                                    "worker-renew-reassigned", 1'151);
    EXPECT_FALSE(ledger.renew_lease(first, 1'152));
    EXPECT_TRUE(ledger.renew_lease(reassigned, 1'152));
}

TEST_F(EvidenceLedgerTest, RejectsEvidenceBoundToWrongSourceContentHash) {
    ledger.create_task(make_primary_task());
    const auto lease = acquire("extract-1", "lease-forged-hash", "worker-hash");
    auto forged = make_artifact(lease);
    forged.source_content_hash = "sha256:not-the-admitted-source";

    EXPECT_THROW((void)ledger.publish(lease, std::move(forged), 1'010),
                 std::invalid_argument);
    const auto task = ledger.task("owner-a", "extract-1");
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->state, ResearchTaskState::Leased);
}

TEST_F(EvidenceLedgerTest, NegativeEvidenceRetainsScopeAndRequestsIndependentReview) {
    ledger.create_task(make_primary_task());
    const auto lease = acquire("extract-1", "lease-negative", "worker-negative");
    auto negative = make_artifact(lease, EvidencePolarity::NoSupport);
    negative.observation = "No support found in the searched source version.";
    negative.searched_scope = "all methods, results, and appendices of source version 2026-08-04";
    ASSERT_EQ(ledger.publish(lease, std::move(negative), 1'010), EvidencePublishResult::Published);

    const auto artifacts = ledger.artifacts_for_claim("owner-a", "claim-1");
    ASSERT_EQ(artifacts.size(), 1U);
    EXPECT_EQ(artifacts.front().polarity, EvidencePolarity::NoSupport);
    EXPECT_EQ(artifacts.front().searched_scope,
              "all methods, results, and appendices of source version 2026-08-04");

    const auto resolution = ledger.resolve_claim("owner-a", "claim-1");
    EXPECT_EQ(resolution.kind, ClaimResolutionKind::Inconclusive);
    ASSERT_TRUE(resolution.recommended_next_task.has_value());
    EXPECT_EQ(*resolution.recommended_next_task, ResearchTaskKind::IndependentReview);
}

TEST_F(EvidenceLedgerTest, ConflictingClaimsCoexistAndSelectReconciliation) {
    ledger.create_task(make_primary_task());
    const auto primary = acquire("extract-1", "lease-primary", "worker-primary");
    ASSERT_EQ(ledger.publish(primary, make_artifact(primary, EvidencePolarity::Supports), 1'010),
              EvidencePublishResult::Published);

    ledger.create_task(make_review_task("review-1"));
    const auto review = acquire("review-1", "lease-review", "worker-review", 1'020);
    ASSERT_EQ(ledger.publish(review, make_artifact(review, EvidencePolarity::Contradicts), 1'030),
              EvidencePublishResult::Published);

    EXPECT_EQ(ledger.source_lifecycle("owner-a", "source-1", 1'040),
              SourceLifecycle::Contradicted);
    const auto resolution = ledger.resolve_claim("owner-a", "claim-1");
    EXPECT_EQ(resolution.kind, ClaimResolutionKind::ReconciliationRequired);
    ASSERT_TRUE(resolution.recommended_next_task.has_value());
    EXPECT_EQ(*resolution.recommended_next_task, ResearchTaskKind::Reconciliation);
    EXPECT_EQ(resolution.supporting_artifact_ids.size(), 1U);
    EXPECT_EQ(resolution.contradicting_artifact_ids.size(), 1U);
}

TEST(EvidenceLedgerPersistence, ReopensTasksAndCommittedEvidence) {
    const auto path = std::filesystem::temp_directory_path()
                    / ("neograph-evidence-ledger-" + std::to_string(::getpid()) + ".sqlite");
    std::filesystem::remove(path);
    {
        SqliteEvidenceLedger ledger(path.string());
        ledger.register_source(make_source());
        ledger.create_task(make_primary_task());
        const auto lease = ledger.acquire_lease(
            ResearchLeaseRequest{"extract-1", "lease-persist", "worker-persist", "owner-a", 2'000});
        ASSERT_TRUE(lease.has_value());
        ASSERT_EQ(ledger.publish(*lease, make_artifact(*lease), 2'001), EvidencePublishResult::Published);
    }
    {
        SqliteEvidenceLedger reopened(path.string());
        const auto task = reopened.task("owner-a", "extract-1");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->state, ResearchTaskState::Published);
        EXPECT_EQ(task->published_artifact_id, "artifact-extract-1");
        const auto artifacts = reopened.artifacts_for_claim("owner-a", "claim-1");
        ASSERT_EQ(artifacts.size(), 1U);
        EXPECT_EQ(artifacts.front().program_run_id, "run-extract-1");
    }
    std::filesystem::remove(path);
}

TEST(EvidenceLedgerPersistence, DurableBoardBudgetSurvivesRestart) {
    const auto path = std::filesystem::temp_directory_path()
                    / ("neograph-evidence-board-" + std::to_string(::getpid()) + ".sqlite");
    std::filesystem::remove(path);
    {
        SqliteEvidenceLedger ledger(path.string());
        ledger.register_source(make_source());

        auto first = make_primary_task("board-task-1");
        first.scope = "board-scope-1";
        first.claim_id = "board-claim-1";
        first.requirements = {{"information_value", 3.0}, {"cost_microunits", 3}};
        ledger.create_task(std::move(first));

        auto second = make_primary_task("board-task-2");
        second.scope = "board-scope-2";
        second.claim_id = "board-claim-2";
        second.requirements = {{"information_value", 3.0}, {"cost_microunits", 3}};
        ledger.create_task(std::move(second));

        ResearchTaskBoard board(ledger);
        const auto lease = board.acquire_next(
            "owner-a", "worker-board", "durable-board", 2'000,
            ResearchTaskBoardBudget{1, 3});
        ASSERT_TRUE(lease.has_value());
        auto artifact = make_artifact(*lease);
        artifact.claim_id = "board-claim-1";
        ASSERT_EQ(board.publish(*lease, std::move(artifact), 2'001),
                  EvidencePublishResult::Published);
    }
    {
        SqliteEvidenceLedger reopened(path.string());
        ResearchTaskBoard board(reopened);
        EXPECT_FALSE(board.acquire_next(
            "owner-a", "worker-after-restart", "durable-board", 2'002,
            ResearchTaskBoardBudget{1, 3}));
        const auto task = reopened.task("owner-a", "board-task-1");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->board_id, "durable-board");
    }
    std::filesystem::remove(path);
}

} // namespace
