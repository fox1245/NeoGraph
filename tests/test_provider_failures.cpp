// Provider failures must surface as typed errors, never as a successful
// (empty or partial) completion, and only transient ones may be retried.
//
//   * ProviderError carries status / retryable / vendor code / request id;
//     which statuses are transient is schema data (`connection.retryable_*`).
//   * RateLimitedProvider retries exactly the retryable set.
//   * Error bodies are redacted and truncated before they reach a message.
//   * Failure signals inside a 2xx response or stream (`error` events,
//     `response.failed`, `status:"failed"`, EOF before the terminal event,
//     blocked prompts, failing finish reasons) throw instead of returning
//     `end_turn`, for SSE and WebSocket alike.
//
// The wire shapes follow the vendors' documented error formats. The vendor
// mock server is real HTTP (httplib), so the transport paths are exercised too.

#include <gtest/gtest.h>

#include <neograph/llm/openai_provider.h>
#include <neograph/llm/rate_limited_provider.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using neograph::ChatCompletion;
using neograph::CompletionParams;
using neograph::json;
using neograph::ProviderError;
using neograph::RateLimitError;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

struct Reply {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string content_type = "application/json";
};

// Serves `replies` in order (the last one repeats) for any POST path.
struct Vendor {
    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::atomic<int> calls{0};
    std::vector<Reply> replies;

    explicit Vendor(std::vector<Reply> r) : replies(std::move(r)) {
        svr.Post(R"(.*)", [this](const httplib::Request&, httplib::Response& res) {
            const auto index = std::min<std::size_t>(
                static_cast<std::size_t>(calls.fetch_add(1)), replies.size() - 1);
            const auto& reply = replies[index];
            res.status = reply.status;
            for (const auto& [name, value] : reply.headers) res.set_header(name, value);
            res.set_content(reply.body, reply.content_type);
        });
        port = svr.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~Vendor() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

std::unique_ptr<SchemaProvider> schema_provider(const std::string& schema, const Vendor* vendor) {
    SchemaProvider::Config cfg;
    cfg.schema_path = schema;
    cfg.api_key = "test-key";
    if (vendor != nullptr) {
        cfg.base_url_override = vendor->base_url();
        cfg.allow_insecure_loopback = true;
    }
    auto provider = SchemaProvider::create(cfg);
    EXPECT_NE(provider, nullptr);
    return provider;
}

CompletionParams params() {
    CompletionParams p;
    p.model = "test-model";
    p.messages.push_back({"user", "hi"});
    return p;
}

template <class F>
ProviderError caught(F&& call) {
    try {
        call();
    } catch (const ProviderError& error) {
        return error;
    } catch (const std::exception& error) {
        ADD_FAILURE() << "expected ProviderError, got: " << error.what();
        return ProviderError("wrong type", -1, false);
    }
    ADD_FAILURE() << "expected ProviderError, nothing was thrown";
    return ProviderError("no throw", -1, false);
}

constexpr const char* kClaudeOk =
    R"({"role":"assistant","content":[{"type":"text","text":"pong"}],)"
    R"("stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})";

std::string claude_error(const std::string& type, const std::string& message) {
    return json({{"type", "error"},
                 {"error", {{"type", type}, {"message", message}}},
                 {"request_id", "req_from_body"}})
        .dump();
}

// SSE fixture lines, `event:` style (Anthropic) or data-only.
std::vector<std::string> sse_events(const std::vector<std::pair<std::string, json>>& events) {
    std::vector<std::string> lines;
    for (const auto& [name, payload] : events) {
        lines.push_back("event: " + name);
        lines.push_back("data: " + payload.dump());
        lines.push_back("");
    }
    return lines;
}

std::vector<std::string> sse_data(const std::vector<json>& chunks) {
    std::vector<std::string> lines;
    for (const auto& chunk : chunks) {
        lines.push_back("data: " + chunk.dump());
        lines.push_back("");
    }
    return lines;
}

std::vector<std::pair<std::string, json>> claude_partial_stream() {
    return {
        {"message_start", {{"type", "message_start"}, {"message", {{"usage", {{"input_tokens", 3}}}}}}},
        {"content_block_start",
         {{"type", "content_block_start"}, {"content_block", {{"type", "text"}, {"text", ""}}}}},
        {"content_block_delta",
         {{"type", "content_block_delta"},
          {"delta", {{"type", "text_delta"}, {"text", "partial "}}}}},
    };
}

}  // namespace

