// Provider reasoning items must survive a tool turn: a response captures the
// schema-declared items verbatim into ChatMessage::reasoning_details, and the
// next request replays them on the assistant message that produced them.
//
// The interpreter knows no vendor names. Each built-in schema declares a
// `reasoning` section, and these tests drive the real built-in schemas.
//
// Wire shapes follow the vendors' current API documentation and were checked
// against the live APIs (Anthropic thinking + signature, OpenAI Responses
// reasoning items, Gemini 3 thoughtSignature, OpenRouter reasoning_details).

#include <gtest/gtest.h>

#include <neograph/llm/schema_provider.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using neograph::ChatMessage;
using neograph::CompletionParams;
using neograph::json;
using neograph::ToolCall;
using neograph::llm::SchemaProvider;
using neograph::llm::test_access::SchemaProviderTestAccess;

namespace {

std::unique_ptr<SchemaProvider> builtin(const std::string& name) {
    SchemaProvider::Config cfg;
    cfg.schema_path = name;
    cfg.api_key     = "test-key";
    auto sp         = SchemaProvider::create(cfg);
    EXPECT_NE(sp, nullptr);
    return sp;
}

// user -> assistant(tool call + carried reasoning) -> tool result
CompletionParams tool_turn(const ChatMessage& assistant, const std::string& tool_name) {
    CompletionParams p;
    p.model = "test-model";
    p.messages.push_back({"user", "What is the weather in Seoul?"});
    p.messages.push_back(assistant);
    ChatMessage result;
    result.role         = "tool";
    result.tool_call_id = assistant.tool_calls.at(0).id;
    result.tool_name    = tool_name;
    result.content      = "18C, clear";
    p.messages.push_back(result);
    return p;
}

const json kThinking = {{"type", "thinking"},
                        {"thinking", "The user wants weather; call the tool."},
                        {"signature", "EqQBCkYIBRgCIkD-opaque-signature=="}};
const json kRedacted = {{"type", "redacted_thinking"}, {"data", "EmwKAhgBEgy3opaque"}};

}  // namespace

// ─── Anthropic (content-array blocks) ───

TEST(ReasoningCarryClaude, ThinkingBlocksAreCapturedVerbatimAndKeptOutOfContent) {
    auto sp = builtin("claude");
    const json response = {
        {"role", "assistant"},
        {"content",
         json::array({kThinking, kRedacted,
                      {{"type", "tool_use"},
                       {"id", "toolu_01"},
                       {"name", "get_weather"},
                       {"input", {{"city", "Seoul"}}}}})},
        {"stop_reason", "tool_use"}};

    const ChatMessage msg = SchemaProviderTestAccess::parse_response(*sp, response);

    ASSERT_EQ(msg.reasoning_details.size(), 2u);
    EXPECT_EQ(msg.reasoning_details[0], kThinking);  // byte-for-byte, incl. signature
    EXPECT_EQ(msg.reasoning_details[1], kRedacted);
    EXPECT_EQ(msg.reasoning, "The user wants weather; call the tool.");
    EXPECT_TRUE(msg.content.empty());
    ASSERT_EQ(msg.tool_calls.size(), 1u);
    EXPECT_EQ(msg.tool_calls[0].name, "get_weather");
}

TEST(ReasoningCarryClaude, CarriedBlocksAreReplayedFirstInTheAssistantTurn) {
    auto sp = builtin("claude");
    ChatMessage assistant;
    assistant.role              = "assistant";
    assistant.content           = "Let me check.";
    assistant.tool_calls        = {ToolCall{"toolu_01", "get_weather", R"({"city":"Seoul"})"}};
    assistant.reasoning_details = json::array({kThinking, kRedacted});

    const json body = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json& turn = body.at("messages").at(1);

    ASSERT_EQ(turn.at("role"), "assistant");
    const json& content = turn.at("content");
    ASSERT_EQ(content.size(), 4u) << content.dump();
    EXPECT_EQ(content[0], kThinking) << "thinking must come first and stay unmodified";
    EXPECT_EQ(content[1], kRedacted);
    EXPECT_EQ(content[2].at("type"), "text");
    EXPECT_EQ(content[3].at("type"), "tool_use");
    EXPECT_EQ(content[3].at("id"), "toolu_01");
}

