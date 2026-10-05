#include <neograph/sqlite_runtime_stores.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/types.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/cancel.h>
#ifdef NEOGRAPH_SQLITE_TOOL_TESTS_HAVE_LLM
#include <neograph/llm/agent.h>
#endif
#include <neograph/tool_dispatch.h>
#include "fixtures/typed_provider.h"
#include <neograph/provider_outcome_codec.h>
#include <codecs/messages.h>
#include <core/native.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

using namespace neograph;
namespace {
std::string sha(char c) { return "sha256:" + std::string(64, c); }
std::string database(std::string_view name) {
    auto path = std::filesystem::temp_directory_path() / (std::string("neograph_") + std::string(name) + ".sqlite");
    std::error_code ec; std::filesystem::remove(path, ec); std::filesystem::remove(path.string() + "-wal", ec);
    return path.string();
}
RuntimeHistoryRecord record(std::uint64_t sequence, std::optional<std::string> predecessor = std::nullopt) {
    RuntimeHistoryRecordData d; d.feed_id = "feed"; d.sequence = sequence; d.message_id = "message_" + std::to_string(sequence);
    d.trust = RuntimeTrustClass::UntrustedInput; d.message = test::message("message", sp::Role::User); d.predecessor_id = std::move(predecessor);
    return RuntimeHistoryRecord::create(std::move(d));
}
HookInvocation invocation(const RuntimeEvent& event) {
    return HookInvocation::create({{}, sha('a'), event.id(), "audit", HookPhase::BeforeToolExecution,
        HookDelivery::BlockingMandatory, HookFailureMode::FailClosed, HookIdempotency::Idempotent,
        ToolEffectClass::ReadOnly, {}, {}, json::object()});
}
}

TEST(SQLiteRuntimeStores, ContextReopensWithOwnerIsolationAndCas) {
    const auto path = database("context_reopen"); const ContextStoreFeed owner{"owner_a", "feed"};
    const auto first = record(1);
    { SQLiteContextStore store(path); EXPECT_EQ(store.append_history(owner, first, {}), ContextStoreAppendResult::Appended); }
    SQLiteContextStore reopened(path);
    EXPECT_EQ(reopened.history_head(owner).record_id, first.id());
    ASSERT_TRUE(reopened.history_record_by_message_id(owner, "message_1"));
    EXPECT_EQ(reopened.history_record_by_message_id(owner, "message_1")->id(),
              first.id());
    EXPECT_FALSE(reopened.history_record_by_message_id(owner, "missing"));
    EXPECT_EQ(reopened.history_head({"owner_b", "feed"}).sequence, 0u);
    EXPECT_FALSE(reopened.history_record_by_message_id(
        {"owner_b", "feed"}, "message_1"));
    const auto second = record(2, first.id());
    std::atomic<int> wins{0}; std::vector<std::thread> workers;
    for (int i = 0; i != 6; ++i) workers.emplace_back([&] { if (reopened.append_history(owner, second, first.id()) == ContextStoreAppendResult::Appended) ++wins; });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(wins, 1); EXPECT_EQ(reopened.hydrate_history(reopened.snapshot_history(owner, 1, 2)), first.serialize_canonical() + "\n" + second.serialize_canonical());
    RuntimeHistoryRecordData duplicate_data;
    duplicate_data.feed_id = owner.feed_id;
    duplicate_data.sequence = 3;
    duplicate_data.message_id = "message_1";
    duplicate_data.trust = RuntimeTrustClass::UntrustedInput;
    duplicate_data.message = test::message("duplicate identity", sp::Role::User);
    duplicate_data.predecessor_id = second.id();
    EXPECT_EQ(reopened.append_history(
                  owner, RuntimeHistoryRecord::create(std::move(duplicate_data)),
                  second.id()),
              ContextStoreAppendResult::Conflict);
}

TEST(SQLiteRuntimeStores, HookLeaseSurvivesReopenAndFencesStaleWorker) {
    const auto path = database("hook_reopen"); const auto now = std::chrono::system_clock::now();
    const auto event = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json::object()});
    const auto call = invocation(event);
    const auto first = [&] {
        SQLiteHookJournal journal(path); journal.enqueue(call, event, 2, now + std::chrono::minutes(1)); journal.publish(call.id());
        return *journal.claim(call.id(), "first", now, std::chrono::milliseconds(1));
    }();
    SQLiteHookJournal reopened(path);
    const auto second = *reopened.claim(call.id(), "second", now + std::chrono::seconds(1), std::chrono::seconds(1));
    EXPECT_GT(second.fencing_token, first.fencing_token);
    const auto stale = HookExecutionReceipt::create({call.id(), first.entry.data().attempt_count, HookExecutionState::Succeeded, {"effect", true, true, {}}, {}});
    EXPECT_FALSE(reopened.settle(call.id(), first.fencing_token, stale, now + std::chrono::seconds(1)));
}

