// A resume value is not a user turn while tool calls wait for their results
// (issue #544).
//
// A graph with a "messages" channel receives GraphEngine::resume()'s value there
// as a user message (issue #94). When the run paused on a tool gate, the history
// ends in an assistant turn whose calls have no results yet, and the user turn
// landed between the calls and the results the tool_dispatch step then wrote.
// Every provider family requires a result to follow its call directly, so the
// next model call was refused before anything was sent.
//
// The value belongs to the gate that asked, through ToolGateContext::resume_value.
// The user turn stays for the graphs that rely on it once nothing is waiting.

#include <gtest/gtest.h>

#include "fixtures/typed_wire_peer.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/tool_dispatch.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
namespace wire = neograph::test::wire;

namespace {

class DeleteRecord : public Tool {
public:
    DeleteRecord(std::atomic<int>* runs, bool fails) : runs_(runs), fails_(fails) {}
    ChatTool get_definition() const override {
        return {"delete_record", "Delete a record.", json{{"type", "object"},
            {"properties", {{"record_id", {{"type", "string"}}}}}, {"required", json::array({"record_id"})}}};
    }
    std::string execute(const json&) override {
        runs_->fetch_add(1);
        if (fails_) throw std::runtime_error("record R-200 is locked");
        return R"({"deleted":"R-200"})";
    }
    std::string get_name() const override { return "delete_record"; }
private:
    std::atomic<int>* runs_;
    bool fails_;
};

// The model asks for delete_record, or answers once it has the result.
std::string asks_for_the_tool(std::string_view family) {
    const json arguments = {{"record_id", "R-200"}};
    if (family == "anthropic.messages")
        return json{{"id", "msg"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
            {"content", json::array({{{"type", "tool_use"}, {"id", "call-1"}, {"name", "delete_record"},
                                      {"input", arguments}}})},
            {"stop_reason", "tool_use"}, {"usage", {{"input_tokens", 3}, {"output_tokens", 2}}}}.dump();
    if (family == "openai.responses") {
        auto body = json::parse(wire::responses_body("unused"));
        body["output"] = json::array({{{"type", "function_call"}, {"id", "fc-1"}, {"call_id", "call-1"},
            {"name", "delete_record"}, {"arguments", arguments.dump()}, {"status", "completed"}}});
        return body.dump();
    }
    return wire::chat_envelope(json{{"choices", json::array({{{"index", 0},
        {"message", {{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array({
            {{"id", "call-1"}, {"type", "function"},
             {"function", {{"name", "delete_record"}, {"arguments", arguments.dump()}}}}})}}},
        {"finish_reason", "tool_calls"}}})}}).dump();
}

std::string final_answer(std::string_view family) {
    if (family == "anthropic.messages")
        return json{{"id", "msg"}, {"type", "message"}, {"model", "fixture-model"}, {"role", "assistant"},
            {"content", json::array({{{"type", "text"}, {"text", "Done."}}})}, {"stop_reason", "end_turn"},
            {"usage", {{"input_tokens", 3}, {"output_tokens", 2}}}}.dump();
    if (family == "openai.responses") return wire::responses_body("Done.");
    return wire::chat_response("Done.");
}

json agent_graph(bool pause_before_tools) {
    json graph{
        {"name", "gated_agent"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"llm", {{"type", "llm_call"}}}, {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"type", "conditional"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}})}};
    if (pause_before_tools) graph["interrupt_before"] = json::array({"tools"});
    return graph;
}

// The agent graph on a loopback peer that asks for delete_record, then answers.
struct Agent {
    Agent(const std::string& family, bool tool_fails, bool pause_before_tools)
        : peer(asks_for_the_tool(family)) {
        peer.state->next_bodies.push_back(final_answer(family));
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::make_unique<DeleteRecord>(&runs, tool_fails));
        NodeContext context;
        context.provider = std::shared_ptr<Provider>(wire::provider(family, peer.origin()));
        context.model = "fixture-model";
        context.tools = ToolSet(std::move(tools));
        context.provider_controls.max_output_tokens = 64;
        if (family != "openai.chat") context.provider_controls.account_scope = "fixture-account";
        engine = GraphEngine::compile(agent_graph(pause_before_tools), context,
                                      std::make_shared<InMemoryCheckpointStore>());
    }
    RunConfig config(const std::string& thread_id) const {
        RunConfig config;
        config.thread_id = thread_id;
        config.input = {{"messages", json::array({{{"role", "user"}, {"content", "Delete R-200."}}})}};
        return config;
    }
    std::vector<json> requests() {
        std::lock_guard lock(peer.state->mutex);
        return peer.state->requests;
    }
    wire::Peer peer;
    std::atomic<int> runs{0};
    std::unique_ptr<GraphEngine> engine;
};

