/**
 * @file async/host_loop.h
 * @brief Runs coroutines on an Asio io_context that a host's own event loop pumps (#535).
 *
 * A WebAssembly host (a browser, a Worker, Node) owns the thread. NeoGraph cannot block it, and nothing in it can
 * block, so the engine's io_context is run in short turns that the host schedules: a turn runs the handlers that
 * are ready and returns. HostLoop schedules those turns on a SchemaProvider host executor
 * (`sp::runtime::Executor`, the interface a Client runs on), the same seam that the SDK's own work goes through.
 * The SDK's tasks and the engine's turns are then all tasks of the host's event loop.
 *
 * A turn is scheduled when:
 *   - work is posted to executor(), whether by `spawn`, by a cancellation, or by the SDK completing a call that a
 *     coroutine awaits (the completion posts the coroutine's continuation);
 *   - a deadline noted with `wake_at` or `note_deadline` arrives, for an Asio timer that the loop cannot see.
 * It is never scheduled while the loop has nothing to do, so an idle loop costs the host nothing.
 *
 * The loop runs no thread and calls no blocking function. A synchronous entry point (`GraphEngine::run`,
 * `Provider::dispatch`) drives a private io_context to completion and would wait for work that only the host can
 * do; when it reaches a provider on a host-pumped executor it fails with the SDK's `runtime.blocking.host_pumped`
 * refusal instead. Use the `_async` forms on a HostLoop.
 *
 * HostLoop is not thread-safe. Create it, use executor() and destroy it on the host's thread.
 */
#pragma once

#include <neograph/api.h>
#include <runtime/host.h>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/execution_context.hpp>
#include <asio/io_context.hpp>
#include <asio/prefer.hpp>
#include <asio/query.hpp>
#include <asio/require.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace neograph::async {

namespace detail {
class HostLoopCore;
/// Schedules a turn of the loop. Does nothing once the loop is closed.
NEOGRAPH_API void host_loop_wake(HostLoopCore* core) noexcept;

/// The io_context's executor, plus a turn of the loop whenever work is executed on it. Properties are forwarded, so
/// the adapted executors that Asio derives from it (blocking.never for a post, a strand's inner executor) wake the
/// loop too.
template <typename Executor>
class PumpedExecutor {
public:
    PumpedExecutor(Executor executor, HostLoopCore* core) noexcept
        : executor_(std::move(executor)), core_(core) {}

    template <typename Function>
    void execute(Function&& function) const {
        executor_.execute(std::forward<Function>(function));
        host_loop_wake(core_);
    }

    template <typename Property>
    auto query(const Property& property) const
        noexcept(noexcept(asio::query(std::declval<const Executor&>(), property)))
        -> decltype(asio::query(std::declval<const Executor&>(), property)) {
        return asio::query(executor_, property);
    }

    template <typename Property>
        requires asio::can_require<const Executor&, const Property&>::value
    auto require(const Property& property) const
        -> PumpedExecutor<std::decay_t<asio::require_result_t<const Executor&, const Property&>>> {
        return {asio::require(executor_, property), core_};
    }

    template <typename Property>
        requires asio::can_prefer<const Executor&, const Property&>::value
    auto prefer(const Property& property) const
        -> PumpedExecutor<std::decay_t<asio::prefer_result_t<const Executor&, const Property&>>> {
        return {asio::prefer(executor_, property), core_};
    }

    friend bool operator==(const PumpedExecutor& left, const PumpedExecutor& right) noexcept {
        return left.core_ == right.core_ && left.executor_ == right.executor_;
    }
    friend bool operator!=(const PumpedExecutor& left, const PumpedExecutor& right) noexcept {
        return !(left == right);
    }

private:
    Executor executor_;
    HostLoopCore* core_;
};
}  // namespace detail

struct HostLoopOptions {
    /// Handlers one turn runs before it yields to the host and schedules another. Bounds how long a graph of
    /// steps that never wait can hold the host's thread.
    std::size_t turn_handlers = 256;
    /// Called with a failure that escapes a handler. The engine's coroutines and `spawn` catch their own, so
    /// this is for a handler that someone posted to the loop. The loop continues. Unset: the failure is dropped.
    std::function<void(std::exception_ptr)> on_error;
};

class NEOGRAPH_API HostLoop {
public:
    using Options = HostLoopOptions;

    /// `host` is the executor that turns are scheduled on. Give the loop an executor of its own, not the one a
    /// Client runs on: a Client shuts its executor down when it is destroyed. The loop's turns and the Clients'
    /// tasks are then separate queues of one host event loop.
    explicit HostLoop(std::shared_ptr<sp::runtime::Executor> host, Options options = {});
    /// Destroys the io_context, and with it every coroutine that has not finished.
    ~HostLoop();
    HostLoop(const HostLoop&) = delete;
    HostLoop& operator=(const HostLoop&) = delete;

    /// The executor to run engine work on (`engine->run_async`, `co_spawn`). It must not outlive the loop.
    asio::any_io_executor executor();

    /// Starts `work` on executor() and returns at once; `done(exception, result)` is called from a later turn when it
    /// finishes. The result of a failed `work` is default-constructed.
    template <typename T, typename Done>
    void spawn(asio::awaitable<T> work, Done done) {
        asio::co_spawn(executor(), std::move(work), std::move(done));
        wake();
    }

    /// Schedules a turn if none is scheduled.
    void wake() noexcept;
    /// Schedules a turn no earlier than `at` for an Asio timer that expires then.
    void wake_at(sp::runtime::SteadyTime at) noexcept;
    /// Takes back one `wake_at` for `at` whose timer was cancelled. Does nothing if none is noted.
    void forget_at(sp::runtime::SteadyTime at) noexcept;

    /// True when no turn is scheduled and no handler is ready.
    bool idle() const noexcept;

private:
    std::shared_ptr<detail::HostLoopCore> core_;
};

/// Tells the loop that runs `executor` about an Asio timer that expires at `at`, so that a turn is scheduled then. A
/// timer cannot do that itself: the host's loop does not see Asio's timer queue. Does nothing for an executor that
/// no HostLoop runs. Code that waits on an Asio timer under a HostLoop calls it, as the engine's own waits do. A turn
/// can run before Asio dispatches the timer, because the host's clock runs ahead of Asio's or Asio learns that the
/// timer is due from a thread of its own; the loop then looks again shortly, until Asio has dispatched it.
NEOGRAPH_API void note_deadline(const asio::any_io_executor& executor,
                                std::chrono::steady_clock::time_point at) noexcept;

/// Takes back one `note_deadline` for `at`, once the Asio timer it was for is cancelled, so that the loop does not
/// keep a host timer armed for a deadline nothing waits for. Does nothing for an executor that no HostLoop runs.
NEOGRAPH_API void forget_deadline(const asio::any_io_executor& executor,
                                  std::chrono::steady_clock::time_point at) noexcept;

/// True when `executor` is run by a HostLoop, including an executor derived from it (a strand of it, or one with
/// other properties). Nothing that waits for work on such an executor can finish: only the host's own turn runs it.
NEOGRAPH_API bool is_host_pumped(const asio::any_io_executor& executor) noexcept;

}  // namespace neograph::async
