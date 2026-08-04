#include <neograph/harness/contract.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::harness;

ManifestSpec make_spec() {
    ManifestSpec spec;
    spec.manifest_id = "harness-contract";
    spec.owner_scope = "owner-a";
    spec.scope = "program-change";
    spec.assumptions = {"Program is the sole executor"};
    spec.requirements = {{"compile", "compile the program"}};
    spec.non_goals = {"generic-bytecode-vm"};
    spec.acceptance = {{"accept-compile", "independent compile succeeds", true,
                        json{{"status", "ok"}}}};
    spec.fixed_test_vectors = {{"vector-1", json{{"input", 1}}, json{{"status", "ok"}}}};
    spec.independent_oracles = {"oracle-1"};
    spec.risk_register = {{"risk-1", "worker may claim success", "require independent evidence", true}};
    spec.retry_policy = {2, 10, 1000};
    return spec;
}

Evidence make_evidence(const Manifest& manifest,
                       std::string id,
                       EvidenceKind kind,
                       bool passed) {
    Evidence evidence;
    evidence.evidence_id = std::move(id);
    evidence.acceptance_id = kind == EvidenceKind::IndependentOracle ? "" : "accept-compile";
    evidence.kind = kind;
    evidence.manifest_hash = manifest.content_hash();
    evidence.program_version_id = "program-v1";
    evidence.workspace_revision = "workspace-v1";
    evidence.command = "ctest --test-dir build";
    evidence.toolchain = "gcc-13";
    evidence.artifact_hash = "sha256:artifact";
    evidence.executed = true;
    evidence.passed = passed;
    if (kind == EvidenceKind::IndependentOracle)
        evidence.details = json{{"oracle_id", "oracle-1"}};
    return evidence;
}

Manifest frozen_manifest() {
    return ContractBoundary::freeze(
        ContractBoundary::review(ContractBoundary::propose(make_spec()),
                                 ManifestReview{"reviewer-a", "reviewed", true}));
}

TEST(HarnessContractTest, RejectsExecutionUntilFrozen) {
    const auto proposed = ContractBoundary::propose(make_spec());
    EXPECT_FALSE(ContractBoundary::worker_selection_allowed(proposed));
    EXPECT_THROW(ContractBoundary::start_run(proposed), std::invalid_argument);
    EXPECT_THROW(ContractBoundary::freeze(proposed), std::logic_error);

    const auto reviewed = ContractBoundary::review(
        proposed, ManifestReview{"reviewer-a", "reviewed", true});
    EXPECT_FALSE(ContractBoundary::worker_selection_allowed(reviewed));
    EXPECT_THROW(ContractBoundary::start_run(reviewed), std::invalid_argument);
}

TEST(HarnessContractTest, FrozenManifestIsAnImmutableCanonicalSnapshot) {
    auto spec = make_spec();
    const auto proposed = ContractBoundary::propose(spec);
    spec.scope = "mutated-after-propose";
    const auto frozen = ContractBoundary::freeze(
        ContractBoundary::review(proposed, ManifestReview{"reviewer-a", "reviewed", true}));

    EXPECT_TRUE(ContractBoundary::worker_selection_allowed(frozen));
    EXPECT_EQ(frozen.spec().scope, "program-change");
    EXPECT_THROW(frozen.review(ManifestReview{"reviewer-b", "again", true}), std::logic_error);
    EXPECT_THROW(frozen.freeze(), std::logic_error);
    EXPECT_EQ(Manifest::parse(frozen.serialize_canonical()).serialize_canonical(),
              frozen.serialize_canonical());
}

TEST(HarnessContractTest, MissingEvidenceCannotPublish) {
    auto run = ContractBoundary::start_run(frozen_manifest());
    run.begin_attempt();
    run.record_worker_report("worker claims success", true);
    const auto result = ContractBoundary::verify(run, "program-v1", "workspace-v1");

    EXPECT_FALSE(result.publishable);
    EXPECT_EQ(result.missing_acceptance_ids, std::vector<std::string>({"accept-compile"}));
    EXPECT_THROW(ContractBoundary::publish(run), std::logic_error);
}

TEST(HarnessContractTest, FailedOracleBlocksPublication) {
    const auto manifest = frozen_manifest();
    auto run = ContractBoundary::start_run(manifest);
    run.begin_attempt();
    ContractBoundary::record_evidence(
        run, make_evidence(manifest, "compile", EvidenceKind::DeterministicRun, true));
    ContractBoundary::record_evidence(
        run, make_evidence(manifest, "oracle", EvidenceKind::IndependentOracle, false));

    const auto result = ContractBoundary::verify(run, "program-v1", "workspace-v1");
    EXPECT_FALSE(result.publishable);
    EXPECT_EQ(result.status, RunStatus::Failed);
    EXPECT_EQ(result.failed_evidence_ids, std::vector<std::string>({"oracle"}));
    EXPECT_THROW(ContractBoundary::publish(run), std::logic_error);
}

TEST(HarnessContractTest, IndependentlyVerifiedEvidencePublishes) {
    const auto manifest = frozen_manifest();
    auto run = ContractBoundary::start_run(manifest);
    run.begin_attempt();
    ContractBoundary::record_evidence(
        run, make_evidence(manifest, "compile", EvidenceKind::DeterministicRun, true));
    ContractBoundary::record_evidence(
        run, make_evidence(manifest, "oracle", EvidenceKind::IndependentOracle, true));

    const auto result = ContractBoundary::verify(run, "program-v1", "workspace-v1");
    ASSERT_TRUE(result.publishable);
    ContractBoundary::publish(run);
    EXPECT_EQ(run.status(), RunStatus::Published);
}

}  // namespace