// ─── #312: typed errors, classification, redaction ───

TEST(ProviderErrorClassification, TransientStatusesAreRetryableTypedErrors) {
    for (const int status : {500, 502, 503, 529}) {
        Vendor vendor({{status, claude_error("overloaded_error", "Overloaded"),
                        {{"request-id", "req_from_header"}}}});
        auto provider = schema_provider("claude", &vendor);
        const auto error = caught([&] { provider->complete(params()); });
        EXPECT_EQ(error.status(), status);
        EXPECT_TRUE(error.retryable()) << status;
        EXPECT_EQ(error.code(), "overloaded_error");
        EXPECT_EQ(error.request_id(), "req_from_header");  // header beats body
    }
}

TEST(ProviderErrorClassification, RateLimitKeepsItsTypeAndRetryAfter) {
    Vendor vendor({{429, claude_error("rate_limit_error", "slow down"), {{"Retry-After", "7"}}}});
    auto provider = schema_provider("claude", &vendor);
    try {
        provider->complete(params());
        FAIL() << "expected RateLimitError";
    } catch (const RateLimitError& error) {
        EXPECT_EQ(error.status(), 429);
        EXPECT_TRUE(error.retryable());
        EXPECT_EQ(error.retry_after_seconds(), 7);
        EXPECT_EQ(error.code(), "rate_limit_error");
    }
}

TEST(ProviderErrorClassification, ClientErrorsAreNotRetryable) {
    for (const int status : {400, 401, 404}) {
        Vendor vendor({{status, claude_error("invalid_request_error", "bad"), {}}});
        auto provider = schema_provider("claude", &vendor);
        const auto error = caught([&] { provider->complete(params()); });
        EXPECT_EQ(error.status(), status);
        EXPECT_FALSE(error.retryable()) << status;
        EXPECT_EQ(error.code(), "invalid_request_error");
    }
}

TEST(ProviderErrorClassification, RetryableStatusListIsSchemaData) {
    // Anthropic documents 529 "overloaded" as transient; the OpenAI schema does not list it.
    Vendor vendor({{529, R"({"error":{"message":"overloaded"}})", {}}});
    EXPECT_TRUE(caught([&] { schema_provider("claude", &vendor)->complete(params()); }).retryable());
    EXPECT_FALSE(caught([&] { schema_provider("openai", &vendor)->complete(params()); }).retryable());
}

TEST(ProviderErrorClassification, OpenAIProviderReportsTypedErrorsToo) {
    Vendor vendor({{503, R"({"error":{"message":"unavailable","type":"server_error"}})",
                    {{"x-request-id", "req_oa"}}}});
    neograph::llm::OpenAIProvider::Config cfg;
    cfg.api_key = "test-key";
    cfg.base_url = vendor.base_url();
    cfg.allow_insecure_loopback = true;
    auto provider = neograph::llm::OpenAIProvider::create(cfg);
    const auto error = caught([&] { provider->complete(params()); });
    EXPECT_EQ(error.status(), 503);
    EXPECT_TRUE(error.retryable());
    EXPECT_EQ(error.code(), "server_error");
    EXPECT_EQ(error.request_id(), "req_oa");
}

TEST(ProviderErrorRedaction, MessagesDoNotCarryAccountIdentifiers) {
    // OpenRouter echoes the account in error bodies; that must not reach logs.
    Vendor vendor({{401,
                    R"({"error":{"message":"User not found. Key sk-or-v1-abcdef0123456789abcdef used by org-Ab12Cd34Ef56","code":401,)"
                    R"("metadata":{"user_id":"user_2abcDEF123456xyz","raw":"Bearer abcdef0123456789"}}})",
                    {}}});
    auto provider = schema_provider("openai", &vendor);
    const std::string what = caught([&] { provider->complete(params()); }).what();
    EXPECT_NE(what.find("User not found"), std::string::npos) << what;
    EXPECT_EQ(what.find("user_2abcDEF123456xyz"), std::string::npos) << what;
    EXPECT_EQ(what.find("sk-or-v1-abcdef0123456789"), std::string::npos) << what;
    EXPECT_EQ(what.find("org-Ab12Cd34Ef56"), std::string::npos) << what;
    EXPECT_EQ(what.find("abcdef0123456789"), std::string::npos) << what;
    EXPECT_NE(what.find("[redacted]"), std::string::npos) << what;
}

