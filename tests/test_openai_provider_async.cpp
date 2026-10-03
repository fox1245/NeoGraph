#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;
namespace {
sp::runtime::Result stream(std::string body, sp::runtime::Options options = {}, int status = 200) {
    wire::Peer peer(std::move(body), true); peer.state->status = status;
    auto provider = wire::provider("openai.chat", peer.origin(), std::move(options));
    return provider->invoke(wire::request("openai.chat", ProviderMode::Stream));
}
}

TEST(SchemaProviderChatAsync, InvokeAsyncReturnsFullOwnedOutcome) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request(); asio::io_context io;
    auto result = asio::co_spawn(io, provider->invoke_async(std::move(request)), asio::use_future);
    io.run(); const auto outcome = result.get(); provider.reset();
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*outcome));
    EXPECT_EQ(test::text(outcome), "pong");
    const auto& completion = test::completion(outcome);
    EXPECT_EQ(completion.stop.kind, sp::StopKind::EndTurn);
    EXPECT_EQ(completion.usage.input_total->value, 3u);
    EXPECT_EQ(completion.usage.output_total->value, 2u);
    EXPECT_EQ(completion.usage.total->value, 5u);
}

TEST(SchemaProviderChatAsync, SyncInvokeBridgesThroughRunSync) {
    wire::Peer peer(wire::chat_response()); auto provider = wire::provider("openai.chat", peer.origin());
    EXPECT_EQ(test::text(provider->invoke(wire::request())), "pong");
    EXPECT_EQ(peer.state->entered, 1u);
}

TEST(SchemaProviderChatAsync, HttpFailuresRemainTypedAndRetainStatus) {
    for (const auto& [status, kind] : std::vector<std::pair<int, sp::ErrorKind>>{
        {401, sp::ErrorKind::Authentication}, {403, sp::ErrorKind::Permission},
        {429, sp::ErrorKind::LimitUnknown}, {500, sp::ErrorKind::Overloaded}}) {
        SCOPED_TRACE(status);
        wire::Peer peer(R"({"error":{"message":"private-server-error"}})"); peer.state->status = status;
        auto provider = wire::provider("openai.chat", peer.origin());
        const auto outcome = provider->invoke(wire::request());
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
        EXPECT_EQ(wire::failure(outcome).error.kind, kind);
        EXPECT_EQ(wire::failure(outcome).error.http_status, status);
        EXPECT_EQ(peer.state->entered, 1u);
    }
}

TEST(SchemaProviderChatStream, TopLevelErrorIsNotAnEmptySuccess) {
    const auto result = stream("data: {\"error\":{\"message\":\"exploded\"}}\n\ndata: [DONE]\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::RemoteFailure);
}

TEST(SchemaProviderChatStream, ErrorAfterContentPreservesPartialCompletion) {
    const auto result = stream(wire::chat_frame(json::parse(R"({"choices":[{"index":0,"delta":{"role":"assistant","content":"partial"},"finish_reason":null}]})"))
        + "data: {\"error\":{\"message\":\"exploded\"}}\n\ndata: [DONE]\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    const auto& failure = wire::failure(result);
    EXPECT_EQ(failure.error.kind, sp::ErrorKind::RemoteFailure);
    ASSERT_EQ(failure.partial.messages.size(), 1u);
    ASSERT_EQ(failure.partial.messages[0].parts.size(), 1u);
    EXPECT_EQ(std::get<sp::Text>(failure.partial.messages[0].parts[0]).value, "partial");
    EXPECT_FALSE(failure.partial.usage.input_total);
    EXPECT_FALSE(failure.partial.usage.output_total);
}

TEST(SchemaProviderChatStream, MalformedDataIsNotSilentlySkipped) {
    const auto result = stream("data: {not-json}\n\ndata: [DONE]\n\n");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ProtocolCorrupt);
}

TEST(SchemaProviderChatStream, UnterminatedDataCannotInventSuccess) {
    const auto result = stream("data: {\"choices\":[]}");
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::Truncated);
}

TEST(SchemaProviderChatStream, RejectsOversizedSseLine) {
    sp::runtime::Options options; options.limits.sse.max_line_bytes = 24;
    const auto result = stream(wire::chat_sse(), std::move(options));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ResourceLimit);
}

TEST(SchemaProviderChatStream, RejectsOversizedAggregateStream) {
    sp::runtime::Options options; options.limits.sse.max_total_bytes = 24;
    const auto result = stream(wire::chat_sse(), std::move(options));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::ResourceLimit);
}

TEST(SchemaProviderChatStream, SuccessfulContentAndUsageRemainIntact) {
    const auto result = stream(wire::chat_sse());
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(test::text(result), "pong");
    EXPECT_EQ(test::completion(result).usage.total->value, 5u);
}

TEST(SchemaProviderChatStream, EmptyTextWithAuthoritativeTerminalDoesNotInventText) {
    const auto result = stream(wire::chat_sse(""));
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
    EXPECT_TRUE(test::text(result).empty());
    EXPECT_EQ(test::completion(result).stop.kind, sp::StopKind::EndTurn);
}

TEST(SchemaProviderChatAsync, AdmittedVersionedEndpointsDispatchExactPathAndCredential) {
    for (const auto mode : {ProviderMode::Collect, ProviderMode::Stream}) {
        SCOPED_TRACE(mode == ProviderMode::Stream);
        wire::Peer peer(mode == ProviderMode::Stream ? wire::chat_sse() : wire::chat_response(),
                        mode == ProviderMode::Stream);
        const auto source = json{{"descriptor_version", 1}, {"revision", 1},
            {"id", "explicit-endpoint"}, {"family", "openai.chat"},
            {"connection", {{"base_url", peer.origin()},
                {"paths", {{"buffered", "/v1/chat/completions"},
                           {"streaming", "/v1/chat/completions"}}}}}}.dump();
        auto admitted = sp::descriptor::load(source);
        ASSERT_TRUE(std::holds_alternative<sp::descriptor::ValidatedDescriptor>(admitted));
        sp::runtime::Options options; options.api_key = "fixture-key"; options.retry_tokens = 0;
        auto provider = llm::SchemaProvider::create(std::get<sp::descriptor::ValidatedDescriptor>(std::move(admitted)), options);
        const auto result = provider->invoke(wire::request("openai.chat", mode));
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
        std::lock_guard lock(peer.state->mutex);
        ASSERT_EQ(peer.state->paths.size(), 1u);
        EXPECT_EQ(peer.state->paths[0], "/v1/chat/completions");
        EXPECT_EQ(peer.state->authorization[0], "Bearer fixture-key");
    }
}

TEST(SchemaProviderChatAsync, AwaitableOwnsRealHttpDispatchAfterProviderDestruction) {
    wire::Peer peer(wire::chat_response("owned after destruction"));
    auto provider = wire::provider("openai.chat", peer.origin());
    auto work = provider->invoke_async(wire::request());
    provider.reset();
    asio::io_context io;
    auto result = asio::co_spawn(io, std::move(work), asio::use_future);
    io.run();
    const auto outcome = result.get();
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*outcome));
    EXPECT_EQ(test::text(outcome), "owned after destruction");
    EXPECT_EQ(peer.state->entered, 1u);
}
