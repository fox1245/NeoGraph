// Fault-injection tests for the PendingWrite / partial-failure recovery
// machinery. The goal is to prove two claims:
//
//   1. When a Send fan-out partially fails mid-super-step, successful
//      siblings' results are durably recorded as pending writes against
//      the parent checkpoint.
//   2. On resume (with the failure disabled), those successful siblings
//      are replayed from the pending log instead of being re-executed,
//      so only the previously-failing node runs a second time.
//
// The scenario mirrors the Plan & Executor pattern: one "planner" node
// produces N Sends to a shared "executor" node, each with a distinct
// task_idx input. The executor is wired to throw when its input matches
// a configurable "fail_on_idx" — flipping that flag to -1 on resume
// simulates the operator restoring service after a transient failure.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <atomic>
#include <stdexcept>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Planner: stateless, emits N Sends to "executor" with {task_idx: i}.
class PlannerNode : public GraphNode {
public:
    explicit PlannerNode(int fanout) : fanout_(fanout) {}

    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }

    NodeResult execute_full(const GraphState&) override {
        NodeResult nr;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "executor";
            s.input = {{"task_idx", i}};
            nr.sends.push_back(std::move(s));
        }
        return nr;
    }
    std::string name() const override { return "planner"; }
private:
    int fanout_;
};

// Executor: reads task_idx, counts invocations, throws if fail_on matches.
class CrashyExecutorNode : public GraphNode {
public:
    CrashyExecutorNode(std::atomic<int>* counter, std::atomic<int>* fail_on)
        : counter_(counter), fail_on_(fail_on) {}

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        json v = state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -999;
        counter_->fetch_add(1, std::memory_order_relaxed);

        if (fail_on_->load(std::memory_order_relaxed) == idx) {
            throw std::runtime_error("simulated executor failure at idx=" +
                                     std::to_string(idx));
        }
        // Write a result entry. The main-state "results" channel is
        // append-reduced, so all successful sends accumulate.
        return {ChannelWrite{"results", json(idx * 10)}};
    }
    std::string name() const override { return "executor"; }

private:
    std::atomic<int>* counter_;
    std::atomic<int>* fail_on_;
};

// A trivial setup node so the graph has a committed checkpoint BEFORE
// the planner super-step runs. Pending writes need a parent cp to attach
// to; without this, crash recovery on the first super-step has no anchor.
class SetupNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {ChannelWrite{"setup_done", json(true)}};
    }
    std::string name() const override { return "setup"; }
};

