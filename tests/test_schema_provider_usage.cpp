// Token usage mapping per built-in schema: prompt/completion/total plus the
// cached-prompt and reasoning subsets, for non-stream, SSE and WebSocket.
//
// Vendors disagree about what the basic counters include, so the mapping is
// schema data (`response.prompt_extra_fields`, `cached_tokens_path`,
// `reasoning_tokens_path`, `completion_includes_reasoning`):
//   * Anthropic reports `input_tokens` WITHOUT the cached prefix; the real
//     prompt is input + cache_read + cache_creation, and the cached part is
//     cache_read. (Live: input_tokens=3, cache_read_input_tokens=9818.)
//   * OpenAI chat / Responses report cached and reasoning tokens inside
//     `*_tokens_details`; both are subsets of prompt / completion.
//   * Gemini's `candidatesTokenCount` EXCLUDES thoughts (`thoughtsTokenCount`);
//     completion must be everything the model produced.
//   * Streams: Anthropic sends input at message_start and output at
//     message_delta and never a total.

#include <gtest/gtest.h>

#include <neograph/llm/openai_provider.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using neograph::ChatCompletion;
using neograph::CompletionParams;
using neograph::json;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

struct OneBodyServer {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::string body;

    explicit OneBodyServer(std::string b) : body(std::move(b)) {
        svr.Post(R"(.*)", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(body, "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~OneBodyServer() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

CompletionParams params() {
    CompletionParams p;
    p.model = "test-model";
    p.messages.push_back({"user", "hi"});
    return p;
}

std::unique_ptr<SchemaProvider> builtin(const std::string& name, const OneBodyServer* server = nullptr) {
    SchemaProvider::Config cfg;
    cfg.schema_path = name;
    cfg.api_key = "test-key";
    if (server != nullptr) {
        cfg.base_url_override = server->base_url();
        cfg.allow_insecure_loopback = true;
    }
    auto sp = SchemaProvider::create(cfg);
    EXPECT_NE(sp, nullptr);
    return sp;
}

ChatCompletion::Usage non_stream_usage(const std::string& schema, const json& body) {
    OneBodyServer server(body.dump());
    return builtin(schema, &server)->complete(params()).usage;
}

std::vector<std::string> sse_events(const std::vector<std::pair<std::string, json>>& events) {
    std::vector<std::string> lines;
    for (const auto& [name, payload] : events) {
        lines.push_back("event: " + name);
        lines.push_back("data: " + payload.dump());
        lines.push_back("");
    }
    return lines;
}

std::vector<std::string> sse_data(const std::vector<json>& chunks, bool done = false) {
    std::vector<std::string> lines;
    for (const auto& chunk : chunks) {
        lines.push_back("data: " + chunk.dump());
        lines.push_back("");
    }
    if (done) {
        lines.push_back("data: [DONE]");
        lines.push_back("");
    }
    return lines;
}

}  // namespace

// ─── non-stream ───

TEST(SchemaProviderUsage, ClaudePromptIncludesTheCachedPrefix) {
    const auto usage = non_stream_usage(
        "claude", {{"role", "assistant"},
                   {"content", json::array({{{"type", "text"}, {"text", "ok"}}})},
                   {"stop_reason", "end_turn"},
                   {"usage",
                    {{"input_tokens", 12},
                     {"cache_read_input_tokens", 900},
                     {"cache_creation_input_tokens", 300},
                     {"output_tokens", 40}}}});
    EXPECT_EQ(usage.prompt_tokens, 1212);      // 12 + 900 + 300
    EXPECT_EQ(usage.cached_prompt_tokens, 900);  // only what was READ from cache
    EXPECT_EQ(usage.completion_tokens, 40);
    EXPECT_EQ(usage.total_tokens, 1252);
    EXPECT_EQ(usage.reasoning_tokens, 0);
}

TEST(SchemaProviderUsage, ClaudeWithoutCacheFieldsIsUnchanged) {
    const auto usage = non_stream_usage(
        "claude", {{"role", "assistant"},
                   {"content", json::array({{{"type", "text"}, {"text", "ok"}}})},
                   {"stop_reason", "end_turn"},
                   {"usage", {{"input_tokens", 7}, {"output_tokens", 5}}}});
    EXPECT_EQ(usage.prompt_tokens, 7);
    EXPECT_EQ(usage.cached_prompt_tokens, 0);
    EXPECT_EQ(usage.total_tokens, 12);
}

TEST(SchemaProviderUsage, OpenAIChatReadsCachedAndReasoningSubsets) {
    const auto usage = non_stream_usage(
        "openai",
        {{"choices", json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}},
                                   {"finish_reason", "stop"}}})},
         {"usage",
          {{"prompt_tokens", 100},
           {"completion_tokens", 80},
           {"total_tokens", 180},
           {"prompt_tokens_details", {{"cached_tokens", 64}}},
           {"completion_tokens_details", {{"reasoning_tokens", 50}}}}}});
    EXPECT_EQ(usage.prompt_tokens, 100);
    EXPECT_EQ(usage.cached_prompt_tokens, 64);
    EXPECT_EQ(usage.completion_tokens, 80);
    EXPECT_EQ(usage.reasoning_tokens, 50);
    EXPECT_EQ(usage.total_tokens, 180);
}

