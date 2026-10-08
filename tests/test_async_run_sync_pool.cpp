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
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

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
            co_await asio::post(ex, asio::use_awaitable);
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
TEST(RunSync, RepeatedTrackedPreferenceOwnsExecutorAfterSourceRelease) {
    auto operation = []() -> asio::awaitable<int> {
        auto current = co_await asio::this_coro::executor;
        auto first = asio::prefer(current, asio::execution::outstanding_work.tracked);
        auto retained = asio::prefer(first, asio::execution::outstanding_work.tracked);
        first = asio::any_io_executor{};
        co_await asio::post(retained, asio::use_awaitable);
        co_return 71;
    };
    EXPECT_EQ(neograph::async::run_sync(operation()), 71);
}


namespace {

class PostedHandlerFailure : public std::runtime_error {
public:
    PostedHandlerFailure() : std::runtime_error("side handler failed") {}
};

struct PendingFrameProbe {
    bool entered = false;
    bool destroyed = false;
    bool aborted = false;
    bool caller_thread = false;
    std::thread::id caller = std::this_thread::get_id();
};

asio::awaitable<int> pending_frame_with_throwing_handler(PendingFrameProbe& probe) {
    struct FrameLifetime {
        PendingFrameProbe& probe;
        ~FrameLifetime() { probe.destroyed = true; }
    } lifetime{probe};
    probe.entered = true;
    probe.caller_thread = std::this_thread::get_id() == probe.caller;
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(5s);
    asio::post(executor, [] { throw PostedHandlerFailure{}; });
    try {
        co_await timer.async_wait(asio::use_awaitable);
    } catch (const asio::system_error& error) {
        probe.aborted = error.code() == asio::error::operation_aborted;
        throw;
    }
    co_return 42;
}

asio::awaitable<void> pending_void_frame_with_throwing_handler(PendingFrameProbe& probe) {
    static_cast<void>(co_await pending_frame_with_throwing_handler(probe));
}

void expect_pending_frame_drained(const PendingFrameProbe& probe) {
    EXPECT_TRUE(probe.entered);
    EXPECT_TRUE(probe.caller_thread);
    EXPECT_TRUE(probe.aborted) << "the owned timer must be cancelled, not abandoned";
    EXPECT_TRUE(probe.destroyed) << "the suspended frame must finish before run_sync throws";
}

struct LateCleanupProbe {
    int released = 0;
    bool completed = false;
    bool cancellation_notified = false;
};

// An asynchronous consumer may retain its cancellation handler until the
// bridge releases the completed slot. This real handler's RAII cleanup posts
// a final notification; no timer, mock executor or internal test hook is used.
struct NotifyOnSlotRelease {
    NotifyOnSlotRelease(asio::any_io_executor executor, LateCleanupProbe& probe,
                        neograph::graph::CancelToken* cancel, bool stop_first)
        : executor(std::move(executor)), probe(probe), cancel(cancel),
          stop_first(stop_first) {}

    ~NotifyOnSlotRelease() {
        ++probe.released;
        if (cancel) cancel->cancel();
        if (stop_first) {
            auto& context = static_cast<asio::io_context&>(
                asio::query(executor, asio::execution::context));
            asio::post(executor, [&context] { context.stop(); });
        }
        asio::post(executor, [probe = &probe] { probe->completed = true; });
    }

    void operator()(asio::cancellation_type_t type) noexcept {
        if (type != asio::cancellation_type::none) probe.cancellation_notified = true;
    }

    asio::any_io_executor executor;
    LateCleanupProbe& probe;
    neograph::graph::CancelToken* cancel;
    bool stop_first;
};

asio::awaitable<int> notify_after_completed_slot_release(
    LateCleanupProbe& probe, neograph::graph::CancelToken* cancel = nullptr,
    bool stop_first = false) {
    auto executor = co_await asio::this_coro::executor;
    auto state = co_await asio::this_coro::cancellation_state;
    state.slot().emplace<NotifyOnSlotRelease>(
        std::move(executor), probe, cancel, stop_first);
    co_return 42;
}

asio::awaitable<void> notify_after_completed_void_slot_release(
    LateCleanupProbe& probe, neograph::graph::CancelToken* cancel) {
    static_cast<void>(co_await notify_after_completed_slot_release(probe, cancel));
}

} // namespace

