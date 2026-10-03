#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <array>

using namespace neograph;
namespace wire = neograph::test::wire;
namespace {
constexpr std::array<const char*, 5> families = {"openai.chat", "anthropic.messages", "openai.responses", "google.generate", "google.interactions"};
void add_weather_tool(ProviderRequest& request) {
    const auto schema = test::document(R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})");
    std::visit([&](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, sp::messages::Request>) payload.tools.push_back({"lookup_weather", "Weather lookup", schema, "", {}});
        else if constexpr (std::is_same_v<T, sp::responses::Request>) payload.tools.push_back({"lookup_weather", "Weather lookup", schema, true});
        else payload.tools.push_back({"lookup_weather", "Weather lookup", schema});
    }, request.payload);
}
}

TEST(SchemaProviderWireContract, TypedToolsEncodeAcrossAllFiveFamiliesBeforeDispatch) {
    for (auto family : families) {
        SCOPED_TRACE(family);
        auto provider = wire::provider(family, "http://127.0.0.1:1");
        auto request = wire::request(family);
        add_weather_tool(request);
        auto prepared = provider->prepare(std::move(request));
        ASSERT_TRUE(prepared.valid());
        const auto body = json::parse(std::string(prepared.encoded_body()));
        EXPECT_EQ(prepared.model(), "fixture-model");
        if (std::string_view(family) == "google.generate") {
            EXPECT_FALSE(body.contains("model"));
            ASSERT_NE(prepared.admitted_descriptor(), nullptr);
            EXPECT_EQ(prepared.admitted_descriptor()->path(false), "/v1beta/models/fixture-model:generateContent");
            EXPECT_EQ(prepared.admitted_descriptor()->path(true), "/v1beta/models/fixture-model:streamGenerateContent?alt=sse");
        } else {
            EXPECT_EQ(body.at("model"), "fixture-model");
        }
        ASSERT_EQ(body.at("tools").size(), 1u);
        auto tool = body.at("tools").at(0);
        std::string schema_member = "parameters";
        if (std::string_view(family) == "openai.chat") {
            EXPECT_EQ(tool.at("type"), "function");
            tool = tool.at("function");
        } else if (std::string_view(family) == "anthropic.messages") {
            schema_member = "input_schema";
        } else if (std::string_view(family) == "google.generate") {
            ASSERT_EQ(tool.at("functionDeclarations").size(), 1u);
            tool = tool.at("functionDeclarations").at(0);
            schema_member = "parametersJsonSchema";
        } else {
            EXPECT_EQ(tool.at("type"), "function");
        }
        EXPECT_EQ(tool.at("name"), "lookup_weather");
        EXPECT_EQ(tool.at("description"), "Weather lookup");
        EXPECT_EQ(tool.at(schema_member).at("properties").at("city").at("type"), "string");
        EXPECT_EQ(tool.at(schema_member).at("required"), json::array({"city"}));
        EXPECT_EQ(prepared.family(), family);
    }
}

TEST(SchemaProviderWireContract, TypedToolResponsesRemainOwnedAcrossProviders) {
    struct Case { const char* family; const char* response; const char* id; };
    const std::array cases = {
        Case{"openai.chat", R"({"choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call-1","type":"function","function":{"name":"lookup_weather","arguments":"{\"city\":\"Seoul\"}"}}]},"finish_reason":"tool_calls"}]})", "call-1"},
        Case{"anthropic.messages", R"({"id":"msg-1","type":"message","model":"fixture-model","role":"assistant","content":[{"type":"text","text":"checking"},{"type":"tool_use","id":"tool-1","name":"lookup_weather","input":{"city":"Seoul"}}],"stop_reason":"tool_use","usage":{"input_tokens":3,"output_tokens":2}})", "tool-1"},
        Case{"google.generate", R"({"candidates":[{"content":{"role":"model","parts":[{"text":"checking"},{"functionCall":{"id":"gemini-1","name":"lookup_weather","args":{"city":"Seoul"}}}]},"finishReason":"STOP"}]})", "gemini-1"}
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.family);
        sp::runtime::Result result;
        { const auto response = std::string_view(item.family) == "openai.chat"
              ? wire::chat_envelope(json::parse(item.response)).dump() : std::string(item.response);
          wire::Peer peer(response); auto provider = wire::provider(item.family, peer.origin());
          auto request = wire::request(item.family);
          add_weather_tool(request);
          result = provider->invoke(std::move(request)); }
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto& completion = test::completion(result);
        std::vector<sp::ToolCall> calls;
        for (const auto& message : completion.messages) for (const auto& part : message.parts)
            if (const auto* call = std::get_if<sp::ToolCall>(&part)) calls.push_back(*call);
        ASSERT_EQ(calls.size(), 1u);
        EXPECT_EQ(calls[0].name, "lookup_weather");
        EXPECT_EQ(calls[0].id, item.id);
        ASSERT_TRUE(calls[0].input);
        EXPECT_EQ(calls[0].input->root().get("city").as_string(), "Seoul");
    }
}

