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

        // v0.3.1+: fire all registered cancel-hooks. Each nested
        // run_sync registers one — this is how concurrent run_syncs
        // (e.g. multi-Send fan-out workers each calling
        // provider.complete()) all get their HTTP sockets aborted.
        // The single cancellation_signal above only reaches the
        // outermost co_spawn; without per-consumer hooks, sibling
        // workers' run_syncs would silently overwrite each other's
        // bound executor and only one HTTP would be torn down.
        //
        // Invoke under the lock so a concurrent ``Hook`` destructor
        // either runs to completion before us (we won't see the
        // hook) or blocks until we finish (the hook always observes
        // a live capture).
        std::lock_guard<std::mutex> lk(hooks_mu_);
        for (auto& [id, cb] : hooks_) {
            (void)id;
            if (cb) cb();
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
     * @brief RAII handle returned by ``add_cancel_hook``. Destroying
     *        the handle unregisters the hook.
     *
     * Move-only. The destructor blocks on the parent token's
     * hooks_mu_ to serialize against an in-flight ``cancel()`` —
     * once the destructor returns, no further callbacks will fire
     * for this hook, so it is safe to destroy any state the
     * callback captured.
     */
    class Hook {
    public:
        Hook() noexcept = default;
        Hook(Hook&& o) noexcept : token_(o.token_), id_(o.id_) {
            o.token_ = nullptr;
            o.id_    = 0;
        }
        Hook& operator=(Hook&& o) noexcept {
            if (this != &o) {
                reset();
                token_ = o.token_;
                id_    = o.id_;
                o.token_ = nullptr;
                o.id_    = 0;
            }
            return *this;
        }
        Hook(const Hook&) = delete;
        Hook& operator=(const Hook&) = delete;
        ~Hook() noexcept { reset(); }

    private:
        friend class CancelToken;
        Hook(CancelToken* t, std::uint64_t id) noexcept
            : token_(t), id_(id) {}

        void reset() noexcept {
            if (token_ && id_ != 0) {
                token_->remove_hook_(id_);
            }
            token_ = nullptr;
            id_    = 0;
        }

        CancelToken*  token_ = nullptr;
        std::uint64_t id_    = 0;
    };

    /**
     * @brief Register a cancellation callback.
     *
     * Each nested ``run_sync`` (e.g. inside ``Provider::complete``)
     * creates its own private ``cancellation_signal`` and registers
     * a hook here that emits that signal on the inner io_context's
     * executor. ``cancel()`` then fans out to every registered
     * hook in addition to the single outer ``sig_`` emit, so
     * concurrent nested run_syncs (multi-Send fan-out workers) each
     * see their HTTP sockets aborted instead of silently sharing
     * one slot.
     *
     * If the token is already cancelled when this is called, the
     * callback fires synchronously and an empty handle is returned.
     */
    [[nodiscard]] Hook add_cancel_hook(std::function<void()> cb) {
        if (cancelled_.load(std::memory_order_acquire)) {
            // Already cancelled — fire and don't register.
            cb();
            return Hook{};
        }
        std::uint64_t id;
        {
            std::lock_guard<std::mutex> lk(hooks_mu_);
            // Re-check under lock to close the cancel-vs-register race.
            if (cancelled_.load(std::memory_order_acquire)) {
                // cancel() was called between the unlocked check and
                // here, but it has already iterated hooks_ — so it
                // won't see us. Fire ourselves.
                cb();
                return Hook{};
            }
            id = ++next_hook_id_;
            hooks_.emplace_back(id, std::move(cb));
        }
        return Hook{this, id};
    }

private:
    void remove_hook_(std::uint64_t id) noexcept {
        std::lock_guard<std::mutex> lk(hooks_mu_);
        auto it = std::find_if(hooks_.begin(), hooks_.end(),
            [id](const auto& p) { return p.first == id; });
        if (it != hooks_.end()) hooks_.erase(it);
    }

    std::atomic<bool>        cancelled_{false};
    mutable std::mutex       mu_;        // guards ex_ vs cancel() race
    asio::any_io_executor    ex_;        // bound by engine before HTTP I/O
    asio::cancellation_signal sig_;      // for asio operation cancel

    // Per-consumer hooks (nested run_sync). Guarded by its own
    // mutex so cancel() can iterate without contending with
    // bind_executor on mu_.
    mutable std::mutex hooks_mu_;
    std::uint64_t next_hook_id_ = 0;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> hooks_;
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
