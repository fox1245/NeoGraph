#include <neograph/a2a/collaboration.h>

#ifdef NEOGRAPH_A2A_PROGRAM
#include <neograph/program/program.h>
#endif

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace neograph::a2a;
using neograph::json;

CollaborationLink make_link() {
    CollaborationLinkSpec spec;
    spec.link_id = "link-1";
    spec.sender_owner_scope = "owner-a";
    spec.receiver_owner_scope = "owner-b";
    spec.sender_agent_id = "coordinator-a";
    spec.receiver_agent_id = "executor-b";
    spec.task_scope = "bounded-coding-task";
    spec.capability_allowlist = {"code-read", "code-write"};
    spec.effect_allowlist = {"artifact-publish"};
    spec.artifact_allowlist = {"artifact-1"};
    spec.cancellation_rights = {"sender", "receiver"};
    spec.expires_at_unix_ms = 4102444800000ULL;
    spec.max_retries = 2;
    spec.acknowledgement_timeout_ms = 1000;
    return CollaborationLink::create(std::move(spec));
}

CollaborationEnvelope make_envelope(const CollaborationLink& link,
                                     std::uint64_t sequence = 1,
                                     std::string kind = "progress",
                                     std::string idempotency_key = "idem-1") {
    return CollaborationEnvelope::create(link,
                                          "run-a",
                                          "run-b",
                                          "task-1",
                                          "context-1",
                                          "message-" + std::to_string(sequence),
                                          "correlation-1",
                                          sequence,
                                          std::move(kind),
                                          std::move(idempotency_key),
                                          json{{"percent", sequence}},
                                          {CollaborationArtifactReference{"artifact-1",
                                                                          "artifact://one",
                                                                          "application/json",
                                                                          42}});
}

#ifdef NEOGRAPH_A2A_PROGRAM
std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

neograph::program::ProgramVersion make_program_version() {
    using namespace neograph::program;
    RegistrySnapshotBuilder registry_builder;
    registry_builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "a2a-reducer", "1.0.0", digest('1')},
                           EffectMode::Brokered, "attestation:a2a", {}, {}},
        [](const json&, const json& incoming) { return json(incoming); });
    auto registry = std::move(registry_builder).build();

    AdmissionProfileBuilder admission_builder;
    admission_builder.id("a2a-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    auto admission = std::move(admission_builder).build();

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("a2a-policy")
        .semantic_version("1.0.0")
        .owner_scope("owner-b")
        .admission_profile(admission)
        .budget_ceiling(BudgetLimits{1000, 1000, 1000, 1, 1, 20, 1, 1, 1});
    auto policy = std::move(policy_builder).build();
    return ProgramVersion(ProgramVersionData{
        digest('2'), admission, policy, {}, "owner-b",
        CoreMaterializationReceipt{"a2a-tests", registry.fingerprint(),
                                   {CorePlanIdentity{"main", digest('3')}}, {}}});
}
#endif

TEST(A2ACollaboration, ConsentFreezesOwnerAndCapabilityBoundary) {
    const auto proposed = make_link();
    EXPECT_EQ(proposed.state(), CollaborationLinkState::Proposed);
    EXPECT_THROW(CollaborationEnvelope::create(proposed, "a", "b", "t", "c", "m", "x", 1,
                                               "progress", "i"), std::invalid_argument);

    const auto accepted = proposed.accept("executor-b", "consent-secret");
    EXPECT_EQ(accepted.state(), CollaborationLinkState::Accepted);
    EXPECT_TRUE(accepted.permits_capability("code-read"));
    EXPECT_TRUE(accepted.permits_effect("artifact-publish"));
    EXPECT_TRUE(accepted.permits_artifact("artifact-1"));
    EXPECT_FALSE(accepted.permits_capability("host-shell"));
    EXPECT_EQ(CollaborationLink::parse(accepted.serialize_canonical()).content_hash(),
              accepted.content_hash());
    EXPECT_THROW(proposed.accept("wrong-agent", "consent-secret"), std::invalid_argument);
    EXPECT_THROW(proposed.accept("executor-b", ""), std::invalid_argument);
}

