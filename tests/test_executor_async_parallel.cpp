// Stage 3 / Sem 3.7 — NodeExecutor::run_parallel_async regression.
//
// The async fan-out path uses asio::experimental::make_parallel_group
// on co_spawn-deferred workers. Contract vs sync run_parallel:
//   * same ready-order result application
//   * first exception propagates (and if NodeInterrupt, a dedicated
//     cp with next_nodes={offender} is saved first)
//   * siblings' pending_writes are recorded before the throw
//   * io_context is NOT blocked during branch work — other
//     coroutines on the same executor keep running
//
// Uses the shared harness conventions from test_executor_async_retry
// but with a richer setup (multiple nodes, CheckpointCoordinator,
// multi-channel state).

#include <gtest/gtest.h>
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
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Async-native work node — each execute_async waits delay_ms on a
// timer and writes to the "findings" append channel. Async so that
// the parallel path shows real overlap on a single-threaded
// io_context.
class AsyncWorkerNode : public GraphNode {
    std::string name_;
    int delay_ms_;
public:
    AsyncWorkerNode(std::string name, int delay_ms)
        : name_(std::move(name)), delay_ms_(delay_ms) {}

    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState&) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await t.async_wait(asio::use_awaitable);
        json append = json::array();
        append.push_back("done-by-" + name_);
        co_return std::vector<ChannelWrite>{ChannelWrite{"findings", append}};
    }

    // 3.0: execute_full_async default bridges to sync execute_full
    // (for Command/Send preservation), which would serialize this
    // node's async timer under a fresh io_context per call. Override
    // here to keep the non-blocking timer on the caller's executor —
    // the contract documented on GraphNode::execute_full_async.
    asio::awaitable<NodeResult>
    execute_full_async(const GraphState& state) override {
        auto writes = co_await execute_async(state);
        co_return NodeResult{std::move(writes)};
    }

    std::string get_name() const override { return name_; }
};

class InterruptingNode : public GraphNode {
    std::string name_;
public:
    explicit InterruptingNode(std::string name) : name_(std::move(name)) {}
    std::vector<ChannelWrite> execute(const GraphState&) override {
        throw NodeInterrupt("human input required: " + name_);
    }
    std::string get_name() const override { return name_; }
};

struct ParallelHarness {
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<ChannelDef> channel_defs;
    NodeExecutor executor;

    explicit ParallelHarness(
        std::vector<std::unique_ptr<GraphNode>> node_vec,
        RetryPolicy policy = RetryPolicy{})
        : nodes(map_from(std::move(node_vec))),
          channel_defs(make_defs()),
          executor(nodes, channel_defs,
                   [policy](const std::string&) { return policy; }) {}

private:
    static std::map<std::string, std::unique_ptr<GraphNode>>
    map_from(std::vector<std::unique_ptr<GraphNode>> v) {
        std::map<std::string, std::unique_ptr<GraphNode>> m;
        for (auto& n : v) {
            auto name = n->get_name();
            m.emplace(name, std::move(n));
        }
        return m;
    }
    static std::vector<ChannelDef> make_defs() {
        ChannelDef cd;
        cd.name = "findings";
        cd.reducer_name = "append";
        cd.type = ReducerType::APPEND;
        return {cd};
    }
};

void init_parallel_state(GraphState& state,
                         const std::vector<ChannelDef>& defs) {
    for (const auto& cd : defs) {
        auto reducer = ReducerRegistry::instance().get(cd.reducer_name);
        json initial = json::array();  // APPEND channel starts as []
        state.init_channel(cd.name, cd.type, reducer, initial);
    }
}

} // namespace

TEST(ExecutorAsyncParallel, AllBranchesCompleteInReadyOrder) {
    std::vector<std::unique_ptr<GraphNode>> nodes;
    nodes.push_back(std::make_unique<AsyncWorkerNode>("alpha", 30));
    nodes.push_back(std::make_unique<AsyncWorkerNode>("beta", 30));
    nodes.push_back(std::make_unique<AsyncWorkerNode>("gamma", 30));
    ParallelHarness h(std::move(nodes));
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid-parallel");
    BarrierState barrier;
    std::vector<std::string> trace;
    std::unordered_map<std::string, NodeResult> replay;
    std::vector<std::string> ready{"alpha", "beta", "gamma"};

    asio::io_context io;
    std::optional<std::vector<NodeResult>> got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_parallel_state(state, h.channel_defs);
            got = co_await h.executor.run_parallel_async(
                ready, 0, state, replay, coord, "", barrier,
                trace, nullptr, StreamMode::ALL);
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->size(), 3u);
    // trace is appended in ready-order (scheduler-friendly contract)
    ASSERT_EQ(trace.size(), 3u);
    EXPECT_EQ(trace[0], "alpha");
    EXPECT_EQ(trace[1], "beta");
    EXPECT_EQ(trace[2], "gamma");
}

