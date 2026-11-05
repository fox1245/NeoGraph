// A ProgramHandle kept after the execution context of its runtime's executor is gone. The runtime and its handles hold
// the executor until that context shuts down, which it does as it is destroyed; the shutdown releases every hold, and
// from then on a handle answers from the state it holds and never reaches the context.
//
// A to E and I run the scenarios themselves, on a thread_pool that the test destroys, and check the answers. F to H
// count what reaches the executor once its context has shut down, so they show it in any build: an executor that counts
// its live copies, and the work and context lookups it receives, over an io_context whose services the test shuts down
// before it destroys them. J checks that the waits, which make their strands as they need them, are all woken.
#include <gtest/gtest.h>

#include "program_expansion_fixture.h"

#include <neograph/program/runtime.h>

#include <asio/co_spawn.hpp>
#include <asio/execution/context.hpp>
#include <asio/execution/context_as.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/prefer.hpp>
#include <asio/query.hpp>
#include <asio/require.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace neograph;
using namespace neograph::program;
using namespace neograph::program_test;

namespace {

class LifetimeRecordNode final : public graph::GraphNode {
public:
    explicit LifetimeRecordNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", "recorded"});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// A gate the test opens: a node that passes it runs only once the test has opened it.
struct LifetimeGate {
    std::mutex              mutex;
    std::condition_variable opened;
    bool                    open = false;

    void close() {
        std::lock_guard lock(mutex);
        open = false;
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        opened.notify_all();
    }
    void pass() {
        std::unique_lock lock(mutex);
        opened.wait(lock, [this] { return open; });
    }
};
LifetimeGate lifetime_gate;

// Writes like LifetimeRecordNode once the test has opened the gate, so the run cannot end before then.
class LifetimeGatedNode final : public graph::GraphNode {
public:
    explicit LifetimeGatedNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        lifetime_gate.pass();
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", "recorded"});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Asks for an answer, so the run pauses: its result is Interrupted.
class LifetimePauseNode final : public graph::GraphNode {
public:
    explicit LifetimePauseNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput input) override {
        if (!input.ctx.resume_value) throw graph::NodeInterrupt("approval required", json{{"request", "approve"}});
        co_return graph::NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot lifetime_registry(ExecutionGuarantee guarantee = ExecutionGuarantee::Strict) {
    (void)guarantee;
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "executor-record", '1'),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<LifetimeRecordNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "executor-pause", '2'),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<LifetimePauseNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "executor-gated", '4'),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<LifetimeGatedNode>(name);
        },
        json{{"type", "object"}},
        json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "runtime-overwrite", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

using LifetimeFixture = BasicAdmittedRuntime<lifetime_registry>;

ProgramInvocation lifetime_invocation(std::string trace) {
    return ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}, std::move(trace), {}};
}

RuntimeConfig lifetime_config(LifetimeFixture& fixture) {
    return RuntimeConfig{fixture.catalog, fixture.checkpoints, {}, fixture.journal, 1};
}

// Awaits `work` on an io_context of its own, which this thread runs until nothing is left: an executor that is not
// the runtime's.
template <typename T>
T await_elsewhere(asio::awaitable<T> work) {
    asio::io_context   elsewhere;
    std::optional<T>   value;
    std::exception_ptr failure;
    asio::co_spawn(elsewhere, std::move(work), [&](std::exception_ptr error, T result) {
        failure = error;
        if (!error) value.emplace(std::move(result));
    });
    elsewhere.run();
    if (failure) std::rethrow_exception(failure);
    if (!value) throw std::runtime_error("the awaited work was destroyed before it finished");
    return std::move(*value);
}

// A completed handle answers from what it holds: its result, its events, a wait, a cancellation that finds the run
// over, and an awaited wait on an executor that is not the runtime's.
void expect_answers_of_a_completed_run(ProgramHandle& handle) {
    const auto held = handle.try_result();
    ASSERT_TRUE(held);
    EXPECT_EQ(held->status(), ProgramTerminalStatus::Completed);
    EXPECT_FALSE(handle.events_after(0).empty());
    EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_FALSE(handle.cancel());
    EXPECT_EQ(await_elsewhere(handle.wait_async()).status(), ProgramTerminalStatus::Completed);
}

