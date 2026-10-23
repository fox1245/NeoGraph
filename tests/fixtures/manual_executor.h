// A host's executor for tests: the work of an async::HostLoop queues up, and the test runs it from its own thread, one
// task at a time, as a JavaScript host runs its microtasks. Nothing runs inline, and a task runs only on the thread that
// calls step(), so a store that completes only when the loop is pumped really does.
//
// `post` may be called from any thread (a blocking tool's completion posts from the pool's thread), as a host executor
// that takes work from threads of its own must allow. `schedule`, `cancel`, `step` and `run_until` are the host
// thread's, which is how HostLoop calls them. No lock is held while a task runs or is destroyed.
#pragma once

#include <neograph/async/host_loop.h>

#include <runtime/host.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace neograph::host_test {

using namespace std::chrono_literals;

// The executor of the host: work queues up, the host runs it from its own loop, and nothing runs inline. A timer runs
// when the clock reaches it and the host next looks. `clock_lead` makes the host's clock run ahead of the one Asio's
// timers use, so that a host timer runs before the Asio timer that expires at the same instant has been dispatched.
class ManualExecutor final : public sp::runtime::Executor {
public:
    explicit ManualExecutor(bool pumped = true) : pumped_(pumped) {}

    // A refused post throws, as the SDK's contract allows for an executor that is stopping: the task was not accepted.
    void post(Task task) override {
        // Runs first, on the posting thread, with no lock held: it may block or start threads that post.
        if (before_post && before_post()) throw std::runtime_error("the host executor refuses work");
        std::lock_guard<std::mutex> lock(mutex_);
        if (refusing_) throw std::runtime_error("the host executor refuses work");
        ready_.push_back(std::move(task));
    }
    Timer schedule(sp::runtime::SteadyTime at, Task task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id = ++last_timer_;
        timers_.emplace(id, Timed{at, std::move(task)});
        return id;
    }
    void cancel(Timer id) noexcept override {
        Timed doomed;  // destroyed after the lock is released: its task may own a lot
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = timers_.find(id);
        if (found == timers_.end()) return;
        doomed = std::move(found->second);
        timers_.erase(found);
    }
    sp::runtime::SteadyTime now() const noexcept override { return std::chrono::steady_clock::now() + clock_lead; }
    sp::runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
    // True on the thread that is running one of this executor's tasks, and on no other.
    bool in_thread() const noexcept override { return RunningHere::on_this_thread(this); }
    bool host_pumped() const noexcept override { return pumped_; }
    void shutdown() noexcept override { ++shutdowns; }

    // Runs the tasks that are ready and the timers that are due; false when there was nothing to run.
    bool step() {
        Task task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = timers_.begin(); it != timers_.end();) {
                if (it->second.at <= now()) {
                    ready_.push_back(std::move(it->second.task));
                    it = timers_.erase(it);
                } else {
                    ++it;
                }
            }
            if (ready_.empty()) return false;
            task = std::move(ready_.front());
            ready_.pop_front();
        }
        ++tasks_run;
        RunningHere running(this);
        task();
        return true;
    }
    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_.size();
    }
    std::size_t timers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return timers_.size();
    }
    // From now on `post` throws (true), or accepts work again (false).
    void refuse_posts(bool refuse) {
        std::lock_guard<std::mutex> lock(mutex_);
        refusing_ = refuse;
    }

    // Runs the host until `done`, waiting for the next timer when nothing is ready. False if it took too long.
    template <typename Done>
    bool run_until(Done done, std::chrono::milliseconds limit = 10s) {
        const auto stop = std::chrono::steady_clock::now() + limit;
        while (!done()) {
            if (std::chrono::steady_clock::now() > stop) return false;
            if (!step()) std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    // Called at the start of every `post`; returning true refuses that post. Set it before the executor is used.
    std::function<bool()> before_post;
    std::atomic<int> shutdowns{0};
    std::atomic<std::size_t> tasks_run{0};
    std::chrono::steady_clock::duration clock_lead{};

private:
    struct Timed { sp::runtime::SteadyTime at; Task task; };

    // The executors whose tasks the calling thread is running, innermost last (a task may pump another host).
    class RunningHere {
    public:
        explicit RunningHere(const ManualExecutor* executor) { stack().push_back(executor); }
        ~RunningHere() { stack().pop_back(); }
        RunningHere(const RunningHere&) = delete;
        RunningHere& operator=(const RunningHere&) = delete;
        static bool on_this_thread(const ManualExecutor* executor) noexcept {
            for (const auto* running : stack())
                if (running == executor) return true;
            return false;
        }

    private:
        static std::vector<const ManualExecutor*>& stack() {
            thread_local std::vector<const ManualExecutor*> running;
            return running;
        }
    };

    mutable std::mutex mutex_;
    std::deque<Task> ready_;
    std::map<Timer, Timed> timers_;
    Timer last_timer_ = 0;
    bool refusing_ = false;
    bool pumped_;
};

struct HostedRuntime {
    std::shared_ptr<ManualExecutor> host = std::make_shared<ManualExecutor>();
    async::HostLoop                 loop{host};
};

}  // namespace neograph::host_test