TEST(ReasoningCarryClaude, ItemsFromAnotherRouteAreNotReplayed) {
    auto sp = builtin("claude");
    ChatMessage assistant;
    assistant.role       = "assistant";
    assistant.tool_calls = {ToolCall{"toolu_01", "get_weather", "{}"}};
    // A Responses reasoning item and an OpenRouter detail must never reach Anthropic.
    assistant.reasoning_details = json::array(
        {json{{"type", "reasoning"}, {"id", "rs_1"}, {"encrypted_content", "x"}},
         json{{"type", "reasoning.encrypted"}, {"data", "y"}}});

    const json body    = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json content = body.at("messages").at(1).at("content");
    ASSERT_EQ(content.size(), 1u) << content.dump();
    EXPECT_EQ(content[0].at("type"), "tool_use");
}

#ifndef _WIN32  // these two tests write a temporary schema file
TEST(ReasoningCarryClaude, SchemaWithoutAReasoningSectionBehavesAsBefore) {
    // Back-compat: no `reasoning` section -> thinking blocks are ignored on the
    // way in and nothing is replayed on the way out.
    const json schema = json::parse(R"({
      "name": "claude_like",
      "connection": {"base_url": "http://127.0.0.1:9", "endpoint": "/v1/messages",
                     "auth_header": "x-api-key", "auth_prefix": ""},
      "request": {"model_field": "model", "messages_field": "messages", "tools_field": "tools",
                  "temperature_path": null, "max_tokens_path": "max_tokens",
                  "max_tokens_default": 64, "stream_field": "stream"},
      "system_prompt": {"strategy": "top_level", "field": "system"},
      "messages": {"role_field": "role", "content_field": "content",
                   "role_map": {"system": "user", "user": "user", "assistant": "assistant", "tool": "user"},
                   "content_is_parts": false},
      "tool_definition": {"wrapper": "none", "name_field": "name",
                          "description_field": "description", "parameters_field": "input_schema"},
      "tool_call_in_message": {"strategy": "content_array",
                               "text_item": {"type": "text", "text": "$TEXT"},
                               "item": {"type": "tool_use", "id": "$ID", "name": "$NAME", "input": "$ARGUMENTS_OBJECT"}},
      "tool_result": {"role": "user", "strategy": "content_array",
                      "item": {"type": "tool_result", "tool_use_id": "$ID", "content": "$CONTENT"}},
      "image": {"strategy": "none"},
      "response": {"strategy": "content_array", "content_path": "content", "text_type": "text",
                   "text_field": "text", "tool_use_type": "tool_use", "tool_call_id_field": "id",
                   "tool_call_name_field": "name", "tool_call_args_field": "input",
                   "tool_call_args_is_string": false},
      "stream": {"format": "sse_events"}
    })");
    char path[] = "/tmp/neograph_reasoning_carry_XXXXXX.json";
    const int fd = mkstemps(path, 5);
    ASSERT_GE(fd, 0);
    close(fd);
    { std::ofstream(path) << schema.dump(); }
    SchemaProvider::Config cfg;
    cfg.schema_path             = path;
    cfg.api_key                 = "test-key";
    cfg.allow_insecure_loopback = true;
    auto sp                     = SchemaProvider::create(cfg);
    std::remove(path);
    ASSERT_NE(sp, nullptr);

    const json response = {{"role", "assistant"},
                           {"content", json::array({kThinking,
                                                    {{"type", "tool_use"}, {"id", "t1"},
                                                     {"name", "get_weather"}, {"input", json::object()}}})}};
    const ChatMessage msg = SchemaProviderTestAccess::parse_response(*sp, response);
    EXPECT_TRUE(msg.reasoning_details.empty());
    EXPECT_TRUE(msg.reasoning.empty());

    ChatMessage assistant;
    assistant.role              = "assistant";
    assistant.tool_calls        = {ToolCall{"t1", "get_weather", "{}"}};
    assistant.reasoning_details = json::array({kThinking});
    const json body    = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json content = body.at("messages").at(1).at("content");
    ASSERT_EQ(content.size(), 1u) << content.dump();
    EXPECT_EQ(content[0].at("type"), "tool_use");
}

