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

    try {
        engine->run(cfg);
        FAIL() << "expected NodeExecutionError";
    } catch (const NodeExecutionError& e) {
        EXPECT_EQ(e.node_name(), "boom");
        EXPECT_EQ(e.attempts(), 1);
        EXPECT_THROW(std::rethrow_exception(e.cause()), std::runtime_error);
    }

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
    try {
        std::rethrow_exception(captured);
        FAIL() << "expected NodeExecutionError";
    } catch (const NodeExecutionError& e) {
        EXPECT_EQ(e.node_name(), "boom");
        EXPECT_EQ(e.attempts(), 1);
        EXPECT_THROW(std::rethrow_exception(e.cause()), std::runtime_error);
    }
}

TEST(GraphEngineAsyncApi, SubgraphFailureStillHonorsOuterRetry) {
    auto calls = std::make_shared<std::atomic<int>>(0);
    NodeFactory::instance().register_type("subgraph_transient_123",
        [calls](const std::string& name, const json&, const NodeContext&) {
            class TransientNode final : public GraphNode {
            public:
                TransientNode(std::string name,
                              std::shared_ptr<std::atomic<int>> calls)
                    : name_(std::move(name)), calls_(std::move(calls)) {}
                asio::awaitable<NodeOutput> run(NodeInput) override {
                    if (calls_->fetch_add(1) < 2) {
                        throw std::runtime_error("inner transient failure");
                    }
                    co_return NodeOutput{};
                }
                std::string get_name() const override { return name_; }
            private:
                std::string name_;
                std::shared_ptr<std::atomic<int>> calls_;
            };
            return std::make_unique<TransientNode>(name, calls);
        });

    json inner = {
        {"name", "inner"},
        {"channels", json::object()},
        {"nodes", {{"deep", {{"type", "subgraph_transient_123"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "deep"}},
            {{"from", "deep"}, {"to", "__end__"}}
        })}
    };
    json outer = {
        {"name", "outer"},
        {"channels", json::object()},
        {"nodes", {{"shell", {{"type", "subgraph"},
                                {"definition", inner}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "shell"}},
            {{"from", "shell"}, {"to", "__end__"}}
        })},
        {"retry_policy", {{"max_retries", 2}, {"initial_delay_ms", 1}}}
    };

    auto engine = GraphEngine::compile(outer, NodeContext{});
    RunConfig cfg;
    cfg.thread_id = "subgraph-retry";
    EXPECT_NO_THROW(engine->run(cfg));
    EXPECT_EQ(calls->load(), 3);
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