struct GatedRun {
    RunResult paused;
    RunResult resumed;
    std::vector<json> requests;
    int tool_runs = 0;
};

// The run pauses at the tools step, the host answers, and the model is called
// again with the result. The pause is the tool gate's, or interrupt_before on
// the tools node when `use_gate` is false. `answer` is what the operator sends.
GatedRun pause_then_answer(const std::string& family, const json& answer, bool use_gate = true) {
    Agent agent(family, false, !use_gate);
    if (use_gate)
        agent.engine->set_tool_gate([](ToolCall, ToolGateContext gctx) -> asio::awaitable<ToolDecision> {
            if (!gctx.resume_value) co_return ToolDecision::interrupt("delete_record needs an operator");
            if (gctx.resume_value->value("approved", false)) co_return ToolDecision::allow();
            co_return ToolDecision::deny("the operator declined");
        });
    GatedRun run;
    const auto config = agent.config("gated-" + family);
    run.paused = agent.engine->run(config);
    run.resumed = agent.engine->resume(config.thread_id, answer);
    run.requests = agent.requests();
    run.tool_runs = agent.runs.load();
    return run;
}

std::vector<std::string> roles(const RunResult& result) {
    std::vector<std::string> out;
    for (const auto& message : result.output.at("channels").at("messages").at("value"))
        out.push_back(message.at("role").get<std::string>());
    return out;
}

class ResumeAfterToolGate : public ::testing::TestWithParam<std::string> {};

}  // namespace

TEST_P(ResumeAfterToolGate, ApprovingAGatedCallLetsTheNextModelCallThrough) {
    const auto run = pause_then_answer(GetParam(), json{{"approved", true}});
    ASSERT_TRUE(run.paused.interrupted);
    EXPECT_EQ(run.paused.interrupt_node, "tools");
    ASSERT_FALSE(run.resumed.interrupted);
    EXPECT_EQ(run.tool_runs, 1);
    ASSERT_EQ(run.requests.size(), 2u) << "the model call after the resume was refused locally";
    EXPECT_EQ(roles(run.resumed), (std::vector<std::string>{"user", "assistant", "tool", "assistant"}))
        << "the answer was written as a user turn between the call and its result";
    EXPECT_EQ(run.requests[1].dump().find("approved"), std::string::npos)
        << "the operator's answer reached the model as a message";
}

TEST_P(ResumeAfterToolGate, RefusingAGatedCallTellsTheModelAndLetsItAnswer) {
    const auto run = pause_then_answer(GetParam(), json{{"approved", false}});
    ASSERT_TRUE(run.paused.interrupted);
    ASSERT_FALSE(run.resumed.interrupted);
    EXPECT_EQ(run.tool_runs, 0);
    ASSERT_EQ(run.requests.size(), 2u) << "the model call after the resume was refused locally";
    EXPECT_EQ(roles(run.resumed), (std::vector<std::string>{"user", "assistant", "tool", "assistant"}));
    EXPECT_NE(run.requests[1].dump().find("the operator declined"), std::string::npos)
        << "the refusal is the call's result";
}

TEST_P(ResumeAfterToolGate, AValueGivenAtAnInterruptBeforeTheToolsNodeIsNotAMessageEither) {
    const auto run = pause_then_answer(GetParam(), json("go ahead"), false);
    ASSERT_TRUE(run.paused.interrupted);
    EXPECT_EQ(run.paused.interrupt_node, "tools");
    ASSERT_FALSE(run.resumed.interrupted);
    EXPECT_EQ(run.tool_runs, 1);
    ASSERT_EQ(run.requests.size(), 2u) << "the model call after the resume was refused locally";
    EXPECT_EQ(roles(run.resumed), (std::vector<std::string>{"user", "assistant", "tool", "assistant"}));
}

