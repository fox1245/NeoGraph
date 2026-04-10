#include <gtest/gtest.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// Helper: get built-in reducer from registry
static ReducerFn overwrite_fn() { return ReducerRegistry::instance().get("overwrite"); }
static ReducerFn append_fn()    { return ReducerRegistry::instance().get("append"); }

// ── Overwrite Reducer ──

TEST(GraphState, OverwriteInit) {
    GraphState state;
    state.init_channel("status", ReducerType::OVERWRITE, overwrite_fn(), json("init"));
    EXPECT_EQ(state.get("status"), "init");
}

TEST(GraphState, OverwriteWrite) {
    GraphState state;
    state.init_channel("status", ReducerType::OVERWRITE, overwrite_fn(), json("init"));
    state.write("status", json("updated"));
    EXPECT_EQ(state.get("status"), "updated");
    EXPECT_EQ(state.channel_version("status"), 1);
}

TEST(GraphState, OverwriteMultipleWrites) {
    GraphState state;
    state.init_channel("val", ReducerType::OVERWRITE, overwrite_fn(), json(0));
    state.write("val", json(1));
    state.write("val", json(2));
    state.write("val", json(3));
    EXPECT_EQ(state.get("val"), 3);
    EXPECT_EQ(state.channel_version("val"), 3);
}

// ── Append Reducer ──

TEST(GraphState, AppendBasic) {
    GraphState state;
    state.init_channel("msgs", ReducerType::APPEND, append_fn(), json::array());
    state.write("msgs", json::array({"hello"}));
    state.write("msgs", json::array({"world"}));

    auto msgs = state.get("msgs");
    ASSERT_EQ(msgs.size(), 2);
    EXPECT_EQ(msgs[0], "hello");
    EXPECT_EQ(msgs[1], "world");
}

TEST(GraphState, AppendEmpty) {
    GraphState state;
    state.init_channel("msgs", ReducerType::APPEND, append_fn(), json::array());
    state.write("msgs", json::array());
    EXPECT_TRUE(state.get("msgs").empty());
}

// ── Custom Reducer ──

TEST(GraphState, CustomReducerSum) {
    auto sum_reducer = [](const json& current, const json& incoming) -> json {
        return current.get<int>() + incoming.get<int>();
    };
    GraphState state;
    state.init_channel("total", ReducerType::CUSTOM, sum_reducer, json(0));
    state.write("total", json(10));
    state.write("total", json(20));
    state.write("total", json(5));
    EXPECT_EQ(state.get("total"), 35);
}

// ── Batch Writes ──

TEST(GraphState, ApplyWritesAtomic) {
    GraphState state;
    state.init_channel("a", ReducerType::OVERWRITE, overwrite_fn(), json(0));
    state.init_channel("b", ReducerType::OVERWRITE, overwrite_fn(), json(0));

    std::vector<ChannelWrite> writes = {
        {"a", json(10)},
        {"b", json(20)}
    };
    state.apply_writes(writes);

    EXPECT_EQ(state.get("a"), 10);
    EXPECT_EQ(state.get("b"), 20);
}

// ── Serialization ──

TEST(GraphState, SerializeRestore) {
    GraphState state;
    state.init_channel("data", ReducerType::OVERWRITE, overwrite_fn(), json(42));
    state.init_channel("msgs", ReducerType::APPEND, append_fn(), json::array());
    state.write("msgs", json::array({"hello"}));

    auto snapshot = state.serialize();

    GraphState restored;
    restored.init_channel("data", ReducerType::OVERWRITE, nullptr);
    restored.init_channel("msgs", ReducerType::APPEND, append_fn(), json::array());
    restored.restore(snapshot);

    EXPECT_EQ(restored.get("data"), 42);
    EXPECT_EQ(restored.get("msgs").size(), 1);
    EXPECT_EQ(restored.get("msgs")[0], "hello");
}

// ── Channel Names ──

TEST(GraphState, ChannelNames) {
    GraphState state;
    state.init_channel("a", ReducerType::OVERWRITE, nullptr);
    state.init_channel("b", ReducerType::OVERWRITE, nullptr);
    state.init_channel("c", ReducerType::OVERWRITE, nullptr);

    auto names = state.channel_names();
    EXPECT_EQ(names.size(), 3);
}

// ── GetMessages Convenience ──

TEST(GraphState, GetMessages) {
    GraphState state;
    state.init_channel("messages", ReducerType::APPEND, append_fn(), json::array());

    json msg;
    to_json(msg, ChatMessage{"user", "hello"});
    state.write("messages", json::array({msg}));

    auto messages = state.get_messages();
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0].role, "user");
    EXPECT_EQ(messages[0].content, "hello");
}

// ── Non-existent Channel ──

TEST(GraphState, GetNonExistent) {
    GraphState state;
    auto val = state.get("nonexistent");
    EXPECT_TRUE(val.is_null());
}

// ── Global Version ──

TEST(GraphState, GlobalVersion) {
    GraphState state;
    state.init_channel("a", ReducerType::OVERWRITE, overwrite_fn(), json(0));
    state.init_channel("b", ReducerType::OVERWRITE, overwrite_fn(), json(0));

    EXPECT_EQ(state.global_version(), 0);
    state.write("a", json(1));
    EXPECT_EQ(state.global_version(), 1);
    state.write("b", json(2));
    EXPECT_EQ(state.global_version(), 2);
}

// ── Thread Safety ──

TEST(GraphState, ConcurrentWritesSafe) {
    GraphState state;
    auto sum_reducer = [](const json& current, const json& incoming) -> json {
        return current.get<int>() + incoming.get<int>();
    };
    state.init_channel("counter", ReducerType::CUSTOM, sum_reducer, json(0));

    constexpr int NUM_THREADS = 10;
    constexpr int WRITES_PER_THREAD = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < WRITES_PER_THREAD; ++j) {
                state.write("counter", json(1));
            }
        });
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(state.get("counter"), NUM_THREADS * WRITES_PER_THREAD);
}