TEST(A2ACollaboration, EnvelopeRoundTripsThroughA2AMessage) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    const auto envelope = make_envelope(accepted);
    const auto decoded = collaboration_from_message(collaboration_to_message(envelope));
    EXPECT_EQ(decoded.serialize_canonical(), envelope.serialize_canonical());
    EXPECT_EQ(decoded.content_hash(), envelope.content_hash());

    auto unknown = make_envelope(accepted, 1, "vendor-terminal-unknown", "idem-unknown");
    EXPECT_FALSE(unknown.is_terminal());
    EXPECT_EQ(CollaborationEnvelope::parse(unknown.serialize_canonical()).kind,
              "vendor-terminal-unknown");
    EXPECT_THROW(CollaborationEnvelope::create(
                     accepted, "run-a", "run-b", "task-1", "context-1", "message-2", "correlation-1",
                     2, "progress", "idem-2", json{{"ok", true}},
                     {CollaborationArtifactReference{"unlisted", "artifact://x", "text/plain", 1}}),
                 std::invalid_argument);
}

TEST(A2ACollaboration, MailboxIsOwnerScopedAndIdempotent) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    const auto first = make_envelope(accepted);
    EXPECT_EQ(receiver.submit(first), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(first), CollaborationSubmitResult::Duplicate);

    auto conflict = first;
    conflict.kind = "correction";
    EXPECT_EQ(receiver.submit(conflict), CollaborationSubmitResult::Conflict);

    auto second = make_envelope(accepted, 2, "artifact", "idem-2");
    EXPECT_EQ(receiver.submit(second), CollaborationSubmitResult::Accepted);
    EXPECT_TRUE(receiver.acknowledge("idem-1"));
    ASSERT_TRUE(receiver.get("idem-1").has_value());
    EXPECT_EQ(receiver.get("idem-1")->state, CollaborationRecordState::Acknowledged);
    EXPECT_TRUE(receiver.cancel("link-1", "correlation-1", "coordinator-a"));
    EXPECT_EQ(receiver.get("idem-2")->state, CollaborationRecordState::Canceled);

    CollaborationMailbox wrong_owner("owner-a", "coordinator-a");
    EXPECT_EQ(wrong_owner.submit(first), CollaborationSubmitResult::Rejected);
    EXPECT_FALSE(wrong_owner.get("idem-1").has_value());
}

TEST(A2ACollaboration, MailboxJournalReopensWithoutLosingCorrelation) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    EXPECT_EQ(receiver.submit(make_envelope(accepted)), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Accepted);
    auto reopened = CollaborationMailbox::parse(receiver.serialize_canonical());
    ASSERT_EQ(reopened.snapshot().size(), 2U);
    EXPECT_EQ(reopened.snapshot()[0].envelope.sequence, 1U);
    EXPECT_EQ(reopened.snapshot()[1].envelope.sequence, 2U);
    EXPECT_EQ(reopened.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Duplicate);
    EXPECT_EQ(reopened.submit(make_envelope(accepted, 3, "correction", "idem-3")),
              CollaborationSubmitResult::Accepted);
}