TEST(SQLiteRuntimeStores, ProviderReceiptReopensAndFailsClosedOnCorruption) {
    const auto path = database("receipt_reopen");
    const auto receipt = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('b'), sha('c'), "model", ProviderMode::Collect});
    const auto outcome = ProviderDispatchOutcomeReceipt::create(
        {"dispatch", receipt.id(), ProviderDispatchState::Succeeded, sha('d'), {}});
    { SQLiteProviderDispatchReceiptStore store(path);
      EXPECT_EQ(store.persist(receipt), ProviderDispatchReceiptPutResult::Stored);
      EXPECT_EQ(store.settle({}, outcome), ProviderDispatchOutcomePutResult::Stored); }
    SQLiteProviderDispatchReceiptStore reopened(path);
    EXPECT_EQ(reopened.persist(receipt), ProviderDispatchReceiptPutResult::AlreadyPresent);
    EXPECT_EQ(reopened.state("dispatch"), ProviderDispatchState::Succeeded);
    ASSERT_TRUE(reopened.outcome({}, "dispatch"));
    EXPECT_EQ(reopened.outcome({}, "dispatch")->id(), outcome.id());
    sqlite3* raw = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE ng_provider_receipts SET canonical='bad' WHERE dispatch_id='dispatch'", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_THROW(reopened.state("dispatch"), std::exception);
}

TEST(SQLiteRuntimeStores, ProviderReceiptsAreOwnerIsolated) {
    SQLiteProviderDispatchReceiptStore store(database("receipt_owner_isolation"));
    const auto first = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('b'), sha('c'), "model", ProviderMode::Collect});
    const auto second = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('d'), sha('e'), "model", ProviderMode::Collect});
    EXPECT_EQ(store.persist("owner_a", first), ProviderDispatchReceiptPutResult::Stored);
    EXPECT_EQ(store.persist("owner_b", second), ProviderDispatchReceiptPutResult::Stored);
    EXPECT_EQ(store.persist("owner_a", second), ProviderDispatchReceiptPutResult::Conflict);
    const auto outcome = ProviderDispatchOutcomeReceipt::create(
        {"dispatch", first.id(), ProviderDispatchState::Succeeded, sha('f'), {}});
    EXPECT_EQ(store.settle("owner_a", outcome), ProviderDispatchOutcomePutResult::Stored);
    EXPECT_EQ(store.settle("owner_b", outcome), ProviderDispatchOutcomePutResult::Conflict);
    EXPECT_EQ(store.settle("owner_c", outcome), ProviderDispatchOutcomePutResult::MissingDispatch);
    EXPECT_EQ(store.state("owner_a", "dispatch"), ProviderDispatchState::Succeeded);
    EXPECT_EQ(store.state("owner_b", "dispatch"), ProviderDispatchState::AdmittedPending);
    EXPECT_EQ(store.state("owner_c", "dispatch"), ProviderDispatchState::Missing);
}

TEST(SQLiteRuntimeStores, HookScansRejectSqlStateCorruption) {
    const auto path = database("hook_scan_corruption");
    const auto now = std::chrono::system_clock::now();
    const auto event = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json::object()});
    SQLiteHookJournal journal(path);
    const auto call = invocation(event);
    journal.enqueue(call, event, 1, now + std::chrono::minutes(1));
    journal.publish(call.id());
    sqlite3* raw = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE ng_hook_outbox SET state=2", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_THROW(journal.pending(), std::exception);
    EXPECT_THROW(journal.reconciliation_required(), std::exception);
}

TEST(SQLiteRuntimeStores, HistoryHeadAndSnapshotRejectSqlMetadataCorruption) {
    const auto path = database("history_metadata_corruption");
    const ContextStoreFeed feed{"owner", "feed"};
    SQLiteContextStore store(path);
    const auto first = record(1);
    ASSERT_EQ(store.append_history(feed, first, {}), ContextStoreAppendResult::Appended);
    sqlite3* raw = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE ng_runtime_history SET record_id='wrong'", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_THROW(store.history_head(feed), std::exception);
    EXPECT_THROW(store.snapshot_history(feed, 1, 1), std::exception);
}