TEST(ReasoningCarrySchema, MalformedReasoningSectionIsRejectedAtCreation) {
    for (const char* bad : {R"("carry_types": "thinking")", R"("carry_types": [1])",
                            R"("carry_types": [""])", R"("text_field": 5)"}) {
        // Reuse a working built-in shape by patching a copy written to disk.
        const std::string schema = std::string(R"({"name":"x","connection":{"base_url":"http://127.0.0.1:9","endpoint":"/e"},)") +
            R"("request":{"messages_field":"messages"},"system_prompt":{"strategy":"in_messages"},)" +
            R"("messages":{"role_field":"role","content_field":"content"},"tool_call_in_message":{"strategy":"tool_calls_array"},)" +
            R"("tool_result":{"strategy":"flat"},"image":{"strategy":"none"},)" +
            R"("response":{"strategy":"choices_message","message_path":"choices.0.message"},"stream":{"format":"sse_data"},)" +
            R"("reasoning":{)" + bad + R"(}})";
        char path[] = "/tmp/neograph_reasoning_bad_XXXXXX.json";
        const int fd = mkstemps(path, 5);
        ASSERT_GE(fd, 0);
        close(fd);
        { std::ofstream(path) << schema; }
        SchemaProvider::Config cfg;
        cfg.schema_path             = path;
        cfg.api_key                 = "k";
        cfg.allow_insecure_loopback = true;
        EXPECT_THROW((void)SchemaProvider::create(cfg), std::invalid_argument) << bad;
        std::remove(path);
    }
}
#endif  // !_WIN32

// ─── OpenAI Responses (output[] items) ───

TEST(ReasoningCarryResponses, ReasoningItemsAreCapturedAndSummaryBecomesReasoningText) {
    auto sp = builtin("openai_responses");
    const json item = {{"type", "reasoning"},
                       {"id", "rs_0123"},
                       {"encrypted_content", "gAAAA-opaque"},
                       {"summary", json::array({{{"type", "summary_text"}, {"text", "Checking the forecast."}}})}};
    const json response = {
        {"output",
         json::array({item,
                      {{"type", "function_call"},
                       {"call_id", "call_1"},
                       {"name", "get_weather"},
                       {"arguments", R"({"city":"Seoul"})"}}})}};

    const ChatMessage msg = SchemaProviderTestAccess::parse_response(*sp, response);

    ASSERT_EQ(msg.reasoning_details.size(), 1u);
    EXPECT_EQ(msg.reasoning_details[0], item);
    EXPECT_EQ(msg.reasoning, "Checking the forecast.");
    EXPECT_TRUE(msg.content.empty());
    ASSERT_EQ(msg.tool_calls.size(), 1u);
    EXPECT_EQ(msg.tool_calls[0].id, "call_1");
}

TEST(ReasoningCarryResponses, ReasoningItemPrecedesItsFunctionCallOnReplay) {
    auto sp = builtin("openai_responses");
    const json item = {{"type", "reasoning"}, {"id", "rs_0123"}, {"encrypted_content", "gAAAA"}, {"summary", json::array()}};
    ChatMessage assistant;
    assistant.role              = "assistant";
    assistant.tool_calls        = {ToolCall{"call_1", "get_weather", R"({"city":"Seoul"})"}};
    assistant.reasoning_details = json::array({item});

    const json body = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json& input = body.at("input");

    ASSERT_EQ(input.size(), 4u) << input.dump();
    EXPECT_EQ(input[1], item);
    EXPECT_EQ(input[2].at("type"), "function_call");
    EXPECT_EQ(input[2].at("call_id"), "call_1");
    EXPECT_EQ(input[3].at("type"), "function_call_output");
}

// ─── Gemini (parts with thoughtSignature) ───

TEST(ReasoningCarryGemini, ThoughtPartsLeaveContentAndSignatureIsKeyedByToolCall) {
    auto sp = builtin("gemini");
    const json response = {
        {"candidates",
         json::array({{{"content",
                        {{"role", "model"},
                         {"parts",
                          json::array({{{"text", "Planning the weather lookup."}, {"thought", true}},
                                       {{"functionCall", {{"name", "get_weather"}, {"args", {{"city", "Seoul"}}}}},
                                        {"thoughtSignature", "CiQBjz1rX-opaque-signature"}}})}}},
                       {"finishReason", "STOP"}}})}};

    const ChatMessage msg = SchemaProviderTestAccess::parse_response(*sp, response);

    EXPECT_TRUE(msg.content.empty()) << "a thought part must never become user-visible content";
    EXPECT_EQ(msg.reasoning, "Planning the weather lookup.");
    ASSERT_EQ(msg.tool_calls.size(), 1u);
    ASSERT_EQ(msg.reasoning_details.size(), 1u);
    EXPECT_EQ(msg.reasoning_details[0].at("type"), "tool_call_signature");
    EXPECT_EQ(msg.reasoning_details[0].at("tool_call_id"), msg.tool_calls[0].id);
    EXPECT_EQ(msg.reasoning_details[0].at("signature"), "CiQBjz1rX-opaque-signature");
}

TEST(ReasoningCarryGemini, SignatureIsEchoedOnTheSameFunctionCallPart) {
    auto sp = builtin("gemini");
    ChatMessage assistant;
    assistant.role       = "assistant";
    assistant.tool_calls = {ToolCall{"call_a", "get_weather", R"({"city":"Seoul"})"},
                            ToolCall{"call_b", "get_time", "{}"}};
    // Only the first call carries a signature (Gemini attaches it to the first
    // functionCall of a parallel set).
    assistant.reasoning_details = json::array(
        {json{{"type", "tool_call_signature"}, {"tool_call_id", "call_a"}, {"signature", "SIG-A"}}});

    const json body = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json parts = body.at("contents").at(1).at("parts");

    ASSERT_EQ(parts.size(), 2u) << parts.dump();
    EXPECT_EQ(parts[0].at("functionCall").at("name"), "get_weather");
    EXPECT_EQ(parts[0].at("thoughtSignature"), "SIG-A");
    EXPECT_EQ(parts[1].at("functionCall").at("name"), "get_time");
    EXPECT_FALSE(parts[1].contains("thoughtSignature")) << "no signature was captured for call_b";
}

// ─── OpenRouter / chat completions (opaque message array) ───

TEST(ReasoningCarryChat, ReasoningDetailsRoundTripVerbatim) {
    auto sp = builtin("openai");
    const json details = json::array(
        {json{{"type", "reasoning.summary"}, {"summary", "Thinking about it."}, {"index", 0}},
         json{{"type", "reasoning.encrypted"}, {"data", "opaque-encrypted-block"}, {"index", 1}}});
    const json response = {
        {"choices",
         json::array({{{"message",
                        {{"role", "assistant"},
                         {"content", nullptr},
                         {"reasoning_details", details},
                         {"tool_calls",
                          json::array({{{"id", "call_1"},
                                        {"type", "function"},
                                        {"function", {{"name", "get_weather"}, {"arguments", "{}"}}}}})}}},
                       {"finish_reason", "tool_calls"}}})}};

    const ChatMessage msg = SchemaProviderTestAccess::parse_response(*sp, response);
    EXPECT_EQ(msg.reasoning_details, details);

    ChatMessage assistant  = msg;
    const json body        = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    const json& replayed   = body.at("messages").at(1);
    ASSERT_TRUE(replayed.contains("reasoning_details")) << replayed.dump();
    EXPECT_EQ(replayed.at("reasoning_details"), details);
}

TEST(ReasoningCarryChat, NoDetailsMeansNoExtraFieldOnTheWire) {
    auto sp = builtin("openai");
    ChatMessage assistant;
    assistant.role       = "assistant";
    assistant.tool_calls = {ToolCall{"call_1", "get_weather", "{}"}};
    const json body      = SchemaProviderTestAccess::build_body(*sp, tool_turn(assistant, "get_weather"));
    EXPECT_FALSE(body.at("messages").at(1).contains("reasoning_details"));
}

// ─── Streaming: the same items must be reconstructed from the wire fragments ───

namespace {

std::string data_line(const json& j) { return "data: " + j.dump(); }
std::string event_line(const std::string& name) { return "event: " + name; }

}  // namespace

TEST(ReasoningCarryStream, ClaudeThinkingIsAssembledFromDeltasWithItsSignature) {
    auto sp = builtin("claude");
    const std::vector<std::string> lines = {
        event_line("message_start"),
        data_line({{"type", "message_start"}, {"message", {{"usage", {{"input_tokens", 10}, {"output_tokens", 1}}}}}}),
        event_line("content_block_start"),
        data_line({{"type", "content_block_start"}, {"index", 0},
                   {"content_block", {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}}}}),
        event_line("content_block_delta"),
        data_line({{"type", "content_block_delta"}, {"index", 0},
                   {"delta", {{"type", "thinking_delta"}, {"thinking", "Let me "}}}}),
        event_line("content_block_delta"),
        data_line({{"type", "content_block_delta"}, {"index", 0},
                   {"delta", {{"type", "thinking_delta"}, {"thinking", "think."}}}}),
        event_line("content_block_delta"),
        data_line({{"type", "content_block_delta"}, {"index", 0},
                   {"delta", {{"type", "signature_delta"}, {"signature", "SIG=="}}}}),
        event_line("content_block_stop"),
        data_line({{"type", "content_block_stop"}, {"index", 0}}),
        event_line("content_block_start"),
        data_line({{"type", "content_block_start"}, {"index", 1},
                   {"content_block", {{"type", "tool_use"}, {"id", "toolu_9"}, {"name", "get_weather"}, {"input", json::object()}}}}),
        event_line("content_block_delta"),
        data_line({{"type", "content_block_delta"}, {"index", 1},
                   {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"city\":\"Seoul\"}"}}}}),
        event_line("content_block_stop"),
        data_line({{"type", "content_block_stop"}, {"index", 1}}),
        event_line("message_delta"),
        data_line({{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}, {"usage", {{"output_tokens", 7}}}}),
        event_line("message_stop"),
        data_line({{"type", "message_stop"}}),
    };
    std::string streamed;
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, lines, [&streamed](const std::string& s) { streamed += s; });

    ASSERT_EQ(completion.message.reasoning_details.size(), 1u);
    EXPECT_EQ(completion.message.reasoning_details[0],
              (json{{"type", "thinking"}, {"thinking", "Let me think."}, {"signature", "SIG=="}}));
    EXPECT_EQ(completion.message.reasoning, "Let me think.");
    EXPECT_TRUE(streamed.empty()) << "thinking text must never reach on_chunk: " << streamed;
    EXPECT_TRUE(completion.message.content.empty());
    ASSERT_EQ(completion.message.tool_calls.size(), 1u);
    EXPECT_EQ(completion.message.tool_calls[0].id, "toolu_9");
    EXPECT_EQ(completion.message.tool_calls[0].arguments, R"({"city":"Seoul"})");
}

