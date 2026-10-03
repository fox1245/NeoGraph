#include <gtest/gtest.h>

#include <neograph/provider_outcome_codec.h>
#include <neograph/runtime_context.h>
#include "fixtures/typed_provider.h"

namespace {
using namespace neograph;

TEST(MessageReasoning, HistoryRetainsOrderedTypedReasoningWithoutGrantingNativeAuthority) {
    sp::Message original;
    original.id = "assistant-reasoning";
    original.role = sp::Role::Assistant;
    original.parts = {
        sp::Text{"before"},
        sp::Reasoning{"reasoning-1", {"summary one", "summary two"},
                      "encrypted", "completed", {"private reasoning"}},
        sp::Opaque{"reasoning.text", test::document(R"({"text":"opaque","index":0})")},
        sp::Thinking{"signed reasoning", std::string("signature")},
        sp::RedactedThinking{"redacted"},
        sp::Thought{{"thought summary"}, std::string("thought-signature")},
        sp::Text{"done"}};
    original.wire_output = test::document(R"([{"type":"reasoning","encrypted_content":"encrypted"}])");
    RuntimeHistoryRecordData data;
    data.feed_id = "reasoning-feed";
    data.sequence = 1;
    data.message_id = original.id;
    data.trust = RuntimeTrustClass::ModelOutput;
    data.message = original;
    const auto record = RuntimeHistoryRecord::create(std::move(data));
    const auto restored = RuntimeHistoryRecord::parse(record.serialize_canonical());

    EXPECT_EQ(provider_codec::encode_message(restored.message()),
              provider_codec::encode_message(original));
    ASSERT_EQ(restored.message().parts.size(), 7u);
    EXPECT_EQ(std::get<sp::Text>(restored.message().parts[0]).value, "before");
    const auto& reasoning = std::get<sp::Reasoning>(restored.message().parts[1]);
    EXPECT_EQ(reasoning.summary, (std::vector<std::string>{"summary one", "summary two"}));
    EXPECT_EQ(reasoning.encrypted_content, std::optional<std::string>("encrypted"));
    EXPECT_EQ(reasoning.content, (std::vector<std::string>{"private reasoning"}));
    EXPECT_EQ(std::get<sp::Thinking>(restored.message().parts[3]).signature,
              std::optional<std::string>("signature"));
    EXPECT_EQ(std::get<sp::RedactedThinking>(restored.message().parts[4]).data, "redacted");
    EXPECT_EQ(std::get<sp::Thought>(restored.message().parts[5]).signature,
              std::optional<std::string>("thought-signature"));
    EXPECT_EQ(std::get<sp::Text>(restored.message().parts[6]).value, "done");
    EXPECT_FALSE(restored.message().native);
}

TEST(MessageReasoning, ImportedProjectionCannotClaimNativeContinuation) {
    auto encoded = provider_codec::encode_message(test::message("done"));
    encoded["native"] = true;
    EXPECT_THROW(provider_codec::decode_message(encoded), std::runtime_error);
}

TEST(MessageReasoning, EmptyAndAbsentSignaturesRemainDistinct) {
    sp::Message original;
    original.parts = {sp::Thinking{"unsigned", std::nullopt},
                      sp::Thinking{"empty signature", std::string{}},
                      sp::Thought{{"unsigned"}, std::nullopt},
                      sp::Thought{{"empty signature"}, std::string{}}};
    const auto restored = provider_codec::decode_message(provider_codec::encode_message(original));
    EXPECT_FALSE(std::get<sp::Thinking>(restored.parts.at(0)).signature);
    ASSERT_TRUE(std::get<sp::Thinking>(restored.parts.at(1)).signature);
    EXPECT_TRUE(std::get<sp::Thinking>(restored.parts.at(1)).signature->empty());
    EXPECT_FALSE(std::get<sp::Thought>(restored.parts.at(2)).signature);
    ASSERT_TRUE(std::get<sp::Thought>(restored.parts.at(3)).signature);
    EXPECT_TRUE(std::get<sp::Thought>(restored.parts.at(3)).signature->empty());
}

TEST(MessageReasoning, FullRawPartialBeyondOneMiBRoundTripsWithoutFabricatedUsage) {
    const std::string payload(2 * 1024 * 1024, 'r');
    const auto source = "{\"vendor_blob\":" + sp::json::quote(payload) +
                        ",\"ordered\":[3,1,2]}";
    auto parsed = sp::json::parse(source, {source.size(), 8});
    ASSERT_TRUE(std::holds_alternative<sp::json::Document>(parsed));
    auto document = std::make_shared<const sp::json::Document>(
        std::get<sp::json::Document>(std::move(parsed)));
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::Truncated;
    failure.error.attempt.request_may_have_left = true;
    failure.error.attempt.response_head_seen = true;
    failure.partial.messages = {test::message("partial")};
    failure.partial.usage.output_total = sp::Count{7, sp::Evidence::Reported};
    failure.partial.usage.stage = sp::UsageStage::Partial;
    failure.partial.raw_events = {sp::RawWire{"response.partial", document}};
    failure.partial.wire_envelope = document;
    const auto encoded = provider_codec::encode_outcome(sp::Outcome(std::move(failure)));
    document.reset();
    const auto retained = provider_codec::decode_outcome(encoded);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*retained));
    const auto& actual = std::get<sp::Failure>(*retained);
    EXPECT_EQ(actual.error.kind, sp::ErrorKind::Truncated);
    EXPECT_TRUE(actual.error.attempt.request_may_have_left);
    EXPECT_TRUE(actual.error.attempt.response_head_seen);
    ASSERT_EQ(actual.partial.raw_events.size(), 1u);
    EXPECT_EQ(actual.partial.raw_events[0].payload->root().get("vendor_blob").as_string(), payload);
    EXPECT_EQ(actual.partial.wire_envelope->root().get("ordered").dump(), "[3,1,2]");
    EXPECT_EQ(std::get<sp::Text>(actual.partial.messages[0].parts[0]).value, "partial");
    ASSERT_TRUE(actual.partial.usage.output_total);
    EXPECT_EQ(actual.partial.usage.output_total->value, 7u);
    EXPECT_FALSE(actual.partial.usage.input_total);
    EXPECT_FALSE(actual.partial.usage.total);
    EXPECT_EQ(actual.partial.usage.stage, sp::UsageStage::Partial);
}

