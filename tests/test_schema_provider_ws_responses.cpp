// Tests for SchemaProvider's use_websocket dispatch path.
//
// Two layers of coverage:
//   1. Misconfiguration guard — runs everywhere. Setting use_websocket
//      with a non-Responses schema (e.g. plain "openai") throws on
//      complete_stream rather than silently making an HTTP request.
//      Catches refactor regressions to the dispatch branch in
//      complete_stream.
//
//   2. Live OpenAI round-trip — gated on `NEOGRAPH_LIVE_OPENAI=1`
//      and `OPENAI_API_KEY`. Connects to wss://api.openai.com and
//      drives one response.create → response.completed cycle, asserts
//      content non-empty and usage tokens > 0. This is the only test
//      that actually exercises the OpenAI WS endpoint; CI runs it
//      manually, never on the offline default ctest.

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#include <cstdlib>
#include <string>

using namespace neograph;

TEST(SchemaProviderWs, ThrowsForNonResponsesSchema) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path   = "openai";       // chat-completions, NOT responses
    cfg.api_key       = "sk-fake-key";  // unused, never reaches the wire
    cfg.default_model = "gpt-4o-mini";
    cfg.use_websocket = true;

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.messages.push_back({"user", "hi"});

    EXPECT_THROW(
        provider->complete_stream(params, [](const std::string&){}),
        std::runtime_error);
}

TEST(SchemaProviderWs, LiveRoundTripIfEnabled) {
    if (const char* gate = std::getenv("NEOGRAPH_LIVE_OPENAI");
        !gate || std::string(gate) != "1") {
        GTEST_SKIP() << "NEOGRAPH_LIVE_OPENAI != 1, skipping live OpenAI test";
    }
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        GTEST_SKIP() << "OPENAI_API_KEY not set, skipping live OpenAI test";
    }

    llm::SchemaProvider::Config cfg;
    cfg.schema_path   = "openai_responses";
    cfg.api_key       = key;
    cfg.default_model = "gpt-4o-mini";
    cfg.use_websocket = true;
    cfg.timeout_seconds = 60;

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.messages.push_back(
        {"user", "Reply with the single word: pong."});
    params.max_tokens = 16;
    // No temperature override — the provider must strip it internally
    // for the WS path. Default CompletionParams.temperature=0.7f used
    // to make OpenAI close the socket with code=1000 + zero events.

    std::string streamed;
    auto completion = provider->complete_stream(
        params, [&](const std::string& tok) { streamed += tok; });

    EXPECT_FALSE(completion.message.content.empty())
        << "expected non-empty assistant content";
    EXPECT_EQ(completion.message.content, streamed)
        << "streamed tokens should reassemble into the final content";
    EXPECT_GT(completion.usage.prompt_tokens, 0)
        << "response.completed should populate prompt_tokens";
    EXPECT_GT(completion.usage.completion_tokens, 0)
        << "response.completed should populate completion_tokens";
}
