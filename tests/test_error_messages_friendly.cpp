// DX batch — error message regression tests.
//
// The four registry / channel "Unknown <thing>: foo" errors now carry
// the available names + the next-action hint inline. This file locks
// the contract so the next refactor doesn't accidentally regress the
// message back to the bare-name shape.
//
// Why test the message text: the messages are surfaced to humans
// (CMake build error output, exception what(), tracer span errors).
// If the hint text disappears, downstream onboarding breaks but no
// behavioural test catches it.

#include <gtest/gtest.h>

#include <neograph/neograph.h>

#include <stdexcept>
#include <string>

using namespace neograph;
using namespace neograph::graph;

// Tiny helper — substring-match wrapper so failure output is clearer
// than EXPECT_TRUE(s.find(...) != npos).
static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// ── ReducerRegistry::get ──

TEST(ErrorMessagesFriendly, UnknownReducerListsAvailableAndHint) {
    try {
        ReducerRegistry::instance().get("nonexistent_reducer_xyz");
        FAIL() << "expected throw on unknown reducer";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(contains(msg, "'nonexistent_reducer_xyz'")) << msg;
        EXPECT_TRUE(contains(msg, "Available:")) << msg;
        // Built-in reducers must show up in the list.
        EXPECT_TRUE(contains(msg, "append")) << msg;
        EXPECT_TRUE(contains(msg, "overwrite")) << msg;
        EXPECT_TRUE(contains(msg, "register_reducer")) << msg;
        EXPECT_TRUE(contains(msg, "troubleshooting")) << msg;
    }
}

// ── ConditionRegistry::get ──

TEST(ErrorMessagesFriendly, UnknownConditionListsAvailableAndHint) {
    try {
        ConditionRegistry::instance().get("nonexistent_condition_xyz");
        FAIL() << "expected throw on unknown condition";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(contains(msg, "'nonexistent_condition_xyz'")) << msg;
        EXPECT_TRUE(contains(msg, "Available:")) << msg;
        EXPECT_TRUE(contains(msg, "has_tool_calls")) << msg;
        EXPECT_TRUE(contains(msg, "register_condition")) << msg;
    }
}

// ── NodeFactory::create ──

TEST(ErrorMessagesFriendly, UnknownNodeTypeListsAvailableAndHint) {
    NodeContext ctx;
    try {
        NodeFactory::instance().create("nonexistent_type_xyz", "my_node", json::object(), ctx);
        FAIL() << "expected throw on unknown node type";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(contains(msg, "'nonexistent_type_xyz'")) << msg;
        EXPECT_TRUE(contains(msg, "my_node")) << msg;  // node name reference
        EXPECT_TRUE(contains(msg, "Available:")) << msg;
        // Built-in node types must show up.
        EXPECT_TRUE(contains(msg, "llm_call")) << msg;
        EXPECT_TRUE(contains(msg, "register_type")) << msg;
    }
}

// ── GraphState::write — Write to unknown channel ──

TEST(ErrorMessagesFriendly, WriteToUnknownChannelListsDeclaredAndHint) {
    GraphState state;
    state.init_channel("counter", ReducerType::OVERWRITE,
                       ReducerRegistry::instance().get("overwrite"),
                       json(0));
    state.init_channel("messages", ReducerType::APPEND,
                       ReducerRegistry::instance().get("append"),
                       json::array());

    try {
        state.write("nonexistent_channel_xyz", json("anything"));
        FAIL() << "expected throw on unknown channel";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(contains(msg, "'nonexistent_channel_xyz'")) << msg;
        EXPECT_TRUE(contains(msg, "Declared channels:")) << msg;
        EXPECT_TRUE(contains(msg, "counter")) << msg;
        EXPECT_TRUE(contains(msg, "messages")) << msg;
        EXPECT_TRUE(contains(msg, "case-sensitive")) << msg;
    }
}

// ── apply_writes path takes the same code shape — guard separately ──

TEST(ErrorMessagesFriendly, ApplyWritesUnknownChannelListsDeclaredAndHint) {
    GraphState state;
    state.init_channel("only_one", ReducerType::OVERWRITE,
                       ReducerRegistry::instance().get("overwrite"),
                       json(0));

    std::vector<ChannelWrite> writes = {
        {"only_one",   json(1)},
        {"missing_xyz", json(2)},
    };
    try {
        state.apply_writes(writes);
        FAIL() << "expected throw on unknown channel in apply_writes";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_TRUE(contains(msg, "'missing_xyz'")) << msg;
        EXPECT_TRUE(contains(msg, "only_one")) << msg;
        EXPECT_TRUE(contains(msg, "Declared channels:")) << msg;
    }
}
