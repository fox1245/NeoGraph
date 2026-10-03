#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;

TEST(SchemaProviderTypedControls, CallerTemperatureAndOutputCapAreNotClamped) {
    auto provider = wire::provider("openai.chat", "http://127.0.0.1:1");
    auto request = wire::request(); auto& payload = std::get<sp::chat::Request>(request.payload);
    payload.temperature = 0.3; payload.max_output_tokens = 1234;
    const auto prepared = provider->prepare(std::move(request)); ASSERT_TRUE(prepared.valid());
    const auto body = json::parse(std::string(prepared.encoded_body()));
    EXPECT_DOUBLE_EQ(body.at("temperature").get<double>(), 0.3);
    EXPECT_EQ(body.at("max_tokens"), 1234);
}

TEST(SchemaProviderTypedControls, UnsupportedReasoningAndTemperatureRejectBeforeDispatch) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    for (const bool reasoning : {false, true}) {
        auto request = wire::request(); auto& payload = std::get<sp::chat::Request>(request.payload);
        if (reasoning) payload.reasoning_effort = "unsupported_effort"; else payload.temperature = -1.0;
        auto prepared = provider->prepare(std::move(request));
        ASSERT_FALSE(prepared.valid()); ASSERT_NE(prepared.error(), nullptr);
        EXPECT_EQ(prepared.error()->kind, sp::ErrorKind::InvalidRequest);
    }
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(SchemaProviderTypedControls, RoutingDefaultsAndPerCallReplacementRemainExplicit) {
    sp::OpenRouterRouting defaults; defaults.zdr = true; defaults.only = {"default-provider"}; defaults.allow_fallbacks = false;
    llm::SchemaProvider::Defaults controls; controls.provider = defaults;
    const auto source = json{{"descriptor_version", 1}, {"revision", 1},
        {"id", "router-controls"}, {"family", "openai.chat"},
        {"connection", {{"base_url", "https://openrouter.ai"},
            {"paths", {{"buffered", "/api/v1/chat/completions"},
                       {"streaming", "/api/v1/chat/completions"}}}}}}.dump();
    auto admitted = sp::descriptor::load(source);
    ASSERT_TRUE(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(admitted));
    auto provider = llm::SchemaProvider::create(
        std::get<sp::descriptor::ValidatedDescriptor>(std::move(admitted)), {}, controls);
    auto prepared = provider->prepare(wire::request()); ASSERT_TRUE(prepared.valid());
    auto body = json::parse(std::string(prepared.encoded_body()));
    EXPECT_EQ(body.at("provider").at("zdr"), true);
    EXPECT_EQ(body.at("provider").at("only").at(0), "default-provider");
    auto request = wire::request(); sp::OpenRouterRouting override;
    override.zdr = true; override.only = {"morph"}; override.allow_fallbacks = false;
    std::get<sp::chat::Request>(request.payload).provider = override;
    auto replacement = provider->prepare(std::move(request)); ASSERT_TRUE(replacement.valid());
    body = json::parse(std::string(replacement.encoded_body()));
    EXPECT_EQ(body.at("provider").at("only"), json::array({"morph"}));
    EXPECT_EQ(body.at("provider").at("allow_fallbacks"), false);
}

TEST(SchemaProviderTypedControls, RoutingOnUnadmittedOriginNeverLeaksToWire) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request(); sp::OpenRouterRouting routing; routing.zdr = true;
    std::get<sp::chat::Request>(request.payload).provider = routing;
    auto prepared = provider->prepare(std::move(request));
    ASSERT_FALSE(prepared.valid()); EXPECT_EQ(peer.state->entered, 0u);
}

TEST(SchemaProviderTypedControls, StructuredOutputUsesOnlyDeclaredTypedControls) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request(); sp::ResponseFormat format;
    format.kind = sp::ResponseFormat::Kind::JsonSchema; format.name = "weather"; format.strict = true;
    format.schema = test::document(R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})");
    std::get<sp::chat::Request>(request.payload).response_format = format;
    auto prepared = provider->prepare(std::move(request)); ASSERT_TRUE(prepared.valid());
    auto body = json::parse(std::string(prepared.encoded_body()));
    EXPECT_EQ(body.at("response_format").at("type"), "json_schema");
    EXPECT_EQ(body.at("response_format").at("json_schema").at("name"), "weather");
    EXPECT_EQ(body.at("response_format").at("json_schema").at("strict"), true);
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(SchemaProviderTypedControls, MalformedStructuredOutputRejectsInsteadOfBeingDropped) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request(); sp::ResponseFormat format;
    format.kind = sp::ResponseFormat::Kind::JsonSchema; format.name = "weather";
    format.schema = test::document(R"({"type":"object","required":false})");
    std::get<sp::chat::Request>(request.payload).response_format = format;
    const auto prepared = provider->prepare(std::move(request));
    ASSERT_FALSE(prepared.valid()); EXPECT_EQ(peer.state->entered, 0u);
}

TEST(SchemaProviderTypedControls, ClosedDescriptorRejectsInterpreterExtensionFields) {
    auto loaded = sp::descriptor::load(R"({"descriptor_version":1,"revision":1,"id":"closed","family":"openai.chat","connection":{"base_url":"https://fixture.invalid","paths":{"buffered":"/chat","streaming":"/chat"}},"request":{"extra_fields":{"unchecked":true}}})");
    ASSERT_TRUE(std::holds_alternative<sp::descriptor::ConfigError>(loaded));
}
