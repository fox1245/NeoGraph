// Stage 3 / Sem 3.6 (incremental) — NodeExecutor::execute_node_with_retry_async
// regression. The new coroutine retry path replaces sync
// std::this_thread::sleep_for with asio::steady_timer.async_wait
// so a backoff window doesn't freeze the io_context. The existing
// sync execute_node_with_retry stays untouched and is still
// covered by tests/test_node_execution.cpp's 12 cases.

#include <gtest/gtest.h>
#include <neograph/graph/engine.h>   // RunContext
#include <neograph/graph/executor.h>
#include <neograph/graph/node.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

namespace {

class FlakyNode : public GraphNode {
public:
    FlakyNode(std::string name, int fail_remaining)
        : name_(std::move(name)), fail_remaining_(fail_remaining) {}

    std::vector<ChannelWrite> execute(const GraphState&) override {
        int remaining = fail_remaining_.fetch_sub(1, std::memory_order_acq_rel);
        if (remaining > 0) {
            throw std::runtime_error("flaky: injected failure in " + name_);
        }
        return {ChannelWrite{"result", json("ok from " + name_)}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int> fail_remaining_;
};

class AlwaysSuccessNode : public GraphNode {
public:
    explicit AlwaysSuccessNode(std::string name) : name_(std::move(name)) {}
    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {ChannelWrite{"result", json("ok")}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

class AlwaysInterruptNode : public GraphNode {
public:
    explicit AlwaysInterruptNode(std::string name) : name_(std::move(name)) {}
    std::vector<ChannelWrite> execute(const GraphState&) override {
        throw NodeInterrupt("needs human");
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

// Helper to construct a NodeExecutor over a single node. Real engine
// builds these inside compile(); reaching in directly keeps the test
// scoped to the executor unit.
struct ExecutorHarness {
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<ChannelDef> channel_defs;
    NodeExecutor executor;

    ExecutorHarness(std::unique_ptr<GraphNode> node, RetryPolicy policy)
        : nodes(make_node_map(std::move(node))),
          channel_defs(make_channels()),
          executor(nodes, channel_defs,
                   [policy](const std::string&) { return policy; }) {}

private:
    static std::map<std::string, std::unique_ptr<GraphNode>>
    make_node_map(std::unique_ptr<GraphNode> node) {
        std::map<std::string, std::unique_ptr<GraphNode>> m;
        auto name = node->get_name();
        m.emplace(name, std::move(node));
        return m;
    }
    static std::vector<ChannelDef> make_channels() {
        ChannelDef def;
        def.name = "result";
        def.reducer_name = "overwrite";
        def.type = ReducerType::OVERWRITE;
        return {def};
    }
};

void init_state(GraphState& state, const std::vector<ChannelDef>& defs) {
    for (const auto& cd : defs) {
        auto reducer = ReducerRegistry::instance().get(cd.reducer_name);
        json initial = cd.initial_value;
        state.init_channel(cd.name, cd.type, reducer, initial);
    }
}

// Mirror of the engine's static task_id scheme so the replay map test
// can populate it correctly. NodeExecutor's make_static_task_id is
// file-static; this duplicate keeps the test compile-only — they must
// stay in sync ("s<step>:<node_name>").
std::string make_static_task_id_for_test(int step, const std::string& node_name) {
    return "s" + std::to_string(step) + ":" + node_name;
}

} // namespace

TEST(ExecutorAsyncRetry, SucceedsFirstTryReturnsResult) {
    ExecutorHarness h(std::make_unique<AlwaysSuccessNode>("worker"),
                      RetryPolicy{});

    asio::io_context io;
    std::optional<NodeResult> got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_state(state, h.channel_defs);
            got = co_await h.executor.execute_node_with_retry_async(
                "worker", state, nullptr, StreamMode::ALL, RunContext{});
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->writes.size(), 1u);
    EXPECT_EQ(got->writes[0].channel, "result");
}

TEST(ExecutorAsyncRetry, RetriesUntilSuccess) {
    RetryPolicy p;
    p.max_retries = 3;
    p.initial_delay_ms = 5;
    p.backoff_multiplier = 1;
    p.max_delay_ms = 5;

    auto node = std::make_unique<FlakyNode>("flaky", 2);  // fail twice
    auto* node_ptr = node.get();  // stash before move
    ExecutorHarness h(std::move(node), p);

    asio::io_context io;
    std::optional<NodeResult> got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_state(state, h.channel_defs);
            got = co_await h.executor.execute_node_with_retry_async(
                "flaky", state, nullptr, StreamMode::ALL, RunContext{});
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->writes[0].channel, "result");
    (void)node_ptr;
}

TEST(ExecutorAsyncRetry, ExhaustedRetriesPropagateException) {
    RetryPolicy p;
    p.max_retries = 1;
    p.initial_delay_ms = 1;
    p.backoff_multiplier = 1;
    p.max_delay_ms = 1;

    ExecutorHarness h(std::make_unique<FlakyNode>("doomed", 5), p);

    asio::io_context io;
    std::exception_ptr err;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                GraphState state; init_state(state, h.channel_defs);
                co_await h.executor.execute_node_with_retry_async(
                    "doomed", state, nullptr, StreamMode::ALL, RunContext{});
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(err);
    EXPECT_THROW(std::rethrow_exception(err), std::runtime_error);
}

TEST(ExecutorAsyncRetry, NodeInterruptShortCircuits) {
    ExecutorHarness h(std::make_unique<AlwaysInterruptNode>("hitl"),
                      RetryPolicy{});

    asio::io_context io;
    std::exception_ptr err;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                GraphState state; init_state(state, h.channel_defs);
                co_await h.executor.execute_node_with_retry_async(
                    "hitl", state, nullptr, StreamMode::ALL, RunContext{});
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(err);
    EXPECT_THROW(std::rethrow_exception(err), NodeInterrupt);
}

TEST(ExecutorAsyncRunOne, ReplayShortCircuitsExecute) {
    // run_one_async should honor the replay map: when a task_id is
    // already present, the cached NodeResult is applied without
    // re-executing the node.
    ExecutorHarness h(std::make_unique<FlakyNode>("worker", 100), RetryPolicy{});

    NodeResult cached;
    cached.writes.push_back(ChannelWrite{"result", json("from-replay")});

    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid");
    std::unordered_map<std::string, NodeResult> replay;
    replay.emplace(make_static_task_id_for_test(0, "worker"), cached);

    asio::io_context io;
    std::optional<NodeResult> got;
    std::vector<std::string> trace;
    BarrierState barrier;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_state(state, h.channel_defs);
            got = co_await h.executor.run_one_async(
                "worker", 0, state, replay, coord, "", barrier,
                trace, nullptr, StreamMode::ALL, RunContext{});
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->writes.size(), 1u);
    EXPECT_EQ(got->writes[0].value.get<std::string>(), "from-replay");
    ASSERT_EQ(trace.size(), 1u);
    EXPECT_EQ(trace[0], "worker");
}

TEST(ExecutorAsyncRunOne, NodeInterruptSavesDedicatedCheckpoint) {
    ExecutorHarness h(std::make_unique<AlwaysInterruptNode>("hitl"),
                      RetryPolicy{});
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid-interrupt");
    BarrierState barrier;
    std::vector<std::string> trace;
    std::unordered_map<std::string, NodeResult> replay;

    asio::io_context io;
    std::exception_ptr err;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                GraphState state; init_state(state, h.channel_defs);
                co_await h.executor.run_one_async(
                    "hitl", 0, state, replay, coord, "", barrier,
                    trace, nullptr, StreamMode::ALL, RunContext{});
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(err);
    EXPECT_THROW(std::rethrow_exception(err), NodeInterrupt);

    auto cps = store->list("tid-interrupt", 100);
    ASSERT_GE(cps.size(), 1u)
        << "NodeInterrupt path must save at least one checkpoint";
    bool found_interrupt_phase = false;
    for (const auto& cp : cps) {
        if (cp.interrupt_phase == CheckpointPhase::NodeInterrupt) {
            ASSERT_EQ(cp.next_nodes.size(), 1u);
            EXPECT_EQ(cp.next_nodes[0], "hitl");
            found_interrupt_phase = true;
        }
    }
    EXPECT_TRUE(found_interrupt_phase);
}

TEST(ExecutorAsyncRetry, BackoffDoesNotBlockIoContext) {
    // Slow flaky node + ticker coroutine on the same io_context.
    // If the retry sleep blocked (sleep_for path), the ticker would
    // never fire during the wait window. Async path lets the ticker
    // run in parallel.
    RetryPolicy p;
    p.max_retries = 2;
    p.initial_delay_ms = 50;
    p.backoff_multiplier = 1;
    p.max_delay_ms = 50;

    ExecutorHarness h(std::make_unique<FlakyNode>("slow", 1), p);

    asio::io_context io;
    std::optional<NodeResult> got;
    std::atomic<int> ticks{0};

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_state(state, h.channel_defs);
            got = co_await h.executor.execute_node_with_retry_async(
                "slow", state, nullptr, StreamMode::ALL, RunContext{});
        },
        asio::detached);

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto ex = co_await asio::this_coro::executor;
            for (int i = 0; i < 10; ++i) {
                asio::steady_timer t(ex);
                t.expires_after(std::chrono::milliseconds(10));
                co_await t.async_wait(asio::use_awaitable);
                ticks.fetch_add(1, std::memory_order_relaxed);
                if (got) co_return;
            }
        },
        asio::detached);

    io.run();

    ASSERT_TRUE(got.has_value());
    // Backoff window is ~50ms; ticker fires every 10ms. Conservative
    // bound: we expect at least 2 ticks before the retry resolves.
    // sleep_for path would yield 0.
    EXPECT_GE(ticks.load(), 2)
        << "ticks=" << ticks.load()
        << " — async retry sleep appears to block io_context";
}
