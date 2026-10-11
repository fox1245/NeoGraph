// Tool-result grouping across provider families.
//
// NeoGraph records one Role::Tool message per client tool result. Anthropic
// Messages and Gemini generateContent only accept the results of one assistant
// turn together in a single message, while openai.chat needs exactly one result
// per message and openai.responses / google.interactions take either shape.
// set_provider_request_messages() is the single boundary that adapts the stored
// history, so these tests drive the real Agent loop against a loopback peer and
// inspect prepared bodies for the other families.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/llm/agent.h>
#include "fixtures/typed_wire_peer.h"
#include <codecs/messages.h>
#include <codecs/gemini.h>
#include <core/native.h>

#include <array>
#include <algorithm>
#include <type_traits>
#include <memory>
#include <string_view>

using namespace neograph;
namespace wire = neograph::test::wire;

namespace {

using Calls = std::vector<std::pair<std::string, std::string>>;  // {call id, city}

// Provider responses carrying the given parallel client tool calls (or, with no
// calls, the final answer) for each family.
std::string reply(std::string_view family, const Calls& calls) {
    json out;
    if (family == "openai.chat") {
        if (calls.empty()) return wire::chat_response("both done");
        json list = json::array();
        for (const auto& [id, city] : calls)
            list.push_back({{"id", id}, {"type", "function"},
                {"function", {{"name", "lookup_weather"}, {"arguments", json{{"city", city}}.dump()}}}});
        return wire::chat_envelope(json{{"choices", json::array({{{"index", 0},
            {"message", {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", list}}},
            {"finish_reason", "tool_calls"}}})}}).dump();
    }
    if (family == "anthropic.messages") {
        json content = json::array();
        if (calls.empty()) content.push_back({{"type", "text"}, {"text", "both done"}});
        for (const auto& [id, city] : calls)
            content.push_back({{"type", "tool_use"}, {"id", id}, {"name", "lookup_weather"}, {"input", {{"city", city}}}});
        return json{{"id", "msg"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
            {"content", content}, {"stop_reason", calls.empty() ? "end_turn" : "tool_use"},
            {"usage", {{"input_tokens", 3}, {"output_tokens", 2}}}}.dump();
    }
    if (family == "openai.responses") {
        out = json::parse(wire::responses_body("both done"));
        if (calls.empty()) return out.dump();
        out["output"] = json::array();
        for (const auto& [id, city] : calls)
            out["output"].push_back({{"type", "function_call"}, {"id", "fc-" + id}, {"call_id", id},
                {"name", "lookup_weather"}, {"arguments", json{{"city", city}}.dump()}, {"status", "completed"}});
        return out.dump();
    }
    if (family == "google.generate") {
        json parts = json::array();
        if (calls.empty()) parts.push_back({{"text", "both done"}});
        for (const auto& [id, city] : calls)
            parts.push_back({{"functionCall", {{"id", id}, {"name", "lookup_weather"}, {"args", {{"city", city}}}}}});
        return json{{"candidates", json::array({{{"content", {{"role", "model"}, {"parts", parts}}},
            {"finishReason", "STOP"}}})}}.dump();
    }
    json steps = json::array();
    if (calls.empty()) steps.push_back({{"type", "model_output"}, {"content", json::array({{{"type", "text"}, {"text", "both done"}}})}});
    for (const auto& [id, city] : calls)
        steps.push_back({{"type", "function_call"}, {"id", id}, {"name", "lookup_weather"}, {"arguments", {{"city", city}}}});
    return json{{"id", "interaction"}, {"model", "fixture-model"}, {"status", calls.empty() ? "completed" : "requires_action"},
        {"steps", steps}, {"usage", {{"total_input_tokens", 4}, {"total_output_tokens", 2}, {"total_tokens", 6}}}}.dump();
}
const Calls kTwoCalls = {{"c1", "Seoul"}, {"c2", "Busan"}};
// Agent has no per-turn controls argument, so the fixture supplies explicit
// finite output limits and native replay scope before real SDK preparation.
class BoundedWireProvider final : public Provider {
public:
    BoundedWireProvider(std::string family, const std::string& origin)
        : provider_(wire::provider(std::move(family), origin)) {}
    std::string get_name() const override { return provider_->get_name(); }
    std::string_view family() const noexcept override { return provider_->family(); }
    PreparedProviderRequest prepare(ProviderRequest request) override {
        std::visit([](auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, sp::messages::Request>) payload.max_tokens = 64;
            else payload.max_output_tokens = 64;
            if constexpr (!std::is_same_v<T, sp::chat::Request>) payload.account_scope = "fixture-account";
        }, request.payload);
        return provider_->prepare(std::move(request));
    }
private:
    std::unique_ptr<llm::SchemaProvider> provider_;
};

// Answers with JSON objects (Gemini function responses must be objects) and
// flips the peer to its final reply, so the loop ends after exactly one
// round of results.
class WeatherTool : public Tool {
public:
    WeatherTool(std::shared_ptr<wire::State> state, std::string final_body)
        : state_(std::move(state)), final_body_(std::move(final_body)) {}
    ChatTool get_definition() const override {
        return {"lookup_weather", "Weather lookup", json{{"type", "object"},
            {"properties", {{"city", {{"type", "string"}}}}}, {"required", json::array({"city"})}}};
    }
    std::string execute(const json& args) override {
        { std::lock_guard lock(state_->mutex); state_->body = final_body_; }
        return json{{"city", args.at("city")}, {"temp", 20}}.dump();
    }
    std::string get_name() const override { return "lookup_weather"; }
private:
    std::shared_ptr<wire::State> state_;
    std::string final_body_;
};

sp::Message user_text(std::string text) { return test::message(std::move(text), sp::Role::User); }
// Hand-built history is enough wherever a request is never prepared.
sp::Message assistant_calls(const Calls& calls) {
    sp::Message message;
    message.role = sp::Role::Assistant;
    for (const auto& [id, city] : calls) {
        sp::ToolCall call;
        call.id = id;
        call.name = "lookup_weather";
        call.input = test::document(R"({"city":")" + city + R"("})");
        message.parts.emplace_back(std::move(call));
    }
    return message;
}
sp::Message tool_result(std::string id, std::string content = R"({"temp":20})", std::string status = "succeeded") {
    ChatMessage source;
    source.role = "tool";
    source.tool_call_id = std::move(id);
    source.tool_name = "lookup_weather";
    source.content = std::move(content);
    source.tool_status = std::move(status);
    return portable_message(source);
}
std::vector<ChatTool> weather_tools() { return {WeatherTool(nullptr, "").get_definition()}; }
ProviderControls bounded() {
    ProviderControls controls;
    controls.max_output_tokens = 64;
    controls.account_scope = "fixture-account";
    return controls;
}
// Capture genuine native authority using the SDK codecs, with no transport.
// Descriptor, controls and prefix match the subsequent prepared request.
sp::Message assistant_turn(std::string_view family, const Calls& calls, std::vector<sp::Message> prefix) {
    constexpr auto origin = "http://127.0.0.1:1";
    const auto descriptor = test::descriptor(std::string(family), origin);
    auto provider = wire::provider(std::string(family), origin);
    const auto request = make_provider_request(
        *provider, "fixture-model", std::move(prefix), weather_tools(), bounded());
    sp::Accumulator accumulator;
    if (family == "anthropic.messages") {
        auto encoded = sp::messages::encode(descriptor, std::get<sp::messages::Request>(request.payload), false);
        if (auto* error = std::get_if<sp::Error>(&encoded)) throw std::runtime_error(error->safe_message);
        sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator,
            std::get<sp::messages::EncodedRequest>(encoded).context);
        codec.buffered(reply(family, calls), {});
        codec.finish();
    } else {
        auto encoded = sp::gemini::encode(descriptor, std::get<sp::gemini::Request>(request.payload), false);
        if (auto* error = std::get_if<sp::Error>(&encoded)) throw std::runtime_error(error->safe_message);
        sp::gemini::Codec codec(descriptor, sp::gemini::Mode::Buffered, accumulator,
            std::get<sp::gemini::EncodedRequest>(encoded).context);
        codec.buffered(reply(family, calls), {});
        codec.finish();
    }
    const auto& result = accumulator.outcome();
    if (!result || !std::holds_alternative<sp::Completion>(*result))
        throw std::runtime_error(std::string(family) + ": fixture turn failed");
    return std::get<sp::Completion>(*result).messages.at(0);
}
json prepared_body(std::string_view family, std::vector<sp::Message> history, bool portable_foreign = false) {
    auto provider = wire::provider(std::string(family), "http://127.0.0.1:1");
    auto controls = bounded();
    if (portable_foreign) controls.gemini_history_mode = sp::gemini::HistoryMode::PortableForeign;
    auto prepared = provider->prepare(make_provider_request(
        *provider, "fixture-model", std::move(history), weather_tools(), std::move(controls)));
    if (!prepared.valid()) {
        ADD_FAILURE() << family << " rejected the request, error kind "
                      << (prepared.error() ? static_cast<int>(prepared.error()->kind) : -1);
        return json::object();
    }
    return json::parse(std::string(prepared.encoded_body()));
}

struct AgentRun {
    sp::runtime::Result result;
    std::vector<json> requests;
    std::vector<sp::Message> history;
};
// Runs the real Agent loop: the peer first answers with `calls`, and flips to
// the final answer when the tool executes, so the loop ends after one round.
AgentRun run_agent(std::string_view family, const Calls& calls) {
    wire::Peer peer(reply(family, calls));
    std::shared_ptr<Provider> provider = std::make_shared<BoundedWireProvider>(std::string(family), peer.origin());
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<WeatherTool>(peer.state, reply(family, {})));
    llm::Agent agent(provider, std::move(tools), "Be brief.", "fixture-model");
    std::vector<sp::Message> messages{user_text("weather in Seoul and Busan?")};
    AgentRun run;
    try { run.result = agent.run(messages, 3); }
    catch (const ProviderFailure& failure) { run.result = failure.outcome(); }
    std::lock_guard lock(peer.state->mutex);
    run.requests = peer.state->requests;
    run.history = std::move(messages);
    return run;
}
void expect_completed(const AgentRun& run) {
    ASSERT_TRUE(run.result);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*run.result))
        << "provider turn failed, error kind "
        << static_cast<int>(std::get<sp::Failure>(*run.result).error.kind)
        << " after " << run.requests.size() << " wire request(s)";
    EXPECT_EQ(test::text(run.result), "both done");
    ASSERT_EQ(run.requests.size(), 2u);
    std::vector<std::string> result_ids;
    for (const auto& message : run.history) {
        if (message.role != sp::Role::Tool) continue;
        ASSERT_EQ(message.parts.size(), 1u);
        ASSERT_TRUE(std::holds_alternative<sp::ToolResult>(message.parts[0]));
        result_ids.push_back(std::get<sp::ToolResult>(message.parts[0]).tool_use_id);
    }
    EXPECT_EQ(result_ids, (std::vector<std::string>{"c1", "c2"}));
    const auto native_turn = std::find_if(run.history.begin(), run.history.end(),
        [](const auto& message) { return !client_tool_calls(message).empty(); });
    ASSERT_NE(native_turn, run.history.end());
    ASSERT_TRUE(native_turn->native);
    EXPECT_TRUE(native_turn->native->complete());
}
}

