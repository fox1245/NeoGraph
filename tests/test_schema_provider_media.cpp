#include <gtest/gtest.h>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/graph/cancel.h>
#include <neograph/async/run_sync.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace neograph;
using namespace std::chrono_literals;

namespace {
struct MediaServer {
    httplib::Server server;
    std::thread worker;
    int port = 0;
    std::atomic<int> polls{0};
    std::atomic<int> finalized{0};
    std::atomic<bool> fail{false};
    std::atomic<bool> submit_error{false};
    std::atomic<bool> never_done{false};
    std::atomic<bool> unsafe_id{false};
    std::atomic<bool> missing_result{false};
    std::atomic<bool> http_error{false};
    std::atomic<int> submit_http_failures{0};  // fail this many submissions with HTTP 503
    std::atomic<int> submissions{0};
    std::atomic<bool> omit_pending_status{false};
    std::atomic<bool> wrong_status_type{false};
    std::atomic<bool> saw_veo_envelope{false};
    std::atomic<bool> saw_images_envelope{false};
    std::atomic<bool> saw_responses_envelope{false};

    MediaServer() {
        server.Post("/v1/responses", [this](const httplib::Request& req, httplib::Response& res) {
            const auto body = json::parse(req.body);
            saw_responses_envelope = body.contains("input") && body.at("input").is_array();
            res.set_content(R"({"output":[{"type":"message","content":[{"type":"output_text","text":"ready"}]},{"type":"image_generation_call","id":"img-7","result":"aW1hZ2U="},{"type":"function_call","call_id":"call-1","name":"save","arguments":"{}"}],"usage":{"input_tokens":3,"output_tokens":4,"total_tokens":7}})", "application/json");
        });
        server.Post("/v1/images/generations", [this](const httplib::Request& req, httplib::Response& res) {
            const auto body = json::parse(req.body);
            saw_images_envelope = body.at("model") == "gpt-image-1" &&
                body.at("prompt") == "Draw a lighthouse" && !body.contains("messages") &&
                !body.contains("temperature");
            res.set_content(R"({"data":[{"b64_json":"UE5H","revised_prompt":"lighthouse"},{"url":"https://example.invalid/image/2"}]})", "application/json");
        });
        server.Post("/v1beta/models/veo-test:predictLongRunning", [this](const httplib::Request& req, httplib::Response& res) {
            const auto body = json::parse(req.body);
            saw_veo_envelope = body.at("instances").at(0).at("prompt") == "A paper kite" &&
                !body.contains("messages") && !body.contains("temperature");
            ++submissions;
            if (submit_http_failures > 0) {
                --submit_http_failures;
                res.status = 503;
                res.set_content(R"({"error":{"code":503,"message":"overloaded","status":"UNAVAILABLE"}})", "application/json");
            } else if (submit_error) {
                res.set_content(R"({"error":{"code":7,"message":"submit denied"}})", "application/json");
            } else if (omit_pending_status) {
                res.set_content(R"({"name":"models/veo-test/operations/op-7"})", "application/json");
            } else if (wrong_status_type) {
                res.set_content(R"({"name":"models/veo-test/operations/op-7","done":"false"})", "application/json");
            } else {
                res.set_content(unsafe_id ? R"({"name":"../danger","done":false})" :
                    R"({"name":"models/veo-test/operations/op-7","done":false})", "application/json");
            }
        });
        server.Get(R"(/v1beta/models/veo-test/operations/op-7)", [this](const httplib::Request& req, httplib::Response& res) {
            EXPECT_EQ(req.get_header_value("x-goog-api-key"), "test-key");
            ++polls;
            if (http_error) {
                res.status = 503;
                res.set_content("upstream error", "text/plain");
            } else if (fail) {
                res.set_content(R"({"done":true,"error":{"code":7,"message":"permission denied"}})", "application/json");
            } else if (omit_pending_status && polls == 1) {
                res.set_content(R"({"name":"models/veo-test/operations/op-7"})", "application/json");
            } else if (wrong_status_type) {
                res.set_content(R"({"done":null})", "application/json");
            } else if (never_done) {
                res.set_content(R"({"done":false})", "application/json");
            } else if (missing_result) {
                res.set_content(R"({"done":true})", "application/json");
            } else {
                res.set_content(R"({"done":true,"response":{"generateVideoResponse":{"generatedSamples":[{"video":{"uri":"https://example.invalid/video/7","mimeType":"video/mp4","fileId":"file-7","durationSeconds":8}}]}}})", "application/json");
            }
        });
        server.Post("/submit", [](const httplib::Request& req, httplib::Response& res) {
            EXPECT_EQ(json::parse(req.body).at("request").at("text"), "Write a report");
            res.set_content(R"({"operation":{"name":"job-1"},"state":{"done":false}})", "application/json");
        });
        server.Post("/poll/job-1", [this](const httplib::Request&, httplib::Response& res) {
            ++polls;
            res.set_content(R"({"state":{"done":true}})", "application/json");
        });
        server.Get("/final/job-1", [this](const httplib::Request&, httplib::Response& res) {
            ++finalized;
            res.set_content(R"({"payload":{"files":[{"id":"file-91","url":"https://example.invalid/file/91","content_type":"application/pdf","info":{"filename":"report.pdf","size":42}}]}})", "application/json");
        });
        server.Post("/v1beta/models/veo-test:generateContent",
            [](const httplib::Request&, httplib::Response& res) {
                res.set_content(R"({"candidates":[{"content":{"parts":[{"text":"done"},{"inlineData":{"mimeType":"image/jpeg","data":"SlBFRw=="}}]}}]})", "application/json");
            });
        port = server.bind_to_any_port("127.0.0.1");
        worker = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i != 200 && !server.is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~MediaServer() {
        server.stop();
        if (worker.joinable()) worker.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

std::unique_ptr<llm::SchemaProvider> provider(MediaServer& server, const std::string& schema,
                                               int timeout = 4) {
    llm::SchemaProvider::Config config;
    config.schema_path = schema;
    config.api_key = "test-key";
    config.base_url_override = server.url();
    config.default_model = "veo-test";
    config.timeout_seconds = timeout;
    config.allow_insecure_loopback = true;
    return llm::SchemaProvider::create(config);
}

CompletionParams prompt_params() {
    CompletionParams params;
    params.prompt = "A paper kite";
    return params;
}
} // namespace

TEST(SchemaProviderMedia, ResponsesPreservesMixedTextToolAndImage) {
    MediaServer server;
    auto p = provider(server, "openai_responses");
    CompletionParams params;
    params.messages.push_back(ChatMessage{"user", "Draw"});
    auto result = p->complete(params);
    EXPECT_TRUE(server.saw_responses_envelope);
    EXPECT_EQ(result.message.content, "ready");
    ASSERT_EQ(result.message.tool_calls.size(), 1u);
    EXPECT_EQ(result.message.tool_calls[0].name, "save");
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].kind, "image");
    EXPECT_EQ(result.artifacts[0].mime_type, "image/png");
    EXPECT_EQ(result.artifacts[0].base64_data, "aW1hZ2U=");
    EXPECT_EQ(result.artifacts[0].metadata, "img-7");
    EXPECT_EQ(result.usage.total_tokens, 7);
}

TEST(SchemaProviderMedia, PromptEnvelopeImagesBase64AndUrl) {
    MediaServer server;
    auto p = provider(server, "openai_images");
    CompletionParams params;
    params.model = "gpt-image-1";
    params.prompt = "Draw a lighthouse";
    const auto result = p->complete(params);
    EXPECT_TRUE(server.saw_images_envelope);
    ASSERT_EQ(result.artifacts.size(), 2u);
    EXPECT_EQ(result.artifacts[0].base64_data, "UE5H");
    EXPECT_EQ(result.artifacts[0].metadata, "lighthouse");
    EXPECT_EQ(result.artifacts[1].url, "https://example.invalid/image/2");
    EXPECT_EQ(result.artifacts[1].mime_type, "image/png");
    params.messages.push_back(ChatMessage{"user", "mixed"});
    EXPECT_THROW(p->complete(params), std::invalid_argument);
}

TEST(SchemaProviderMedia, VeoSubmitPollAndResult) {
    MediaServer server;
    auto p = provider(server, "veo");
    auto result = p->complete(prompt_params());
    EXPECT_TRUE(server.saw_veo_envelope);
    EXPECT_EQ(server.polls, 1);
    ASSERT_EQ(result.artifacts.size(), 1u);
    const auto& video = result.artifacts[0];
    EXPECT_EQ(video.kind, "video");
    EXPECT_EQ(video.mime_type, "video/mp4");
    EXPECT_EQ(video.url, "https://example.invalid/video/7");
    EXPECT_EQ(video.file_id, "file-7");
    EXPECT_EQ(video.metadata.at("durationSeconds"), 8);
}

TEST(SchemaProviderMedia, GenericPostPollAndGetFinalizeFileMetadata) {
    MediaServer server;
    const auto schema = std::filesystem::path(__FILE__).parent_path() /
        "fixtures/media_finalize.json";
    auto p = provider(server, schema.string());
    CompletionParams params;
    params.prompt = "Write a report";
    auto result = p->complete(params);
    EXPECT_EQ(server.polls, 1);
    EXPECT_EQ(server.finalized, 1);
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].kind, "file");
    EXPECT_EQ(result.artifacts[0].mime_type, "application/pdf");
    EXPECT_EQ(result.artifacts[0].file_id, "file-91");
    EXPECT_EQ(result.artifacts[0].url, "https://example.invalid/file/91");
    EXPECT_EQ(result.artifacts[0].metadata.at("filename"), "report.pdf");
    EXPECT_EQ(result.artifacts[0].metadata.at("size"), 42);
}

TEST(SchemaProviderMedia, ProviderFailureAndMalformedTerminalFailClosed) {
    MediaServer server;
    auto p = provider(server, "veo");
    server.fail = true;
    server.submit_error = true;
    try {
        (void)p->complete(prompt_params());
        FAIL() << "submission error must fail closed";
    } catch (const llm::OperationError& error) {
        EXPECT_NE(std::string(error.what()).find("submit denied"), std::string::npos);
    }
    EXPECT_EQ(server.polls, 0);
    server.submit_error = false;
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
    server.fail = false;
    server.missing_result = true;
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
    server.missing_result = false;
    server.unsafe_id = true;
    const int before = server.polls;
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
    EXPECT_EQ(server.polls, before);
}

TEST(SchemaProviderMedia, HttpFailureDoesNotBecomeSuccess) {
    MediaServer server;
    auto p = provider(server, "veo");
    server.http_error = true;
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
}

TEST(SchemaProviderMedia, PollHttpFailureChainsTheTypedCause) {
    MediaServer server;
    auto p = provider(server, "veo");
    server.http_error = true;
    try {
        p->complete(prompt_params());
        FAIL() << "expected OperationError";
    } catch (const llm::OperationError& error) {
        try {
            std::rethrow_if_nested(error);
            FAIL() << "the poll failure lost its cause";
        } catch (const ProviderError& cause) {
            EXPECT_EQ(cause.status(), 503);
            EXPECT_TRUE(cause.retryable());
        }
    }
}

TEST(SchemaProviderMedia, SubmissionHttpFailureKeepsItsTypeBecauseNoJobExists) {
    MediaServer server;
    auto p = provider(server, "veo");
    server.submit_http_failures = 1;
    try {
        p->complete(prompt_params());
        FAIL() << "expected ProviderError";
    } catch (const llm::OperationError&) {
        FAIL() << "a rejected submission must not be an OperationError";
    } catch (const ProviderError& error) {
        EXPECT_EQ(error.status(), 503);
        EXPECT_TRUE(error.retryable());
        EXPECT_EQ(error.code(), "503");
    }
    EXPECT_EQ(server.polls, 0);
}

TEST(SchemaProviderMedia, RateLimitedProviderRetriesARejectedSubmission) {
    MediaServer server;
    server.submit_http_failures = 1;
    llm::RateLimitedProvider::Config retry;
    retry.transient_base_wait_seconds = 0;
    auto retrying = llm::RateLimitedProvider::create(
        std::shared_ptr<Provider>(provider(server, "veo").release()), retry);
    const auto completion = retrying->complete(prompt_params());
    EXPECT_FALSE(completion.artifacts.empty());
    EXPECT_EQ(server.submissions, 2);
}

TEST(SchemaProviderMedia, RateLimitedProviderNeverResubmitsAJobThatAlreadyExists) {
    MediaServer server;
    server.http_error = true;  // every poll fails with 503
    llm::RateLimitedProvider::Config retry;
    retry.transient_base_wait_seconds = 0;
    auto retrying = llm::RateLimitedProvider::create(
        std::shared_ptr<Provider>(provider(server, "veo").release()), retry);
    EXPECT_THROW(retrying->complete(prompt_params()), llm::OperationError);
    EXPECT_EQ(server.submissions, 1);  // one job, not four
}

TEST(SchemaProviderMedia, DeadlineBoundsRepeatedPolling) {
    MediaServer server;
    server.never_done = true;
    auto p = provider(server, "veo", 1);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationTimeoutError);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
}

TEST(SchemaProviderMedia, CancellationStopsBeforeNextPoll) {
    MediaServer server;
    server.never_done = true;
    auto p = provider(server, "veo", 10);
    auto token = std::make_shared<graph::CancelToken>();
    auto params = prompt_params();
    params.cancel_token = token;
    std::jthread canceller([token] {
        std::this_thread::sleep_for(150ms);
        token->cancel();
    });
    EXPECT_THROW(p->complete(params), graph::CancelledException);
    EXPECT_EQ(server.polls, 0);
}

TEST(SchemaProviderMedia, GeminiInlineImageAndTextRemainVisible) {
    MediaServer server;
    auto p = provider(server, "gemini");
    CompletionParams params;
    params.messages.push_back(ChatMessage{"user", "Draw"});
    const auto result = p->complete(params);
    EXPECT_EQ(result.message.content, "done");
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].mime_type, "image/jpeg");
    EXPECT_EQ(result.artifacts[0].base64_data, "SlBFRw==");
}