TEST(ReasoningCarryStream, ClaudeRedactedThinkingBlockIsCarriedFromItsStartEvent) {
    auto sp = builtin("claude");
    const json block = {{"type", "redacted_thinking"}, {"data", "EmwKAhgBopaque"}};
    const std::vector<std::string> lines = {
        event_line("content_block_start"),
        data_line({{"type", "content_block_start"}, {"index", 0}, {"content_block", block}}),
        event_line("content_block_stop"),
        data_line({{"type", "content_block_stop"}, {"index", 0}}),
        event_line("message_stop"),
        data_line({{"type", "message_stop"}}),
    };
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(*sp, lines);
    ASSERT_EQ(completion.message.reasoning_details.size(), 1u);
    EXPECT_EQ(completion.message.reasoning_details[0], block);
}

TEST(ReasoningCarryStream, ResponsesReasoningItemComesVerbatimFromOutputItemDone) {
    auto sp = builtin("openai_responses");
    const json done_item = {{"type", "reasoning"}, {"id", "rs_77"}, {"encrypted_content", "gAAAA-final"},
                            {"summary", json::array({{{"type", "summary_text"}, {"text", "Checked the forecast."}}})}};
    const std::vector<std::string> lines = {
        data_line({{"type", "response.output_item.added"}, {"item", {{"type", "reasoning"}, {"id", "rs_77"}, {"summary", json::array()}}}}),
        data_line({{"type", "response.output_item.done"}, {"item", done_item}}),
        data_line({{"type", "response.output_item.added"},
                   {"item", {{"type", "function_call"}, {"call_id", "call_1"}, {"name", "get_weather"}, {"arguments", ""}}}}),
        data_line({{"type", "response.function_call_arguments.delta"}, {"delta", "{\"city\":\"Seoul\"}"}}),
        data_line({{"type", "response.output_item.done"},
                   {"item", {{"type", "function_call"}, {"call_id", "call_1"}, {"name", "get_weather"}, {"arguments", "{\"city\":\"Seoul\"}"}}}}),
        data_line({{"type", "response.completed"}, {"response", {{"usage", {{"input_tokens", 5}, {"output_tokens", 3}}}}}}),
    };
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(*sp, lines);

    ASSERT_EQ(completion.message.reasoning_details.size(), 1u);
    EXPECT_EQ(completion.message.reasoning_details[0], done_item) << "the complete item, not the partial added-event";
    EXPECT_EQ(completion.message.reasoning, "Checked the forecast.");
    ASSERT_EQ(completion.message.tool_calls.size(), 1u);
    EXPECT_EQ(completion.message.tool_calls[0].id, "call_1");
}

