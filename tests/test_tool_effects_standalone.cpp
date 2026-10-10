// Durable mediated Tool effects through the standalone Core engine and the
// direct llm::Agent entry points (the Program composition lives in
// test_program_tool_effects_runtime.cpp).
//
// Both entry points run multi-call assistant batches against the real
// SQLiteToolEffectBroker. Portable histories require distinct unresolved call
// ids; Program's direct dispatcher also covers duplicate model call ids.
// The external ledger is
// counted independently of the broker's receipts, and every "restart" rebuilds
// the Tool instance, the broker connection and the engine/Agent.

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/sqlite_runtime_stores.h>
#include <neograph/tool_dispatch.h>
#ifdef NEOGRAPH_TOOL_EFFECT_TESTS_HAVE_LLM
#include <neograph/llm/agent.h>
#include "fixtures/typed_provider.h"
#endif
#include "fixtures/tool_effect_ledger.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace neograph;
using namespace neograph::test::tool_effects;
namespace fs = std::filesystem;

class TestFiles {
public:
    TestFiles() {
        static std::atomic<std::uint64_t> next{0};
        path_ = fs::canonical(fs::temp_directory_path()) /
                ("ng-standalone-tool-effects-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(++next));
        if (!fs::create_directory(path_)) throw std::runtime_error("cannot reserve test directory");
    }
    ~TestFiles() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TestFiles(const TestFiles&) = delete;
    TestFiles& operator=(const TestFiles&) = delete;
    std::string journal() const { return (path_ / "journal.sqlite").string(); }
    std::string ledger() const { return (path_ / "ledger.sqlite").string(); }

private:
    fs::path path_;
};

std::shared_ptr<ToolExecutionController> widened_controller() {
    auto controller = std::make_shared<ToolExecutionController>();
    ToolExecutionPolicy widened;
    widened.concurrency = ToolConcurrency::Capacity;
    widened.capacity = 16;
    widened.effect = ToolEffectClass::ExternalWrite;
    controller->policies()->upsert("ledger", widened);
    return controller;
}

// `flags` merges extra arguments into the call at that batch position.
std::vector<json> batch_arguments(const std::string& who, int size,
                                  const std::map<int, json>& flags = {}) {
    std::vector<json> arguments;
    for (int n = 0; n < size; ++n) {
        json call{{"who", who}, {"n", n}};
        if (const auto found = flags.find(n); found != flags.end())
            for (const auto& [key, value] : found->second.items()) call[key] = value;
        arguments.push_back(std::move(call));
    }
    return arguments;
}

bool denies(const std::string& arguments) {
    return arguments.find("\"deny\":true") != std::string::npos;
}

void expect_same_rows(const std::vector<EffectRow>& before, const std::vector<EffectRow>& after) {
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].slot(), after[i].slot());
        EXPECT_EQ(before[i].version, after[i].version);
        EXPECT_EQ(before[i].operation, after[i].operation);
        EXPECT_EQ(before[i].fingerprint, after[i].fingerprint);
        EXPECT_EQ(before[i].tool_name, after[i].tool_name);
        EXPECT_EQ(before[i].grant, after[i].grant);
        EXPECT_EQ(before[i].executable, after[i].executable);
        EXPECT_EQ(before[i].arguments, after[i].arguments);
        EXPECT_EQ(before[i].first_attempt, after[i].first_attempt);
        EXPECT_EQ(before[i].has_receipt, after[i].has_receipt);
        EXPECT_EQ(before[i].receipt, after[i].receipt);
    }
}

// ── Standalone Core ──────────────────────────────────────────────────────

// Emits one assistant message whose Tool calls come from the node config; each
// call is tagged with the run's thread id so concurrent runs stay distinguishable.
class AssistantBatchNode final : public graph::GraphNode {
public:
    explicit AssistantBatchNode(json config) : config_(std::move(config)) {}
    asio::awaitable<graph::NodeResult> run(graph::NodeInput input) override {
        json calls = json::array();
        for (const auto& template_call : config_.at("calls")) {
            json arguments = template_call;
            arguments["who"] = input.ctx.thread_id;
            if (arguments.value("rendezvous", false)) arguments["rendezvous"] = input.ctx.thread_id;
            calls.push_back(json{{"id", "model-call-" + std::to_string(calls.size())},
                                 {"name", "ledger"},
                                 {"arguments", arguments.dump()}});
        }
        graph::NodeResult result;
        result.writes.push_back(graph::ChannelWrite{
            "messages", json::array({json{{"role", "assistant"}, {"content", ""},
                                          {"tool_calls", std::move(calls)}}})});
        co_return result;
    }
    std::string get_name() const override { return "assistant-batch"; }

private:
    json config_;
};

