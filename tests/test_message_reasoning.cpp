#include <gtest/gtest.h>

#include <neograph/types.h>

namespace {

using neograph::ChatMessage;
using neograph::json;

TEST(MessageReasoning, DurableJsonRoundTripsReasoningAndContinuation) {
    ChatMessage original;
    original.role = "assistant";
    original.content = "done";
    original.reasoning = "private reasoning";
    original.reasoning_details = json::array({
        {{"type", "reasoning.text"}, {"text", "opaque"}, {"index", 0}}
    });

    json stored;
    neograph::to_json(stored, original);
    ChatMessage restored;
    neograph::from_json(stored, restored);

    EXPECT_EQ(restored.reasoning, original.reasoning);
    EXPECT_EQ(restored.reasoning_details, original.reasoning_details);
}

TEST(MessageReasoning, OpenRouterContinuationTakesWirePrecedence) {
    ChatMessage message;
    message.role = "assistant";
    message.content = "done";
    message.reasoning = "private reasoning";
    message.reasoning_details = json::array({
        {{"type", "reasoning.text"}, {"text", "opaque"}, {"index", 0}}
    });

    const auto wire = neograph::messages_to_json({message});

    ASSERT_EQ(wire.size(), 1U);
    EXPECT_EQ(wire[0]["reasoning_details"], message.reasoning_details);
    EXPECT_FALSE(wire[0].contains("reasoning_content"));
}

TEST(MessageReasoning, DeepSeekReasoningTextUsesOfficialWireFieldWithoutDetails) {
    ChatMessage message;
    message.role = "assistant";
    message.reasoning = "private reasoning";

    const auto wire = neograph::messages_to_json({message});

    ASSERT_EQ(wire.size(), 1U);
    EXPECT_EQ(wire[0]["reasoning_content"], "private reasoning");
    EXPECT_FALSE(wire[0].contains("reasoning_details"));
}

TEST(MessageReasoning, ParsesOpenRouterReasoningResponse) {
    const json choice = {
        {"message", {
            {"role", "assistant"},
            {"content", "done"},
            {"reasoning", "private reasoning"},
            {"reasoning_details", json::array({
                {{"type", "reasoning.text"}, {"text", "opaque"}, {"index", 0}}
            })}
        }}
    };

    const auto parsed = neograph::parse_response_message(choice);

    EXPECT_EQ(parsed.reasoning, "private reasoning");
    EXPECT_EQ(parsed.reasoning_details, choice["message"]["reasoning_details"]);
}

TEST(MessageReasoning, ParsesOfficialDeepSeekReasoningResponse) {
    const json choice = {
        {"message", {
            {"role", "assistant"},
            {"content", "done"},
            {"reasoning_content", "private reasoning"}
        }}
    };

    const auto parsed = neograph::parse_response_message(choice);

    EXPECT_EQ(parsed.reasoning, "private reasoning");
    EXPECT_TRUE(parsed.reasoning_details.empty());
}

}  // namespace