namespace {
class ToolEffectTestFiles {
public:
    ToolEffectTestFiles() {
        static std::atomic<std::uint64_t> next{0};
        const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                         + "-" + std::to_string(++next);
        // Resolve the OS temp parent before creating paths subject to no-symlink custody.
        path_ = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                ("ng-tool-effect-" + token);
        if (!std::filesystem::create_directory(path_))
            throw std::runtime_error("cannot reserve Tool effect test directory");
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    ~ToolEffectTestFiles() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    ToolEffectTestFiles(const ToolEffectTestFiles&) = delete;
    ToolEffectTestFiles& operator=(const ToolEffectTestFiles&) = delete;
    std::string journal() const { return (path_ / "journal.sqlite").string(); }
    std::string ledger() const { return (path_ / "sdk.sqlite").string(); }
    std::string archive() const { return (path_ / "native-archive").string(); }
    std::string key() const { return (path_ / "archive.key").string(); }
private:
    std::filesystem::path path_;
};

using FixtureDb = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using FixtureStmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
FixtureDb fixture_db(const std::string& path) {
    sqlite3* raw = nullptr;
    const int status = sqlite3_open(path.c_str(), &raw);
    FixtureDb db(raw, sqlite3_close);
    if (status != SQLITE_OK) throw std::runtime_error("cannot open fixture SQLite database");
    return db;
}

sp::messages::Request native_request() {
    sp::messages::Request request;
    request.model = "fixture-model";
    request.account_scope = "fixture-account";
    request.messages = {test::message("message", sp::Role::User)};
    request.tools.push_back({"read", "read fixture",
                             test::document(R"({"type":"object"})"), {}, {}});
    return request;
}

sp::Message captured_native_message(const sp::descriptor::ValidatedDescriptor& descriptor) {
    const auto request = native_request();
    auto encoded = sp::messages::encode(descriptor, request, false);
    const auto context = std::get<sp::messages::EncodedRequest>(std::move(encoded)).context;
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator, context);
    codec.buffered(R"({"id":"msg-native","type":"message","role":"assistant","model":"fixture-model",
        "content":[{"type":"thinking","thinking":"inspect","signature":"fixture-signature"},
                   {"type":"tool_use","id":"call-native","name":"read","input":{"path":"a"}},
                   {"type":"text","text":"pending"}],
        "stop_reason":"tool_use","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3}})", {});
    codec.finish();
    return std::get<sp::Completion>(*accumulator.outcome()).messages.at(0);
}

RuntimeHistoryRecord native_record(const sp::Message& message, const RuntimeHistoryRecord& first) {
    RuntimeHistoryRecordData data;
    data.feed_id = first.feed_id();
    data.sequence = 2;
    data.message_id = "native-message";
    data.trust = RuntimeTrustClass::ModelOutput;
    data.message = message;
    data.predecessor_id = first.id();
    return RuntimeHistoryRecord::create(std::move(data));
}

class LedgerTool final : public Tool {
public:
    LedgerTool(std::string name, std::string ledger_path, bool throw_after_write = false,
               std::chrono::milliseconds delay = {},
               std::shared_ptr<graph::CancelToken> cancel_after_write = {},
               std::shared_ptr<std::atomic<bool>> signal_after_write = {},
               bool crash_after_write = false)
        : name_(std::move(name)), ledger_path_(std::move(ledger_path)),
          throw_after_write_(throw_after_write), delay_(delay),
          cancel_after_write_(std::move(cancel_after_write)),
          signal_after_write_(std::move(signal_after_write)),
          crash_after_write_(crash_after_write) {
        const auto db = fixture_db(ledger_path_);
        if (sqlite3_exec(db.get(), "CREATE TABLE IF NOT EXISTS sdk_effects "
                               "(id INTEGER PRIMARY KEY, payload TEXT NOT NULL)",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot create fixture effect ledger");
    }
    ChatTool get_definition() const override {
        ChatTool definition;
        definition.name = name_;
        definition.description = "External SQLite SDK write fixture";
        definition.parameters = json::object();
        return definition;
    }
    std::string get_name() const override { return name_; }
    std::string execute(const json& arguments) override {
        sqlite3_int64 effect_id;
        {
            const auto db = fixture_db(ledger_path_);
            sqlite3_stmt* raw_statement = nullptr;
            if (sqlite3_prepare_v2(db.get(), "INSERT INTO sdk_effects(payload) VALUES (?)",
                                   -1, &raw_statement, nullptr) != SQLITE_OK)
                throw std::runtime_error("fixture SDK prepare failed");
            FixtureStmt statement(raw_statement, sqlite3_finalize);
            const auto payload = arguments.dump();
            sqlite3_bind_text(statement.get(), 1, payload.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(statement.get()) != SQLITE_DONE)
                throw std::runtime_error("fixture SDK write failed");
            effect_id = sqlite3_last_insert_rowid(db.get());
        }
        if (signal_after_write_) signal_after_write_->store(true, std::memory_order_release);
#ifdef __linux__
        if (crash_after_write_) std::_Exit(77);
#endif
        if (delay_ != std::chrono::milliseconds::zero()) std::this_thread::sleep_for(delay_);
        if (cancel_after_write_) {
            cancel_after_write_->cancel();
            throw graph::CancelledException("SDK effect committed before cancellation");
        }
        if (throw_after_write_) throw std::runtime_error("response lost after external commit");
        return json{{"effect_id", effect_id}, {"payload", arguments}}.dump();
    }
    static int count(const std::string& path) {
        const auto db = fixture_db(path);
        sqlite3_stmt* raw_statement = nullptr;
        if (sqlite3_prepare_v2(db.get(), "SELECT count(*) FROM sdk_effects",
                               -1, &raw_statement, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot inspect fixture ledger");
        FixtureStmt statement(raw_statement, sqlite3_finalize);
        return sqlite3_step(statement.get()) == SQLITE_ROW
            ? sqlite3_column_int(statement.get(), 0) : -1;
    }
private:
    std::string name_;
    std::string ledger_path_;
    bool throw_after_write_;
    std::chrono::milliseconds delay_;
    std::shared_ptr<graph::CancelToken> cancel_after_write_;
    std::shared_ptr<std::atomic<bool>> signal_after_write_;
    bool crash_after_write_;
};

ToolExecutionContext tool_context(const std::shared_ptr<SQLiteToolEffectBroker>& broker,
                                   std::string run = "program-run") {
    ToolExecutionContext execution;
    execution.effect_broker = broker;
    execution.identity.owner_scope = "tenant";
    execution.identity.root_run_id = std::move(run);
    execution.identity.thread_id = "core-thread";
    execution.effect_task_id = "s2:node:task-123";
    execution.effect_grant = {"program-v1", "operation-1", "grant-1", 1};
    execution.effect_grant.binding_fingerprint = "admitted-program-binding:v1";
    return execution;
}

ToolCall sdk_call(std::string model_id, std::string name, std::string arguments) {
    return ToolCall{std::move(model_id), std::move(name), std::move(arguments)};
}

std::vector<ChatMessage> broker_dispatch(std::vector<ToolCall> calls,
                                         std::vector<Tool*> tools,
                                         ToolExecutionContext execution,
                                         ToolGate gate = {}) {
    return async::run_sync(dispatch_tool_calls(std::move(calls), std::move(tools),
                                                std::move(gate), {}, std::move(execution)));
}
} // namespace

#if defined(__unix__) || defined(__APPLE__)
TEST(SQLiteRuntimeStores, NativeHistoryRequiresIndependentArchiveCustodyAndReplaysAfterReopen) {
    ToolEffectTestFiles files;
    const auto descriptor = test::descriptor("anthropic.messages");
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = record(1);
    const auto original = captured_native_message(descriptor);
    ASSERT_TRUE(original.native);
    ASSERT_TRUE(original.native->complete());
    const auto second = native_record(original, first);
    {
        InMemoryContextStore memory;
        ASSERT_EQ(memory.append_history(feed, first, {}), ContextStoreAppendResult::Appended);
        ASSERT_EQ(memory.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
        const auto hydrated = memory.hydrate_records(memory.snapshot_history(feed, 1, 2));
        ASSERT_EQ(hydrated.size(), 2u);
        EXPECT_EQ(hydrated[1].message().native, original.native);
        EXPECT_EQ(provider_codec::encode_message(hydrated[1].message()),
                  provider_codec::encode_message(original));
        EXPECT_THROW(RuntimeHistoryRecord::parse(hydrated[1].serialize_canonical()), std::exception);
    }
    auto activation = sp::NativeArchive::provision(files.archive(), files.key(), feed.owner_id, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(activation));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(activation));
    {
        SQLiteContextStore no_archive(files.ledger());
        ASSERT_EQ(no_archive.append_history(feed, first, {}), ContextStoreAppendResult::Appended);
        EXPECT_THROW(no_archive.append_history(feed, second, first.id()), std::invalid_argument);
        EXPECT_EQ(no_archive.history_head(feed).record_id, first.id());
    }
    ContextHistoryRange range;
    {
        SQLiteContextStore store(files.journal(), archive);
        ASSERT_EQ(store.append_history(feed, first, {}), ContextStoreAppendResult::Appended);
        ASSERT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
        EXPECT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::AlreadyPresent);
        range = store.snapshot_history(feed, 1, 2);
        EXPECT_THROW(store.append_history({"other-owner", feed.feed_id}, second, first.id()),
                     std::invalid_argument);
    }
    archive.reset();
    auto reopened_archive = sp::NativeArchive::open(files.archive(), files.key(), feed.owner_id, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(reopened_archive));
    archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(reopened_archive));
    SQLiteContextStore reopened(files.journal(), archive);
    const auto restored = reopened.hydrate_records(range);
    ASSERT_EQ(restored.size(), 2u);
    EXPECT_EQ(restored[0].id(), first.id());
    EXPECT_EQ(restored[1].id(), second.id());
    EXPECT_EQ(provider_codec::encode_message(restored[1].message()),
              provider_codec::encode_message(original));
    ASSERT_TRUE(restored[1].message().native);
    EXPECT_TRUE(restored[1].message().native->complete());
    auto replay = native_request();
    replay.messages = {restored[0].message(), restored[1].message(),
                       sp::Message{{}, sp::Role::User,
                                   {sp::ToolResult{"call-native", "read result", false}}}};
    const auto encoded = sp::messages::encode(descriptor, replay, false);
    ASSERT_TRUE(std::holds_alternative<sp::messages::EncodedRequest>(encoded));
    const auto wire = json::parse(std::get<sp::messages::EncodedRequest>(encoded).body);
    EXPECT_EQ(wire.at("messages").at(1).at("content").at(0).at("signature"), "fixture-signature");
    EXPECT_EQ(wire.at("messages").at(1).at("content").at(1).at("input").at("path"), "a");
    EXPECT_EQ(wire.at("messages").at(2).at("content").at(0).at("tool_use_id"), "call-native");

    SQLiteContextStore untrusted_reopen(files.journal());
    EXPECT_THROW(untrusted_reopen.hydrate_records(range), std::invalid_argument);
    EXPECT_THROW(RuntimeHistoryRecord::parse(restored[1].serialize_canonical()), std::invalid_argument);
    EXPECT_THROW(RuntimeHistoryRecord::parse(restored[1].serialize_canonical(), archive, "other-owner"),
                 std::invalid_argument);
}

TEST(SQLiteRuntimeStores, NativeHistoryRejectsTamperedProjectionEvenWithAuthenticReference) {
    ToolEffectTestFiles files;
    const auto descriptor = test::descriptor("anthropic.messages");
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = record(1);
    const auto second = native_record(captured_native_message(descriptor), first);
    auto activation = sp::NativeArchive::provision(files.archive(), files.key(), feed.owner_id, descriptor);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<sp::NativeArchive>>(activation));
    auto archive = std::get<std::shared_ptr<sp::NativeArchive>>(std::move(activation));
    SQLiteContextStore store(files.journal(), archive);
    ASSERT_EQ(store.append_history(feed, first, {}), ContextStoreAppendResult::Appended);
    ASSERT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
    const auto range = store.snapshot_history(feed, 2, 2);
    auto projection = json::parse(store.hydrate_history(range));
    projection["message"]["parts"][0]["signature"] = "forged-signature";
    const auto bytes = projection.dump();
    const auto db = fixture_db(files.journal());
    sqlite3_stmt* raw = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(), "UPDATE ng_runtime_history SET canonical=? WHERE sequence=2",
                                 -1, &raw, nullptr), SQLITE_OK);
    FixtureStmt update(raw, sqlite3_finalize);
    ASSERT_EQ(sqlite3_bind_text(update.get(), 1, bytes.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(update.get()), SQLITE_DONE);
    EXPECT_THROW(store.hydrate_records(range), std::invalid_argument);
    EXPECT_THROW(store.history_record_by_message_id(feed, "native-message"), std::invalid_argument);
}
#endif

