// The Program store interfaces are awaitable (#556): a store whose backend is synchronous completes an operation at
// once or on a pool thread, a caller that cannot suspend uses the blocking view, and that view waits for a store that
// completes later, or refuses when its policy says that waiting would hang the thread that has to complete the call.
// The vehicle is the grant store, the smallest of the five interfaces; the contract is the same for all of them.
#include <gtest/gtest.h>

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/store_call.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

using namespace neograph;
using namespace neograph::program;
using namespace std::chrono_literals;

namespace {

ProgramCoreToolGrantRecord grant_record(std::string grant_id = "grant-1") {
    return ProgramCoreToolGrantRecord{"tenant:store", "version-1", "run-1", "root", 1, "binding-1", std::move(grant_id)};
}

ProgramCoreToolGrantContext context_of(const ProgramCoreToolGrantRecord& record) {
    return ProgramCoreToolGrantContext{record.owner_scope,  record.program_version_id, record.run_id,
                                       record.operation_id, record.attempt,            record.binding_fingerprint};
}

// A store that keeps grants in a map and records the threads its synchronous operations ran on.
class MapGrantStore : public SyncProgramCoreToolGrantStore {
public:
    explicit MapGrantStore(bool offload = false) : offload_(offload) {}

    ProgramCoreToolGrantAdmission admit_sync(const ProgramCoreToolGrantRecord& record) override {
        note_thread();
        std::lock_guard lock(mutex_);
        const auto      [it, inserted] = grants_.emplace(record.grant_id, record);
        if (inserted) return ProgramCoreToolGrantAdmission::Admitted;
        return it->second.run_id == record.run_id ? ProgramCoreToolGrantAdmission::AlreadyPresent
                                                  : ProgramCoreToolGrantAdmission::Conflict;
    }
    std::optional<ProgramCoreToolGrantRecord> load_sync(const ProgramCoreToolGrantContext& context) const override {
        note_thread();
        std::lock_guard lock(mutex_);
        for (const auto& [id, record] : grants_)
            if (record.run_id == context.run_id && record.attempt == context.attempt) return record;
        return std::nullopt;
    }
    bool revoke_sync(const ProgramCoreToolGrantContext& context) override {
        note_thread();
        std::lock_guard lock(mutex_);
        for (auto it = grants_.begin(); it != grants_.end(); ++it)
            if (it->second.run_id == context.run_id) {
                grants_.erase(it);
                return true;
            }
        return false;
    }

    std::set<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }

protected:
    bool offloads() const noexcept override { return offload_; }

private:
    void note_thread() const {
        std::lock_guard lock(mutex_);
        threads_.insert(std::this_thread::get_id());
    }

    const bool                                  offload_;
    mutable std::mutex                          mutex_;
    mutable std::set<std::thread::id>           threads_;
    std::map<std::string, ProgramCoreToolGrantRecord> grants_;
};

// A store that is not synchronous: every operation completes after a timer on the executor of the awaiting coroutine.
class TimerGrantStore final : public ProgramCoreToolGrantStore {
public:
    explicit TimerGrantStore(std::chrono::milliseconds delay) : delay_(delay) {}

    asio::awaitable<ProgramCoreToolGrantAdmission> admit(const ProgramCoreToolGrantRecord& record) override {
        co_await pause();
        ++calls;
        co_return co_await inner_.admit(record);
    }
    asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(
        const ProgramCoreToolGrantContext& context) const override {
        co_await pause();
        ++calls;
        co_return co_await inner_.load(context);
    }
    asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext& context) override {
        co_await pause();
        ++calls;
        co_return co_await inner_.revoke(context);
    }

    mutable std::atomic<unsigned> calls{0};

private:
    asio::awaitable<void> pause() const {
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(delay_);
        co_await timer.async_wait(asio::use_awaitable);
    }

    std::chrono::milliseconds delay_;
    MapGrantStore             inner_;
};

// A store whose operations finish on a thread of their own, as a client library with its own event loop does.
class ForeignThreadGrantStore final : public ProgramCoreToolGrantStore {
public:
    ~ForeignThreadGrantStore() override {
        for (auto& worker : workers_) worker.join();
    }

