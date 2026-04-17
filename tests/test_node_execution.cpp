// Fault-injection tests for execute_node_with_retry + NodeInterrupt paths.
// These lock in behavior that previously had no direct coverage:
//
//   * retry loop: exhaustion, recovery, exponential backoff, per-node override
//   * NodeInterrupt: bypasses retry, saves a dedicated checkpoint, resumes
//   * parallel fan-out under partial failure: successful siblings' pending
//     writes are durably recorded even when one branch throws
//
// The intent is to freeze the contract NodeExecutor will later be extracted
// against. If any of these tests start failing during a refactor, the
// extraction broke observable behavior — not the test.
//
// Uses minimal ad-hoc node classes (FlakyNode, InterruptOnceNode) rather
// than real LLMCallNode so the failure/interrupt trigger is deterministic.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>
#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A node whose execution can be configured to fail the first N attempts
// and then succeed. `attempts` counts every invocation (including retries).
// `fail_remaining` is decremented each attempt; when >0 it throws, when
// <=0 it writes {name}_done=true and returns normally.
class FlakyNode : public GraphNode {
public:
    FlakyNode(std::string n, std::atomic<int>* attempts, std::atomic<int>* fail_remaining)
        : name_(std::move(n)), attempts_(attempts), fail_remaining_(fail_remaining) {}

    std::vector<ChannelWrite> execute(const GraphState&) override {
        attempts_->fetch_add(1, std::memory_order_relaxed);
        if (fail_remaining_->fetch_sub(1, std::memory_order_relaxed) > 0) {
            throw std::runtime_error("flaky: injected failure in " + name_);
        }
        return {ChannelWrite{name_ + "_done", json(true)}};
    }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* attempts_;
    std::atomic<int>* fail_remaining_;
};

// A node that throws NodeInterrupt as long as a flag is set, otherwise
// completes successfully. Used to drive interrupt + resume flows.
class InterruptOnceNode : public GraphNode {
public:
    InterruptOnceNode(std::string n, std::atomic<int>* attempts, std::atomic<bool>* should_interrupt)
        : name_(std::move(n)), attempts_(attempts), should_interrupt_(should_interrupt) {}

    std::vector<ChannelWrite> execute(const GraphState&) override {
        attempts_->fetch_add(1, std::memory_order_relaxed);
        if (should_interrupt_->load(std::memory_order_relaxed)) {
            throw NodeInterrupt("needs approval");
        }
        return {ChannelWrite{name_ + "_done", json(true)}};
    }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* attempts_;
    std::atomic<bool>* should_interrupt_;
};

// Helpers to register one-off node types for a given fixture.
NodeFactoryFn make_flaky_factory(std::atomic<int>* attempts, std::atomic<int>* fail_remaining) {
    return [attempts, fail_remaining](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<FlakyNode>(name, attempts, fail_remaining);
    };
}

NodeFactoryFn make_interrupt_factory(std::atomic<int>* attempts, std::atomic<bool>* should_interrupt) {
    return [attempts, should_interrupt](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<InterruptOnceNode>(name, attempts, should_interrupt);
    };
}

// Minimal single-node graph definition (__start__ → node → __end__).
json make_single_flaky_graph(int global_max_retries = 0, int initial_delay_ms = 0) {
    json graph = {
        {"name", "flaky_single"},
        {"channels", {{"flaky_done", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"flaky", {{"type", "flaky"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "flaky"}},
            {{"from", "flaky"}, {"to", "__end__"}}
        })}
    };
    if (global_max_retries > 0) {
        graph["retry_policy"] = {
            {"max_retries", global_max_retries},
            {"initial_delay_ms", initial_delay_ms},
            {"backoff_multiplier", 2.0},
            {"max_delay_ms", 5000}
        };
    }
    return graph;
}

// Trivial predecessor that just commits a parent checkpoint so partial
// fan-out failures downstream have somewhere to attach pending writes.
class SetupNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {ChannelWrite{"setup_done", json(true)}};
    }
    std::string name() const override { return "setup"; }
};

// Fan-out graph: setup → {a, b} → __end__. setup produces a committed
// parent cp so pending writes from the partial-failure test have an
// anchor — same pattern as test_pending_writes.cpp.
json make_parallel_fanout_graph() {
    return {
        {"name", "fanout_parallel"},
        {"channels", {
            {"setup_done", {{"reducer", "overwrite"}}},
            {"a_done", {{"reducer", "overwrite"}}},
            {"b_done", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"setup", {{"type", "parallel_setup"}}},
            {"a", {{"type", "flaky_a"}}},
            {"b", {{"type", "flaky_b"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"}, {"to", "a"}},
            {{"from", "setup"}, {"to", "b"}},
            {{"from", "a"}, {"to", "__end__"}},
            {{"from", "b"}, {"to", "__end__"}}
        })}
    };
}

void register_parallel_setup_once() {
    NodeFactory::instance().register_type(
        "parallel_setup",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SetupNode>();
        });
}

} // namespace

// =========================================================================
// Retry loop
// =========================================================================

