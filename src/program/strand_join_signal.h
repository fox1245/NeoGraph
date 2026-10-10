#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/cancellation_state.hpp>
#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

namespace neograph::program::detail {

/**
 * One-shot completion signal for a join whose producers and single waiter all run on one strand.
 *
 * `asio::steady_timer::cancel()` wakes only waits that are already pending and leaves the expiry
 * untouched. A bare "timer expiring at time_point::max, cancelled by the last worker" therefore
 * loses the wake-up when the last worker completes before the waiter initiates its wait, and the
 * waiter then sleeps forever. The flag records the signal so a late waiter returns at once; the
 * timer only parks a waiter that arrives first.
 *
 * Construct before publishing the signal to producers. After construction, both members are
 * touched exclusively by handlers on the strand serializing `executor`, so neither needs its
 * own synchronization: post `signal()` to that strand from any other executor, and await
 * `wait()` only from a coroutine running on it. There is a single waiter at a time.
 *
 * This is a private join coroutine, not cancellable work: a cancelled parent must still drain
 * every producer before destroying locals those producers capture. The join disables inherited
 * coroutine cancellation; Program operation/deadline cancellation continues through its tokens.
 */
class StrandJoinSignal final {
public:
    explicit StrandJoinSignal(const asio::any_io_executor& strand) : timer_(strand) {
        timer_.expires_at((asio::steady_timer::time_point::max)());
    }

    StrandJoinSignal(const StrandJoinSignal&)            = delete;
    StrandJoinSignal& operator=(const StrandJoinSignal&) = delete;

    /** Idempotent. Wakes a parked waiter and makes every later `wait()` return immediately. */
    void signal() {
        signalled_ = true;
        timer_.cancel();
    }

    /** Drains producers even if the private join coroutine receives cancellation. */
    asio::awaitable<void> wait() {
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
        while (!signalled_) {
            asio::error_code error;
            co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (error && error != asio::error::operation_aborted) throw asio::system_error(error);
        }
    }

private:
    asio::steady_timer timer_;
    bool               signalled_ = false;
};

}  // namespace neograph::program::detail
