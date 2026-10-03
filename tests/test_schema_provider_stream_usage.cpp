#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;

TEST(SchemaProviderStreamUsage, MessagesStreamingReportsInclusiveCacheUsage) {
    const std::string events =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg-1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"model\":\"fixture-model\",\"stop_reason\":null,\"stop_sequence\":null,\"usage\":{\"input_tokens\":50,\"output_tokens\":0,\"cache_read_input_tokens\":10,\"cache_creation_input_tokens\":5}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hello!\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    wire::Peer peer(events, true);
    auto provider = wire::provider("anthropic.messages", peer.origin());
    const auto result = provider->invoke(wire::request("anthropic.messages", ProviderMode::Stream));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "hello!");
    const auto& usage = test::completion(result).usage;
    ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.output_total); ASSERT_TRUE(usage.total);
    EXPECT_EQ(usage.input_total->value, 65u);
    EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Derived);
    ASSERT_TRUE(usage.input_uncached); ASSERT_TRUE(usage.cache_read); ASSERT_TRUE(usage.cache_write);
    EXPECT_EQ(usage.input_uncached->value, 50u);
    EXPECT_EQ(usage.cache_read->value, 10u); EXPECT_EQ(usage.cache_write->value, 5u);
    EXPECT_EQ(usage.output_total->value, 7u); EXPECT_EQ(usage.total->value, 72u);
    EXPECT_EQ(usage.input_uncached->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.cache_read->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.cache_write->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    EXPECT_EQ(usage.stage, sp::UsageStage::Final);
}

TEST(SchemaProviderStreamUsage, MessagesMissingCacheBandsCannotInventInclusiveTotals) {
    for (const bool read : {false, true}) for (const bool write : {false, true}) {
        SCOPED_TRACE(read);
        SCOPED_TRACE(write);
        json initial_usage = {{"input_tokens", 0}, {"output_tokens", 0}};
        if (read) initial_usage["cache_read_input_tokens"] = 0;
        if (write) initial_usage["cache_creation_input_tokens"] = 0;
        const json start = {{"type", "message_start"}, {"message", {
            {"id", "msg-cache"}, {"type", "message"}, {"role", "assistant"},
            {"content", json::array()}, {"model", "fixture-model"},
            {"stop_reason", nullptr}, {"stop_sequence", nullptr}, {"usage", initial_usage}}}};
        const auto events = "event: message_start\ndata: " + start.dump() + "\n\n"
            + "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}\n\n"
            + "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
        wire::Peer peer(events, true);
        auto provider = wire::provider("anthropic.messages", peer.origin());
        const auto result = provider->invoke(wire::request("anthropic.messages", ProviderMode::Stream));
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto& usage = test::completion(result).usage;
        ASSERT_TRUE(usage.input_uncached); ASSERT_TRUE(usage.output_total);
        EXPECT_EQ(usage.input_uncached->value, 0u);
        EXPECT_EQ(usage.input_uncached->evidence, sp::Evidence::Reported);
        EXPECT_EQ(usage.output_total->value, 3u);
        EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Reported);
        ASSERT_EQ(usage.cache_read.has_value(), read);
        ASSERT_EQ(usage.cache_write.has_value(), write);
        if (read) EXPECT_EQ(usage.cache_read->value, 0u);
        if (write) EXPECT_EQ(usage.cache_write->value, 0u);
        ASSERT_EQ(usage.input_total.has_value(), read && write);
        ASSERT_EQ(usage.total.has_value(), read && write);
        if (read && write) {
            EXPECT_EQ(usage.input_total->value, 0u);
            EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Derived);
            EXPECT_EQ(usage.total->value, 3u);
            EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
        }
        EXPECT_FALSE(usage.provider_reported_total);
        EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    }
}

