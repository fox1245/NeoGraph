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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

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

        // v1.0 (9c): the legacy ``add_cancel_hook`` / hooks_ iteration
        // is gone — ``fork()`` is the canonical primitive for nested
        // cancel scopes, and every internal ``run_sync`` cascades via
        // a forked child.

        // Cascade to live children produced by ``fork()``.
        // Snapshot live shared_ptrs under the lock, then call
        // ``cancel()`` on each outside the lock — the recursive call
        // would otherwise re-enter ``children_mu_`` if a grandchild
        // exists, and lock_guard isn't reentrant.
        std::vector<std::shared_ptr<CancelToken>> live_children;
        {
            std::lock_guard<std::mutex> lk(children_mu_);
            live_children.reserve(children_.size());
            for (auto& w : children_) {
                if (auto sp = w.lock()) {
                    live_children.push_back(std::move(sp));
                }
            }
            // Don't bother pruning expired entries here — fork() does
            // it on the next call, and after this cancel() the parent
            // is "done" anyway.
        }
        for (auto& child : live_children) {
            child->cancel();
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

    /**
     * @brief Create a child token that cascades from this one.
     *
     * Each child has its **own** ``cancellation_signal``, so concurrent
     * consumers (multi-Send fan-out workers each calling
     * ``Provider::complete`` → ``run_sync`` with its own io_context)
     * never overwrite each other's cancellation slot. Calling
     * ``cancel()`` on the parent walks the live children list and
     * cascades — every child's signal fires on its own bound executor.
     *
     *
     * **Lifetime / ownership**: returns a ``shared_ptr<CancelToken>``;
     * the parent stores a ``weak_ptr`` so a forked child that goes
     * out of scope is automatically pruned from the cascade list on
     * the next ``cancel()`` / ``fork()``. The parent itself can outlive
     * its children freely.
     *
     * **Eager-cancel safety**: if the parent is already cancelled at
     * the time of ``fork()``, the new child is constructed with its
     * polling flag pre-set (``is_cancelled() == true``). When the
     * caller subsequently ``bind_executor`` s the child, the eager-
     * emit branch in ``bind_executor`` fires the child's signal on
     * its first co_await checkpoint. This closes the v0.3.2
     * "emit-vs-bind race" without an explicit short-circuit at
     * every consumer site.
     *
     * Pass-by-value into a coroutine frame is fine — ``shared_ptr``
     * copy is cheap and the parent reference inside the child stays
     * valid via shared ownership of the parent. (PR 2 trap shape
     * does NOT apply.)
     *
     * @see ROADMAP_v1.md "Execution plan" → PR 3
     */
    [[nodiscard]] std::shared_ptr<CancelToken> fork() {
        // Construct child outside any lock — its ctor takes no lock.
        // shared_ptr ctor allocates the control block; cheap relative
        // to a real cancel scope (one io_context spin-up downstream).
        auto child = std::shared_ptr<CancelToken>(new CancelToken());

        {
            std::lock_guard<std::mutex> lk(children_mu_);
            // Opportunistic prune: every fork() is a natural place to
            // drop expired weak_ptrs from previous children that have
            // already been released by their owners. Bounds the
            // children_ vector at the live-fan-out width even on
            // long-lived parents (e.g. an engine with 1000 LLM calls
            // per run, each forking once).
            children_.erase(
                std::remove_if(
                    children_.begin(), children_.end(),
                    [](const std::weak_ptr<CancelToken>& w) {
                        return w.expired();
                    }),
                children_.end());
            children_.push_back(child);
        }

        // Eager propagation: parent already cancelled → child sees
        // the polling flag immediately. ``bind_executor`` on the
        // child will then fire its signal at the next co_await.
        if (cancelled_.load(std::memory_order_acquire)) {
            child->cancel();
        }
        return child;
    }

private:
    std::atomic<bool>        cancelled_{false};
    mutable std::mutex       mu_;        // guards ex_ vs cancel() race
    asio::any_io_executor    ex_;        // bound by engine before HTTP I/O
    asio::cancellation_signal sig_;      // for asio operation cancel

    // Hierarchical cascade list. Each entry is a child token
    // produced by ``fork()``. ``cancel()`` walks live children and
    // cascades. Stored as ``weak_ptr`` so a child that goes out of
    // scope (its run_sync returned, run completed) is automatically
    // pruned on the next ``cancel()`` / ``fork()`` traversal.
    mutable std::mutex children_mu_;
    std::vector<std::weak_ptr<CancelToken>> children_;
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