TEST(ProviderErrorRedaction, HugeBodiesAreTruncated) {
    Vendor vendor({{500, std::string(200000, 'x'), {}}});
    auto provider = schema_provider("claude", &vendor);
    const std::string what = caught([&] { provider->complete(params()); }).what();
    EXPECT_LT(what.size(), 1300u);
    EXPECT_NE(what.find("[truncated]"), std::string::npos);
}

// ─── #312: RateLimitedProvider retries exactly the retryable set ───

namespace {
std::unique_ptr<neograph::llm::RateLimitedProvider> retrying(
    const Vendor& vendor, int max_retries = 3) {
    neograph::llm::RateLimitedProvider::Config cfg;
    cfg.max_retries = max_retries;
    cfg.default_wait_seconds = 0;  // sleeps the mandatory +1s slack only
    return neograph::llm::RateLimitedProvider::create(
        std::shared_ptr<neograph::Provider>(schema_provider("claude", &vendor).release()), cfg);
}
}  // namespace

TEST(RateLimitedProviderRetries, RetriesTransientServerErrors) {
    for (const int status : {500, 503, 529}) {
        Vendor vendor({{status, claude_error("overloaded_error", "Overloaded"), {}},
                       {200, kClaudeOk, {}}});
        const auto completion = retrying(vendor)->complete(params());
        EXPECT_EQ(completion.message.content, "pong") << status;
        EXPECT_EQ(vendor.calls.load(), 2) << status;
    }
}

TEST(RateLimitedProviderRetries, DoesNotRetryNonRetryableErrors) {
    Vendor vendor({{400, claude_error("invalid_request_error", "bad"), {}}, {200, kClaudeOk, {}}});
    const auto error = caught([&] { retrying(vendor)->complete(params()); });
    EXPECT_EQ(error.status(), 400);
    EXPECT_EQ(vendor.calls.load(), 1);
}

TEST(RateLimitedProviderRetries, GivingUpKeepsTheConcreteErrorType) {
    Vendor vendor({{429, claude_error("rate_limit_error", "slow"), {{"Retry-After", "0"}}}});
    EXPECT_THROW(retrying(vendor, /*max_retries=*/1)->complete(params()), RateLimitError);
    EXPECT_EQ(vendor.calls.load(), 2);
}

// ─── #307: Anthropic ───

TEST(ProviderFailureClaude, ErrorEventMidStreamIsARetryableError) {
    auto sp = schema_provider("claude", nullptr);
    auto events = claude_partial_stream();
    events.push_back({"error",
                      {{"type", "error"},
                       {"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}}});
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(*sp, sse_events(events));
    });
    EXPECT_EQ(error.code(), "overloaded_error");
    EXPECT_TRUE(error.retryable());
}

TEST(ProviderFailureClaude, StreamCutBeforeMessageStopIsNotACompletion) {
    auto sp = schema_provider("claude", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(*sp, sse_events(claude_partial_stream()));
    });
    EXPECT_EQ(error.code(), "stream_truncated");
    EXPECT_TRUE(error.retryable());
}

TEST(ProviderFailureClaude, CompleteStreamStillSucceeds) {
    auto sp = schema_provider("claude", nullptr);
    auto events = claude_partial_stream();
    events.push_back({"content_block_stop", {{"type", "content_block_stop"}}});
    events.push_back({"message_delta",
                      {{"type", "message_delta"},
                       {"delta", {{"stop_reason", "end_turn"}}},
                       {"usage", {{"output_tokens", 2}}}}});
    events.push_back({"message_stop", {{"type", "message_stop"}}});
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(*sp, sse_events(events));
    EXPECT_EQ(completion.message.content, "partial ");
    EXPECT_EQ(completion.stop_reason, "end_turn");
}

TEST(ProviderFailureClaude, HttpStreamErrorEventKeepsItsType) {
    auto events = claude_partial_stream();
    events.push_back({"error",
                      {{"type", "error"},
                       {"error", {{"type", "overloaded_error"}, {"message", "Overloaded"}}}}});
    std::string body;
    for (const auto& line : sse_events(events)) body += line + "\n";
    Vendor vendor({{200, body, {}, "text/event-stream"}});
    auto sp = schema_provider("claude", &vendor);
    std::string streamed;
    const auto error = caught([&] {
        sp->complete_stream(params(), [&](const std::string& chunk) { streamed += chunk; });
    });
    EXPECT_EQ(error.code(), "overloaded_error");
    EXPECT_TRUE(error.retryable());
    EXPECT_EQ(streamed, "partial ");  // what arrived before the failure was still delivered
}

TEST(ProviderFailureClaude, HttpStreamCutBeforeMessageStopThrows) {
    std::string body;
    for (const auto& line : sse_events(claude_partial_stream())) body += line + "\n";
    Vendor vendor({{200, body, {}, "text/event-stream"}});
    auto sp = schema_provider("claude", &vendor);
    const auto error = caught([&] { sp->complete_stream(params(), {}); });
    EXPECT_EQ(error.code(), "stream_truncated");
}

// ─── #307: OpenAI Responses (SSE, WebSocket, non-stream) ───

namespace {
std::vector<json> responses_partial_events() {
    return {
        {{"type", "response.output_item.added"}, {"item", {{"type", "message"}, {"id", "m1"}}}},
        {{"type", "response.output_text.delta"}, {"delta", "partial "}},
    };
}
json responses_failed() {
    return {{"type", "response.failed"},
            {"response",
             {{"status", "failed"},
              {"error", {{"code", "server_error"}, {"message", "The model failed"}}}}}};
}
json responses_error_event() {
    return {{"type", "error"}, {"code", "rate_limit_exceeded"}, {"message", "Slow down"}};
}
json responses_completed() {
    return {{"type", "response.completed"},
            {"response",
             {{"status", "completed"},
              {"usage", {{"input_tokens", 4}, {"output_tokens", 2}, {"total_tokens", 6}}}}}};
}
std::vector<json> with(std::vector<json> events, const json& last) {
    events.push_back(last);
    return events;
}
}  // namespace

TEST(ProviderFailureResponses, ResponseFailedAfterPartialOutputIsAnError_Sse) {
    auto sp = schema_provider("openai_responses", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(
            *sp, sse_data(with(responses_partial_events(), responses_failed())));
    });
    EXPECT_EQ(error.code(), "server_error");
    EXPECT_TRUE(error.retryable());
}

