#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <core/native.h>

using namespace neograph;
namespace wire = neograph::test::wire;
namespace {
json mixed_response() {
    auto response = json::parse(wire::responses_body("ready"));
    response["output"].push_back({{"type", "image_generation_call"}, {"id", "img-7"},
        {"status", "completed"}, {"result", "aW1hZ2U="}, {"output_format", "png"}});
    response["output"].push_back({{"type", "function_call"}, {"id", "fc-7"}, {"call_id", "call-1"},
        {"name", "save"}, {"arguments", "{}"}, {"status", "completed"}});
    response["usage"] = {{"input_tokens", 3}, {"output_tokens", 4}, {"total_tokens", 7}};
    return response;
}
void expect_mixed(const sp::runtime::Result& result) {
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& completion = test::completion(result);
    ASSERT_EQ(completion.messages.size(), 1u);
    const auto& message = completion.messages[0];
    ASSERT_EQ(message.parts.size(), 3u);
    EXPECT_EQ(std::get<sp::Text>(message.parts[0]).value, "ready");
    const auto& image = std::get<sp::Opaque>(message.parts[1]);
    EXPECT_EQ(image.wire_type, "image_generation_call");
    ASSERT_TRUE(image.wire_metadata);
    EXPECT_EQ(image.wire_metadata->root().get("result").as_string(), "aW1hZ2U=");
    EXPECT_EQ(image.wire_metadata->root().get("id").as_string(), "img-7");
    EXPECT_EQ(std::get<sp::ToolCall>(message.parts[2]).name, "save");
    const auto& usage = completion.usage;
    ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.output_total);
    ASSERT_TRUE(usage.total); ASSERT_TRUE(usage.provider_reported_total);
    EXPECT_EQ(usage.input_total->value, 3u);
    EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.output_total->value, 4u);
    EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.total->value, 7u);
    EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
    EXPECT_EQ(usage.provider_reported_total->value, 7u);
    EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
    EXPECT_EQ(usage.stage, sp::UsageStage::Final);
    EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
    ASSERT_TRUE(message.native); EXPECT_TRUE(message.native->complete());
    ASSERT_TRUE(message.wire_output); EXPECT_EQ(message.wire_output->root().size(), 3u);
    EXPECT_EQ(message.wire_output->root().at(1).get("result").as_string(), "aW1hZ2U=");
    EXPECT_EQ(message.wire_output->root().at(2).get("call_id").as_string(), "call-1");
    ASSERT_FALSE(completion.raw_events.empty());
    ASSERT_TRUE(completion.raw_events.back().payload);
    const auto terminal = completion.raw_events.back().payload->root();
    const auto response = completion.raw_events.back().type == "response" ? terminal : terminal.get("response");
    EXPECT_EQ(response.get("output").at(1).get("result").as_string(), "aW1hZ2U=");
    EXPECT_EQ(response.get("output").at(2).get("call_id").as_string(), "call-1");
}
}

TEST(SchemaProviderMedia, ResponsesPreservesOrderedTextOpaqueImageAndClientTool) {
    sp::runtime::Result retained;
    { wire::Peer peer(mixed_response().dump());
      auto provider = wire::provider("openai.responses", peer.origin());
      retained = provider->invoke(wire::request("openai.responses")); }
    expect_mixed(retained);
}

TEST(SchemaProviderMedia, ResponsesStreamPreservesTerminalOpaqueArtifactsOnce) {
    const auto final = mixed_response();
    auto initial = final; initial["status"] = "in_progress"; initial["output"] = json::array();
    auto frame = [](std::string type, json body) { body["type"] = type; return "event: " + type + "\ndata: " + body.dump() + "\n\n"; };
    auto events = frame("response.created", {{"response", initial}});
    for (std::size_t i = 0; i < final.at("output").size(); ++i) {
        auto item = final.at("output").at(i);
        events += frame("response.output_item.added", {{"output_index", i}, {"item", item}});
        events += frame("response.output_item.done", {{"output_index", i}, {"item", item}});
    }
    events += frame("response.completed", {{"response", final}});
    wire::Peer peer(events, true);
    auto provider = wire::provider("openai.responses", peer.origin());
    expect_mixed(provider->invoke(wire::request("openai.responses", ProviderMode::Stream)));
}

