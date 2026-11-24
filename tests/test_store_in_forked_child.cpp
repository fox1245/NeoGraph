// A store that a child made by fork inherited from its parent. The copy refuses every operation with
// StoreOpenedInAnotherProcess before it touches the connection or takes the store's lock, and destroying it calls
// nothing that would close the parent's connection. The parent's store works before and after the fork.
//
// Each test forks; a child reports through its exit status and never returns into the test runner (fork_child.h). A
// child that hangs is ended with its test by the test's TIMEOUT property.
#include "fork_child.h"

#if defined(NEOGRAPH_TESTS_HAVE_FORK)

#include "../src/core/process_generation.h"
#include "../src/core/tool_failure.h"
#include "fixtures/unique_temp_dir.h"

#include <neograph/async/run_sync.h>
#include <neograph/graph/configuration_failure.h>
#include <neograph/hook.h>
#include <neograph/hook_outbox.h>
#include <neograph/research/sqlite_evidence_ledger.h>
#include <neograph/runtime_context.h>
#include <neograph/sqlite_runtime_stores.h>
#include <neograph/store_opened_in_another_process.h>
#include <neograph/tool.h>
#include <neograph/tool_execution.h>
#if defined(NEOGRAPH_STORE_FORK_TESTS_HAVE_PROGRAM_SQLITE)
#include <neograph/program/sqlite_core_tool_grant_store.h>
#endif

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neograph;
using test::ForkChildEnd;
using test::run_in_forked_child;

