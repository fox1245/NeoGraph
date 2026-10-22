// A host's executor for tests: the work of an async::HostLoop queues up, and the test runs it from its own thread, one
// task at a time, as a JavaScript host runs its microtasks. Nothing runs inline and nothing runs on another thread, so a
// store that completes only when the loop is pumped really does.
#pragma once

#include <neograph/async/host_loop.h>

#include <runtime/host.h>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <thread>

namespace neograph::host_test {

using namespace std::chrono_literals;

// The executor of the host: work queues up, the host runs it from its own loop, and nothing runs inline.
class ManualExecutor final : public sp::runtime::Executor {
public:
    void post(Task task) override { ready_.push_back(std::move(task)); }
    Timer schedule(sp::runtime::SteadyTime at, Task task) override {
        const auto id = ++last_timer_;
        timers_.emplace(id, Timed{at, std::move(task)});
        return id;
    }
    void cancel(Timer id) noexcept override { timers_.erase(id); }
    sp::runtime::SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
    sp::runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
    bool in_thread() const noexcept override { return running_ != 0; }
    bool host_pumped() const noexcept override { return true; }
    void shutdown() noexcept override {}

    bool step() {
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.at <= now()) {
                ready_.push_back(std::move(it->second.task));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
        if (ready_.empty()) return false;
        auto task = std::move(ready_.front());
        ready_.pop_front();
        ++running_;
        try { task(); } catch (...) { --running_; throw; }
        --running_;
        return true;
    }
    std::size_t timers() const { return timers_.size(); }

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

private:
    struct Timed { sp::runtime::SteadyTime at; Task task; };
    std::deque<Task> ready_;
    std::map<Timer, Timed> timers_;
    Timer last_timer_ = 0;
    unsigned running_ = 0;
};

struct HostedRuntime {
    std::shared_ptr<ManualExecutor> host = std::make_shared<ManualExecutor>();
    async::HostLoop                 loop{host};
};

}  // namespace neograph::host_test
