// Effect-aware node retry (#364, WP9), through the real SQLiteToolEffectBroker.
//
// Every attempt of a node gets its own effect namespace: attempt 0 keeps the
// task id ("s1:act") byte for byte, attempt k >= 1 uses "s1:act#a<k>", and the
// broker's call ordinals start again inside it. Before attempt k >= 1 starts, a
// pending write keyed by that namespace records that the attempt began, so a
// resume continues the attempt that was in flight and the broker replays what
// it already committed; a policy retry gets fresh slots.
//
// Before this, one invocation scope wrapped the whole retry loop: every attempt
// reused the first attempt's task id and ordinals, so the broker replayed the
// first attempt's receipt or rejected the slot as changed.
//
// An attempt that committed a non-idempotent effect is never retried (it fails
// as NG.EffectCommitted), and an uncertain effect pauses the run for
// reconciliation instead of being retried.

#include "fixtures/unique_temp_dir.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/sqlite_runtime_stores.h>
#include <neograph/tool_dispatch.h>
#include "fixtures/tool_effect_ledger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace neograph;
using namespace neograph::test::tool_effects;
namespace fs = std::filesystem;

class TestFiles {
public:
    std::string journal() const { return dir_.file("journal.sqlite"); }
    std::string ledger() const { return dir_.file("ledger.sqlite"); }
    std::string checkpoints() const { return dir_.file("checkpoints.sqlite"); }

private:
    neograph::graph::test_support::UniqueTempDir dir_{"ng-node-failure-effects-"};
};

// What the node saw, kept across "process restarts" of one test.
struct Probe {
    std::mutex mutex;
    int calls = 0;
    std::vector<std::string> namespaces;
    std::vector<int> steps;
    std::vector<std::string> statuses;
    Tool* tool = nullptr;
};

std::map<std::string, std::shared_ptr<Probe>>& probes() {
    static std::map<std::string, std::shared_ptr<Probe>> instance;
    return instance;
}

std::shared_ptr<Probe> probe(const std::string& name) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto& slot = probes()[name];
    if (!slot) slot = std::make_shared<Probe>();
    return slot;
}

// Calls the ledger tool once through mediated dispatch, then fails or succeeds
// as configured by the 0-based index of the call.
//   fail:     calls that throw an ordinary (retryable) error after the tool
//   crash:    a call that throws a non-retryable error after the tool, which
//             ends the run like a crash: nothing after it is recorded
//   vary:     the tool arguments change with every call
//   lose:     calls whose tool effect commits but whose response is lost
//   swallow:  the node turns the reconciliation interrupt into an ordinary error
class EffectNode final : public graph::GraphNode {
public:
    explicit EffectNode(json config) : config_(std::move(config)) {}

    asio::awaitable<graph::NodeResult> run(graph::NodeInput in) override {
        auto seen = probe(config_.at("probe").get<std::string>());
        int call = 0;
        auto execution = graph::make_tool_execution_context(in.ctx);
        {
            std::lock_guard lock(seen->mutex);
            call = seen->calls++;
            seen->namespaces.push_back(execution.effect_task_id);
            seen->steps.push_back(in.ctx.step);
        }
        json arguments{{"who", in.ctx.thread_id}, {"n", 0}};
        if (config_.value("vary", false)) arguments["try"] = call;
        if (listed("lose", call)) arguments["lose"] = true;
        std::vector<ToolCall> calls{ToolCall{"model-call-0", "ledger", arguments.dump()}};
        std::vector<Tool*> tools{seen->tool};

        std::vector<ChatMessage> messages;
        std::optional<std::string> swallowed;
        try {
            messages = co_await dispatch_tool_calls(std::move(calls), std::move(tools), {}, {},
                                                    std::move(execution));
        } catch (const graph::NodeInterrupt& interrupt) {
            if (!config_.value("swallow", false)) throw;
            swallowed = interrupt.reason();
        }
        if (swallowed) throw std::runtime_error("node ignored: " + *swallowed);
        {
            std::lock_guard lock(seen->mutex);
            for (const auto& message : messages) seen->statuses.push_back(message.tool_status);
        }
        if (listed("fail", call)) throw std::runtime_error("ordinary failure after the effect");
        if (config_.value("crash", -1) == call) throw std::logic_error("process lost here");
        graph::NodeResult result;
        result.writes.push_back(graph::ChannelWrite{"result", json("act")});
        co_return result;
    }
    std::string get_name() const override { return "act"; }

private:
    bool listed(const char* key, int call) const {
        if (!config_.contains(key)) return false;
        for (const auto& value : config_.at(key))
            if (value.get<int>() == call) return true;
        return false;
    }
    json config_;
};

