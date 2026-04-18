// Concurrent-use regression for SchemaProvider.
//
// Real bug: when 3+ researchers in a Deep Research run issued parallel
// provider->complete() calls on ONE SchemaProvider instance, one of
// them occasionally sent an empty HTTP body to Anthropic (HTTP 400,
// "Input is a zero-length, empty document"). Root cause was concurrent
// traversal of the shared schema_ / template yyjson_mut_doc handles —
// yyjson's own docs disclaim thread safety for `mut` docs even on
// reads, because iterator init/next touches internal state.
//
// Fix: mutex around the schema-touching parts of complete() /
// complete_stream() (body + header + response parse), HTTP left outside
// so fan-out still overlaps on the wire. Plus thread_local PRNG in
// generate_tool_call_id so the SSE id path is race-free.
//
// This test pins it: a local httplib server rejects any empty-body POST
// with HTTP 400 and counts it; N threads hammer one SchemaProvider with
// complete() calls; we assert the server saw zero empty bodies.

#include <gtest/gtest.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;

namespace {

// Fixed Claude-shaped response body. parse_response walks content[0]
// as type=text and reads usage.input_tokens / output_tokens.
constexpr const char* kClaudeBody = R"({
    "id": "msg_01XYZ",
    "type": "message",
    "role": "assistant",
    "content": [
        {"type": "text", "text": "ack"}
    ],
    "model": "claude-sonnet-4-5",
    "stop_reason": "end_turn",
    "usage": {"input_tokens": 10, "output_tokens": 1}
})";

struct MockServer {
    httplib::Server svr;
    std::thread t;
    int port = 0;
    std::atomic<int> empty_body_hits{0};
    std::atomic<int> ok_hits{0};
    std::atomic<int> malformed_hits{0};

    MockServer() {
        auto handler = [this](const httplib::Request& req,
                              httplib::Response& res) {
            // The precise real-world failure signal: an empty POST body.
            // If the mutex fix regresses, a concurrent thread will enter
            // this branch and the assertion downstream catches it.
            if (req.body.empty()) {
                empty_body_hits.fetch_add(1, std::memory_order_relaxed);
                res.status = 400;
                res.set_content(R"({"error":"empty body"})",
                                "application/json");
                return;
            }
            // Sanity: the body should be a valid JSON object with a
            // `messages` array. If the schema race produced a truncated /
            // malformed body, the mock sees it here.
            try {
                auto parsed = json::parse(req.body);
                if (!parsed.is_object() ||
                    !parsed.contains("messages") ||
                    !parsed["messages"].is_array() ||
                    parsed["messages"].size() == 0) {
                    malformed_hits.fetch_add(1, std::memory_order_relaxed);
                    res.status = 400;
                    res.set_content(R"({"error":"malformed"})",
                                    "application/json");
                    return;
                }
            } catch (...) {
                malformed_hits.fetch_add(1, std::memory_order_relaxed);
                res.status = 400;
                res.set_content(R"({"error":"parse failed"})",
                                "application/json");
                return;
            }
            ok_hits.fetch_add(1, std::memory_order_relaxed);
            res.set_content(kClaudeBody, "application/json");
        };
        svr.Post("/v1/messages", handler);
        // OpenAI-compatible path in case we ever re-use this mock for the
        // openai schema; Claude schema only uses /v1/messages.
        svr.Post("/v1/chat/completions", handler);

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~MockServer() {
        svr.stop();
        if (t.joinable()) t.join();
    }
};

} // namespace

