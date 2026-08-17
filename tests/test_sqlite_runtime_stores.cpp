#include <neograph/sqlite_runtime_stores.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <thread>

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
    d.trust = RuntimeTrustClass::UntrustedInput; d.message = {"user", "message"}; d.predecessor_id = std::move(predecessor);
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
    EXPECT_EQ(reopened.history_head({"owner_b", "feed"}).sequence, 0u);
    const auto second = record(2, first.id());
    std::atomic<int> wins{0}; std::vector<std::thread> workers;
    for (int i = 0; i != 6; ++i) workers.emplace_back([&] { if (reopened.append_history(owner, second, first.id()) == ContextStoreAppendResult::Appended) ++wins; });
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(wins, 1); EXPECT_EQ(reopened.hydrate_history(reopened.snapshot_history(owner, 1, 2)), first.serialize_canonical() + "\n" + second.serialize_canonical());
}

TEST(SQLiteRuntimeStores, HookLeaseSurvivesReopenAndFencesStaleWorker) {
    const auto path = database("hook_reopen"); const auto now = std::chrono::system_clock::now();
    const auto event = RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json::object()});
    const auto call = invocation(event); HookLease first;
    { SQLiteHookJournal journal(path); journal.enqueue(call, event, 2, now + std::chrono::minutes(1)); journal.publish(call.id()); first = *journal.claim(call.id(), "first", now, std::chrono::milliseconds(1)); }
    SQLiteHookJournal reopened(path);
    const auto second = *reopened.claim(call.id(), "second", now + std::chrono::seconds(1), std::chrono::seconds(1));
    EXPECT_GT(second.fencing_token, first.fencing_token);
    const auto stale = HookExecutionReceipt::create({call.id(), first.entry.data().attempt_count, HookExecutionState::Succeeded, {"effect", true, true, {}}, {}});
    EXPECT_FALSE(reopened.settle(call.id(), first.fencing_token, stale, now + std::chrono::seconds(1)));
}

TEST(SQLiteRuntimeStores, ProviderReceiptReopensAndFailsClosedOnCorruption) {
    const auto path = database("receipt_reopen");
    const auto receipt = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('b'), sha('c'), "model", CompletionMode::COLLECT});
    { SQLiteProviderDispatchReceiptStore store(path); EXPECT_EQ(store.persist(receipt), ProviderDispatchReceiptPutResult::Stored); }
    SQLiteProviderDispatchReceiptStore reopened(path);
    EXPECT_EQ(reopened.persist(receipt), ProviderDispatchReceiptPutResult::AlreadyPresent);
    EXPECT_EQ(reopened.state("dispatch"), ProviderDispatchState::AdmittedPending);
    sqlite3* raw = nullptr; ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "UPDATE ng_provider_receipts SET canonical='bad' WHERE dispatch_id='dispatch'", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);
    EXPECT_THROW(reopened.state("dispatch"), std::exception);
}

TEST(SQLiteRuntimeStores, ProviderReceiptsAreOwnerIsolated) {
    SQLiteProviderDispatchReceiptStore store(database("receipt_owner_isolation"));
    const auto first = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('b'), sha('c'), "model", CompletionMode::COLLECT});
    const auto second = ProviderDispatchReceipt::create({"dispatch", sha('a'), sha('d'), sha('e'), "model", CompletionMode::COLLECT});
    EXPECT_EQ(store.persist("owner_a", first), ProviderDispatchReceiptPutResult::Stored);
    EXPECT_EQ(store.persist("owner_b", second), ProviderDispatchReceiptPutResult::Stored);
    EXPECT_EQ(store.persist("owner_a", second), ProviderDispatchReceiptPutResult::Conflict);
    EXPECT_EQ(store.state("owner_a", "dispatch"), ProviderDispatchState::AdmittedPending);
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