// ---- A census of a host's executor ----

// What reached a host's executor: its live copies, and the work and context lookups it received after its context had
// shut down.
struct ExecutorCensus {
    std::atomic<long> copies{0};
    std::atomic<bool> context_shut_down{false};
    std::atomic<int>  touches_after_shutdown{0};
};

// An executor that counts its live copies and the work and context lookups it receives after its context has shut
// down; the work runs on `Inner`. Every other property is forwarded, as an executor adapter does.
template <typename Inner>
class CensusExecutor {
public:
    CensusExecutor(Inner inner, std::shared_ptr<ExecutorCensus> census) noexcept
        : inner_(std::move(inner)), census_(std::move(census)) {
        ++census_->copies;
    }
    CensusExecutor(const CensusExecutor& other) noexcept : inner_(other.inner_), census_(other.census_) {
        ++census_->copies;
    }
    CensusExecutor(CensusExecutor&& other) noexcept : inner_(other.inner_), census_(other.census_) {
        ++census_->copies;
    }
    CensusExecutor& operator=(const CensusExecutor& other) noexcept {
        inner_ = other.inner_;
        if (census_ != other.census_) {
            --census_->copies;
            census_ = other.census_;
            ++census_->copies;
        }
        return *this;
    }
    CensusExecutor& operator=(CensusExecutor&& other) noexcept {
        return *this = static_cast<const CensusExecutor&>(other);
    }
    ~CensusExecutor() { --census_->copies; }

    template <typename Function>
    void execute(Function&& function) const {
        // Work given to a context that has shut down is dropped, as the context itself drops it.
        if (touched_after_shutdown()) return;
        inner_.execute(std::forward<Function>(function));
    }
    asio::execution_context& query(asio::execution::context_t) const noexcept {
        (void)touched_after_shutdown();
        return asio::query(inner_, asio::execution::context);
    }
    asio::execution_context& query(asio::execution::context_as_t<asio::execution_context&>) const noexcept {
        (void)touched_after_shutdown();
        return asio::query(inner_, asio::execution::context);
    }
    template <typename Property>
        requires(!std::is_same_v<Property, asio::execution::context_t> &&
                 !std::is_same_v<Property, asio::execution::context_as_t<asio::execution_context&>>)
    auto query(const Property& property) const
        noexcept(noexcept(asio::query(std::declval<const Inner&>(), property)))
            -> decltype(asio::query(std::declval<const Inner&>(), property)) {
        return asio::query(inner_, property);
    }
    template <typename Property>
        requires asio::can_require<const Inner&, const Property&>::value
    auto require(const Property& property) const
        -> CensusExecutor<std::decay_t<asio::require_result_t<const Inner&, const Property&>>> {
        return {asio::require(inner_, property), census_};
    }
    template <typename Property>
        requires asio::can_prefer<const Inner&, const Property&>::value
    auto prefer(const Property& property) const
        -> CensusExecutor<std::decay_t<asio::prefer_result_t<const Inner&, const Property&>>> {
        return {asio::prefer(inner_, property), census_};
    }

    friend bool operator==(const CensusExecutor& left, const CensusExecutor& right) noexcept {
        return left.census_ == right.census_ && left.inner_ == right.inner_;
    }
    friend bool operator!=(const CensusExecutor& left, const CensusExecutor& right) noexcept {
        return !(left == right);
    }

private:
    bool touched_after_shutdown() const noexcept {
        if (!census_->context_shut_down.load()) return false;
        ++census_->touches_after_shutdown;
        return true;
    }

    Inner                           inner_;
    std::shared_ptr<ExecutorCensus> census_;
};