TEST(A2ACollaboration, MailboxJournalRejectsIdentityTamperingOnReopen) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");
    EXPECT_EQ(receiver.submit(make_envelope(accepted)), CollaborationSubmitResult::Accepted);
    EXPECT_EQ(receiver.submit(make_envelope(accepted, 2, "retry", "idem-2")),
              CollaborationSubmitResult::Accepted);

    const auto stored = json::parse(receiver.serialize_canonical());

    auto duplicate_link = stored;
    duplicate_link["links"].push_back(duplicate_link["links"][0]);
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_link.dump()), std::invalid_argument);

    auto duplicate_record = stored;
    duplicate_record["records"].push_back(duplicate_record["records"][0]);
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_record.dump()), std::invalid_argument);

    auto unknown_link = stored;
    unknown_link["records"][0]["envelope"]["link_id"] = "link-missing";
    EXPECT_THROW(CollaborationMailbox::parse(unknown_link.dump()), std::invalid_argument);

    auto foreign_sender = stored;
    foreign_sender["records"][0]["envelope"]["sender_owner_scope"] = "owner-forged";
    EXPECT_THROW(CollaborationMailbox::parse(foreign_sender.dump()), std::invalid_argument);

    auto duplicate_sequence = stored;
    auto duplicate = duplicate_sequence["records"][0];
    duplicate["envelope"]["idempotency_key"] = "idem-forged";
    duplicate["envelope"]["message_id"] = "message-forged";
    duplicate_sequence["records"].push_back(std::move(duplicate));
    EXPECT_THROW(CollaborationMailbox::parse(duplicate_sequence.dump()), std::invalid_argument);

    auto sequence_gap = stored;
    auto records = json::array();
    records.push_back(sequence_gap["records"][1]);
    sequence_gap["records"] = std::move(records);
    EXPECT_THROW(CollaborationMailbox::parse(sequence_gap.dump()), std::invalid_argument);
}

#ifdef NEOGRAPH_A2A_PROGRAM
TEST(A2ACollaboration, MailboxRetainsTypedProgramRequestAcrossReconnect) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    auto version = make_program_version();
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");

    auto envelope = CollaborationEnvelope::bind_program(make_envelope(accepted), version);
    neograph::program::ProgramInvocation invocation;
    invocation.input = json{{"prompt", "typed"}};
    invocation.budget = neograph::program::RunBudget{1000, 1000, 1000, 1, 1, 20, 0, 0, 0};
    invocation.trace_id = "a2a:run-b";
    invocation.requested_run_id = "run-b";

    EXPECT_EQ(receiver.submit_program(envelope, version, invocation),
              CollaborationSubmitResult::Accepted);
    EXPECT_TRUE(receiver.permits_artifact("link-1", "artifact-1"));
    EXPECT_FALSE(receiver.permits_artifact("link-1", "unlisted"));
    auto reopened = CollaborationMailbox::parse(receiver.serialize_canonical());
    const auto request = reopened.get_program_request("idem-1");
    ASSERT_TRUE(request.has_value());
    ASSERT_TRUE(request->version);
    ASSERT_TRUE(request->invocation);
    EXPECT_EQ(request->version->id(), version.id());
    EXPECT_EQ(request->invocation->requested_run_id, "run-b");
    EXPECT_EQ(request->invocation->input, invocation.input);
    EXPECT_EQ(reopened.submit(envelope, version, invocation),
              CollaborationSubmitResult::Duplicate);
}

TEST(A2ACollaboration, MailboxJournalRejectsLostTypedProgramRequestOnReopen) {
    const auto accepted = make_link().accept("executor-b", "consent-secret");
    auto version = make_program_version();
    CollaborationMailbox receiver("owner-b", "executor-b");
    receiver.accept_link(accepted, "consent-secret");

    auto envelope = CollaborationEnvelope::bind_program(make_envelope(accepted), version);
    neograph::program::ProgramInvocation invocation;
    invocation.input = json{{"prompt", "typed"}};
    invocation.budget = neograph::program::RunBudget{1000, 1000, 1000, 1, 1, 20, 0, 0, 0};
    invocation.trace_id = "a2a:run-b";
    invocation.requested_run_id = "run-b";
    EXPECT_EQ(receiver.submit_program(envelope, version, invocation),
              CollaborationSubmitResult::Accepted);

    auto stored = json::parse(receiver.serialize_canonical());
    stored["records"][0]["program_request"] = nullptr;
    EXPECT_THROW(CollaborationMailbox::parse(stored.dump()), std::invalid_argument);
}
#endif

}  // namespace
