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
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#endif
#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Planner: stateless, emits N Sends to "executor" with {task_idx: i}.
class PlannerNode : public GraphNode {
public:
    explicit PlannerNode(int fanout, std::atomic<int>* counter = nullptr)
        : fanout_(fanout), counter_(counter) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        if (counter_) counter_->fetch_add(1, std::memory_order_relaxed);
        NodeOutput out;
        for (int i = 0; i < fanout_; ++i) {
            Send s;
            s.target_node = "executor";
            s.input = {{"task_idx", i}};
            out.sends.push_back(std::move(s));
        }
        co_return out;
    }
    std::string get_name() const override { return "planner"; }
private:
    int fanout_;
    std::atomic<int>* counter_;
};

// Executor: reads task_idx, counts invocations, throws if fail_on matches.
class CrashyExecutorNode : public GraphNode {
public:
    CrashyExecutorNode(std::atomic<int>* counter, std::atomic<int>* fail_on)
        : counter_(counter), fail_on_(fail_on) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        json v = in.state.get("task_idx");
        int idx = v.is_number_integer() ? v.get<int>() : -999;
        counter_->fetch_add(1, std::memory_order_relaxed);

        if (fail_on_->load(std::memory_order_relaxed) == idx) {
            throw std::runtime_error("simulated executor failure at idx=" +
                                     std::to_string(idx));
        }
        // Write a result entry. The main-state "results" channel is
        // append-reduced, so all successful sends accumulate.
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json(idx * 10)});
        co_return out;
    }
    std::string get_name() const override { return "executor"; }

private:
    std::atomic<int>* counter_;
    std::atomic<int>* fail_on_;
};

// A trivial setup node so the graph has a committed checkpoint BEFORE
// the planner super-step runs. Pending writes need a parent cp to attach
// to; without this, crash recovery on the first super-step has no anchor.
class SetupNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"setup_done", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "setup"; }
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
    ASSERT_EQ(cp->next_nodes.size(), 1u);
    EXPECT_EQ(cp->next_nodes[0], "planner");

    size_t pending = store->pending_writes_count("plan-exec-001", cp->id);
    // 1 (planner) + 4 (successful sends: 0, 1, 3, 4) = 5
    EXPECT_EQ(pending, 5u);

    // Counter: executor ran for idx 0,1,2,3,4 exactly once each. This preserves
    // the historical fan-out contract: all five branches finish even though
    // idx=2 throws; the failure only stops the super-step from committing.
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
    auto ch = resumed.output["channels"];
    ASSERT_TRUE(ch.contains("results"));
    auto results_value = ch["results"]["value"];
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

    // Regression for the leak observed in real PG dumps: the very first
    // super-step ran with `last_checkpoint_id == ""`, and the
    // record-vs-clear asymmetry left a permanent dead bucket keyed at
    // parent="". A clean run must leave NO leftover at parent=""
    // either, otherwise every fresh thread accumulates one row of
    // garbage in the writes table.
    EXPECT_EQ(store->pending_writes_count("plan-exec-003", ""), 0u)
        << "leftover pending writes at the empty-parent bucket "
           "— record_pending_write must skip when parent_cp_id is empty";
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