TEST(SchemaProviderMedia, GeminiInlineMediaAndThoughtMetadataStayOrderedAndOwned) {
    const std::string body = R"({"candidates":[{"content":{"role":"model","parts":[{"text":"before"},{"inlineData":{"mimeType":"image/jpeg","data":"SlBFRw=="},"thoughtSignature":"media-seal","videoMetadata":{"startOffset":"1s"}},{"text":"after"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":0,"candidatesTokenCount":0,"totalTokenCount":0}})";
    for (const auto mode : {ProviderMode::Collect, ProviderMode::Stream}) for (const bool thoughts : {false, true}) {
        SCOPED_TRACE(mode == ProviderMode::Stream);
        SCOPED_TRACE(thoughts);
        auto response = json::parse(body);
        if (thoughts) response["usageMetadata"]["thoughtsTokenCount"] = 0;
        const auto payload = response.dump();
        sp::runtime::Result result;
        { wire::Peer peer(mode == ProviderMode::Stream ? "data: " + payload + "\n\n" : payload, mode == ProviderMode::Stream);
          auto provider = wire::provider("google.generate", peer.origin());
          result = provider->invoke(wire::request("google.generate", mode)); }
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        const auto& completion = test::completion(result);
        ASSERT_EQ(completion.messages.size(), 1u);
        const auto& message = completion.messages[0];
        ASSERT_EQ(message.parts.size(), 3u);
        EXPECT_EQ(std::get<sp::Text>(message.parts[0]).value, "before");
        const auto& media = std::get<sp::Opaque>(message.parts[1]);
        EXPECT_EQ(media.wire_type, "inlineData");
        ASSERT_TRUE(media.wire_metadata);
        EXPECT_EQ(media.wire_metadata->root().get("inlineData").get("data").as_string(), "SlBFRw==");
        EXPECT_EQ(media.wire_metadata->root().get("thoughtSignature").as_string(), "media-seal");
        EXPECT_EQ(media.wire_metadata->root().get("videoMetadata").get("startOffset").as_string(), "1s");
        EXPECT_EQ(std::get<sp::Text>(message.parts[2]).value, "after");
        const auto& usage = completion.usage;
        ASSERT_TRUE(usage.input_total); ASSERT_TRUE(usage.provider_reported_total);
        EXPECT_EQ(usage.input_total->value, 0u);
        EXPECT_EQ(usage.input_total->evidence, sp::Evidence::Reported);
        EXPECT_EQ(usage.provider_reported_total->value, 0u);
        EXPECT_EQ(usage.provider_reported_total->evidence, sp::Evidence::Reported);
        ASSERT_EQ(usage.output_total.has_value(), thoughts);
        ASSERT_EQ(usage.total.has_value(), thoughts);
        ASSERT_EQ(usage.reasoning.has_value(), thoughts);
        if (thoughts) {
            EXPECT_EQ(usage.reasoning->value, 0u);
            EXPECT_EQ(usage.reasoning->evidence, sp::Evidence::Reported);
            EXPECT_EQ(usage.output_total->value, 0u);
            EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Derived);
            EXPECT_EQ(usage.total->value, 0u);
            EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
        }
        EXPECT_EQ(usage.stage, sp::UsageStage::Final);
        EXPECT_EQ(usage.quality, sp::UsageQuality::Consistent);
        ASSERT_TRUE(message.wire_output);
        EXPECT_EQ(message.wire_output->root().at(1).get("thoughtSignature").as_string(), "media-seal");
        ASSERT_TRUE(message.native); EXPECT_TRUE(message.native->complete());
    }
}

TEST(SchemaProviderMedia, RealWireFailureRetainsOrderedPartialMediaWithoutReplayAuthority) {
    const std::string events =
        "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"text\":\"before\"},{\"inlineData\":{\"mimeType\":\"image/png\",\"data\":\"UE5H\"}},{\"text\":\"after\"}]}}],\"usageMetadata\":{\"promptTokenCount\":0}}\n\n"
        "data: {\"error\":{\"code\":500,\"message\":\"remote failure\"}}\n\n";
    sp::runtime::Result result;
    { wire::Peer peer(events, true); auto provider = wire::provider("google.generate", peer.origin());
      result = provider->invoke(wire::request("google.generate", ProviderMode::Stream)); }
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    const auto& failure = wire::failure(result);
    EXPECT_EQ(failure.error.kind, sp::ErrorKind::RemoteFailure);
    ASSERT_EQ(failure.partial.messages.size(), 1u);
    const auto& message = failure.partial.messages[0];
    ASSERT_EQ(message.parts.size(), 3u);
    EXPECT_EQ(std::get<sp::Text>(message.parts[0]).value, "before");
    ASSERT_TRUE(std::get<sp::Opaque>(message.parts[1]).wire_metadata);
    EXPECT_EQ(std::get<sp::Opaque>(message.parts[1]).wire_metadata->root().get("inlineData").get("data").as_string(), "UE5H");
    EXPECT_EQ(std::get<sp::Text>(message.parts[2]).value, "after");
    ASSERT_TRUE(failure.partial.usage.input_total); EXPECT_EQ(failure.partial.usage.input_total->value, 0u);
    EXPECT_EQ(failure.partial.usage.input_total->evidence, sp::Evidence::Reported);
    EXPECT_FALSE(failure.partial.usage.output_total);
    EXPECT_FALSE(failure.partial.usage.reasoning);
    EXPECT_FALSE(failure.partial.usage.total);
    EXPECT_FALSE(failure.partial.usage.provider_reported_total);
    EXPECT_EQ(failure.partial.usage.stage, sp::UsageStage::Partial);
    EXPECT_EQ(failure.partial.usage.quality, sp::UsageQuality::Consistent);
    EXPECT_FALSE(message.native && message.native->complete());
}

