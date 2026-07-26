#include <gtest/gtest.h>

#include <neograph/util/request_queue.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

class RejectingQueue {
public:
    bool enqueue(std::function<void()>&&) { return false; }
};

TEST(RequestQueue, ConcurrentAdmissionNeverExceedsCapacity) {
    constexpr std::size_t kCapacity = 8;
    constexpr std::size_t kSubmitters = 48;
    neograph::util::RequestQueue queue(1, kCapacity);

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool worker_started = false;
    bool release_worker = false;

    auto [running_accepted, running] = queue.submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_worker; });
    });
    ASSERT_TRUE(running_accepted);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return worker_started;
        }));
    }

    std::barrier start(kSubmitters + 1);
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> observed_max_pending{0};
    std::atomic<bool> rejected_future_valid{false};
    std::mutex futures_mutex;
    std::vector<std::future<void>> futures;
    futures.reserve(kSubmitters);
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);

    for (std::size_t i = 0; i < kSubmitters; ++i) {
        submitters.emplace_back([&] {
            start.arrive_and_wait();
            auto [was_accepted, future] = queue.submit([] {});
            const auto pending = queue.stats().pending;
            auto previous = observed_max_pending.load(std::memory_order_relaxed);
            while (previous < pending
                   && !observed_max_pending.compare_exchange_weak(
                       previous, pending, std::memory_order_relaxed)) {
            }

            if (was_accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(futures_mutex);
                futures.push_back(std::move(future));
            } else {
                if (future.valid()) {
                    rejected_future_valid.store(true, std::memory_order_relaxed);
                }
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.arrive_and_wait();
    for (auto& submitter : submitters) submitter.join();

    EXPECT_EQ(accepted.load(), kCapacity);
    EXPECT_EQ(rejected.load(), kSubmitters - kCapacity);
    EXPECT_EQ(queue.stats().pending, kCapacity);
    EXPECT_LE(observed_max_pending.load(), kCapacity);
    EXPECT_FALSE(rejected_future_valid.load());

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_worker = true;
    }
    gate_cv.notify_all();

    ASSERT_NO_THROW(running.get());
    for (auto& future : futures) ASSERT_NO_THROW(future.get());

    const auto expected_completed = kCapacity + 1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((queue.stats().completed != expected_completed || queue.stats().active != 0)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.active, 0u);
    EXPECT_EQ(stats.completed, expected_completed);
    EXPECT_EQ(stats.rejected, kSubmitters - kCapacity);
}

TEST(RequestQueue, FailedEnqueueRollsBackReservationAndReportsError) {
    RejectingQueue queue;
    std::atomic<std::size_t> pending{1};
    std::atomic<std::size_t> rejected{0};

    auto [accepted, future] = neograph::util::detail::enqueue_reserved_task(
        queue, pending, rejected, [] {});

    EXPECT_FALSE(accepted);
    ASSERT_TRUE(future.valid());
    EXPECT_THROW(future.get(), std::runtime_error);
    EXPECT_EQ(pending.load(), 0u);
    EXPECT_EQ(rejected.load(), 1u);
}

TEST(RequestQueue, TaskExceptionsDoNotLeakPendingOrActiveAccounting) {
    neograph::util::RequestQueue queue(1, 1);

    auto [accepted, future] = queue.submit([] {
        throw std::runtime_error("expected task failure");
    });

    ASSERT_TRUE(accepted);
    EXPECT_THROW(future.get(), std::runtime_error);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (queue.stats().completed != 1
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.active, 0u);
    EXPECT_EQ(stats.completed, 1u);
}

}  // namespace