TEST(SchemaProviderUsage, ResponsesReadsCachedAndReasoningSubsets) {
    const auto usage = non_stream_usage(
        "openai_responses",
        {{"status", "completed"},
         {"output", json::array({{{"type", "message"},
                                  {"content", json::array({{{"type", "output_text"}, {"text", "ok"}}})}}})},
         {"usage",
          {{"input_tokens", 3300},
           {"output_tokens", 100},
           {"total_tokens", 3400},
           {"input_tokens_details", {{"cached_tokens", 3200}}},
           {"output_tokens_details", {{"reasoning_tokens", 64}}}}}});
    EXPECT_EQ(usage.prompt_tokens, 3300);
    EXPECT_EQ(usage.cached_prompt_tokens, 3200);
    EXPECT_EQ(usage.completion_tokens, 100);
    EXPECT_EQ(usage.reasoning_tokens, 64);
    EXPECT_EQ(usage.total_tokens, 3400);
}

TEST(SchemaProviderUsage, GeminiCompletionIncludesThoughts) {
    // Live: candidatesTokenCount 3 while ~269 tokens were billed.
    const auto usage = non_stream_usage(
        "gemini",
        {{"candidates", json::array({{{"content", {{"parts", json::array({{{"text", "ok"}}})}}},
                                      {"finishReason", "STOP"}}})},
         {"usageMetadata",
          {{"promptTokenCount", 20},
           {"candidatesTokenCount", 3},
           {"thoughtsTokenCount", 266},
           {"cachedContentTokenCount", 10},
           {"totalTokenCount", 289}}}});
    EXPECT_EQ(usage.prompt_tokens, 20);
    EXPECT_EQ(usage.cached_prompt_tokens, 10);
    EXPECT_EQ(usage.completion_tokens, 269);  // 3 visible + 266 thoughts
    EXPECT_EQ(usage.reasoning_tokens, 266);
    EXPECT_EQ(usage.total_tokens, 289);
}

TEST(SchemaProviderUsage, NativeOpenAIProviderReadsTheSameSubsets) {
    OneBodyServer server(
        json({{"choices", json::array({{{"message", {{"role", "assistant"}, {"content", "ok"}}},
                                        {"finish_reason", "stop"}}})},
              {"usage",
               {{"prompt_tokens", 100},
                {"completion_tokens", 80},
                {"total_tokens", 180},
                {"prompt_tokens_details", {{"cached_tokens", 64}}},
                {"completion_tokens_details", {{"reasoning_tokens", 50}}}}}})
            .dump());
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = "test-key";
    cfg.base_url = server.base_url();
    cfg.allow_insecure_loopback = true;
    const auto usage = neograph::llm::OpenAIProvider::create(cfg)->complete(params()).usage;
    EXPECT_EQ(usage.prompt_tokens, 100);
    EXPECT_EQ(usage.cached_prompt_tokens, 64);
    EXPECT_EQ(usage.reasoning_tokens, 50);
    EXPECT_EQ(usage.total_tokens, 180);
}

// ─── streaming ───