TEST(ToolResultGrouping, AgentSendsParallelResultsInOneMessageOnAnthropicMessages) {
    const auto run = run_agent("anthropic.messages", kTwoCalls);
    ASSERT_NO_FATAL_FAILURE(expect_completed(run));
    const auto& messages = run.requests[1].at("messages");
    ASSERT_EQ(messages.size(), 3u);  // user, assistant(tool_use x2), user(tool_result x2)
    EXPECT_EQ(messages[2].at("role"), "user");
    const auto& blocks = messages[2].at("content");
    ASSERT_EQ(blocks.size(), 2u);
    EXPECT_EQ(blocks[0].at("type"), "tool_result");
    EXPECT_EQ(blocks[0].at("tool_use_id"), "c1");
    EXPECT_EQ(blocks[0].at("is_error"), false);
    EXPECT_EQ(blocks[1].at("type"), "tool_result");
    EXPECT_EQ(blocks[1].at("tool_use_id"), "c2");
}

TEST(ToolResultGrouping, AgentSendsParallelResultsInOneContentOnGoogleGenerate) {
    const auto run = run_agent("google.generate", kTwoCalls);
    ASSERT_NO_FATAL_FAILURE(expect_completed(run));
    const auto& contents = run.requests[1].at("contents");
    ASSERT_EQ(contents.size(), 3u);  // user, model(functionCall x2), user(functionResponse x2)
    EXPECT_EQ(contents[2].at("role"), "user");
    const auto& parts = contents[2].at("parts");
    ASSERT_EQ(parts.size(), 2u);
    EXPECT_EQ(parts[0].at("functionResponse").at("id"), "c1");
    EXPECT_EQ(parts[0].at("functionResponse").at("response").at("city"), "Seoul");
    EXPECT_EQ(parts[1].at("functionResponse").at("id"), "c2");
    EXPECT_EQ(parts[1].at("functionResponse").at("response").at("city"), "Busan");
}