TEST(ExecutorAsyncParallel, BranchesRunConcurrentlyOnSharedIoContext) {
    // Three branches × 50ms each. Sequential would take 150ms; true
    // parallel on one io_context converges to ~50ms. Bound generously
    // against scheduling jitter.
    std::vector<std::unique_ptr<GraphNode>> nodes;
    nodes.push_back(std::make_unique<AsyncWorkerNode>("a", 50));
    nodes.push_back(std::make_unique<AsyncWorkerNode>("b", 50));
    nodes.push_back(std::make_unique<AsyncWorkerNode>("c", 50));
    ParallelHarness h(std::move(nodes));
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid-overlap");
    BarrierState barrier;
    std::vector<std::string> trace;
    std::unordered_map<std::string, NodeResult> replay;
    std::vector<std::string> ready{"a", "b", "c"};

    asio::io_context io;
    std::optional<std::vector<NodeResult>> got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            GraphState state; init_parallel_state(state, h.channel_defs);
            got = co_await h.executor.run_parallel_async(
                ready, 0, state, replay, coord, "", barrier,
                trace, nullptr, StreamMode::ALL);
        },
        asio::detached);

    auto t0 = std::chrono::steady_clock::now();
    io.run();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    ASSERT_TRUE(got.has_value());
    // Anything under 140ms proves we're not purely sequential (3 × 50);
    // a tight bound of ~60ms would demand less jitter than CI provides.
    EXPECT_LT(elapsed_ms, 140)
        << "parallel async took " << elapsed_ms
        << "ms — expected ~50ms with true overlap, sequential floor 150ms";
}

TEST(ExecutorAsyncParallel, NodeInterruptSavesDedicatedCheckpoint) {
    // One interrupter among quick async siblings. First exception
    // (NodeInterrupt) must surface AND a cp with next_nodes={offender}
    // must land in the store before the throw.
    std::vector<std::unique_ptr<GraphNode>> nodes;
    nodes.push_back(std::make_unique<AsyncWorkerNode>("alpha", 5));
    nodes.push_back(std::make_unique<InterruptingNode>("halt"));
    nodes.push_back(std::make_unique<AsyncWorkerNode>("gamma", 5));
    ParallelHarness h(std::move(nodes));
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid-interrupt");
    BarrierState barrier;
    std::vector<std::string> trace;
    std::unordered_map<std::string, NodeResult> replay;
    std::vector<std::string> ready{"alpha", "halt", "gamma"};

    asio::io_context io;
    std::exception_ptr err;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                GraphState state; init_parallel_state(state, h.channel_defs);
                co_await h.executor.run_parallel_async(
                    ready, 0, state, replay, coord, "", barrier,
                    trace, nullptr, StreamMode::ALL);
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(err);
    EXPECT_THROW(std::rethrow_exception(err), NodeInterrupt);

    auto cps = store->list("tid-interrupt", 100);
    bool found_interrupt = false;
    for (const auto& cp : cps) {
        if (cp.interrupt_phase == CheckpointPhase::NodeInterrupt) {
            ASSERT_EQ(cp.next_nodes.size(), 1u);
            EXPECT_EQ(cp.next_nodes[0], "halt");
            found_interrupt = true;
        }
    }
    EXPECT_TRUE(found_interrupt);
}

TEST(ExecutorAsyncParallel, SiblingsRecordPendingWritesBeforeThrow) {
    // Resume contract: when one branch throws, the siblings that
    // already succeeded must have their pending writes persisted, so
    // resume's replay map skips them. Pair this with a real InMemory
    // store so we can observe pending writes after the throw.
    std::vector<std::unique_ptr<GraphNode>> nodes;
    nodes.push_back(std::make_unique<AsyncWorkerNode>("quick", 5));
    nodes.push_back(std::make_unique<InterruptingNode>("halt"));
    ParallelHarness h(std::move(nodes));
    auto store = std::make_shared<InMemoryCheckpointStore>();
    CheckpointCoordinator coord(store, "tid-pending");

    // Seed a parent cp so record_pending_write isn't short-circuited
    // by the empty-parent guard.
    BarrierState empty;
    GraphState seeder;
    init_parallel_state(seeder, h.channel_defs);
    auto parent_cp_id = coord.save_super_step(seeder, "", {}, CheckpointPhase::Completed,
                                              -1, "", empty);

    BarrierState barrier;
    std::vector<std::string> trace;
    std::unordered_map<std::string, NodeResult> replay;
    std::vector<std::string> ready{"quick", "halt"};

    asio::io_context io;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                GraphState state; init_parallel_state(state, h.channel_defs);
                co_await h.executor.run_parallel_async(
                    ready, 0, state, replay, coord, parent_cp_id, barrier,
                    trace, nullptr, StreamMode::ALL);
            } catch (...) {
                // expected
            }
        },
        asio::detached);
    io.run();

    EXPECT_EQ(store->pending_writes_count("tid-pending", parent_cp_id), 1u)
        << "the successful sibling ('quick') must have recorded its pending "
           "write before the parallel group resolved the interrupt";
}
