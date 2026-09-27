#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/tool.h>

#include <sqlite3.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>

namespace {
using namespace neograph;
using namespace neograph::program;
using neograph::graph::ProviderCallIdentity;
using Journal = SQLiteProgramProviderCallJournal;

std::string digest(char c) { return "sha256:" + std::string(64, c); }
struct SqlConnection {
    sqlite3* db = nullptr;
    ~SqlConnection() { if (db) sqlite3_close_v2(db); }
};
struct Database {
    explicit Database(std::string name) {
        path = (std::filesystem::temp_directory_path() /
                ("neograph-program-provider-" + name + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".sqlite")).string();
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-shm", error);
    }
    ~Database() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(path + "-wal", error);
        std::filesystem::remove(path + "-shm", error);
    }
    std::string path;
};
class Transport final : public Provider {
public:
    std::atomic<int> calls{0};
    bool fail_after_send = false;
    ChatCompletion complete(const CompletionParams&) override {
        ++calls;
        if (fail_after_send) throw std::runtime_error("transport disconnected after send");
        ChatCompletion result;
        result.message = {"assistant", "response"};
        result.message.reasoning = "thought";
        result.usage.prompt_tokens = 7;
        result.usage.cached_prompt_tokens = 3;
        return result;
    }
    std::string get_name() const override { return "transport"; }
};

struct Call {
    std::string owner = "tenant:a";
    std::string version = "version:a";
    std::string run = "run:a";
    std::string operation = "root";
    std::uint64_t attempt = 1;
    std::string thread = "core-thread";
    std::string task = "s1:reason";
    ProgramCoreProviderCallContext context() const {
        return {owner, version, run, operation, attempt};
    }
    ProviderCallIdentity identity() const {
        return {owner, run, thread, task, "reason", 0};
    }
    std::string id() const { return Journal::logical_call_id(context(), identity()); }
    CompletionParams params() const {
        CompletionParams params;
        params.model = "model";
        params.messages = {{"user", "question"}};
        return params;
    }
};
ChatCompletion invoke(Journal& journal, const Call& call, std::shared_ptr<Transport> transport,
                      std::string binding = digest('a'), CompletionParams params = {}) {
    if (params.model.empty()) params = call.params();
    auto resolved = journal.bind(call.context(), std::move(binding));
    return neograph::async::run_sync(resolved.broker->invoke(call.identity(), transport, std::move(params), {}));
}
} // namespace

TEST(ProgramProviderJournal, DurableReplayChecksRequestDeploymentAndAttemptProvenance) {
    Database db("replay");
    auto transport = std::make_shared<Transport>();
    Call call;
    {
        Journal journal(db.path);
        const auto result = invoke(journal, call, transport);
        EXPECT_EQ(result.message.content, "response");
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state, Journal::State::Succeeded);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    const auto replayed = invoke(reopened, call, transport);
    EXPECT_EQ(replayed.message.reasoning, "thought");
    EXPECT_EQ(replayed.usage.cached_prompt_tokens, 3);
    EXPECT_EQ(reopened.inspect(call.owner, call.id())->original_attempt, "1");
    auto changed = call.params();
    changed.messages[0].content = "different";
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    changed = call.params();
    changed.extra_fields = {{"reasoning.effort", "high"}};
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    changed = call.params();
    changed.model = "different-model";
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    EXPECT_THROW(invoke(reopened, call, transport, digest('b')), std::runtime_error);
    EXPECT_EQ(transport->calls, 1);
}

TEST(ProgramProviderJournal, DispatchedMarkerSurvivesCrashWindowAndRequiresEvidence) {
    Database db("uncertain");
    auto transport = std::make_shared<Transport>();
    transport->fail_after_send = true;
    Call call;
    {
        Journal journal(db.path);
        EXPECT_THROW(invoke(journal, call, transport), std::runtime_error);
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state, Journal::State::ReconciliationRequired);
    }
    transport->fail_after_send = false;
    Journal reopened(db.path);
    call.attempt = 4;
    EXPECT_THROW(invoke(reopened, call, transport), std::runtime_error);
    EXPECT_EQ(transport->calls, 1);
    EXPECT_THROW(reopened.reconcile_success(call.owner, call.id(), "", ChatCompletion{}),
                 std::invalid_argument);
    ChatCompletion proven;
    proven.message = {"assistant", "confirmed externally"};
    reopened.reconcile_success(call.owner, call.id(), digest('c'), proven);
    EXPECT_EQ(invoke(reopened, call, transport).message.content, "confirmed externally");
    EXPECT_EQ(transport->calls, 1);
    EXPECT_THROW(reopened.reconcile_success(call.owner, call.id(), digest('d'), proven),
                 std::runtime_error);
}