TEST(ToolResultGrouping, OtherFamiliesStillAcceptOneResultPerMessage) {
    // Scope guard: the grouping must not widen to openai.chat (exactly one
    // result per Tool message) or to the families that accept either shape.
    {
        const auto run = run_agent("openai.chat", kTwoCalls);
        ASSERT_NO_FATAL_FAILURE(expect_completed(run));
        const auto& messages = run.requests[1].at("messages");
        std::vector<std::string> ids;
        for (const auto& message : messages)
            if (message.at("role") == "tool") ids.push_back(message.at("tool_call_id").get<std::string>());
        EXPECT_EQ(ids, (std::vector<std::string>{"c1", "c2"}));
    }
    {
        const auto run = run_agent("openai.responses", kTwoCalls);
        ASSERT_NO_FATAL_FAILURE(expect_completed(run));
        std::vector<std::string> ids;
        for (const auto& item : run.requests[1].at("input"))
            if (item.contains("type") && item.at("type") == "function_call_output") ids.push_back(item.at("call_id").get<std::string>());
        EXPECT_EQ(ids, (std::vector<std::string>{"c1", "c2"}));
    }
    ASSERT_NO_FATAL_FAILURE(expect_completed(run_agent("google.interactions", kTwoCalls)));
}

TEST(ToolResultGrouping, OnlyAdjacentResultOnlyMessagesAreMerged) {
    const std::vector<sp::Message> history = {
        user_text("first"), assistant_calls({{"a1", "Seoul"}}), tool_result("a1"),
        test::message("sunny", sp::Role::Assistant), user_text("second"),
        assistant_calls({{"b1", "Seoul"}, {"b2", "Busan"}, {"b3", "Jeju"}}),
        tool_result("b1"), tool_result("b2"), tool_result("b3")};
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        auto request = wire::request(family);
        set_provider_request_messages(request, history);
        const auto& stored = provider_request_messages(request);
        ASSERT_EQ(stored.size(), 7u);
        EXPECT_EQ(stored[2].parts.size(), 1u);
        EXPECT_EQ(stored[4].parts.size(), 1u);
        EXPECT_TRUE(std::holds_alternative<sp::Text>(stored[4].parts[0]));
        ASSERT_EQ(stored[6].parts.size(), 3u);
        EXPECT_EQ(std::get<sp::ToolResult>(stored[6].parts[2]).tool_use_id, "b3");
    }
}