TEST(SQLiteToolEffects, ReopenReplaysExactReceiptWithoutSDKWriteAndRejectsChangedAuthority) {
    ToolEffectTestFiles files;
    const auto journal_path = files.journal();
    const auto ledger_path = files.ledger();
    LedgerTool tool("write", ledger_path);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1:endpoint:a"}});
    auto context = tool_context(broker);
    auto initial = broker_dispatch({sdk_call("model-one", "write", R"({"a":1,"b":2})")},
                                   {&tool}, context);
    ASSERT_EQ(initial.size(), 1u);
    ASSERT_EQ(initial[0].tool_status, "succeeded");
    ASSERT_EQ(LedgerTool::count(ledger_path), 1);
    broker.reset();
    broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1:endpoint:a"}});
    context.effect_broker = broker;
    context.effect_grant.attempt = 3;
    auto replay = broker_dispatch({sdk_call("different-model-id", "write", R"({"b":2,"a":1})")},
                                  {&tool}, context);
    ASSERT_EQ(replay.size(), 1u);
    EXPECT_EQ(replay[0].content, initial[0].content);
    EXPECT_EQ(replay[0].tool_call_id, "different-model-id");
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);

    for (const auto& mutated : {"grant", "version", "operation", "binding", "arguments"}) {
        auto changed = context;
        auto args = std::string(R"({"a":1,"b":2})");
        if (std::string(mutated) == "grant") changed.effect_grant.grant_id = "other-grant";
        if (std::string(mutated) == "version") changed.effect_grant.program_version_id = "other-version";
        if (std::string(mutated) == "operation") changed.effect_grant.operation_id = "other-operation";
        if (std::string(mutated) == "binding") changed.effect_grant.binding_fingerprint = "different-binding";
        if (std::string(mutated) == "arguments") args = R"({"a":8,"b":2})";
        const auto denied = broker_dispatch({sdk_call("model-one", "write", args)},
                                            {&tool}, changed);
        ASSERT_EQ(denied.size(), 1u);
        EXPECT_EQ(denied[0].tool_status, "rejected");
    }
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
    auto changed_executable = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v2:endpoint:b"}});
    context.effect_broker = changed_executable;
    auto denied = broker_dispatch({sdk_call("model-one", "write", R"({"a":1,"b":2})")},
                                  {&tool}, context);
    ASSERT_EQ(denied.size(), 1u);
    EXPECT_EQ(denied[0].tool_status, "rejected");
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
    LedgerTool renamed("renamed", ledger_path);
    context.effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&renamed, "sdk-build:v1:endpoint:a"}});
    const auto wrong_tool = broker_dispatch(
        {sdk_call("model-one", "renamed", R"({"a":1,"b":2})")}, {&renamed}, context);
    ASSERT_EQ(wrong_tool.size(), 1u);
    EXPECT_EQ(wrong_tool[0].tool_status, "rejected");
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
}

