/**
 * @file util/request_queue.h
 * @brief Lock-free request queue with worker pool and backpressure.
 *
 * Provides a concurrent task queue backed by moodycamel::ConcurrentQueue
 * with a configurable worker thread pool. Useful for decoupling HTTP
 * connection acceptance from LLM call concurrency.
 */
#pragma once

#include <concurrentqueue.h>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <exception>
#include <stdexcept>

namespace neograph::util {

namespace detail {

// This is separate from RequestQueue only so tests can force the rare
// enqueue-failure branch without relying on allocator failure. It does not
// change RequestQueue's public object layout.
template <typename Queue, typename F>
std::pair<bool, std::future<void>> enqueue_reserved_task(
    Queue& queue,
    std::atomic<size_t>& pending,
    std::atomic<size_t>& rejected,
    F&& task) {
    bool reservation_active = true;
    auto rollback_reservation = [&] {
        if (!reservation_active) return;
        pending.fetch_sub(1, std::memory_order_acq_rel);
        rejected.fetch_add(1, std::memory_order_relaxed);
        reservation_active = false;
    };

    std::shared_ptr<std::promise<void>> promise;
    std::future<void> future;
    try {
        promise = std::make_shared<std::promise<void>>();
        future = promise->get_future();
        std::function<void()> queued_task(
            [t = std::forward<F>(task), p = promise]() mutable {
                try {
                    t();
                    p->set_value();
                } catch (...) {
                    p->set_exception(std::current_exception());
                }
            });

        // Vendored ConcurrentQueue documents enqueue() as thread-safe but
        // fallible; see deps/concurrentqueue.h:997-1017, checked for #207.
        if (!queue.enqueue(std::move(queued_task))) {
            rollback_reservation();
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("RequestQueue enqueue failed")));
            return {false, std::move(future)};
        }
        reservation_active = false;
        return {true, std::move(future)};
    } catch (...) {
        const auto error = std::current_exception();
        rollback_reservation();
        if (promise && future.valid()) {
            promise->set_exception(error);
            return {false, std::move(future)};
        }
        throw;
    }
}

} // namespace detail

/**
 * @brief Lock-free task queue with a fixed worker thread pool and backpressure.
 *
 * Tasks are submitted via submit() and executed asynchronously by worker
 * threads. When the queue is full, new tasks are rejected (backpressure).
 *
 * @code
 * RequestQueue queue(4, 1000);  // 4 workers, max 1000 pending tasks
 * auto [accepted, future] = queue.submit([]{ do_work(); });
 * if (accepted) future.wait();
 * @endcode
 */
class RequestQueue {
public:
    /// Runtime statistics for monitoring queue health.
    struct Stats {
        size_t pending;         ///< Tasks waiting in the queue.
        size_t active;          ///< Tasks currently being executed.
        size_t completed;       ///< Total tasks finished.
        size_t rejected;        ///< Tasks rejected due to backpressure (queue full).
        size_t num_workers;     ///< Number of worker threads.
        size_t max_queue_size;  ///< Maximum queue capacity.
    };

    /**
     * @brief Construct a request queue with a worker thread pool.
     * @param num_workers Number of worker threads to spawn (default: 128).
     * @param max_queue_size Maximum number of pending tasks before backpressure (default: 10000).
     */
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000)
        : max_queue_size_(max_queue_size)
    {
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        // No banner on stdout — library code shouldn't pollute the
        // host process's stdout (breaks scripts that parse it). If
        // observability is desired, route through an injected logger
        // instead of std::cout.
    }

    /// Destructor: stops all workers and waits for them to finish.
    ~RequestQueue() {
        running_ = false;
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    /**
     * @brief Submit a task to the queue.
     *
     * Admission atomically reserves one pending slot before enqueueing, so
     * concurrent submitters cannot exceed max_queue_size. A full queue returns
     * {false, invalid_future}. If the underlying queue rejects an already
     * reserved task, the reservation is rolled back and the returned future is
     * valid but completes with an exception.
     *
     * @tparam F Callable type (must be invocable with no arguments).
     * @param task The task to execute.
     * @return A pair of {accepted, future}: accepted is true if the task
     *         was enqueued; future can be waited on for completion.
     *         Capacity rejection returns an invalid future; enqueue failure
     *         returns a valid future that propagates the failure.
     */
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task) {
        if (!try_reserve_pending_slot()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return {false, std::future<void>()};
        }

        auto result = detail::enqueue_reserved_task(
            queue_, pending_, rejected_, std::forward<F>(task));
        if (result.first) cv_.notify_one();
        return result;
    }

    /**
     * @brief Get current queue statistics.
     * @return Stats snapshot with pending, active, completed, and rejected counts.
     */
    Stats stats() const {
        return {
            pending_.load(std::memory_order_relaxed),
            active_.load(std::memory_order_relaxed),
            completed_.load(std::memory_order_relaxed),
            rejected_.load(std::memory_order_relaxed),
            workers_.size(),
            max_queue_size_
        };
    }

private:
    bool try_reserve_pending_slot() {
        size_t pending = pending_.load(std::memory_order_relaxed);
        while (pending < max_queue_size_) {
            if (pending_.compare_exchange_weak(
                    pending, pending + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void worker_loop() {
        std::function<void()> task;
        while (running_.load(std::memory_order_relaxed)) {
            if (queue_.try_dequeue(task)) {
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                active_.fetch_add(1, std::memory_order_relaxed);
                try {
                    task();
                } catch (...) {
                    // A task wrapper should already complete its promise, but
                    // a malformed task must not strand accounting or kill the
                    // worker thread.
                }
                active_.fetch_sub(1, std::memory_order_relaxed);
                completed_.fetch_add(1, std::memory_order_relaxed);
            } else {
                // No busy-spin — sleep until notified by submit()
                std::unique_lock<std::mutex> lock(cv_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !running_.load(std::memory_order_relaxed)
                        || queue_.size_approx() > 0;
                });
            }
        }
    }

    moodycamel::ConcurrentQueue<std::function<void()>> queue_;
    std::vector<std::thread> workers_;
    std::mutex cv_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{true};
    std::atomic<size_t> pending_{0};
    std::atomic<size_t> active_{0};
    std::atomic<size_t> completed_{0};
    std::atomic<size_t> rejected_{0};
    size_t max_queue_size_;
};

} // namespace neograph::util
