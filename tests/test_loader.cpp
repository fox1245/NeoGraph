#include <gtest/gtest.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

using namespace neograph;
using namespace neograph::graph;

static ReducerFn overwrite_fn() { return ReducerRegistry::instance().get("overwrite"); }
static ReducerFn append_fn()    { return ReducerRegistry::instance().get("append"); }

// ── ReducerRegistry ──

TEST(LoaderTest, BuiltinOverwriteReducer) {
    auto fn = ReducerRegistry::instance().get("overwrite");
    ASSERT_NE(fn, nullptr);
    auto result = fn(json("old"), json("new"));
    EXPECT_EQ(result, "new");
}

TEST(LoaderTest, BuiltinAppendReducer) {
    auto fn = ReducerRegistry::instance().get("append");
    ASSERT_NE(fn, nullptr);
    auto result = fn(json::array({1, 2}), json::array({3, 4}));
    ASSERT_EQ(result.size(), 4);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[3], 4);
}

TEST(LoaderTest, CustomReducerRegistration) {
    ReducerRegistry::instance().register_reducer("max_reducer",
        [](const json& current, const json& incoming) -> json {
            return std::max(current.get<int>(), incoming.get<int>());
        });

    auto fn = ReducerRegistry::instance().get("max_reducer");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(json(5), json(3)), 5);
    EXPECT_EQ(fn(json(3), json(7)), 7);
}

// ── ConditionRegistry ──

TEST(LoaderTest, BuiltinHasToolCallsCondition) {
    auto fn = ConditionRegistry::instance().get("has_tool_calls");
    ASSERT_NE(fn, nullptr);

    // State with assistant message that has tool_calls
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage msg;
    msg.role = "assistant";
    msg.tool_calls = {ToolCall{"id1", "calc", "{}"}};
    json msg_json;
    to_json(msg_json, msg);
    state.write("messages", json::array({msg_json}));

    EXPECT_EQ(fn(state), "true");
}

TEST(LoaderTest, HasToolCallsFalse) {
    auto fn = ConditionRegistry::instance().get("has_tool_calls");

    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    ChatMessage msg;
    msg.role = "assistant";
    msg.content = "No tools here";
    json msg_json;
    to_json(msg_json, msg);
    state.write("messages", json::array({msg_json}));

    EXPECT_EQ(fn(state), "false");
}

TEST(LoaderTest, BuiltinRouteChannelCondition) {
    auto fn = ConditionRegistry::instance().get("route_channel");
    ASSERT_NE(fn, nullptr);

    GraphState state;
    state.init_channel("__route__", ReducerType::OVERWRITE, overwrite_fn(), json(""));
    state.write("__route__", json("path_a"));

    EXPECT_EQ(fn(state), "path_a");
}

TEST(LoaderTest, CustomConditionRegistration) {
    ConditionRegistry::instance().register_condition("always_yes",
        [](const GraphState& /*state*/) -> std::string {
            return "yes";
        });

    auto fn = ConditionRegistry::instance().get("always_yes");
    ASSERT_NE(fn, nullptr);

    GraphState dummy;
    EXPECT_EQ(fn(dummy), "yes");
}

// ── NodeFactory ──

TEST(LoaderTest, CustomNodeTypeRegistration) {
    NodeFactory::instance().register_type("test_type",
        [](const std::string& /*name*/, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return nullptr;
        });

    // Should not throw — type is registered
    EXPECT_NO_THROW(
        NodeFactory::instance().create("test_type", "test", json::object(), NodeContext{})
    );
}

TEST(LoaderTest, NodeFactoryUnknownTypeThrows) {
    EXPECT_THROW(
        NodeFactory::instance().create("totally_unknown_type_xyz", "n", json::object(), NodeContext{}),
        std::runtime_error
    );
}