TEST(SchemaProviderWireContract, CanonicalInlineVisionRetainsOrderAcrossFamilies) {
    const auto bytes = std::make_shared<const std::string>("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a2ioAAAAASUVORK5CYII=");
    for (auto family : families) {
        SCOPED_TRACE(family);
        auto provider = wire::provider(family, "http://127.0.0.1:1");
        auto request = wire::request(family);
        set_provider_request_messages(request, {sp::Message{"", sp::Role::User,
            {sp::Text{"before-image"}, sp::Image{"image/png", bytes}, sp::Text{"after-image"}}}});
        const auto prepared = provider->prepare(std::move(request));
        ASSERT_TRUE(prepared.valid());
        const auto body = std::string(prepared.encoded_body());
        const auto first = body.find("before-image"), image = body.find(*bytes), last = body.find("after-image");
        ASSERT_NE(first, std::string::npos); ASSERT_NE(image, std::string::npos); ASSERT_NE(last, std::string::npos);
        EXPECT_LT(first, image); EXPECT_LT(image, last);
    }
}

TEST(SchemaProviderWireContract, InvalidInlineVisionRejectsBeforeAnyWireActivity) {
    wire::Peer peer(wire::chat_response());
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request();
    std::get<sp::chat::Request>(request.payload).canonical_messages[0].parts.push_back(
        sp::Image{"image/png", std::make_shared<const std::string>("not base64")});
    const auto prepared = provider->prepare(std::move(request));
    ASSERT_FALSE(prepared.valid()); ASSERT_NE(prepared.error(), nullptr);
    EXPECT_EQ(prepared.error()->kind, sp::ErrorKind::InvalidRequest);
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(SchemaProviderWireContract, StreamingRequestPreparationDoesNotDispatch) {
    wire::Peer peer(wire::chat_sse(), true);
    auto provider = wire::provider("openai.chat", peer.origin());
    auto collect = provider->prepare(wire::request());
    auto stream = provider->prepare(wire::request("openai.chat", ProviderMode::Stream));
    ASSERT_TRUE(collect.valid()); ASSERT_TRUE(stream.valid());
    const auto body = json::parse(std::string(stream.encoded_body()));
    EXPECT_EQ(body.at("stream"), true);
    EXPECT_EQ(body.at("stream_options").at("include_usage"), true);
    EXPECT_EQ(peer.state->entered, 0u);
    EXPECT_EQ(test::text(provider->dispatch(std::move(stream))), "pong");
    EXPECT_EQ(peer.state->entered, 1u);
}

TEST(SchemaProviderWireContract, ChatReasoningAndClientToolRemainDistinctOwnedParts) {
    wire::Peer peer(wire::chat_envelope(json::parse(R"({"choices":[{"index":0,"message":{"role":"assistant","content":null,"reasoning_content":"Need the approved lookup.","tool_calls":[{"id":"call-glm","type":"function","function":{"name":"lookup_weather","arguments":"{\"city\":\"Seoul\"}"}}]},"finish_reason":"tool_calls"}]})")).dump());
    auto provider = wire::provider("openai.chat", peer.origin());
    const auto result = provider->invoke(wire::request());
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& completion = test::completion(result);
    ASSERT_EQ(completion.messages.size(), 1u);
    ASSERT_EQ(completion.messages[0].parts.size(), 2u);
    EXPECT_EQ(std::get<sp::Thinking>(completion.messages[0].parts[0]).text, "Need the approved lookup.");
    const auto& call = std::get<sp::ToolCall>(completion.messages[0].parts[1]);
    EXPECT_EQ(call.id, "call-glm"); EXPECT_EQ(call.name, "lookup_weather");
    EXPECT_EQ(call.input->root().get("city").as_string(), "Seoul");
    EXPECT_EQ(completion.stop.kind, sp::StopKind::ToolUse);
    EXPECT_TRUE(test::text(result).empty());
}
