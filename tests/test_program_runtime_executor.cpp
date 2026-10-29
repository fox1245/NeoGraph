// ProgramRuntime on an executor the host supplies (#557): no thread of the runtime's own, attempts, deadlines and
// waiters on that executor, the synchronous calls refused where they would hang a host loop, and an awaitable form
// of every call. The host here is a manual executor that the test steps from its own thread, as a JavaScript host
// runs its microtasks, so nothing waits and nothing runs on another thread.
#include <gtest/gtest.h>

#include "program_expansion_fixture.h"
#include "operation_lock.h"
#include "fixtures/manual_executor.h"
#include "fixtures/thread_census.h"
#include <neograph/program/core_tool_grant_store.h>

#include <neograph/async/host_loop.h>
#include <neograph/program/runtime.h>

#include <runtime/host.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/detached.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>

using namespace neograph;
using namespace neograph::program;
using namespace neograph::program_test;
using namespace std::chrono_literals;
using neograph::host_test::HostedRuntime;
using neograph::host_test::ManualExecutor;

namespace {

std::mutex                  executor_threads_mutex;
std::vector<std::thread::id> executor_node_threads;
std::atomic<int>            executor_nodes_waiting{0};

void note_node_thread() {
    std::lock_guard lock(executor_threads_mutex);
    executor_node_threads.push_back(std::this_thread::get_id());
}

std::vector<std::thread::id> node_threads() {
    std::lock_guard lock(executor_threads_mutex);
    return executor_node_threads;
}

void forget_node_threads() {
    std::lock_guard lock(executor_threads_mutex);
    executor_node_threads.clear();
}

class RecordNode final : public graph::GraphNode {
public:
    explicit RecordNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        note_node_thread();
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", "recorded"});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Waits on a timer for as long as the run lets it, and leaves when the run is cancelled.
class WaitNode final : public graph::GraphNode {
public:
    explicit WaitNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput input) override {
        note_node_thread();
        executor_nodes_waiting.fetch_add(1);
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(5s);
        async::note_deadline(co_await asio::this_coro::executor, timer.expiry());
        asio::error_code ended;
        co_await timer.async_wait(asio::bind_cancellation_slot(
            input.ctx.cancel_token->slot(), asio::redirect_error(asio::use_awaitable, ended)));
        executor_nodes_waiting.fetch_sub(1);
        if (ended) throw asio::system_error(ended);
        co_return graph::NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot executor_registry(ExecutionGuarantee guarantee = ExecutionGuarantee::Strict) {
    (void)guarantee;
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "executor-record", '1'),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<RecordNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "executor-wait", '2'),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<WaitNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

using ExecutorFixture = BasicAdmittedRuntime<executor_registry>;

RunBudget grant(std::uint64_t wall_time_ms = 10000) {
    return RunBudget{wall_time_ms, 1000, 1000, 1, 1, 20, 0, 0, 0};
}

ProgramInvocation invocation(std::string trace, std::uint64_t wall_time_ms = 10000) {
    return ProgramInvocation{json::object(), grant(wall_time_ms), std::move(trace), {}};
}

RuntimeConfig runtime_config(ExecutorFixture& fixture) {
    RuntimeConfig config{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1};
    return config;
}

// A transition store over the in-memory one whose backend "blocks for long": every operation an awaiting coroutine makes
// runs on a pool thread, as a PostgreSQL store's does. It records the threads its operations ran on, and can hold the
// first operation that ran on a pool thread.
class OffloadingJournal final : public SyncProgramTransitionStore {
public:
    OffloadingJournal() : inner_(std::make_shared<InMemoryProgramTransitionStore>()) {}

    std::set<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }
    // Holds the first operation that runs on a thread other than `a` and `b`, until release_gate().
    void hold_first_pool_operation(std::thread::id a, std::thread::id b) {
        std::lock_guard lock(mutex_);
        gate_armed_ = true;
        main_       = a;
        io_         = b;
    }
    bool wait_for_gate(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [this] { return gate_entered_; });
    }
    void release_gate() {
        std::lock_guard lock(mutex_);
        gate_released_ = true;
        changed_.notify_all();
    }

