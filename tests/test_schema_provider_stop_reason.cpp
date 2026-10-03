#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;

TEST(SchemaProviderStopReason, ChatReasonsRetainNormalizedKindAndRawVendorValue) {
    for (const auto& [raw, kind] : std::vector<std::pair<std::string, sp::StopKind>>{
        {"stop", sp::StopKind::EndTurn}, {"length", sp::StopKind::MaxTokens},
        {"content_filter", sp::StopKind::ContentFilter}, {"vendor_future_reason", sp::StopKind::Unknown}}) {
        SCOPED_TRACE(raw);
        wire::Peer peer(wire::chat_response("done", raw));
        auto provider = wire::provider("openai.chat", peer.origin());
        const auto result = provider->invoke(wire::request());
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        EXPECT_EQ(test::completion(result).stop.kind, kind);
        EXPECT_EQ(test::completion(result).stop.raw, raw);
    }
}

TEST(SchemaProviderStopReason, MessagesSequenceAndRefusalRemainDistinct) {
    for (const auto& [raw, kind] : std::vector<std::pair<std::string, sp::StopKind>>{
        {"stop_sequence", sp::StopKind::StopSequence}, {"refusal", sp::StopKind::Refusal}}) {
        json body = {{"id", "msg-1"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
            {"content", json::array({{{"type", "text"}, {"text", "done"}}})},
            {"stop_reason", raw}, {"usage", {{"input_tokens", 1}, {"output_tokens", 2}}}};
        if (raw == "stop_sequence") body["stop_sequence"] = "END";
        wire::Peer peer(body.dump()); auto provider = wire::provider("anthropic.messages", peer.origin());
        const auto result = provider->invoke(wire::request("anthropic.messages"));
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        EXPECT_EQ(test::completion(result).stop.kind, kind);
        EXPECT_EQ(test::completion(result).stop.raw, raw);
        if (raw == "stop_sequence") EXPECT_EQ(test::completion(result).stop.sequence, "END");
    }
}

TEST(SchemaProviderStopReason, ResponsesIncompletePreservesPartialTextAndMaxTokens) {
    auto body = json::parse(wire::responses_body("done"));
    body["status"] = "incomplete";
    body["incomplete_details"] = {{"reason", "max_output_tokens"}};
    wire::Peer peer(body.dump()); auto provider = wire::provider("openai.responses", peer.origin());
    const auto result = provider->invoke(wire::request("openai.responses"));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "done");
    EXPECT_EQ(test::completion(result).stop.kind, sp::StopKind::MaxTokens);
    EXPECT_EQ(test::completion(result).stop.raw, "max_output_tokens");
}

TEST(SchemaProviderStopReason, GeminiSafetyIsFailureWithPreservedStopDetails) {
    wire::Peer peer(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"partial"}]},"finishReason":"SAFETY","safetyRatings":[{"category":"HARM_CATEGORY_DANGEROUS_CONTENT","probability":"HIGH"}]}]})");
    auto provider = wire::provider("google.generate", peer.origin());
    const auto result = provider->invoke(wire::request("google.generate"));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    const auto& failure = wire::failure(result);
    EXPECT_EQ(failure.error.kind, sp::ErrorKind::RemoteFailure);
    ASSERT_TRUE(failure.partial.stop);
    EXPECT_EQ(failure.partial.stop->kind, sp::StopKind::ContentFilter);
    EXPECT_EQ(failure.partial.stop->raw, "SAFETY");
    ASSERT_TRUE(failure.partial.stop->details);
    EXPECT_EQ(failure.partial.stop->details->root().get("safetyRatings").at(0).get("probability").as_string(), "HIGH");
}