static json make_plan_executor_graph(int fanout) {
    return {
        {"name", "plan_executor_test"},
        {"channels", {
            {"setup_done", {{"reducer", "overwrite"}}},
            {"task_idx",   {{"reducer", "overwrite"}}},
            {"results",    {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"setup",    {{"type", "setup"}}},
            {"planner",  {{"type", "planner"}}},
            {"executor", {{"type", "executor"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"},     {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        }}
    };
}

class PendingWritesTest : public ::testing::Test {
protected:
    std::atomic<int> exec_counter{0};
    std::atomic<int> fail_on{-1};
    int fanout = 5;

    void SetUp() override {
        exec_counter = 0;
        fail_on = -1;

        NodeFactory::instance().register_type("setup",
            [](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<SetupNode>();
            });
        NodeFactory::instance().register_type("planner",
            [this](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<PlannerNode>(fanout);
            });
        NodeFactory::instance().register_type("executor",
            [this](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<CrashyExecutorNode>(&exec_counter, &fail_on);
            });
    }
};

} // namespace

// ── 1. First run: crash mid-fan-out leaves pending writes durably stored ──

TEST_F(PendingWritesTest, PartialFailureRecordsPendingWrites) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        make_plan_executor_graph(fanout), NodeContext{}, store);

    fail_on = 2;  // send index 2 will throw

    RunConfig cfg;
    cfg.thread_id = "plan-exec-001";

    EXPECT_THROW(engine->run(cfg), std::runtime_error);

    // Latest committed cp is from the setup super-step. Pending writes
    // should be attached to it — one for the planner (which produced the
    // sends successfully) plus one per successful send (4 out of 5).
    auto cp = store->load_latest("plan-exec-001");
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->current_node, "setup");
    EXPECT_EQ(cp->next_node, "planner");

    size_t pending = store->pending_writes_count("plan-exec-001", cp->id);
    // 1 (planner) + 4 (successful sends: 0, 1, 3, 4) = 5
    EXPECT_EQ(pending, 5u);

    // Counter: executor ran for idx 0,1,2,3,4 exactly once each.
    // (Taskflow fans out all five even though idx=2 throws; the failure
    // only stops the super-step from committing.)
    EXPECT_EQ(exec_counter.load(), 5);
}

// ── 2. Resume replays successful siblings; only the failed idx re-runs ──

TEST_F(PendingWritesTest, ResumeSkipsCompletedSends) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        make_plan_executor_graph(fanout), NodeContext{}, store);

    // Phase 1: crash on idx=2
    fail_on = 2;
    RunConfig cfg;
    cfg.thread_id = "plan-exec-002";
    EXPECT_THROW(engine->run(cfg), std::runtime_error);
    EXPECT_EQ(exec_counter.load(), 5);

    // Phase 2: disable the failure and resume.
    fail_on = -1;
    auto resumed = engine->resume("plan-exec-002");

    // Executor should have been invoked exactly once more (the retry for
    // idx=2). Total = 5 (phase 1) + 1 (phase 2 retry) = 6.
    EXPECT_EQ(exec_counter.load(), 6);

    // The super-step that crashed should now be committed. Final state
    // must contain all 5 "results" entries (order-independent).
    ASSERT_TRUE(resumed.output.contains("channels"));
    auto& ch = resumed.output["channels"];
    ASSERT_TRUE(ch.contains("results"));
    auto& results_value = ch["results"]["value"];
    ASSERT_TRUE(results_value.is_array());
    EXPECT_EQ(results_value.size(), 5u);

    std::set<int> got;
    for (const auto& v : results_value) got.insert(v.get<int>());
    EXPECT_EQ(got, (std::set<int>{0, 10, 20, 30, 40}));

    // Pending writes for the parent cp must have been cleared once the
    // super-step committed successfully.
    auto cp_list = store->list("plan-exec-002");
    ASSERT_FALSE(cp_list.empty());
    // The setup cp is the oldest — its pending log should be empty.
    const Checkpoint* setup_cp = nullptr;
    for (const auto& cp : cp_list) {
        if (cp.current_node == "setup") { setup_cp = &cp; break; }
    }
    ASSERT_NE(setup_cp, nullptr);
    EXPECT_EQ(store->pending_writes_count("plan-exec-002", setup_cp->id), 0u);
}

// ── 3. Fresh run with no crash: zero pending writes left behind ──

TEST_F(PendingWritesTest, HappyPathLeavesNoPendingWrites) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        make_plan_executor_graph(fanout), NodeContext{}, store);

    fail_on = -1;  // no failure
    RunConfig cfg;
    cfg.thread_id = "plan-exec-003";
    auto result = engine->run(cfg);

    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(exec_counter.load(), 5);

    // All pending write buckets should have been cleared at super-step commit.
    auto cps = store->list("plan-exec-003");
    for (const auto& cp : cps) {
        EXPECT_EQ(store->pending_writes_count("plan-exec-003", cp.id), 0u)
            << "leftover pending writes on cp " << cp.id
            << " (" << cp.current_node << ")";
    }
}

// ── 4. Task ID determinism: planner → sends have stable hashed ids ──
// Indirectly verified by test 2 (if task_ids weren't stable, replay would
// miss and exec_counter would be 10 on the second phase, not 6). This
// test adds a direct check by running the same crash scenario twice on
// two different threads and confirming identical pending counts.

TEST_F(PendingWritesTest, TaskIdsAreStableAcrossRuns) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        make_plan_executor_graph(fanout), NodeContext{}, store);

    fail_on = 3;

    for (const std::string tid : {"tid-a", "tid-b"}) {
        exec_counter = 0;
        RunConfig cfg;
        cfg.thread_id = tid;
        EXPECT_THROW(engine->run(cfg), std::runtime_error);

        auto cp = store->load_latest(tid);
        ASSERT_TRUE(cp.has_value());
        EXPECT_EQ(store->pending_writes_count(tid, cp->id), 5u);
    }
}
