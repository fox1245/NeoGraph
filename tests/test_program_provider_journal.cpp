#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/program/program.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/tool.h>
#include <neograph/provider_outcome_codec.h>
#include "fixtures/typed_provider.h"
#include "provider_failure.h"
#include <core/native_archive.h>
#include <core/native.h>
#include <codecs/messages.h>
#include "canonical_json.h"

#include <sqlite3.h>
#include <gtest/gtest.h>

#include <algorithm>
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
        // Native custody roots derived from this path must not inherit OS temp symlinks.
        path = (std::filesystem::canonical(std::filesystem::temp_directory_path()) /
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
class Transport final : public neograph::test::LocalProvider {
    struct State {
        std::atomic<int> calls{0};
        bool fail_after_send = false;
        std::vector<sp::Part> parts;
        sp::AttemptEvidence attempt;
        std::vector<sp::RawWire> raw_events;
    };
public:
    explicit Transport(std::shared_ptr<State> state = std::make_shared<State>())
        : LocalProvider([state](auto, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              ++state->calls;
              if (state->fail_after_send)
                  throw std::runtime_error("transport disconnected after send");
              auto message = neograph::test::message("response");
              message.parts.emplace_back(sp::Thinking{"thought"});
              message.parts.insert(message.parts.end(), state->parts.begin(), state->parts.end());
              auto usage = neograph::test::usage(7, std::nullopt, std::nullopt,
                                                sp::UsageStage::Partial);
              usage.cache_read = sp::Count{3};
              sp::Completion completion;
              completion.messages = {std::move(message)};
              completion.stop = {sp::StopKind::EndTurn, "stop"};
              completion.usage = std::move(usage);
              completion.attempt = state->attempt;
              completion.raw_events = state->raw_events;
              co_return std::make_shared<const sp::Outcome>(std::move(completion));
          }, "transport"), calls(state->calls),
          fail_after_send(state->fail_after_send), parts(state->parts),
          attempt(state->attempt), raw_events(state->raw_events) {}
    std::atomic<int>& calls;
    bool& fail_after_send;
    std::vector<sp::Part>& parts;
    sp::AttemptEvidence& attempt;
    std::vector<sp::RawWire>& raw_events;
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
    ProviderRequest params() const {
        return neograph::test::request("model", "question");
    }
};
sp::runtime::Result invoke(Journal& journal, const Call& call, std::shared_ptr<Provider> transport,
                          std::string binding = digest('a'),
                          std::optional<ProviderRequest> params = std::nullopt) {
    auto resolved = journal.bind(call.context(), std::move(binding));
    return neograph::async::run_sync(resolved.broker->invoke(
        call.identity(), transport, params ? std::move(*params) : call.params()));
}
} // namespace

TEST(ProgramProviderJournal, RejectsVolatileStorage) {
    for (const auto* path : {"", ":memory:", "file::memory:?cache=shared",
                             "file:provider-journal?mode=memory&cache=shared"}) {
        SCOPED_TRACE(path);
        EXPECT_THROW((void)Journal{path}, std::invalid_argument);
    }
}

