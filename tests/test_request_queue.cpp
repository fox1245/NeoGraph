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

TEST(RequestQueue, ZeroWorkersAreRejected) {
    EXPECT_THROW(neograph::util::RequestQueue(0, 1), std::invalid_argument);
}

TEST(RequestQueue, CloseCancelsQueuedTasksAndRejectsNewWork) {
    neograph::util::RequestQueue queue(1, 4);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool worker_started = false;
    bool release_worker = false;
    std::atomic<int> queued_executions{0};

    auto [active_accepted, active] = queue.submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_worker; });
    });
    ASSERT_TRUE(active_accepted);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return worker_started;
        }));
    }

    auto [first_accepted, first] = queue.submit([&] {
        queued_executions.fetch_add(1, std::memory_order_relaxed);
    });
    auto [second_accepted, second] = queue.submit([&] {
        queued_executions.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(first_accepted);
    ASSERT_TRUE(second_accepted);

    std::thread closer([&] { queue.close(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!queue.is_closed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool observed_closed = queue.is_closed();

    auto [post_close_accepted, post_close] = queue.submit([] {});
    EXPECT_FALSE(post_close_accepted);
    EXPECT_FALSE(post_close.valid());

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_worker = true;
    }
    gate_cv.notify_all();
    closer.join();

    EXPECT_TRUE(observed_closed);
    EXPECT_TRUE(queue.is_closed());
    ASSERT_NO_THROW(active.get());
    ASSERT_EQ(first.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(second.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_THROW(first.get(), std::runtime_error);
    EXPECT_THROW(second.get(), std::runtime_error);
    EXPECT_EQ(queued_executions.load(), 0);

    queue.close();
    const auto stats = queue.stats();
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.active, 0u);
    EXPECT_EQ(stats.completed, 3u);
    EXPECT_EQ(stats.rejected, 1u);
}

TEST(RequestQueue, ConcurrentCloseCallsAreIdempotent) {
    constexpr std::size_t kClosers = 8;
    neograph::util::RequestQueue queue(1, 1);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool worker_started = false;
    bool release_worker = false;

    auto [active_accepted, active] = queue.submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_worker; });
    });
    ASSERT_TRUE(active_accepted);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return worker_started;
        }));
    }

    auto [queued_accepted, queued] = queue.submit([] {});
    ASSERT_TRUE(queued_accepted);

    std::barrier start(kClosers + 1);
    std::atomic<std::size_t> returned{0};
    std::vector<std::thread> closers;
    closers.reserve(kClosers);
    for (std::size_t i = 0; i < kClosers; ++i) {
        closers.emplace_back([&] {
            start.arrive_and_wait();
            queue.close();
            returned.fetch_add(1, std::memory_order_relaxed);
        });
    }

    start.arrive_and_wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!queue.is_closed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool observed_closed = queue.is_closed();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_worker = true;
    }
    gate_cv.notify_all();

    for (auto& closer : closers) closer.join();

    EXPECT_TRUE(observed_closed);
    EXPECT_EQ(returned.load(), kClosers);
    ASSERT_NO_THROW(active.get());
    ASSERT_EQ(queued.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_THROW(queued.get(), std::runtime_error);
}