TEST(SchemaProviderConcurrent, ParallelCompletesNeverSendEmptyOrMalformedBody) {
    MockServer mock;
    ASSERT_GT(mock.port, 0);
    ASSERT_TRUE(mock.svr.is_running());

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.default_model = "claude-sonnet-4-5";
    cfg.timeout_seconds = 5;
    cfg.base_url_override =
        "http://127.0.0.1:" + std::to_string(mock.port);

    auto provider = llm::SchemaProvider::create(cfg);
    ASSERT_TRUE(provider);

    constexpr int kConcurrency = 8;
    constexpr int kIterations = 4;

    std::atomic<int> exceptions{0};
    std::atomic<int> completed{0};

    std::vector<std::future<void>> futures;
    for (int i = 0; i < kConcurrency; ++i) {
        futures.push_back(std::async(std::launch::async, [&, i] {
            for (int j = 0; j < kIterations; ++j) {
                CompletionParams params;
                params.model = "claude-sonnet-4-5";
                params.temperature = 0.3f;
                params.max_tokens = 64;
                ChatMessage sys; sys.role = "system";
                sys.content = "thread " + std::to_string(i);
                params.messages.push_back(sys);
                ChatMessage u; u.role = "user";
                u.content = "ping " + std::to_string(i) + "/" + std::to_string(j);
                params.messages.push_back(u);

                ChatTool t1;
                t1.name = "tool_a";
                t1.description = "first";
                t1.parameters = json{
                    {"type", "object"},
                    {"properties", {{"x", {{"type", "string"}}}}}
                };
                params.tools.push_back(t1);
                ChatTool t2;
                t2.name = "tool_b";
                t2.description = "second";
                t2.parameters = json{
                    {"type", "object"},
                    {"properties", json::object()}
                };
                params.tools.push_back(t2);

                try {
                    auto r = provider->complete(params);
                    EXPECT_EQ("ack", r.message.content);
                } catch (const std::exception& e) {
                    exceptions.fetch_add(1);
                }
                completed.fetch_add(1);
            }
        }));
    }
    for (auto& f : futures) f.get();

    EXPECT_EQ(kConcurrency * kIterations, completed.load());
    EXPECT_EQ(0, exceptions.load())
        << "one or more concurrent complete() calls threw";
    EXPECT_EQ(0, mock.empty_body_hits.load())
        << "server saw empty request body(s) — schema_mutex regression";
    EXPECT_EQ(0, mock.malformed_hits.load())
        << "server saw malformed JSON — schema traversal corrupted output";
    EXPECT_EQ(kConcurrency * kIterations, mock.ok_hits.load());
}

// ---------------------------------------------------------------------------
// Retry-After honouring.
//
// On a 429 the provider should read Retry-After and sleep exactly that long
// before retrying once. A mock server that returns 429 on the first hit
// (with Retry-After: 1) and 200 on the second hit validates the behaviour
// end-to-end: the client must succeed without the caller lifting a finger.
// ---------------------------------------------------------------------------

struct RetryAfterMock {
    httplib::Server svr;
    std::thread t;
    int port = 0;
    std::atomic<int> hits{0};

    explicit RetryAfterMock(int retry_after_sec) {
        int wait = retry_after_sec;
        svr.Post("/v1/messages",
            [this, wait](const httplib::Request&, httplib::Response& res) {
                int n = hits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n == 1) {
                    res.status = 429;
                    res.set_header("Retry-After", std::to_string(wait));
                    res.set_content(
                        R"({"type":"error","error":{"type":"rate_limit_error","message":"mock rate limit"}})",
                        "application/json");
                    return;
                }
                res.set_content(kClaudeBody, "application/json");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~RetryAfterMock() {
        svr.stop();
        if (t.joinable()) t.join();
    }
};

TEST(SchemaProviderConcurrent, RetryAfterOn429) {
    RetryAfterMock mock(/*retry_after_sec=*/1);
    ASSERT_GT(mock.port, 0);

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.default_model = "claude-sonnet-4-5";
    cfg.timeout_seconds = 30;
    cfg.base_url_override =
        "http://127.0.0.1:" + std::to_string(mock.port);

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.model = "claude-sonnet-4-5";
    params.max_tokens = 32;
    ChatMessage u; u.role = "user"; u.content = "hi";
    params.messages.push_back(u);

    auto t0 = std::chrono::steady_clock::now();
    auto result = provider->complete(params);  // must NOT throw
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_EQ("ack", result.message.content);
    EXPECT_EQ(2, mock.hits.load())  // first 429 + successful retry
        << "expected exactly one retry after the 429";
    // Slept at least the Retry-After duration — allow generous slack for
    // scheduling jitter on busy CI.
    EXPECT_GE(elapsed, 900)
        << "provider returned too fast; Retry-After not honoured";
    EXPECT_LT(elapsed, 5000)  // sanity upper bound
        << "provider took unexpectedly long";
}

// Retry-After that exceeds the caller's timeout must NOT be honoured —
// the caller explicitly budgeted less than the wait. Provider should
// throw promptly so the outer RetryPolicy / user can decide.
TEST(SchemaProviderConcurrent, RetryAfterRespectsTimeout) {
    RetryAfterMock mock(/*retry_after_sec=*/60);
    ASSERT_GT(mock.port, 0);

    llm::SchemaProvider::Config cfg;
    cfg.schema_path = "claude";
    cfg.api_key = "test-key";
    cfg.default_model = "claude-sonnet-4-5";
    cfg.timeout_seconds = 2;  // shorter than retry-after
    cfg.base_url_override =
        "http://127.0.0.1:" + std::to_string(mock.port);

    auto provider = llm::SchemaProvider::create(cfg);

    CompletionParams params;
    params.model = "claude-sonnet-4-5";
    params.max_tokens = 32;
    ChatMessage u; u.role = "user"; u.content = "hi";
    params.messages.push_back(u);

    auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(provider->complete(params), std::runtime_error);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 5)
        << "provider waited for retry-after > timeout_seconds";
}
