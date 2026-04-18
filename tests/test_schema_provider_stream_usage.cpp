// Before the audit fix, complete_stream() populated message content +
// tool calls but never parsed `usage`, leaving prompt_tokens /
// completion_tokens / total_tokens at zero regardless of provider.
// Anyone tracking LLM cost lost the data silently once they switched
// to streaming.
//
// The fix is schema-driven:
//   * SSE_DATA providers (OpenAI, Gemini): the stream loop reads
//     `resp_.usage_path` from each chunk. OpenAI's terminal chunk
//     carries usage (with stream_options.include_usage = true); Gemini
//     emits usageMetadata on every chunk so the last write wins.
//   * SSE_EVENTS providers (Claude): new `action: "usage"` is triggered
//     by the `message_start` and `message_delta` events, which carry
//     input_tokens and output_tokens respectively.
//
// These tests pin both paths end-to-end with a local mock server
// speaking each provider's SSE dialect.

#include <gtest/gtest.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace neograph;

namespace {

// Mock that answers /v1/messages with a Claude-style SSE_EVENTS stream.
// Emits usage in message_start (input_tokens=50) and message_delta
// (output_tokens=7). Content is three text deltas: "he", "llo", "!".
struct ClaudeStreamMock {
    httplib::Server svr;
    std::thread t;
    int port = 0;

    ClaudeStreamMock() {
        svr.Post("/v1/messages",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(
                    "event: message_start\n"
                    "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[],\"model\":\"claude-sonnet-4-5\",\"usage\":{\"input_tokens\":50,\"output_tokens\":0}}}\n"
                    "\n"
                    "event: content_block_start\n"
                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n"
                    "\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"he\"}}\n"
                    "\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"llo\"}}\n"
                    "\n"
                    "event: content_block_delta\n"
                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"!\"}}\n"
                    "\n"
                    "event: content_block_stop\n"
                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n"
                    "\n"
                    "event: message_delta\n"
                    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":7}}\n"
                    "\n"
                    "event: message_stop\n"
                    "data: {\"type\":\"message_stop\"}\n"
                    "\n",
                    "text/event-stream");
            });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~ClaudeStreamMock() {
        svr.stop();
        if (t.joinable()) t.join();
    }
};

// OpenAI-shaped SSE_DATA stream. Terminal chunk carries usage with
// prompt_tokens=12, completion_tokens=3, total_tokens=15.
struct OpenAIStreamMock {
    httplib::Server svr;
    std::thread t;
    int port = 0;

    OpenAIStreamMock() {
        svr.Post("/v1/chat/completions",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(
                    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"hi\"},\"finish_reason\":null}]}\n"
                    "\n"
                    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\" there\"},\"finish_reason\":null}]}\n"
                    "\n"
                    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n"
                    "\n"
                    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3,\"total_tokens\":15}}\n"
                    "\n"
                    "data: [DONE]\n"
                    "\n",
                    "text/event-stream");
            });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~OpenAIStreamMock() {
        svr.stop();
        if (t.joinable()) t.join();
    }
};

} // namespace

TEST(SchemaProviderStreamUsage, ClaudeStreamingPopulatesUsage) {
    ClaudeStreamMock mock;
    ASSERT_GT(mock.port, 0);

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.default_model = "claude-sonnet-4-5";
    cfg.timeout_seconds = 10;
    cfg.base_url_override =
        "http://127.0.0.1:" + std::to_string(mock.port);

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.model = "claude-sonnet-4-5";
    params.max_tokens = 64;
    ChatMessage u; u.role = "user"; u.content = "hi";
    params.messages.push_back(u);

    std::string streamed;
    auto result = provider->complete_stream(params,
        [&streamed](const std::string& t) { streamed += t; });

    EXPECT_EQ("hello!", streamed);
    EXPECT_EQ("hello!", result.message.content);
    EXPECT_EQ(50, result.usage.prompt_tokens)
        << "message_start's input_tokens lost — pre-audit regression";
    EXPECT_EQ(7, result.usage.completion_tokens)
        << "message_delta's output_tokens lost — pre-audit regression";
}

TEST(SchemaProviderStreamUsage, OpenAIStreamingPopulatesUsage) {
    OpenAIStreamMock mock;
    ASSERT_GT(mock.port, 0);

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "openai";
    cfg.api_key = "test-key";
    cfg.default_model = "gpt-4o-mini";
    cfg.timeout_seconds = 10;
    cfg.base_url_override =
        "http://127.0.0.1:" + std::to_string(mock.port);

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.model = "gpt-4o-mini";
    params.max_tokens = 64;
    ChatMessage u; u.role = "user"; u.content = "hi";
    params.messages.push_back(u);

    std::string streamed;
    auto result = provider->complete_stream(params,
        [&streamed](const std::string& t) { streamed += t; });

    EXPECT_EQ("hi there", streamed);
    EXPECT_EQ("hi there", result.message.content);
    EXPECT_EQ(12, result.usage.prompt_tokens);
    EXPECT_EQ(3,  result.usage.completion_tokens);
    EXPECT_EQ(15, result.usage.total_tokens);
}