TEST(ProgramProviderJournal, ReopenedCompletionPreservesOrderedPartsWithoutRedispatch) {
    Database db("artifacts");
    auto transport = std::make_shared<Transport>();
    transport->parts = {
        sp::Image{"image/png", std::make_shared<const std::string>("AAEC/w==")},
        sp::Opaque{"generated-image", neograph::test::document(
            R"({"url":"https://example.test/image","file_id":"file-image","width":2,"nested":{"seed":17,"flags":[true,false]}})")},
        sp::Opaque{"generated-video", neograph::test::document(
            R"({"mime_type":"video/mp4","base64_data":"","url":"https://example.test/video","file_id":"file-video","duration":1.25,"provider":"test"})")},
        sp::RedactedThinking{"redacted"},
        sp::Refusal{"refusal detail", "safety"},
        sp::ToolCall{"tool-1", "lookup", sp::ToolCallKind::ClientExecuted,
                     neograph::test::document(R"({"key":"value"})")}};
    transport->attempt = {true, 91, true, 1, 2, true};
    transport->raw_events = {
        {"unknown.first", neograph::test::document(
            R"({"marker":1,"future":{"integer":18446744073709551615,"items":[null,true]}})")},
        {"unknown.second", neograph::test::document(R"({"marker":2,"unrecognized":"retained"})")}};
    Call call;
    {
        Journal journal(db.path);
        const auto result = invoke(journal, call, transport);
        ASSERT_EQ(neograph::test::completion(result).messages.at(0).parts.size(),
                  transport->parts.size() + 2);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    const auto replayed = invoke(reopened, call, transport);
    EXPECT_EQ(transport->calls, 1);
    const auto& message = neograph::test::completion(replayed).messages.at(0);
    ASSERT_EQ(message.parts.size(), transport->parts.size() + 2);
    auto expected = neograph::test::message("response");
    expected.parts.emplace_back(sp::Thinking{"thought"});
    expected.parts.insert(expected.parts.end(), transport->parts.begin(), transport->parts.end());
    EXPECT_EQ(neograph::program::detail::canonical_json_bytes(
                  neograph::provider_codec::encode_message(message)),
              neograph::program::detail::canonical_json_bytes(
                  neograph::provider_codec::encode_message(expected)));
    const auto& completion = neograph::test::completion(replayed);
    EXPECT_TRUE(completion.attempt.prior_usage_unknown);
    EXPECT_EQ(completion.attempt.attempts, 2U);
    ASSERT_EQ(completion.raw_events.size(), 2U);
    for (std::size_t i = 0; i < completion.raw_events.size(); ++i) {
        EXPECT_EQ(completion.raw_events[i].type, transport->raw_events[i].type);
        ASSERT_TRUE(completion.raw_events[i].payload);
        EXPECT_EQ(completion.raw_events[i].payload->root().dump(),
                  transport->raw_events[i].payload->root().dump());
    }
}

TEST(ProgramProviderJournal, DurableReplayChecksRequestDeploymentAndAttemptProvenance) {
    Database db("replay");
    auto transport = std::make_shared<Transport>();
    Call call;
    {
        Journal journal(db.path);
        const auto result = invoke(journal, call, transport);
        EXPECT_EQ(neograph::test::text(result), "response");
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state, Journal::State::Succeeded);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    const auto replayed = invoke(reopened, call, transport);
    const auto& completion = neograph::test::completion(replayed);
    EXPECT_EQ(std::get<sp::Thinking>(completion.messages.at(0).parts.at(1)).text, "thought");
    ASSERT_TRUE(completion.usage.cache_read.has_value());
    EXPECT_EQ(completion.usage.cache_read->value, 3);
    EXPECT_FALSE(completion.usage.output_total.has_value());
    EXPECT_FALSE(completion.usage.total.has_value());
    EXPECT_EQ(completion.usage.stage, sp::UsageStage::Partial);
    EXPECT_EQ(reopened.inspect(call.owner, call.id())->original_attempt, "1");
    auto changed = call.params();
    std::get<sp::chat::Request>(changed.payload).canonical_messages[0].parts[0] = sp::Text{"different"};
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    changed = call.params();
    std::get<sp::chat::Request>(changed.payload).reasoning_effort = "high";
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    changed = call.params();
    std::get<sp::chat::Request>(changed.payload).model = "different-model";
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
    EXPECT_THROW(invoke(reopened, call, transport, digest('b')), std::runtime_error);
    changed = call.params();
    changed.mode = ProviderMode::Stream;
    EXPECT_THROW(invoke(reopened, call, transport, digest('a'), changed), std::runtime_error);
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
    EXPECT_THROW(reopened.reconcile_success(call.owner, call.id(), "",
                                           neograph::test::success("")),
                 std::invalid_argument);
    const auto proven = neograph::test::success(std::vector<sp::Message>{
        {"reconciled", sp::Role::Assistant, {
            sp::Text{"confirmed externally"},
            sp::Opaque{"generated-file", neograph::test::document(
                R"({"mime_type":"application/octet-stream","base64_data":"AAE=","url":"","file_id":"reconciled-file","receipt":"external-proof"})")}}}});
    reopened.reconcile_success(call.owner, call.id(), digest('c'), proven);
    EXPECT_EQ(neograph::test::text(invoke(reopened, call, transport)), "confirmed externally");
    EXPECT_EQ(transport->calls, 1);
    const auto replayed = invoke(reopened, call, transport);
    EXPECT_EQ(neograph::program::detail::canonical_json_bytes(
                  neograph::provider_codec::observe_outcome(*replayed)),
              neograph::program::detail::canonical_json_bytes(
                  neograph::provider_codec::observe_outcome(*proven)));
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
    EXPECT_THROW(neograph::async::run_sync(stale.broker->invoke(identity, transport, call.params())),
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
    const auto actual = neograph::test::success(std::vector<sp::Message>{
        {"actual-message", sp::Role::Assistant, {sp::Text{"response"},
            sp::Thinking{"thought"},
            sp::Opaque{"provider-output", neograph::test::document(R"({"nested":[true,7]})")}}}},
        neograph::test::usage(std::nullopt, 0, std::nullopt, sp::UsageStage::Partial));
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto transport = std::make_shared<neograph::test::LocalProvider>(
        [calls, actual](auto, const auto&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            co_return actual;
        }, "persistence-fault-provider", neograph::test::bounded_client());
    Call call;
    auto request = neograph::test::request("test-model", "question");
    std::get<sp::chat::Request>(request.payload).max_output_tokens = 1;
    auto usage = std::make_shared<UsageAccumulator>();
    auto invoke_scoped = [&](Journal& journal) {
        auto identity = call.identity();
        identity.usage = usage;
        identity.model_token_budget = 2;
        identity.budget_exhausted = std::make_shared<std::atomic_bool>(false);
        identity.budget_cancel_token = std::make_shared<neograph::graph::CancelToken>();
        auto resolved = journal.bind(call.context(), digest('a'));
        return neograph::async::run_sync(
            resolved.broker->invoke(std::move(identity), transport, request));
    };
    {
        Journal journal(db.path);
        SqlConnection fault;
        ASSERT_EQ(sqlite3_open(db.path.c_str(), &fault.db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(fault.db,
            "CREATE TRIGGER deny_provider_outcome BEFORE UPDATE ON ng_program_provider_calls "
            "BEGIN SELECT RAISE(FAIL, 'injected outcome failure'); END",
            nullptr, nullptr, nullptr), SQLITE_OK);
        try {
            (void)invoke_scoped(journal);
            FAIL() << "outcome persistence fault must escape with the actual owned result";
        } catch (const ProgramProviderOutcomePersistenceError& error) {
            EXPECT_EQ(error.outcome(), actual);
            ASSERT_TRUE(error.cause());
            EXPECT_FALSE(error.observer_error());
            const auto& returned = neograph::test::completion(error.outcome());
            ASSERT_EQ(returned.messages.at(0).parts.size(), 3U);
            EXPECT_EQ(std::get<sp::Text>(returned.messages.at(0).parts.at(0)).value, "response");
            EXPECT_EQ(std::get<sp::Thinking>(returned.messages.at(0).parts.at(1)).text, "thought");
            EXPECT_EQ(std::get<sp::Opaque>(returned.messages.at(0).parts.at(2)).wire_type,
                      "provider-output");
            EXPECT_FALSE(returned.usage.input_total);
            ASSERT_TRUE(returned.usage.output_total);
            EXPECT_EQ(returned.usage.output_total->value, 0U);
            EXPECT_FALSE(returned.usage.total);
        }
        EXPECT_EQ(calls->load(), 1U);
        EXPECT_EQ(usage->total_tokens_wide(), 2U);
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state, Journal::State::Dispatched);
        EXPECT_EQ(journal.inspect(call.owner, call.id())->claim_amount, 2U);
        EXPECT_EQ(journal.inspect(call.owner, call.id())->committed_amount, 2U);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    usage = std::make_shared<UsageAccumulator>();
    EXPECT_THROW(invoke_scoped(reopened), std::runtime_error);
    EXPECT_EQ(calls->load(), 1U);
    EXPECT_EQ(usage->total_tokens_wide(), 2U);
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
    EXPECT_EQ(neograph::test::text(first.get()), "response");
    EXPECT_EQ(neograph::test::text(second.get()), "response");
    EXPECT_FALSE(journal.inspect(left.owner, right.id()));
    Call react = left;
    react.task = "s3:reason";
    auto observed = react.params();
    auto& history = std::get<sp::chat::Request>(observed.payload).canonical_messages;
    history.push_back(sp::Message{"tool-request", sp::Role::Assistant,
        {sp::ToolCall{"tool-1", "observe", sp::ToolCallKind::ClientExecuted,
                      neograph::test::document("{}")}}});
    history.push_back(sp::Message{"tool-observation", sp::Role::Tool,
                                  {sp::ToolResult{"tool-1", "observation"}}});
    std::get<sp::chat::Request>(observed.payload).tools.push_back(
        {"observe", "Observe", neograph::test::document(R"({"type":"object"})")});
    EXPECT_EQ(neograph::test::text(invoke(journal, react, transport, digest('a'), observed)), "response");
    EXPECT_EQ(neograph::test::text(invoke(journal, react, transport, digest('a'), observed)), "response");
    Call send_a = left;
    send_a.task = "s1:send[0]:reason:abc";
    Call send_b = send_a;
    send_b.task = "s1:send[1]:reason:abc";
    EXPECT_NE(send_a.id(), send_b.id());
    EXPECT_EQ(neograph::test::text(invoke(journal, send_a, transport)), "response");
    EXPECT_EQ(neograph::test::text(invoke(journal, send_b, transport)), "response");
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
    auto engine = GraphEngine::compile(before, nodes, checkpoints);
    RunConfig config;
    config.thread_id = "core-thread";
    config.input = {{"messages", json::array({{{"role", "user"}, {"content", "question"}}})}};
    RunMetadata metadata;
    metadata.owner_scope = "tenant:a";
    metadata.run_id = "run:a";
    auto resources_for = [&](Journal& journal, std::uint64_t attempt) {
        auto binding = journal.bind(ProgramCoreProviderCallContext{
            "tenant:a", "version:a", "run:a", "root", attempt}, digest('a'));
        RunResources resources;
        resources.provider_call_broker = binding.broker;
        return resources;
    };
    auto original = std::make_unique<Journal>(db.path);
    const auto paused = neograph::async::run_sync(engine->run_async(
        config, metadata, resources_for(*original, 1)));
    ASSERT_TRUE(paused.interrupted);
    ASSERT_FALSE(paused.checkpoint_id.empty());
    auto resume = [&](Journal& journal, std::uint64_t attempt) {
        return neograph::async::run_sync(engine->resume_from_async(
            config, paused.checkpoint_id, {}, {}, metadata, resources_for(journal, attempt)));
    };
    EXPECT_FALSE(resume(*original, 1).interrupted);
    EXPECT_EQ(transport->calls, 1);
    original.reset();
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

class ReactTransport final : public neograph::test::LocalProvider {
public:
    explicit ReactTransport(std::shared_ptr<std::atomic<int>> count =
                                std::make_shared<std::atomic<int>>(0))
        : LocalProvider([count](neograph::ProviderRequest request, const auto&, const auto&)
                            -> asio::awaitable<sp::runtime::Result> {
              if (++*count == 1)
                  co_return neograph::test::success(std::vector<sp::Message>{
                      {"first", sp::Role::Assistant, {sp::Text{"Thought: call tool"},
                          sp::ToolCall{"tool-1", "observe", sp::ToolCallKind::ClientExecuted,
                                       neograph::test::document("{}")}}}});
              bool found_observation = false;
              for (const auto& message : std::get<sp::chat::Request>(request.payload).canonical_messages)
                  for (const auto& part : message.parts)
                      if (const auto* result = std::get_if<sp::ToolResult>(&part))
                          found_observation |= message.role == sp::Role::Tool &&
                              result->tool_use_id == "tool-1" && result->content == "tool-observation";
              if (!found_observation)
                  throw std::runtime_error("ReAct second turn lost its tool observation");
              co_return neograph::test::success("final-answer");
          }, "react-transport"), calls(*count) {}
    std::atomic<int>& calls;
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

TEST(ProgramProviderJournal, InvalidPreparationNeverWritesDispatchReceipt) {
    Database db("invalid-preparation");
    Journal journal(db.path);
    Call call;
    auto transport = std::make_shared<Transport>();
    auto request = call.params();
    std::get<sp::chat::Request>(request.payload).max_output_tokens = 0;
    const auto rejected = invoke(journal, call, transport, digest('a'), std::move(request));
    ASSERT_TRUE(rejected);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*rejected));
    EXPECT_EQ(std::get<sp::Failure>(*rejected).error.kind, sp::ErrorKind::InvalidRequest);
    EXPECT_EQ(transport->calls, 0);
    EXPECT_FALSE(journal.inspect(call.owner, call.id()));
}

TEST(ProgramProviderJournal, DurableFailureRetainsPartialPartsUnknownUsageAndDeliveryEvidence) {
    Database db("failure-outcome");
    Call call;
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::Truncated;
    failure.error.safe_message = "stream ended after partial output";
    failure.error.retry_class = sp::RetryClass::Transient;
    failure.error.retry_safety = sp::RetrySafety::OutputObserved;
    failure.error.http_status = 200;
    failure.error.vendor_code = "partial_stream";
    failure.error.retry_after = std::chrono::milliseconds(17);
    failure.error.attempt = {true, 123, true, 1, 2, true};
    failure.partial.messages = {
        {"partial", sp::Role::Assistant, {sp::Text{"prefix"},
            sp::InvalidToolCall{"broken-call", "lookup", sp::ToolCallKind::ClientExecuted,
                                R"({"unfinished":)", sp::InvalidReason::Truncated}}}};
    failure.partial.usage = neograph::test::usage(std::nullopt, 0, std::nullopt,
                                                sp::UsageStage::Partial);
    failure.partial.usage.reasoning = sp::Count{0, sp::Evidence::Derived};
    failure.partial.usage.quality = sp::UsageQuality::Inconsistent;
    failure.partial.usage.conflicts.push_back({"total", "provider partial report conflict"});
    failure.partial.stop = sp::StopReason{sp::StopKind::MaxTokens, "length"};
    failure.partial.raw_events = {
        {"unknown.before-truncation", neograph::test::document(R"({"marker":1,"future":null})")},
        {"unknown.after-partial", neograph::test::document(R"({"marker":2,"tail":[true,7]})")}};
    const auto expected = std::make_shared<const sp::Outcome>(failure);
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto transport = std::make_shared<neograph::test::LocalProvider>(
        [calls, expected](auto, const auto&, const auto& on_event)
                          -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            if (on_event) on_event(sp::UsageUpdate{std::get<sp::Failure>(*expected).partial.usage});
            co_return expected;
        });
    auto events = std::make_shared<std::atomic<int>>(0);
    auto request = call.params();
    request.mode = ProviderMode::Stream;
    request.on_event = [events](const sp::Event&) { ++*events; };
    {
        Journal journal(db.path);
        const auto observed = invoke(journal, call, transport, digest('a'), request);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*observed));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->state,
                  Journal::State::ReconciliationRequired);
    }
    Journal reopened(db.path);
    call.attempt = 2;
    const auto restored = invoke(reopened, call, transport, digest('a'), request);
    ASSERT_TRUE(restored);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*restored));
    const auto& actual = std::get<sp::Failure>(*restored);
    EXPECT_EQ(actual.error.retry_safety, sp::RetrySafety::OutputObserved);
    EXPECT_TRUE(actual.error.attempt.request_may_have_left);
    EXPECT_EQ(actual.error.attempt.request_body_bytes, 123);
    EXPECT_EQ(actual.error.attempt.transport_internal_resends, 1U);
    EXPECT_EQ(actual.error.attempt.attempts, 2U);
    EXPECT_TRUE(actual.error.attempt.prior_usage_unknown);
    ASSERT_EQ(actual.partial.raw_events.size(), 2U);
    EXPECT_EQ(actual.partial.raw_events[0].type, "unknown.before-truncation");
    EXPECT_EQ(actual.partial.raw_events[1].type, "unknown.after-partial");
    EXPECT_EQ(actual.partial.raw_events[0].payload->root().dump(),
              failure.partial.raw_events[0].payload->root().dump());
    EXPECT_EQ(actual.partial.raw_events[1].payload->root().dump(),
              failure.partial.raw_events[1].payload->root().dump());
    EXPECT_FALSE(actual.partial.usage.input_total);
    ASSERT_TRUE(actual.partial.usage.output_total);
    EXPECT_EQ(actual.partial.usage.output_total->value, 0U);
    EXPECT_FALSE(actual.partial.usage.total);
    EXPECT_EQ(actual.partial.usage.quality, sp::UsageQuality::Inconsistent);
    EXPECT_EQ(std::get<sp::InvalidToolCall>(actual.partial.messages.at(0).parts.at(1)).raw_fragment,
              R"({"unfinished":)");
    EXPECT_EQ(neograph::program::detail::canonical_json_bytes(provider_codec::observe_outcome(*restored)),
              neograph::program::detail::canonical_json_bytes(provider_codec::observe_outcome(*expected)));
    EXPECT_EQ(calls->load(), 1);
    EXPECT_EQ(events->load(), 1) << "replay must not replay borrowed stream events";
}

#ifndef _WIN32
TEST(ProgramProviderJournal, TrustedArchiveRestoresSealedNativeToolHistoryAndRejectsProjectionForgery) {
    Database db("native-history");
    const auto root = std::filesystem::path(db.path + ".custody");
    struct Custody {
        std::filesystem::path path;
        ~Custody() { std::error_code error; std::filesystem::remove_all(path, error); }
    } custody{root};
    std::filesystem::create_directory(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    auto descriptor = neograph::test::descriptor("anthropic.messages");
    sp::messages::Request payload;
    payload.model = "model";
    payload.max_tokens = 64;
    payload.account_scope = "tenant:a";
    payload.messages = {neograph::test::message("question", sp::Role::User)};
    payload.tools.push_back({"lookup", "Look up a value",
                            neograph::test::document(R"({"type":"object"})")});
    auto wire = sp::messages::encode(descriptor, payload, false);
    ASSERT_TRUE(std::holds_alternative<sp::messages::EncodedRequest>(wire));
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator,
                               std::get<sp::messages::EncodedRequest>(wire).context);
    ASSERT_TRUE(codec.buffered(R"({"id":"sealed-message","type":"message",
        "role":"assistant","model":"model","content":[
        {"type":"thinking","thinking":"signed consideration","signature":"native-signature"},
        {"type":"redacted_thinking","data":"native-redacted"},
        {"type":"tool_use","id":"native-call","name":"lookup","input":{"key":"value"}},
        {"type":"text","text":"checking"}],"stop_reason":"tool_use","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3}})", {true, sp::ErrorKind::Truncated}));
    codec.finish();
    ASSERT_TRUE(accumulator.outcome());
    const auto expected = std::make_shared<const sp::Outcome>(*accumulator.outcome());
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*expected));
    ASSERT_TRUE(std::get<sp::Completion>(*expected).messages.at(0).native);
    auto provisioned = sp::NativeArchive::provision(
        (root / "archive").string(), (root / "key").string(), "tenant:a", descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(provisioned));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(provisioned));
    class NativeTransport final : public Provider {
    public:
        NativeTransport(sp::descriptor::ValidatedDescriptor descriptor,
                        sp::runtime::Result result,
                        std::shared_ptr<std::atomic<int>> calls)
            : client_(std::make_shared<sp::runtime::Client>(std::move(descriptor))),
              result_(std::move(result)), calls_(std::move(calls)) {}
        std::string get_name() const override { return "native-transport"; }
        std::string_view family() const noexcept override { return "anthropic.messages"; }
        PreparedProviderRequest prepare(ProviderRequest request) override {
            return prepare_local(client_, std::move(request),
                [result = result_, calls = calls_](const auto&, const auto&)
                    -> asio::awaitable<sp::runtime::Result> {
                    ++*calls;
                    co_return result;
                });
        }
    private:
        std::shared_ptr<sp::runtime::Client> client_;
        sp::runtime::Result result_;
        std::shared_ptr<std::atomic<int>> calls_;
    };
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto transport = std::make_shared<NativeTransport>(descriptor, expected, calls);
    Call call;
    ProviderRequest request;
    request.payload = payload;
    {
        Journal journal(db.path, archive);
        ASSERT_TRUE(invoke(journal, call, transport, digest('a'), request));
    }
    archive.reset();
    auto opened = sp::NativeArchive::open(
        (root / "archive").string(), (root / "key").string(), "tenant:a", descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(opened));
    archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(opened));
    Journal reopened(db.path, archive);
    call.attempt = 2;
    const auto restored = invoke(reopened, call, transport, digest('a'), request);
    const auto& message = std::get<sp::Completion>(*restored).messages.at(0);
    ASSERT_TRUE(message.native);
    EXPECT_EQ(neograph::program::detail::canonical_json_bytes(provider_codec::encode_message(message)),
              neograph::program::detail::canonical_json_bytes(provider_codec::encode_message(
                  std::get<sp::Completion>(*expected).messages.at(0))));
    EXPECT_THROW(provider_codec::decode_message(provider_codec::encode_message(message)),
                 std::runtime_error);
    payload.messages.push_back(message);
    payload.messages.push_back(sp::Message{"tool-result", sp::Role::User,
                                           {sp::ToolResult{"native-call", "observation"}}});
    request.payload = payload;
    auto continuation = transport->prepare(request);
    ASSERT_TRUE(continuation.valid());
    const auto body = json::parse(continuation.encoded_body());
    EXPECT_EQ(body.at("messages").at(1).at("content").at(0).at("signature"), "native-signature");
    EXPECT_EQ(body.at("messages").at(1).at("content").at(1).at("data"), "native-redacted");
    EXPECT_EQ(body.at("messages").at(1).at("content").at(2).at("id"), "native-call");
    EXPECT_EQ(body.at("messages").at(2).at("content").at(0).at("tool_use_id"), "native-call");
    EXPECT_EQ(calls->load(), 1) << "durable restore and continuation preparation perform no dispatch";
    std::get<sp::Thinking>(payload.messages.at(1).parts.at(0)).text = "edited";
    request.payload = std::move(payload);
    auto tampered = transport->prepare(std::move(request));
    ASSERT_FALSE(tampered.valid());
    ASSERT_NE(tampered.error(), nullptr);
    EXPECT_EQ(tampered.error()->kind, sp::ErrorKind::ReplayIneligible);
    EXPECT_EQ(calls->load(), 1);
}