TEST(ReasoningCarryStream, ResponsesWebSocketCarriesTheSameItem) {
    auto sp = builtin("openai_responses");
    const json done_item = {{"type", "reasoning"}, {"id", "rs_ws"}, {"encrypted_content", "gAAAA-ws"}, {"summary", json::array()}};
    const std::vector<json> events = {
        {{"type", "response.output_item.added"}, {"item", {{"type", "reasoning"}, {"id", "rs_ws"}}}},
        {{"type", "response.output_item.done"}, {"item", done_item}},
        {{"type", "response.completed"}, {"response", {{"usage", {{"input_tokens", 2}, {"output_tokens", 2}}}}}},
    };
    const auto completion = SchemaProviderTestAccess::parse_ws_events(*sp, events);
    ASSERT_EQ(completion.message.reasoning_details.size(), 1u);
    EXPECT_EQ(completion.message.reasoning_details[0], done_item);
}

TEST(ReasoningCarryStream, GeminiThoughtsStayOutOfContentAndSignatureFollowsTheToolCall) {
    auto sp = builtin("gemini");
    const auto chunk = [](const json& parts) {
        return data_line({{"candidates", json::array({{{"content", {{"role", "model"}, {"parts", parts}}}}})}});
    };
    const std::vector<std::string> lines = {
        chunk(json::array({{{"text", "Planning. "}, {"thought", true}}})),
        chunk(json::array({{{"functionCall", {{"name", "get_weather"}, {"args", {{"city", "Seoul"}}}}},
                            {"thoughtSignature", "SIG-STREAM"}}})),
        chunk(json::array({{{"text", "Done."}}})),
    };
    std::string streamed;
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, lines, [&streamed](const std::string& s) { streamed += s; });

    EXPECT_EQ(completion.message.reasoning, "Planning. ");
    EXPECT_EQ(streamed, "Done.") << "a thought part must not be streamed as content";
    EXPECT_EQ(completion.message.content, "Done.");
    ASSERT_EQ(completion.message.tool_calls.size(), 1u);
    ASSERT_EQ(completion.message.reasoning_details.size(), 1u);
    EXPECT_EQ(completion.message.reasoning_details[0].at("tool_call_id"), completion.message.tool_calls[0].id);
    EXPECT_EQ(completion.message.reasoning_details[0].at("signature"), "SIG-STREAM");
}

