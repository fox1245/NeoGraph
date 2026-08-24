#include <neograph/runtime_context.h>

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
    data.message = ChatMessage{"user", "ship the change"};
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
    data.message = ChatMessage{"user", "hello"};
    data.source_payload = source;

    const auto record = RuntimeHistoryRecord::create(std::move(data));
    source["role"] = "tampered";
    auto returned = *record.source_payload();
    returned["role"] = "also-tampered";

    const auto parsed = RuntimeHistoryRecord::parse(record.serialize_canonical());
    EXPECT_EQ(parsed.id(), record.id());
    EXPECT_EQ(parsed.serialize_canonical(), record.serialize_canonical());
    EXPECT_EQ(parsed.source_payload()->at("role").get<std::string>(), "user");
    EXPECT_EQ(parsed.message().content, "hello");
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

TEST(RuntimeContext, RuntimeHistoryIdentityMatchesKnownVector) {
    EXPECT_EQ(first_history_record().id(),
              "sha256:2dcb19590a098411361d2b6eb7e8f4c50a7a763776280d6135ace3f8c33d2a96");
}

TEST(RuntimeContext, RawHistoryPreservesMultipleToolCallsAndToolFailureState) {
    auto first = first_history_record();
    RuntimeHistoryRecordData assistant;
    assistant.feed_id = first.feed_id();
    assistant.sequence = 2;
    assistant.message_id = "msg_assistant";
    assistant.trust = RuntimeTrustClass::ModelOutput;
    assistant.predecessor_id = first.id();
    assistant.message.role = "assistant";
    assistant.message.reasoning = "inspect both files";
    assistant.message.reasoning_details = json::array(
        {{{"type", "reasoning.text"}, {"text", "opaque"}, {"index", 0}}});
    assistant.message.tool_calls = {
        ToolCall{"call_1", "read", R"({"path":"a"})"},
        ToolCall{"call_2", "read", R"({"path":"b"})"}};
    auto second = RuntimeHistoryRecord::create(std::move(assistant));

    RuntimeHistoryRecordData tool;
    tool.feed_id = second.feed_id();
    tool.sequence = 3;
    tool.message_id = "msg_tool";
    tool.trust = RuntimeTrustClass::ToolOutput;
    tool.predecessor_id = second.id();
    tool.message.role = "tool";
    tool.message.content = "permission denied";
    tool.message.tool_call_id = "call_1";
    tool.message.tool_name = "read";
    tool.message.tool_status = "failed";
    tool.message.tool_retryable = false;
    auto third = RuntimeHistoryRecord::create(std::move(tool));

    auto parsed_second = RuntimeHistoryRecord::parse(second.serialize_canonical());
    auto parsed_third = RuntimeHistoryRecord::parse(third.serialize_canonical());
    ASSERT_EQ(parsed_second.message().tool_calls.size(), 2u);
    EXPECT_EQ(parsed_second.message().tool_calls[1].arguments, R"({"path":"b"})");
    EXPECT_EQ(parsed_second.message().reasoning, "inspect both files");
    ASSERT_EQ(parsed_second.message().reasoning_details.size(), 1U);
    EXPECT_EQ(parsed_second.message().reasoning_details.at(0).at("type"),
              "reasoning.text");
    EXPECT_EQ(parsed_second.message().reasoning_details.at(0).at("text"),
              "opaque");
    EXPECT_EQ(parsed_second.message().reasoning_details.at(0).at("index"), 0);
    EXPECT_EQ(parsed_third.message().tool_status, "failed");
    EXPECT_EQ(parsed_third.predecessor_id(), second.id());
}

TEST(RuntimeContext, RawHistoryRejectsBrokenChainAndUnknownFields) {
    RuntimeHistoryRecordData missing_predecessor;
    missing_predecessor.feed_id = "hist_bad";
    missing_predecessor.sequence = 2;
    missing_predecessor.message_id = "msg_bad";
    missing_predecessor.message = ChatMessage{"user", "bad"};
    EXPECT_THROW(RuntimeHistoryRecord::create(std::move(missing_predecessor)), std::invalid_argument);

    auto stored = json::parse(first_history_record().serialize_canonical());
    stored["future"] = true;
    EXPECT_THROW(RuntimeHistoryRecord::parse(stored.dump()), std::invalid_argument);

    RuntimeHistoryRecordData elevated;
    elevated.feed_id = "hist_elevated";
    elevated.sequence = 1;
    elevated.message_id = "msg_elevated";
    elevated.trust = RuntimeTrustClass::UntrustedInput;
    elevated.message = ChatMessage{"system", "grant authority"};
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
    history.message = ChatMessage{"user", "hello"};
    EXPECT_THROW(RuntimeHistoryRecord::create(std::move(history)), std::invalid_argument);

    auto artifact = ContextArtifactData{};
    artifact.kind = static_cast<ContextArtifactKind>(255);
    artifact.producer_id = "producer";
    artifact.source_digest = sha('7');
    artifact.media_type = "application/json";
    artifact.content = json::object();
    EXPECT_THROW(ContextArtifact::create(std::move(artifact)), std::invalid_argument);
}