void prove_native_program_failure_custody(bool ordinary_failure) {
    Database provider_db("program-native-fault");
    Database program_db("program-native-result");
    const std::string owner = "tenant:native-program-failure";
    const auto root = std::filesystem::path(provider_db.path + ".custody");
    struct Custody {
        std::filesystem::path path;
        ~Custody() { std::error_code error; std::filesystem::remove_all(path, error); }
    } custody{root};
    std::filesystem::create_directory(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    auto policy_source = json::parse(sp::config_defaults::descriptor_policy_json);
    json defaults;
    for (const auto& family : policy_source.at("families"))
        if (family.at("family") == "anthropic.messages") defaults = family.at("defaults");
    defaults["max_output_tokens"] = nullptr;
    policy_source.at("models").push_back(
        json{{"family", "anthropic.messages"}, {"model", "program-native-fault"},
             {"defaults", std::move(defaults)}, {"input_limit", 64U}, {"output_limit", 64U}});
    auto admitted_policy = sp::descriptor::load_policy(
        policy_source.dump(), sp::config_defaults::codec_defaults_json);
    ASSERT_TRUE(std::holds_alternative<sp::descriptor::PolicySnapshot>(admitted_policy));
    const auto descriptor = neograph::test::descriptor(
        "anthropic.messages", "https://fixture.invalid",
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted_policy)));
    sp::messages::Request payload;
    payload.model = "program-native-fault";
    payload.max_tokens = 64;
    payload.account_scope = owner;
    payload.messages = {neograph::test::message("question", sp::Role::User)};
    payload.tools.push_back({"lookup", "Look up a value",
                             neograph::test::document(R"({"type":"object"})")});
    const auto wire = sp::messages::encode(descriptor, payload, false);
    ASSERT_TRUE(std::holds_alternative<sp::messages::EncodedRequest>(wire));
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator,
                               std::get<sp::messages::EncodedRequest>(wire).context);
    const auto accepted = codec.buffered(R"({"id":"program-native-observation","type":"message",
        "role":"assistant","model":"program-native-fault","content":[
        {"type":"thinking","thinking":"actual signed observation","signature":"fixture-signature"},
        {"type":"redacted_thinking","data":"fixture-redacted"},
        {"type":"tool_use","id":"sealed-call","name":"lookup","input":{"key":"value"}},
        {"type":"text","text":"checking"}],"stop_reason":"tool_use","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3},"future":{"marker":[null,true,7]}})",
        {!ordinary_failure, sp::ErrorKind::Transport});
    EXPECT_EQ(accepted, !ordinary_failure);
    codec.finish();
    ASSERT_TRUE(accumulator.outcome());
    sp::runtime::Result actual = std::make_shared<const sp::Outcome>(std::move(*accumulator.take_outcome()));
    ASSERT_EQ(std::holds_alternative<sp::Failure>(*actual), ordinary_failure);
    ASSERT_TRUE(outcome_messages(*actual).front().native);
    if (ordinary_failure) {
        EXPECT_EQ(std::get<sp::Failure>(*actual).error.kind, sp::ErrorKind::Transport);
        EXPECT_FALSE(outcome_messages(*actual).front().native->complete());
    }
    ProgramResultData public_data;
    public_data.status = ProgramTerminalStatus::Failed;
    public_data.run_id = "native-public-create-probe";
    public_data.program_version_id = digest('e');
    public_data.bundle_id = digest('f');
    public_data.attempt = 1;
    public_data.failure = ProgramFailure{"P_NATIVE_PROBE", "native custody probe", "root", "", 0,
                                        json::object(), actual};
    for (const auto& reference : {json(nullptr), json("spna3:forged-reference")}) {
        public_data.failure->witness["provider_outcome"] = provider_codec::observe_outcome(*actual);
        public_data.failure->witness["provider_outcome"]["native_archive_reference"] = reference;
        EXPECT_THROW(ProgramResult::create(public_data), std::runtime_error);
    }
    auto provisioned = sp::NativeArchive::provision(
        (root / "archive").string(), (root / "key").string(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(provisioned));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(provisioned));
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    class NativeTransport final : public Provider {
    public:
        NativeTransport(sp::descriptor::ValidatedDescriptor descriptor, sp::runtime::Result result,
                        std::shared_ptr<std::atomic<unsigned>> calls)
            : client_(std::make_shared<sp::runtime::Client>(std::move(descriptor))),
              result_(std::move(result)), calls_(std::move(calls)) {}
        std::string get_name() const override { return "native-program-transport"; }
        std::string_view family() const noexcept override { return "anthropic.messages"; }
        PreparedProviderRequest prepare(ProviderRequest request) override {
            return prepare_local(client_, std::move(request),
                [result = result_, calls = calls_](const auto&, const auto&)
                    -> asio::awaitable<sp::runtime::Result> {
                    ++*calls;
                    co_return result;
                });
        }
    private:
        std::shared_ptr<sp::runtime::Client> client_;
        sp::runtime::Result result_;
        std::shared_ptr<std::atomic<unsigned>> calls_;
    };
    auto provider = std::make_shared<NativeTransport>(descriptor, actual, calls);
    ProviderRequest request;
    request.payload = payload;
    const auto prepared = provider->prepare(request);
    ASSERT_TRUE(prepared.valid());
    const auto claim = Provider::conservative_token_upper_bound(prepared);
    ASSERT_TRUE(claim && *claim > 0);
    auto provider_journal = std::make_shared<Journal>(provider_db.path, archive);
    if (!ordinary_failure) {
        SqlConnection connection;
        ASSERT_EQ(sqlite3_open(provider_db.path.c_str(), &connection.db), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(connection.db,
            "CREATE TRIGGER reject_native_settlement BEFORE UPDATE OF completion "
            "ON ng_program_provider_calls BEGIN "
            "SELECT RAISE(FAIL,'actual settlement storage fault'); END",
            nullptr, nullptr, nullptr), SQLITE_OK);
    }
    class NativeBrokerNode final : public graph::GraphNode {
    public:
        NativeBrokerNode(std::shared_ptr<Provider> provider, ProviderRequest request)
            : provider_(std::move(provider)), request_(std::move(request)) {}
        std::string get_name() const override { return "work"; }
        asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
            const auto broker = graph::provider_call_broker(in.ctx);
            if (!broker) throw std::runtime_error("Native Program provider broker is absent");
            auto result = co_await graph::observe_provider_result(
                in.ctx, broker->invoke(graph::make_provider_call_identity(in.ctx, get_name()),
                                       provider_, request_));
            graph::record_usage(in.ctx, result);
            graph::NodeOutput output;
            output.writes.push_back(graph::ChannelWrite{
                "value", neograph::test::text(outcome_or_throw(result))});
            co_return output;
        }
    private:
        std::shared_ptr<Provider> provider_;
        ProviderRequest request_;
    };
    const ExecutableIdentity provider_identity{
        ExecutableKind::Provider, provider->get_name(), "1.0.0", digest('a')};
    RegistrySnapshotBuilder registry_builder;
    registry_builder.add_provider(
        ExecutableManifest{provider_identity, EffectMode::Brokered, "native-program-provider", {}, {}, {}},
        ProviderMetadata{json::object(), json::object()});
    registry_builder.add_host_brokered_node(
        ExecutableManifest{{ExecutableKind::Node, "native-program-node", "1.0.0", digest('b')},
                           EffectMode::Brokered, "native-program-node", {}, {}, {provider_identity}},
        [request](const std::string&, const json&, const graph::NodeContext& context) {
            return std::make_unique<NativeBrokerNode>(context.provider, request);
        }, json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
    registry_builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "native-overwrite", "1.0.0", digest('c')},
                           EffectMode::Brokered, "native-reducer", {}, {}, {}},
        [](const json&, const json& value) { return value; });
    const auto registry = std::move(registry_builder).build();
    AdmissionProfileBuilder profile_builder;
    profile_builder.id("native-program-profile").semantic_version("1.0.0").registry(registry)
        .mode(AdmissionMode::MultiTenant).max_program_schema_version(2)
        .allow_source_kind(SourceKind::CppBuilder).allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) profile_builder.allow_executable(identity);
    const auto profile = std::move(profile_builder).build();
    PolicySnapshotBuilder program_policy_builder;
    program_policy_builder.id("native-program-policy").semantic_version("1.0.0")
        .owner_scope(owner).admission_profile(profile)
        .budget_ceiling(BudgetLimits{10000, 1000000, 1, 1, 4, 20, 1, 0, 0});
    const auto program_policy = std::move(program_policy_builder).build();
    auto store = std::make_shared<SQLiteProgramStore>(program_db.path);
    auto transitions = std::make_shared<SQLiteProgramTransitionStore>(program_db.path);
    const auto make_catalog = [&] {
        CatalogConfig config{store, registry, std::make_shared<EngineGenerationCache>(),
                             "native-program-fixture/v1"};
        config.capability_binder = [provider, provider_identity](const auto&) {
            CatalogCapabilityBinding binding;
            binding.node_context.provider = provider;
            binding.receipts = {{provider_identity, digest('d')}};
            return binding;
        };
        return std::make_shared<ProgramCatalog>(std::move(config));
    };
    auto catalog = make_catalog();
    const json definition{
        {"schema_version", 1}, {"name", "main"},
        {"channels", {{"value", {{"reducer", "native-overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"work", {{"type", "native-program-node"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                              json{{"from", "work"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()}};
    json document{
        {"program_schema_version", 2U},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "retry"}, {"name", "main"}, {"definition", definition},
                  {"max_attempts", 3U}, {"body", {{"op", "call_core"}}}}},
        {"declared_budget_requirements", json::array({
            json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
            json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000000}},
            json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
            json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
            json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 4}},
            json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
            json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
            json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
            json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
    ProgramCompiler compiler(registry, {"native-program-fixture/v1"});
    const auto bundle = compiler.compile(ProgramSource::from_cpp_builder(
        "native-program-failure", 2, std::move(document)));
    const auto version = catalog->admit(bundle, ProgramAdmission{owner, profile, program_policy, {}});
    const auto make_runtime = [&](std::shared_ptr<sp::NativeArchive> custody_archive) {
        RuntimeConfig config;
        config.catalog = catalog;
        config.checkpoints = std::make_shared<graph::InMemoryCheckpointStore>();
        config.transitions = transitions;
        config.native_history_archive = std::move(custody_archive);
        config.require_core_provider_call_broker = true;
        config.core_provider_call_resolver = [provider_journal](const auto& context) {
            return provider_journal->bind(context, digest('a'));
        };
        return std::make_unique<ProgramRuntime>(std::move(config));
    };
    auto runtime = make_runtime(archive);
    const auto result = runtime->run(owner, version,
        ProgramInvocation{json::object(), RunBudget{10000, *claim + 64, 0, 1, 4, 20, 0, 0, 0},
                          "native-program-fault", {}});
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(result.failure() && result.failure()->provider_outcome && result.failure()->provider_cause);
    EXPECT_EQ(result.failure()->provider_outcome, actual);
    EXPECT_EQ(calls->load(), 1U);
    std::string original_storage_message;
    try {
        std::rethrow_exception(result.failure()->provider_cause);
    } catch (const ProviderFailure& error) {
        EXPECT_TRUE(ordinary_failure);
        EXPECT_EQ(error.outcome(), actual);
        original_storage_message = error.what();
    } catch (const ProgramProviderOutcomePersistenceError& error) {
        EXPECT_FALSE(ordinary_failure);
        EXPECT_EQ(error.outcome(), actual);
        try {
            std::rethrow_exception(error.cause());
        } catch (const std::exception& storage_error) {
            original_storage_message = storage_error.what();
        }
    }
    if (!ordinary_failure) {
        EXPECT_NE(original_storage_message.find("actual settlement storage fault"), std::string::npos);
    }
    ASSERT_TRUE(result.provider_budget_authority());
    EXPECT_EQ(result.provider_budget_authority()->charged, 0U);
    EXPECT_EQ(result.provider_budget_authority()->reserved, *claim);
    EXPECT_TRUE(result.provider_budget_authority()->has_report);
    EXPECT_FALSE(result.provider_budget_authority()->reports.input_total);
    EXPECT_FALSE(result.provider_budget_authority()->reports.total);
    const auto stored_bytes = result.serialize_canonical();
    auto reused = *result.failure();
    const auto original_custody = reused.witness.at("provider_outcome_custody");
    EXPECT_THROW(neograph::program::detail::persist_provider_failure(reused, original_custody, {}),
                 std::invalid_argument);
    auto wrong_owner = sp::NativeArchive::provision(
        (root / "wrong-owner-archive").string(), (root / "wrong-owner-key").string(),
        "tenant:other-owner", descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(wrong_owner));
    EXPECT_THROW(neograph::program::detail::persist_provider_failure(
        reused, original_custody, std::get<std::shared_ptr<sp::NativeArchive>>(wrong_owner)),
        std::invalid_argument);
    public_data.failure->witness["provider_outcome"] = result.failure()->witness.at("provider_outcome");
    EXPECT_THROW(ProgramResult::create(public_data), std::runtime_error);
    EXPECT_THROW(ProgramResult::parse(stored_bytes).failure(), std::invalid_argument);
    runtime.reset();
    catalog.reset();
    transitions = std::make_shared<SQLiteProgramTransitionStore>(program_db.path);
    store = std::make_shared<SQLiteProgramStore>(program_db.path);
    catalog = make_catalog();
    auto reopened_archive = sp::NativeArchive::open(
        (root / "archive").string(), (root / "key").string(), owner, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(reopened_archive));
    archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(reopened_archive));
    runtime = make_runtime(archive);
    const auto restored = runtime->reconnect(owner, result.run_id()).wait();
    ASSERT_TRUE(restored.failure() && restored.failure()->provider_outcome);
    EXPECT_FALSE(restored.failure()->provider_cause);
    const auto factual_cause = ordinary_failure
        ? restored.failure()->witness.at("provider_error").at("message")
        : restored.failure()->witness.at("provider_error").at("cause").at("message");
    EXPECT_EQ(factual_cause, original_storage_message);
    const auto snapshot = runtime->reconnect(owner, result.run_id()).snapshot();
    ASSERT_TRUE(snapshot.terminal_result() && snapshot.terminal_result()->failure());
    EXPECT_EQ(provider_codec::observe_outcome(
                  *snapshot.terminal_result()->failure()->provider_outcome),
              provider_codec::observe_outcome(*actual));
    EXPECT_EQ(restored.id(), result.id());
    EXPECT_EQ(provider_codec::observe_outcome(*restored.failure()->provider_outcome),
              provider_codec::observe_outcome(*actual));
    ASSERT_TRUE(restored.provider_budget_authority());
    EXPECT_EQ(restored.provider_budget_authority()->reserved, *claim);
    EXPECT_EQ(restored.provider_budget_authority()->provider_effects,
              result.provider_budget_authority()->provider_effects);
    const auto& restored_outcome = *restored.failure()->provider_outcome;
    const auto& native_message = std::holds_alternative<sp::Completion>(restored_outcome)
        ? std::get<sp::Completion>(restored_outcome).messages.front()
        : std::get<sp::Failure>(restored_outcome).partial.messages.front();
    ASSERT_TRUE(native_message.native);
    EXPECT_THROW(provider_codec::decode_message(provider_codec::encode_message(native_message)),
                 std::runtime_error);
    auto forgery = provider_codec::encode_message(neograph::test::message("forged native authority"));
    forgery["native"] = true;
    EXPECT_THROW(provider_codec::decode_message(forgery), std::runtime_error);
    runtime.reset();
    runtime = make_runtime({});
    EXPECT_THROW(runtime->reconnect(owner, result.run_id()), std::invalid_argument);
    const auto nonpersistable = runtime->run(owner, version,
        ProgramInvocation{json::object(), RunBudget{10000, *claim + 64, 0, 1, 4, 20, 0, 0, 0},
                          "native-program-without-result-custody", {}});
    ASSERT_EQ(nonpersistable.status(), ProgramTerminalStatus::Failed);
    ASSERT_TRUE(nonpersistable.failure() && nonpersistable.failure()->provider_cause);
    EXPECT_EQ(nonpersistable.failure()->provider_outcome, actual);
    EXPECT_THROW(nonpersistable.serialize_canonical(), std::runtime_error);
    const auto uncommitted = transitions->load(owner, nonpersistable.run_id());
    ASSERT_TRUE(uncommitted);
    EXPECT_FALSE(uncommitted->terminal_result());
    auto rejected_replay = uncommitted->invocation();
    rejected_replay.run_id = "cannot-replay-unpersisted-native-failure";
    EXPECT_THROW(runtime->replay_recorded(
        nonpersistable.run_id(), rejected_replay,
        RecordedBindingSet(version.core_materialization_receipt().capability_bindings, {}, {})),
        ProgramDiagnosticError);
    auto changed_descriptor = neograph::test::descriptor(
        "anthropic.messages", "https://other.fixture.invalid");
    const auto changed = sp::NativeArchive::open(
        (root / "archive").string(), (root / "key").string(), owner, changed_descriptor);
    EXPECT_TRUE(std::holds_alternative<sp::Error>(changed));
    EXPECT_EQ(calls->load(), 2U);
}

TEST(ProgramProviderJournal, NativeProgramSettlementFailureRetainsRealOutcomeAndReopensAuthenticatedCustody) {
    prove_native_program_failure_custody(false);
}

TEST(ProgramProviderJournal, NativeProgramOrdinaryFailureStopsOuterRetriesAndReopensPartialEvidence) {
    prove_native_program_failure_custody(true);
}
#endif

TEST(ProgramProviderJournal, ReopenedUnknownEffectRetainsReservationWithoutInventingReport) {
    Database db("unknown-effect-custody");
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto provider = std::make_shared<neograph::test::LocalProvider>(
        [calls](auto, const auto&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            ++*calls;
            throw std::runtime_error("transport disconnected after send without an outcome");
            co_return nullptr;
        }, "unknown-effect-provider", neograph::test::bounded_client());
    auto request = neograph::test::request("test-model", "question");
    std::get<sp::chat::Request>(request.payload).max_output_tokens = 1;
    Call call;
    auto invoke_bounded = [&](Journal& journal, const std::shared_ptr<UsageAccumulator>& bank) {
        auto identity = call.identity();
        identity.usage = bank;
        identity.model_token_budget = 2;
        identity.budget_exhausted = std::make_shared<std::atomic_bool>(false);
        identity.budget_cancel_token = std::make_shared<neograph::graph::CancelToken>();
        auto binding = journal.bind(call.context(), digest('a'));
        return neograph::async::run_sync(
            binding.broker->invoke(std::move(identity), provider, request));
    };
    auto original = std::make_shared<UsageAccumulator>();
    {
        Journal journal(db.path);
        EXPECT_THROW(invoke_bounded(journal, original), std::runtime_error);
        ASSERT_TRUE(journal.inspect(call.owner, call.id()));
        EXPECT_EQ(journal.inspect(call.owner, call.id())->claim_amount, 2U);
    }
    const auto held = original->authority_snapshot();
    EXPECT_EQ(held.charged, 0U);
    EXPECT_EQ(held.reserved, 2U);
    EXPECT_FALSE(held.has_report);
    EXPECT_FALSE(held.reports.input_total);
    EXPECT_FALSE(held.reports.output_total);
    EXPECT_EQ(held.provider_effects.size(), 1U);
    call.attempt = 2;
    auto resumed = std::make_shared<UsageAccumulator>();
    {
        Journal reopened(db.path);
        EXPECT_THROW(invoke_bounded(reopened, resumed), std::runtime_error);
        const auto restored = resumed->authority_snapshot();
        EXPECT_EQ(restored.charged, 0U);
        EXPECT_EQ(restored.reserved, 2U);
        EXPECT_EQ(restored.provider_effects, held.provider_effects);
        EXPECT_FALSE(restored.has_report);
        EXPECT_FALSE(restored.reports.input_total);
        EXPECT_FALSE(restored.reports.output_total);
        EXPECT_EQ(calls->load(), 1U);
        EXPECT_THROW(invoke_bounded(reopened, resumed), std::runtime_error);
        EXPECT_EQ(resumed->authority_snapshot().reserved, 2U);
        EXPECT_EQ(resumed->authority_snapshot().provider_effects, held.provider_effects);
        EXPECT_EQ(calls->load(), 1U);
    }
}

TEST(ProgramProviderJournal, ReopenedCheckpointPrefixHydratesNonrenewableClaimsExactlyOnce) {
    Database db("claim-hydration");
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    const auto unknown = neograph::test::success("partial-usage",
        neograph::test::usage(std::nullopt, 0, std::nullopt, sp::UsageStage::Partial));
    auto zero_usage = neograph::test::usage(0, 0, 0);
    zero_usage.input_uncached = sp::Count{0};
    zero_usage.cache_read = sp::Count{0};
    zero_usage.cache_write = sp::Count{0};
    const auto zero = neograph::test::success("known-zero", std::move(zero_usage));
    auto retried_completion = neograph::test::completion(zero);
    retried_completion.attempt = {true, 91, true, 1, 2, true};
    retried_completion.raw_events = {
        {"unknown.retry-attempt", neograph::test::document(R"({"attempt":1,"usage":null})")},
        {"unknown.final-attempt", neograph::test::document(R"({"attempt":2,"usage":0})")}};
    const auto prior_unknown =
        std::make_shared<const sp::Outcome>(std::move(retried_completion));
    auto provider = std::make_shared<neograph::test::LocalProvider>(
        [calls, unknown, zero, prior_unknown](auto, const auto&, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            const auto index = calls->fetch_add(1);
            co_return index == 0 ? unknown : index == 1 ? zero : prior_unknown;
        }, "claim-provider", neograph::test::bounded_client());
    auto request = neograph::test::request("test-model", "question");
    std::get<sp::chat::Request>(request.payload).max_output_tokens = 1;
    Call first;
    Call second = first;
    second.task = "s2:reason";
    auto original_usage = std::make_shared<UsageAccumulator>();
    auto invoke_bounded = [&](Journal& journal, const Call& call,
                              const std::shared_ptr<UsageAccumulator>& usage) {
        auto identity = call.identity();
        identity.usage = usage;
        identity.model_token_budget = 4;
        identity.budget_exhausted = std::make_shared<std::atomic_bool>(false);
        identity.budget_cancel_token = std::make_shared<neograph::graph::CancelToken>();
        auto resolved = journal.bind(call.context(), digest('a'));
        return neograph::async::run_sync(
            resolved.broker->invoke(std::move(identity), provider, request));
    };
    {
        Journal journal(db.path);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(
            *invoke_bounded(journal, first, original_usage)));
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(
            *invoke_bounded(journal, second, original_usage)));
        ASSERT_TRUE(journal.inspect(first.owner, first.id()));
        EXPECT_EQ(journal.inspect(first.owner, first.id())->claim_amount, 2U);
        EXPECT_EQ(journal.inspect(first.owner, first.id())->committed_amount, 2U);
        ASSERT_TRUE(journal.inspect(second.owner, second.id()));
        EXPECT_EQ(journal.inspect(second.owner, second.id())->claim_amount, 2U);
        EXPECT_EQ(journal.inspect(second.owner, second.id())->committed_amount, 0U);
        EXPECT_EQ(original_usage->total_tokens_wide(), 2U);
        const auto authority = original_usage->authority_snapshot();
        EXPECT_EQ(authority.charged, 0U);
        EXPECT_EQ(authority.reserved, 2U);
        EXPECT_TRUE(authority.has_report);
        EXPECT_EQ(authority.reports.stage, sp::UsageStage::Partial);
    }
    auto resumed_usage = std::make_shared<UsageAccumulator>();
    Call next = first;
    next.attempt = 2;
    next.task = "s3:reason";
    Call exhausted = next;
    exhausted.task = "s4:reason";
    {
        Journal reopened(db.path);
        // The checkpoint already skipped both prefix calls: invoking only this
        // new slot must hydrate their durable claims before reserving its effect.
        const auto next_result = invoke_bounded(reopened, next, resumed_usage);
        ASSERT_TRUE(next_result);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*next_result));
        const auto& retried = std::get<sp::Completion>(*next_result);
        EXPECT_TRUE(retried.attempt.prior_usage_unknown);
        EXPECT_EQ(retried.usage.stage, sp::UsageStage::Final);
        ASSERT_TRUE(retried.usage.total);
        EXPECT_EQ(retried.usage.total->value, 0U);
        ASSERT_TRUE(reopened.inspect(next.owner, next.id()));
        EXPECT_EQ(reopened.inspect(next.owner, next.id())->committed_amount, 2U)
            << "a known-zero final attempt cannot refund unknown earlier delivery";
        EXPECT_EQ(resumed_usage->total_tokens_wide(), 4U);
        EXPECT_FALSE(resumed_usage->snapshot().input_total);
        EXPECT_FALSE(resumed_usage->snapshot().total);
        const auto authority = resumed_usage->authority_snapshot();
        EXPECT_EQ(authority.charged, 0U);
        EXPECT_EQ(authority.reserved, 4U);
        EXPECT_TRUE(authority.has_report);
        EXPECT_EQ(authority.reports.stage, sp::UsageStage::Partial);
        ASSERT_TRUE(authority.reports.output_total);
        EXPECT_EQ(authority.reports.output_total->value, 0U);
        EXPECT_FALSE(authority.reports.input_total);
        EXPECT_FALSE(authority.reports.cache_read);
        EXPECT_FALSE(authority.reports.cache_write);
        EXPECT_EQ(authority.provider_effects.size(), 3U);
        const auto original_effects = original_usage->authority_snapshot().provider_effects;
        for (const auto& effect : original_effects)
            EXPECT_NE(std::find(authority.provider_effects.begin(),
                                authority.provider_effects.end(), effect),
                      authority.provider_effects.end());
        EXPECT_EQ(calls->load(), 3U);
        const auto denied = invoke_bounded(reopened, exhausted, resumed_usage);
        ASSERT_TRUE(denied);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*denied));
        EXPECT_EQ(calls->load(), 3U);
        EXPECT_FALSE(reopened.inspect(exhausted.owner, exhausted.id()));
    }
    {
        Journal reopened_again(db.path);
        exhausted.attempt = 3;
        const auto denied = invoke_bounded(reopened_again, exhausted, resumed_usage);
        ASSERT_TRUE(denied);
        ASSERT_TRUE(std::holds_alternative<sp::Failure>(*denied));
        EXPECT_EQ(resumed_usage->total_tokens_wide(), 4U)
            << "the same live sink must not charge hydrated receipts again";
        EXPECT_EQ(calls->load(), 3U);
        EXPECT_FALSE(reopened_again.inspect(exhausted.owner, exhausted.id()));
    }
}