TEST(SchemaProviderMedia, VeoAbsentStatusRemainsPendingOnSubmitAndPoll) {
    MediaServer server;
    server.omit_pending_status = true;
    auto p = provider(server, "veo");
    const auto result = p->complete(prompt_params());
    EXPECT_EQ(server.polls, 2);
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].file_id, "file-7");
}

TEST(SchemaProviderMedia, VeoRejectsWrongStatusTypesOnSubmitAndPoll) {
    MediaServer server;
    server.wrong_status_type = true;
    auto p = provider(server, "veo");
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
    EXPECT_EQ(server.polls, 0);
    server.omit_pending_status = true;
    EXPECT_THROW(p->complete(prompt_params()), llm::OperationError);
    EXPECT_EQ(server.polls, 2);
}

TEST(SchemaProviderMedia, ResponsesStreamPreservesTerminalArtifactsOnce) {
    MediaServer server;
    server.server.Post("/stream/v1/responses", [](const httplib::Request&, httplib::Response& res) {
        const std::string events =
            "event: response.output_item.added\ndata: {\"item\":{\"type\":\"message\",\"id\":\"msg-1\"}}\n\n"
            "event: response.output_text.delta\ndata: {\"delta\":\"ready\"}\n\n"
            "event: response.output_item.done\ndata: {\"item\":{\"type\":\"image_generation_call\",\"id\":\"img-7\",\"result\":\"UE5H\"}}\n\n"
            "event: response.completed\ndata: {\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"image_generation_call\",\"id\":\"img-7\",\"result\":\"UE5H\"}],\"usage\":{\"input_tokens\":3,\"output_tokens\":4,\"total_tokens\":7}}}\n\n";
        res.set_chunked_content_provider("text/event-stream",
            [events](size_t, httplib::DataSink& sink) {
                sink.write(events.data(), events.size());
                sink.done();
                return true;
            });
    });
    llm::SchemaProvider::Config config;
    config.schema_path = "openai_responses";
    config.api_key = "test-key";
    config.base_url_override = server.url() + "/stream";
    config.timeout_seconds = 2;
    config.allow_insecure_loopback = true;
    auto p = llm::SchemaProvider::create(config);
    CompletionParams params;
    params.messages.push_back({"user", "Draw"});
    std::string chunks;
    const auto result = async::run_sync(p->invoke(params,
        [&](const std::string& chunk) { chunks += chunk; }));
    EXPECT_EQ(chunks, "ready");
    EXPECT_EQ(result.message.content, chunks);
    EXPECT_EQ(result.usage.total_tokens, 7);
    ASSERT_EQ(result.artifacts.size(), 1u);
    EXPECT_EQ(result.artifacts[0].base64_data, "UE5H");
    EXPECT_EQ(result.artifacts[0].metadata, "img-7");
}