class PrepNode final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput) override {
        graph::NodeResult result;
        result.writes.push_back(graph::ChannelWrite{"result", json("prep")});
        co_return result;
    }
    std::string get_name() const override { return "prep"; }
};

class BoomNode final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput) override {
        throw std::runtime_error("a later child node fails");
        co_return graph::NodeResult{};
    }
    std::string get_name() const override { return "boom"; }
};

void register_nodes() {
    static const bool registered = [] {
        graph::NodeFactory::instance().register_type(
            "node_failure_boom", [](const std::string&, const json&, const graph::NodeContext&) {
                return std::make_unique<BoomNode>();
            });
        graph::NodeFactory::instance().register_type(
            "node_failure_effect", [](const std::string&, const json& config, const graph::NodeContext&) {
                return std::make_unique<EffectNode>(config);
            });
        graph::NodeFactory::instance().register_type(
            "node_failure_prep", [](const std::string&, const json&, const graph::NodeContext&) {
                return std::make_unique<PrepNode>();
            });
        return true;
    }();
    (void)registered;
}

struct Policy {
    ToolEffectClass effect = ToolEffectClass::ReadOnly;
    ToolIdempotency idempotency = ToolIdempotency::Unknown;
};

// One "process": a Tool, a broker connection, a checkpoint store connection and
// a freshly compiled engine over durable files.
struct Process {
    // With `in_subgraph`, "act" is a SubgraphNode whose child graph runs the
    // effect node and then a node that always fails.
    Process(const TestFiles& files, json act, Policy policy, int max_retries = 2,
            bool in_subgraph = false) {
        register_nodes();
        tool = std::make_shared<LedgerTool>(files.ledger(), nullptr);
        probe(act.at("probe").get<std::string>())->tool = tool.get();
        broker = std::make_shared<SQLiteToolEffectBroker>(
            files.journal(), std::vector<SQLiteToolExecutableBinding>{{tool.get(), kExecutable}});
        store = std::make_shared<graph::SqliteCheckpointStore>(files.checkpoints());
        act["type"] = "node_failure_effect";
        if (in_subgraph) {
            const json child_definition{
                {"name", "node_failure_child"},
                {"channels", {{"result", {{"reducer", "overwrite"}}}}},
                {"nodes", {{"act", act}, {"boom", {{"type", "node_failure_boom"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "act"}},
                                       json{{"from", "act"}, {"to", "boom"}},
                                       json{{"from", "boom"}, {"to", "__end__"}}})}};
            child = std::shared_ptr<graph::GraphEngine>(
                graph::GraphEngine::compile(child_definition, graph::NodeContext{}).release());
            static std::atomic<int> next_type{0};
            const auto type = "node_failure_subgraph_" + std::to_string(++next_type);
            graph::NodeFactory::instance().register_type(
                type, [engine = child](const std::string& name, const json&, const graph::NodeContext&) {
                    return std::make_unique<graph::SubgraphNode>(name, engine);
                });
            act = json{{"type", type}};
        }
        const json definition{
            {"name", "node_failure_effects"},
            {"channels", {{"result", {{"reducer", "overwrite"}}}}},
            {"nodes", {{"prep", {{"type", "node_failure_prep"}}}, {"act", act}}},
            {"edges", json::array({json{{"from", "__start__"}, {"to", "prep"}},
                                   json{{"from", "prep"}, {"to", "act"}},
                                   json{{"from", "act"}, {"to", "__end__"}}})}};
        engine = graph::GraphEngine::compile(definition, graph::NodeContext{}, store);
        auto controller = std::make_shared<ToolExecutionController>();
        ToolExecutionPolicy tool_policy;
        tool_policy.effect = policy.effect;
        tool_policy.idempotency = policy.idempotency;
        controller->policies()->upsert("ledger", tool_policy);
        engine->set_tool_execution_controller(controller);
        graph::RetryPolicy retry;
        retry.max_retries = max_retries;
        retry.initial_delay_ms = 1;
        retry.backoff_multiplier = 1.0f;
        retry.max_delay_ms = 1;
        engine->set_retry_policy(retry);
    }