TEST(SQLiteToolEffects, MissingMarkerAndBatchGateDenialDoNotCallSDK) {
    ToolEffectTestFiles files;
    const auto journal_path = files.journal();
    const auto ledger_path = files.ledger();
    LedgerTool tool("write", ledger_path);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto context = tool_context(broker);
    ToolGate deny = [](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        co_return ToolDecision::deny("host grant denied");
    };
    auto denied = broker_dispatch({sdk_call("first", "write", "{}")}, {&tool}, context, deny);
    EXPECT_EQ(denied[0].tool_status, "rejected");
    EXPECT_EQ(LedgerTool::count(ledger_path), 0);
    const auto raw = fixture_db(journal_path);
    ASSERT_EQ(sqlite3_exec(raw.get(), "CREATE TRIGGER refuse_tool_marker BEFORE INSERT "
                                "ON neograph_tool_effects BEGIN SELECT RAISE(FAIL, 'marker refused'); END",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    auto refused = broker_dispatch({sdk_call("first", "write", "{}")}, {&tool}, context);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0].tool_status, "failed");
    EXPECT_EQ(LedgerTool::count(ledger_path), 0);
}

TEST(SQLiteToolEffects, FailedReceiptCommitBlocksRedispatchAfterSDKWrite) {
    ToolEffectTestFiles files;
    LedgerTool tool("write", files.ledger());
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    const auto raw = fixture_db(files.journal());
    ASSERT_EQ(sqlite3_exec(raw.get(), "CREATE TRIGGER refuse_tool_receipt BEFORE UPDATE "
                                "ON neograph_tool_effects BEGIN SELECT RAISE(FAIL, 'receipt refused'); END",
                           nullptr, nullptr, nullptr), SQLITE_OK);
    auto context = tool_context(broker);
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
    broker.reset();
    context.effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    EXPECT_THROW(broker_dispatch({sdk_call("two", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
}

TEST(SQLiteToolEffects, ExternalCommitWithoutReceiptBlocksRestartAndAdvancedAttempt) {
    ToolEffectTestFiles files;
    const auto journal_path = files.journal();
    const auto ledger_path = files.ledger();
    LedgerTool tool("write", ledger_path, true);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto context = tool_context(broker);
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    ASSERT_EQ(LedgerTool::count(ledger_path), 1);
    broker.reset();
    context.effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    context.effect_grant.attempt = 5;
    EXPECT_THROW(broker_dispatch({sdk_call("another", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
}

TEST(SQLiteToolEffects, IndependentConcurrentRunsAndBatchOrdinalsHaveDistinctEffects) {
    ToolEffectTestFiles files;
    const auto journal_path = files.journal();
    const auto ledger_path = files.ledger();
    LedgerTool tool("write", ledger_path);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    std::atomic<int> completed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            const auto context = tool_context(broker, "run-" + std::to_string(i));
            const auto result = broker_dispatch(
                {sdk_call("same-model-id", "write", R"({"segment":1})"),
                 sdk_call("same-model-id", "write", R"({"segment":2})")},
                {&tool}, context);
            if (result.size() == 2 && result[0].tool_status == "succeeded" &&
                result[1].tool_status == "succeeded") ++completed;
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(completed.load(), 4);
    EXPECT_EQ(LedgerTool::count(ledger_path), 8);
    auto replay = broker_dispatch(
        {sdk_call("new-id", "write", R"({"segment":1})"),
         sdk_call("new-id", "write", R"({"segment":2})")},
        {&tool}, tool_context(broker, "run-2"));
    EXPECT_EQ(replay[0].tool_status, "succeeded");
    EXPECT_EQ(replay[1].tool_status, "succeeded");
    EXPECT_EQ(LedgerTool::count(ledger_path), 8);
    // Reordering the same model IDs does not reassign a committed ordinal.
    const auto swapped = broker_dispatch(
        {sdk_call("same-model-id", "write", R"({"segment":2})"),
         sdk_call("same-model-id", "write", R"({"segment":1})")},
        {&tool}, tool_context(broker, "run-2"));
    EXPECT_EQ(swapped[0].tool_status, "rejected");
    EXPECT_EQ(swapped[1].tool_status, "rejected");
    EXPECT_EQ(LedgerTool::count(ledger_path), 8);
}

TEST(SQLiteToolEffects, CancellationAfterSDKCommitLeavesUnresolvedMarker) {
    ToolEffectTestFiles files;
    auto token = std::make_shared<graph::CancelToken>();
    LedgerTool tool("write", files.ledger(), false, {}, token);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto context = tool_context(broker);
    context.cancel_token = token;
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::CancelledException);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
    context.effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    context.cancel_token.reset();
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
}

TEST(SQLiteToolEffects, TimeoutAfterSDKCommitDoesNotRedispatch) {
    ToolEffectTestFiles files;
    LedgerTool tool("write", files.ledger(), false, std::chrono::milliseconds(120));
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto controller = std::make_shared<ToolExecutionController>();
    ToolExecutionPolicy policy;
    policy.execution_timeout = std::chrono::milliseconds(5);
    policy.effect = ToolEffectClass::ExternalWrite;
    controller->policies()->upsert("write", policy);
    auto context = tool_context(broker);
    context.controller = controller;
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_EQ(LedgerTool::count(files.ledger()), 1);
    context.effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    EXPECT_THROW(broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
}

TEST(SQLiteToolEffects, SimultaneousSameSlotCannotDispatchTwice) {
    ToolEffectTestFiles files;
    auto signalled = std::make_shared<std::atomic<bool>>(false);
    LedgerTool tool("write", files.ledger(), false, std::chrono::milliseconds(150),
                    {}, signalled);
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto context = tool_context(broker);
    std::atomic<bool> first_succeeded{false};
    std::string first_error;
    std::jthread first([&] {
        try {
            const auto result = broker_dispatch({sdk_call("one", "write", "{}")}, {&tool}, context);
            first_succeeded = result.size() == 1 && result[0].tool_status == "succeeded";
        } catch (const std::exception& error) {
            first_error = error.what();
        }
    });
    for (int i = 0; i < 500 && !signalled->load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(signalled->load(std::memory_order_acquire));
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
    EXPECT_THROW(broker_dispatch({sdk_call("another", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    first.join();
    EXPECT_TRUE(first_succeeded) << first_error;
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
}

#ifdef __linux__
namespace {
class ChildReaper {
public:
    explicit ChildReaper(pid_t pid) : pid_(pid) {}
    ChildReaper(const ChildReaper&) = delete;
    ChildReaper& operator=(const ChildReaper&) = delete;
    ~ChildReaper() {
        if (pid_ > 0) {
            int ignored = 0;
            while (::waitpid(pid_, &ignored, 0) == -1 && errno == EINTR) {}
        }
    }
    int wait() {
        int status = 0;
        pid_t observed;
        do { observed = ::waitpid(pid_, &status, 0); } while (observed == -1 && errno == EINTR);
        if (observed != pid_) throw std::runtime_error("Tool effect fixture child wait failed");
        pid_ = -1;
        return status;
    }
private:
    pid_t pid_;
};
} // namespace

TEST(SQLiteToolEffects, ProcessCrashBlocksEffectAfterRestart) {
    if (const char* child_journal = std::getenv("NG_TOOL_EFFECT_CRASH_JOURNAL")) {
        const char* child_ledger = std::getenv("NG_TOOL_EFFECT_CRASH_LEDGER");
        if (!child_ledger) throw std::runtime_error("Tool effect fixture missing child ledger");
        LedgerTool tool("write", child_ledger, false, {}, {}, {}, true);
        auto broker = std::make_shared<SQLiteToolEffectBroker>(
            child_journal, std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
        (void)broker_dispatch({sdk_call("first", "write", "{}")}, {&tool}, tool_context(broker));
        std::_Exit(79); // The child must terminate inside Tool::execute, before receipt.
    }

    ToolEffectTestFiles files;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
    const std::string journal_env = "NG_TOOL_EFFECT_CRASH_JOURNAL=" + files.journal();
    const std::string ledger_env = "NG_TOOL_EFFECT_CRASH_LEDGER=" + files.ledger();
    std::vector<char*> child_env;
    for (auto entry = environ; *entry; ++entry) {
        if (!std::string_view(*entry).starts_with("NG_TOOL_EFFECT_CRASH_"))
            child_env.push_back(*entry);
    }
    child_env.push_back(const_cast<char*>(journal_env.c_str()));
    child_env.push_back(const_cast<char*>(ledger_env.c_str()));
    child_env.push_back(nullptr);
    std::string selector = "--gtest_filter=SQLiteToolEffects.ProcessCrashBlocksEffectAfterRestart";
    char* child_args[] = {const_cast<char*>(executable.c_str()), selector.data(), nullptr};
    pid_t pid = -1;
    ASSERT_EQ(::posix_spawn(&pid, executable.c_str(), nullptr, nullptr,
                            child_args, child_env.data()), 0);
    ChildReaper child(pid);
    const int status = child.wait();
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 77);
    ASSERT_EQ(LedgerTool::count(files.ledger()), 1);

    LedgerTool tool("write", files.ledger());
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        files.journal(), std::vector<SQLiteToolExecutableBinding>{{&tool, "sdk-build:v1"}});
    auto context = tool_context(broker);
    context.effect_grant.attempt = 4;
    EXPECT_THROW(broker_dispatch({sdk_call("different-id", "write", "{}")}, {&tool}, context),
                 graph::NodeInterrupt);
    EXPECT_EQ(LedgerTool::count(files.ledger()), 1);
}
#endif

namespace {
class ToolEffectAssistantNode final : public graph::GraphNode {
public:
    asio::awaitable<graph::NodeResult> run(graph::NodeInput) override {
        graph::NodeResult result;
        result.writes.push_back(graph::ChannelWrite{
            "messages", json::array({json{
                {"role", "assistant"}, {"content", ""},
                {"tool_calls", json::array({json{
                    {"id", "model-id"}, {"name", "write"},
                    {"arguments", R"({"item":"core"})"}}})}}})});
        co_return result;
    }
    std::string get_name() const override { return "tool-effect-source"; }
};
} // namespace

TEST(SQLiteToolEffects, StandaloneCoreRunResourcesBrokerReplaysWithoutEngineMutation) {
    ToolEffectTestFiles files;
    const auto journal_path = files.journal();
    const auto ledger_path = files.ledger();
    auto tool = std::make_shared<LedgerTool>("write", ledger_path);
    graph::NodeFactory::instance().register_type("sqlite_tool_effect_source",
        [](const std::string&, const json&, const graph::NodeContext&) {
            return std::make_unique<ToolEffectAssistantNode>();
        });
    const auto graph_definition = json{
        {"name", "sqlite_tool_effect_graph"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"source", {{"type", "sqlite_tool_effect_source"}}},
                   {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "source"}},
                                json{{"from", "source"}, {"to", "tools"}},
                                json{{"from", "tools"}, {"to", "__end__"}}})}};
    graph::NodeContext nodes;
    nodes.tools = ToolSet(std::vector<std::shared_ptr<Tool>>{tool});
    auto engine = graph::GraphEngine::compile(graph_definition, nodes);
    graph::RunConfig config;
    config.thread_id = "core-thread";
    graph::RunMetadata metadata;
    metadata.owner_scope = "tenant";
    metadata.run_id = "standalone-core-run";
    auto broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{tool.get(), "sdk-build:v1"}});
    graph::RunResources resources;
    resources.tool_effect_broker = broker;
    resources.tool_effect_grant = {"", "standalone-operation", "host-grant", 1};
    const auto first = async::run_sync(engine->run_async(config, metadata, resources));
    EXPECT_FALSE(first.interrupted);
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
    broker.reset();
    resources.tool_effect_broker = std::make_shared<SQLiteToolEffectBroker>(
        journal_path, std::vector<SQLiteToolExecutableBinding>{{tool.get(), "sdk-build:v1"}});
    resources.tool_effect_grant.attempt = 2;
    const auto replay = async::run_sync(engine->run_async(config, metadata, resources));
    EXPECT_FALSE(replay.interrupted);
    EXPECT_EQ(LedgerTool::count(ledger_path), 1);
}

#ifdef NEOGRAPH_SQLITE_TOOL_TESTS_HAVE_LLM
namespace {
class SDKAgentProvider final : public test::LocalProvider {
public:
    SDKAgentProvider() : LocalProvider(
        [](ProviderRequest request, const PreparedProviderRequest&,
           const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            const auto& messages = std::get<sp::chat::Request>(request.payload).canonical_messages;
            if (std::any_of(messages.begin(), messages.end(),
                            [](const sp::Message& message) { return message.role == sp::Role::Tool; }))
                co_return test::success("done");
            sp::Message assistant;
            assistant.role = sp::Role::Assistant;
            assistant.parts.emplace_back(sp::ToolCall{
                "untrusted-model-id", "write", sp::ToolCallKind::ClientExecuted,
                test::document(R"({"item":"agent"})")});
            co_return test::success(std::vector<sp::Message>{std::move(assistant)});
        }, "sdk-agent-fixture") {}
};
} // namespace

TEST(SQLiteToolEffects, StandaloneAgentDeniesAndReplaysPendingAssistantOnRestart) {
    ToolEffectTestFiles files;
    const auto ledger_path = files.ledger();
    const auto journal_path = files.journal();
    std::vector<sp::Message> denied_messages{test::message("write", sp::Role::User)};
    {
        auto effect = std::make_unique<LedgerTool>("write", ledger_path);
        auto* tool = effect.get();
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::move(effect));
        llm::Agent agent(std::make_shared<SDKAgentProvider>(), std::move(tools), "", "fixture-model");
        agent.set_tool_gate([](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
            co_return ToolDecision::deny("no SDK writes");
        });
        auto context = tool_context(std::make_shared<SQLiteToolEffectBroker>(
            journal_path, std::vector<SQLiteToolExecutableBinding>{{tool, "sdk-build:v1"}}),
            "denied-agent-run");
        EXPECT_EQ(test::text(agent.run(denied_messages, 3, context)), "done");
        EXPECT_EQ(LedgerTool::count(ledger_path), 0);
    }

    std::vector<sp::Message> messages{test::message("write", sp::Role::User)};
    {
        auto effect = std::make_unique<LedgerTool>("write", ledger_path);
        auto* tool = effect.get();
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::move(effect));
        llm::Agent agent(std::make_shared<SDKAgentProvider>(), std::move(tools), "", "fixture-model");
        auto context = tool_context(std::make_shared<SQLiteToolEffectBroker>(
            journal_path, std::vector<SQLiteToolExecutableBinding>{{tool, "sdk-build:v1"}}),
            "allowed-agent-run");
        EXPECT_EQ(test::text(agent.run(messages, 3, context)), "done");
        EXPECT_EQ(LedgerTool::count(ledger_path), 1);
    }
    messages.pop_back(); // Reconnect with the committed assistant, not its final answer.
    messages.pop_back(); // Its Tool reply was not persisted by the standalone host.
    {
        auto effect = std::make_unique<LedgerTool>("write", ledger_path);
        auto* tool = effect.get();
        std::vector<std::unique_ptr<Tool>> tools;
        tools.push_back(std::move(effect));
        llm::Agent agent(std::make_shared<SDKAgentProvider>(), std::move(tools), "", "fixture-model");
        auto context = tool_context(std::make_shared<SQLiteToolEffectBroker>(
            journal_path, std::vector<SQLiteToolExecutableBinding>{{tool, "sdk-build:v1"}}),
            "allowed-agent-run");
        context.effect_grant.attempt = 2;
        EXPECT_EQ(test::text(agent.run(messages, 3, context)), "done");
        EXPECT_EQ(LedgerTool::count(ledger_path), 1);
    }
}
#endif
