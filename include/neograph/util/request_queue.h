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

// Lock-free request queue with worker pool and backpressure.
// Decouples HTTP connection acceptance from LLM call concurrency.
class RequestQueue {
public:
    struct Stats {
        size_t pending;
        size_t active;
        size_t completed;
        size_t rejected;
        size_t num_workers;
        size_t max_queue_size;
    };

    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000)
        : max_queue_size_(max_queue_size)
    {
        for (size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        std::cout << "[RequestQueue] " << num_workers
                  << " workers, max queue: " << max_queue_size << "\n";
    }

    ~RequestQueue() {
        running_ = false;
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // Submit a task.  Returns {accepted, future}.
    // If the queue is full (backpressure), returns {false, {}}.
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