TEST(NodeExecutionRetry, PropagatesAfterMaxRetriesExhausted) {
    std::atomic<int> attempts{0};
    std::atomic<int> fail_remaining{999}; // never recovers
    NodeFactory::instance().register_type(
        "flaky", make_flaky_factory(&attempts, &fail_remaining));

    auto engine = GraphEngine::compile(
        make_single_flaky_graph(/*max_retries=*/2, /*delay_ms=*/1),
        NodeContext{});

    RunConfig cfg; cfg.thread_id = "retry-exhaust";
    EXPECT_THROW(engine->run(cfg), std::runtime_error);

    // 1 initial attempt + 2 retries = 3 total invocations.
    EXPECT_EQ(attempts.load(), 3);
}

TEST(NodeExecutionRetry, RecoversAfterTransientFailures) {
    std::atomic<int> attempts{0};
    std::atomic<int> fail_remaining{2}; // fail 2x, then succeed
    NodeFactory::instance().register_type(
        "flaky", make_flaky_factory(&attempts, &fail_remaining));

    auto engine = GraphEngine::compile(
        make_single_flaky_graph(/*max_retries=*/3, /*delay_ms=*/1),
        NodeContext{});

    RunConfig cfg; cfg.thread_id = "retry-recover";
    auto result = engine->run(cfg);

    // 2 failed + 1 successful = 3 invocations.
    EXPECT_EQ(attempts.load(), 3);
    EXPECT_EQ(result.output["channels"]["flaky_done"]["value"].get<bool>(), true);
    EXPECT_FALSE(result.interrupted);
}

TEST(NodeExecutionRetry, ExponentialBackoffWaitsAtLeastInitialDelay) {
    std::atomic<int> attempts{0};
    std::atomic<int> fail_remaining{1}; // fail once, then succeed
    NodeFactory::instance().register_type(
        "flaky", make_flaky_factory(&attempts, &fail_remaining));

    const int initial_delay_ms = 50;
    auto engine = GraphEngine::compile(
        make_single_flaky_graph(/*max_retries=*/3, initial_delay_ms),
        NodeContext{});

    RunConfig cfg; cfg.thread_id = "retry-backoff";

    auto t0 = std::chrono::steady_clock::now();
    engine->run(cfg);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // One failure → one backoff sleep before the successful retry. Engine
    // should have slept ≥ initial_delay_ms. Upper bound is generous
    // because CI machines are noisy.
    EXPECT_GE(elapsed, initial_delay_ms);
    EXPECT_LT(elapsed, initial_delay_ms + 2000);
    EXPECT_EQ(attempts.load(), 2);
}

TEST(NodeExecutionRetry, PerNodePolicyOverridesDefault) {
    std::atomic<int> attempts{0};
    std::atomic<int> fail_remaining{2};
    NodeFactory::instance().register_type(
        "flaky", make_flaky_factory(&attempts, &fail_remaining));

    // Graph declares NO retry policy — default is max_retries=0.
    auto engine = GraphEngine::compile(make_single_flaky_graph(), NodeContext{});

    // But we override per-node: grant the flaky node 3 retries.
    RetryPolicy policy;
    policy.max_retries = 3;
    policy.initial_delay_ms = 1;
    policy.backoff_multiplier = 2.0f;
    policy.max_delay_ms = 100;
    engine->set_node_retry_policy("flaky", policy);

    RunConfig cfg; cfg.thread_id = "retry-per-node";
    auto result = engine->run(cfg);

    EXPECT_EQ(attempts.load(), 3);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(result.output["channels"]["flaky_done"]["value"].get<bool>(), true);
}

TEST(NodeExecutionRetry, ZeroRetriesByDefaultMeansOneAttempt) {
    std::atomic<int> attempts{0};
    std::atomic<int> fail_remaining{1}; // one failure is enough
    NodeFactory::instance().register_type(
        "flaky", make_flaky_factory(&attempts, &fail_remaining));

    // No retry_policy in JSON → default (max_retries=0).
    auto engine = GraphEngine::compile(make_single_flaky_graph(), NodeContext{});

    RunConfig cfg; cfg.thread_id = "retry-default";
    EXPECT_THROW(engine->run(cfg), std::runtime_error);

    EXPECT_EQ(attempts.load(), 1);
}

// =========================================================================
// NodeInterrupt vs retry interaction
// =========================================================================

