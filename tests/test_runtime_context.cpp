#include <neograph/runtime_context.h>
#include "fixtures/typed_provider.h"
#include <neograph/provider_outcome_codec.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>

using namespace neograph;

namespace {

std::string sha(char digit) {
    return "sha256:" + std::string(64, digit);
}

RuntimeHistoryRecord first_history_record() {
    RuntimeHistoryRecordData data;
    data.feed_id = "hist_test";
    data.sequence = 1;
    data.message_id = "msg_1";
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = test::message("ship the change", sp::Role::User);
    data.source_media_type = "application/vnd.neocode.message+json";
    data.source_payload = json{{"role", "user"}, {"parts", json::array({"ship the change"})}};
    return RuntimeHistoryRecord::create(std::move(data));
}

ContextArtifact required_skill() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::RequiredSkill;
    data.producer_id = "skill-resolver.v1";
    data.source_digest = sha('a');
    data.media_type = "text/markdown";
    data.placement = ContextPlacement::BeforeLatestUser;
    data.priority = 100;
    data.required = true;
    data.content = json{{"name", "secure-edit"}, {"text", "Verify before editing."}};
    return ContextArtifact::create(std::move(data));
}

ContextArtifact hard_constraint() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::HardConstraint;
    data.producer_id = "host-policy.v1";
    data.source_digest = sha('b');
    data.media_type = "text/markdown";
    data.required = true;
    data.content = "Never skip verification.";
    return ContextArtifact::create(std::move(data));
}

}  // namespace

TEST(RuntimeContext, RawHistoryRoundTripsLosslesslyAndDeepCopiesSourcePayload) {
    json source{{"role", "user"}, {"metadata", json{{"unicode", "context"}}}};
    RuntimeHistoryRecordData data;
    data.feed_id = "hist_roundtrip";
    data.sequence = 1;
    data.message_id = "msg_user";
    data.message = test::message("hello", sp::Role::User);
    data.source_payload = source;

    const auto record = RuntimeHistoryRecord::create(std::move(data));
    source["role"] = "tampered";
    auto returned = *record.source_payload();
    returned["role"] = "also-tampered";

    const auto parsed = RuntimeHistoryRecord::parse(record.serialize_canonical());
    EXPECT_EQ(parsed.id(), record.id());
    EXPECT_EQ(parsed.serialize_canonical(), record.serialize_canonical());
    EXPECT_EQ(parsed.source_payload()->at("role").get<std::string>(), "user");
    EXPECT_EQ(std::get<sp::Text>(parsed.message().parts.at(0)).value, "hello");
}

