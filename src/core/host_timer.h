// The timer that engine code waits on (#567).
//
// A HostLoop (neograph/async/host_loop.h) pumps its io_context from a host's event loop and cannot see Asio's timer
// queue. A timer that the loop was not told about never fires there, and the run waits forever. HostTimer arms the
// timer and tells the loop when it expires in one call, so a wait cannot do the first without the second. Without a
// HostLoop the second does nothing.
//
// Engine code does not construct an Asio timer or call expires_after/expires_at itself: the host_timer_guard test
// (scripts/check_host_timers.py) refuses it in src/ and include/. Code that never runs on a host loop is listed in
// scripts/host_timer_allowlist.txt, each with its reason.
#pragma once

#include <neograph/async/host_loop.h>

#include <asio/any_io_executor.hpp>
#include <asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <utility>

namespace neograph::async {

class HostTimer {
public:
    explicit HostTimer(const asio::any_io_executor& executor) : timer_(executor) {}
    HostTimer(const HostTimer&)            = delete;
    HostTimer& operator=(const HostTimer&) = delete;
    ~HostTimer() { forget(); }

    /// Expires `delay` from now. A wait that is pending is cancelled, as with the Asio timer.
    void arm_after(std::chrono::steady_clock::duration delay) {
        forget();
        timer_.expires_after(delay);
        note(timer_.expiry());
    }

    /// Expires at `at`. A wait that is pending is cancelled, as with the Asio timer.
    void arm_at(std::chrono::steady_clock::time_point at) {
        forget();
        timer_.expires_at(at);
        if (at < (std::chrono::steady_clock::time_point::max)()) note(at);
    }

    /// A timer that never expires: a signal that `cancel()` or a later `arm_*` wakes. It has no deadline, so the loop
    /// is told nothing. (`arm_at(time_point::min())` wakes a pending wait at once and also leaves the timer expired.)
    void arm_never() {
        forget();
        timer_.expires_at((std::chrono::steady_clock::time_point::max)());
    }

    template <typename CompletionToken>
    decltype(auto) async_wait(CompletionToken&& token) {
        return timer_.async_wait(std::forward<CompletionToken>(token));
    }

    /// Cancels the pending wait, and takes the deadline back from the loop: a timer that is cancelled leaves no host
    /// timer armed.
    std::size_t cancel() {
        forget();
        return timer_.cancel();
    }

    std::chrono::steady_clock::time_point expiry() const { return timer_.expiry(); }
    asio::any_io_executor get_executor() { return timer_.get_executor(); }

private:
    void note(std::chrono::steady_clock::time_point at) {
        note_deadline(timer_.get_executor(), at);
        noted_    = true;
        noted_at_ = at;
    }
    void forget() noexcept {
        if (!noted_) return;
        noted_ = false;
        // A deadline that has passed is the loop's to retire once Asio has dispatched the timer; only a deadline that is
        // still ahead is surely still noted, so only that one is taken back.
        if (noted_at_ <= std::chrono::steady_clock::now()) return;
        forget_deadline(timer_.get_executor(), noted_at_);
    }

    asio::steady_timer                    timer_;
    bool                                  noted_ = false;
    std::chrono::steady_clock::time_point noted_at_{};
};

}  // namespace neograph::async