void register_batch_node() {
    static const bool registered = [] {
        graph::NodeFactory::instance().register_type(
            "tool_effect_assistant_batch",
            [](const std::string&, const json& config, const graph::NodeContext&) {
                return std::make_unique<AssistantBatchNode>(config);
            });
        return true;
    }();
    (void)registered;
}

struct CoreOptions {
    std::string executable = kExecutable;
    bool bind_foreign_tool = false;
    std::vector<json> calls;
    std::shared_ptr<ToolExecutionController> controller;
    ToolGate gate;
};

// One "process": a Tool, a broker connection and a freshly compiled engine.
struct CoreProcess {
    CoreProcess(const TestFiles& files, const CoreOptions& options,
                std::shared_ptr<Rendezvous> rendezvous = {},
                std::shared_ptr<DispatchLatch> latch = {}) {
        register_batch_node();
        tool = std::make_shared<LedgerTool>(files.ledger(), rendezvous, std::move(latch));
        foreign = std::make_shared<LedgerTool>(files.ledger(), rendezvous);
        broker = std::make_shared<SQLiteToolEffectBroker>(
            files.journal(),
            std::vector<SQLiteToolExecutableBinding>{
                {options.bind_foreign_tool ? foreign.get() : tool.get(), options.executable}});
        json calls = json::array();
        for (const auto& call : options.calls) calls.push_back(call);
        const json definition{
            {"name", "standalone_tool_effect_graph"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"source", {{"type", "tool_effect_assistant_batch"},
                                   {"calls", std::move(calls)}}},
                       {"tools", {{"type", "tool_dispatch"}}}}},
            {"edges", json::array({json{{"from", "__start__"}, {"to", "source"}},
                                   json{{"from", "source"}, {"to", "tools"}},
                                   json{{"from", "tools"}, {"to", "__end__"}}})}};
        graph::NodeContext nodes;
        nodes.tools = ToolSet(std::vector<std::shared_ptr<Tool>>{tool});
        engine = graph::GraphEngine::compile(definition, nodes);
        if (options.controller) engine->set_tool_execution_controller(options.controller);
        if (options.gate) engine->set_tool_gate(options.gate);
    }

    graph::RunResult run(const std::string& run_id, std::uint64_t attempt,
                         std::string grant_id = "host-grant") const {
        graph::RunConfig config;
        config.thread_id = "thread-" + run_id;
        graph::RunMetadata metadata;
        metadata.owner_scope = "tenant";
        metadata.run_id = run_id;
        graph::RunResources resources;
        resources.tool_effect_broker = broker;
        resources.tool_effect_grant = {"", "standalone-operation", std::move(grant_id), attempt};
        return async::run_sync(engine->run_async(config, metadata, resources));
    }

    std::shared_ptr<LedgerTool> tool, foreign;
    std::shared_ptr<SQLiteToolEffectBroker> broker;
    std::shared_ptr<graph::GraphEngine> engine;
};

std::vector<json> tool_messages(const graph::RunResult& result) {
    std::vector<json> out;
    for (const auto& message : result.output.at("channels").at("messages").at("value"))
        if (message.value("role", "") == "tool") out.push_back(message);
    return out;
}

std::vector<std::string> statuses(const graph::RunResult& result) {
    std::vector<std::string> out;
    for (const auto& message : tool_messages(result)) out.push_back(message.value("tool_status", ""));
    return out;
}

std::vector<std::string> contents(const graph::RunResult& result) {
    std::vector<std::string> out;
    for (const auto& message : tool_messages(result)) out.push_back(message.value("content", ""));
    return out;
}

}  // namespace

TEST(StandaloneCoreToolEffects, GateDenialDispatchesNothingAndKeepsLaterSlotsStable) {
    const TestFiles files;
    CoreOptions options;
    options.calls = batch_arguments("core", 2, {{0, json{{"deny", true}}}});
    options.gate = [](ToolCall call, ToolGateContext) -> asio::awaitable<ToolDecision> {
        if (denies(call.arguments)) co_return ToolDecision::deny("host policy denies this call");
        co_return ToolDecision::allow();
    };
    CoreProcess process(files, options);
    const auto result = process.run("core-deny", 1);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(statuses(result), (std::vector<std::string>{"rejected", "succeeded"}));
    EXPECT_EQ(process.tool->executions.load(), 1);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    const auto rows = effect_rows(files.journal());
    ASSERT_EQ(rows.size(), 1U);  // the denied call never reserved a slot
    EXPECT_EQ(rows[0].ordinal, 1);
}

