// Provider schemas carry vendor-specific completion reasons. This pins their
// normalized public contract without contacting an external provider.

#include <gtest/gtest.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <chrono>
#include <string>
#include <thread>

using namespace neograph;

namespace {

struct StopReasonMock {
    httplib::Server svr;
    std::thread t;
    int port = 0;

    StopReasonMock() {
        svr.Post("/v1/chat/completions",
            [](const httplib::Request& req, httplib::Response& res) {
                if (req.body.find("gpt-tool") != std::string::npos) {
                    res.set_content(R"({"choices":[{"message":{"role":"assistant","content":"done","tool_calls":[{"id":"call_1","function":{"name":"lookup","arguments":"{}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3}})", "application/json");
                    return;
                }
                res.set_content(R"({"choices":[{"message":{"role":"assistant","content":"done"},"finish_reason":"length"}],"usage":{"prompt_tokens":1,"completion_tokens":2,"total_tokens":3}})", "application/json");
            });
        svr.Post("/v1/messages",
            [](const httplib::Request& req, httplib::Response& res) {
                if (req.body.find("claude-refusal") != std::string::npos) {
                    res.set_content(R"({"content":[{"type":"text","text":"done"}],"stop_reason":"refusal","usage":{"input_tokens":1,"output_tokens":2}})", "application/json");
                    return;
                }
                res.set_content(R"({"content":[{"type":"text","text":"done"}],"stop_reason":"stop_sequence","usage":{"input_tokens":1,"output_tokens":2}})", "application/json");
            });
        svr.Post("/v1beta/models/gemini-test:generateContent",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({"candidates":[{"content":{"parts":[{"text":"done"}]},"finishReason":"SAFETY"}],"usageMetadata":{"promptTokenCount":1,"candidatesTokenCount":2,"totalTokenCount":3}})", "application/json");
            });
        svr.Post("/v1/responses",
            [](const httplib::Request& req, httplib::Response& res) {
                if (req.body.find("gpt-complete") != std::string::npos) {
                    res.set_content(R"({"status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"done"}]}],"usage":{"input_tokens":1,"output_tokens":2,"total_tokens":3}})", "application/json");
                    return;
                }
                res.set_content(R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[{"type":"message","content":[{"type":"output_text","text":"done"}]}],"usage":{"input_tokens":1,"output_tokens":2,"total_tokens":3}})", "application/json");
            });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~StopReasonMock() {
        svr.stop();
        if (t.joinable()) t.join();
    }
};

llm::SchemaProvider::Config config_for(const std::string& schema, int port,
                                       const std::string& model) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path = schema;
    cfg.api_key = "test-key";
    cfg.default_model = model;
    cfg.timeout_seconds = 10;
    cfg.base_url_override = "http://127.0.0.1:" + std::to_string(port);
    return cfg;
}

CompletionParams params_for(const std::string& model) {
    CompletionParams params;
    params.model = model;
    params.messages.push_back({"user", "hello"});
    return params;
}

} // namespace

TEST(SchemaProviderStopReason, BundledSchemasNormalizeProviderReasons) {
    StopReasonMock mock;
    ASSERT_GT(mock.port, 0);

    struct Case {
        const char* schema;
        const char* model;
        const char* expected_reason;
    };
    const Case cases[] = {
        {"openai", "gpt-test", "max_tokens"},
        {"openai", "gpt-tool", "tool_use"},
        {"claude", "claude-test", "stop_sequence"},
        {"claude", "claude-refusal", "refusal"},
        {"gemini", "gemini-test", "content_filter"},
        {"openai_responses", "gpt-test", "max_tokens"},
        {"openai_responses", "gpt-complete", "end_turn"},
    };

    for (const auto& test : cases) {
        auto provider = llm::SchemaProvider::create(
            config_for(test.schema, mock.port, test.model));
        const auto completion = provider->complete(params_for(test.model));
        EXPECT_EQ(completion.stop_reason, test.expected_reason) << test.schema;
        EXPECT_EQ(completion.message.content, "done") << test.schema;
    }
}