TEST(SchemaProviderMedia, HostedFileMetadataIsOwnedWithoutSyntheticDownloadAuthority) {
    auto response = json::parse(wire::responses_body("report ready"));
    response["output"].push_back({{"type", "file_search_call"}, {"id", "file-call"},
        {"status", "completed"}, {"results", json::array({{{"file_id", "file-91"},
            {"filename", "report.pdf"}, {"mime_type", "application/pdf"},
            {"url", "https://example.invalid/file/91"}, {"attributes", {{"size", 42}}}}})}});
    sp::runtime::Result result;
    { wire::Peer peer(response.dump()); auto provider = wire::provider("openai.responses", peer.origin());
      result = provider->invoke(wire::request("openai.responses")); }
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    const auto& message = test::completion(result).messages.at(0);
    ASSERT_EQ(message.parts.size(), 2u);
    const auto& file = std::get<sp::Opaque>(message.parts[1]);
    EXPECT_EQ(file.wire_type, "file_search_call");
    ASSERT_TRUE(file.wire_metadata);
    const auto metadata = file.wire_metadata->root().get("results").at(0);
    EXPECT_EQ(metadata.get("file_id").as_string(), "file-91");
    EXPECT_EQ(metadata.get("filename").as_string(), "report.pdf");
    EXPECT_EQ(metadata.get("mime_type").as_string(), "application/pdf");
    EXPECT_EQ(metadata.get("attributes").get("size").as_uint(), 42u);
    EXPECT_EQ(metadata.get("url").as_string(), "https://example.invalid/file/91");
    ASSERT_TRUE(message.native); EXPECT_TRUE(message.native->complete());
}

TEST(SchemaProviderMedia, NativeMixedModalityContinuationReplaysOpaqueItemWithoutFlattening) {
    wire::Peer peer(mixed_response().dump());
    auto provider = wire::provider("openai.responses", peer.origin());
    auto initial = wire::request("openai.responses");
    auto& initial_payload = std::get<sp::responses::Request>(initial.payload);
    initial_payload.tools.push_back({"save", "Save the generated image",
        test::document(R"({"type":"object","properties":{},"required":[],"additionalProperties":false})"), true});
    initial_payload.hosted_tools.push_back(sp::responses::ImageGenerationTool{});
    initial_payload.max_tool_calls = 1;
    const auto first = provider->invoke(initial);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*first));
    const auto& original = test::completion(first).messages.at(0);
    auto followup = initial;
    auto& payload = std::get<sp::responses::Request>(followup.payload);
    payload.messages.push_back(original);
    payload.messages.push_back(sp::Message{"", sp::Role::Tool, {sp::ToolResult{"call-1", "saved"}}});
    auto prepared = provider->prepare(std::move(followup));
    ASSERT_TRUE(prepared.valid());
    {
        std::lock_guard lock(peer.state->mutex);
        peer.state->body = wire::responses_body("stored");
    }
    const auto second = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*second));
    EXPECT_EQ(test::text(second), "stored");
    {
        std::lock_guard lock(peer.state->mutex);
        ASSERT_EQ(peer.state->requests.size(), 2u);
        const auto input = peer.state->requests[1].at("input");
        EXPECT_EQ(input.at(2).at("type"), "image_generation_call");
        EXPECT_EQ(input.at(2).at("id"), "img-7");
        EXPECT_EQ(input.at(2).at("result"), "aW1hZ2U=");
        EXPECT_EQ(input.at(3).at("call_id"), "call-1");
        EXPECT_EQ(input.at(4).at("type"), "function_call_output");
        EXPECT_EQ(input.at(4).at("output"), "saved");
    }
    EXPECT_EQ(std::get<sp::Opaque>(original.parts.at(1)).wire_metadata->root().get("result").as_string(), "aW1hZ2U=");
    ASSERT_TRUE(original.native); EXPECT_TRUE(original.native->complete());
}
