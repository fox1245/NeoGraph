#include <gtest/gtest.h>

#include "strand_join_signal.h"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>

namespace {

using neograph::program::detail::StrandJoinSignal;

// Generous: only ever consumed when the join is lost, never on the passing path.
constexpr auto kWatchdog = std::chrono::seconds(10);

// A wait that returns is the expected outcome; the watchdog turns a lost wake-up into a
// bounded failure instead of a hung test binary.
bool settles(asio::io_context& io, const bool& returned) {
    if (io.stopped()) io.restart();
    io.run_for(kWatchdog);
    return returned;
}

}  // namespace

// The last worker signals on the strand before the join coroutine has initiated its wait.
// This is the ParallelMap/expansion-batch shape (one fast worker, strand handler completes
// first); a cancel-only timer signal is lost here and the join never returns.
TEST(StrandJoinSignalTest, SignalDeliveredBeforeWaitStillReleasesTheWaiter) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);

    asio::post(strand, [&] { join.signal(); });
    io.poll();  // the signal has fully run; no waiter exists yet

    bool returned = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            co_await join.wait();
            returned = true;
        },
        [](std::exception_ptr error) { EXPECT_FALSE(error); });
    EXPECT_TRUE(settles(io, returned))
        << "a signal delivered before wait() initiation must not be lost";
}

// The waiter parks first; the later signal wakes it, and only that signal does.
TEST(StrandJoinSignalTest, ParkedWaiterStaysSuspendedUntilSignalled) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);

    bool returned = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            co_await join.wait();
            returned = true;
        },
        [](std::exception_ptr error) { EXPECT_FALSE(error); });
    io.poll();  // the waiter has run up to its suspended wait
    EXPECT_FALSE(returned) << "wait() must not return before signal()";

    asio::post(strand, [&] { join.signal(); });
    EXPECT_TRUE(settles(io, returned)) << "signal() must wake a parked waiter";
}

// Cancellation is not producer completion. Releasing a private join here would let its
// parent destroy stack captures while ParallelMap/expansion workers still use them.
TEST(StrandJoinSignalTest, CancellingAParkedWaiterDoesNotReleaseItBeforeProducerCompletion) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);
    asio::cancellation_signal cancellation;

    bool returned = false;
    bool completed = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            co_await join.wait();
            returned = true;
        },
        asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error) {
            EXPECT_FALSE(error);
            completed = true;
        }));
    io.poll();
    ASSERT_FALSE(returned);
    ASSERT_FALSE(completed);

    asio::post(strand, [&] { cancellation.emit(asio::cancellation_type::all); });
    io.poll();  // Drain cancellation delivery, but deliberately do not complete the producer.
    EXPECT_FALSE(returned) << "cancelling the waiter must not stand in for producer completion";
    EXPECT_FALSE(completed) << "the private join must not terminate while producers are live";

    asio::post(strand, [&] { join.signal(); });
    EXPECT_TRUE(settles(io, returned));
    EXPECT_TRUE(completed);
}

TEST(StrandJoinSignalTest, PreviouslyCancelledCleanupWaiterStillDrainsProducers) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);
    asio::cancellation_signal cancellation;

    bool returned = false;
    bool completed = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            // A caller entering cleanup after cancellation must allow the nested cleanup
            // awaitable to start; wait() then clears the inherited state before parking.
            co_await asio::this_coro::throw_if_cancelled(false);
            cancellation.emit(asio::cancellation_type::all);
            co_await join.wait();
            returned = true;
        },
        asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error) {
            EXPECT_FALSE(error);
            completed = true;
        }));
    io.poll();
    EXPECT_FALSE(returned);
    EXPECT_FALSE(completed);

    asio::post(strand, [&] { join.signal(); });
    EXPECT_TRUE(settles(io, returned));
    EXPECT_TRUE(completed);
}