TEST(ReasoningCarryStream, OpenRouterFragmentsAreMergedPerIndexAndKeepOrder) {
    auto sp = builtin("openai");
    const auto delta = [](const json& d) {
        return data_line({{"choices", json::array({{{"delta", d}}})}});
    };
    const std::vector<std::string> lines = {
        delta({{"reasoning", "Hello "}, {"reasoning_details", json::array({{{"type", "reasoning.summary"}, {"summary", "Hello "}, {"format", "fmt-v1"}, {"index", 0}}})}}),
        delta({{"reasoning", "world"}, {"reasoning_details", json::array({{{"type", "reasoning.summary"}, {"summary", "world"}, {"format", "fmt-v1"}, {"index", 0}}})}}),
        delta({{"reasoning_details", json::array({{{"type", "reasoning.encrypted"}, {"data", "AB"}, {"format", "fmt-v1"}, {"index", 1}}})}}),
        delta({{"reasoning_details", json::array({{{"type", "reasoning.encrypted"}, {"data", "CD"}, {"format", "fmt-v1"}, {"index", 1}}})}}),
        delta({{"content", "ok"}}),
        "data: [DONE]",
    };
    std::string streamed;
    const auto completion = SchemaProviderTestAccess::parse_stream_lines(
        *sp, lines, [&streamed](const std::string& s) { streamed += s; });

    ASSERT_EQ(completion.message.reasoning_details.size(), 2u) << completion.message.reasoning_details.dump();
    EXPECT_EQ(completion.message.reasoning_details[0],
              (json{{"type", "reasoning.summary"}, {"summary", "Hello world"}, {"format", "fmt-v1"}, {"index", 0}}));
    EXPECT_EQ(completion.message.reasoning_details[1],
              (json{{"type", "reasoning.encrypted"}, {"data", "ABCD"}, {"format", "fmt-v1"}, {"index", 1}}));
    EXPECT_EQ(completion.message.reasoning, "Hello world") << "delta.reasoning must be read on the stream path";
    EXPECT_EQ(streamed, "ok");
}