namespace {

// Entry-step planner->executor graph with no setup node in front:
// step 0 runs `planner` alone, which emits `fanout` Sends to `executor`.
json make_entry_plan_executor_graph() {
    return {
        {"name", "entry_plan_executor_test"},
        {"channels", {
            {"seed",     {{"reducer", "overwrite"}}},
            {"task_idx", {{"reducer", "overwrite"}}},
            {"results",  {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner",  {{"type", "entry_planner"}}},
            {"executor", {{"type", "entry_executor"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        }}
    };
}

class IndexedExecutorNode final : public GraphNode {
public:
    IndexedExecutorNode(std::string name,
                        std::atomic<int>* total_counter,
                        std::mutex* per_idx_mutex,
                        std::map<int, int>* per_idx_runs,
                        std::atomic<int>* fail_on)
        : name_(std::move(name)),
          total_counter_(total_counter),
          per_idx_mutex_(per_idx_mutex),
          per_idx_runs_(per_idx_runs),
          fail_on_(fail_on) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const json v = in.state.get("task_idx");
        const int idx = v.is_number_integer() ? v.get<int>() : -999;
        total_counter_->fetch_add(1, std::memory_order_relaxed);
        int invocation = 0;
        {
            std::lock_guard<std::mutex> lock(*per_idx_mutex_);
            invocation = ++(*per_idx_runs_)[idx];
        }
        if (fail_on_->load(std::memory_order_relaxed) == idx && invocation == 1) {
            throw std::runtime_error("simulated first-attempt failure at idx=" +
                                     std::to_string(idx));
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json(idx * 10)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* total_counter_;
    std::mutex* per_idx_mutex_;
    std::map<int, int>* per_idx_runs_;
    std::atomic<int>* fail_on_;
};

class EntryBranchNode final : public GraphNode {
public:
    EntryBranchNode(std::string name,
                    int branch_idx,
                    std::atomic<int>* total_counter,
                    std::mutex* per_idx_mutex,
                    std::map<int, int>* per_idx_runs,
                    std::atomic<int>* fail_on)
        : name_(std::move(name)),
          branch_idx_(branch_idx),
          total_counter_(total_counter),
          per_idx_mutex_(per_idx_mutex),
          per_idx_runs_(per_idx_runs),
          fail_on_(fail_on) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        total_counter_->fetch_add(1, std::memory_order_relaxed);
        int invocation = 0;
        {
            std::lock_guard<std::mutex> lock(*per_idx_mutex_);
            invocation = ++(*per_idx_runs_)[branch_idx_];
        }
        if (fail_on_->load(std::memory_order_relaxed) == branch_idx_ && invocation == 1) {
            throw std::runtime_error("simulated branch failure at branch=" +
                                     std::to_string(branch_idx_));
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json(branch_idx_ * 10)});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    int branch_idx_;
    std::atomic<int>* total_counter_;
    std::mutex* per_idx_mutex_;
    std::map<int, int>* per_idx_runs_;
    std::atomic<int>* fail_on_;
};

json make_entry_parallel_graph(int branch_count,
                               const std::vector<std::string>& interrupt_before = {}) {
    json nodes = json::object();
    json edges = json::array();
    for (int i = 0; i < branch_count; ++i) {
        const std::string name = "branch_" + std::to_string(i);
        nodes[name] = {{"type", name}};
        edges.push_back({{"from", "__start__"}, {"to", name}});
        edges.push_back({{"from", name}, {"to", "__end__"}});
    }
    json def = {
        {"name", "entry_parallel_test"},
        {"channels", {
            {"results", {{"reducer", "append"}}}
        }},
        {"nodes", std::move(nodes)},
        {"edges", std::move(edges)}
    };
    if (!interrupt_before.empty()) {
        def["interrupt_before"] = interrupt_before;
    }
    return def;
}

class CountingCheckpointStore final : public CheckpointStore {
public:
    explicit CountingCheckpointStore(std::shared_ptr<CheckpointStore> inner)
        : inner_(std::move(inner)) {}

    void save(const Checkpoint& cp) override {
        save_count.fetch_add(1, std::memory_order_relaxed);
        if (cp.interrupt_phase == CheckpointPhase::Before) {
            before_save_count.fetch_add(1, std::memory_order_relaxed);
        }
        inner_->save(cp);
    }

    std::optional<Checkpoint> load_latest(const std::string& thread_id) override {
        return inner_->load_latest(thread_id);
    }

    std::optional<Checkpoint> load_by_id(const std::string& checkpoint_id) override {
        return inner_->load_by_id(checkpoint_id);
    }

    std::vector<Checkpoint> list(const std::string& thread_id, int limit = 100) override {
        return inner_->list(thread_id, limit);
    }

    void delete_thread(const std::string& thread_id) override {
        inner_->delete_thread(thread_id);
    }

    void put_writes(const std::string& thread_id,
                    const std::string& checkpoint_id,
                    const PendingWrite& write) override {
        put_writes_count.fetch_add(1, std::memory_order_relaxed);
        inner_->put_writes(thread_id, checkpoint_id, write);
    }

    std::vector<PendingWrite> get_writes(const std::string& thread_id,
                                         const std::string& checkpoint_id) override {
        return inner_->get_writes(thread_id, checkpoint_id);
    }

    void clear_writes(const std::string& thread_id,
                      const std::string& checkpoint_id) override {
        inner_->clear_writes(thread_id, checkpoint_id);
    }

    bool requires_managed_budget(const std::string& thread_id) override {
        return inner_->requires_managed_budget(thread_id);
    }

    std::atomic<size_t> save_count{0};
    std::atomic<size_t> before_save_count{0};
    std::atomic<size_t> put_writes_count{0};

private:
    std::shared_ptr<CheckpointStore> inner_;
};

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
struct TempSqliteFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("neograph-first-step-anchor-" + Checkpoint::generate_id() + ".sqlite");
    ~TempSqliteFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
};
#endif

std::multiset<int> sorted_int_results(const RunResult& result) {
    std::multiset<int> out;
    const auto& arr = result.output.at("channels").at("results").at("value");
    for (const auto& item : arr) {
        out.insert(item.get<int>());
    }
    return out;
}

void verify_step_zero_anchor_and_clean_writes(
    CheckpointStore& store,
    GraphEngine& engine,
    const std::string& thread_id,
    const std::vector<std::string>& expected_anchor_next) {
    const auto cps = store.list(thread_id);
    ASSERT_EQ(cps.size(), 2u);
    const Checkpoint* completed = nullptr;
    const Checkpoint* anchor = nullptr;
    for (const auto& cp : cps) {
        if (cp.interrupt_phase == CheckpointPhase::Completed) completed = &cp;
        if (cp.interrupt_phase == CheckpointPhase::Before) anchor = &cp;
        EXPECT_TRUE(store.get_writes(thread_id, cp.id).empty())
            << "leftover pending writes on checkpoint " << cp.id;
    }
    EXPECT_TRUE(store.get_writes(thread_id, "").empty());
    ASSERT_NE(completed, nullptr);
    ASSERT_NE(anchor, nullptr);
    EXPECT_EQ(anchor->step, 0);
    EXPECT_EQ(anchor->current_node, kFirstStepAnchorNode);
    EXPECT_TRUE(anchor->parent_id.empty());
    EXPECT_EQ(anchor->next_nodes, expected_anchor_next);
    EXPECT_EQ(completed->step, 0);
    EXPECT_EQ(completed->parent_id, anchor->id);

    const auto history = engine.get_state_history(thread_id);
    ASSERT_EQ(history.size(), 2u);
}

void run_first_step_sends_recovery_check(
    std::function<std::shared_ptr<CheckpointStore>()> open_store,
    bool limit_planner_visits = false) {
    constexpr int kFanout = 5;
    const auto graph_json = [limit_planner_visits] {
        auto graph = make_entry_plan_executor_graph();
        // The anchor of a step must keep the visit counters from before the
        // step: the resumed step replays the planner and counts it once more.
        if (limit_planner_visits) {
            graph["schema_version"]        = 1;  // failure routing keys are read from version 1 on
            graph["node_retry_policies"] = json{{"planner", json{{"max_visits", 2}}}};
        }
        return graph;
    };
    std::atomic<int> planner_runs{0};
    std::atomic<int> exec_runs{0};
    std::atomic<int> fail_on{2};
    std::mutex per_idx_mutex;
    std::map<int, int> per_idx_runs;

    NodeFactory::instance().register_type("entry_planner",
        [&](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>(kFanout, &planner_runs);
        });
    NodeFactory::instance().register_type("entry_executor",
        [&](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IndexedExecutorNode>(
                name, &exec_runs, &per_idx_mutex, &per_idx_runs, &fail_on);
        });

    const std::string crash_tid = "step0-sends-crash";
    {
        auto store = open_store();
        auto engine = GraphEngine::compile(
            graph_json(), NodeContext{}, store);
        RunConfig cfg;
        cfg.thread_id = crash_tid;
        cfg.input = {{"seed", 1}};
        EXPECT_THROW((void)engine->run(cfg), std::runtime_error);
        EXPECT_EQ(planner_runs.load(), 1);
        EXPECT_EQ(exec_runs.load(), kFanout);

        const auto latest = store->load_latest(crash_tid);
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->interrupt_phase, CheckpointPhase::Before);
        EXPECT_EQ(latest->step, 0);
        EXPECT_EQ(store-> get_writes(crash_tid, latest->id).size(),
                  static_cast<size_t>(1 + (kFanout - 1)));
    }

    fail_on.store(-1, std::memory_order_relaxed);
    auto store = open_store();
    auto resumed_engine = GraphEngine::compile(
        graph_json(), NodeContext{}, store);
    const auto resumed = resumed_engine->resume(crash_tid);
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(planner_runs.load(), 1);
    EXPECT_EQ(exec_runs.load(), kFanout + 1);
    for (int i = 0; i < kFanout; ++i) {
        EXPECT_EQ(per_idx_runs[i], i == 2 ? 2 : 1) << "idx=" << i;
    }
    if (limit_planner_visits) {
        // The planner ran once, so the run's last checkpoint counts one visit,
        // not two: the replay of the step is not a second activation.
        const auto last = store->load_latest(crash_tid);
        ASSERT_TRUE(last.has_value());
        EXPECT_EQ(last->metadata.at("_neograph").at("node_visits").at("planner").get<int>(), 1);
    }

    // Compare against an uninterrupted run on the same graph.
    RunConfig clean_cfg;
    clean_cfg.thread_id = "step0-sends-clean";
    clean_cfg.input = {{"seed", 1}};
    const auto clean = resumed_engine->run(clean_cfg);
    EXPECT_FALSE(clean.interrupted);
    EXPECT_EQ(sorted_int_results(resumed), (std::multiset<int>{0, 10, 20, 30, 40}));
    EXPECT_EQ(sorted_int_results(resumed), sorted_int_results(clean));
    EXPECT_EQ(resumed.output.at("channels").at("seed").at("value"),
              clean.output.at("channels").at("seed").at("value"));

    verify_step_zero_anchor_and_clean_writes(
        *store, *resumed_engine, crash_tid, {"planner"});
}

void run_first_step_parallel_recovery_check(
    std::function<std::shared_ptr<CheckpointStore>()> open_store) {
    constexpr int kBranches = 4;
    std::atomic<int> exec_runs{0};
    std::atomic<int> fail_on{2};
    std::mutex per_idx_mutex;
    std::map<int, int> per_idx_runs;

    std::vector<std::string> expected_ready;
    for (int i = 0; i < kBranches; ++i) {
        const std::string name = "branch_" + std::to_string(i);
        expected_ready.push_back(name);
        NodeFactory::instance().register_type(name,
            [&, i](const std::string& node_name, const json&, const NodeContext&) {
                return std::make_unique<EntryBranchNode>(
                    node_name, i, &exec_runs, &per_idx_mutex, &per_idx_runs, &fail_on);
            });
    }

    const std::string crash_tid = "step0-parallel-crash";
    {
        auto store = open_store();
        auto engine = GraphEngine::compile(
            make_entry_parallel_graph(kBranches), NodeContext{}, store);
        RunConfig cfg;
        cfg.thread_id = crash_tid;
        EXPECT_THROW((void)engine->run(cfg), std::exception);
        EXPECT_EQ(exec_runs.load(), kBranches);

        const auto latest = store->load_latest(crash_tid);
        ASSERT_TRUE(latest.has_value());
        EXPECT_EQ(latest->interrupt_phase, CheckpointPhase::Before);
        EXPECT_EQ(latest->step, 0);
        EXPECT_EQ(store->get_writes(crash_tid, latest->id).size(),
                  static_cast<size_t>(kBranches - 1));
    }

    fail_on.store(-1, std::memory_order_relaxed);
    auto store = open_store();
    auto resumed_engine = GraphEngine::compile(
        make_entry_parallel_graph(kBranches), NodeContext{}, store);
    const auto resumed = resumed_engine->resume(crash_tid);
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(exec_runs.load(), kBranches + 1);
    for (int i = 0; i < kBranches; ++i) {
        EXPECT_EQ(per_idx_runs[i], i == 2 ? 2 : 1) << "branch=" << i;
    }

    RunConfig clean_cfg;
    clean_cfg.thread_id = "step0-parallel-clean";
    const auto clean = resumed_engine->run(clean_cfg);
    EXPECT_FALSE(clean.interrupted);
    EXPECT_EQ(sorted_int_results(resumed), (std::multiset<int>{0, 10, 20, 30}));
    EXPECT_EQ(sorted_int_results(resumed), sorted_int_results(clean));

    verify_step_zero_anchor_and_clean_writes(
        *store, *resumed_engine, crash_tid, expected_ready);
}

} // namespace

// ── 5. Sends at step 0: anchor checkpoint records planner + finished Sends ──

TEST_F(PendingWritesTest, FirstStepSendsReplayFinishedTasksAfterFailureInMemory) {
    auto shared_store = std::make_shared<InMemoryCheckpointStore>();
    run_first_step_sends_recovery_check([shared_store] { return shared_store; });
}

// The planner may run twice (max_visits 2). The anchor is saved after it ran,
// but it re-enters the step, so it holds the counters from before the step: the
// resumed step replays the planner and the run counts one visit, not two.
TEST_F(PendingWritesTest, FirstStepSendsAnchorKeepsVisitCountersFromBeforeTheStep) {
    auto shared_store = std::make_shared<InMemoryCheckpointStore>();
    run_first_step_sends_recovery_check([shared_store] { return shared_store; },
                                        /*limit_planner_visits=*/true);
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST_F(PendingWritesTest, FirstStepSendsReplayFinishedTasksAfterFailureSqlite) {
    TempSqliteFile db;
    run_first_step_sends_recovery_check([&db] {
        return std::make_shared<SqliteCheckpointStore>(db.path.string());
    });
}
#endif

// ── 6. Several ready nodes at step 0: anchor checkpoint records finished branches ──

TEST_F(PendingWritesTest, FirstStepParallelNodesReplayFinishedBranchesAfterFailureInMemory) {
    auto shared_store = std::make_shared<InMemoryCheckpointStore>();
    run_first_step_parallel_recovery_check([shared_store] { return shared_store; });
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST_F(PendingWritesTest, FirstStepParallelNodesReplayFinishedBranchesAfterFailureSqlite) {
    TempSqliteFile db;
    run_first_step_parallel_recovery_check([&db] {
        return std::make_shared<SqliteCheckpointStore>(db.path.string());
    });
}
#endif

// ── 7. Single-task first step writes no anchor checkpoint and no pending write ──

TEST_F(PendingWritesTest, SingleTaskFirstStepWritesNoAnchorOrPendingWrites) {
    auto inner = std::make_shared<InMemoryCheckpointStore>();
    auto counting = std::make_shared<CountingCheckpointStore>(inner);

    const json single_step_graph = {
        {"name", "single_task_first_step"},
        {"channels", {
            {"setup_done", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"setup", {{"type", "setup"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "setup"}},
            {{"from", "setup"},     {"to", "__end__"}}
        }}
    };

    auto engine = GraphEngine::compile(single_step_graph, NodeContext{}, counting);
    RunConfig cfg;
    cfg.thread_id = "single-step-thread";
    const auto result = engine->run(cfg);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(counting->save_count.load(), 1u);
    EXPECT_EQ(counting->before_save_count.load(), 0u);
    EXPECT_EQ(counting->put_writes_count.load(), 0u);

    const auto cps = inner->list("single-step-thread");
    ASSERT_EQ(cps.size(), 1u);
    EXPECT_EQ(cps[0].interrupt_phase, CheckpointPhase::Completed);
    EXPECT_TRUE(cps[0].parent_id.empty());
}

// ── 8. Gated node in a multi-node first step pauses before writing any anchor ──

TEST_F(PendingWritesTest, GatedMultiNodeFirstStepWritesOnlyGateCheckpointBeforePause) {
    constexpr int kBranches = 2;
    std::atomic<int> exec_runs{0};
    std::atomic<int> fail_on{-1};
    std::mutex per_idx_mutex;
    std::map<int, int> per_idx_runs;

    for (int i = 0; i < kBranches; ++i) {
        const std::string name = "branch_" + std::to_string(i);
        NodeFactory::instance().register_type(name,
            [&, i](const std::string& node_name, const json&, const NodeContext&) {
                return std::make_unique<EntryBranchNode>(
                    node_name, i, &exec_runs, &per_idx_mutex, &per_idx_runs, &fail_on);
            });
    }

    auto inner = std::make_shared<InMemoryCheckpointStore>();
    auto counting = std::make_shared<CountingCheckpointStore>(inner);
    auto engine = GraphEngine::compile(
        make_entry_parallel_graph(kBranches, {"branch_1"}), NodeContext{}, counting);

    RunConfig cfg;
    cfg.thread_id = "gated-first-step";
    const auto paused = engine->run(cfg);
    ASSERT_TRUE(paused.interrupted);
    EXPECT_EQ(paused.interrupt_node, "branch_1");
    EXPECT_EQ(exec_runs.load(), 0);
    EXPECT_EQ(counting->save_count.load(), 1u);
    EXPECT_EQ(counting->before_save_count.load(), 1u);
    EXPECT_EQ(counting->put_writes_count.load(), 0u);

    const auto paused_cps = inner->list("gated-first-step");
    ASSERT_EQ(paused_cps.size(), 1u);
    EXPECT_EQ(paused_cps[0].id, paused.checkpoint_id);
    EXPECT_EQ(paused_cps[0].current_node, "branch_1");
    EXPECT_EQ(paused_cps[0].interrupt_phase, CheckpointPhase::Before);
    EXPECT_TRUE(paused_cps[0].parent_id.empty());

    const auto resumed = engine->resume("gated-first-step");
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(exec_runs.load(), kBranches);
    EXPECT_EQ(sorted_int_results(resumed), (std::multiset<int>{0, 10}));
    // Gate Before checkpoint already serves as the step's parent on resume,
    // so no extra anchor checkpoint is written.
    EXPECT_EQ(counting->save_count.load(), 2u);
    EXPECT_EQ(counting->before_save_count.load(), 1u);
}

// ── 9. SubgraphNode PerInvocation: step-0 Send fan-out to PerInvocation child ──

TEST_F(PendingWritesTest, FirstStepSendFanOutToPerInvocationSubgraphReplaysFinishedChildren) {
    constexpr int kFanout = 4;
    std::atomic<int> planner_runs{0};
    std::atomic<int> child_runs{0};
    std::atomic<int> fail_on{2};
    std::mutex per_idx_mutex;
    std::map<int, int> per_idx_runs;

    NodeFactory::instance().register_type("sub_planner",
        [&](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>(kFanout, &planner_runs);
        });
    NodeFactory::instance().register_type("sub_child_worker",
        [&](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IndexedExecutorNode>(
                name, &child_runs, &per_idx_mutex, &per_idx_runs, &fail_on);
        });

    const json child_def = {
        {"name", "per_invocation_child"},
        {"channels", {
            {"task_idx", {{"reducer", "overwrite"}}},
            {"results",  {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"worker", {{"type", "sub_child_worker"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "worker"}},
            {{"from", "worker"},    {"to", "__end__"}}
        }}
    };
    auto child_engine = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(child_def, NodeContext{}).release());

    NodeFactory::instance().register_type("per_invocation_subgraph_executor",
        [child_engine](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child_engine,
                std::map<std::string, std::string>{{"task_idx", "task_idx"}},
                std::map<std::string, std::string>{{"results", "results"}},
                SubgraphPersistence::PerInvocation);
        });

    const json root_def = {
        {"name", "root_with_per_invocation_subgraph"},
        {"channels", {
            {"task_idx", {{"reducer", "overwrite"}}},
            {"results",  {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner",  {{"type", "sub_planner"}}},
            {"executor", {{"type", "per_invocation_subgraph_executor"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "__end__"}}
        }}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    const std::string tid = "step0-per-invocation-subgraph";
    {
        auto engine = GraphEngine::compile(root_def, NodeContext{}, store);
        RunConfig cfg;
        cfg.thread_id = tid;
        EXPECT_THROW((void)engine->run(cfg), std::exception);
        EXPECT_EQ(planner_runs.load(), 1);
        EXPECT_EQ(child_runs.load(), kFanout);
    }

    fail_on.store(-1, std::memory_order_relaxed);
    auto resumed_engine = GraphEngine::compile(root_def, NodeContext{}, store);
    const auto resumed = resumed_engine->resume(tid);
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(planner_runs.load(), 1);
    EXPECT_EQ(child_runs.load(), kFanout + 1);
    for (int i = 0; i < kFanout; ++i) {
        EXPECT_EQ(per_idx_runs[i], i == 2 ? 2 : 1) << "idx=" << i;
    }
    EXPECT_EQ(sorted_int_results(resumed), (std::multiset<int>{0, 10, 20, 30}));
    verify_step_zero_anchor_and_clean_writes(*store, *resumed_engine, tid, {"planner"});
}