TEST(ToolResultGrouping, MixedMessageNeitherAbsorbsNorIsAbsorbed) {
    sp::Message mixed;
    mixed.role = sp::Role::User;
    mixed.parts = {sp::ToolResult{"c2", R"({"temp":20})", false, {}}, sp::Text{"note"}};
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        auto request = wire::request(family);
        set_provider_request_messages(request, {user_text("q"), assistant_calls(kTwoCalls), tool_result("c1"), mixed});
        const auto& stored = provider_request_messages(request);
        ASSERT_EQ(stored.size(), 4u);
        EXPECT_EQ(stored[2].parts.size(), 1u);
        EXPECT_EQ(stored[3].parts.size(), 2u);
    }
}

TEST(ToolResultGrouping, GroupingKeepsOrderAndFirstMessageIdentity) {
    auto first = tool_result("c1");
    first.id = "first-id";
    auto second = tool_result("c2");
    second.id = "second-id";
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        auto request = wire::request(family);
        set_provider_request_messages(request, {user_text("q"), assistant_calls(kTwoCalls), first, second});
        const auto& stored = provider_request_messages(request);
        ASSERT_EQ(stored.size(), 3u);
        EXPECT_EQ(stored[2].id, "first-id");
        EXPECT_EQ(stored[2].role, std::string_view(family) == "anthropic.messages" ? sp::Role::User : sp::Role::Tool);
        ASSERT_EQ(stored[2].parts.size(), 2u);
        EXPECT_EQ(std::get<sp::ToolResult>(stored[2].parts[0]).tool_use_id, "c1");
        EXPECT_EQ(std::get<sp::ToolResult>(stored[2].parts[1]).tool_use_id, "c2");
    }
}

