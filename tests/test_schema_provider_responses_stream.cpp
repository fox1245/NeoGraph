#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <core/native.h>

using namespace neograph;
namespace wire = neograph::test::wire;
namespace {
sp::runtime::Result stream(std::string body, sp::runtime::Options options = {}, int status = 200) {
    wire::Peer peer(std::move(body), true); peer.state->status = status;
    auto provider = wire::provider("openai.responses", peer.origin(), std::move(options));
    return provider->invoke(wire::request("openai.responses", ProviderMode::Stream));
}
std::string frame(std::string type, json payload) {
    payload["type"] = type;
    return "event: " + type + "\ndata: " + payload.dump() + "\n\n";
}
std::string tool_stream() {
    auto final = json::parse(wire::responses_body());
    const json item = {{"id", "fc-item"}, {"type", "function_call"}, {"call_id", "call-1"},
        {"name", "lookup_weather"}, {"arguments", R"({"city":"Seoul","unit":"C"})"}, {"status", "completed"}};
    final["output"] = json::array({item});
    auto initial = final; initial["output"] = json::array(); initial["status"] = "in_progress";
    auto added = item; added["arguments"] = ""; added["status"] = "in_progress";
    return frame("response.created", {{"response", initial}})
        + frame("response.output_item.added", {{"output_index", 0}, {"item", added}})
        + frame("response.function_call_arguments.delta", {{"output_index", 0}, {"item_id", "fc-item"}, {"delta", R"({"city":)"}})
        + frame("response.function_call_arguments.delta", {{"output_index", 0}, {"item_id", "fc-item"}, {"delta", R"("Seoul","unit":"C"})"}})
        + frame("response.function_call_arguments.done", {{"output_index", 0}, {"item_id", "fc-item"}, {"arguments", item.at("arguments")}})
        + frame("response.output_item.done", {{"output_index", 0}, {"item", item}})
        + frame("response.completed", {{"response", final}});
}
}

TEST(SchemaProviderResponsesStream, TextDeltasReconcileWithoutDuplicatingFinalSnapshot) {
    wire::Peer peer(wire::responses_sse("Hello world!"), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    auto request = wire::request("openai.responses", ProviderMode::Stream);
    auto deltas = std::make_shared<std::string>();
    request.on_event = [owned = deltas](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event); delta && delta->payload.kind == sp::PartKind::Text)
            owned->append(delta->payload.bytes);
    };
    const auto result = provider->invoke(std::move(request));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "Hello world!");
    EXPECT_EQ(*deltas, "Hello world!");
    const auto& completion = test::completion(result);
    const auto& usage = completion.usage;
    ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.output_total);
    ASSERT_TRUE(usage.total); ASSERT_TRUE(usage.provider_reported_total);
    EXPECT_EQ(usage.input_total->value, 3u);
    EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.output_total->value, 2u);
    EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.total->value, 5u);
    EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(usage.provider_reported_total->value, 5u);
    EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.stage, sp::UsageStage::Final);
    EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    ASSERT_EQ(completion.messages.size(), 1u);
    ASSERT_TRUE(completion.messages[0].native);
    EXPECT_TRUE(completion.messages[0].native->complete());
    ASSERT_TRUE(completion.messages[0].wire_output);
    EXPECT_EQ(completion.messages[0].wire_output->root().at(0).get("content").at(0).get("text").as_string(), "Hello world!");
    const std::vector<std::string> expected_types = {"response.created", "response.output_item.added",
        "response.content_part.added", "response.output_text.delta", "response.output_text.done",
        "response.output_item.done", "response.completed"};
    ASSERT_EQ(completion.raw_events.size(), expected_types.size());
    for (std::size_t i = 0; i < expected_types.size(); ++i) {
        EXPECT_EQ(completion.raw_events[i].type, expected_types[i]);
        ASSERT_TRUE(completion.raw_events[i].payload);
        EXPECT_EQ(completion.raw_events[i].payload->root().get("type").as_string(), expected_types[i]);
    }
    EXPECT_EQ(completion.raw_events[3].payload->root().get("delta").as_string(), "Hello world!");
    EXPECT_EQ(completion.raw_events.back().payload->root().get("response").get("usage").get("total_tokens").as_uint(), 5u);
}