    asio::awaitable<ProgramCoreToolGrantAdmission> admit(const ProgramCoreToolGrantRecord& record) override {
        co_await finish_elsewhere();
        co_return co_await inner_.admit(record);
    }
    asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(
        const ProgramCoreToolGrantContext& context) const override {
        co_await finish_elsewhere();
        co_return co_await inner_.load(context);
    }
    asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext& context) override {
        co_await finish_elsewhere();
        co_return co_await inner_.revoke(context);
    }

private:
    asio::awaitable<void> finish_elsewhere() const {
        const auto executor = co_await asio::this_coro::executor;
        co_await asio::async_initiate<decltype(asio::use_awaitable), void()>(
            [this, executor](auto handler) {
                auto completion = std::make_shared<decltype(handler)>(std::move(handler));
                std::lock_guard lock(mutex_);
                workers_.emplace_back([executor, completion] {
                    std::this_thread::sleep_for(20ms);
                    asio::post(executor, [completion] { (*completion)(); });
                });
            },
            asio::use_awaitable);
    }

    mutable std::mutex              mutex_;
    mutable std::vector<std::thread> workers_;
    MapGrantStore                   inner_;
};

// Counts the operations that reach the store.
class CountingGrantStore final : public MapGrantStore {
public:
    ProgramCoreToolGrantAdmission admit_sync(const ProgramCoreToolGrantRecord& record) override {
        ++calls;
        return MapGrantStore::admit_sync(record);
    }
    std::atomic<unsigned> calls{0};
};

}  // namespace

TEST(ProgramStoreAwaitableTest, ABlockingViewCallsASynchronousStoreDirectly) {
    auto store = std::make_shared<MapGrantStore>();
    BlockingProgramCoreToolGrantStore view(store);
    const auto record = grant_record();

    EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::Admitted);
    EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::AlreadyPresent);
    const auto loaded = view.load(context_of(record));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->grant_id, "grant-1");
    EXPECT_TRUE(view.revoke(context_of(record)));
    EXPECT_FALSE(view.load(context_of(record)));

    // The store's own operations ran on the calling thread: nothing was awaited.
    EXPECT_EQ(store->threads(), std::set<std::thread::id>{std::this_thread::get_id()});
}

TEST(ProgramStoreAwaitableTest, TheAwaitedFormOfASynchronousStoreCompletesWithoutSuspending) {
    auto             store = std::make_shared<MapGrantStore>();
    asio::io_context io;
    std::optional<ProgramCoreToolGrantAdmission> admitted;
    std::optional<ProgramCoreToolGrantRecord>    loaded;
    bool                                         body_finished = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            const auto record = grant_record();
            admitted          = co_await store->admit(record);
            loaded            = co_await store->load(context_of(record));
            body_finished     = true;
        },
        asio::detached);

    // One handler ran the whole body: neither operation suspended the coroutine to wait for anything.
    EXPECT_EQ(io.run_one(), 1U);
    EXPECT_TRUE(body_finished);
    ASSERT_TRUE(admitted);
    EXPECT_EQ(*admitted, ProgramCoreToolGrantAdmission::Admitted);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(store->threads(), std::set<std::thread::id>{std::this_thread::get_id()});
}

TEST(ProgramStoreAwaitableTest, AnOffloadingStoreRunsOnAPoolThreadAndResumesTheAwaiterOnItsExecutor) {
    auto             store = std::make_shared<MapGrantStore>(true);
    asio::io_context io;
    std::thread::id  resumed_on;
    std::optional<ProgramCoreToolGrantAdmission> admitted;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            admitted   = co_await store->admit(grant_record());
            resumed_on = std::this_thread::get_id();
        },
        asio::detached);
    io.run();

    ASSERT_TRUE(admitted);
    EXPECT_EQ(*admitted, ProgramCoreToolGrantAdmission::Admitted);
    EXPECT_EQ(resumed_on, std::this_thread::get_id());
    const auto threads = store->threads();
    ASSERT_EQ(threads.size(), 1U);
    EXPECT_NE(*threads.begin(), std::this_thread::get_id());
}