TEST(SchemaProviderUsageStream, ClaudeCombinesMessageStartAndDeltaAndDerivesTheTotal) {
    auto sp = builtin("claude");
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp,
        sse_events(
            {{"message_start",
              {{"type", "message_start"},
               {"message",
                {{"usage",
                  {{"input_tokens", 12},
                   {"cache_read_input_tokens", 900},
                   {"cache_creation_input_tokens", 300}}}}}}},
             {"message_delta",
              {{"type", "message_delta"},
               {"delta", {{"stop_reason", "end_turn"}}},
               {"usage", {{"output_tokens", 40}}}}},
             {"message_stop", {{"type", "message_stop"}}}}));
    EXPECT_EQ(completion.usage.prompt_tokens, 1212);
    EXPECT_EQ(completion.usage.cached_prompt_tokens, 900);
    EXPECT_EQ(completion.usage.completion_tokens, 40);
    EXPECT_EQ(completion.usage.total_tokens, 1252) << "Anthropic streams never report a total";
}

namespace {
json responses_completed_with_details() {
    return {{"type", "response.completed"},
            {"response",
             {{"status", "completed"},
              {"usage",
               {{"input_tokens", 3300},
                {"output_tokens", 100},
                {"total_tokens", 3400},
                {"input_tokens_details", {{"cached_tokens", 3200}}},
                {"output_tokens_details", {{"reasoning_tokens", 64}}}}}}}};
}
}  // namespace

TEST(SchemaProviderUsageStream, ResponsesSseCarriesCachedAndReasoning) {
    auto sp = builtin("openai_responses");
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data({responses_completed_with_details()}));
    EXPECT_EQ(completion.usage.prompt_tokens, 3300);
    EXPECT_EQ(completion.usage.cached_prompt_tokens, 3200);
    EXPECT_EQ(completion.usage.completion_tokens, 100);
    EXPECT_EQ(completion.usage.reasoning_tokens, 64);
    EXPECT_EQ(completion.usage.total_tokens, 3400);
}

TEST(SchemaProviderUsageStream, ResponsesWebSocketCarriesCachedAndReasoning) {
    auto sp = builtin("openai_responses");
    const auto completion = SchemaProviderTestAccess::parse_ws_events(
        *sp, {responses_completed_with_details()});
    EXPECT_EQ(completion.usage.prompt_tokens, 3300);
    EXPECT_EQ(completion.usage.cached_prompt_tokens, 3200);
    EXPECT_EQ(completion.usage.reasoning_tokens, 64);
    EXPECT_EQ(completion.usage.total_tokens, 3400);
}

TEST(SchemaProviderUsageStream, GeminiLatestChunkWinsAndThoughtsJoinTheCompletion) {
    auto sp = builtin("gemini");
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp,
        sse_data(
            {{{"candidates", json::array({{{"content", {{"parts", json::array({{{"text", "o"}}})}}}}})},
              {"usageMetadata",
               {{"promptTokenCount", 20}, {"candidatesTokenCount", 1}, {"totalTokenCount", 21}}}},
             {{"candidates", json::array({{{"content", {{"parts", json::array({{{"text", "k"}}})}}},
                                           {"finishReason", "STOP"}}})},
              {"usageMetadata",
               {{"promptTokenCount", 20},
                {"candidatesTokenCount", 3},
                {"thoughtsTokenCount", 266},
                {"cachedContentTokenCount", 10},
                {"totalTokenCount", 289}}}}}));
    EXPECT_EQ(completion.usage.prompt_tokens, 20);
    EXPECT_EQ(completion.usage.cached_prompt_tokens, 10);
    EXPECT_EQ(completion.usage.completion_tokens, 269);
    EXPECT_EQ(completion.usage.reasoning_tokens, 266);
    EXPECT_EQ(completion.usage.total_tokens, 289);
}

TEST(SchemaProviderUsageStream, OpenAIChatFinalUsageChunkCarriesSubsets) {
    auto sp = builtin("openai");
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp,
        sse_data({{{"choices", json::array({{{"delta", {{"content", "ok"}}}}})}},
                  {{"choices", json::array()},
                   {"usage",
                    {{"prompt_tokens", 100},
                     {"completion_tokens", 80},
                     {"total_tokens", 180},
                     {"prompt_tokens_details", {{"cached_tokens", 64}}},
                     {"completion_tokens_details", {{"reasoning_tokens", 50}}}}}}},
                 /*done=*/true));
    EXPECT_EQ(completion.usage.prompt_tokens, 100);
    EXPECT_EQ(completion.usage.cached_prompt_tokens, 64);
    EXPECT_EQ(completion.usage.reasoning_tokens, 50);
    EXPECT_EQ(completion.usage.total_tokens, 180);
}