#if !defined(_WIN32) && defined(NEOGRAPH_PROGRAM_TESTS_HAVE_LLM)
TEST(ProgramProviderJournal, ActualRuntimeNativeCustodyRejectsMissingAndMismatchedDescriptorBeforeReceipt) {
    Database db("native-admission");
    const auto root = std::filesystem::path(db.path + ".custody");
    struct Custody {
        std::filesystem::path path;
        ~Custody() { std::error_code error; std::filesystem::remove_all(path, error); }
    } custody{root};
    std::filesystem::create_directory(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    const auto admitted = neograph::test::descriptor("anthropic.messages");
    std::shared_ptr<Provider> runtime_provider =
        neograph::llm::SchemaProvider::create(admitted);
    ProviderControls controls;
    controls.max_output_tokens = 64;
    const auto request = make_provider_request(*runtime_provider, "model",
        {neograph::test::message("question", sp::Role::User)}, {}, controls);
    auto prepared = runtime_provider->prepare(request);
    ASSERT_TRUE(prepared.valid());
    ASSERT_TRUE(prepared.requires_native_custody());
    Call call;
    auto usage = std::make_shared<UsageAccumulator>();
    auto identity = call.identity();
    identity.usage = usage;
    identity.model_token_budget = 128;
    identity.budget_exhausted = std::make_shared<std::atomic_bool>(false);
    identity.budget_cancel_token = std::make_shared<neograph::graph::CancelToken>();
    {
        Journal missing(db.path);
        auto bound = missing.bind(call.context(), digest('a'));
        EXPECT_THROW(neograph::async::run_sync(
            bound.broker->invoke(identity, runtime_provider, request)),
            std::invalid_argument);
        EXPECT_FALSE(missing.inspect(call.owner, call.id()));
        EXPECT_EQ(usage->total_tokens_wide(), 0U);
        EXPECT_FALSE(usage->snapshot().total);
        EXPECT_FALSE(identity.budget_exhausted->load());
        EXPECT_FALSE(identity.budget_cancel_token->is_cancelled());
    }
    auto mismatch_descriptor = neograph::test::descriptor(
        "anthropic.messages", "https://other.fixture.invalid");
    auto provisioned = sp::NativeArchive::provision(
        (root / "archive").string(), (root / "key").string(), call.owner,
        std::move(mismatch_descriptor));
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(provisioned));
    Journal mismatch(db.path,
        std::get<std::shared_ptr<sp::NativeArchive>>(std::move(provisioned)));
    auto bound = mismatch.bind(call.context(), digest('a'));
    EXPECT_THROW(neograph::async::run_sync(
        bound.broker->invoke(identity, runtime_provider, request)),
        std::invalid_argument);
    EXPECT_FALSE(mismatch.inspect(call.owner, call.id()));
    EXPECT_EQ(usage->total_tokens_wide(), 0U);
    EXPECT_FALSE(usage->snapshot().total);
    EXPECT_FALSE(identity.budget_exhausted->load());
    EXPECT_FALSE(identity.budget_cancel_token->is_cancelled());
}
#endif