TEST(MessageReasoning, DuplicateDiagnosticDocumentsNeverBecomeExecutableToolInputs) {
    auto parsed = sp::json::parse(R"({"x":1,"x":2})");
    ASSERT_TRUE(std::holds_alternative<sp::json::ParseError>(parsed));
    auto document = std::make_shared<const sp::json::Document>(
        std::move(std::get<sp::json::ParseError>(parsed).context));
    sp::Message message;
    message.parts = {sp::InvalidToolCall{"bad", "lookup", sp::ToolCallKind::ClientExecuted,
        R"({"x":1,"x":2})", sp::InvalidReason::DuplicateKey, "tool_use", document},
        sp::Opaque{"diagnostic", document}};
    message.wire_output = document;
    const auto retained = provider_codec::decode_message(provider_codec::encode_message(message));
    EXPECT_FALSE(retained.native);
    const auto& invalid = std::get<sp::InvalidToolCall>(retained.parts[0]);
    EXPECT_EQ(invalid.reason, sp::InvalidReason::DuplicateKey);
    EXPECT_EQ(invalid.raw_fragment, R"({"x":1,"x":2})");
    EXPECT_EQ(invalid.wire_metadata->root().dump(), R"({"x":1,"x":2})");
    EXPECT_EQ(retained.wire_output->root().dump(), R"({"x":1,"x":2})");
    message.parts = {sp::ToolCall{"bad", "lookup", sp::ToolCallKind::ClientExecuted, document}};
    EXPECT_THROW(provider_codec::decode_message(provider_codec::encode_message(message)),
                 std::runtime_error);
}
} // namespace
