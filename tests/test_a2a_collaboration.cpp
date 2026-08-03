#include <neograph/a2a/collaboration.h>

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

}  // namespace