TEST(SchemaProviderStreamUsage, ChatStreamingOwnsReasoningAndFinalUsage) {
    const std::string events =
        wire::chat_frame(json::parse(R"({"choices":[{"index":0,"delta":{"role":"assistant","content":"hi","reasoning_content":"Need "},"finish_reason":null}]})"))
        + wire::chat_frame(json::parse(R"({"choices":[{"index":0,"delta":{"content":" there","reasoning_content":"approved lookup."},"finish_reason":null}]})"))
        + wire::chat_frame(json::parse(R"({"choices":[{"index":0,"delta":{},"finish_reason":"length"}]})"))
        + wire::chat_frame(json::parse(R"({"choices":[],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}})"))
        + "data: [DONE]\n\n";
    wire::Peer peer(events, true);
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request("openai.chat", ProviderMode::Stream);
    auto observed = std::make_shared<std::string>();
    request.on_event = [owned = observed](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event); delta && delta->payload.kind == sp::PartKind::Thinking)
            owned->append(delta->payload.bytes);
    };
    const auto result = provider->invoke(std::move(request));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& completion = test::completion(result);
    EXPECT_EQ(test::text(result), "hi there");
    std::string reasoning;
    for (const auto& message : completion.messages) for (const auto& part : message.parts)
        if (const auto* thinking = std::get_if<sp::Thinking>(&part)) reasoning += thinking->text;
    EXPECT_EQ(reasoning, "Need approved lookup.");
    EXPECT_EQ(*observed, reasoning);
    ASSERT_TRUE(completion.usage.input_total);
    ASSERT_TRUE(completion.usage.output_total);
    ASSERT_TRUE(completion.usage.total);
    ASSERT_TRUE(completion.usage.provider_reported_total);
    EXPECT_EQ(completion.usage.input_total->value, 12u);
    EXPECT_EQ(completion.usage.output_total->value, 3u);
    EXPECT_EQ(completion.usage.total->value, 15u);
    EXPECT_EQ(completion.usage.input_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(completion.usage.output_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(completion.usage.total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(completion.usage.provider_reported_total->value, 15u);
    EXPECT_EQ(completion.usage.provider_reported_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(completion.usage.stage, sp::UsageStage::Final);
    EXPECT_EQ(completion.usage.quality, sp::UsageQuality::Consistent);
    EXPECT_EQ(completion.stop.kind, sp::StopKind::MaxTokens);
}

TEST(SchemaProviderStreamUsage, KnownZeroAndMissingCountersStayDistinct) {
    for (const bool zero : {false, true}) {
        auto body = json::parse(wire::chat_response());
        body["usage"] = nullptr;
        if (zero) body["usage"] = {{"prompt_tokens", 0}, {"completion_tokens", 0}, {"total_tokens", 0}};
        wire::Peer peer(body.dump());
        auto provider = wire::provider("openai.chat", peer.origin());
        const auto result = provider->invoke(wire::request());
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto& usage = test::completion(result).usage;
        ASSERT_EQ(usage.input_total.has_value(), zero);
        ASSERT_EQ(usage.output_total.has_value(), zero);
        ASSERT_EQ(usage.total.has_value(), zero);
        ASSERT_EQ(usage.provider_reported_total.has_value(), zero);
        if (zero) {
            EXPECT_EQ(usage.input_total->value, 0u);
            EXPECT_EQ(usage.output_total->value, 0u);
            EXPECT_EQ(usage.total->value, 0u);
            EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
            EXPECT_EQ(usage.provider_reported_total->value, 0u);
            EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
            EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        } else EXPECT_EQ(usage.stage, sp::UsageStage::Missing);
        EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    }
}

TEST(SchemaProviderStreamUsage, WideInconsistentReportsRetainEvidenceWithoutTruncation) {
    auto body = json::parse(wire::chat_response());
    const std::uint64_t wide = std::uint64_t{1} << 40;
    body["usage"] = {{"prompt_tokens", wide}, {"completion_tokens", 3}, {"total_tokens", wide + 4}};
    wire::Peer peer(body.dump()); auto provider = wire::provider("openai.chat", peer.origin());
    const auto result = provider->invoke(wire::request());
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& usage = test::completion(result).usage;
    ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.total); ASSERT_TRUE(usage.provider_reported_total);
    EXPECT_EQ(usage.input_total->value, wide);
    EXPECT_EQ(usage.total->value, wide + 3);
    EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(usage.provider_reported_total->value, wide + 4);
    EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.quality, sp::UsageQuality::Inconsistent);
    ASSERT_EQ(usage.conflicts.size(), 1u);
    EXPECT_EQ(usage.conflicts[0].counter, "total");
}

TEST(SchemaProviderStreamUsage, InteractionsReportedTotalDoesNotInventMissingThoughtOutput) {
    for (const bool thoughts : {false, true}) {
        SCOPED_TRACE(thoughts);
        auto body = json::parse(R"({"id":"interaction-usage","model":"fixture-model","status":"completed","steps":[{"type":"model_output","content":[{"type":"text","text":"done"}]}],"usage":{"total_input_tokens":4,"total_output_tokens":2,"total_tokens":6}})");
        if (thoughts) body["usage"]["total_thought_tokens"] = 0;
        wire::Peer peer(body.dump());
        auto provider = wire::provider("google.interactions", peer.origin());
        const auto result = provider->invoke(wire::request("google.interactions"));
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        EXPECT_EQ(test::text(result), "done");
        const auto& usage = test::completion(result).usage;
        ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.provider_reported_total); ASSERT_TRUE(usage.total);
        EXPECT_EQ(usage.input_total->value, 4u);
        EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Reported);
        EXPECT_EQ(usage.provider_reported_total->value, 6u);
        EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
        EXPECT_EQ(usage.total->value, 6u);
        EXPECT_EQ(usage.total->evidence, thoughts ? sp::Evidence::Derived : sp::Evidence::Reported);
        ASSERT_EQ(usage.output_total.has_value(), thoughts);
        ASSERT_EQ(usage.reasoning.has_value(), thoughts);
        if (thoughts) {
            EXPECT_EQ(usage.reasoning->value, 0u);
            EXPECT_EQ(usage.reasoning->evidence, sp::Evidence::Reported);
            EXPECT_EQ(usage.output_total->value, 2u);
            EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Derived);
        }
        EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    }
}