// An exhausted expansion task completes on the parent executor, so its notification must
// be posted to the join strand rather than touching the timer from that executor.
TEST(StrandJoinSignalTest, SignalPostedFromAnotherExecutorReleasesAParkedWaiter) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);

    bool returned = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            co_await join.wait();
            returned = true;
        },
        [](std::exception_ptr error) { EXPECT_FALSE(error); });
    io.poll();
    EXPECT_FALSE(returned);

    asio::post(io, [&] {
        asio::post(strand, [&] { join.signal(); });
    });
    EXPECT_TRUE(settles(io, returned));
}

// A repeated signal (e.g. a defensive second completion path) neither blocks a later waiter
// nor wakes it spuriously before the first signal.
TEST(StrandJoinSignalTest, RepeatedSignalKeepsLaterWaitsImmediate) {
    asio::io_context io;
    auto             strand = asio::make_strand(io);
    StrandJoinSignal join(strand);

    asio::post(strand, [&] {
        join.signal();
        join.signal();
    });
    io.poll();

    bool returned = false;
    asio::co_spawn(
        strand,
        [&]() -> asio::awaitable<void> {
            co_await join.wait();
            co_await join.wait();
            returned = true;
        },
        [](std::exception_ptr error) { EXPECT_FALSE(error); });
    EXPECT_TRUE(settles(io, returned));
}

namespace {

constexpr int kWorkers = 4;

struct Round {
    explicit Round(const asio::any_io_executor& strand) : join(strand) {}
    StrandJoinSignal join;
    // strand-only
    int remaining  = kWorkers;
    int completed  = 0;
    int seen_after = -1;
    std::promise<void> entered;
    std::promise<void> signalled;
    std::promise<void> released;
};

}  // namespace

// Real workers on a multi-threaded pool complete on the join strand exactly as the Program
// runtime does; the waiter is started before, concurrently with, or strictly after the last
// completion. Every interleaving must release the waiter, and only after all workers finished.
TEST(StrandJoinSignalTest, WorkersCompletingOnThePoolAlwaysReleaseTheJoin) {
    constexpr int kRounds = 450;
    asio::thread_pool pool(4);

    for (int round = 0; round < kRounds; ++round) {
        const auto start = round % 3;  // 0: waiter after signal, 1: waiter first, 2: racing
        auto       strand = asio::make_strand(pool.get_executor());
        auto       state  = std::make_shared<Round>(strand);
        auto       done   = state->released.get_future();
        auto       entered = state->entered.get_future();
        auto       signalled = state->signalled.get_future();

        const auto spawn_waiter = [&] {
            asio::co_spawn(
                strand,
                [state]() -> asio::awaitable<void> {
                    state->entered.set_value();
                    co_await state->join.wait();
                    state->seen_after = state->completed;
                },
                asio::bind_executor(strand, [state](std::exception_ptr error) {
                    if (error) state->released.set_exception(error);
                    else state->released.set_value();
                }));
        };
        const auto spawn_workers = [&] {
            for (int worker = 0; worker < kWorkers; ++worker) {
                asio::co_spawn(
                    pool.get_executor(), []() -> asio::awaitable<void> { co_return; },
                    asio::bind_executor(strand, [state](std::exception_ptr error) {
                        EXPECT_FALSE(error);
                        ++state->completed;
                        if (--state->remaining == 0) {
                            state->join.signal();
                            state->signalled.set_value();
                        }
                    }));
            }
        };

        if (start == 1) {
            spawn_waiter();
            ASSERT_EQ(entered.wait_for(kWatchdog), std::future_status::ready)
                << "round " << round << ": waiter never started";
        }
        spawn_workers();
        if (start == 0) {
            ASSERT_EQ(signalled.wait_for(kWatchdog), std::future_status::ready)
                << "round " << round << ": workers never completed";
        }
        if (start != 1) spawn_waiter();

        if (done.wait_for(kWatchdog) != std::future_status::ready) {
            pool.stop();
            FAIL() << "round " << round << " (start=" << start
                   << "): join waiter was never released (lost wake-up)";
        }
        EXPECT_NO_THROW(done.get()) << "round " << round << ": waiter failed";
        EXPECT_EQ(state->seen_after, kWorkers)
            << "round " << round << ": waiter released before every worker completed";
    }
    pool.join();
}