TEST(SchemaProviderResponsesStream, OpenRouterDataOnlyEventsDispatchFromTypedPayload) {
    const auto result = stream(wire::responses_sse("data-only", false));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "data-only");
}

TEST(SchemaProviderResponsesStream, OpenRouterResponseDoneUsesAuthoritativeEnvelope) {
    auto events = wire::responses_sse("hosted-terminal", false);
    const std::string completed = "response.completed";
    const auto position = events.rfind(completed);
    ASSERT_NE(position, std::string::npos);
    events.replace(position, completed.size(), "response.done");
    const auto result = stream(events + "data: [DONE]\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "hosted-terminal");
}

TEST(SchemaProviderResponsesStream, DoneSentinelCannotInventMissingAuthoritativeTerminal) {
    const auto result = stream("data: [DONE]\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_TRUE(wire::failure(result).partial.messages.empty());
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::Truncated);
    EXPECT_EQ(wire::failure(result).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(wire::failure(result).partial.usage.total);
}

TEST(SchemaProviderResponsesStream, MissingOrUnknownTerminalRetainsActualPartialWireHistory) {
    for (const bool unknown : {false, true}) {
        SCOPED_TRACE(unknown);
        auto events = wire::responses_sse("owned partial");
        const auto terminal = events.rfind("event: response.completed\n");
        ASSERT_NE(terminal, std::string::npos);
        events.resize(terminal);
        if (unknown) events += frame("response.future_observation", {{"marker", "unprojected-wire"}});
        const auto result = stream(std::move(events));
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
        const auto& failure = wire::failure(result);
        EXPECT_EQ(failure.error.kind, unknown ? sp::ErrorKind::Unsupported : sp::ErrorKind::Truncated);
        EXPECT_TRUE(failure.error.attempt.request_may_have_left);
        ASSERT_EQ(failure.partial.messages.size(), 1u);
        const auto& message = failure.partial.messages[0];
        ASSERT_EQ(message.parts.size(), 1u);
        EXPECT_EQ(std::get<sp::Text>(message.parts[0]).value, "owned partial");
        EXPECT_FALSE(message.native && message.native->complete());
        EXPECT_EQ(failure.partial.usage.stage, sp::UsageStage::Partial);
        EXPECT_EQ(failure.partial.usage.quality, sp::UsageQuality::Consistent);
        ASSERT_TRUE(failure.partial.usage.total);
        EXPECT_EQ(failure.partial.usage.total->value, 5u);
        EXPECT_EQ(failure.partial.usage.total->evidence, sp::Evidence::Derived);
        const auto& raw = failure.partial.raw_events;
        ASSERT_EQ(raw.size(), unknown ? 7u : 6u);
        EXPECT_EQ(raw.front().type, "response.created");
        ASSERT_TRUE(raw[3].payload);
        EXPECT_EQ(raw[3].payload->root().get("delta").as_string(), "owned partial");
        EXPECT_EQ(raw.back().type, unknown ? "response.future_observation" : "response.output_item.done");
        ASSERT_TRUE(raw.back().payload);
        if (unknown) EXPECT_EQ(raw.back().payload->root().get("marker").as_string(), "unprojected-wire");
        else EXPECT_EQ(raw.back().payload->root().get("item").get("content").at(0).get("text").as_string(), "owned partial");
    }
}

TEST(SchemaProviderResponsesStream, ExplicitEventPayloadDisagreementFailsClosed) {
    const auto result = stream("event: response.created\ndata: {\"type\":\"response.completed\"}\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ProtocolCorrupt);
}

TEST(SchemaProviderResponsesStream, ErrorStatusCannotBeMaskedByTerminalPayload) {
    const auto result = stream(wire::responses_sse(), {}, 500);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.http_status, 500);
}

TEST(SchemaProviderResponsesStream, RejectsOversizedSseLine) {
    sp::runtime::Options options; options.limits.sse.max_line_bytes = 24;
    const auto result = stream(wire::responses_sse(), std::move(options));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ResourceLimit);
}

TEST(SchemaProviderResponsesStream, RejectsOversizedAggregateStream) {
    sp::runtime::Options options; options.limits.sse.max_total_bytes = 64;
    const auto result = stream(wire::responses_sse(), std::move(options));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ResourceLimit);
}

