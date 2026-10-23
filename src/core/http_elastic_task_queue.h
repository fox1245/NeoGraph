#pragma once

// The worker-thread queue of the ACP remote transports (src/acp/http_server.cpp),
// whose streams and WebSocket connections hold a thread for as long as they
// are open. The MCP Streamable HTTP server carries an identical queue of its
// own (src/mcp/http_server.cpp).
//
// cpp-httplib's ThreadPool decides whether to start a thread from a count of
// idle threads that lags behind the jobs already handed to them: connections
// that arrive together can wait for one thread while it serves a request that
// takes minutes, so a request meant to stop that work sat behind it. This
// queue starts a thread whenever the jobs waiting and running outnumber the
// threads, up to `max` threads, which then stay.
//
// Internal and not installed. It includes httplib, so a translation unit must
// have the same TLS macro as every other NeoGraph translation unit that
// instantiates httplib types (see src/mcp/http_server.cpp).

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace neograph::detail {

class ElasticTaskQueue final : public httplib::TaskQueue {
public:
    ElasticTaskQueue(std::size_t base, std::size_t max, std::size_t max_queued)
        : max_(std::max(base, max)), max_queued_(max_queued) {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < base; ++i) spawn_locked();
    }
    ~ElasticTaskQueue() override { shutdown(); }

    bool enqueue(std::function<void()> job) override {
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            if (max_queued_ != 0 && jobs_.size() >= max_queued_) return false;
            jobs_.push_back(std::move(job));
            if (jobs_.size() + busy_ > threads_.size() && threads_.size() < max_) spawn_locked();
        }
        ready_.notify_one();
        return true;
    }

    void shutdown() override {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            threads.swap(threads_);
        }
        ready_.notify_all();
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }

private:
    void spawn_locked() {
        threads_.emplace_back([this] { work(); });
    }

    void work() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;  // stopping, and every job ran
                job = std::move(jobs_.front());
                jobs_.pop_front();
                ++busy_;
            }
            job();
            std::lock_guard lock(mutex_);
            --busy_;
        }
    }

    const std::size_t                 max_;
    const std::size_t                 max_queued_;
    std::mutex                        mutex_;
    std::condition_variable           ready_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread>          threads_;
    std::size_t                       busy_     = 0;
    bool                              stopping_ = false;
};

}  // namespace neograph::detail
