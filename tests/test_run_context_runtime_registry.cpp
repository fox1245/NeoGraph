// The engine binds execution-only state (task id, checkpoint store, write
// journal) to each RunContext through a process-wide registry keyed by the
// context's address. Many runs on many threads bind and unbind contexts at the
// same time; these tests pin that every context sees exactly its own binding
// however many are alive, however they are released, and that nothing stays
// bound afterwards.
#include "../src/core/run_context_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace neograph::graph;
using neograph::graph::detail::RunContextRuntime;
using neograph::graph::detail::ScopedInvocationRuntime;
using neograph::graph::detail::ScopedRunContextRuntime;
using neograph::graph::detail::runtime_for;

namespace {

std::shared_ptr<const RunContextRuntime> runtime_named(std::string name) {
    auto runtime = std::make_shared<RunContextRuntime>();
    runtime->graph_invocation_id = std::move(name);
    return runtime;
}

}  // namespace

TEST(RunContextRuntimeRegistry, ContextWithoutAScopeIsUnbound) {
    const RunContext ctx;
    EXPECT_FALSE(static_cast<bool>(runtime_for(ctx)));
    EXPECT_TRUE(runtime_for(ctx).invocation_id().empty());
}

TEST(RunContextRuntimeRegistry, InvocationInheritsTheRunsStateAndAddsItsOwnId) {
    const RunContext run_ctx, node_ctx;
    {
        ScopedRunContextRuntime run_scope(run_ctx, runtime_named("run-1"));
        ASSERT_TRUE(static_cast<bool>(runtime_for(run_ctx)));
        EXPECT_EQ(runtime_for(run_ctx)->graph_invocation_id, "run-1");
        EXPECT_TRUE(runtime_for(run_ctx).invocation_id().empty());
        {
            ScopedInvocationRuntime node_scope(run_ctx, node_ctx, "task-a");
            ASSERT_TRUE(static_cast<bool>(runtime_for(node_ctx)));
            EXPECT_EQ(runtime_for(node_ctx)->graph_invocation_id, "run-1");
            EXPECT_EQ(runtime_for(node_ctx).invocation_id(), "task-a");
            EXPECT_TRUE(runtime_for(run_ctx).invocation_id().empty());
        }
        EXPECT_FALSE(static_cast<bool>(runtime_for(node_ctx)));
        EXPECT_EQ(runtime_for(run_ctx)->graph_invocation_id, "run-1");
    }
    EXPECT_FALSE(static_cast<bool>(runtime_for(run_ctx)));
}

TEST(RunContextRuntimeRegistry, InvocationOfAnUnboundParentGetsAnEmptyRuntime) {
    const RunContext parent, node_ctx;
    ScopedInvocationRuntime scope(parent, node_ctx, "task-x");
    ASSERT_TRUE(static_cast<bool>(runtime_for(node_ctx)));
    EXPECT_EQ(runtime_for(node_ctx).invocation_id(), "task-x");
    EXPECT_EQ(runtime_for(node_ctx)->subgraph_write_journal, nullptr);
    EXPECT_EQ(runtime_for(node_ctx)->checkpoint_store, nullptr);
    EXPECT_FALSE(static_cast<bool>(runtime_for(parent)));
}

TEST(RunContextRuntimeRegistry, ReboundContextRestoresEachBindingInReverseOrder) {
    const RunContext run_ctx, node_ctx;
    ScopedRunContextRuntime run_scope(run_ctx, runtime_named("run-2"));
    auto first = std::make_unique<ScopedInvocationRuntime>(run_ctx, node_ctx, "first");
    EXPECT_EQ(runtime_for(node_ctx).invocation_id(), "first");
    auto second = std::make_unique<ScopedInvocationRuntime>(run_ctx, node_ctx, "second");
    EXPECT_EQ(runtime_for(node_ctx).invocation_id(), "second");
    second.reset();
    EXPECT_EQ(runtime_for(node_ctx).invocation_id(), "first");
    first.reset();
    EXPECT_FALSE(static_cast<bool>(runtime_for(node_ctx)));
}

// Eight threads each keep thousands of contexts bound at once (enough to grow
// every shard well past its initial table), look them up while the others do
// the same, then release them in random order and must leave nothing behind.
TEST(RunContextRuntimeRegistry, ManyThreadsKeepManyContextsBoundWithoutCrossTalk) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 1500;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<int> wrong{0};
    std::atomic<int> leaked{0};

    auto worker = [&](int thread_index) {
        std::vector<std::unique_ptr<RunContext>> runs, nodes;
        std::vector<std::unique_ptr<ScopedRunContextRuntime>> run_scopes;
        std::vector<std::unique_ptr<ScopedInvocationRuntime>> node_scopes;
        for (int i = 0; i < kPerThread; ++i) {
            runs.push_back(std::make_unique<RunContext>());
            nodes.push_back(std::make_unique<RunContext>());
        }
        ready.fetch_add(1);
        while (!go.load()) std::this_thread::yield();

        for (int i = 0; i < kPerThread; ++i) {
            const std::string tag = std::to_string(thread_index) + "-" + std::to_string(i);
            run_scopes.push_back(std::make_unique<ScopedRunContextRuntime>(*runs[i], runtime_named("run-" + tag)));
            node_scopes.push_back(std::make_unique<ScopedInvocationRuntime>(*runs[i], *nodes[i], "task-" + tag));
        }
        for (int i = 0; i < kPerThread; ++i) {
            const std::string tag = std::to_string(thread_index) + "-" + std::to_string(i);
            const auto run_view = runtime_for(*runs[i]);
            const auto node_view = runtime_for(*nodes[i]);
            if (!run_view || run_view->graph_invocation_id != "run-" + tag || !run_view.invocation_id().empty()) ++wrong;
            if (!node_view || node_view->graph_invocation_id != "run-" + tag ||
                node_view.invocation_id() != "task-" + tag) ++wrong;
        }

        std::mt19937 rng(static_cast<unsigned>(thread_index) + 1);
        std::vector<int> order(kPerThread);
        for (int i = 0; i < kPerThread; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        for (int k = 0; k < kPerThread; ++k) {
            const int i = order[k];
            node_scopes[i].reset();
            if (runtime_for(*nodes[i])) ++leaked;
            // Every other binding is still intact while its neighbours leave.
            const int other = order[(k + kPerThread / 2) % kPerThread];
            if (other != i && node_scopes[other] && !runtime_for(*nodes[other])) ++wrong;
        }
        for (int k = 0; k < kPerThread; ++k) {
            const int i = order[k];
            run_scopes[i].reset();
            if (runtime_for(*runs[i])) ++leaked;
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    while (ready.load() < kThreads) std::this_thread::yield();
    go.store(true);
    for (auto& t : threads) t.join();

    EXPECT_EQ(wrong.load(), 0);
    EXPECT_EQ(leaked.load(), 0);
}