// An io_context whose services the test shuts down before it destroys the context. The destructor does the same first,
// so the runtime lets go of the executor there as it would in the destructor, and what is left can still be counted
// while the context exists.
class ShutdownIoContext final : public asio::io_context {
public:
    void shut_down() { asio::execution_context::shutdown(); }
};

// A host with a census executor over an io_context, run by one thread of the host's until the test drains it (or not
// run at all), and a thread test that reports the calls made after the context has shut down.
class CensusHost {
public:
    explicit CensusHost(bool run = true) {
        if (!run) return;
        work_.emplace(io_.get_executor());
        thread_ = std::thread([this] { io_.run(); });
    }
    ~CensusHost() { drain(); }

    asio::any_io_executor executor() {
        return CensusExecutor<asio::io_context::executor_type>(io_.get_executor(), census);
    }
    // ProgramExecutorIdentity::running_in_this_thread of the host; it holds `thread_test_token` while it lives.
    std::function<bool()> thread_test() {
        return [this, token = thread_test_token] {
            if (census->context_shut_down.load()) ++thread_tests_after_shutdown;
            return io_.get_executor().running_in_this_thread();
        };
    }
    // Lets the host's thread finish what the context has to run, and stop.
    void drain() {
        work_.reset();
        if (thread_.joinable()) thread_.join();
    }
    // Shuts the context's services down, as its destructor does first, and counts from then on.
    void shut_down() {
        drain();
        io_.shut_down();
        census->context_shut_down.store(true);
    }

    std::shared_ptr<ExecutorCensus> census            = std::make_shared<ExecutorCensus>();
    std::shared_ptr<int>            thread_test_token = std::make_shared<int>(0);
    std::atomic<int>                thread_tests_after_shutdown{0};

private:
    ShutdownIoContext                                                         io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_;
    std::thread                                                               thread_;
};

}  // namespace

// ---- The scenarios, on a thread_pool the host destroys ----

TEST(ProgramHandleExecutorLifetimeTest, A_HandleOutlivesTheInjectedPool) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    auto pool = std::make_unique<asio::thread_pool>(1);
    std::optional<ProgramHandle> handle;
    {
        auto config     = lifetime_config(fixture);
        config.executor = pool->get_executor();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-a")));
        EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    }
    pool->join();
    pool.reset();
    handle.reset();  // the last reference to the run's control goes after the pool's context
}

TEST(ProgramHandleExecutorLifetimeTest, B_HandleDestroyedBeforeThePool) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    auto pool = std::make_unique<asio::thread_pool>(1);
    std::optional<ProgramHandle> handle;
    {
        auto config     = lifetime_config(fixture);
        config.executor = pool->get_executor();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-b")));
        EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    }
    handle.reset();
    pool->join();
    pool.reset();
}

TEST(ProgramHandleExecutorLifetimeTest, C_RuntimeOwnPoolsHandleOutlivesRuntime) {
    LifetimeFixture fixture(2);
    const auto version = fixture.admit("executor-record");
    std::optional<ProgramHandle> handle;
    handle.emplace(fixture.runtime->start("tenant:runtime", version, lifetime_invocation("trace-c")));
    EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    fixture.runtime.reset();  // the runtime's own pools go with it
    expect_answers_of_a_completed_run(*handle);
    handle.reset();
}

TEST(ProgramHandleExecutorLifetimeTest, D_HandleOutlivesAStrandOfTheInjectedPool) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    auto pool = std::make_unique<asio::thread_pool>(1);
    std::optional<ProgramHandle> handle;
    {
        auto config     = lifetime_config(fixture);
        config.executor = asio::make_strand(pool->get_executor());
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-d")));
        EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    }
    pool->join();
    pool.reset();
    expect_answers_of_a_completed_run(*handle);
    handle.reset();  // every copy of the strand the runtime kept would end here, after the pool's context
}

