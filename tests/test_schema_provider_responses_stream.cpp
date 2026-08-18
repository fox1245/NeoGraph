// Streaming regressions for SchemaProvider("openai_responses") —
// pinning the SSE_EVENTS path against /v1/responses' event sequence:
//
//   1. Plain-text deltas (response.output_text.delta) accumulate into
//      ChatMessage::content and drive on_chunk per event.
//   2. Function-call argument deltas (response.function_call_arguments.
//      delta) concatenate verbatim into ToolCall::arguments. id/name
//      are lifted from the prior response.output_item.added event,
//      *not* re-read off each delta — the deltas only carry the args
//      slice.
//   3. response.completed's nested response.usage populates
//      ChatCompletion::usage. Regression for the empty action="done"
//      handler that used to drop usage on every streamed call.
//
// Mock /v1/responses with httplib::Server speaking the documented
// SSE event dialect; SchemaProvider points at it via base_url_override.

#include <gtest/gtest.h>
#include <neograph/llm/schema_provider.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace neograph;

namespace {

struct ResponsesMock {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    explicit ResponsesMock(std::string sse_body, int status = 200) {
        svr.Post("/v1/responses",
            [body = std::move(sse_body), status]
            (const httplib::Request&, httplib::Response& res) {
                res.status = status;
                res.set_header("Content-Type", "text/event-stream");
                res.set_content(body, "text/event-stream");
            });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this]{ svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~ResponsesMock() { svr.stop(); if (t.joinable()) t.join(); }
};

llm::SchemaProvider::Config cfg_for(int port) {
    llm::SchemaProvider::Config cfg;
    cfg.schema_path       = "openai_responses";
    cfg.api_key           = "test-key";
    cfg.default_model     = "gpt-test";
    cfg.timeout_seconds   = 10;
    cfg.base_url_override = "http://127.0.0.1:" + std::to_string(port);
    cfg.allow_insecure_loopback = true;
    return cfg;
}

CompletionParams params_with(std::string user_msg) {
    CompletionParams p;
    p.model = "gpt-test";
    ChatMessage u; u.role = "user"; u.content = std::move(user_msg);
    p.messages.push_back(u);
    return p;
}

} // namespace

TEST(SchemaProviderResponsesStream, TextDeltasAccumulateAndUsageCaptured) {
    ResponsesMock mock{
        // 3 text deltas: "Hello", " ", "world!"
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
              "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
              "\"role\":\"assistant\"}}\n"
        "\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\","
              "\"delta\":\"Hello\"}\n"
        "\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\","
              "\"delta\":\" \"}\n"
        "\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\","
              "\"delta\":\"world!\"}\n"
        "\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0}\n"
        "\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\","
              "\"response\":{\"id\":\"resp_1\","
              "\"usage\":{\"input_tokens\":42,\"output_tokens\":18,"
                         "\"total_tokens\":60}}}\n"
        "\n"};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    auto p = params_with("hi");

    std::string streamed;
    int chunk_count = 0;
    auto r = provider->complete_stream(p,
        [&](const std::string& tok) { streamed += tok; ++chunk_count; });

    EXPECT_EQ(3, chunk_count) << "one on_chunk per output_text.delta";
    EXPECT_EQ("Hello world!", streamed);
    EXPECT_EQ("Hello world!", r.message.content);
    EXPECT_EQ(0u, r.message.tool_calls.size());

    // Regression: action="done" handler used to be empty, so this
    // would land at 0/0/0 even though response.completed carried it.
    EXPECT_EQ(42, r.usage.prompt_tokens);
    EXPECT_EQ(18, r.usage.completion_tokens);
    EXPECT_EQ(60, r.usage.total_tokens);
    EXPECT_EQ("end_turn", r.stop_reason);
}

TEST(SchemaProviderResponsesStream,
     OpenRouterDataOnlyEventsDispatchAndTerminateAtResponseDone) {
    ResponsesMock mock{
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
              "\"item\":{\"type\":\"message\",\"id\":\"msg_1\","
              "\"role\":\"assistant\"}}\n\n"
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\"Open\"}\n\n"
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\"Router\"}\n\n"
        "data: {\"type\":\"response.done\","
              "\"response\":{\"status\":\"completed\","
              "\"usage\":{\"input_tokens\":4,\"output_tokens\":2,"
              "\"total_tokens\":6}}}\n\n"
        // A terminal event must prevent unrelated trailing data from being read.
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\" ignored\"}\n\n"
        "data: [DONE]\n\n"};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    auto p = params_with("hi");

    std::string streamed;
    auto r = provider->complete_stream(
        p, [&](const std::string& token) { streamed += token; });

    EXPECT_EQ("OpenRouter", streamed);
    EXPECT_EQ("OpenRouter", r.message.content);
    EXPECT_EQ(4, r.usage.prompt_tokens);
    EXPECT_EQ(2, r.usage.completion_tokens);
    EXPECT_EQ(6, r.usage.total_tokens);
}

TEST(SchemaProviderResponsesStream, DoneSentinelTerminatesDataOnlyStream) {
    ResponsesMock mock{
        "data: {\"type\":\"response.output_item.added\","
              "\"item\":{\"type\":\"message\"}}\n\n"
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\"done\"}\n\n"
        "data: [DONE]\n\n"
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\" ignored\"}\n\n"};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    auto r = provider->complete_stream(params_with("hi"), nullptr);

    EXPECT_EQ("done", r.message.content);
}

TEST(SchemaProviderResponsesStream, ExplicitEventTakesPriorityOverPayloadType) {
    ResponsesMock mock{
        "event: response.output_item.added\n"
        "data: {\"type\":\"response.output_item.added\","
              "\"item\":{\"type\":\"message\"}}\n\n"
        "event: response.output_text.delta\n"
        "data: {\"type\":\"unrelated.payload.type\","
              "\"delta\":\"explicit\"}\n\n"
        "data: {\"type\":\"response.content_part.delta\","
              "\"delta\":\" fallback\"}\n\n"
        "data: [DONE]\n\n"};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    auto r = provider->complete_stream(params_with("hi"), nullptr);

    EXPECT_EQ("explicit fallback", r.message.content);
}

TEST(SchemaProviderResponsesStream, ErrorStatusCannotBeMaskedByTerminalPayload) {
    ResponsesMock mock{
        "data: {\"type\":\"response.done\","
        "\"response\":{\"status\":\"completed\"}}\n\n",
        503};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    try {
        (void)provider->complete_stream(params_with("hi"), nullptr);
        FAIL() << "expected HTTP status failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("HTTP 503"), std::string::npos);
    }
}

TEST(SchemaProviderResponsesStream, RejectsOversizedSseLine) {
    ResponsesMock mock{
        "data: {\"type\":\"response.content_part.delta\","
        "\"delta\":\"too-long\"}\n\n"};
    ASSERT_GT(mock.port, 0);

    auto config = cfg_for(mock.port);
    config.max_stream_line_bytes = 24;
    config.max_stream_response_bytes = 1024;
    auto provider = llm::SchemaProvider::create(config);
    EXPECT_THROW(provider->complete_stream(params_with("hi"), nullptr),
                 std::length_error);
}

TEST(SchemaProviderResponsesStream, RejectsOversizedAggregateStream) {
    ResponsesMock mock{
        "data: {\"type\":\"response.output_item.added\","
        "\"item\":{\"type\":\"message\"}}\n\n"
        "data: {\"type\":\"response.content_part.delta\","
        "\"delta\":\"bounded\"}\n\n"};
    ASSERT_GT(mock.port, 0);

    auto config = cfg_for(mock.port);
    config.max_stream_line_bytes = 120;
    config.max_stream_response_bytes = 120;
    auto provider = llm::SchemaProvider::create(config);
    try {
        (void)provider->complete_stream(params_with("hi"), nullptr);
        FAIL() << "expected aggregate stream limit";
    } catch (const std::length_error& error) {
        EXPECT_NE(std::string(error.what()).find("response limit"),
                  std::string::npos);
    }
}

TEST(SchemaProviderResponsesStream,
     FunctionCallArgumentsConcatenateVerbatim) {
    // get_weather({"city":"Tokyo","unit":"C"}) — args split across
    // 4 deltas to exercise the slice-concat path.
    ResponsesMock mock{
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,"
              "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\","
              "\"call_id\":\"call_abc\",\"name\":\"get_weather\","
              "\"arguments\":\"\"}}\n"
        "\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
              "\"item_id\":\"fc_1\",\"delta\":\"{\\\"city\\\":\"}\n"
        "\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
              "\"item_id\":\"fc_1\",\"delta\":\"\\\"Tokyo\\\"\"}\n"
        "\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
              "\"item_id\":\"fc_1\",\"delta\":\",\\\"unit\\\":\"}\n"
        "\n"
        "data: {\"type\":\"response.function_call_arguments.delta\","
              "\"item_id\":\"fc_1\",\"delta\":\"\\\"C\\\"}\"}\n"
        "\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,"
              "\"item\":{\"type\":\"function_call\",\"id\":\"fc_1\","
              "\"call_id\":\"call_abc\",\"name\":\"get_weather\","
              "\"arguments\":\"{\\\"city\\\":\\\"Tokyo\\\","
                              "\\\"unit\\\":\\\"C\\\"}\"}}\n"
        "\n"
        "data: {\"type\":\"response.completed\","
              "\"response\":{\"id\":\"resp_1\","
              "\"usage\":{\"input_tokens\":12,\"output_tokens\":24,"
                         "\"total_tokens\":36}}}\n"
        "\n"};
    ASSERT_GT(mock.port, 0);

    auto provider = llm::SchemaProvider::create(cfg_for(mock.port));
    auto p = params_with("weather in Tokyo?");

    int chunk_count = 0;
    auto r = provider->complete_stream(p,
        [&](const std::string&) { ++chunk_count; });

    // Tool-only streams emit no output_text.delta, so on_chunk
    // should never fire. (If a callback assumes "always called at
    // least once", the agent loop will deadlock on tool requests.)
    EXPECT_EQ(0, chunk_count);
    EXPECT_EQ("", r.message.content);

    ASSERT_EQ(1u, r.message.tool_calls.size());
    EXPECT_EQ("call_abc",     r.message.tool_calls[0].id);
    EXPECT_EQ("get_weather",  r.message.tool_calls[0].name);
    EXPECT_EQ(R"({"city":"Tokyo","unit":"C"})",
              r.message.tool_calls[0].arguments)
        << "args slices must concatenate exactly with no padding "
           "or re-quoting";

    EXPECT_EQ(12, r.usage.prompt_tokens);
    EXPECT_EQ(24, r.usage.completion_tokens);
    EXPECT_EQ(36, r.usage.total_tokens);
}

TEST(SchemaProviderResponsesStream, FunctionCallOutputIsFlatOnTheFollowupTurn) {
    auto provider = llm::SchemaProvider::create(cfg_for(1));

    CompletionParams p;
    p.model = "gpt-test";
    p.messages.push_back(ChatMessage{"user", "weather?"});

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls.push_back(
        ToolCall{"call_abc", "get_weather", R"({"city":"Tokyo"})"});
    p.messages.push_back(std::move(assistant));

    ChatMessage tool;
    tool.role = "tool";
    tool.content = R"({"temperature":24})";
    tool.tool_call_id = "call_abc";
    p.messages.push_back(std::move(tool));

    const auto body = llm::test_access::SchemaProviderTestAccess::build_body(
        *provider, p);
    ASSERT_TRUE(body.contains("input"));
    ASSERT_TRUE(body["input"].is_array());

    std::optional<json> function_call;
    std::optional<json> function_output;
    for (const auto item : body["input"]) {
        if (item.value("type", "") == "function_call") function_call = item;
        if (item.value("type", "") == "function_call_output") function_output = item;
    }
    ASSERT_TRUE(function_call.has_value());
    ASSERT_TRUE(function_output.has_value());
    EXPECT_EQ("call_abc", function_call->at("call_id").get<std::string>());
    EXPECT_EQ("call_abc", function_output->at("call_id").get<std::string>());
    EXPECT_EQ(R"({"temperature":24})",
              function_output->at("output").get<std::string>());
}