TEST(ProgramStoreAwaitableTest, ABlockingViewWaitsForAStoreThatCompletesOnATimer) {
    auto store = std::make_shared<TimerGrantStore>(30ms);
    BlockingProgramCoreToolGrantStore view(store);
    const auto record = grant_record();

    // Each call returns only after the store's operation has run, which is after its timer fired.
    EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::Admitted);
    EXPECT_EQ(store->calls.load(), 1U);
    EXPECT_TRUE(view.load(context_of(record)));
    EXPECT_EQ(store->calls.load(), 2U);
}

TEST(ProgramStoreAwaitableTest, ABlockingViewWaitsForAStoreThatCompletesOnAnotherThread) {
    auto store = std::make_shared<ForeignThreadGrantStore>();
    BlockingProgramCoreToolGrantStore view(store);
    const auto record = grant_record();

    EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::Admitted);
    const auto loaded = view.load(context_of(record));
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->run_id, "run-1");
}

TEST(ProgramStoreAwaitableTest, ARefusingPolicyStopsAnAwaitBeforeTheStoreIsReached) {
    BlockingStorePolicy policy;
    policy.refuse = [] { return std::string("it is called on the thread that runs the loop"); };
    const auto check_refused = [](const ProgramBlockingCallRefused& error) {
        EXPECT_NE(std::string(error.what()).find("it is called on the thread that runs the loop"), std::string::npos);
        EXPECT_NE(error.operation().find("admit"), std::string::npos);
    };

    // A store that is not synchronous is awaited, and the wait is refused.
    auto timer_store = std::make_shared<TimerGrantStore>(1ms);
    BlockingProgramCoreToolGrantStore awaiting(timer_store, policy);
    try {
        (void)awaiting.admit(grant_record());
        ADD_FAILURE() << "the call was not refused";
    } catch (const ProgramBlockingCallRefused& error) {
        check_refused(error);
    }
    EXPECT_EQ(timer_store->calls.load(), 0U);

    // A synchronous store is awaited too when the view is asked to (the conformance suite does), and is refused alike.
    auto counting = std::make_shared<CountingGrantStore>();
    policy.await_synchronous_stores = true;
    BlockingProgramCoreToolGrantStore forced(counting, policy);
    try {
        (void)forced.admit(grant_record());
        ADD_FAILURE() << "the call was not refused";
    } catch (const ProgramBlockingCallRefused& error) {
        check_refused(error);
    }
    EXPECT_EQ(counting->calls.load(), 0U);
}

TEST(ProgramStoreAwaitableTest, ARefusingPolicyDoesNotStopACallThatCompletesAtOnce) {
    // A policy refuses a wait. A synchronous store never makes the caller wait for a loop, so it is called.
    auto                counting = std::make_shared<CountingGrantStore>();
    BlockingStorePolicy policy;
    policy.refuse = [] { return std::string("refused"); };
    BlockingProgramCoreToolGrantStore view(counting, policy);
    EXPECT_EQ(view.admit(grant_record()), ProgramCoreToolGrantAdmission::Admitted);
    EXPECT_EQ(counting->calls.load(), 1U);
}

TEST(ProgramStoreAwaitableTest, AViewThatAwaitsASynchronousStoreGetsTheSameAnswers) {
    for (const bool offload : {false, true}) {
        auto                store = std::make_shared<MapGrantStore>(offload);
        BlockingStorePolicy policy;
        policy.await_synchronous_stores = true;
        BlockingProgramCoreToolGrantStore view(store, policy);
        const auto record = grant_record();

        EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::Admitted);
        EXPECT_EQ(view.admit(record), ProgramCoreToolGrantAdmission::AlreadyPresent);
        EXPECT_TRUE(view.load(context_of(record)));
        EXPECT_TRUE(view.revoke(context_of(record)));
        EXPECT_FALSE(view.load(context_of(record)));
        if (offload)
            EXPECT_EQ(store->threads().count(std::this_thread::get_id()), 0U);
        else
            EXPECT_EQ(store->threads(), std::set<std::thread::id>{std::this_thread::get_id()});
    }
}