    graph::RunConfig config() const {
        graph::RunConfig config;
        config.thread_id = "thread-node-failure";
        return config;
    }
    graph::RunMetadata metadata() const {
        graph::RunMetadata metadata;
        metadata.owner_scope = "tenant";
        metadata.run_id = "run-node-failure";
        return metadata;
    }
    graph::RunResources resources() const {
        graph::RunResources resources;
        resources.tool_effect_broker = broker;
        resources.tool_effect_grant = {"", "node-failure-operation", "host-grant", 1};
        return resources;
    }

    graph::RunResult run() const {
        return async::run_sync(engine->run_async(config(), metadata(), resources()));
    }
    graph::RunResult resume() const {
        return async::run_sync(engine->resume_async(config(), json(), nullptr, metadata(),
                                                    resources()));
    }

    std::shared_ptr<LedgerTool> tool;
    std::shared_ptr<SQLiteToolEffectBroker> broker;
    std::shared_ptr<graph::CheckpointStore> store;
    std::shared_ptr<graph::GraphEngine> child;
    std::shared_ptr<graph::GraphEngine> engine;
};

std::string unique_probe(const char* name) {
    static std::atomic<int> next{0};
    return std::string(name) + "-" + std::to_string(++next);
}

std::set<std::string> journal_tasks(const std::string& journal) {
    std::set<std::string> tasks;
    for (const auto& row : effect_rows(journal)) {
        EXPECT_EQ(row.ordinal, 0) << row.task;  // ordinals restart in every attempt
        EXPECT_TRUE(row.has_receipt) << row.task;
        tasks.insert(row.task);
    }
    return tasks;
}

