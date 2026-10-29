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

#include "runtime_clock.h"

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/basic_waitable_timer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/error_code.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/post.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace neograph::async {

namespace detail {

struct SimulatedWaitHandlerBase {
    virtual ~SimulatedWaitHandlerBase() = default;
    virtual void post(asio::error_code ec) = 0;
};

template <typename Handler, typename Executor>
struct SimulatedWaitHandler final : SimulatedWaitHandlerBase {
    Handler handler;
    asio::executor_work_guard<Executor> work;

    SimulatedWaitHandler(Handler&& h, const Executor& ex)
        : handler(std::move(h)), work(ex) {}

    void post(asio::error_code ec) override {
        auto ex = work.get_executor();
        auto h = std::move(handler);
        auto w = std::move(work);
        asio::post(ex, [h = std::move(h), w = std::move(w), ec]() mutable {
            auto slot = asio::get_associated_cancellation_slot(h);
            if (slot.is_connected()) slot.clear();
            w.reset();
            std::move(h)(ec);
        });
    }
};

struct SimulatedHostTimer;

NEOGRAPH_API std::shared_ptr<SimulatedHostTimer> make_simulated_host_timer(
    const asio::any_io_executor& executor);
NEOGRAPH_API void arm_simulated_host_timer(
    SimulatedHostTimer& timer, RuntimeClock::time_point at);
NEOGRAPH_API std::size_t cancel_simulated_host_timer(
    SimulatedHostTimer& timer) noexcept;
NEOGRAPH_API void async_wait_simulated_host_timer(
    const std::shared_ptr<SimulatedHostTimer>& timer,
    asio::cancellation_slot slot,
    std::unique_ptr<SimulatedWaitHandlerBase> handler);

}  // namespace detail

class HostTimer {
public:
    explicit HostTimer(const asio::any_io_executor& executor) : timer_(executor) {}
    HostTimer(const HostTimer&)            = delete;
    HostTimer& operator=(const HostTimer&) = delete;
    ~HostTimer() {
        if (simulated_) detail::cancel_simulated_host_timer(*simulated_);
        forget();
    }

    /// Expires `delay` from now. A wait that is pending is cancelled, as with the Asio timer.
    void arm_after(RuntimeClock::duration delay) {
        if (detail::simulated_clock_for_testing()) {
            if (arm_simulated(add_saturated(RuntimeClock::now(), delay))) return;
        } else if (simulated_) {
            leave_simulated();
        }
        forget();
        timer_.expires_after(delay);
        note(timer_.expiry());
    }

    /// Expires at `at`. A wait that is pending is cancelled, as with the Asio timer.
    void arm_at(RuntimeClock::time_point at) {
        if (detail::simulated_clock_for_testing()) {
            if (arm_simulated(at)) return;
        } else if (simulated_) {
            leave_simulated();
        }
        forget();
        timer_.expires_at(at);
        if (at < (RuntimeClock::time_point::max)()) note(at);
    }

    /// A timer that never expires: a signal that `cancel()` or a later `arm_*` wakes. It has no deadline, so the loop
    /// is told nothing. (`arm_at(time_point::min())` wakes a pending wait at once and also leaves the timer expired.)
    void arm_never() {
        if (detail::simulated_clock_for_testing()) {
            if (arm_simulated((RuntimeClock::time_point::max)())) return;
        } else if (simulated_) {
            leave_simulated();
        }
        forget();
        timer_.expires_at((RuntimeClock::time_point::max)());
    }

    template <typename CompletionToken>
    decltype(auto) async_wait(CompletionToken&& token) {
        return asio::async_initiate<CompletionToken, void(asio::error_code)>(
            [this](auto&& handler) {
                if (!simulated_) {
                    timer_.async_wait(std::forward<decltype(handler)>(handler));
                    return;
                }
                using Handler = std::decay_t<decltype(handler)>;
                Handler decaying_handler(std::forward<decltype(handler)>(handler));
                auto slot = asio::get_associated_cancellation_slot(decaying_handler);
                auto work_ex = asio::get_associated_executor(decaying_handler, timer_.get_executor());
                using WorkEx = decltype(work_ex);
                auto op = std::make_unique<detail::SimulatedWaitHandler<Handler, WorkEx>>(
                    std::move(decaying_handler), work_ex);
                detail::async_wait_simulated_host_timer(simulated_, slot, std::move(op));
            },
            token);
    }

    /// Cancels the pending wait, and takes the deadline back from the loop: a timer that is cancelled leaves no host
    /// timer armed.
    std::size_t cancel() {
        if (simulated_) {
            return detail::cancel_simulated_host_timer(*simulated_);
        }
        forget();
        return timer_.cancel();
    }

    RuntimeClock::time_point expiry() const {
        return simulated_ ? simulated_expiry_ : timer_.expiry();
    }
    asio::any_io_executor get_executor() { return timer_.get_executor(); }

private:
    static RuntimeClock::time_point add_saturated(RuntimeClock::time_point now,
                                                  RuntimeClock::duration delay) noexcept {
        if (delay >= RuntimeClock::duration::zero()) {
            const auto max_tp = (RuntimeClock::time_point::max)();
            return max_tp - delay < now ? max_tp : now + delay;
        }
        const auto min_tp = (RuntimeClock::time_point::min)();
        return min_tp - delay > now ? min_tp : now + delay;
    }

    bool arm_simulated(RuntimeClock::time_point at) {
        if (!simulated_) {
            simulated_ = detail::make_simulated_host_timer(timer_.get_executor());
            if (!simulated_) return false;
            forget();
            timer_.cancel();
        }
        simulated_expiry_ = at;
        detail::arm_simulated_host_timer(*simulated_, at);
        return true;
    }

    void leave_simulated() noexcept {
        detail::cancel_simulated_host_timer(*simulated_);
        simulated_.reset();
    }

    void note(RuntimeClock::time_point at) {
        detail::note_deadline(timer_.get_executor(), at, detail::RuntimeClockTag{});
        noted_    = true;
        noted_at_ = at;
    }
    void forget() noexcept {
        if (!noted_) return;
        noted_ = false;
        // A deadline that has passed is the loop's to retire once Asio has dispatched the timer; only a deadline that is
        // still ahead is surely still noted, so only that one is taken back.
        if (noted_at_ <= RuntimeClock::now()) return;
        detail::forget_deadline(timer_.get_executor(), noted_at_, detail::RuntimeClockTag{});
    }

    asio::basic_waitable_timer<RuntimeClock>    timer_;
    bool                                        noted_ = false;
    RuntimeClock::time_point                    noted_at_{};
    std::shared_ptr<detail::SimulatedHostTimer> simulated_;
    RuntimeClock::time_point                    simulated_expiry_{};
};

}  // namespace neograph::async
