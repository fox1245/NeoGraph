// Stage 3 / Semester 3.6 (API surface) regression — GraphEngine now
// exposes run_async / run_stream_async / resume_async returning
// asio::awaitable<RunResult>. The current implementation is a thin
// wrapper that co_returns the matching sync call (the engine internals
// are not yet coroutine-native). These cases pin the wrapper contract:
//
//   * run_async resolves to the same RunResult as run() on the happy
//     path.
//   * Exceptions thrown inside a node propagate out of the awaitable
//     (i.e. caller catches via try/catch around co_await, not inside
//     the engine).
//   * resume_async honours the same checkpoint state as resume().
//   * Multiple concurrent run_async invocations on a shared io_context
//     all complete (no lockup or interleaving fault).
//
// When the engine internals get coroutinized later, these tests must
// keep passing — they encode the public contract that follow-up work
// has to preserve.

#include <gtest/gtest.h>
#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

namespace {

json minimal_graph(const std::string& node_name) {
    return {
        {"name", "async_api_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {node_name, {{"type", "custom"}}},
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}},
        }},
    };
}

class WriteNode : public GraphNode {
public:
    WriteNode(const std::string& name, std::string value)
        : name_(name), value_(std::move(value)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json(value_)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::string value_;
};

class ThrowingNode : public GraphNode {
public:
    explicit ThrowingNode(const std::string& name) : name_(name) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        throw std::runtime_error("intentional failure");
        co_return NodeOutput{};  // unreachable
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void register_writer(const std::string& value) {
    NodeFactory::instance().register_type("custom",
        [value](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WriteNode>(name, value);
        });
}

void register_thrower() {
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ThrowingNode>(name);
        });
}

} // namespace

TEST(GraphEngineAsyncApi, RunAsyncMatchesSyncResult) {
    register_writer("hello");
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "t-1";

    auto sync_result = engine->run(cfg);

    asio::io_context io;
    RunResult async_result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            async_result = co_await engine->run_async(cfg);
        },
        asio::detached);
    io.run();

    EXPECT_EQ(sync_result.output, async_result.output);
    EXPECT_FALSE(async_result.interrupted);
}

TEST(GraphEngineAsyncApi, RunAsyncPropagatesNodeException) {
    register_thrower();
    auto engine = GraphEngine::compile(minimal_graph("boom"), NodeContext{});

    RunConfig cfg;
    cfg.thread_id = "t-2";

    asio::io_context io;
    std::exception_ptr captured;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await engine->run_async(cfg);
            } catch (...) {
                captured = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(captured);
    EXPECT_THROW(std::rethrow_exception(captured), std::runtime_error);
}

TEST(GraphEngineAsyncApi, ResumeAsyncMatchesSyncResume) {
    // resume_async needs a checkpoint store + a thread that has a
    // checkpoint to resume from. Run once first to seed it, then drive
    // resume_async.
    register_writer("v");
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "t-resume";
    auto first = engine->run(cfg);
    ASSERT_FALSE(first.checkpoint_id.empty());

    asio::io_context io;
    RunResult resumed;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            resumed = co_await engine->resume_async("t-resume");
        },
        asio::detached);
    io.run();

    // Run that has already completed to END returns interrupted=false
    // with no further work — match the sync resume() contract.
    EXPECT_FALSE(resumed.interrupted);
}

TEST(GraphEngineAsyncApi, ConcurrentRunAsyncOnSharedIoContext) {
    // Multiple runs on one io_context. Today's wrapper executes them
    // serially (the inner sync run blocks the worker thread), but the
    // contract is "no lockup, no interleaving fault". When the engine
    // internals become coroutine-native, this test will start showing
    // real overlap — same assertions still hold.
    register_writer("v");
    auto engine = GraphEngine::compile(minimal_graph("worker"), NodeContext{});

    asio::io_context io;
    std::atomic<int> done{0};
    constexpr int N = 5;

    for (int i = 0; i < N; ++i) {
        asio::co_spawn(
            io,
            [&, i]() -> asio::awaitable<void> {
                RunConfig cfg;
                cfg.thread_id = "t-" + std::to_string(i);
                auto r = co_await engine->run_async(cfg);
                if (!r.interrupted) {
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            },
            asio::detached);
    }
    io.run();

    EXPECT_EQ(done.load(), N);
}