TEST(StandaloneCoreToolEffects, ConcurrentRunsOnOneEngineAndBrokerGetDistinctSlots) {
    constexpr int kRuns = 4;
    constexpr int kBatch = 3;
    const TestFiles files;
    CoreOptions options;
    options.controller = widened_controller();
    options.calls = batch_arguments("template", kBatch, {{0, json{{"rendezvous", true}}}});
    auto rendezvous = std::make_shared<Rendezvous>(2);
    CoreProcess process(files, options, rendezvous);

    std::vector<graph::RunResult> results(kRuns);
    std::vector<std::exception_ptr> errors(kRuns);
    std::vector<std::thread> threads;
    for (int r = 0; r < kRuns; ++r)
        threads.emplace_back([&, r] {
            try { results[r] = process.run("core-run-" + std::to_string(r), 1); }
            catch (...) { errors[r] = std::current_exception(); }
        });
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);

    EXPECT_TRUE(rendezvous->overlapped);
    for (const auto& result : results) {
        EXPECT_FALSE(result.interrupted);
        EXPECT_EQ(statuses(result), (std::vector<std::string>(kBatch, "succeeded")));
    }
    EXPECT_EQ(LedgerTool::count(files.ledger()), static_cast<std::size_t>(kRuns * kBatch));
    const auto rows = effect_rows(files.journal());
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kRuns * kBatch));
    std::map<std::string, std::set<std::int64_t>> ordinals;
    std::set<std::string> threads_seen;
    for (const auto& row : rows) {
        ordinals[row.run].insert(row.ordinal);
        threads_seen.insert(row.thread);
        EXPECT_TRUE(row.has_receipt);
        // The slot is the position in the batch: ordinal n carries argument n.
        EXPECT_EQ(json::parse(row.arguments).at("n"), row.ordinal);
        EXPECT_EQ(json::parse(row.arguments).at("who"), row.thread);
    }
    EXPECT_EQ(ordinals.size(), static_cast<std::size_t>(kRuns));
    EXPECT_EQ(threads_seen.size(), static_cast<std::size_t>(kRuns));
    for (const auto& [run, seen] : ordinals)
        EXPECT_EQ(seen, (std::set<std::int64_t>{0, 1, 2})) << run;
}

namespace {
struct CoreRestart {
    std::vector<std::string> first_statuses, statuses, first_contents, contents;
    bool interrupted = false;
    std::size_t ledger_before = 0, ledger_after = 0;
    int executions = 0;
    std::vector<EffectRow> rows_before, rows_after;
};

// Attempt 1 commits `initial` calls; a process restart then reruns the same
// logical run as attempt 2 with the options/grant given.
CoreRestart core_restart(int initial, CoreOptions restarted, std::string grant_id = "host-grant") {
    const TestFiles files;
    CoreOptions first;
    first.calls = batch_arguments("template", initial);
    CoreRestart outcome;
    {
        CoreProcess process(files, first);
        const auto result = process.run("core-restart", 1);
        EXPECT_FALSE(result.interrupted);
        outcome.first_statuses = statuses(result);
        outcome.first_contents = contents(result);
    }
    outcome.ledger_before = LedgerTool::count(files.ledger());
    outcome.rows_before = effect_rows(files.journal());
    CoreProcess process(files, restarted);
    const auto result = process.run("core-restart", 2, std::move(grant_id));
    outcome.interrupted = result.interrupted;
    if (!result.interrupted) {
        outcome.statuses = statuses(result);
        outcome.contents = contents(result);
    }
    outcome.ledger_after = LedgerTool::count(files.ledger());
    outcome.executions = process.tool->executions.load();
    outcome.rows_after = effect_rows(files.journal());
    return outcome;
}
}  // namespace

