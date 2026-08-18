// Coverage for `neograph::async::run_sync_pool` — the N-worker
// sync↔async bridge introduced in 3.0 to let sync callers drive an
// awaitable whose internals use parallel coroutines without
// serializing on a single-threaded executor.
//
// Pins:
//   * value and void specializations return / complete cleanly;
//   * exceptions thrown inside the awaitable rethrow on the caller;
//   * n_threads == 0 still runs (clamped to 1);
//   * a make_parallel_group can enter four blocking branches
//     concurrently on a 4-worker pool — i.e. CPU parallelism actually
//     materializes.

#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

using namespace std::chrono_literals;
using neograph::async::run_sync_pool;

TEST(RunSyncPool, ReturnsValue) {
    auto aw = []() -> asio::awaitable<int> { co_return 42; };
    EXPECT_EQ(run_sync_pool(aw(), 2), 42);
}

TEST(RunSyncPool, VoidSpecialization) {
    std::atomic<bool> ran{false};
    auto aw = [&]() -> asio::awaitable<void> {
        ran = true;
        co_return;
    };
    run_sync_pool(aw(), 2);
    EXPECT_TRUE(ran.load());
}

TEST(RunSyncPool, PropagatesException) {
    auto aw = []() -> asio::awaitable<int> {
        throw std::runtime_error("boom");
        co_return 0;
    };
    EXPECT_THROW(run_sync_pool(aw(), 2), std::runtime_error);
}

TEST(RunSyncPool, PropagatesExceptionVoid) {
    auto aw = []() -> asio::awaitable<void> {
        throw std::runtime_error("boom");
        co_return;
    };
    EXPECT_THROW(run_sync_pool(aw(), 2), std::runtime_error);
}

TEST(RunSyncPool, ZeroThreadsClampsToOne) {
    auto aw = []() -> asio::awaitable<int> { co_return 7; };
    EXPECT_EQ(run_sync_pool(aw(), 0), 7);
}

TEST(RunSyncPool, ParallelGroupActuallyParallelizes) {
    std::mutex              mutex;
    std::condition_variable ready;
    std::size_t             arrived   = 0;
    bool                    timed_out = false;

    auto aw = [&]() -> asio::awaitable<void> {
        auto ex = co_await asio::this_coro::executor;
        auto branch = [&](auto) -> asio::awaitable<void> {
            std::unique_lock lock(mutex);
            ++arrived;
            ready.notify_all();
            if (!ready.wait_for(lock, 5s, [&] { return arrived == 4 || timed_out; })) {
                timed_out = true;
                ready.notify_all();
            }
            co_return;
        };
        co_await asio::experimental::make_parallel_group(
            asio::co_spawn(ex, branch(0), asio::deferred),
            asio::co_spawn(ex, branch(1), asio::deferred),
            asio::co_spawn(ex, branch(2), asio::deferred),
            asio::co_spawn(ex, branch(3), asio::deferred))
            .async_wait(asio::experimental::wait_for_all(),
                        asio::use_awaitable);
    };

    run_sync_pool(aw(), 4);
    EXPECT_FALSE(timed_out) << "four worker branches did not overlap";
    EXPECT_EQ(arrived, 4U);
}