TEST(ProgramProviderJournal, PreDispatchPersistenceFailureAndStaleScopeNeverCallTransport) {
    Database db("failure");
    Journal journal(db.path);
    auto transport = std::make_shared<Transport>();
    Call call;
    auto stale = journal.bind(call.context(), digest('a'));
    auto identity = call.identity();
    identity.run_id = "another-run";
    EXPECT_THROW(neograph::async::run_sync(stale.broker->invoke(identity, transport, call.params(), {})),
                 std::invalid_argument);
    // SQLite aborts the write-ahead marker insert before transport.
    SqlConnection fault;
    ASSERT_EQ(sqlite3_open(db.path.c_str(), &fault.db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(fault.db,
        "CREATE TRIGGER deny_provider_dispatch BEFORE INSERT ON ng_program_provider_calls "
        "BEGIN SELECT RAISE(FAIL, 'injected persistence failure'); END",
        nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_THROW(invoke(journal, call, transport), std::runtime_error);
    EXPECT_EQ(transport->calls, 0);
    EXPECT_FALSE(journal.inspect(call.owner, call.id()));
}

TEST(ProgramProviderJournal, CrashWindowAfterMarkerBeforeOutcomeDoesNotRedispatch) {
    Database db("missing-outcome");
    auto transport = std::make_shared<Transport>();
    Call call;
    {
        Journal journal(db.path);
        SqlConnection fault;
        ASSERT_EQ(sqlite3_open(db.path.c_str(), &fault.db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(fault.db,
            "CREATE TRIGGER deny_provider_outcome BEFORE UPDATE ON ng_program_provider_calls "
            "BEGIN SELECT RAISE(FAIL, 'injected outcome failure'); END",
            nullptr, nullptr, nullptr), SQLITE_OK);
        EXPECT_THROW(invoke(journal, call, transport), std::runtime_error);
        EXPECT_EQ(transport->calls, 1);
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state, Journal::State::Dispatched);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    EXPECT_THROW(invoke(reopened, call, transport), std::runtime_error);
    EXPECT_EQ(transport->calls, 1);
}

TEST(ProgramProviderJournal, ConcurrentRunsAndSecondModelTurnAreDistinctSlots) {
    Database db("parallel");
    Journal journal(db.path);
    auto transport = std::make_shared<Transport>();
    Call left;
    Call right = left;
    right.owner = "tenant:b";
    right.run = "run:b";
    right.thread = "another-thread";
    auto first = std::async(std::launch::async, [&] { return invoke(journal, left, transport); });
    auto second = std::async(std::launch::async, [&] { return invoke(journal, right, transport); });
    EXPECT_EQ(first.get().message.content, "response");
    EXPECT_EQ(second.get().message.content, "response");
    EXPECT_FALSE(journal.inspect(left.owner, right.id()));
    Call react = left;
    react.task = "s3:reason";
    auto observed = react.params();
    observed.messages.push_back({"tool", "observation"});
    EXPECT_EQ(invoke(journal, react, transport, digest('a'), observed).message.content, "response");
    EXPECT_EQ(invoke(journal, react, transport, digest('a'), observed).message.content, "response");
    Call send_a = left;
    send_a.task = "s1:send[0]:reason:abc";
    Call send_b = send_a;
    send_b.task = "s1:send[1]:reason:abc";
    EXPECT_NE(send_a.id(), send_b.id());
    EXPECT_EQ(invoke(journal, send_a, transport).message.content, "response");
    EXPECT_EQ(invoke(journal, send_b, transport).message.content, "response");
    EXPECT_EQ(transport->calls, 5);
}

TEST(ProgramProviderJournal, SharedEngineConcurrentRunsKeepDistinctOwnerAndRun) {
    using namespace neograph::graph;
    Database db("shared-engine");
    Journal journal(db.path);
    auto transport = std::make_shared<Transport>();
    NodeContext nodes;
    nodes.provider = transport;
    nodes.model = "model";
    const json definition{
        {"name", "provider-journal-shared"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"reason", {{"type", "llm_call"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "reason"}},
                                {{"from", "reason"}, {"to", "__end__"}}})}};
    auto engine = GraphEngine::compile(definition, nodes);
    auto invoke_run = [&](Call call) {
        auto binding = journal.bind(call.context(), digest('a'));
        RunResources resources;
        resources.provider_call_broker = binding.broker;
        RunConfig config;
        config.thread_id = call.thread;
        config.input = {{"messages", json::array({{{"role", "user"}, {"content", "question"}}})}};
        RunMetadata metadata;
        metadata.owner_scope = call.owner;
        metadata.run_id = call.run;
        const auto result = neograph::async::run_sync(
            engine->run_async(config, metadata, std::move(resources)));
        EXPECT_FALSE(result.interrupted);
        call.task = "s0:reason";
        const auto receipt = journal.inspect(call.owner, call.id());
        EXPECT_TRUE(receipt.has_value());
        if (receipt) EXPECT_EQ(receipt->state, Journal::State::Succeeded);
        return call;
    };
    Call first;
    Call second = first;
    second.owner = "tenant:b";
    second.run = "run:b";
    second.thread = "other-core-thread";
    auto running_a = std::async(std::launch::async, [&] { return invoke_run(first); });
    auto running_b = std::async(std::launch::async, [&] { return invoke_run(second); });
    const auto completed_a = running_a.get();
    const auto completed_b = running_b.get();
    EXPECT_NE(completed_a.id(), completed_b.id());
    EXPECT_EQ(transport->calls, 2);
}

TEST(ProgramProviderJournal, CoreCheckpointResumeReplaysBoundCompletionWithoutTransport) {
    using namespace neograph::graph;
    Database db("checkpoint");
    auto transport = std::make_shared<Transport>();
    NodeContext nodes;
    nodes.provider = transport;
    nodes.model = "model";
    const json definition{
        {"name", "provider-journal"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"reason", {{"type", "llm_call"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "reason"}},
                                {{"from", "reason"}, {"to", "__end__"}}})}};
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto before = definition;
    before["interrupt_before"] = json::array({"reason"});
    auto paused_engine = GraphEngine::compile(before, nodes, checkpoints);
    RunConfig config;
    config.thread_id = "core-thread";
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "question"}}})}};
    RunMetadata metadata;
    metadata.owner_scope = "tenant:a";
    metadata.run_id = "run:a";
    const auto paused = neograph::async::run_sync(paused_engine->run_async(config, metadata));
    ASSERT_TRUE(paused.interrupted);
    ASSERT_FALSE(paused.checkpoint_id.empty());
    auto resumed_engine = GraphEngine::compile(definition, nodes, checkpoints);
    auto resume = [&](Journal& journal, std::uint64_t attempt) {
        auto binding = journal.bind(ProgramCoreProviderCallContext{
            "tenant:a", "version:a", "run:a", "root", attempt}, digest('a'));
        RunResources resources;
        resources.provider_call_broker = binding.broker;
        return neograph::async::run_sync(resumed_engine->resume_from_async(
            config, paused.checkpoint_id, {}, {}, metadata, std::move(resources)));
    };
    {
        Journal journal(db.path);
        EXPECT_FALSE(resume(journal, 1).interrupted);
        EXPECT_EQ(transport->calls, 1);
    }
    Journal reopened(db.path);
    EXPECT_FALSE(resume(reopened, 2).interrupted);
    EXPECT_EQ(transport->calls, 1);
}

