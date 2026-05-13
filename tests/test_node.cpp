// PR 9a: these tests now drive the v0.4 unified ``run(NodeInput)``
// virtual on built-in nodes (LLMCallNode, ToolDispatchNode). The few
// remaining legacy-default-chain tests in this file (e.g.
// ``DefaultExecuteFullWrapsExecute`` exercising a ``SimpleNode``
// subclass that overrides only ``execute``) are still covered by
// ``test_node_default_dispatch.cpp`` / ``test_node_async_default.cpp``;
// when the legacy chain is deleted in PR 9 those test files go too.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/async/run_sync.h>

using namespace neograph;
using namespace neograph::graph;

namespace {
// Drive a built-in node's awaitable ``run`` to completion on a
// fresh single-threaded io_context — the right shape for unit
// tests that exercise just one node out of an engine context.
NodeOutput drive_run(GraphNode& node, const GraphState& state) {
    RunContext ctx;
    return neograph::async::run_sync(
        node.run(NodeInput{state, ctx, nullptr}));
}
}  // namespace

static ReducerFn overwrite_fn() { return ReducerRegistry::instance().get("overwrite"); }
static ReducerFn append_fn()    { return ReducerRegistry::instance().get("append"); }

// ── Mock Provider ──

class MockProvider : public Provider {
public:
    ChatMessage next_response;

    ChatCompletion complete(const CompletionParams& /*params*/) override {
        ChatCompletion comp;
        comp.message = next_response;
        return comp;
    }
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& /*on_chunk*/) override {
        return complete(params);
    }
    std::string get_name() const override { return "mock"; }
};

// ── Mock Tool ──

class MockTool : public Tool {
public:
    ChatTool get_definition() const override {
        return {"mock_tool", "A mock tool", json{{"type", "object"}, {"properties", json::object()}}};
    }
    std::string execute(const json& /*args*/) override {
        return "mock_result";
    }
    std::string get_name() const override { return "mock_tool"; }
};

// ── LLMCallNode ──

TEST(NodeTest, LLMCallNodeExecute) {
    auto provider = std::make_shared<MockProvider>();
    provider->next_response = ChatMessage{"assistant", "Hello from LLM"};

    NodeContext ctx;
    ctx.provider = provider;

    LLMCallNode node("llm", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    json user_msg;
    to_json(user_msg, ChatMessage{"user", "Hi"});
    state.write("messages", json::array({user_msg}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);
    EXPECT_EQ(out.writes[0].channel, "messages");

    // The written value should contain the assistant response
    auto msgs = out.writes[0].value;
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 1);
    EXPECT_EQ(msgs[0]["role"], "assistant");
    EXPECT_EQ(msgs[0]["content"], "Hello from LLM");
}

TEST(NodeTest, LLMCallNodeName) {
    NodeContext ctx;
    ctx.provider = std::make_shared<MockProvider>();
    LLMCallNode node("my_llm", ctx);
    EXPECT_EQ(node.get_name(), "my_llm");
}

// ── ToolDispatchNode ──

TEST(NodeTest, ToolDispatchExecutes) {
    auto tool = std::make_unique<MockTool>();
    Tool* tool_ptr = tool.get();

    NodeContext ctx;
    ctx.tools = {tool_ptr};

    ToolDispatchNode node("tools", ctx);

    // Create state with an assistant message containing tool_calls
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = {ToolCall{"tc_1", "mock_tool", "{}"}};

    json msg_json;
    to_json(msg_json, assistant_msg);
    state.write("messages", json::array({msg_json}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);

    auto tool_msgs = out.writes[0].value;
    ASSERT_TRUE(tool_msgs.is_array());
    ASSERT_EQ(tool_msgs.size(), 1);
    EXPECT_EQ(tool_msgs[0]["role"], "tool");
    EXPECT_EQ(tool_msgs[0]["content"], "mock_result");
}

TEST(NodeTest, ToolDispatchNoToolCalls) {
    NodeContext ctx;
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    // No assistant message with tool_calls
    json user_msg;
    to_json(user_msg, ChatMessage{"user", "Hi"});
    state.write("messages", json::array({user_msg}));

    auto out = drive_run(node, state);
    EXPECT_TRUE(out.writes.empty());
}

TEST(NodeTest, ToolDispatchToolNotFound) {
    NodeContext ctx;
    // No tools registered
    ToolDispatchNode node("tools", ctx);

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = {ToolCall{"tc_1", "nonexistent", "{}"}};

    json msg_json;
    to_json(msg_json, assistant_msg);
    state.write("messages", json::array({msg_json}));

    auto out = drive_run(node, state);
    ASSERT_EQ(out.writes.size(), 1);
    // Should contain error message
    auto content = out.writes[0].value[0]["content"].get<std::string>();
    EXPECT_TRUE(content.find("error") != std::string::npos ||
                content.find("not found") != std::string::npos ||
                content.find("Tool not found") != std::string::npos);
}

// ── NodeResult / Send / Command ──

TEST(NodeTest, NodeResultFromWrites) {
    std::vector<ChannelWrite> writes = {{"result", json("ok")}};
    NodeResult nr(writes);
    EXPECT_EQ(nr.writes.size(), 1);
    EXPECT_FALSE(nr.command.has_value());
    EXPECT_TRUE(nr.sends.empty());
}

TEST(NodeTest, NodeResultWithCommand) {
    NodeResult nr;
    nr.command = Command{"next_node", {ChannelWrite{"status", json("routed")}}};
    EXPECT_TRUE(nr.command.has_value());
    EXPECT_EQ(nr.command->goto_node, "next_node");
}

TEST(NodeTest, NodeResultWithSend) {
    NodeResult nr;
    nr.sends.push_back(Send{"worker", json{{"topic", "AI"}}});
    nr.sends.push_back(Send{"worker", json{{"topic", "ML"}}});
    EXPECT_EQ(nr.sends.size(), 2);
}

// ── NodeInterrupt ──

TEST(NodeTest, NodeInterruptThrows) {
    try {
        throw NodeInterrupt("test reason");
        FAIL() << "Should have thrown";
    } catch (const NodeInterrupt& ni) {
        EXPECT_EQ(ni.reason(), "test reason");
        EXPECT_EQ(std::string(ni.what()), "test reason");
    }
}

// v1.0 removal (9b): the `DefaultExecuteFullWrapsExecute` test verified
// the legacy 8-virtual default chain (execute_full default wrapping
// execute output into NodeResult::writes). The chain is gone — run() is
// the only dispatch surface and NodeResult/NodeOutput is the only
// return shape — so the test has nothing left to assert.