    std::optional<ProgramRunRecord> load_sync(std::string_view owner, std::string_view run) const override {
        enter();
        return inner_->load_sync(owner, run);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner, std::string_view run) const override {
        enter();
        return inner_->latest_sync(owner, run);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner, std::string_view run,
                                               std::uint64_t after) const override {
        enter();
        return inner_->load_events_sync(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner, std::string_view run,
                                                            std::uint64_t after) const override {
        enter();
        return inner_->load_effects_sync(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        enter();
        return inner_->load_javascript_commands_sync(owner, run, after);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        enter();
        return inner_->load_context_publications_sync(owner, run, after);
    }
    std::vector<HookOutboxEntry> load_hook_outbox_entries_sync(std::string_view owner,
                                                               std::string_view run) const override {
        enter();
        return inner_->load_hook_outbox_entries_sync(owner, run);
    }
    std::optional<MigrationPlan> load_migration_plan_sync(std::string_view owner,
                                                          std::string_view run) const override {
        enter();
        return inner_->load_migration_plan_sync(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                       std::string_view lineage) const override {
        enter();
        return inner_->load_lineage_sync(owner, lineage);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                           std::string_view run) const override {
        enter();
        return inner_->load_run_lineage_sync(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(std::string_view owner, std::string_view lineage,
                                                            std::string_view head) const override {
        enter();
        return inner_->load_lineage_head_sync(owner, lineage, head);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(std::string_view owner, std::string_view lineage,
                                                             std::uint64_t generation) const override {
        enter();
        return inner_->load_generation_sync(owner, lineage, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner, std::string_view lineage, std::uint64_t generation) const override {
        enter();
        return inner_->load_generation_initial_publication_sync(owner, lineage, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view owner, std::string_view run, std::string_view head) const override {
        enter();
        return inner_->load_graph_migration_capsule_sync(owner, run, head);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(std::string_view owner,
                                                                   std::string_view run) const override {
        enter();
        return inner_->load_execution_lease_sync(owner, run);
    }
    ProgramTransitionPublishResult compare_publish_sync(std::string_view owner, std::string_view expected,
                                                        std::uint64_t expected_fence,
                                                        ProgramTransitionPublication publication) override {
        enter();
        return inner_->compare_publish_sync(owner, expected, expected_fence, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner, std::string_view expected, std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        enter();
        return inner_->compare_publish_execution_sync(owner, expected, expected_fence, std::move(publication),
                                                      std::move(expected_lease), std::move(next_lease));
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner, std::string_view expected, std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence, const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& lease) override {
        enter();
        return inner_->compare_publish_graph_safe_point_sync(owner, expected, expected_fence, std::move(publication),
                                                             evidence, capsule, lease);
    }
    ProgramTransitionPublishResult compare_publish_takeover_sync(std::string_view owner,
                                                                 std::string_view run_id,
                                                                 std::string_view expected,
                                                                 std::uint64_t expected_fence) override {
        enter();
        return inner_->compare_publish_takeover_sync(owner, run_id, expected, expected_fence);
    }

protected:
    bool offloads() const noexcept override { return true; }

private:
    void enter() const {
        std::unique_lock lock(mutex_);
        const auto       self = std::this_thread::get_id();
        threads_.insert(self);
        if (gate_armed_ && self != main_ && self != io_ && !gate_entered_) {
            gate_entered_ = true;
            changed_.notify_all();
            changed_.wait_for(lock, 10s, [this] { return gate_released_; });
        }
    }

    std::shared_ptr<InMemoryProgramTransitionStore> inner_;
    mutable std::mutex                              mutex_;
    mutable std::condition_variable                 changed_;
    mutable std::set<std::thread::id>               threads_;
    mutable bool                                    gate_entered_ = false;
    bool                                            gate_armed_   = false;
    bool                                            gate_released_ = false;
    std::thread::id                                 main_;
    std::thread::id                                 io_;
};

// A transition store that is not synchronous: every operation completes after a short wait on the awaiting coroutine's
// executor, as a store whose backend answers through an event loop does.
class DelayedJournal final : public ProgramTransitionStore {
public:
    DelayedJournal() : inner_(std::make_shared<InMemoryProgramTransitionStore>()) {}

    asio::awaitable<std::optional<ProgramRunRecord>> load(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramJournalRecord>> latest(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->latest(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunPublicationHead>> load_run_publication_head(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_run_publication_head(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramEvent>> load_events(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await pause();
        co_return co_await inner_->load_events(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<ProgramEffectOutboxEntry>> load_effects(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await pause();
        co_return co_await inner_->load_effects(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<ProgramJavaScriptCommandJournalEntry>> load_javascript_commands(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await pause();
        co_return co_await inner_->load_javascript_commands(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::optional<ProgramCommandPublicationHead>> load_command_publication_head(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_command_publication_head(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramContextPublication>> load_context_publications(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await pause();
        co_return co_await inner_->load_context_publications(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<HookOutboxEntry>> load_hook_outbox_entries(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_hook_outbox_entries(owner_scope, run_id);
    }
    asio::awaitable<ProgramEffectiveRuntimeState> load_effective_runtime_state(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_effective_runtime_state(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramChildSynthesisRecord>> load_child_syntheses(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_child_syntheses(owner_scope, run_id);
    }
    asio::awaitable<std::optional<MigrationPlan>> load_migration_plan(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_migration_plan(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_lineage(std::string_view owner_scope, std::string_view lineage_id) const override {
        co_await pause();
        co_return co_await inner_->load_lineage(owner_scope, lineage_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_run_lineage(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_run_lineage(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_lineage_head(std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const override {
        co_await pause();
        co_return co_await inner_->load_lineage_head(owner_scope, lineage_id, head_id);
    }
    asio::awaitable<std::optional<ProgramRunGeneration>> load_generation(std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        co_await pause();
        co_return co_await inner_->load_generation(owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<ProgramTransitionPublication>> load_generation_initial_publication(std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        co_await pause();
        co_return co_await inner_->load_generation_initial_publication(owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<GraphMigrationCapsule>> load_graph_migration_capsule(std::string_view owner_scope, std::string_view source_run_id, std::string_view source_lineage_head_id) const override {
        co_await pause();
        co_return co_await inner_->load_graph_migration_capsule(owner_scope, source_run_id, source_lineage_head_id);
    }
    asio::awaitable<std::optional<ProgramExecutionLease>> load_execution_lease(std::string_view owner_scope, std::string_view run_id) const override {
        co_await pause();
        co_return co_await inner_->load_execution_lease(owner_scope, run_id);
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish(std::string_view owner_scope, std::string_view expected_journal_head, std::uint64_t expected_fence, ProgramTransitionPublication publication) override {
        co_await pause();
        co_return co_await inner_->compare_publish(owner_scope, expected_journal_head, expected_fence, std::move(publication));
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish_execution(std::string_view owner_scope, std::string_view expected_journal_head, std::uint64_t expected_fence, ProgramTransitionPublication publication, std::optional<ProgramExecutionLease> expected_lease, std::optional<ProgramExecutionLease> next_lease) override {
        co_await pause();
        co_return co_await inner_->compare_publish_execution(owner_scope, expected_journal_head, expected_fence, std::move(publication), std::move(expected_lease), std::move(next_lease));
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish_graph_safe_point(std::string_view owner_scope, std::string_view expected_journal_head, std::uint64_t expected_fence, ProgramTransitionPublication publication, const ProgramGraphSafePointEvidence& evidence, const GraphMigrationCapsule& capsule, const ProgramExecutionLease& execution_lease) override {
        co_await pause();
        co_return co_await inner_->compare_publish_graph_safe_point(owner_scope, expected_journal_head, expected_fence, std::move(publication), evidence, capsule, execution_lease);
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish_takeover(std::string_view owner_scope, std::string_view run_id, std::string_view expected_journal_head, std::uint64_t expected_fence) override {
        co_await pause();
        co_return co_await inner_->compare_publish_takeover(owner_scope, run_id, expected_journal_head, expected_fence);
    }

private:
    asio::awaitable<void> pause() const {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(1ms);
        // A host loop wakes for a deadline it has been told about.
        async::note_deadline(co_await asio::this_coro::executor, timer.expiry());
        co_await timer.async_wait(asio::use_awaitable);
    }

    std::shared_ptr<InMemoryProgramTransitionStore> inner_;
};

// co_spawn builds a default result for a failure, and a ProgramHandle has none: the handle is kept on the side.
asio::awaitable<int> start_and_keep(ProgramRuntime& runtime, ProgramVersion version, ProgramInvocation invocation,
                                    std::optional<ProgramHandle>& out) {
    out.emplace(co_await runtime.start_async("tenant:runtime", std::move(version), std::move(invocation)));
    co_return 0;
}

// A grant store that is not synchronous: every operation finishes after a short wait on the awaiting coroutine's executor.
class LaterGrantStore final : public ProgramCoreToolGrantStore {
public:
    asio::awaitable<ProgramCoreToolGrantAdmission> admit(const ProgramCoreToolGrantRecord&) override {
        co_await pause();
        co_return ProgramCoreToolGrantAdmission::Admitted;
    }
    asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(const ProgramCoreToolGrantContext&) const override {
        co_await pause();
        co_return std::nullopt;
    }
    asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext&) override {
        co_await pause();
        co_return false;
    }

private:
    static asio::awaitable<void> pause() {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(1ms);
        async::note_deadline(co_await asio::this_coro::executor, timer.expiry());
        co_await timer.async_wait(asio::use_awaitable);
    }
};

// Reads through a view that has no policy, from a handler of the loop: the way ProgramCatalog and the grant resolver
// read a store.
asio::awaitable<int> read_grant_in_a_turn(const BlockingProgramCoreToolGrantStore& view,
                                          ProgramCoreToolGrantContext context, std::optional<std::string>& verdict) {
    try {
        (void)view.load(context);
        verdict = "waited";
    } catch (const ProgramBlockingCallRefused& error) {
        verdict = error.operation();
    }
    co_return 0;
}

}  // namespace

// ---- A single-thread io_context the test runs on its own thread ----

TEST(ProgramRuntimeExecutorTest, AnInjectedIoContextRunsTheAttemptOnItsThread) {
    forget_node_threads();
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    const auto       io_id = io_thread.get_id();

    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    {
        ProgramRuntime runtime(std::move(config));
        auto           handle = runtime.start("tenant:runtime", version, invocation("trace-io"));
        const auto     result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    }
    const auto threads = node_threads();
    ASSERT_EQ(threads.size(), 1U);
    EXPECT_EQ(threads.front(), io_id);
    EXPECT_NE(threads.front(), std::this_thread::get_id());
    work.reset();
    io_thread.join();
}

TEST(ProgramRuntimeExecutorTest, AWallTimeDeadlineFiresOnTheInjectedExecutor) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-wait");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    const auto started = std::chrono::steady_clock::now();
    {
        ProgramRuntime runtime(std::move(config));
        auto           handle = runtime.start("tenant:runtime", version, invocation("trace-deadline", 200));
        const auto     result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
    work.reset();
    io_thread.join();
}

TEST(ProgramRuntimeExecutorTest, AWaitMadeOnTheExecutorsOwnThreadIsRefused) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-wait");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    ProgramRuntime runtime(std::move(config));
    auto           handle = runtime.start("tenant:runtime", version, invocation("trace-own-thread"));

    std::promise<std::string> verdict;
    asio::post(io, [&] {
        try {
            (void)handle.wait();
            verdict.set_value("waited");
        } catch (const ProgramBlockingCallRefused& error) {
            verdict.set_value(error.alternative());
        } catch (...) {
            verdict.set_value("other");
        }
    });
    EXPECT_EQ(verdict.get_future().get(), "ProgramHandle::wait_async");
    // Made from another thread, the same call waits, and ends when the run does.
    handle.cancel();
    EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Cancelled);
    work.reset();
    io_thread.join();
}

// run and migrate_graph wait for work that only the executor can do. Made on a thread that runs it they would wait for
// ever, so they are refused, and run is refused before it starts the Program.
TEST(ProgramRuntimeExecutorTest, RunAndMigrateGraphAreRefusedOnTheExecutorsOwnThread) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-wait");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    ProgramRuntime runtime(std::move(config));
    const int      waiting_before = executor_nodes_waiting.load();
    auto           handle = runtime.start("tenant:runtime", version, invocation("trace-own-thread-calls"));
    for (int tries = 0; executor_nodes_waiting.load() == waiting_before && tries < 5000; ++tries)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(executor_nodes_waiting.load(), waiting_before + 1);

    std::promise<std::vector<std::string>> verdicts;
    asio::post(io, [&] {
        std::vector<std::string> seen;
        try {
            (void)runtime.run("tenant:runtime", version, invocation("trace-own-thread-run"));
            seen.push_back("run: ran");
        } catch (const ProgramBlockingCallRefused& error) {
            seen.push_back(error.alternative());
        } catch (...) {
            seen.push_back("run: other");
        }
        try {
            (void)runtime.migrate_graph(handle, ProgramGraphMigrationTarget{version.id()});
            seen.push_back("migrate_graph: ran");
        } catch (const ProgramBlockingCallRefused& error) {
            seen.push_back(error.alternative());
        } catch (...) {
            seen.push_back("migrate_graph: other");
        }
        verdicts.set_value(std::move(seen));
    });
    EXPECT_EQ(verdicts.get_future().get(),
              (std::vector<std::string>{"ProgramRuntime::run_async", "ProgramRuntime::migrate_graph_async"}));
    // The refused run started nothing: the node of the first run is the only one waiting.
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(executor_nodes_waiting.load(), waiting_before + 1);
    handle.cancel();
    EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Cancelled);
    work.reset();
    io_thread.join();
}

// ---- The runtime's own pools, as before ----

TEST(ProgramRuntimeExecutorTest, WithoutAnExecutorTheRuntimeKeepsItsPools) {
    ExecutorFixture fixture(2);
    const auto      version = fixture.admit("executor-record");
    auto            handle  = fixture.runtime->start("tenant:runtime", version, invocation("trace-pools"));
    EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_NO_THROW((void)fixture.runtime->run("tenant:runtime", version, invocation("trace-pools-run")));
}

// ---- An executor that a host's event loop pumps ----

TEST(ProgramRuntimeExecutorTest, SynchronousCallsAreRefusedOnAHostPumpedExecutor) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    EXPECT_TRUE(async::is_host_pumped(config.executor));
    ProgramRuntime runtime(std::move(config));

    try {
        (void)runtime.start("tenant:runtime", version, invocation("trace-refused"));
        FAIL() << "a synchronous start was accepted";
    } catch (const ProgramBlockingCallRefused& error) {
        EXPECT_EQ(error.operation(), "ProgramRuntime::start");
        EXPECT_EQ(error.alternative(), "ProgramRuntime::start_async");
        EXPECT_NE(std::string(error.what()).find("start_async"), std::string::npos);
        EXPECT_NE(dynamic_cast<const graph::ConfigurationFailure*>(&error), nullptr);
    }
    EXPECT_THROW((void)runtime.run("tenant:runtime", version, invocation("trace-refused-run")),
                 ProgramBlockingCallRefused);
    EXPECT_THROW((void)runtime.reconnect("tenant:runtime", "run-absent"), ProgramBlockingCallRefused);
    // Nothing was started: the executor has nothing to run.
    EXPECT_FALSE(hosted.host->step());
}

TEST(ProgramRuntimeExecutorTest, AProgramRunsOnAHostPumpedExecutorWithoutAThread) {
    forget_node_threads();
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    const neograph::test::ThreadCensus census;
    ProgramRuntime runtime(std::move(config));

    bool                         done = false;
    std::exception_ptr           failure;
    std::optional<ProgramResult> result;
    hosted.loop.spawn(
        runtime.run_async("tenant:runtime", version, invocation("trace-host")),
        [&](std::exception_ptr error, ProgramResult value) {
            failure = error;
            if (!error) result = std::move(value);
            done = true;
        });
    ASSERT_TRUE(hosted.host->run_until([&] { return done; }));
    if (failure) std::rethrow_exception(failure);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed);

    // The node ran in a host turn, on this thread, and the runtime started no thread.
    const auto nodes = node_threads();
    ASSERT_EQ(nodes.size(), 1U);
    EXPECT_EQ(nodes.front(), std::this_thread::get_id());
    if (census.available()) {
        const auto started = census.started();
        EXPECT_TRUE(started.empty()) << "the runtime started " << started.size() << " thread(s)";
    }
}

TEST(ProgramRuntimeExecutorTest, AHandleCannotBeWaitedOnWhileTheHostLoopMustRunTheProgram) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-wait");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    ProgramRuntime runtime(std::move(config));

    std::optional<ProgramHandle> handle;
    std::exception_ptr           failure;
    hosted.loop.spawn(start_and_keep(runtime, version, invocation("trace-wait"), handle),
                      [&](std::exception_ptr error, int) { failure = error; });
    ASSERT_TRUE(hosted.host->run_until([&] { return handle.has_value() || failure; }));
    if (failure) std::rethrow_exception(failure);
    // The node waits on a timer that only a later turn of the host loop can fire. A blocking wait here would wait for
    // a turn that only this thread can run.
    try {
        (void)handle->wait();
        FAIL() << "a blocking wait was accepted";
    } catch (const ProgramBlockingCallRefused& error) {
        EXPECT_EQ(error.alternative(), "ProgramHandle::wait_async");
    }
    EXPECT_FALSE(handle->try_result().has_value());
    handle->cancel();
    ASSERT_TRUE(hosted.host->run_until([&] { return handle->try_result().has_value(); }));
    // With the result in, the same call returns it.
    EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Cancelled);
}

TEST(ProgramRuntimeExecutorTest, ADeadlineTimerFiresThroughTheHostLoop) {
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-wait");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    ProgramRuntime runtime(std::move(config));

    bool                         done = false;
    std::optional<ProgramResult> result;
    const auto                   started = std::chrono::steady_clock::now();
    hosted.loop.spawn(
        runtime.run_async("tenant:runtime", version, invocation("trace-host-deadline", 200)),
        [&](std::exception_ptr error, ProgramResult value) {
            if (!error) result = std::move(value);
            done = true;
        });
    ASSERT_TRUE(hosted.host->run_until([&] { return done; }));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::TimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
}

TEST(ProgramRuntimeExecutorTest, EveryCallHasAnAwaitableFormThatRunsOnTheExecutor) {
    forget_node_threads();
    ExecutorFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    ProgramRuntime runtime(std::move(config));

    bool               done = false;
    std::exception_ptr failure;
    std::string        run_id;
    std::optional<ProgramResult> first;
    std::optional<ProgramResult> reconnected;
    auto flow = [&]() -> asio::awaitable<int> {
        auto handle = co_await runtime.start_async("tenant:runtime", version, invocation("trace-flow"));
        run_id      = handle.run_id();
        first       = co_await handle.wait_async();
        auto again  = co_await runtime.reconnect_async("tenant:runtime", run_id);
        reconnected = co_await again.wait_async();
        co_return 0;
    };
    hosted.loop.spawn(flow(), [&](std::exception_ptr error, int) {
        failure = error;
        done    = true;
    });
    ASSERT_TRUE(hosted.host->run_until([&] { return done; }));
    if (failure) std::rethrow_exception(failure);
    ASSERT_TRUE(first && reconnected);
    EXPECT_EQ(first->status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(reconnected->status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(reconnected->run_id(), run_id);
}

// ---- Store calls the run awaits (#556) ----

TEST(ProgramRuntimeExecutorTest, AStoreThatOffloadsIsAwaitedByTheRunOnItsPoolThreads) {
    forget_node_threads();
    auto            journal = std::make_shared<OffloadingJournal>();
    ExecutorFixture fixture(1, {}, journal);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    const auto       io_id = io_thread.get_id();

    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    {
        ProgramRuntime runtime(std::move(config));
        auto           handle = runtime.start("tenant:runtime", version, invocation("trace-offload"));
        const auto     result = handle.wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    }
    work.reset();
    io_thread.join();

    // A read the attempt awaits ran on a pool thread: a thread of neither the caller nor the executor.
    std::size_t pool_threads = 0;
    for (const auto id : journal->threads())
        if (id != std::this_thread::get_id() && id != io_id) ++pool_threads;
    EXPECT_GE(pool_threads, 1U);
}

TEST(ProgramRuntimeExecutorTest, ARunWaitingForAStoreLeavesTheExecutorFreeForAnotherRun) {
    forget_node_threads();
    auto            journal = std::make_shared<OffloadingJournal>();
    ExecutorFixture fixture(1, {}, journal);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    asio::io_context io;
    auto             work = asio::make_work_guard(io);
    std::thread      io_thread([&] { io.run(); });
    journal->hold_first_pool_operation(std::this_thread::get_id(), io_thread.get_id());

    auto config     = runtime_config(fixture);
    config.executor = io.get_executor();
    {
        ProgramRuntime runtime(std::move(config));
        auto           first = runtime.start("tenant:runtime", version, invocation("trace-held"));
        // The first run's attempt is waiting for its store call on a pool thread.
        EXPECT_TRUE(journal->wait_for_gate(5s));

        // The executor has one thread. If the attempt were waiting on it, a second run could not run.
        auto second  = runtime.start("tenant:runtime", version, invocation("trace-free"));
        auto outcome = std::async(std::launch::async, [&] { return second.wait(); });
        const bool second_finished = outcome.wait_for(5s) == std::future_status::ready;
        journal->release_gate();
        EXPECT_TRUE(second_finished) << "the second run could not run while the first waited for its store";
        if (second_finished) EXPECT_EQ(outcome.get().status(), ProgramTerminalStatus::Completed);
        else outcome.wait();
        EXPECT_EQ(first.wait().status(), ProgramTerminalStatus::Completed);
    }
    work.reset();
    io_thread.join();
}

TEST(ProgramRuntimeExecutorTest, AStoreThatCompletesLaterServesAProgramRunByTheHostLoop) {
    ExecutorFixture fixture(1, {}, std::make_shared<DelayedJournal>());
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    HostedRuntime hosted;
    auto          config = runtime_config(fixture);
    config.executor      = hosted.loop.executor();
    ProgramRuntime runtime(std::move(config));

    // The runtime's entry points await their stores on the host loop (#557), so a store that completes in a later turn
    // is served: nothing waits for it, and nothing is refused.
    bool                         done = false;
    std::exception_ptr           failure;
    std::optional<ProgramResult> result;
    hosted.loop.spawn(runtime.run_async("tenant:runtime", version, invocation("trace-late")),
                      [&](std::exception_ptr error, ProgramResult value) {
                          failure = error;
                          if (!error) result = std::move(value);
                          done = true;
                      });
    ASSERT_TRUE(hosted.host->run_until([&] { return done; }));
    if (failure) std::rethrow_exception(failure);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed);

    // The synchronous form is still refused there, before it touches a store.
    try {
        (void)runtime.start("tenant:runtime", version, invocation("trace-late-sync"));
        FAIL() << "a synchronous start was accepted";
    } catch (const ProgramBlockingCallRefused& error) {
        EXPECT_EQ(error.operation(), "ProgramRuntime::start");
        EXPECT_EQ(error.alternative(), "ProgramRuntime::start_async");
    }
}

TEST(ProgramRuntimeExecutorTest, AStoreThatCompletesLaterIsRefusedBySynchronousCallsInsideAHostTurn) {
    // A runtime with its own pools, called from a handler of an unrelated host loop: the call would wait for a store that
    // only a later turn of that loop can complete, so it is refused before the store is touched.
    ExecutorFixture fixture(1, {}, std::make_shared<DelayedJournal>());
    const auto      version = fixture.admit("executor-record");

    HostedRuntime hosted;
    std::optional<std::string> verdict;
    auto call = [&]() -> asio::awaitable<int> {
        try {
            (void)fixture.runtime->start("tenant:runtime", version, invocation("trace-in-turn"));
            verdict = "started";
        } catch (const ProgramBlockingCallRefused& error) {
            verdict = error.operation();
        }
        co_return 0;
    };
    hosted.loop.spawn(call(), [](std::exception_ptr, int) {});
    ASSERT_TRUE(hosted.host->run_until([&] { return verdict.has_value(); }));
    EXPECT_EQ(*verdict, "ProgramRuntime::start");
}

TEST(ProgramRuntimeExecutorTest, AStoreThatCompletesLaterServesARuntimeWithItsOwnPools) {
    // Without an executor of the host's, a thread that waits for a store blocks nothing but itself, so the same store
    // that a host loop refuses completes the run: the entry points wait for it, and the attempt awaits it.
    ExecutorFixture fixture(1, {}, std::make_shared<DelayedJournal>());
    const auto      version = fixture.admit("executor-record");
    const auto      result  = fixture.runtime->run("tenant:runtime", version, invocation("trace-pools"));
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed);
    const auto record = fixture.journal->load("tenant:runtime", result.run_id());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->continuation().state, ContinuationState::Completed);
}

TEST(ProgramRuntimeExecutorTest, AViewWithoutAPolicyRefusesToWaitForAStoreInsideAHostTurn) {
    HostedRuntime hosted;
    const ProgramCoreToolGrantContext context{"tenant:runtime", "version-1", "run-1", "root", 1, "binding-1"};
    const BlockingProgramCoreToolGrantStore view(std::make_shared<LaterGrantStore>());

    // Called from the host's own thread, outside a turn, the view waits for the store, as a view always has.
    EXPECT_FALSE(view.load(context));

    // Called by a handler of the loop, it would block the thread that has to turn the loop: it refuses, with no policy.
    std::optional<std::string> verdict;
    hosted.loop.spawn(read_grant_in_a_turn(view, context, verdict), [](std::exception_ptr, int) {});
    ASSERT_TRUE(hosted.host->run_until([&] { return verdict.has_value(); }));
    EXPECT_EQ(*verdict, "ProgramCoreToolGrantStore::load");
}

// ---- The operation lock ----

TEST(ProgramOperationLockTest, GrantsInOrderAndNeverToTwoHoldersAtOnce) {
    neograph::program::detail::OperationLock lock;
    asio::io_context      io;
    std::vector<int>      order;
    int                   inside = 0;
    int                   peak   = 0;
    auto contender = [&](int id) -> asio::awaitable<void> {
        auto guard = co_await lock.lock_async();
        ++inside;
        peak = std::max(peak, inside);
        order.push_back(id);
        asio::steady_timer pause(co_await asio::this_coro::executor);
        pause.expires_after(2ms);
        co_await pause.async_wait(asio::use_awaitable);
        --inside;
    };
    for (int id = 0; id < 5; ++id) asio::co_spawn(io, contender(id), asio::detached);
    io.run();
    EXPECT_EQ(peak, 1);
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
    EXPECT_TRUE(lock.try_lock().has_value());
}

TEST(ProgramOperationLockTest, ABlockingCallerWaitsForAnAwaitingHolder) {
    neograph::program::detail::OperationLock lock;
    asio::io_context      io;
    auto                  work = asio::make_work_guard(io);
    std::thread           io_thread([&] { io.run(); });
    std::promise<void>    held;
    std::promise<void>    release;
    auto                  release_future = release.get_future().share();
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto guard = co_await lock.lock_async();
            held.set_value();
            asio::steady_timer pause(co_await asio::this_coro::executor);
            pause.expires_after(50ms);
            co_await pause.async_wait(asio::use_awaitable);
        },
        asio::detached);
    held.get_future().get();
    EXPECT_FALSE(lock.try_lock().has_value());
    const auto started = std::chrono::steady_clock::now();
    {
        auto guard = lock.lock_blocking();
        EXPECT_TRUE(guard.owns());
        EXPECT_GE(std::chrono::steady_clock::now() - started, 30ms);
    }
    EXPECT_TRUE(lock.try_lock().has_value());
    work.reset();
    io_thread.join();
}