TEST(ProviderFailureResponses, ResponseFailedAfterPartialOutputIsAnError_WebSocket) {
    auto sp = schema_provider("openai_responses", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_ws_events(
            *sp, with(responses_partial_events(), responses_failed()));
    });
    EXPECT_EQ(error.code(), "server_error");
    EXPECT_TRUE(error.retryable());
}

TEST(ProviderFailureResponses, ErrorEventIsATypedRateLimit_Sse) {
    auto sp = schema_provider("openai_responses", nullptr);
    EXPECT_THROW(SchemaProviderTestAccess::parse_stream_lines(
                     *sp, sse_data({responses_error_event()})),
                 RateLimitError);
}

TEST(ProviderFailureResponses, ErrorEventIsATypedRateLimit_WebSocket) {
    auto sp = schema_provider("openai_responses", nullptr);
    EXPECT_THROW(SchemaProviderTestAccess::parse_ws_events(*sp, {responses_error_event()}),
                 RateLimitError);
}

TEST(ProviderFailureResponses, StreamCutWithoutCompletedIsNotACompletion_Sse) {
    auto sp = schema_provider("openai_responses", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(*sp, sse_data(responses_partial_events()));
    });
    EXPECT_EQ(error.code(), "stream_truncated");
}

TEST(ProviderFailureResponses, StreamCutWithoutCompletedIsNotACompletion_WebSocket) {
    auto sp = schema_provider("openai_responses", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_ws_events(*sp, responses_partial_events());
    });
    EXPECT_EQ(error.code(), "stream_truncated");
}

TEST(ProviderFailureResponses, CompletedStreamStillSucceeds) {
    auto sp = schema_provider("openai_responses", nullptr);
    const auto sse = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data(with(responses_partial_events(), responses_completed())));
    EXPECT_EQ(sse.message.content, "partial ");
    const auto ws = SchemaProviderTestAccess::parse_ws_events(
        *sp, with(responses_partial_events(), responses_completed()));
    EXPECT_EQ(ws.message.content, "partial ");
}

TEST(ProviderFailureResponses, FailedStatusBodyIsAnErrorNotAnEmptyAnswer) {
    auto sp = schema_provider("openai_responses", nullptr);
    const json failed = {{"id", "resp_1"},
                         {"status", "failed"},
                         {"error", {{"code", "server_error"}, {"message", "boom"}}},
                         {"output", json::array()}};
    const auto error = caught([&] { SchemaProviderTestAccess::parse_response(*sp, failed); });
    EXPECT_EQ(error.code(), "server_error");

    const json cancelled = {{"id", "resp_2"}, {"status", "cancelled"}, {"output", json::array()}};
    EXPECT_EQ(caught([&] { SchemaProviderTestAccess::parse_response(*sp, cancelled); }).code(),
              "cancelled");
}

