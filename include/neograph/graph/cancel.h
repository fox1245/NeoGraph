/**
 * @file graph/cancel.h
 * @brief Cooperative cancellation primitive for graph runs.
 *
 * v0.3 introduces ``CancelToken`` so a caller can abort an in-flight
 * ``GraphEngine::run_async`` cleanly, including the LLM HTTP request
 * downstream of the engine super-step.
 *
 * Two propagation paths from one token:
 *
 *   1. **Polling** (`is_cancelled()`) — the engine super-step loop and
 *      the per-node dispatch checkpoint poll this between steps. Stops
 *      *future* node work after the next checkpoint, so a `time.sleep`
 *      in a Python node won't be preempted but the subsequent node
 *      won't fire.
 *
 *   2. **asio cancellation_signal** (`slot()`) — bound to the run's
 *      coroutine via ``asio::bind_cancellation_slot`` at ``co_spawn``
 *      time. asio propagates cancel down through every ``co_await``,
 *      including ``ConnPool::async_post``, so an in-flight HTTPS
 *      socket is closed and the LLM request aborts on the wire. This
 *      is what closes the cost-leak gap reported in v0.2.3.
 *
 * The signal must be ``emit()``ed on the executor that owns it (asio
 * rule). ``cancel()`` may be called from any thread; it stores the
 * flag eagerly and posts the emit onto the bound executor so the
 * actual signal fires on the right strand. ``bind_executor`` is
 * called by the engine on the first co_await after spawning, before
 * any HTTP work.
 */
#pragma once

#include <neograph/api.h>

#include <asio/any_io_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace neograph::graph {

/**
 * @brief Thrown by the engine when a run is cancelled mid-flight via
 *        ``CancelToken::cancel()``.
 *
 * Surfaces at the ``run_async`` / ``run_stream_async`` awaitable's
 * completion, and through the pybind binding's asyncio.Future as a
 * ``RuntimeError("run cancelled")`` (which the asyncio task wrapper
 * generally consumes silently after the upstream ``Future.cancel()``
 * already transitioned the future to CANCELLED).
 */
class NEOGRAPH_API CancelledException : public std::runtime_error {
public:
    CancelledException()
        : std::runtime_error("neograph: run cancelled") {}
    explicit CancelledException(const std::string& detail)
        : std::runtime_error("neograph: run cancelled — " + detail) {}
};

/**
 * @brief Cooperative cancel primitive shared between caller and engine.
 *
 * Construction is cheap (one atomic + one asio::cancellation_signal).
 * Pass via ``RunConfig::cancel_token`` (shared_ptr) to opt in.
 * Re-entrant: ``cancel()`` is idempotent; callers may share the token
 * across concurrent runs to fan out a single abort.
 */
class NEOGRAPH_API CancelToken {
public:
    CancelToken() = default;

    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    /**
     * @brief Request cancellation. Thread-safe, idempotent.
     *
     * Sets the polling flag immediately so subsequent
     * ``is_cancelled()`` checks return true. Posts the asio signal
     * emit onto the bound executor so any in-flight ``co_await`` —
     * including the LLM HTTP socket operation — receives an
     * ``operation_aborted`` error and unwinds cleanly.
     *
     * Safe to call before ``bind_executor()``: in that case only the
     * polling flag is set; whoever later binds the executor will
     * notice via ``is_cancelled()`` before any HTTP work has begun.
     */
    void cancel() noexcept {
        if (cancelled_.exchange(true, std::memory_order_acq_rel)) {
            return;  // already cancelled
        }

        // Snapshot the executor under the mutex; bind_executor and
        // cancel may race across threads.
        asio::any_io_executor ex_snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ex_snapshot = ex_;
        }
        if (ex_snapshot) {
            // Post the emit onto the strand that owns the signal —
            // emit() is documented as not thread-safe across executors.
            // The engine binds the same executor it co_spawns on.
            asio::post(ex_snapshot, [this]() {
                sig_.emit(asio::cancellation_type::all);
            });
        }
    }

    /// @brief Polling read of the cancel flag. Cheap, lock-free.
    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    /**
     * @brief Bind the executor that will handle ``cancellation_signal``
     *        emits. Called once by the engine's run-spawn lambda.
     *
     * If ``cancel()`` has already been requested (caller cancelled
     * before the run started) the bind also schedules an immediate
     * emit so the just-spawned coroutine unwinds at its first
     * co_await checkpoint.
     */
    void bind_executor(asio::any_io_executor ex) {
        bool fire_immediately = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ex_ = std::move(ex);
            fire_immediately = cancelled_.load(std::memory_order_acquire);
        }
        if (fire_immediately) {
            std::lock_guard<std::mutex> lk(mu_);
            if (ex_) {
                asio::post(ex_, [this]() {
                    sig_.emit(asio::cancellation_type::all);
                });
            }
        }
    }

    /**
     * @brief Cancellation slot for binding to a coroutine via
     *        ``asio::bind_cancellation_slot`` at ``co_spawn``. asio
     *        propagates the cancel through nested co_awaits down to
     *        socket operations.
     *
     * Slots are not thread-safe; this is consumed once by the
     * spawn site, then never read again by user code.
     */
    asio::cancellation_slot slot() noexcept {
        return sig_.slot();
    }

    /**
     * @brief Throws ``CancelledException`` if cancelled. Convenience
     *        wrapper used at engine super-step boundaries.
     */
    void throw_if_cancelled(const std::string& detail = {}) const {
        if (is_cancelled()) {
            throw CancelledException(detail);
        }
    }

private:
    std::atomic<bool>        cancelled_{false};
    mutable std::mutex       mu_;        // guards ex_ vs cancel() race
    asio::any_io_executor    ex_;        // bound by engine before HTTP I/O
    asio::cancellation_signal sig_;      // for asio operation cancel
};

/**
 * @brief Thread-local "current run" cancel token.
 *
 * The engine installs the active ``RunConfig::cancel_token`` here
 * before invoking each node's ``execute_full_async`` and removes it
 * after. ``Provider::complete()`` (sync path → fresh ``run_sync``
 * io_context that the engine's asio cancel slot cannot reach) reads
 * this so a node that synchronously calls ``provider.complete()``
 * still gets cancel propagation into the LLM HTTP request.
 *
 * Returns nullptr when no run is active on this thread, or when the
 * active run was started without a cancel_token.
 *
 * Set/clear via ``CurrentCancelTokenScope`` (RAII).
 */
NEOGRAPH_API CancelToken* current_cancel_token() noexcept;

/**
 * @brief RAII scope that sets ``current_cancel_token()`` for the
 *        enclosing block.
 */
class NEOGRAPH_API CurrentCancelTokenScope {
public:
    explicit CurrentCancelTokenScope(CancelToken* tok) noexcept;
    ~CurrentCancelTokenScope() noexcept;

    CurrentCancelTokenScope(const CurrentCancelTokenScope&) = delete;
    CurrentCancelTokenScope& operator=(const CurrentCancelTokenScope&) = delete;

private:
    CancelToken* prev_;
};

}  // namespace neograph::graph