TEST(ToolResultGrouping, ResultsAreNotMergedForFamiliesThatAcceptEitherShape) {
    for (auto family : {"openai.chat", "openai.responses", "google.interactions"}) {
        SCOPED_TRACE(family);
        auto request = wire::request(family);
        const std::vector<sp::Message> history = {
            user_text("q"), assistant_calls(kTwoCalls), tool_result("c1"), tool_result("c2")};
        set_provider_request_messages(request, history);
        const auto& stored = provider_request_messages(request);
        ASSERT_EQ(stored.size(), 4u);
        EXPECT_EQ(stored[2].parts.size(), 1u);
        EXPECT_EQ(stored[3].parts.size(), 1u);
        for (std::size_t i = 0; i < history.size(); ++i)
            EXPECT_EQ(message_digest(stored[i]), message_digest(history[i]));
    }
}

TEST(ToolResultGrouping, PortableUserAndToolResultsShareOneOrderedRun) {
    auto first = tool_result("c1");
    first.role = sp::Role::User;
    first.id = "user-result";
    auto second = tool_result("c2");
    second.parts.push_back(std::get<sp::ToolResult>(tool_result("c3").parts[0]));
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        auto request = wire::request(family);
        set_provider_request_messages(request, {first, second});
        const auto& stored = provider_request_messages(request);
        ASSERT_EQ(stored.size(), 1u);
        EXPECT_EQ(stored[0].id, "user-result");
        EXPECT_EQ(stored[0].role, sp::Role::User);
        ASSERT_EQ(stored[0].parts.size(), 3u);
        for (std::size_t i = 0; i < 3; ++i)
            EXPECT_EQ(std::get<sp::ToolResult>(stored[0].parts[i]).tool_use_id, "c" + std::to_string(i + 1));
    }
}

