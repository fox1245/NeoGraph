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

    /// Expires `delay` from now. A wait that is pending is cancelled, as with the Asio timer.
    void arm_after(std::chrono::steady_clock::duration delay) {
        timer_.expires_after(delay);
        note_deadline(timer_.get_executor(), timer_.expiry());
    }

    /// Expires at `at`. A wait that is pending is cancelled, as with the Asio timer.
    void arm_at(std::chrono::steady_clock::time_point at) {
        timer_.expires_at(at);
        note_deadline(timer_.get_executor(), at);
    }

    template <typename CompletionToken>
    decltype(auto) async_wait(CompletionToken&& token) {
        return timer_.async_wait(std::forward<CompletionToken>(token));
    }

    std::size_t cancel() { return timer_.cancel(); }

private:
    asio::steady_timer timer_;
};

}  // namespace neograph::async