// ─── #307: Gemini ───

TEST(ProviderFailureGemini, BlockedPromptIsAContentFilterStopNotEndTurn_Stream) {
    auto sp = schema_provider("gemini", nullptr);
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, sse_data({{{"promptFeedback", {{"blockReason", "SAFETY"}}}}}));
    EXPECT_EQ(completion.stop_reason, "content_filter");
    EXPECT_TRUE(completion.message.content.empty());
}

TEST(ProviderFailureGemini, BlockedPromptIsAContentFilterStopNotEndTurn_Http) {
    Vendor vendor({{200, R"({"promptFeedback":{"blockReason":"PROHIBITED_CONTENT"}})", {}}});
    auto sp = schema_provider("gemini", &vendor);
    EXPECT_EQ(sp->complete(params()).stop_reason, "content_filter");
}

TEST(ProviderFailureGemini, FailingFinishReasonIsAnErrorNotUnknown_Stream) {
    auto sp = schema_provider("gemini", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(
            *sp, sse_data({{{"candidates", json::array({{{"finishReason", "MALFORMED_FUNCTION_CALL"}}})}}}));
    });
    EXPECT_EQ(error.code(), "MALFORMED_FUNCTION_CALL");
    EXPECT_FALSE(error.retryable());
}

TEST(ProviderFailureGemini, FailingFinishReasonIsAnErrorNotUnknown_Http) {
    Vendor vendor({{200,
                    R"({"candidates":[{"content":{"parts":[]},"finishReason":"OTHER"}]})", {}}});
    auto sp = schema_provider("gemini", &vendor);
    EXPECT_EQ(caught([&] { sp->complete(params()); }).code(), "OTHER");
}

TEST(ProviderFailureGemini, ImageSafetyIsAContentFilterStop) {
    Vendor vendor({{200,
                    R"({"candidates":[{"content":{"parts":[]},"finishReason":"IMAGE_SAFETY"}]})", {}}});
    auto sp = schema_provider("gemini", &vendor);
    EXPECT_EQ(sp->complete(params()).stop_reason, "content_filter");
}

TEST(ProviderFailureGemini, ErrorChunkMidStreamIsAnError) {
    auto sp = schema_provider("gemini", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(
            *sp, sse_data({{{"error", {{"code", 503}, {"message", "overloaded"}, {"status", "UNAVAILABLE"}}}}}));
    });
    EXPECT_TRUE(error.retryable());  // 503 is in the schema's retryable statuses
}

// ─── #307: OpenAI-compatible chat (OpenAI, OpenRouter, ...) ───

TEST(ProviderFailureChat, ErrorChunkMidStreamIsAnError) {
    auto sp = schema_provider("openai", nullptr);
    const auto error = caught([&] {
        SchemaProviderTestAccess::parse_stream_lines(
            *sp, sse_data({{{"choices", json::array({{{"delta", {{"content", "par"}}}}})}},
                           {{"error", {{"message", "Provider disconnected"}, {"code", 502}}}}}));
    });
    EXPECT_TRUE(error.retryable());
    EXPECT_NE(std::string(error.what()).find("Provider disconnected"), std::string::npos);
}

TEST(ProviderFailureChat, RateLimitChunkMidStreamIsATypedRateLimit) {
    auto sp = schema_provider("openai", nullptr);
    EXPECT_THROW(SchemaProviderTestAccess::parse_stream_lines(
                     *sp, sse_data({{{"error", {{"message", "slow"}, {"code", 429}}}}})),
                 RateLimitError);
}

TEST(ProviderFailureChat, ErrorBodyWithHttp200IsAnErrorNotAParseFailure) {
    Vendor vendor({{200, R"({"error":{"message":"Upstream error","code":502}})", {}}});
    auto sp = schema_provider("openai", &vendor);
    const auto error = caught([&] { sp->complete(params()); });
    EXPECT_TRUE(error.retryable());
    EXPECT_EQ(error.status(), 200);
    EXPECT_NE(std::string(error.what()).find("Upstream error"), std::string::npos);
}