TEST(ProgramHandleExecutorLifetimeTest, E_AHandleIsUsedAfterTheInjectedPoolIsGone) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    auto pool = std::make_unique<asio::thread_pool>(1);
    std::optional<ProgramHandle> handle;
    {
        auto config     = lifetime_config(fixture);
        config.executor = pool->get_executor();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-e")));
        EXPECT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    }
    pool->join();
    pool.reset();
    expect_answers_of_a_completed_run(*handle);
    handle.reset();
}

// ---- What reaches the executor once its context has shut down ----

TEST(ProgramHandleExecutorLifetimeTest, F_TheContextsShutdownReleasesEveryHoldOnTheExecutor) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    CensusHost                   host;
    std::optional<ProgramHandle> handle;
    {
        auto config                                     = lifetime_config(fixture);
        config.executor                                 = host.executor();
        config.executor_identity.running_in_this_thread = host.thread_test();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-f")));
        ASSERT_EQ(handle->wait().status(), ProgramTerminalStatus::Completed);
    }
    host.drain();
    // While the context lives, the handle holds the executor and the host's thread test: the census sees what the
    // shutdown is about to take back.
    EXPECT_GT(host.census->copies.load(), 0);
    EXPECT_GT(host.thread_test_token.use_count(), 1);
    host.shut_down();
    EXPECT_EQ(host.census->copies.load(), 0) << "copies of the executor outlived the shutdown of its context";
    EXPECT_EQ(host.thread_test_token.use_count(), 1) << "the host's thread test outlived the shutdown of its context";

    expect_answers_of_a_completed_run(*handle);
    handle.reset();
    EXPECT_EQ(host.census->touches_after_shutdown.load(), 0);
    EXPECT_EQ(host.thread_tests_after_shutdown.load(), 0);
}

TEST(ProgramHandleExecutorLifetimeTest, G_APausedRunIsCancelledThroughTheStoreAfterTheContextIsGone) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-pause");

    CensusHost                   host;
    std::optional<ProgramHandle> handle;
    {
        auto config                                     = lifetime_config(fixture);
        config.executor                                 = host.executor();
        config.executor_identity.running_in_this_thread = host.thread_test();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-g")));
        ASSERT_EQ(handle->wait().status(), ProgramTerminalStatus::Interrupted);
    }
    host.drain();
    EXPECT_GT(host.census->copies.load(), 0);
    host.shut_down();
    EXPECT_EQ(host.census->copies.load(), 0) << "copies of the executor outlived the shutdown of its context";

    // The Cancelled state of a paused run is the transition store's to record, which this thread does.
    EXPECT_TRUE(handle->cancel());
    const auto held = handle->try_result();
    ASSERT_TRUE(held);
    EXPECT_EQ(held->status(), ProgramTerminalStatus::Cancelled);
    const auto record = fixture.journal->load("tenant:runtime", handle->run_id());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->continuation().state, ContinuationState::Cancelled);
    EXPECT_FALSE(handle->cancel());
    EXPECT_EQ(await_elsewhere(handle->wait_async()).status(), ProgramTerminalStatus::Cancelled);

    handle.reset();
    EXPECT_EQ(host.census->touches_after_shutdown.load(), 0);
    EXPECT_EQ(host.thread_tests_after_shutdown.load(), 0);
}

TEST(ProgramHandleExecutorLifetimeTest, H_AWaitThatNeedsTheDestroyedExecutorIsRefused) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-record");

    CensusHost                   host(/*run=*/false);  // the run's attempt waits in the context's queue
    std::optional<ProgramHandle> handle;
    {
        auto config                                     = lifetime_config(fixture);
        config.executor                                 = host.executor();
        config.executor_identity.running_in_this_thread = host.thread_test();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-h")));
    }
    // The runtime's destruction cancelled the run in memory; its attempt still waits in the queue.
    EXPECT_GT(host.census->copies.load(), 0);
    host.shut_down();  // destroys the attempt that never ran: the run cannot end any more
    EXPECT_EQ(host.census->copies.load(), 0) << "copies of the executor outlived the shutdown of its context";

    EXPECT_FALSE(handle->try_result());
    try {
        (void)await_elsewhere(handle->wait_async());
        ADD_FAILURE() << "wait_async waited for a run whose executor's context is gone";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_EXECUTOR_DESTROYED");
    }
    try {
        (void)handle->wait();
        ADD_FAILURE() << "wait returned for a run whose executor's context is gone";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_EXECUTOR_DESTROYED");
    }
    EXPECT_FALSE(handle->cancel());  // the run is cancelled already

    handle.reset();
    EXPECT_EQ(host.census->touches_after_shutdown.load(), 0);
    EXPECT_EQ(host.thread_tests_after_shutdown.load(), 0);
}