TEST(SchemaProviderMedia, GeminiStreamPreservesArtifactsFromEachPart) {
    MediaServer server;
    server.server.Post("/v1beta/models/veo-test:streamGenerateContent",
        [](const httplib::Request&, httplib::Response& res) {
            const std::string events =
                "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"first\"},{\"inlineData\":{\"mimeType\":\"image/png\",\"data\":\"UE5H\"}}]}}]}\n\n"
                "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"second\"},{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"SlBFRw==\"}},{\"functionCall\":{\"name\":\"save\",\"args\":{\"id\":2}}}]},\"finishReason\":\"STOP\"}],\"usageMetadata\":{\"promptTokenCount\":3,\"candidatesTokenCount\":4,\"totalTokenCount\":7}}\n\n";
            res.set_chunked_content_provider("text/event-stream",
                [events](size_t, httplib::DataSink& sink) {
                    sink.write(events.data(), events.size());
                    sink.done();
                    return true;
                });
        });
    auto p = provider(server, "gemini");
    CompletionParams params;
    params.messages.push_back({"user", "Draw"});
    std::string chunks;
    const auto result = p->complete_stream(params,
        [&](const std::string& chunk) { chunks += chunk; });
    EXPECT_EQ(chunks, "firstsecond");
    EXPECT_EQ(result.message.content, chunks);
    EXPECT_EQ(result.usage.total_tokens, 7);
    ASSERT_EQ(result.message.tool_calls.size(), 1u);
    EXPECT_EQ(result.message.tool_calls[0].name, "save");
    ASSERT_EQ(result.artifacts.size(), 2u);
    EXPECT_EQ(result.artifacts[0].base64_data, "UE5H");
    EXPECT_EQ(result.artifacts[1].base64_data, "SlBFRw==");
    EXPECT_EQ(result.artifacts[1].mime_type, "image/jpeg");
}