namespace {

// One public operation of a store, called with arguments the store accepts.
struct Operation {
    std::string           name;
    std::function<void()> call;
};

// A child that called the operations of a store: 0 when each was refused with StoreOpenedInAnotherProcess, otherwise
// the first that was not: kNotRefused + its index when it returned, kOtherError + its index when it threw something
// else. A store has fewer than kStatusesPerKind operations.
constexpr int kStatusesPerKind = 50;
constexpr int kNotRefused      = 10;
constexpr int kOtherError      = kNotRefused + kStatusesPerKind;

int refuse_every_operation(const std::vector<Operation>& operations) {
    for (std::size_t index = 0; index != operations.size(); ++index) {
        try {
            operations[index].call();
            return kNotRefused + static_cast<int>(index);
        } catch (const StoreOpenedInAnotherProcess&) {
        } catch (...) {
            return kOtherError + static_cast<int>(index);
        }
    }
    return 0;
}

std::string describe(const std::vector<Operation>& operations, const ForkChildEnd& end) {
    const auto name = [&operations](int index) {
        return index >= 0 && static_cast<std::size_t>(index) < operations.size() ? operations[index].name
                                                                                   : std::string("?");
    };
    if (end.exited && end.status >= kOtherError && end.status < kOtherError + kStatusesPerKind)
        return name(end.status - kOtherError) + " threw another error in the child";
    if (end.exited && end.status >= kNotRefused && end.status < kNotRefused + kStatusesPerKind)
        return name(end.status - kNotRefused) + " was not refused in the child";
    std::string text = "the child ";
    text += end.exited ? "exited with status " + std::to_string(end.status)
                       : "was ended by signal " + std::to_string(end.signal);
    return text;
}

// Closing a connection: counted through SQLite's trace hook, which the library calls from its close functions.
std::atomic<int> closes_seen{0};

int count_close(unsigned, void*, void*, void*) {
    closes_seen.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void watch_closing(sqlite3* connection) {
    if (sqlite3_trace_v2(connection, SQLITE_TRACE_CLOSE, &count_close, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot watch the store's connection");
}

// Holds a thread of the parent inside a write of the store: another connection holds the database's write lock, and
// the busy handler that SQLite calls while the write waits (with the store's lock and the connection held) waits until
// the test lets it go.
struct BusyGate {
    std::latch        inside{1};
    std::latch        release{1};
    std::atomic<bool> entered{false};
};

int wait_while_busy(void* context, int) {
    auto& gate = *static_cast<BusyGate*>(context);
    if (!gate.entered.exchange(true)) gate.inside.count_down();
    gate.release.wait();
    return 0;  // the waiting write gives up with SQLITE_BUSY
}

class WriteLock {
public:
    explicit WriteLock(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
            sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot take the database's write lock");
    }
    ~WriteLock() {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close(db_);
    }
    WriteLock(const WriteLock&)            = delete;
    WriteLock& operator=(const WriteLock&) = delete;

private:
    sqlite3* db_ = nullptr;
};

std::string sha(char c) { return "sha256:" + std::string(64, c); }

// ---- The owners. Each opens one store and names its operations, a write and the read of that write. ----

class EvidenceLedgerOwner {
public:
    static constexpr const char* kName = "EvidenceLedger";
    explicit EvidenceLedgerOwner(const std::string& path)
        : store_(std::make_unique<research::SqliteEvidenceLedger>(path)) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) { store_->register_source(source("written-" + std::to_string(n))); }
    bool read_back(int n) const { return store_->source("written-" + std::to_string(n)).has_value(); }

    std::vector<Operation> operations() {
        return {
            {"register_source", [this] { store_->register_source(source("source-1")); }},
            {"source", [this] { static_cast<void>(store_->source("source-1")); }},
            {"create_task", [this] { store_->create_task(task()); }},
            {"task", [this] { static_cast<void>(store_->task("owner-a", "extract-1")); }},
            {"tasks", [this] { static_cast<void>(store_->tasks("owner-a", true)); }},
            {"acquire_lease",
             [this] {
                 if (auto lease = store_->acquire_lease({"extract-1", "lease-1", "worker-1", "owner-a", 1'000}))
                     lease_ = *lease;
             }},
            {"renew_lease", [this] { static_cast<void>(store_->renew_lease(lease_, 1'001)); }},
            {"expire_leases", [this] { static_cast<void>(store_->expire_leases("owner-a", 1'002)); }},
            {"publish", [this] { static_cast<void>(store_->publish(lease_, artifact(), 1'003)); }},
            {"artifacts_for_claim",
             [this] { static_cast<void>(store_->artifacts_for_claim("owner-a", "claim-1")); }},
            {"source_lifecycle",
             [this] { static_cast<void>(store_->source_lifecycle("owner-a", "source-1", 1'004)); }},
            {"resolve_claim", [this] { static_cast<void>(store_->resolve_claim("owner-a", "claim-1")); }},
        };
    }

private:
    static research::SourceIdentity source(const std::string& id) {
        research::SourceIdentity source;
        source.source_id         = id;
        source.canonical_locator = "doi:10.1000/" + id;
        source.version           = "v1";
        source.content_hash      = "sha256:" + id;
        source.metadata          = json::object();
        return source;
    }
    static research::ResearchTaskSpec task() {
        research::ResearchTaskSpec task;
        task.task_id           = "extract-1";
        task.objective_id      = "objective-1";
        task.source_id         = "source-1";
        task.claim_id          = "claim-1";
        task.owner_scope       = "owner-a";
        task.scope             = "methods";
        task.lease_duration_ms = 100;
        task.requirements      = json::object();
        return task;
    }
    research::EvidenceArtifact artifact() const {
        research::EvidenceArtifact artifact;
        artifact.artifact_id         = "artifact-1";
        artifact.task_id             = lease_.task_id;
        artifact.source_id           = "source-1";
        artifact.claim_id            = "claim-1";
        artifact.owner_scope         = lease_.owner_scope;
        artifact.source_version      = "v1";
        artifact.source_content_hash = "sha256:source-1";
        artifact.polarity            = research::EvidencePolarity::Supports;
        artifact.evidence_locator    = "page-1";
        artifact.observation         = "observed";
        artifact.searched_scope      = "pages 1-2";
        artifact.conditions          = json::object();
        artifact.applicability       = json::object();
        artifact.uncertainty         = json::object();
        artifact.confidence          = 0.5;
        artifact.worker_id           = lease_.worker_id;
        artifact.model_id            = "model";
        artifact.tool_id             = "tool";
        artifact.program_version_id  = "program-v1";
        artifact.program_run_id      = "run-1";
        artifact.provenance          = json::object();
        return artifact;
    }

    std::unique_ptr<research::SqliteEvidenceLedger> store_;
    research::ResearchTaskLease                     lease_;
};

RuntimeHistoryRecord history_record(const std::string& feed_id) {
    RuntimeHistoryRecordData data;
    data.feed_id    = feed_id;
    data.sequence   = 1;
    data.message_id = "message_1";
    data.trust      = RuntimeTrustClass::UntrustedInput;
    data.message.role = sp::Role::User;
    data.message.parts.emplace_back(sp::Text{"message"});
    return RuntimeHistoryRecord::create(std::move(data));
}

class ContextStoreOwner {
public:
    static constexpr const char* kName = "ContextStore";
    explicit ContextStoreOwner(const std::string& path)
        : store_(std::make_unique<SQLiteContextStore>(path)), record_(history_record(kFeed.feed_id)),
          artifact_(make_artifact()) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) {
        const ContextStoreFeed feed{kFeed.owner_id, "written_" + std::to_string(n)};
        if (store_->append_history(feed, history_record(feed.feed_id), {}) != ContextStoreAppendResult::Appended)
            throw std::runtime_error("the history record was not appended");
    }
    bool read_back(int n) const {
        return store_->history_head({kFeed.owner_id, "written_" + std::to_string(n)}).sequence == 1;
    }

    std::vector<Operation> operations() {
        return {
            {"append_history", [this] { static_cast<void>(store_->append_history(kFeed, record_, {})); }},
            {"history_head", [this] { static_cast<void>(store_->history_head(kFeed)); }},
            {"history_record_by_message_id",
             [this] { static_cast<void>(store_->history_record_by_message_id(kFeed, "message_1")); }},
            {"snapshot_history", [this] { range_ = store_->snapshot_history(kFeed, 1, 1); }},
            {"hydrate_history", [this] { static_cast<void>(store_->hydrate_history(range_)); }},
            {"hydrate_records", [this] { static_cast<void>(store_->hydrate_records(range_)); }},
            {"put_artifact", [this] { static_cast<void>(store_->put_artifact(kFeed.owner_id, artifact_)); }},
            {"get_artifact",
             [this] { static_cast<void>(store_->get_artifact(kFeed.owner_id, artifact_.id())); }},
        };
    }

private:
    static ContextArtifact make_artifact() {
        ContextArtifactData data;
        data.kind          = ContextArtifactKind::RequiredSkill;
        data.producer_id   = "skill-resolver.v1";
        data.source_digest = sha('a');
        data.media_type    = "text/markdown";
        data.placement     = ContextPlacement::BeforeLatestUser;
        data.priority      = 100;
        data.required      = true;
        data.content       = json{{"name", "skill"}, {"text", "text"}};
        return ContextArtifact::create(std::move(data));
    }

    inline static const ContextStoreFeed kFeed{"owner_a", "feed"};
    std::unique_ptr<SQLiteContextStore> store_;
    RuntimeHistoryRecord                record_;
    ContextArtifact                     artifact_;
    ContextHistoryRange                 range_;
};

class HookJournalOwner {
public:
    static constexpr const char* kName = "HookJournal";
    explicit HookJournalOwner(const std::string& path)
        // In milliseconds, as the journal stores the deadline: enqueueing the same call again compares them.
        : store_(std::make_unique<SQLiteHookJournal>(path)),
          now_(std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())),
          event_(event(0)), call_(invocation(event_)) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) {
        const auto written = event(n);
        const auto call    = invocation(written);
        written_.insert_or_assign(n, call.id());
        static_cast<void>(store_->enqueue(call, written, 1, deadline()));
    }
    bool read_back(int n) const {
        const auto found = written_.find(n);
        return found != written_.end() && store_->get(found->second).has_value();
    }

    std::vector<Operation> operations() {
        return {
            {"enqueue", [this] { static_cast<void>(store_->enqueue(call_, event_, 2, deadline())); }},
            {"publish", [this] { static_cast<void>(store_->publish(call_.id())); }},
            {"claim",
             [this] {
                 if (const auto lease = store_->claim(call_.id(), "worker", now_, std::chrono::seconds(1)))
                     fence_ = lease->fencing_token;
             }},
            {"settle", [this] { static_cast<void>(store_->settle(call_.id(), fence_, receipt(), now_)); }},
            {"reconcile",
             [this] {
                 static_cast<void>(store_->reconcile(call_.id(), HookExecutionState::ReconciliationRequired,
                                                     sha('f'), fence_, receipt()));
             }},
            {"pending", [this] { static_cast<void>(store_->pending()); }},
            {"reconciliation_required", [this] { static_cast<void>(store_->reconciliation_required()); }},
            {"get", [this] { static_cast<void>(store_->get(call_.id())); }},
        };
    }

private:
    static RuntimeEvent event(int n) {
        return RuntimeEvent::create({{}, 1, HookPhase::BeforeToolExecution, "event", "owner", "run", json{{"n", n}}});
    }
    static HookInvocation invocation(const RuntimeEvent& event) {
        return HookInvocation::create({{}, sha('a'), event.id(), "audit", HookPhase::BeforeToolExecution,
                                       HookDelivery::BlockingMandatory, HookFailureMode::FailClosed,
                                       HookIdempotency::Idempotent, ToolEffectClass::ReadOnly, {}, {},
                                       json::object()});
    }
    std::chrono::system_clock::time_point deadline() const { return now_ + std::chrono::hours(1); }
    HookExecutionReceipt receipt() const {
        return HookExecutionReceipt::create(
            {call_.id(), 1, HookExecutionState::Succeeded, {"effect", true, true, {}}, {}});
    }

    std::unique_ptr<SQLiteHookJournal>      store_;
    std::chrono::system_clock::time_point   now_;
    RuntimeEvent                            event_;
    HookInvocation                          call_;
    std::uint64_t                           fence_ = 0;
    std::map<int, std::string>              written_;
};

class ReceiptStoreOwner {
public:
    static constexpr const char* kName = "ProviderDispatchReceiptStore";
    explicit ReceiptStoreOwner(const std::string& path)
        : store_(std::make_unique<SQLiteProviderDispatchReceiptStore>(path)),
          receipt_(ProviderDispatchReceipt::create(
              {"dispatch", sha('a'), sha('b'), sha('c'), "model", ProviderMode::Collect})),
          outcome_(ProviderDispatchOutcomeReceipt::create(
              {"dispatch", receipt_.id(), ProviderDispatchState::Succeeded, sha('d'), {}})) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) {
        if (store_->persist(owner(n), receipt_) != ProviderDispatchReceiptPutResult::Stored)
            throw std::runtime_error("the receipt was not stored");
    }
    bool read_back(int n) const {
        return store_->state(owner(n), "dispatch") == ProviderDispatchState::AdmittedPending;
    }

    std::vector<Operation> operations() {
        return {
            {"persist", [this] { static_cast<void>(store_->persist(receipt_)); }},
            {"persist with an owner", [this] { static_cast<void>(store_->persist("owner_a", receipt_)); }},
            {"state", [this] { static_cast<void>(store_->state("dispatch")); }},
            {"state with an owner", [this] { static_cast<void>(store_->state("owner_a", "dispatch")); }},
            {"settle", [this] { static_cast<void>(store_->settle("owner_a", outcome_)); }},
            {"outcome", [this] { static_cast<void>(store_->outcome("owner_a", "dispatch")); }},
        };
    }

private:
    static std::string owner(int n) { return "written_" + std::to_string(n); }

    std::unique_ptr<SQLiteProviderDispatchReceiptStore> store_;
    ProviderDispatchReceipt                              receipt_;
    ProviderDispatchOutcomeReceipt                       outcome_;
};

// A tool that completes on the calling thread.
class WriteTool final : public Tool {
public:
    ChatTool    get_definition() const override { return {"write", "", json::object()}; }
    std::string get_name() const override { return "write"; }
    std::string execute(const json&) override {
        ++runs;
        return "written";
    }
    asio::awaitable<std::string> execute_async(const json& arguments) override { co_return execute(arguments); }
    std::atomic<int> runs{0};
};

class ToolEffectBrokerOwner {
public:
    static constexpr const char* kName = "ToolEffectBroker";
    explicit ToolEffectBrokerOwner(const std::string& path)
        : store_(std::make_unique<SQLiteToolEffectBroker>(
              path, std::vector<SQLiteToolExecutableBinding>{{&tool_, "write-executable:v1"}})),
          controller_(make_controller()) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) {
        const auto result = execute("written-" + std::to_string(n));
        if (result.status != ToolTerminalStatus::Succeeded) throw std::runtime_error(result.error);
    }
    bool read_back(int n) {
        const int  runs   = tool_.runs.load();
        const auto result = execute("written-" + std::to_string(n));
        return result.status == ToolTerminalStatus::Succeeded && tool_.runs.load() == runs;
    }

    std::vector<Operation> operations() {
        return {{"execute", [this] { static_cast<void>(execute("run-1")); }}};
    }

private:
    static std::shared_ptr<ToolExecutionController> make_controller() {
        auto                policies = std::make_shared<ToolExecutionPolicyRegistry>();
        ToolExecutionPolicy policy;
        policy.effect      = ToolEffectClass::ExternalWrite;
        policy.idempotency = ToolIdempotency::NonIdempotent;
        policies->upsert("write", policy);
        return std::make_shared<ToolExecutionController>(
            ToolExecutionControllerConfig{policies, std::make_shared<ResourceArbiter>(), nullptr});
    }
    ToolExecutionResult execute(const std::string& run) {
        ToolEffectIdentity identity;
        identity.owner_scope  = "tenant";
        identity.run_id       = run;
        identity.thread_id    = "thread";
        identity.task_id      = "s1:act";
        identity.operation_id = "operation";
        identity.grant_id     = "grant";
        identity.tool_call_id = "call-1";
        ToolExecutionContext context;
        context.controller = controller_;
        return async::run_sync(store_->execute(std::move(identity), tool_, json::object(), std::move(context)));
    }

    WriteTool                                tool_;
    std::unique_ptr<SQLiteToolEffectBroker>  store_;
    std::shared_ptr<ToolExecutionController> controller_;
};

#if defined(NEOGRAPH_STORE_FORK_TESTS_HAVE_PROGRAM_SQLITE)
class CoreToolGrantStoreOwner {
public:
    static constexpr const char* kName = "CoreToolGrantStore";
    explicit CoreToolGrantStoreOwner(const std::string& path)
        : store_(std::make_unique<program::SQLiteProgramCoreToolGrantStore>(path)), record_(record("run-1")) {}

    sqlite3* connection() const { return store_->connection_for_testing(); }
    void     destroy() { store_.reset(); }

    void write(int n) {
        if (store_->admit_sync(record(written(n))) != program::ProgramCoreToolGrantAdmission::Admitted)
            throw std::runtime_error("the grant was not admitted");
    }
    bool read_back(int n) const {
        const auto run   = written(n);
        const auto found = record(run);
        return store_->load_sync(context(found)).has_value();
    }

    std::vector<Operation> operations() {
        return {
            {"admit_sync", [this] { static_cast<void>(store_->admit_sync(record_)); }},
            {"load_sync", [this] { static_cast<void>(store_->load_sync(context(record_))); }},
            {"revoke_sync", [this] { static_cast<void>(store_->revoke_sync(context(record_))); }},
            {"admit", [this] { static_cast<void>(async::run_sync(store_->admit(record_))); }},
            {"load",
             [this] {
                 const auto view = context(record_);
                 static_cast<void>(async::run_sync(store_->load(view)));
             }},
            {"revoke",
             [this] {
                 const auto view = context(record_);
                 static_cast<void>(async::run_sync(store_->revoke(view)));
             }},
        };
    }

private:
    static std::string written(int n) { return "written-" + std::to_string(n); }
    static program::ProgramCoreToolGrantRecord record(const std::string& run) {
        return {"tenant", "program-v1", run, "operation-1", 1, "binding:v1", "grant-" + run};
    }
    static program::ProgramCoreToolGrantContext context(const program::ProgramCoreToolGrantRecord& record) {
        return {record.owner_scope, record.program_version_id, record.run_id, record.operation_id, record.attempt,
                record.binding_fingerprint};
    }

    std::unique_ptr<program::SQLiteProgramCoreToolGrantStore> store_;
    program::ProgramCoreToolGrantRecord                       record_;
};
#endif

template <class Owner>
class StoreInForkedChild : public ::testing::Test {
protected:
    std::string path() const { return dir_.file("store.sqlite"); }

private:
    graph::test_support::UniqueTempDir dir_{"ng-fork-store-"};
};

struct OwnerNames {
    template <class Owner>
    static std::string GetName(int) {
        return Owner::kName;
    }
};

using Owners = ::testing::Types<EvidenceLedgerOwner, ContextStoreOwner, HookJournalOwner, ReceiptStoreOwner,
                                ToolEffectBrokerOwner
#if defined(NEOGRAPH_STORE_FORK_TESTS_HAVE_PROGRAM_SQLITE)
                                ,
                                CoreToolGrantStoreOwner
#endif
                                >;
TYPED_TEST_SUITE(StoreInForkedChild, Owners, OwnerNames);

}  // namespace

TYPED_TEST(StoreInForkedChild, DestroyingTheCopyLeavesTheParentsConnectionOpen) {
    TypeParam owner(this->path());
    owner.write(1);
    watch_closing(owner.connection());
    const auto end = run_in_forked_child([&owner] {
        closes_seen.store(0, std::memory_order_relaxed);
        owner.destroy();
        return closes_seen.load(std::memory_order_relaxed) == 0 ? 0 : 1;
    });
    EXPECT_TRUE(end.ok()) << end << " (status 1: the child's copy closed the connection)";
    EXPECT_TRUE(owner.read_back(1));
    owner.write(2);
    EXPECT_TRUE(owner.read_back(2));
}

TYPED_TEST(StoreInForkedChild, EveryOperationOnTheCopyIsRefused) {
    TypeParam  owner(this->path());
    const auto operations = owner.operations();
    for (const auto& operation : operations) ASSERT_NO_THROW(operation.call()) << operation.name;
    const auto end = run_in_forked_child([&operations] { return refuse_every_operation(operations); });
    EXPECT_TRUE(end.ok()) << describe(operations, end);
}

TYPED_TEST(StoreInForkedChild, AnOperationIsRefusedWhileAParentThreadIsInsideTheStore) {
    TypeParam  owner(this->path());
    const auto operations = owner.operations();
    for (const auto& operation : operations) ASSERT_NO_THROW(operation.call()) << operation.name;
    ForkChildEnd end;
    {
        WriteLock lock(this->path());
        BusyGate  gate;
        ASSERT_EQ(sqlite3_busy_handler(owner.connection(), &wait_while_busy, &gate), SQLITE_OK);
        std::thread inside([&owner] {
            try {
                owner.write(1);
            } catch (...) {
            }
        });
        gate.inside.wait();
        end = run_in_forked_child([&operations] { return refuse_every_operation(operations); });
        gate.release.count_down();
        inside.join();
    }
    EXPECT_TRUE(end.ok()) << describe(operations, end);
    ASSERT_EQ(sqlite3_busy_timeout(owner.connection(), 5000), SQLITE_OK);
    owner.write(2);
    EXPECT_TRUE(owner.read_back(2));
}

TYPED_TEST(StoreInForkedChild, EveryOperationWorksInTheParentBeforeAndAfterAFork) {
    TypeParam  owner(this->path());
    const auto operations = owner.operations();
    for (const auto& operation : operations) ASSERT_NO_THROW(operation.call()) << operation.name << " before";
    const auto end = run_in_forked_child([] { return 0; });
    ASSERT_TRUE(end.ok()) << end;
    for (const auto& operation : operations) EXPECT_NO_THROW(operation.call()) << operation.name << " after";
    owner.write(1);
    EXPECT_TRUE(owner.read_back(1));
}

TEST(ProcessGeneration, StaysTheSameInTheParentAcrossAFork) {
    const auto before = detail::register_process_generation();
    const auto end    = run_in_forked_child([] { return 0; });
    ASSERT_TRUE(end.ok()) << end;
    EXPECT_EQ(detail::process_generation(), before);
}

TEST(ProcessGeneration, ChangesInAForkedChild) {
    const auto parent = detail::register_process_generation();
    const auto end    = run_in_forked_child([parent] { return detail::process_generation() != parent ? 0 : 1; });
    EXPECT_TRUE(end.ok()) << end << " (status 1: the child has the parent's generation)";
}

TEST(ProcessGeneration, ChangesAgainInAChildOfTheChild) {
    const auto parent = detail::register_process_generation();
    const auto end    = run_in_forked_child([parent] {
        const auto child      = detail::process_generation();
        const auto grandchild = run_in_forked_child([parent, child] {
            const auto own = detail::process_generation();
            return own != parent && own != child ? 0 : 1;
        });
        if (!grandchild.ok()) return 2;
        return child != parent ? 0 : 3;
    });
    EXPECT_TRUE(end.ok()) << end << " (status 2: the grandchild has an earlier generation; 3: the child has)";
}

TEST(StoreOpenedInAnotherProcess, IsAConfigurationVerdict) {
    static_assert(std::is_base_of_v<graph::ConfigurationFailure, StoreOpenedInAnotherProcess>);
    const auto verdict = detail::classify_node_failure(
        std::make_exception_ptr(StoreOpenedInAnotherProcess("SQLiteContextStore")), nullptr);
    EXPECT_EQ(verdict.failure_class, graph::NodeFailureClass::ConfigError);
    EXPECT_FALSE(verdict.retryable);
    EXPECT_FALSE(verdict.routable);
}

#endif