TEST(StandaloneCoreToolEffects, RestartReplaysCommittedSlotsWithoutExecutingAnyTool) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3);
    const auto outcome = core_restart(3, restarted);
    EXPECT_EQ(outcome.first_statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(outcome.contents, outcome.first_contents);  // the stored receipts, byte for byte
    EXPECT_EQ(outcome.ledger_before, 3U);
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, RestartWithChangedGrantFailsClosed) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3);
    const auto outcome = core_restart(3, restarted, "substituted-grant");
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, RestartWithChangedExecutableFailsClosed) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3);
    restarted.executable = "ledger-build:v2";
    const auto outcome = core_restart(3, restarted);
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, RestartWithToolNotBoundToBrokerFailsClosed) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3);
    restarted.bind_foreign_tool = true;
    const auto outcome = core_restart(3, restarted);
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, RestartWithChangedArgumentsRejectsOnlyTheChangedSlot) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3, {{1, json{{"extra", true}}}});
    const auto outcome = core_restart(3, restarted);
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>{"succeeded", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, MarkerCommitFailureExecutesNoTool) {
    const TestFiles files;
    CoreOptions options;
    options.calls = batch_arguments("template", 2);
    CoreProcess process(files, options);
    Db(files.journal()).exec(
        "CREATE TRIGGER deny_marker BEFORE INSERT ON neograph_tool_effects "
        "BEGIN SELECT RAISE(ABORT, 'marker denied'); END");
    const auto result = process.run("core-marker", 1);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(statuses(result), (std::vector<std::string>(2, "failed")));
    EXPECT_EQ(process.tool->executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 0U);
    EXPECT_TRUE(effect_rows(files.journal()).empty());
}