TEST(ProgramHandleExecutorLifetimeTest, I_APausedRunIsCancelledAfterTheInjectedPoolIsGone) {
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-pause");

    auto pool = std::make_unique<asio::thread_pool>(1);
    std::optional<ProgramHandle> handle;
    {
        auto config     = lifetime_config(fixture);
        config.executor = pool->get_executor();
        ProgramRuntime runtime(std::move(config));
        handle.emplace(runtime.start("tenant:runtime", version, lifetime_invocation("trace-i")));
        ASSERT_EQ(handle->wait().status(), ProgramTerminalStatus::Interrupted);
    }
    pool->join();
    pool.reset();

    EXPECT_TRUE(handle->cancel());
    const auto held = handle->try_result();
    ASSERT_TRUE(held);
    EXPECT_EQ(held->status(), ProgramTerminalStatus::Cancelled);
    const auto record = fixture.journal->load("tenant:runtime", handle->run_id());
    ASSERT_TRUE(record);
    EXPECT_EQ(record->continuation().state, ContinuationState::Cancelled);
    EXPECT_EQ(await_elsewhere(handle->wait_async()).status(), ProgramTerminalStatus::Cancelled);
    handle.reset();
}

// Many awaited waits of one run, started on threads of their own before the run can end, and woken as the run publishes
// its end on the threads of its pool: a wait that asks before the end parks on a strand of its own and must be woken; one
// that asks after it is answered at once. Every one of them ends with the run's result.
TEST(ProgramHandleExecutorLifetimeTest, J_EveryAwaitedWaitOfARunIsWokenWhenTheRunEnds) {
    constexpr int kRuns    = 20;
    constexpr int kWaiters = 16;
    LifetimeFixture fixture(1);
    fixture.runtime.reset();
    const auto version = fixture.admit("executor-gated");

    asio::thread_pool pool(4);
    auto              config = lifetime_config(fixture);
    config.executor          = pool.get_executor();
    ProgramRuntime runtime(std::move(config));
    // Opens the gate however the test ends, before a pool that waits for the run is joined.
    struct OpenAtExit {
        ~OpenAtExit() { lifetime_gate.release(); }
    };
    OpenAtExit open_before_the_runtime_pool;
    for (int run = 0; run < kRuns; ++run) {
        lifetime_gate.close();
        auto handle = runtime.start("tenant:runtime", version, lifetime_invocation("trace-j-" + std::to_string(run)));
        asio::thread_pool waiters(2);
        OpenAtExit        open_before_the_waiters;
        std::latch        started(kWaiters);
        std::atomic<int>  completed{0};
        std::atomic<int>  failed{0};
        for (int waiter = 0; waiter < kWaiters; ++waiter) {
            asio::co_spawn(
                waiters,
                [&]() -> asio::awaitable<void> {
                    started.count_down();
                    const auto result = co_await handle.wait_async();
                    if (result.status() == ProgramTerminalStatus::Completed)
                        ++completed;
                    else
                        ++failed;
                },
                [&](std::exception_ptr error) {
                    if (error) ++failed;
                });
        }
        // Every waiter has started before the run can end.
        started.wait();
        EXPECT_FALSE(handle.try_result());
        lifetime_gate.release();
        waiters.join();
        EXPECT_EQ(completed.load(), kWaiters) << "run " << run;
        EXPECT_EQ(failed.load(), 0) << "run " << run;
    }
}
