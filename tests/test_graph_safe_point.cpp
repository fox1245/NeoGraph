#include <neograph/neograph.h>

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <climits>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::atomic<int> safe_point_a_hits{0};
std::atomic<int> safe_point_b_hits{0};
std::atomic<int> safe_point_active{0};
std::atomic<int> safe_point_started{0};
std::atomic<int> safe_point_finished{0};
std::atomic_bool safe_point_release{false};

class SafePointNode final : public GraphNode {
public:
    explicit SafePointNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        if (name_ == "a") safe_point_a_hits.fetch_add(1, std::memory_order_relaxed);
        if (name_ == "b") safe_point_b_hits.fetch_add(1, std::memory_order_relaxed);
        if (name_ == "left" || name_ == "right") {
            safe_point_active.fetch_add(1, std::memory_order_acq_rel);
            safe_point_started.fetch_add(1, std::memory_order_acq_rel);
            while (!safe_point_release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            safe_point_finished.fetch_add(1, std::memory_order_acq_rel);
            safe_point_active.fetch_sub(1, std::memory_order_acq_rel);
        }

        NodeOutput output;
        output.writes.push_back(ChannelWrite{"visited", json(name_)});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

void register_safe_point_node() {
    NodeFactory::instance().register_type(
        "safe_point_test_node",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SafePointNode>(name);
        });
}

json sequential_graph() {
    return {{"name", "safe_point_sequential"},
            {"channels", {{"visited", {{"reducer", "append"}}}}},
            {"nodes",
             {{"a", {{"type", "safe_point_test_node"}}},
              {"b", {{"type", "safe_point_test_node"}}}}},
            {"edges",
             json::array({{{"from", "__start__"}, {"to", "a"}},
                          {{"from", "a"}, {"to", "b"}},
                          {{"from", "b"}, {"to", "__end__"}}})}};
}

json parallel_graph() {
    return {{"name", "safe_point_parallel"},
            {"channels", {{"visited", {{"reducer", "append"}}}}},
            {"nodes",
             {{"left", {{"type", "safe_point_test_node"}}},
              {"right", {{"type", "safe_point_test_node"}}}}},
            {"edges",
             json::array({{{"from", "__start__"}, {"to", "left"}},
                          {{"from", "__start__"}, {"to", "right"}},
                          {{"from", "left"}, {"to", "__end__"}},
                          {{"from", "right"}, {"to", "__end__"}}})}};
}

GraphGenerationIdentity generation_identity(std::string graph_name) {
    return {std::move(graph_name), "test-core-generation-v1"};
}

std::unique_ptr<GraphEngine> make_identified_engine(
    const json& definition,
    std::shared_ptr<CheckpointStore> store,
    std::size_t worker_count = 1) {
    auto compiled = GraphCompiler::compile(definition, NodeContext{});
    const auto identity = generation_identity(compiled.name);
    EngineConfig config;
    config.checkpoint_store = std::move(store);
    config.worker_count = worker_count;
    return GraphEngine::link(
        std::move(compiled), std::move(config), {}, identity);
}

RunResult run_with_resources(GraphEngine& engine,
                             RunConfig config,
                             RunResources resources,
                             std::shared_ptr<GraphSafePointRequest> request) {
    asio::io_context io;
    auto future = asio::co_spawn(
        io, engine.run_until_safe_point_async(
                std::move(config), std::move(request), {}, {},
                std::move(resources)),
        asio::use_future);
    io.run();
    return future.get();
}

RunResult resume_with_resources(GraphEngine& engine,
                                RunConfig config,
                                std::string checkpoint_id,
                                RunResources resources,
                                std::shared_ptr<GraphSafePointRequest> request = {}) {
    asio::io_context io;
    if (request) {
        auto future = asio::co_spawn(
            io,
            engine.resume_from_until_safe_point_async(
                std::move(config), std::move(checkpoint_id), std::move(request),
                {}, {}, {}, std::move(resources)),
            asio::use_future);
        io.run();
        return future.get();
    }
    auto future = asio::co_spawn(
        io, engine.resume_from_async(
                std::move(config), std::move(checkpoint_id), {}, {}, {},
                std::move(resources)),
        asio::use_future);
    io.run();
    return future.get();
}

class FailingClearStore final : public InMemoryCheckpointStore {
public:
    bool fail_clear = false;

    asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override {
        if (fail_clear) throw std::runtime_error("injected pending-write clear failure");
        clear_writes(thread_id, parent_checkpoint_id);
        co_return;
    }
};

}  // namespace

TEST(GraphSafePoint, StopsBeforeExecutingTheCapturedFrontier) {
    register_safe_point_node();
    safe_point_a_hits = 0;
    safe_point_b_hits = 0;

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(sequential_graph(), store);
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    ASSERT_TRUE(request->request());

    RunConfig config;
    config.thread_id = "safe-point-sequential";
    RunResources resources;
    resources.checkpoint_store = store;
    const auto result = run_with_resources(*engine, config, resources, request);

    EXPECT_EQ(result.status(), RunStatus::SafePoint);
    EXPECT_TRUE(result.safe_point_reached());
    EXPECT_EQ(safe_point_a_hits.load(), 1);
    EXPECT_EQ(safe_point_b_hits.load(), 0);
    ASSERT_TRUE(request->captured());
    const auto safe_point = request->safe_point();
    ASSERT_TRUE(safe_point.has_value());
    const auto& checkpoint = safe_point->checkpoint();
    EXPECT_EQ(checkpoint.id, result.checkpoint_id);
    EXPECT_EQ(checkpoint.thread_id, config.thread_id);
    EXPECT_EQ(checkpoint.interrupt_phase, CheckpointPhase::Completed);
    EXPECT_EQ(checkpoint.step, 0);
    ASSERT_EQ(checkpoint.next_nodes.size(), 1U);
    EXPECT_EQ(checkpoint.next_nodes.front(), "b");

    RunResources resume_resources;
    resume_resources.checkpoint_store = store;
    const auto resumed = resume_with_resources(
        *engine, config, checkpoint.id, std::move(resume_resources));
    EXPECT_EQ(resumed.status(), RunStatus::Completed);
    EXPECT_EQ(safe_point_a_hits.load(), 1);
    EXPECT_EQ(safe_point_b_hits.load(), 1);
}