TEST(ToolResultGrouping, NativeWireEmptyAndSystemMessagesRemainRunBoundaries) {
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        const auto native = assistant_turn(family, kTwoCalls, {user_text("q")}).native;
        ASSERT_TRUE(native);
        auto sealed_result = tool_result("native");
        sealed_result.native = native;
        auto wire_result = tool_result("wire");
        wire_result.wire_output = test::document(R"({"atomic":"fixture"})");
        sp::Message empty;
        empty.role = sp::Role::User;
        const auto system = test::message("instruction", sp::Role::System);
        for (const auto& barrier : {sealed_result, wire_result, empty, system}) {
            const std::vector<sp::Message> history = {tool_result("c1"), barrier, tool_result("c2")};
            const auto first_digest = message_digest(history[0]);
            const auto last_digest = message_digest(history[2]);
            auto request = wire::request(family);
            set_provider_request_messages(request, history);
            const auto& stored = provider_request_messages(request);
            if (barrier.role == sp::Role::System) {
                ASSERT_EQ(stored.size(), 2u);
            } else {
                ASSERT_EQ(stored.size(), 3u);
                EXPECT_EQ(stored[1].native, barrier.native);
                EXPECT_EQ(stored[1].wire_output, barrier.wire_output);
                EXPECT_EQ(stored[1].parts.size(), barrier.parts.size());
            }
            EXPECT_EQ(std::get<sp::ToolResult>(stored.front().parts[0]).tool_use_id, "c1");
            EXPECT_EQ(std::get<sp::ToolResult>(stored.back().parts[0]).tool_use_id, "c2");
            EXPECT_EQ(message_digest(history[0]), first_digest);
            EXPECT_EQ(message_digest(history[2]), last_digest);
        }
    }
}


TEST(ToolResultGrouping, FailedToolIsReportedAsErrorOnMessagesAndGemini) {
    const std::string failed = R"({"error":"boom","status":"failed"})";
    for (auto family : {"anthropic.messages", "google.generate"}) {
        SCOPED_TRACE(family);
        const std::vector<sp::Message> prefix = {user_text("weather?")};
        auto history = prefix;
        history.push_back(assistant_turn(family, kTwoCalls, prefix));
        history.push_back(tool_result("c1"));
        history.push_back(tool_result("c2", failed, "failed"));
        const auto body = prepared_body(family, history);
        if (std::string_view(family) == "anthropic.messages") {
            const auto& blocks = body.at("messages").at(2).at("content");
            ASSERT_EQ(blocks.size(), 2u);
            EXPECT_EQ(blocks[0].at("is_error"), false);
            EXPECT_EQ(blocks[0].at("content"), R"({"temp":20})");
            EXPECT_EQ(blocks[1].at("is_error"), true);
            EXPECT_EQ(blocks[1].at("content"), failed);
        } else {
            const auto& parts = body.at("contents").at(2).at("parts");
            ASSERT_EQ(parts.size(), 2u);
            EXPECT_EQ(parts[0].at("functionResponse").at("response"), json::parse(R"({"temp":20})"));
            EXPECT_EQ(parts[1].at("functionResponse").at("response"), (json{{"error", json::parse(failed)}}));
        }
    }
}

TEST(ToolResultGrouping, GeminiAssistantTextAndToolCallEncodeBothPartsInOrder) {
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "let me check";
    assistant.tool_calls.push_back({"c1", "lookup_weather", R"({"city":"Seoul"})"});
    const auto body = prepared_body("google.generate",
        {user_text("weather?"), portable_message(assistant), tool_result("c1")}, true);
    const auto& model = body.at("contents").at(1);
    EXPECT_EQ(model.at("role"), "model");
    ASSERT_EQ(model.at("parts").size(), 2u);
    EXPECT_EQ(model.at("parts")[0].at("text"), "let me check");
    EXPECT_EQ(model.at("parts")[1].at("functionCall").at("name"), "lookup_weather");
    EXPECT_EQ(model.at("parts")[1].at("functionCall").at("args").at("city"), "Seoul");
    ASSERT_EQ(body.at("contents").size(), 3u);
    EXPECT_EQ(body.at("contents").at(2).at("parts").at(0).at("functionResponse").at("name"), "lookup_weather");
}

