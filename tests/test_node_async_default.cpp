// Stage 3 / Semester 3.4 regression — GraphNode now exposes
// execute_async as the awaitable peer of execute(). The two are
// connected by the same crossover-default pattern as Provider /
// CheckpointStore: the sync execute() is no longer pure virtual,
// defaulting to run_sync(execute_async); execute_async defaults to
// co_returning execute(). Subclasses override one side and inherit
// the other.
//
// All existing built-in nodes (LLMCall, ToolDispatch, IntentClassifier,
// Subgraph) and every user node in tests/examples still override
// execute() — they pick up the async surface for free through the
// default bridge. These cases pin both directions of the bridge end-
// to-end with minimal stubs.

#include <gtest/gtest.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/async/run_sync.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>

using namespace neograph::graph;

namespace {

class SyncOnlyNode : public GraphNode {
  public:
    std::atomic<int> sync_calls{0};
    std::vector<ChannelWrite> execute(const GraphState&) override {
        ++sync_calls;
        return {ChannelWrite{"out", neograph::json("sync")}};
    }
    std::string get_name() const override { return "sync-only"; }
};

class AsyncOnlyNode : public GraphNode {
  public:
    std::atomic<int> async_calls{0};
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState&) override {
        ++async_calls;
        co_return std::vector<ChannelWrite>{
            ChannelWrite{"out", neograph::json("async")}};
    }
    std::string get_name() const override { return "async-only"; }
};

GraphState empty_state() {
    return GraphState{};
}

} // namespace

TEST(NodeAsyncDefault, SyncOverrideBridgesToAsync) {
    SyncOnlyNode n;
    auto state = empty_state();

    auto direct = n.execute(state);
    ASSERT_EQ(direct.size(), 1u);
    EXPECT_EQ(direct[0].channel, "out");

    auto via_async = neograph::async::run_sync(n.execute_async(state));
    ASSERT_EQ(via_async.size(), 1u);
    EXPECT_EQ(via_async[0].channel, "out");
    EXPECT_EQ(n.sync_calls.load(), 2);
}

TEST(NodeAsyncDefault, AsyncOverrideBridgesToSync) {
    AsyncOnlyNode n;
    auto state = empty_state();

    auto via_sync = n.execute(state);
    ASSERT_EQ(via_sync.size(), 1u);
    EXPECT_EQ(via_sync[0].channel, "out");
    EXPECT_EQ(n.async_calls.load(), 1);
}

TEST(NodeAsyncDefault, AsyncDirectlyCoSpawnable) {
    AsyncOnlyNode n;
    auto state = empty_state();
    asio::io_context io;

    std::vector<ChannelWrite> out;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            out = co_await n.execute_async(state);
        },
        asio::detached);
    io.run();

    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].channel, "out");
    EXPECT_EQ(n.async_calls.load(), 1);
}
