#include <neograph/program/contract.h>

#include <gtest/gtest.h>

#include <string>

namespace {

using neograph::json;
using namespace neograph::program;

ContractManifestSpec make_spec() {
    ContractManifestSpec spec;
    spec.manifest_id = "contract-demo";
    spec.owner_scope = "tenant-demo";
    spec.scope = "program-runtime";
    spec.assumptions = {"core-is-the-only-executor"};
    spec.requirements = {{"compile", "Compile the declared program"},
                         {"execute", "Execute the admitted program"}};
    spec.non_goals = {"generic-bytecode-vm"};
    spec.acceptance = {{"compile-ok", "Compiler accepts the source", true, json{{"status", "ok"}}},
                       {"execute-ok", "Independent run returns the expected value", true,
                        json{{"value", 42}}}};
    spec.fixed_test_vectors = {{"happy-path", json{{"input", 40}}, json{{"value", 42}}}};
    spec.independent_oracles = {"oracle-smoke"};
    spec.risk_register = {{"side-effect", "Core can publish an external effect", "Use the effect outbox", true}};
    spec.permissions = {"program-read"};
    spec.retry_policy = ContractRetryPolicy{2, 100, 1000};
    return spec;
}

ContractEvidence make_evidence(const ContractManifest& manifest,
                               std::string id,
                               std::string acceptance_id,
                               ContractEvidenceKind kind,
                               bool passed) {
    ContractEvidence evidence;
    evidence.evidence_id = std::move(id);
    evidence.acceptance_id = std::move(acceptance_id);
    evidence.kind = kind;
    evidence.manifest_hash = manifest.content_hash();
    evidence.program_version_id = "program-v1";
    evidence.run_id             = "run-1";
    evidence.workspace_revision = "workspace-1";
    evidence.command            = "ctest --test-dir build";
    evidence.toolchain = "gcc";
    evidence.artifact_hash = "artifact-1";
    evidence.executed = true;
    evidence.passed = passed;
    return evidence;
}

TEST(ProgramContractTest, LifecycleIsImmutableAndCanonical) {
    const auto proposed = ContractManifest::propose(make_spec());
    EXPECT_EQ(proposed.lifecycle(), ContractManifestLifecycle::Proposed);
    EXPECT_FALSE(proposed.review_record());

    const auto reviewed = proposed.review(ContractReview{"reviewer", "scope and gates checked", true});
    EXPECT_EQ(reviewed.lifecycle(), ContractManifestLifecycle::Reviewed);
    EXPECT_NE(reviewed.content_hash(), proposed.content_hash());

    const auto frozen = reviewed.freeze();
    EXPECT_EQ(frozen.lifecycle(), ContractManifestLifecycle::Frozen);
    EXPECT_EQ(ContractManifest::parse(frozen.serialize_canonical()).content_hash(),
              frozen.content_hash());
    EXPECT_EQ(ContractManifest::parse(frozen.serialize_canonical()).serialize_canonical(),
              frozen.serialize_canonical());
    auto tampered = frozen.serialize_canonical();
    const auto hash_position = tampered.find(frozen.content_hash());
    ASSERT_NE(hash_position, std::string::npos);
    tampered[hash_position] = tampered[hash_position] == '0' ? '1' : '0';
    EXPECT_THROW(ContractManifest::parse(tampered), std::invalid_argument);
    EXPECT_THROW(proposed.freeze(), std::logic_error);
}

TEST(ProgramContractTest, WorkerClaimCannotPublishWithoutIndependentEvidence) {
    const auto frozen = ContractManifest::propose(make_spec())
                             .review(ContractReview{"reviewer", "approved", true})
                             .freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.record_worker_report("implemented everything", true);

    const auto blocked = run.verify("program-v1", "run-1", "workspace-1");
    EXPECT_FALSE(blocked.publishable);
    EXPECT_EQ(blocked.missing_acceptance_ids.size(), 2U);
    EXPECT_EQ(run.status(), ContractRunStatus::Blocked);
    EXPECT_THROW(run.publish(), std::logic_error);
}

TEST(ProgramContractTest, IndependentEvidenceClosesGatesAndSurvivesRecovery) {
    const auto frozen = ContractManifest::propose(make_spec())
                             .review(ContractReview{"reviewer", "approved", true})
                             .freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.record_worker_report("candidate result", true);
    run.verify("program-v1", "run-1", "workspace-1");

    run.record_evidence(make_evidence(frozen, "compile-evidence", "compile-ok",
                                      ContractEvidenceKind::DeterministicRun, true));
    run.record_evidence(make_evidence(frozen, "execute-evidence", "execute-ok",
                                      ContractEvidenceKind::DeterministicRun, true));
    auto oracle = make_evidence(frozen, "oracle-evidence", "", ContractEvidenceKind::IndependentOracle,
                                true);
    oracle.details = json{{"oracle_id", "oracle-smoke"}, {"observed", 42}};
    run.record_evidence(std::move(oracle));

    const auto verified = run.verify("program-v1", "run-1", "workspace-1");
    EXPECT_TRUE(verified.publishable);
    EXPECT_EQ(run.status(), ContractRunStatus::Verified);
    run.publish();
    EXPECT_EQ(run.status(), ContractRunStatus::Published);

    const auto recovered = ContractRun::parse(run.serialize_canonical());
    EXPECT_EQ(recovered.status(), ContractRunStatus::Published);
    EXPECT_EQ(recovered.serialize_canonical(), run.serialize_canonical());
}

TEST(ProgramContractTest, FailedEvidenceCannotBeHiddenByWorkerSuccess) {
    const auto frozen = ContractManifest::propose(make_spec())
                             .review(ContractReview{"reviewer", "approved", true})
                             .freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.record_worker_report("passed", true);
    run.record_evidence(make_evidence(frozen, "compile-evidence", "compile-ok",
                                      ContractEvidenceKind::DeterministicRun, false));
    const auto result = run.verify("program-v1", "run-1", "workspace-1");
    EXPECT_FALSE(result.publishable);
    EXPECT_EQ(result.status, ContractRunStatus::Failed);
    EXPECT_EQ(result.failed_evidence_ids, std::vector<std::string>({"compile-evidence"}));
}

TEST(ProgramContractTest, RejectsUnfrozenManifestAndAnyFailedGate) {
    const auto proposed = ContractManifest::propose(make_spec());
    EXPECT_THROW((void)ContractRun(proposed), std::invalid_argument);

    const auto frozen = proposed.review(ContractReview{"reviewer", "approved", true}).freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.record_worker_report("claims success", true);
    run.record_evidence(make_evidence(frozen, "compile-failed", "compile-ok",
                                      ContractEvidenceKind::DeterministicRun, false));
    run.record_evidence(make_evidence(frozen, "compile-passed", "compile-ok",
                                      ContractEvidenceKind::DeterministicRun, true));

    const auto result = run.verify("program-v1", "run-1", "workspace-1");
    EXPECT_FALSE(result.publishable);
    EXPECT_EQ(result.status, ContractRunStatus::Failed);
    EXPECT_EQ(result.failed_evidence_ids,
              std::vector<std::string>({"compile-failed"}));
    EXPECT_THROW(run.publish(), std::logic_error);
}

TEST(ProgramContractTest, RetryPolicyBoundsAttempts) {
    auto spec = make_spec();
    spec.retry_policy.max_attempts = 1;
    const auto frozen = ContractManifest::propose(std::move(spec))
                             .review(ContractReview{"reviewer", "approved", true})
                             .freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.verify("program-v1", "run-1", "workspace-1");
    EXPECT_THROW(run.begin_attempt(), std::runtime_error);
    EXPECT_EQ(run.attempt(), 1U);
}

TEST(ProgramContractTest, RejectsLegacyRunStorageSchemaWithoutRunLineage) {
    const auto frozen = ContractManifest::propose(make_spec())
                             .review(ContractReview{"reviewer", "approved", true})
                             .freeze();
    ContractRun run(frozen);
    run.begin_attempt();
    run.record_worker_report("worker claim", true);

    auto stored = run.serialize_canonical();
    const std::string marker = "\"storage_schema_version\":2";
    const auto marker_position = stored.find(marker);
    ASSERT_NE(marker_position, std::string::npos);
    stored.replace(marker_position, marker.size(), "\"storage_schema_version\":1");
    EXPECT_THROW(ContractRun::parse(stored), std::invalid_argument);
}

}  // namespace