TEST(SchemaProviderResponsesStream, FunctionCallArgumentsRemainOwnedAndReplayInExactNativeGroup) {
    wire::Peer peer(tool_stream(), true);
    auto provider = wire::provider("openai.responses", peer.origin());
    auto request = wire::request("openai.responses", ProviderMode::Stream);
    std::get<sp::responses::Request>(request.payload).tools.push_back({"lookup_weather", "Weather lookup",
        test::document(R"({"type":"object","properties":{"city":{"type":"string"},"unit":{"type":"string"}},"required":["city","unit"],"additionalProperties":false})"), true});
    const auto first_request = request;
    const auto result = provider->invoke(std::move(request));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& completion = test::completion(result);
    ASSERT_EQ(completion.messages.size(), 1u);
    ASSERT_EQ(completion.messages[0].parts.size(), 1u);
    const auto& call = std::get<sp::ToolCall>(completion.messages[0].parts[0]);
    EXPECT_EQ(call.id, "call-1"); EXPECT_EQ(call.name, "lookup_weather");
    ASSERT_TRUE(call.input);
    EXPECT_EQ(call.input->root().get("city").as_string(), "Seoul");
    EXPECT_EQ(call.input->root().get("unit").as_string(), "C");
    ASSERT_TRUE(completion.messages[0].native);
    EXPECT_TRUE(completion.messages[0].native->complete());
    ASSERT_TRUE(completion.messages[0].wire_output);
    EXPECT_EQ(completion.messages[0].wire_output->root().at(0).get("arguments").as_string(), R"({"city":"Seoul","unit":"C"})");
    auto followup = first_request;
    auto& payload = std::get<sp::responses::Request>(followup.payload);
    payload.messages.push_back(completion.messages[0]);
    payload.messages.push_back(sp::Message{"", sp::Role::Tool, {sp::ToolResult{"call-1", "sunny"}}});
    auto prepared = provider->prepare(followup);
    ASSERT_TRUE(prepared.valid());
    const auto encoded = json::parse(std::string(prepared.encoded_body()));
    const auto& input = encoded.at("input");
    EXPECT_EQ(input.at(1).at("id"), "fc-item");
    EXPECT_EQ(input.at(1).at("call_id"), "call-1");
    EXPECT_EQ(input.at(2).at("type"), "function_call_output");
    EXPECT_EQ(input.at(2).at("call_id"), "call-1");
    EXPECT_EQ(input.at(2).at("output"), "sunny");
    std::get<sp::ToolCall>(payload.messages[1].parts[0]).id = "tampered";
    const auto rejected = provider->prepare(std::move(followup));
    ASSERT_FALSE(rejected.valid());
    ASSERT_NE(rejected.error(), nullptr);
    EXPECT_EQ(rejected.error()->kind, sp::ErrorKind::ReplayIneligible);
    EXPECT_EQ(peer.state->entered, 1u);
    {
        std::lock_guard lock(peer.state->mutex);
        peer.state->body = wire::responses_sse("sunny answer");
    }
    const auto next = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*next));
    EXPECT_EQ(test::text(next), "sunny answer");
    {
        std::lock_guard lock(peer.state->mutex);
        ASSERT_EQ(peer.state->requests.size(), 2u);
        EXPECT_EQ(peer.state->requests[1].at("input").at(1).at("id"), "fc-item");
        EXPECT_EQ(peer.state->requests[1].at("input").at(2).at("call_id"), "call-1");
    }
    EXPECT_EQ(call.input->root().get("city").as_string(), "Seoul");
}