// A failed result is sent to the model whatever the family can say about it:
// Responses has no error field for a function_call_output, so the failure
// travels as the result's text.
TEST_P(ResumeAfterToolGate, AToolThatFailsStillLetsTheNextModelCallThrough) {
    Agent agent(GetParam(), true, false);
    const auto done = agent.engine->run(agent.config("failing-" + GetParam()));
    ASSERT_FALSE(done.interrupted);
    const auto requests = agent.requests();
    ASSERT_EQ(requests.size(), 2u) << "the model call after the failed tool was refused locally";
    EXPECT_NE(requests[1].dump().find("record R-200 is locked"), std::string::npos);
    EXPECT_EQ(roles(done), (std::vector<std::string>{"user", "assistant", "tool", "assistant"}));
}

INSTANTIATE_TEST_SUITE_P(Families, ResumeAfterToolGate,
    ::testing::Values("openai.chat", "openai.responses", "anthropic.messages"),
    [](const auto& info) {
        std::string name = info.param;
        for (auto& c : name) if (c == '.') c = '_';
        return name;
    });

namespace {

// Pauses until it has an answer, whatever the history holds.
class AskNode : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput in) override {
        if (!in.ctx.resume_value) throw NodeInterrupt("needs an answer");
        co_return NodeResult{};
    }
    std::string get_name() const override { return "ask"; }
};

json assistant_calls(std::initializer_list<const char*> ids) {
    json calls = json::array();
    for (const char* id : ids) calls.push_back({{"id", id}, {"name", "delete_record"}, {"arguments", "{}"}});
    return {{"role", "assistant"}, {"content", ""}, {"tool_calls", calls}};
}

json tool_result(const char* id) {
    return {{"role", "tool"}, {"content", "{}"}, {"tool_call_id", id}, {"tool_name", "delete_record"}};
}

// A node pauses with `history` in the channel and the host answers "yes".
std::vector<std::string> roles_after_answering(json history) {
    NodeFactory::instance().register_type("ask_node",
        [](const std::string&, const json&, const NodeContext&) { return std::make_unique<AskNode>(); });
    const json definition = {
        {"name", "ask_graph"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"ask", {{"type", "ask_node"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "ask"}}, {{"from", "ask"}, {"to", "__end__"}}})}};
    auto engine = GraphEngine::compile(definition, NodeContext{}, std::make_shared<InMemoryCheckpointStore>());
    RunConfig config;
    config.thread_id = "ask";
    config.input = {{"messages", std::move(history)}};
    EXPECT_TRUE(engine->run(config).interrupted);
    const auto done = engine->resume("ask", "yes");
    EXPECT_FALSE(done.interrupted);
    return roles(done);
}

}  // namespace

TEST(ResumeUserTurn, AChatGraphStillReceivesTheValueAsAUserTurn) {
    EXPECT_EQ(roles_after_answering(json::array({{{"role", "user"}, {"content", "hi"}},
                                                 {{"role", "assistant"}, {"content", "Shall I go on?"}}})),
              (std::vector<std::string>{"user", "assistant", "user"}));
}

TEST(ResumeUserTurn, AnsweredToolCallsDoNotHoldBackTheUserTurn) {
    EXPECT_EQ(roles_after_answering(json::array({{{"role", "user"}, {"content", "hi"}},
                                                 assistant_calls({"a", "b"}), tool_result("a"), tool_result("b")})),
              (std::vector<std::string>{"user", "assistant", "tool", "tool", "user"}));
}

TEST(ResumeUserTurn, ACallWithoutAResultHoldsTheUserTurnBack) {
    EXPECT_EQ(roles_after_answering(json::array({{{"role", "user"}, {"content", "hi"}},
                                                 assistant_calls({"a", "b"}), tool_result("a")})),
              (std::vector<std::string>{"user", "assistant", "tool"}))
        << "the second call still waits for its result";
}