namespace {
class ObservationTool final : public Tool {
public:
    ChatTool get_definition() const override {
        return {"observe", "Return one observation", json::object()};
    }
    std::string get_name() const override { return "observe"; }
    std::string execute(const json&) override { return "tool-observation"; }
};

class ReactTransport final : public Provider {
public:
    ChatCompletion complete(const CompletionParams& params) override {
        ChatCompletion result;
        result.message.role = "assistant";
        if (++calls == 1) {
            result.message.content = "Thought: call tool";
            result.message.tool_calls.push_back({"tool-1", "observe", "{}"});
        } else {
            bool found_observation = false;
            for (const auto& message : params.messages)
                found_observation |= message.role == "tool" &&
                                     message.content == "tool-observation";
            if (!found_observation)
                throw std::runtime_error("ReAct second turn lost its tool observation");
            result.message.content = "final-answer";
        }
        return result;
    }
    std::string get_name() const override { return "react-transport"; }
    int calls = 0;
};
} // namespace

TEST(ProgramProviderJournal, BuiltInReActSecondTurnGetsNewSlotAfterToolObservation) {
    using namespace neograph::graph;
    Database db("react");
    Journal journal(db.path);
    auto provider = std::make_shared<ReactTransport>();
    auto       tool     = std::make_shared<ObservationTool>();
    NodeContext nodes;
    nodes.provider = provider;
    nodes.model = "model";
    nodes.tools = ToolSet(std::vector<std::shared_ptr<Tool>>{tool});
    const json definition{
        {"name", "provider-journal-react"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"llm", {{"type", "llm_call"}}},
                   {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "llm"}},
                                {{"from", "llm"}, {"condition", "has_tool_calls"},
                                 {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
                                {{"from", "tools"}, {"to", "llm"}}})}};
    auto engine = GraphEngine::compile(definition, nodes);
    auto binding = journal.bind(ProgramCoreProviderCallContext{
        "tenant:a", "version:a", "run:a", "root", 1}, digest('a'));
    RunResources resources;
    resources.provider_call_broker = binding.broker;
    RunConfig config;
    config.thread_id = "core-react-thread";
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "question"}}})}};
    RunMetadata metadata;
    metadata.owner_scope = "tenant:a";
    metadata.run_id = "run:a";
    const auto result = neograph::async::run_sync(
        engine->run_async(config, metadata, std::move(resources)));
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(provider->calls, 2);
    const ProgramCoreProviderCallContext context{"tenant:a", "version:a", "run:a", "root", 1};
    const ProviderCallIdentity first{"tenant:a", "run:a", config.thread_id, "s0:llm", "llm", 0};
    const ProviderCallIdentity second{"tenant:a", "run:a", config.thread_id, "s2:llm", "llm", 0};
    EXPECT_NE(Journal::logical_call_id(context, first), Journal::logical_call_id(context, second));
    ASSERT_TRUE(journal.inspect("tenant:a", Journal::logical_call_id(context, first)));
    ASSERT_TRUE(journal.inspect("tenant:a", Journal::logical_call_id(context, second)));
}