TEST(NodeExecutionInterrupt, NodeInterruptBypassesRetryLoop) {
    std::atomic<int> attempts{0};
    std::atomic<bool> should_interrupt{true};
    NodeFactory::instance().register_type(
        "interrupter", make_interrupt_factory(&attempts, &should_interrupt));

    json graph = {
        {"name", "interrupt_single"},
        {"channels", {{"interrupter_done", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"interrupter", {{"type", "interrupter"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "interrupter"}},
            {{"from", "interrupter"}, {"to", "__end__"}}
        })},
        {"retry_policy", {
            {"max_retries", 5},
            {"initial_delay_ms", 1},
            {"backoff_multiplier", 2.0},
            {"max_delay_ms", 100}
        }}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "interrupt-no-retry";
    auto result = engine->run(cfg);

    // Even with max_retries=5, NodeInterrupt must short-circuit after one
    // attempt. If retry ever runs for NodeInterrupt, attempts > 1.
    EXPECT_EQ(attempts.load(), 1);
    EXPECT_TRUE(result.interrupted);
}

TEST(NodeExecutionInterrupt, SavesNodeInterruptCheckpointOnSingleNodePath) {
    std::atomic<int> attempts{0};
    std::atomic<bool> should_interrupt{true};
    NodeFactory::instance().register_type(
        "interrupter", make_interrupt_factory(&attempts, &should_interrupt));

    json graph = {
        {"name", "interrupt_cp"},
        {"channels", {{"interrupter_done", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"interrupter", {{"type", "interrupter"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "interrupter"}},
            {{"from", "interrupter"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "interrupt-cp";
    engine->run(cfg);

    // Latest checkpoint must carry phase=NodeInterrupt and rewind the
    // ready set to the interrupting node only.
    auto cp = store->load_latest("interrupt-cp");
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(cp->current_node, "interrupter");
    ASSERT_EQ(cp->next_nodes.size(), 1u);
    EXPECT_EQ(cp->next_nodes[0], "interrupter");
}

TEST(NodeExecutionInterrupt, ResumeRerunsInterruptedNodeOnce) {
    std::atomic<int> attempts{0};
    std::atomic<bool> should_interrupt{true};
    NodeFactory::instance().register_type(
        "interrupter", make_interrupt_factory(&attempts, &should_interrupt));

    json graph = {
        {"name", "interrupt_resume"},
        {"channels", {{"interrupter_done", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"interrupter", {{"type", "interrupter"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "interrupter"}},
            {{"from", "interrupter"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "interrupt-resume";
    auto first = engine->run(cfg);
    ASSERT_TRUE(first.interrupted);
    EXPECT_EQ(attempts.load(), 1);

    // Operator clears the interrupt condition and resumes.
    should_interrupt = false;
    auto second = engine->resume("interrupt-resume");

    EXPECT_FALSE(second.interrupted);
    // Resume re-executes the interrupted node exactly once (no retry, no
    // double-fire). Total attempts = 1 (first interrupt) + 1 (resume) = 2.
    EXPECT_EQ(attempts.load(), 2);
    EXPECT_EQ(second.output["channels"]["interrupter_done"]["value"].get<bool>(), true);
}

// =========================================================================
// Parallel fan-out under partial failure
// =========================================================================

TEST(NodeExecutionParallel, SuccessfulSiblingRecordsPendingWriteWhenOtherFails) {
    std::atomic<int> a_attempts{0};
    std::atomic<int> a_fail{0};   // 'a' succeeds
    std::atomic<int> b_attempts{0};
    std::atomic<int> b_fail{999}; // 'b' always fails

    register_parallel_setup_once();
    NodeFactory::instance().register_type(
        "flaky_a", make_flaky_factory(&a_attempts, &a_fail));
    NodeFactory::instance().register_type(
        "flaky_b", make_flaky_factory(&b_attempts, &b_fail));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_parallel_fanout_graph(), NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "parallel-partial";
    EXPECT_THROW(engine->run(cfg), std::runtime_error);

    // 'a' succeeded → its pending write must attach to the parent (the
    // setup cp, which committed the super-step before the fan-out).
    auto cps = store->list("parallel-partial");
    ASSERT_FALSE(cps.empty());

    const Checkpoint* parent = nullptr;
    for (const auto& cp : cps) {
        if (cp.current_node == "setup") { parent = &cp; break; }
    }
    ASSERT_NE(parent, nullptr);
    EXPECT_EQ(store->pending_writes_count("parallel-partial", parent->id), 1u)
        << "exactly one pending write (for successful sibling 'a')";

    EXPECT_EQ(a_attempts.load(), 1);
    EXPECT_EQ(b_attempts.load(), 1);
}

TEST(NodeExecutionParallel, AllSucceedClearsPendingWritesOnCommit) {
    std::atomic<int> a_attempts{0};
    std::atomic<int> a_fail{0};
    std::atomic<int> b_attempts{0};
    std::atomic<int> b_fail{0};

    register_parallel_setup_once();
    NodeFactory::instance().register_type(
        "flaky_a", make_flaky_factory(&a_attempts, &a_fail));
    NodeFactory::instance().register_type(
        "flaky_b", make_flaky_factory(&b_attempts, &b_fail));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_parallel_fanout_graph(), NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "parallel-happy";
    auto result = engine->run(cfg);

    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(a_attempts.load(), 1);
    EXPECT_EQ(b_attempts.load(), 1);

    // After a successful super-step commit the pending write bucket on
    // every parent checkpoint must be empty — otherwise the replay log
    // would accumulate unboundedly across steps.
    auto cps = store->list("parallel-happy");
    for (const auto& cp : cps) {
        EXPECT_EQ(store->pending_writes_count("parallel-happy", cp.id), 0u)
            << "leftover pending writes on cp " << cp.id
            << " (" << cp.current_node << ")";
    }
}