TEST(GraphSafePoint, InflightRequestWaitsForEveryParallelNode) {
    register_safe_point_node();
    safe_point_active = 0;
    safe_point_started = 0;
    safe_point_finished = 0;
    safe_point_release = false;

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(parallel_graph(), store, 2);
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_parallel"));

    RunConfig config;
    config.thread_id = "safe-point-parallel";
    RunResources resources;
    resources.checkpoint_store = store;

    auto running = std::async(std::launch::async, [&] {
        return run_with_resources(*engine, config, resources, request);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (safe_point_started.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (safe_point_started.load() != 2) {
        safe_point_release.store(true, std::memory_order_release);
        (void)running.get();
        FAIL() << "parallel nodes did not start before the deadline";
    }
    ASSERT_TRUE(request->request());
    EXPECT_FALSE(request->captured());

    safe_point_release.store(true, std::memory_order_release);
    const auto result = running.get();
    EXPECT_EQ(result.status(), RunStatus::SafePoint);
    EXPECT_EQ(safe_point_finished.load(), 2);
    EXPECT_EQ(safe_point_active.load(), 0);
    ASSERT_TRUE(request->safe_point().has_value());
    EXPECT_EQ(request->safe_point()->checkpoint().next_nodes,
              std::vector<std::string>{"__end__"});
}

TEST(GraphSafePoint, ClearFailurePublishesNoSafePoint) {
    register_safe_point_node();
    safe_point_a_hits = 0;
    safe_point_b_hits = 0;

    auto store = std::make_shared<FailingClearStore>();
    auto engine = make_identified_engine(sequential_graph(), store);
    RunConfig config;
    config.thread_id = "safe-point-clear-failure";
    config.max_steps = 1;
    const auto first = engine->run(config);
    ASSERT_EQ(first.status(), RunStatus::StepLimit);
    ASSERT_FALSE(first.checkpoint_id.empty());

    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    ASSERT_TRUE(request->request());
    store->fail_clear = true;
    config.max_steps = 10;
    RunResources resources;
    resources.checkpoint_store = store;
    EXPECT_THROW(
        (void)resume_with_resources(
            *engine, config, first.checkpoint_id, resources, request),
        std::runtime_error);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->captured());
    EXPECT_FALSE(request->safe_point().has_value());
    const auto latest = store->load_latest(config.thread_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_NE(latest->id, first.checkpoint_id);
    EXPECT_GT(store->pending_writes_count(config.thread_id, first.checkpoint_id), 0U);
}

TEST(GraphSafePoint, RequestWithoutCheckpointStoreFailsBeforeNodeExecution) {
    register_safe_point_node();
    safe_point_a_hits = 0;

    auto engine = make_identified_engine(sequential_graph(), {});
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    RunConfig config;
    config.thread_id = "safe-point-no-store";
    RunResources resources;

    EXPECT_THROW(
        (void)run_with_resources(*engine, config, resources, request),
        std::runtime_error);
    EXPECT_EQ(safe_point_a_hits.load(), 0);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->captured());
}

TEST(GraphSafePoint, CancellationBeforeCapturePublishesNoSafePoint) {
    register_safe_point_node();
    safe_point_active = 0;
    safe_point_started = 0;
    safe_point_finished = 0;
    safe_point_release = false;

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(parallel_graph(), store, 2);
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_parallel"));
    ASSERT_TRUE(request->request());
    auto cancellation = std::make_shared<CancelToken>();
    RunConfig config;
    config.thread_id = "safe-point-cancelled";
    config.cancel_token = cancellation;
    RunResources resources;
    resources.checkpoint_store = store;

    auto running = std::async(std::launch::async, [&] {
        return run_with_resources(*engine, config, resources, request);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (safe_point_started.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (safe_point_started.load() != 2) {
        safe_point_release.store(true, std::memory_order_release);
        (void)running.get();
        FAIL() << "parallel nodes did not start before the deadline";
    }
    cancellation->cancel();
    safe_point_release.store(true, std::memory_order_release);
    EXPECT_THROW((void)running.get(), CancelledException);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->captured());
    EXPECT_FALSE(request->safe_point().has_value());
}

TEST(GraphSafePoint, RejectsRequestForAnotherEngineGeneration) {
    register_safe_point_node();
    safe_point_a_hits = 0;
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(sequential_graph(), store);
    auto request = std::make_shared<GraphSafePointRequest>(
        GraphGenerationIdentity{"safe_point_sequential", "different-generation"});
    ASSERT_TRUE(request->request());
    RunConfig config;
    config.thread_id = "safe-point-wrong-generation";
    RunResources resources;
    resources.checkpoint_store = store;

    EXPECT_THROW(
        (void)run_with_resources(*engine, config, resources, request),
        std::runtime_error);
    EXPECT_EQ(safe_point_a_hits.load(), 0);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->captured());
}

TEST(GraphSafePoint, PreCancelledControlledResumeClosesItsRequest) {
    register_safe_point_node();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(sequential_graph(), store);
    RunConfig seed_config;
    seed_config.thread_id = "safe-point-pre-cancelled-resume";
    seed_config.max_steps = 1;
    const auto seeded = engine->run(seed_config);
    ASSERT_FALSE(seeded.checkpoint_id.empty());

    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    ASSERT_TRUE(request->request());
    auto cancellation = std::make_shared<CancelToken>();
    cancellation->cancel();
    RunConfig resume_config;
    resume_config.thread_id = seed_config.thread_id;
    resume_config.cancel_token = cancellation;
    RunResources resources;
    resources.checkpoint_store = store;

    EXPECT_THROW(
        (void)resume_with_resources(
            *engine, resume_config, seeded.checkpoint_id, resources, request),
        CancelledException);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->captured());
}

TEST(GraphSafePoint, RejectsOutOfRangeCheckpointStepBeforeResumeExecution) {
    register_safe_point_node();
    safe_point_a_hits = 0;
    safe_point_b_hits = 0;
    auto store = std::make_shared<InMemoryCheckpointStore>();
    Checkpoint checkpoint;
    checkpoint.id = "out-of-range-step";
    checkpoint.thread_id = "safe-point-step-range";
    checkpoint.channel_values =
        json{{"channels", json::object()},
             {"global_version", static_cast<unsigned long long>(0)}};
    checkpoint.current_node = "a";
    checkpoint.next_nodes = {"b"};
    checkpoint.interrupt_phase = CheckpointPhase::Completed;
    checkpoint.step = INT_MAX;
    checkpoint.timestamp = 1;
    store->save(checkpoint);
    auto engine = make_identified_engine(sequential_graph(), store);

    RunConfig config;
    config.thread_id = checkpoint.thread_id;
    EXPECT_THROW(
        (void)engine->resume_from(config, checkpoint.id), std::runtime_error);
    config.resume_if_exists = true;
    EXPECT_THROW((void)engine->run(config), std::runtime_error);
    EXPECT_EQ(safe_point_a_hits.load(), 0);
    EXPECT_EQ(safe_point_b_hits.load(), 0);

    checkpoint.id = "near-safe-point-step-limit";
    checkpoint.thread_id = "safe-point-near-step-limit";
    checkpoint.step = INT_MAX - 2;
    store->save(checkpoint);
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    ASSERT_TRUE(request->request());
    RunConfig controlled;
    controlled.thread_id = checkpoint.thread_id;
    RunResources resources;
    resources.checkpoint_store = store;
    EXPECT_THROW(
        (void)resume_with_resources(
            *engine, controlled, checkpoint.id, resources, request),
        std::runtime_error);
    EXPECT_TRUE(request->closed());
    EXPECT_EQ(safe_point_b_hits.load(), 0);

    checkpoint.id = "int64-max-step";
    checkpoint.thread_id = "safe-point-int64-max-step";
    checkpoint.step = std::numeric_limits<std::int64_t>::max();
    store->save(checkpoint);
    RunConfig overflow;
    overflow.thread_id = checkpoint.thread_id;
    EXPECT_THROW(
        (void)engine->resume_from(overflow, checkpoint.id),
        std::runtime_error);

    checkpoint.id = "int-max-before-step";
    checkpoint.thread_id = "safe-point-int-max-before-step";
    checkpoint.step = INT_MAX;
    checkpoint.interrupt_phase = CheckpointPhase::Before;
    store->save(checkpoint);
    RunConfig before;
    before.thread_id = checkpoint.thread_id;
    EXPECT_THROW(
        (void)engine->resume_from(before, checkpoint.id),
        std::runtime_error);
}

TEST(GraphSafePoint, UnfulfilledRequestClosesWithItsOperation) {
    register_safe_point_node();
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_identified_engine(sequential_graph(), store);
    auto request = std::make_shared<GraphSafePointRequest>(
        generation_identity("safe_point_sequential"));
    RunConfig config;
    config.thread_id = "safe-point-unrequested";
    RunResources resources;
    resources.checkpoint_store = store;

    const auto result = run_with_resources(*engine, config, resources, request);
    EXPECT_EQ(result.status(), RunStatus::Completed);
    EXPECT_TRUE(request->closed());
    EXPECT_FALSE(request->request());
    EXPECT_FALSE(request->safe_point().has_value());
}
