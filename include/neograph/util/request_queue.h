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
#include <utility>

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
        size_t completed;       ///< Total tasks settled, including close cancellation.
        size_t rejected;        ///< Tasks rejected during admission.
        size_t num_workers;     ///< Number of worker threads.
        size_t max_queue_size;  ///< Maximum queue capacity.
    };

private:
    struct State {
        explicit State(size_t max_queue_size)
            : max_queue_size_(max_queue_size) {}

        template<typename F>
        std::pair<bool, std::future<void>> submit(F&& task) {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (!running_.load(std::memory_order_acquire)) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return {false, std::future<void>()};
            }
            if (!try_reserve_pending_slot()) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return {false, std::future<void>()};
            }

            auto result = detail::enqueue_reserved_task(
                queue_, pending_, rejected_,
                [this, t = std::forward<F>(task)]() mutable {
                    if (!claim_task_start()) {
                        throw std::runtime_error("RequestQueue is closed");
                    }
                    t();
                });
            if (result.first) cv_.notify_one();
            return result;
        }

        void close() {
            const bool caller_is_worker = current_worker_ == this;
            const auto caller_id = std::this_thread::get_id();
            {
                std::unique_lock<std::mutex> lock(lifecycle_mutex_);
                if (close_started_) {
                    if (caller_is_worker) return;
                    cv_.wait(lock, [this] { return close_complete_; });
                    return;
                }

                close_started_ = true;
                worker_initiated_close_ = caller_is_worker;
                running_.store(false, std::memory_order_release);
            }
            cv_.notify_all();

            if (caller_is_worker) {
                // A worker cannot join itself. Detach only that thread, then
                // wait for every other claimed callback before the facade can
                // be destroyed. State remains alive through the detached
                // worker until it exits and marks shutdown complete.
                for (auto& worker : workers_) {
                    if (!worker.joinable()) continue;
                    if (worker.get_id() == caller_id) {
                        worker.detach();
                    } else {
                        worker.join();
                    }
                }
                cancel_pending_tasks();
                {
                    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                    cancellation_complete_ = true;
                }
                return;
            }

            for (auto& worker : workers_) {
                if (worker.joinable()) worker.join();
            }
            cancel_pending_tasks();
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                cancellation_complete_ = true;
                close_complete_ = true;
            }
            cv_.notify_all();
        }

        bool is_closed() const noexcept {
            return !running_.load(std::memory_order_acquire);
        }

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

        void run_worker() {
            current_worker_ = this;
            worker_loop();
            worker_exited();
            current_worker_ = nullptr;
        }

        std::mutex lifecycle_mutex_;
        std::condition_variable cv_;
        std::vector<std::thread> workers_;
        std::atomic<bool> running_{true};
        std::atomic<size_t> pending_{0};
        std::atomic<size_t> active_{0};
        std::atomic<size_t> completed_{0};
        std::atomic<size_t> rejected_{0};
        size_t max_queue_size_;
        size_t workers_alive_{0};
        bool close_started_{false};
        bool close_complete_{false};
        bool cancellation_complete_{false};
        bool worker_initiated_close_{false};
        inline static thread_local State* current_worker_ = nullptr;

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

        bool claim_task_start() {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            return running_.load(std::memory_order_acquire);
        }

        void worker_loop() {
            std::function<void()> task;
            while (running_.load(std::memory_order_acquire)) {
                if (queue_.try_dequeue(task)) {
                    pending_.fetch_sub(1, std::memory_order_acq_rel);
                    execute_task(task);
                } else {
                    // No busy-spin — sleep until notified by submit().
                    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
                    cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                        return !running_.load(std::memory_order_acquire)
                            || queue_.size_approx() > 0;
                    });
                }
            }
        }

        void worker_exited() {
            bool finalize_worker_close = false;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                --workers_alive_;
                finalize_worker_close = close_started_
                    && worker_initiated_close_
                    && !close_complete_
                    && workers_alive_ == 0;
            }
            if (!finalize_worker_close) return;

            bool cancel_pending = false;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                cancel_pending = !cancellation_complete_;
            }
            if (cancel_pending) cancel_pending_tasks();
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex_);
                cancellation_complete_ = true;
                close_complete_ = true;
            }
            cv_.notify_all();
        }

        void cancel_pending_tasks() {
            std::function<void()> task;
            while (queue_.try_dequeue(task)) {
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                try {
                    task();
                } catch (...) {
                    // The task wrapper owns its promise and reports errors.
                }
                completed_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        void execute_task(std::function<void()>& task) {
            active_.fetch_add(1, std::memory_order_relaxed);
            try {
                task();
            } catch (...) {
                // A task wrapper should already complete its promise, but a
                // malformed task must not strand accounting or kill a worker.
            }
            active_.fetch_sub(1, std::memory_order_relaxed);
            completed_.fetch_add(1, std::memory_order_relaxed);
        }

        moodycamel::ConcurrentQueue<std::function<void()>> queue_;
    };

public:
    /**
     * @brief Construct a request queue with a worker thread pool.
     * @param num_workers Number of worker threads to spawn (default: 128).
     * @param max_queue_size Maximum number of pending tasks before backpressure (default: 10000).
     * @throws std::invalid_argument if num_workers is zero.
     */
    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000)
        : state_(std::make_shared<State>(max_queue_size))
    {
        if (num_workers == 0) {
            throw std::invalid_argument("RequestQueue requires at least one worker");
        }
        try {
            for (size_t i = 0; i < num_workers; ++i) {
                start_worker();
            }
        } catch (...) {
            state_->close();
            throw;
        }
        // No banner on stdout — library code shouldn't pollute the
        // host process's stdout (breaks scripts that parse it). If
        // observability is desired, route through an injected logger
        // instead of std::cout.
    }

    /// Destructor: applies the same cancel-on-close policy as close().
    ~RequestQueue() {
        close();
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
     *         returns a valid future that propagates the failure. Closure
     *         rejection returns an invalid future.
     */
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task) {
        return state_->submit(std::forward<F>(task));
    }

    /**
     * @brief Stop accepting work, cancel queued tasks, and wait for workers.
     *
     * The first caller transitions the queue to closed. Work a worker claimed
     * before that transition may finish; all unclaimed work completes its
     * future with `std::runtime_error("RequestQueue is closed")`. Concurrent
     * and repeated external callers wait for that same shutdown to complete.
     * A task may call close() to initiate shutdown, but cannot wait for itself.
     */
    void close() {
        auto state = state_;
        state->close();
    }

    /// True once close() has started rejecting new work.
    bool is_closed() const noexcept {
        return state_->is_closed();
    }

    /**
     * @brief Get current queue statistics.
     * @return Stats snapshot with pending, active, completed, and rejected counts.
     */
    Stats stats() const {
        return state_->stats();
    }

private:
    void start_worker() {
        {
            std::lock_guard<std::mutex> lock(state_->lifecycle_mutex_);
            ++state_->workers_alive_;
        }
        try {
            auto state = state_;
            state_->workers_.emplace_back([state] { state->run_worker(); });
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_->lifecycle_mutex_);
            --state_->workers_alive_;
            throw;
        }
    }

    std::shared_ptr<State> state_;
};

} // namespace neograph::util