TEST(RequestQueue, SubmitCloseRaceResolvesEveryAcceptedFuture) {
    constexpr std::size_t kSubmitters = 32;
    neograph::util::RequestQueue queue(1, kSubmitters + 1);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool worker_started = false;
    bool release_worker = false;
    std::atomic<int> queued_executions{0};

    auto [active_accepted, active] = queue.submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_worker; });
    });
    ASSERT_TRUE(active_accepted);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return worker_started;
        }));
    }

    std::barrier start(kSubmitters + 2);
    std::mutex futures_mutex;
    std::vector<std::future<void>> accepted_futures;
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> rejected{0};
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (std::size_t i = 0; i < kSubmitters; ++i) {
        submitters.emplace_back([&] {
            start.arrive_and_wait();
            auto [was_accepted, future] = queue.submit([&] {
                queued_executions.fetch_add(1, std::memory_order_relaxed);
            });
            if (was_accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(futures_mutex);
                accepted_futures.push_back(std::move(future));
            } else {
                EXPECT_FALSE(future.valid());
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::thread closer([&] {
        start.arrive_and_wait();
        queue.close();
    });

    start.arrive_and_wait();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!queue.is_closed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool observed_closed = queue.is_closed();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_worker = true;
    }
    gate_cv.notify_all();

    for (auto& submitter : submitters) submitter.join();
    closer.join();

    EXPECT_TRUE(observed_closed);
    ASSERT_NO_THROW(active.get());
    for (auto& future : accepted_futures) {
        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        EXPECT_THROW(future.get(), std::runtime_error);
    }
    EXPECT_EQ(queued_executions.load(), 0);
    EXPECT_EQ(accepted.load() + rejected.load(), kSubmitters);

    const auto stats = queue.stats();
    EXPECT_EQ(stats.pending, 0u);
    EXPECT_EQ(stats.active, 0u);
    EXPECT_EQ(stats.completed, accepted.load() + 1);
}

TEST(RequestQueue, DestructorCancelsQueuedFutures) {
    std::future<void> queued;
    {
        neograph::util::RequestQueue queue(1, 2);
        std::promise<void> worker_started;
        auto started = worker_started.get_future();

        auto [active_accepted, active] = queue.submit([&] {
            worker_started.set_value();
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(2);
            while (!queue.is_closed()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("RequestQueue destructor did not close");
                }
                std::this_thread::yield();
            }
        });
        ASSERT_TRUE(active_accepted);
        ASSERT_EQ(started.wait_for(std::chrono::seconds(2)), std::future_status::ready);

        auto [queued_accepted, future] = queue.submit([] {});
        ASSERT_TRUE(queued_accepted);
        queued = std::move(future);
    }

    ASSERT_EQ(queued.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    EXPECT_THROW(queued.get(), std::runtime_error);
}

TEST(RequestQueue, WorkerDestructionCancelsQueuedFutures) {
    auto* queue = new neograph::util::RequestQueue(1, 2);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool worker_started = false;
    bool release_worker = false;
    std::atomic<int> queued_executions{0};

    auto [active_accepted, active] = queue->submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_worker; });
    });
    ASSERT_TRUE(active_accepted);
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        ASSERT_TRUE(gate_cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return worker_started;
        }));
    }

    auto [destroy_accepted, destroy] = queue->submit([queue] {
        delete queue;
    });
    auto [queued_accepted, queued] = queue->submit([&] {
        queued_executions.fetch_add(1, std::memory_order_relaxed);
    });
    ASSERT_TRUE(destroy_accepted);
    ASSERT_TRUE(queued_accepted);

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_worker = true;
    }
    gate_cv.notify_all();

    ASSERT_EQ(active.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_NO_THROW(active.get());
    ASSERT_EQ(destroy.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_NO_THROW(destroy.get());
    ASSERT_EQ(queued.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_THROW(queued.get(), std::runtime_error);
    EXPECT_EQ(queued_executions.load(), 0);
}

TEST(RequestQueue, WorkerDestructionWaitsForOtherClaimedWorkers) {
    auto* queue = new neograph::util::RequestQueue(2, 2);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool other_worker_started = false;
    bool release_other_worker = false;
    std::promise<void> destruction_started_promise;
    auto destruction_started = destruction_started_promise.get_future();
    std::promise<void> destruction_returned_promise;
    auto destruction_returned = destruction_returned_promise.get_future();

    auto [other_accepted, other] = queue->submit([&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        other_worker_started = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_other_worker; });
    });
    ASSERT_TRUE(other_accepted);

    auto [destroy_accepted, destroy] = queue->submit([&] {
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate_cv.wait(lock, [&] { return other_worker_started; });
        }
        destruction_started_promise.set_value();
        delete queue;
        destruction_returned_promise.set_value();
    });
    ASSERT_TRUE(destroy_accepted);

    ASSERT_EQ(destruction_started.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    EXPECT_EQ(destruction_returned.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout);

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_other_worker = true;
    }
    gate_cv.notify_all();

    ASSERT_EQ(other.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_NO_THROW(other.get());
    ASSERT_EQ(destruction_returned.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    ASSERT_NO_THROW(destroy.get());
}

}  // namespace