TEST(RunSync, PostedFailureDrainsPendingValueFrame) {
    PendingFrameProbe probe;
    EXPECT_THROW(neograph::async::run_sync(pending_frame_with_throwing_handler(probe)),
                 PostedHandlerFailure);
    expect_pending_frame_drained(probe);
}

TEST(RunSync, PostedFailureDrainsPendingVoidFrame) {
    PendingFrameProbe probe;
    EXPECT_THROW(neograph::async::run_sync(pending_void_frame_with_throwing_handler(probe)),
                 PostedHandlerFailure);
    expect_pending_frame_drained(probe);
}

TEST(RunSync, PostedFailureDrainsCancellableFrameWithoutCancellingParent) {
    PendingFrameProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(pending_frame_with_throwing_handler(probe), &parent),
                 PostedHandlerFailure);
    expect_pending_frame_drained(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSync, PostedFailureDrainsCancellableVoidFrameWithoutCancellingParent) {
    PendingFrameProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(pending_void_frame_with_throwing_handler(probe), &parent),
                 PostedHandlerFailure);
    expect_pending_frame_drained(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSync, PostedFailureDrainsOperationFrameWithoutCancellingOperation) {
    PendingFrameProbe probe;
    auto operation = std::make_shared<neograph::graph::CancelToken>();
    EXPECT_THROW(neograph::async::detail::run_sync_operation(
                     pending_frame_with_throwing_handler(probe), operation),
                 PostedHandlerFailure);
    expect_pending_frame_drained(probe);
    EXPECT_FALSE(operation->is_cancelled());
}

TEST(RunSync, DrainsNotificationPostedWhenCompletedSlotIsReleased) {
    LateCleanupProbe probe;
    EXPECT_EQ(neograph::async::run_sync(notify_after_completed_slot_release(probe)), 42);
    EXPECT_EQ(probe.released, 1);
    EXPECT_TRUE(probe.completed);
    EXPECT_FALSE(probe.cancellation_notified);
}

TEST(RunSync, DrainsFinalNotificationAfterCleanupHandlerStopsContext) {
    LateCleanupProbe probe;
    EXPECT_EQ(neograph::async::run_sync(
                  notify_after_completed_slot_release(probe, nullptr, true)),
              42);
    EXPECT_EQ(probe.released, 1);
    EXPECT_TRUE(probe.completed) << "explicit stop must not abandon the next queued cleanup";
    EXPECT_FALSE(probe.cancellation_notified);
}

TEST(RunSync, VoidDrainsLateSlotReleaseWithParentCancellation) {
    LateCleanupProbe probe;
    neograph::graph::CancelToken parent;
    neograph::async::run_sync(notify_after_completed_void_slot_release(probe, &parent), &parent);
    EXPECT_TRUE(parent.is_cancelled());
    EXPECT_EQ(probe.released, 1);
    EXPECT_TRUE(probe.completed);
    EXPECT_FALSE(probe.cancellation_notified);
}

TEST(RunSync, OperationDrainsCancelAdmittedAfterInitialRunExhausted) {
    LateCleanupProbe probe;
    auto operation = std::make_shared<neograph::graph::CancelToken>();
    // The execution child's completed slot is released first. Its RAII handler
    // requests cancellation while the operation token is still bound, forcing
    // an emit/strand invoker into the already-stopped private context.
    EXPECT_EQ(neograph::async::detail::run_sync_operation(
                  notify_after_completed_slot_release(probe, operation.get()), operation),
              42);
    EXPECT_TRUE(operation->is_cancelled());
    EXPECT_EQ(probe.released, 1);
    EXPECT_TRUE(probe.completed);
    EXPECT_FALSE(probe.cancellation_notified);
}