TEST(StandaloneCoreToolEffects, LostResponseStaysUnresolvedAndIsNeverReExecutedAfterRestart) {
    const TestFiles files;
    CoreOptions options;
    options.calls = batch_arguments("template", 2, {{1, json{{"lose", true}}}});
    std::vector<EffectRow> rows_before;
    {
        CoreProcess process(files, options);
        const auto result = process.run("core-lost", 1);
        EXPECT_TRUE(result.interrupted);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        rows_before = effect_rows(files.journal());
        ASSERT_EQ(rows_before.size(), 2U);
        EXPECT_TRUE(rows_before[0].has_receipt);
        EXPECT_FALSE(rows_before[1].has_receipt);
    }
    CoreProcess process(files, options);
    for (std::uint64_t attempt = 2; attempt < 4; ++attempt) {
        EXPECT_TRUE(process.run("core-lost", attempt).interrupted);
        EXPECT_EQ(process.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
    }
    expect_same_rows(rows_before, effect_rows(files.journal()));
}

TEST(StandaloneCoreToolEffects, RestartWithReorderedBatchRejectsBothMovedSlots) {
    CoreOptions restarted;
    restarted.calls = batch_arguments("template", 3);
    std::swap(restarted.calls[0], restarted.calls[1]);
    const auto outcome = core_restart(3, restarted);
    EXPECT_EQ(outcome.statuses,
              (std::vector<std::string>{"rejected", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneCoreToolEffects, ReceiptPersistenceFailureBlocksAcrossFreshBrokerConnections) {
    const TestFiles files;
    CoreOptions options;
    options.calls = batch_arguments("template", 2);
    std::vector<EffectRow> pending;
    {
        CoreProcess process(files, options);
        Db(files.journal()).exec(
            "CREATE TRIGGER deny_receipt BEFORE UPDATE ON neograph_tool_effects "
            "BEGIN SELECT RAISE(ABORT, 'receipt denied'); END");
        EXPECT_TRUE(process.run("core-receipt", 1).interrupted);
        EXPECT_EQ(process.tool->executions.load(), 2);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        pending = effect_rows(files.journal());
        ASSERT_EQ(pending.size(), 2U);
        for (const auto& row : pending) EXPECT_FALSE(row.has_receipt);
    }
    Db(files.journal()).exec("DROP TRIGGER deny_receipt");
    for (std::uint64_t attempt = 2; attempt < 4; ++attempt) {
        CoreProcess process(files, options);
        EXPECT_TRUE(process.run("core-receipt", attempt).interrupted);
        EXPECT_EQ(process.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        expect_same_rows(pending, effect_rows(files.journal()));
    }
}

TEST(StandaloneCoreToolEffects, TwoBrokerConnectionsContendingForOneSlotExecuteOnlyOnce) {
    const TestFiles files;
    CoreOptions options;
    options.calls = batch_arguments("template", 1);
    {
        const auto latch = std::make_shared<DispatchLatch>();
        CoreProcess first(files, options, {}, latch);
        CoreProcess contender(files, options);
        graph::RunResult first_result, second_result;
        std::exception_ptr first_error, second_error;
        std::thread worker([&] {
            try { first_result = first.run("core-contended", 1); }
            catch (...) { first_error = std::current_exception(); }
        });
        const bool entered = latch->wait_until_entered();
        try { second_result = contender.run("core-contended", 1); }
        catch (...) { second_error = std::current_exception(); }
        latch->release();
        worker.join();
        if (first_error) std::rethrow_exception(first_error);
        if (second_error) std::rethrow_exception(second_error);
        ASSERT_TRUE(entered);
        EXPECT_FALSE(first_result.interrupted);
        EXPECT_TRUE(second_result.interrupted);
        EXPECT_EQ(first.tool->executions.load(), 1);
        EXPECT_EQ(contender.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    }
    const auto settled = effect_rows(files.journal());
    ASSERT_EQ(settled.size(), 1U);
    ASSERT_TRUE(settled[0].has_receipt);
    CoreProcess restarted(files, options);
    const auto replayed = restarted.run("core-contended", 2);
    EXPECT_FALSE(replayed.interrupted);
    EXPECT_EQ(statuses(replayed), (std::vector<std::string>{"succeeded"}));
    EXPECT_EQ(restarted.tool->executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    expect_same_rows(settled, effect_rows(files.journal()));
}

// ── Direct llm::Agent ────────────────────────────────────────────────────

#ifdef NEOGRAPH_TOOL_EFFECT_TESTS_HAVE_LLM
namespace {

sp::Message assistant_batch(const std::vector<json>& arguments,
                            const std::string& id_prefix = "model-call-") {
    sp::Message assistant;
    assistant.role = sp::Role::Assistant;
    for (std::size_t i = 0; i < arguments.size(); ++i)
        assistant.parts.emplace_back(sp::ToolCall{
            id_prefix + std::to_string(i), "ledger", sp::ToolCallKind::ClientExecuted,
            test::document(arguments[i].dump())});
    return assistant;
}

// Requests every call once with portable-valid model ids, then answers.
class BatchProvider final : public test::LocalProvider {
public:
    explicit BatchProvider(std::vector<json> arguments)
        : LocalProvider(
              [arguments = std::move(arguments)](
                  ProviderRequest request, const PreparedProviderRequest&,
                  const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
                  const auto& messages =
                      std::get<sp::chat::Request>(request.payload).canonical_messages;
                  if (std::any_of(messages.begin(), messages.end(), [](const sp::Message& message) {
                          return message.role == sp::Role::Tool;
                      }))
                      co_return test::success("done");
                  co_return test::success(std::vector<sp::Message>{assistant_batch(arguments)});
              },
              "batch-agent-fixture") {}
};

struct AgentOptions {
    std::string executable = kExecutable;
    bool bind_foreign_tool = false;
    std::shared_ptr<ToolExecutionController> controller;
    ToolGate gate;
};

// One "process": a Tool owned by its Agent plus a broker bound to that Tool.
struct AgentProcess {
    AgentProcess(const TestFiles& files, std::vector<json> arguments, const AgentOptions& options,
                 std::shared_ptr<Rendezvous> rendezvous = {},
                 std::shared_ptr<DispatchLatch> latch = {}) {
        auto effect = std::make_unique<LedgerTool>(files.ledger(), rendezvous, std::move(latch));
        tool = effect.get();
        foreign = std::make_unique<LedgerTool>(files.ledger(), rendezvous);
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::move(effect));
        agent = std::make_unique<llm::Agent>(std::make_shared<BatchProvider>(std::move(arguments)),
                                             std::move(tools), "", "fixture-model");
        if (options.gate) agent->set_tool_gate(options.gate);
        if (options.controller) agent->set_tool_execution_controller(options.controller);
        broker = std::make_shared<SQLiteToolEffectBroker>(
            files.journal(),
            std::vector<SQLiteToolExecutableBinding>{
                {options.bind_foreign_tool ? foreign.get() : tool, options.executable}});
    }

    ToolExecutionContext context(const std::string& run, std::uint64_t attempt,
                                 std::string grant_id = "host-grant") const {
        ToolExecutionContext execution;
        execution.effect_broker = broker;
        execution.identity.owner_scope = "tenant";
        execution.identity.root_run_id = run;
        execution.identity.thread_id = "thread-" + run;
        execution.effect_grant = {"", "agent-operation", std::move(grant_id), attempt};
        return execution;
    }

    LedgerTool* tool = nullptr;
    std::unique_ptr<LedgerTool> foreign;
    std::unique_ptr<llm::Agent> agent;
    std::shared_ptr<SQLiteToolEffectBroker> broker;
};

std::vector<std::string> tool_statuses(const std::vector<sp::Message>& messages) {
    std::vector<std::string> out;
    for (const auto& message : messages)
        if (message.role == sp::Role::Tool) out.push_back(project_message(message).tool_status);
    return out;
}

std::vector<std::string> tool_contents(const std::vector<sp::Message>& messages) {
    std::vector<std::string> out;
    for (const auto& message : messages)
        if (message.role == sp::Role::Tool) out.push_back(project_message(message).content);
    return out;
}

}  // namespace

TEST(StandaloneAgentToolEffects, GateDenialDispatchesNothingAndKeepsLaterSlotsStable) {
    const TestFiles files;
    AgentOptions options;
    options.gate = [](ToolCall call, ToolGateContext) -> asio::awaitable<ToolDecision> {
        if (denies(call.arguments)) co_return ToolDecision::deny("host policy denies this call");
        co_return ToolDecision::allow();
    };
    AgentProcess process(files, batch_arguments("agent", 2, {{0, json{{"deny", true}}}}), options);
    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    EXPECT_EQ(test::text(process.agent->run(messages, 3, process.context("agent-deny", 1))), "done");
    EXPECT_EQ(tool_statuses(messages), (std::vector<std::string>{"rejected", "succeeded"}));
    EXPECT_EQ(process.tool->executions.load(), 1);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    const auto rows = effect_rows(files.journal());
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].ordinal, 1);
}

TEST(StandaloneAgentToolEffects, ConcurrentAgentsOnOneBrokerGetDistinctSlots) {
    constexpr int kAgents = 3;
    constexpr int kBatch = 3;
    const TestFiles files;
    auto rendezvous = std::make_shared<Rendezvous>(2);
    auto controller = widened_controller();
    // One broker connection shared by every Agent, each Agent owning its own Tool.
    std::vector<std::unique_ptr<llm::Agent>> agents;
    std::vector<LedgerTool*> tools;
    std::vector<SQLiteToolExecutableBinding> bindings;
    for (int a = 0; a < kAgents; ++a) {
        auto effect = std::make_unique<LedgerTool>(files.ledger(), rendezvous);
        tools.push_back(effect.get());
        bindings.push_back({effect.get(), kExecutable});
        std::vector<std::unique_ptr<Tool>> owned;
        owned.push_back(std::move(effect));
        const auto who = "agent-" + std::to_string(a);
        agents.push_back(std::make_unique<llm::Agent>(
            std::make_shared<BatchProvider>(
                batch_arguments(who, kBatch, {{0, json{{"rendezvous", who}}}})),
            std::move(owned), "", "fixture-model"));
        agents.back()->set_tool_execution_controller(controller);
    }
    const auto broker = std::make_shared<SQLiteToolEffectBroker>(files.journal(), std::move(bindings));

    std::vector<std::vector<sp::Message>> histories(kAgents,
        std::vector<sp::Message>{test::message("go", sp::Role::User)});
    std::vector<std::exception_ptr> errors(kAgents);
    std::vector<std::thread> threads;
    for (int a = 0; a < kAgents; ++a)
        threads.emplace_back([&, a] {
            try {
                ToolExecutionContext execution;
                execution.effect_broker = broker;
                execution.identity.owner_scope = "tenant";
                execution.identity.root_run_id = "agent-run-" + std::to_string(a);
                execution.identity.thread_id = "agent-thread-" + std::to_string(a);
                execution.effect_grant = {"", "agent-operation", "host-grant", 1};
                (void)agents[a]->run(histories[a], 3, std::move(execution));
            } catch (...) { errors[a] = std::current_exception(); }
        });
    for (auto& thread : threads) thread.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);

    EXPECT_TRUE(rendezvous->overlapped);
    for (const auto& history : histories)
        EXPECT_EQ(tool_statuses(history), (std::vector<std::string>(kBatch, "succeeded")));
    EXPECT_EQ(LedgerTool::count(files.ledger()), static_cast<std::size_t>(kAgents * kBatch));
    const auto rows = effect_rows(files.journal());
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kAgents * kBatch));
    std::map<std::string, std::set<std::int64_t>> ordinals;
    for (const auto& row : rows) {
        ordinals[row.run].insert(row.ordinal);
        EXPECT_TRUE(row.has_receipt);
        EXPECT_EQ(json::parse(row.arguments).at("n"), row.ordinal);
    }
    EXPECT_EQ(ordinals.size(), static_cast<std::size_t>(kAgents));
    for (const auto& [run, seen] : ordinals)
        EXPECT_EQ(seen, (std::set<std::int64_t>{0, 1, 2})) << run;
}

namespace {
struct AgentRestart {
    std::vector<std::string> statuses, contents, first_contents;
    bool interrupted = false;
    std::size_t ledger_before = 0, ledger_after = 0;
    int executions = 0;
    std::vector<EffectRow> rows_before, rows_after;
};

// Attempt 1 commits the batch and is cut off before its replies are persisted;
// the restarted Agent reconnects with the committed assistant message only.
AgentRestart agent_restart(std::vector<json> first_arguments, std::vector<json> restarted_arguments,
                           AgentOptions restarted, std::string grant_id = "host-grant") {
    const TestFiles files;
    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    AgentRestart outcome;
    const auto calls = first_arguments.size();
    {
        AgentProcess process(files, std::move(first_arguments), {});
        EXPECT_EQ(test::text(process.agent->run(messages, 3, process.context("agent-restart", 1))),
                  "done");
        outcome.first_contents = tool_contents(messages);
    }
    outcome.ledger_before = LedgerTool::count(files.ledger());
    outcome.rows_before = effect_rows(files.journal());
    for (std::size_t i = 0; i <= calls; ++i) messages.pop_back();  // final answer + tool replies
    // Model ids are not durable effect slots. Apply the requested recovery
    // change to the pending assistant itself, not an unused future provider reply.
    messages.back() = assistant_batch(restarted_arguments, "recovered-call-");
    AgentProcess process(files, std::move(restarted_arguments), restarted);
    try {
        (void)process.agent->run(messages, 3, process.context("agent-restart", 2, std::move(grant_id)));
        outcome.statuses = tool_statuses(messages);
        outcome.contents = tool_contents(messages);
    } catch (const graph::NodeInterrupt&) {
        outcome.interrupted = true;
    }
    outcome.ledger_after = LedgerTool::count(files.ledger());
    outcome.executions = process.tool->executions.load();
    outcome.rows_after = effect_rows(files.journal());
    return outcome;
}
}  // namespace

TEST(StandaloneAgentToolEffects, RestartReplaysCommittedSlotsWithoutExecutingAnyTool) {
    const auto outcome = agent_restart(batch_arguments("agent", 3), batch_arguments("agent", 3), {});
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(outcome.contents, outcome.first_contents);
    EXPECT_EQ(outcome.ledger_before, 3U);
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, RestartWithChangedGrantFailsClosed) {
    const auto outcome = agent_restart(batch_arguments("agent", 3), batch_arguments("agent", 3), {},
                                       "substituted-grant");
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, RestartWithChangedExecutableFailsClosed) {
    AgentOptions restarted;
    restarted.executable = "ledger-build:v2";
    const auto outcome = agent_restart(batch_arguments("agent", 3), batch_arguments("agent", 3),
                                       restarted);
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, RestartWithToolNotBoundToBrokerFailsClosed) {
    AgentOptions restarted;
    restarted.bind_foreign_tool = true;
    const auto outcome = agent_restart(batch_arguments("agent", 3), batch_arguments("agent", 3),
                                       restarted);
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, RestartWithReorderedBatchRejectsBothMovedSlots) {
    auto reordered = batch_arguments("agent", 3);
    std::swap(reordered[0], reordered[1]);
    const auto outcome = agent_restart(batch_arguments("agent", 3), std::move(reordered), {});
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>{"rejected", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, MarkerCommitFailureExecutesNoTool) {
    const TestFiles files;
    AgentProcess process(files, batch_arguments("agent", 2), {});
    Db(files.journal()).exec(
        "CREATE TRIGGER deny_marker BEFORE INSERT ON neograph_tool_effects "
        "BEGIN SELECT RAISE(ABORT, 'marker denied'); END");
    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    EXPECT_EQ(test::text(process.agent->run(messages, 3, process.context("agent-marker", 1))),
              "done");
    EXPECT_EQ(tool_statuses(messages), (std::vector<std::string>(2, "failed")));
    EXPECT_EQ(process.tool->executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 0U);
    EXPECT_TRUE(effect_rows(files.journal()).empty());
}

TEST(StandaloneAgentToolEffects, LostResponseStaysUnresolvedAndIsNeverReExecutedAfterRestart) {
    const TestFiles files;
    const auto arguments = batch_arguments("agent", 2, {{1, json{{"lose", true}}}});
    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    std::vector<EffectRow> rows_before;
    {
        AgentProcess process(files, arguments, {});
        EXPECT_THROW((void)process.agent->run(messages, 3, process.context("agent-lost", 1)),
                     graph::NodeInterrupt);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        rows_before = effect_rows(files.journal());
        ASSERT_EQ(rows_before.size(), 2U);
        EXPECT_TRUE(rows_before[0].has_receipt);
        EXPECT_FALSE(rows_before[1].has_receipt);
    }
    AgentProcess process(files, arguments, {});
    for (std::uint64_t attempt = 2; attempt < 4; ++attempt) {
        EXPECT_THROW((void)process.agent->run(messages, 3, process.context("agent-lost", attempt)),
                     graph::NodeInterrupt);
        EXPECT_EQ(process.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
    }
    expect_same_rows(rows_before, effect_rows(files.journal()));
}

TEST(StandaloneAgentToolEffects, RestartWithChangedArgumentsRejectsOnlyTheChangedSlot) {
    const auto outcome = agent_restart(
        batch_arguments("agent", 3),
        batch_arguments("agent", 3, {{1, json{{"extra", true}}}}), {});
    EXPECT_EQ(outcome.statuses,
              (std::vector<std::string>{"succeeded", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger_after, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(StandaloneAgentToolEffects, ReceiptPersistenceFailureBlocksAcrossFreshBrokerConnections) {
    const TestFiles files;
    const auto arguments = batch_arguments("agent", 2);
    std::vector<sp::Message> messages{test::message("go", sp::Role::User)};
    std::vector<EffectRow> pending;
    {
        AgentProcess process(files, arguments, {});
        Db(files.journal()).exec(
            "CREATE TRIGGER deny_receipt BEFORE UPDATE ON neograph_tool_effects "
            "BEGIN SELECT RAISE(ABORT, 'receipt denied'); END");
        EXPECT_THROW((void)process.agent->run(messages, 3, process.context("agent-receipt", 1)),
                     graph::NodeInterrupt);
        EXPECT_EQ(process.tool->executions.load(), 2);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        pending = effect_rows(files.journal());
        ASSERT_EQ(pending.size(), 2U);
        for (const auto& row : pending) EXPECT_FALSE(row.has_receipt);
    }
    Db(files.journal()).exec("DROP TRIGGER deny_receipt");
    for (std::uint64_t attempt = 2; attempt < 4; ++attempt) {
        AgentProcess process(files, arguments, {});
        EXPECT_THROW((void)process.agent->run(messages, 3, process.context("agent-receipt", attempt)),
                     graph::NodeInterrupt);
        EXPECT_EQ(process.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 2U);
        expect_same_rows(pending, effect_rows(files.journal()));
    }
}

TEST(StandaloneAgentToolEffects, TwoBrokerConnectionsContendingForOneSlotExecuteOnlyOnce) {
    const TestFiles files;
    const auto arguments = batch_arguments("agent", 1);
    {
        const auto latch = std::make_shared<DispatchLatch>();
        AgentProcess first(files, arguments, {}, {}, latch);
        AgentProcess contender(files, arguments, {});
        std::vector<sp::Message> first_messages{test::message("go", sp::Role::User)};
        std::vector<sp::Message> second_messages{test::message("go", sp::Role::User)};
        std::exception_ptr first_error, second_error;
        bool second_interrupted = false;
        std::thread worker([&] {
            try { (void)first.agent->run(first_messages, 3, first.context("agent-contended", 1)); }
            catch (...) { first_error = std::current_exception(); }
        });
        const bool entered = latch->wait_until_entered();
        try {
            (void)contender.agent->run(second_messages, 3, contender.context("agent-contended", 1));
        } catch (const graph::NodeInterrupt&) {
            second_interrupted = true;
        } catch (...) { second_error = std::current_exception(); }
        latch->release();
        worker.join();
        if (first_error) std::rethrow_exception(first_error);
        if (second_error) std::rethrow_exception(second_error);
        ASSERT_TRUE(entered);
        EXPECT_TRUE(second_interrupted);
        EXPECT_EQ(tool_statuses(first_messages), (std::vector<std::string>{"succeeded"}));
        EXPECT_EQ(first.tool->executions.load(), 1);
        EXPECT_EQ(contender.tool->executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    }
    const auto settled = effect_rows(files.journal());
    ASSERT_EQ(settled.size(), 1U);
    ASSERT_TRUE(settled[0].has_receipt);
    AgentProcess restarted(files, arguments, {});
    std::vector<sp::Message> messages{
        test::message("go", sp::Role::User), assistant_batch(arguments, "recovered-call-")};
    EXPECT_EQ(test::text(restarted.agent->run(messages, 3, restarted.context("agent-contended", 2))),
              "done");
    EXPECT_EQ(tool_statuses(messages), (std::vector<std::string>{"succeeded"}));
    EXPECT_EQ(restarted.tool->executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1U);
    expect_same_rows(settled, effect_rows(files.journal()));
}
#endif
