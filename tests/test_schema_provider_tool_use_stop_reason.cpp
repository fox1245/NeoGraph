// A response that carries tool calls must not report `end_turn`.
//
// Gemini answers `finishReason: STOP` next to a `functionCall` part (seen live:
// `stop=end_turn tool_calls=1`), and some OpenAI-compatible gateways send
// `finish_reason: "stop"` next to `tool_calls`. A loop that branches on the stop
// reason then treats a request to run tools as a finished turn. The public
// contract: tool calls present + `end_turn` -> `tool_use`; every more specific
// reason (`max_tokens`, `content_filter`, ...) is kept.

#include <gtest/gtest.h>

#include <neograph/llm/openai_provider.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

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

std::unique_ptr<SchemaProvider> provider(const std::string& schema, const OneBodyServer* server) {
    SchemaProvider::Config cfg;
    cfg.schema_path = schema;
    cfg.api_key = "test-key";
    if (server != nullptr) {
        cfg.base_url_override = server->base_url();
        cfg.allow_insecure_loopback = true;
    }
    auto sp = SchemaProvider::create(cfg);
    EXPECT_NE(sp, nullptr);
    return sp;
}

std::string stop_reason_of(const std::string& schema, const json& body) {
    OneBodyServer server(body.dump());
    return provider(schema, &server)->complete(params()).stop_reason;
}

json gemini_response(const std::string& finish_reason, bool with_call) {
    json parts = json::array();
    if (with_call) {
        parts.push_back({{"functionCall", {{"name", "get_weather"}, {"args", {{"city", "Seoul"}}}}}});
    } else {
        parts.push_back({{"text", "done"}});
    }
    return {{"candidates", json::array({{{"content", {{"parts", parts}}}, {"finishReason", finish_reason}}})}};
}

json chat_response(const std::string& finish_reason, bool with_call) {
    json message = {{"role", "assistant"}, {"content", with_call ? json(nullptr) : json("done")}};
    if (with_call) {
        message["tool_calls"] = json::array(
            {{{"id", "call_1"}, {"type", "function"},
              {"function", {{"name", "get_weather"}, {"arguments", "{}"}}}}});
    }
    return {{"choices", json::array({{{"message", message}, {"finish_reason", finish_reason}}})}};
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

// ─── Gemini ───

TEST(ToolUseStopReason, GeminiFunctionCallWithStopIsToolUse) {
    EXPECT_EQ(stop_reason_of("gemini", gemini_response("STOP", true)), "tool_use");
}

TEST(ToolUseStopReason, GeminiTextOnlyStopStaysEndTurn) {
    EXPECT_EQ(stop_reason_of("gemini", gemini_response("STOP", false)), "end_turn");
}

TEST(ToolUseStopReason, GeminiSpecificReasonsSurviveEvenWithToolCalls) {
    EXPECT_EQ(stop_reason_of("gemini", gemini_response("MAX_TOKENS", true)), "max_tokens");
    EXPECT_EQ(stop_reason_of("gemini", gemini_response("SAFETY", true)), "content_filter");
}

TEST(ToolUseStopReason, GeminiStreamFunctionCallWithStopIsToolUse) {
    auto sp = provider("gemini", nullptr);
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data({gemini_response("STOP", true)}));
    ASSERT_EQ(completion.message.tool_calls.size(), 1u);
    EXPECT_EQ(completion.stop_reason, "tool_use");
}

TEST(ToolUseStopReason, GeminiStreamTextOnlyStopStaysEndTurn) {
    auto sp = provider("gemini", nullptr);
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data({gemini_response("STOP", false)}));
    EXPECT_EQ(completion.stop_reason, "end_turn");
}

// ─── OpenAI-compatible gateways ───

TEST(ToolUseStopReason, ChatToolCallsWithGatewayStopIsToolUse) {
    EXPECT_EQ(stop_reason_of("openai", chat_response("stop", true)), "tool_use");
    EXPECT_EQ(stop_reason_of("openai", chat_response("tool_calls", true)), "tool_use");
    EXPECT_EQ(stop_reason_of("openai", chat_response("stop", false)), "end_turn");
    EXPECT_EQ(stop_reason_of("openai", chat_response("length", true)), "max_tokens");
}

TEST(ToolUseStopReason, ChatStreamToolCallsWithGatewayStopIsToolUse) {
    auto sp = provider("openai", nullptr);
    const json call_chunk = {
        {"choices", json::array({{{"delta",
            {{"tool_calls", json::array({{{"index", 0}, {"id", "call_1"},
                {"function", {{"name", "get_weather"}, {"arguments", "{}"}}}}})}}},
            {"finish_reason", "stop"}}})}};
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data({call_chunk}, /*done=*/true));
    ASSERT_EQ(completion.message.tool_calls.size(), 1u);
    EXPECT_EQ(completion.stop_reason, "tool_use");
}

TEST(ToolUseStopReason, NativeOpenAIProviderAppliesTheSameRule) {
    OneBodyServer server(chat_response("stop", true).dump());
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = "test-key";
    cfg.base_url = server.base_url();
    cfg.allow_insecure_loopback = true;
    EXPECT_EQ(neograph::llm::OpenAIProvider::create(cfg)->complete(params()).stop_reason, "tool_use");

    OneBodyServer plain(chat_response("stop", false).dump());
    cfg.base_url = plain.base_url();
    EXPECT_EQ(neograph::llm::OpenAIProvider::create(cfg)->complete(params()).stop_reason, "end_turn");
}

// ─── vendors that already report it ───

TEST(ToolUseStopReason, ClaudeAndResponsesAreUnchanged) {
    EXPECT_EQ(stop_reason_of(
                  "claude",
                  {{"role", "assistant"},
                   {"content", json::array({{{"type", "tool_use"}, {"id", "toolu_1"},
                                             {"name", "get_weather"}, {"input", json::object()}}})},
                   {"stop_reason", "tool_use"}}),
              "tool_use");
    EXPECT_EQ(stop_reason_of(
                  "claude",
                  {{"role", "assistant"},
                   {"content", json::array({{{"type", "text"}, {"text", "done"}}})},
                   {"stop_reason", "end_turn"}}),
              "end_turn");
    EXPECT_EQ(stop_reason_of(
                  "openai_responses",
                  {{"status", "completed"},
                   {"output", json::array({{{"type", "function_call"}, {"call_id", "call_1"},
                                            {"name", "get_weather"}, {"arguments", "{}"}}})}}),
              "tool_use");
}