std::exception_ptr run_capturing(const Process& process) {
    try {
        (void)process.run();
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

}  // namespace

TEST(NodeFailureEffects, ARetriedReadOnlyToolGetsFreshSlotsInEveryAttempt) {
    const TestFiles files;
    const auto name = unique_probe("fresh");
    Process process(files, json{{"probe", name}, {"fail", {0, 1}}, {"vary", true}}, Policy{});
    const auto result = process.run();
    const auto seen = probe(name);

    EXPECT_EQ(seen->calls, 3);
    ASSERT_EQ(seen->namespaces.size(), 3U);
    const auto task = "s" + std::to_string(seen->steps[0]) + ":act";
    // Attempt 0 keeps today's id; later attempts get their own namespace.
    EXPECT_EQ(seen->namespaces,
              (std::vector<std::string>{task, task + "#a1", task + "#a2"}));
    // No attempt collided with an earlier one: each executed its own effect.
    EXPECT_EQ(seen->statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(LedgerTool::count(files.ledger()), 3U);
    EXPECT_EQ(journal_tasks(files.journal()),
              (std::set<std::string>{task, task + "#a1", task + "#a2"}));
    EXPECT_EQ(result.output["channels"]["result"]["value"], "act");
}

TEST(NodeFailureEffects, ARetryDoesNotReplayAnEarlierAttemptsReceipt) {
    const TestFiles files;
    const auto name = unique_probe("no-replay");
    Process process(files, json{{"probe", name}, {"fail", {0}}}, Policy{});
    (void)process.run();
    // Same arguments in both attempts: the retry is a new read, not a replay.
    EXPECT_EQ(probe(name)->calls, 2);
    EXPECT_EQ(process.tool->executions.load(), 2);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
}

TEST(NodeFailureEffects, AnAttemptThatCommittedAWriteIsNotRetried) {
    for (const auto& [label, policy] : std::vector<std::pair<std::string, Policy>>{
             {"external write", {ToolEffectClass::ExternalWrite, ToolIdempotency::NonIdempotent}},
             {"workspace write", {ToolEffectClass::WorkspaceWrite, ToolIdempotency::Unknown}},
             {"unknown effect", {ToolEffectClass::Unknown, ToolIdempotency::Unknown}}}) {
        SCOPED_TRACE(label);
        const TestFiles files;
        const auto name = unique_probe("committed");
        Process process(files, json{{"probe", name}, {"fail", {0, 1, 2}}}, policy);
        const auto error = run_capturing(process);
        ASSERT_TRUE(error);
        EXPECT_EQ(probe(name)->calls, 1);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
        const auto record = graph::node_failure_of(error);
        ASSERT_TRUE(record);
        EXPECT_EQ(record->node, "act");
        EXPECT_EQ(record->code, "NG.EffectCommitted");
        EXPECT_EQ(record->failure_class, graph::NodeFailureClass::EffectCommitted);
        EXPECT_EQ(record->kind, graph::NodeFailureKind::EffectCommitted);
        EXPECT_FALSE(record->retryable);
        // Never retried, but routable: a failure route can run compensation,
        // and the record names the effects it has to compensate.
        EXPECT_TRUE(record->routable);
        EXPECT_EQ(record->attempts, 1);
        EXPECT_EQ(record->effects,
                  (std::vector<graph::NodeFailureEffect>{{"ledger", 0, "succeeded"}}));
    }
}

TEST(NodeFailureEffects, AnIdempotentWriteIsRetriedWithFreshSlots) {
    const TestFiles files;
    const auto name = unique_probe("idempotent");
    Process process(files, json{{"probe", name}, {"fail", {0, 1}}},
                    Policy{ToolEffectClass::ExternalWrite, ToolIdempotency::Idempotent});
    (void)process.run();
    EXPECT_EQ(probe(name)->calls, 3);
    EXPECT_EQ(journal_tasks(files.journal()).size(), 3U);
}

TEST(NodeFailureEffects, AnExhaustedReadOnlyNodeReportsItsFinalAttemptsEffects) {
    const TestFiles files;
    const auto name = unique_probe("exhausted");
    Process process(files, json{{"probe", name}, {"fail", {0, 1, 2}}}, Policy{});
    const auto record = graph::node_failure_of(run_capturing(process));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.TaskFailed");
    EXPECT_EQ(record->kind, graph::NodeFailureKind::Exhausted);
    EXPECT_TRUE(record->routable);
    EXPECT_EQ(record->attempts, 3);
    EXPECT_EQ(record->effects,
              (std::vector<graph::NodeFailureEffect>{{"ledger", 0, "succeeded"}}));
}

TEST(NodeFailureEffects, ACrashDuringASecondAttemptResumesThatAttempt) {
    const TestFiles files;
    const auto name = unique_probe("crash");
    {
        // Attempt 0 fails ordinarily; attempt 1 commits its read and is lost.
        Process first(files, json{{"probe", name}, {"fail", {0}}, {"crash", 1}}, Policy{});
        EXPECT_TRUE(run_capturing(first));
    }
    const auto seen = probe(name);
    ASSERT_EQ(seen->namespaces.size(), 2U);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);

    // A new process resumes the run: the node continues attempt 1 in attempt
    // 1's namespace, so the broker replays its committed effect.
    Process second(files, json{{"probe", name}}, Policy{});
    const auto result = second.resume();
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(result.output["channels"]["result"]["value"], "act");
    const auto task = "s" + std::to_string(seen->steps[0]) + ":act";
    EXPECT_EQ(seen->namespaces, (std::vector<std::string>{task, task + "#a1", task + "#a1"}));
    EXPECT_EQ(second.tool->executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
    EXPECT_EQ(seen->statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(journal_tasks(files.journal()), (std::set<std::string>{task, task + "#a1"}));
}

TEST(NodeFailureEffects, AResumedAttemptKeepsTheRemainingRetryBudget) {
    const TestFiles files;
    const auto name = unique_probe("budget");
    {
        Process first(files, json{{"probe", name}, {"fail", {0}}, {"crash", 1}}, Policy{});
        EXPECT_TRUE(run_capturing(first));
    }
    // Resumed in attempt 1 of 0..2: one more retry is left, not two.
    Process second(files, json{{"probe", name}, {"fail", {2, 3, 4}}}, Policy{});
    std::exception_ptr error;
    try {
        (void)second.resume();
    } catch (...) {
        error = std::current_exception();
    }
    const auto record = graph::node_failure_of(error);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->attempts, 3);
    EXPECT_EQ(probe(name)->calls, 4);
    const auto task = "s" + std::to_string(probe(name)->steps[0]) + ":act";
    EXPECT_EQ(probe(name)->namespaces.back(), task + "#a2");
}

TEST(NodeFailureEffects, AnUncertainEffectPausesForReconciliationInsteadOfRetrying) {
    for (const bool swallow : {false, true}) {
        SCOPED_TRACE(swallow ? "node swallowed the interrupt" : "node let it through");
        const TestFiles files;
        const auto name = unique_probe("uncertain");
        Process process(files, json{{"probe", name}, {"lose", {0}}, {"swallow", swallow}},
                        Policy{ToolEffectClass::ExternalWrite, ToolIdempotency::NonIdempotent});
        const auto result = process.run();
        EXPECT_TRUE(result.interrupted);
        EXPECT_EQ(result.interrupt_node, "act");
        EXPECT_EQ(result.interrupt_value["value"]["kind"], "tool_effect_reconciliation");
        EXPECT_EQ(probe(name)->calls, 1);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    }
}

// A subgraph's effects happen inside the enclosing node's attempt: a child that
// committed a write keeps the parent from retrying, or the retry would run the
// child again in fresh slots and repeat the write.
TEST(NodeFailureEffects, ACommittedWriteInsideASubgraphStopsTheParentsRetry) {
    const TestFiles files;
    const auto name = unique_probe("subgraph");
    Process process(files, json{{"probe", name}},
                    Policy{ToolEffectClass::ExternalWrite, ToolIdempotency::NonIdempotent}, 2,
                    true);
    const auto error = run_capturing(process);
    ASSERT_TRUE(error);
    EXPECT_EQ(probe(name)->calls, 1);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    const auto record = graph::node_failure_of(error);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->node, "act");
    EXPECT_EQ(record->code, "NG.EffectCommitted");
    EXPECT_EQ(record->kind, graph::NodeFailureKind::EffectCommitted);
    EXPECT_TRUE(record->routable);
    EXPECT_EQ(record->effects,
              (std::vector<graph::NodeFailureEffect>{{"ledger", 0, "succeeded"}}));
    // The innermost dispatch context is kept: the child node that failed.
    try {
        std::rethrow_exception(error);
    } catch (const graph::NodeExecutionError& failure) {
        EXPECT_EQ(failure.node_name(), "boom");
    }
}

// A read-only child effect does not stop the parent's retry; the retry runs the
// child in fresh slots.
TEST(NodeFailureEffects, AReadInsideASubgraphLetsTheParentRetry) {
    const TestFiles files;
    const auto name = unique_probe("subgraph-read");
    Process process(files, json{{"probe", name}}, Policy{}, 2, true);
    const auto record = graph::node_failure_of(run_capturing(process));
    ASSERT_TRUE(record);
    EXPECT_EQ(record->code, "NG.SubgraphFailed");
    EXPECT_EQ(record->attempts, 3);
    EXPECT_EQ(probe(name)->calls, 3);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 3U);
    EXPECT_EQ(effect_rows(files.journal()).size(), 3U);
}
