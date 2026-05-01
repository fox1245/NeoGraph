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
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <condition_variable>

namespace neograph::util {

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
     * If the queue is full (pending >= max_queue_size), the task is
     * rejected and the rejected counter is incremented.
     *
     * @tparam F Callable type (must be invocable with no arguments).
     * @param task The task to execute.
     * @return A pair of {accepted, future}: accepted is true if the task
     *         was enqueued; future can be waited on for completion.
     *         If rejected, future is default-constructed (invalid).
     */
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task) {
        if (pending_.load(std::memory_order_relaxed) >= max_queue_size_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return {false, std::future<void>()};
        }

        auto promise = std::make_shared<std::promise<void>>();
        auto future  = promise->get_future();

        pending_.fetch_add(1, std::memory_order_relaxed);
        queue_.enqueue(
            [t = std::forward<F>(task), p = std::move(promise)]() mutable {
                try {
                    t();
                    p->set_value();
                } catch (...) {
                    p->set_exception(std::current_exception());
                }
            });
        cv_.notify_one();

        return {true, std::move(future)};
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
    void worker_loop() {
        std::function<void()> task;
        while (running_.load(std::memory_order_relaxed)) {
            if (queue_.try_dequeue(task)) {
                pending_.fetch_sub(1, std::memory_order_relaxed);
                active_.fetch_add(1, std::memory_order_relaxed);
                task();
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