TEST(ToolResultGrouping, GeminiTerminalsFollowTheDocumentedStopTable) {
    // Pins the behaviour documented next to ProviderFailure in types.h.
    struct Case { const char* finish; bool failure; sp::StopKind kind; };
    const std::array cases = {
        Case{"STOP", false, sp::StopKind::EndTurn}, Case{"MAX_TOKENS", false, sp::StopKind::MaxTokens},
        Case{"MALFORMED_FUNCTION_CALL", false, sp::StopKind::MalformedCall},
        Case{"TOO_MANY_TOOL_CALLS", false, sp::StopKind::MalformedCall},
        Case{"OTHER", false, sp::StopKind::Unknown}, Case{"SAFETY", true, sp::StopKind::ContentFilter}};
    for (const auto& item : cases) {
        SCOPED_TRACE(item.finish);
        wire::Peer peer(std::string(R"({"candidates":[{"content":{"role":"model","parts":[{"text":"x"}]},"finishReason":")")
                        + item.finish + R"("}]})");
        auto provider = wire::provider("google.generate", peer.origin());
        const auto result = provider->invoke(wire::request("google.generate"));
        ASSERT_TRUE(result);
        if (item.failure) {
            ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
            EXPECT_EQ(std::get<sp::Failure>(*result).error.kind, sp::ErrorKind::RemoteFailure);
            EXPECT_THROW(outcome_or_throw(result), ProviderFailure);
        } else {
            ASSERT_TRUE(std::holds_alternative<sp::Completion>(*result));
            const auto& stop = test::completion(result).stop;
            EXPECT_EQ(stop.kind, item.kind);
            EXPECT_EQ(stop.raw, item.finish);
            EXPECT_NO_THROW(outcome_or_throw(result));
        }
    }
}

// pause_turn: the Agent resends the history, paused assistant turn included,
// instead of returning the paused turn as if it were final (#311).
TEST(AgentPauseTurn, ResendsHistoryWithPausedTurnUntilTheModelFinishes) {
    const auto paused = json{{"id", "msg"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
        {"content", json::array({{{"type", "text"}, {"text", "still searching"}}})}, {"stop_reason", "pause_turn"},
        {"usage", {{"input_tokens", 3}, {"output_tokens", 2}}}}.dump();
    wire::Peer peer(paused);
    { std::lock_guard lock(peer.state->mutex); peer.state->next_bodies = {reply("anthropic.messages", {})}; }
    std::shared_ptr<Provider> provider = std::make_shared<BoundedWireProvider>("anthropic.messages", peer.origin());
    llm::Agent agent(provider, {}, "Be brief.", "fixture-model");
    std::vector<sp::Message> messages{user_text("find it")};
    auto result = agent.run(messages, 3);
    ASSERT_TRUE(result && std::holds_alternative<sp::Completion>(*result));
    EXPECT_EQ(std::get<sp::Completion>(*result).stop.kind, sp::StopKind::EndTurn);
    EXPECT_EQ(test::text(result), "both done");
    std::lock_guard lock(peer.state->mutex);
    ASSERT_EQ(peer.state->requests.size(), 2u);
    const auto& resent = peer.state->requests[1].at("messages");
    ASSERT_EQ(resent.size(), 2u);
    EXPECT_EQ(resent[0].at("role"), "user");
    EXPECT_EQ(resent[1].at("role"), "assistant");
    EXPECT_EQ(resent[1].at("content").at(0).at("text"), "still searching");
}

TEST(AgentPauseTurn, RepeatedPausesStayWithinTheIterationBudget) {
    const auto paused = json{{"id", "msg"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
        {"content", json::array({{{"type", "text"}, {"text", "still searching"}}})}, {"stop_reason", "pause_turn"},
        {"usage", {{"input_tokens", 3}, {"output_tokens", 2}}}}.dump();
    wire::Peer peer(paused);
    std::shared_ptr<Provider> provider = std::make_shared<BoundedWireProvider>("anthropic.messages", peer.origin());
    llm::Agent agent(provider, {}, "Be brief.", "fixture-model");
    std::vector<sp::Message> messages{user_text("find it")};
    EXPECT_THROW(agent.run(messages, 2), std::runtime_error);
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->requests.size(), 2u);
}