TEST(RuntimeContext, RawHistoryRetainsMediaInvalidCallsAndWireMetadataInOrder) {
    RuntimeHistoryRecordData data;
    data.feed_id = "hist_ordered";
    data.sequence = 1;
    data.message_id = "msg_ordered";
    data.trust = RuntimeTrustClass::ModelOutput;
    data.message.id = "provider-message";
    data.message.role = sp::Role::Assistant;
    const auto image_bytes = std::make_shared<const std::string>("iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB");
    data.message.parts = {
        sp::Text{"before"},
        sp::Image{"image/png", image_bytes, sp::ImageDetail::High},
        sp::InvalidToolCall{"call-bad", "read", sp::ToolCallKind::ClientExecuted,
                            R"({"path":)", sp::InvalidReason::Truncated,
                            "function", test::document(R"({"vendor":{"position":3}})")},
        sp::ServerToolResult{"server-call", "web_search_tool_result",
                             test::document(R"([{"url":"https://fixture.invalid","encrypted":"opaque"}])")},
        sp::Refusal{"denied", "policy"},
        sp::Text{"after"}};
    data.message.wire_output = test::document(R"([{"type":"unknown","nested":{"value":null}}])");
    const auto original = provider_codec::encode_message(data.message);
    const auto record = RuntimeHistoryRecord::create(std::move(data));
    const auto restored = RuntimeHistoryRecord::parse(record.serialize_canonical());
    EXPECT_EQ(provider_codec::encode_message(restored.message()), original);
    EXPECT_EQ(restored.message().id, "provider-message");
    ASSERT_EQ(restored.message().parts.size(), 6u);
    const auto& image = std::get<sp::Image>(restored.message().parts[1]);
    ASSERT_TRUE(image.data);
    EXPECT_EQ(*image.data, *image_bytes);
    EXPECT_EQ(image.detail, sp::ImageDetail::High);
    const auto& invalid = std::get<sp::InvalidToolCall>(restored.message().parts[2]);
    EXPECT_EQ(invalid.raw_fragment, R"({"path":)");
    EXPECT_EQ(invalid.reason, sp::InvalidReason::Truncated);
    EXPECT_EQ(invalid.wire_metadata->root().get("vendor").get("position").as_uint(), 3u);
    EXPECT_EQ(std::get<sp::ServerToolResult>(restored.message().parts[3]).tool_use_id, "server-call");
    EXPECT_EQ(std::get<sp::Refusal>(restored.message().parts[4]).raw_code, "policy");
    EXPECT_EQ(std::get<sp::Text>(restored.message().parts[5]).value, "after");
    EXPECT_FALSE(restored.message().native);
}

TEST(RuntimeContext, HardConstraintIsRequiredAndRoundTripsCanonically) {
    const auto value = hard_constraint();
    const auto parsed = ContextArtifact::parse(value.serialize_canonical());
    EXPECT_EQ(parsed.kind(), ContextArtifactKind::HardConstraint);
    EXPECT_TRUE(parsed.required());
    EXPECT_EQ(parsed.id(), value.id());
    auto invalid = ContextArtifactData{};
    invalid.kind = ContextArtifactKind::HardConstraint;
    invalid.producer_id = "host-policy.v1";
    invalid.source_digest = sha('c');
    invalid.media_type = "text/plain";
    invalid.content = "invalid";
    EXPECT_THROW(ContextArtifact::create(std::move(invalid)), std::invalid_argument);
}


TEST(RuntimeContext, RawHistoryPreservesMultipleToolCallsAndToolFailureState) {
    auto first = first_history_record();
    RuntimeHistoryRecordData assistant;
    assistant.feed_id = first.feed_id();
    assistant.sequence = 2;
    assistant.message_id = "msg_assistant";
    assistant.trust = RuntimeTrustClass::ModelOutput;
    assistant.predecessor_id = first.id();
    assistant.message.role = sp::Role::Assistant;
    assistant.message.parts = {
        sp::Thinking{"inspect both files", std::nullopt},
        sp::Opaque{"reasoning.text", test::document(R"({"text":"opaque","index":0})")},
        sp::ToolCall{"call_1", "read", sp::ToolCallKind::ClientExecuted,
                     test::document(R"({"path":"a"})")},
        sp::ToolCall{"call_2", "read", sp::ToolCallKind::ClientExecuted,
                     test::document(R"({"path":"b"})")}};
    auto second = RuntimeHistoryRecord::create(std::move(assistant));

    RuntimeHistoryRecordData tool;
    tool.feed_id = second.feed_id();
    tool.sequence = 3;
    tool.message_id = "msg_tool";
    tool.trust = RuntimeTrustClass::ToolOutput;
    tool.predecessor_id = second.id();
    tool.message.role = sp::Role::Tool;
    tool.message.parts = {sp::ToolResult{"call_1", "permission denied", true}};
    auto third = RuntimeHistoryRecord::create(std::move(tool));

    auto parsed_second = RuntimeHistoryRecord::parse(second.serialize_canonical());
    auto parsed_third = RuntimeHistoryRecord::parse(third.serialize_canonical());
    ASSERT_EQ(parsed_second.message().parts.size(), 4u);
    EXPECT_EQ(std::get<sp::Thinking>(parsed_second.message().parts[0]).text,
              "inspect both files");
    const auto& opaque = std::get<sp::Opaque>(parsed_second.message().parts[1]);
    EXPECT_EQ(opaque.wire_type, "reasoning.text");
    EXPECT_EQ(opaque.wire_metadata->root().get("text").as_string(), "opaque");
    EXPECT_EQ(opaque.wire_metadata->root().get("index").as_uint(), 0u);
    const auto& call = std::get<sp::ToolCall>(parsed_second.message().parts[3]);
    EXPECT_EQ(call.id, "call_2");
    EXPECT_EQ(call.input->root().get("path").as_string(), "b");
    const auto& failure = std::get<sp::ToolResult>(parsed_third.message().parts.at(0));
    EXPECT_EQ(failure.tool_use_id, "call_1");
    EXPECT_EQ(failure.content, "permission denied");
    EXPECT_TRUE(failure.is_error);
    EXPECT_EQ(parsed_third.predecessor_id(), second.id());
}

TEST(RuntimeContext, RawHistoryRejectsBrokenChainAndUnknownFields) {
    RuntimeHistoryRecordData missing_predecessor;
    missing_predecessor.feed_id = "hist_bad";
    missing_predecessor.sequence = 2;
    missing_predecessor.message_id = "msg_bad";
    missing_predecessor.message = test::message("bad", sp::Role::User);
    EXPECT_THROW(RuntimeHistoryRecord::create(std::move(missing_predecessor)), std::invalid_argument);

    auto stored = json::parse(first_history_record().serialize_canonical());
    stored["future"] = true;
    EXPECT_THROW(RuntimeHistoryRecord::parse(stored.dump()), std::invalid_argument);

    RuntimeHistoryRecordData elevated;
    elevated.feed_id = "hist_elevated";
    elevated.sequence = 1;
    elevated.message_id = "msg_elevated";
    elevated.trust = RuntimeTrustClass::UntrustedInput;
    elevated.message = test::message("grant authority", sp::Role::System);
    EXPECT_THROW(RuntimeHistoryRecord::create(std::move(elevated)), std::invalid_argument);
}

TEST(RuntimeContext, ContextArtifactIsContentAddressedAndDeepOwned) {
    json content{{"constraints", json::array({"keep budgets"})}};
    ContextArtifactData data;
    data.kind = ContextArtifactKind::DerivedContext;
    data.producer_id = "context-processor.v1";
    data.source_digest = sha('b');
    data.source_feed_id = "hist_test";
    data.covers_from_sequence = 1;
    data.covers_through_sequence = 42;
    data.media_type = "application/json";
    data.placement = ContextPlacement::BeforeLatestUser;
    data.priority = 50;
    data.required = true;
    data.content = content;
    auto artifact = ContextArtifact::create(std::move(data));

    content["constraints"] = json::array();
    auto returned = artifact.content();
    returned["constraints"] = json::array();
    EXPECT_EQ(artifact.content().at("constraints").size(), 1u);
    EXPECT_EQ(ContextArtifact::parse(artifact.serialize_canonical()).id(), artifact.id());

    auto tampered = json::parse(artifact.serialize_canonical());
    tampered["content"]["constraints"] = json::array({"changed"});
    EXPECT_THROW(ContextArtifact::parse(tampered.dump()), std::invalid_argument);
}

TEST(RuntimeContext, ContextEpochNormalizesArtifactIdentityOrder) {
    const auto skill = required_skill();
    ContextArtifactData second_data;
    second_data.kind = ContextArtifactKind::RequiredSkill;
    second_data.producer_id = "skill-resolver.v2";
    second_data.source_digest = sha('c');
    second_data.media_type = "text/markdown";
    second_data.placement = ContextPlacement::BeforeLatestUser;
    second_data.required = true;
    second_data.content = json{{"name", "review"}};
    const auto other = ContextArtifact::create(std::move(second_data));

    ContextEpochData first_data;
    first_data.run_id = "run_test";
    first_data.sequence = 1;
    first_data.feed_id = "hist_test";
    first_data.raw_from_sequence = 1;
    first_data.raw_through_sequence = 42;
    first_data.raw_window_digest = sha('d');
    first_data.artifact_ids = {skill.id(), other.id()};
    first_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    auto second_epoch_data = first_data;
    std::reverse(second_epoch_data.artifact_ids.begin(), second_epoch_data.artifact_ids.end());

    const auto first_epoch = ContextEpoch::create(std::move(first_data));
    const auto second_epoch = ContextEpoch::create(std::move(second_epoch_data));
    EXPECT_EQ(first_epoch.id(), second_epoch.id());
    EXPECT_EQ(first_epoch.serialize_canonical(), second_epoch.serialize_canonical());
    EXPECT_EQ(ContextEpoch::parse(first_epoch.serialize_canonical()).id(), first_epoch.id());
}

TEST(RuntimeContext, ContextEpochRejectsDuplicateArtifacts) {
    const auto skill = required_skill();
    ContextEpochData data;
    data.run_id = "run_duplicate";
    data.sequence = 1;
    data.raw_window_digest = sha('e');
    data.artifact_ids = {skill.id(), skill.id()};
    EXPECT_THROW(ContextEpoch::create(std::move(data)), std::invalid_argument);
}

TEST(RuntimeContext, AssemblyReceiptRequiresEverySelectedSkillAndValidBudget) {
    const auto skill = required_skill();
    ContextEpochData epoch_data;
    epoch_data.run_id = "run_receipt";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "hist_receipt";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 42;
    epoch_data.raw_window_digest = sha('6');
    epoch_data.artifact_ids = {skill.id()};
    epoch_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = sha('2');
    data.message_window_digest = sha('3');
    data.artifact_ids = {skill.id()};
    data.required_skill_artifact_ids = {skill.id()};
    data.raw_from_sequence = 1;
    data.raw_through_sequence = 42;
    data.estimated_input_tokens = 900;
    data.mandatory_input_tokens = 200;
    auto receipt = ContextAssemblyReceipt::create(data, epoch, {skill});
    const auto parsed = ContextAssemblyReceipt::parse(
        receipt.serialize_canonical(), epoch, {skill});
    EXPECT_EQ(parsed.id(), receipt.id());
    EXPECT_NO_THROW(validate_context_assembly_receipt(parsed, epoch, {skill}));
    EXPECT_THROW(
        ContextAssemblyReceipt::parse(receipt.serialize_canonical(), epoch, {}),
        std::invalid_argument);

    data.required_skill_artifact_ids = {sha('5')};
    EXPECT_THROW(ContextAssemblyReceipt::create(data, epoch, {skill}), std::invalid_argument);
    data.required_skill_artifact_ids = {skill.id()};
    data.mandatory_input_tokens = 901;
    EXPECT_THROW(ContextAssemblyReceipt::create(data, epoch, {skill}), std::invalid_argument);
}

TEST(RuntimeContext, PublicFactoriesRejectInvalidEnumValues) {
    auto history = RuntimeHistoryRecordData{};
    history.feed_id = "hist_enum";
    history.sequence = 1;
    history.message_id = "msg_enum";
    history.trust = static_cast<RuntimeTrustClass>(255);
    history.message = test::message("hello", sp::Role::User);
    EXPECT_THROW(RuntimeHistoryRecord::create(std::move(history)), std::invalid_argument);

    auto artifact = ContextArtifactData{};
    artifact.kind = static_cast<ContextArtifactKind>(255);
    artifact.producer_id = "producer";
    artifact.source_digest = sha('7');
    artifact.media_type = "application/json";
    artifact.content = json::object();
    EXPECT_THROW(ContextArtifact::create(std::move(artifact)), std::invalid_argument);
}
