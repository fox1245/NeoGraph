/**
 * @file graph/cancel.h
 * @brief Cooperative cancellation primitive for graph runs.
 *
 * v0.3 introduces ``CancelToken`` so a caller can abort an in-flight
 * ``GraphEngine::run_async`` cleanly, including the LLM HTTP request
 * downstream of the engine super-step.
 *
 * Three propagation paths from one token:
 *
 *   1. **Polling** (`is_cancelled()`) — the engine super-step loop and
 *      the per-node dispatch checkpoint poll this between steps. Stops
 *      *future* node work after the next checkpoint, so a `time.sleep`
 *      in a Python node won't be preempted but the subsequent node
 *      won't fire.
 *
 *   2. **asio cancellation_signal** (`slot()`) — bound to an Asio
 *      coroutine via ``asio::bind_cancellation_slot`` at ``co_spawn``
 *      time. Asio propagates cancellation through awaited operations,
 *      including ``ConnPool::async_post``. Closing local transport does
 *      not establish that the server stopped its work or did not charge.
 *
 *   3. **std::stop_token** (`stop_token()`) — thread-safe subscriptions forward
 *      cancellation to native SDK operations without binding an Asio executor
 *      or occupying the caller's single cancellation slot.
 *
 * The signal must be ``emit()``ed and connected on one serial executor
 * (asio rule). ``cancel()`` may be called from any thread; it stores the
 * flag eagerly and posts the emit onto the returned serial executor.
 * ``bind_executor`` is called before the operation is spawned, and the
 * slot-bound ``co_spawn`` setup runs on that same executor.
 */
#pragma once

#include <neograph/api.h>

#include <asio/any_io_executor.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/post.hpp>
#include <asio/strand.hpp>

#include <algorithm>
#include <condition_variable>
#include <stop_token>
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
 * One stop source owns the cancellation state; Asio signals retain their
 * serial-executor contract. Native subscriptions need no executor binding.
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
     * Sets the stop state before synchronously invoking native subscriptions,
     * then posts the Asio signal onto its bound serial executor. Subscriptions
     * run without either token mutex held and must not throw.
     *
     * Safe before bind_executor(): native stop callbacks still run, and a
     * subsequently bound coroutine observes the same eager stop state.
     *
     * Engine-created operation children retain themselves until a posted
     * emit executes. A directly constructed token has no such ownership
     * source: callers that use ``bind_executor()`` themselves must keep that
     * token alive until the executor has drained all posted work.
     */
    void cancel() noexcept {
        if (stop_source_.stop_requested()) return;
        // Retain operation children before a native callback can release its
        // awaiting frame. No callback runs while either token mutex is held.
        auto keep_alive = self_keep_alive_for_post();
        if (!stop_source_.request_stop()) return;

        // Serialize executor submission with unbind_executor(), so completed
        // operations cannot post into a context that has begun teardown.
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (ex_) {
                asio::post(ex_,
                           [this, binding = binding_generation_,
                            keep_alive = std::move(keep_alive)]() {
                               emit_if_bound(binding);
                           });
            }
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
                if (auto sp = w.lock(); sp && sp.get() != this) {
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

    /// @brief Thread-safe observation of the same state used by subscriptions.
    [[nodiscard]] bool is_cancelled() const noexcept {
        return stop_source_.stop_requested();
    }

    /// @brief Native cancellation subscriptions independent of Asio slot ownership.
    [[nodiscard]] std::stop_token stop_token() const noexcept {
        return stop_source_.get_token();
    }

    /**
     * @brief Bind a serial executor that owns this token's
     *        ``cancellation_signal``.
     *
     * Returns the serial executor that **must** initiate every
     * ``co_spawn`` bound to ``slot()``. Callers running on another
     * executor first hop with ``co_await asio::post(bound,
     * asio::use_awaitable)``. This keeps slot connection/destruction and
     * ``emit()`` on one strand; generic multi-threaded executors do not
     * provide that guarantee themselves.
     *
     * If ``cancel()`` has already been requested, bind schedules an
     * immediate emit. The coroutine started on the returned executor must
     * still check ``throw_if_cancelled()`` before beginning I/O, because
     * that queued emit may precede its slot connection.
     *
     * @warning Outside GraphEngine, the caller owns the lifetime contract:
     * keep a directly constructed token alive until the bound executor drains.
     */
    [[nodiscard]] asio::any_io_executor bind_executor(asio::any_io_executor ex) {
        asio::any_io_executor bound;
        bool                   fire_immediately = false;
        std::uint64_t          binding = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ex_ = ex ? asio::any_io_executor(asio::make_strand(std::move(ex)))
                     : asio::any_io_executor{};
            binding = ++binding_generation_;
            bound = ex_;
            fire_immediately = stop_source_.stop_requested();
        }
        emit_finished_.notify_all();
        if (fire_immediately && bound) {
            auto keep_alive = self_keep_alive_for_post();
            asio::post(bound,
                       [this, binding, keep_alive = std::move(keep_alive)]() {
                           emit_if_bound(binding);
                       });
        }
        return bound;
    }

    /**
     * @brief Detach the serial executor after every bound operation drained.
     *
     * This does not drain slot-bound operations. The caller must first ensure
     * that no signal handler or slot-bound operation remains active, then call
     * it before destroying the executor's execution context. As with slot
     * connection, rebinding must not race this teardown.
     *
     * It serializes with ``cancel()`` and posted emits: detachment closes emit
     * admission and waits for an in-progress emit to finish, while queued work
     * from the old binding cannot emit into this or a later binding. Callers
     * must still drain queued work before context teardown and keep directly
     * constructed tokens alive until that drain.
     *
     * Destroys the last installed slot handler while its context is alive.
     * A completed ``co_spawn`` may leave that handler installed, retaining its
     * strand independently of the executor stored in this token.
     *
     * GraphEngine and the synchronous bridges call this on their internal
     * operation children. Direct callers of ``bind_executor()`` must do the
     * same after draining their executor when they retain the token longer
     * than that executor.
     */
    void unbind_executor() noexcept {
        asio::any_io_executor released;
        {
            std::unique_lock<std::mutex> lk(mu_);
            released = std::move(ex_);
            ++binding_generation_;
            emit_finished_.wait(lk, [this] { return !emitting_; });
        }
        // No emit can now touch the signal. Clear outside mu_: destruction of
        // a user's completed handler may itself request cancellation.
        sig_.slot().clear();
        // Keep the former executor alive through this method's end, while its
        // execution context is still known to be alive to the caller.
        static_cast<void>(released);
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
     * consumers (such as multi-Send fan-out workers bridging HTTP
     * coroutines through ``run_sync`` with their own io_context)
     * never overwrite each other's cancellation slot. Calling
     * ``cancel()`` on the parent walks the live children list and
     * cascades — every child's signal fires on its own bound executor.
     *
     * **Lifetime / ownership**: returns a ``shared_ptr<CancelToken>``;
     * the parent stores a ``weak_ptr`` so a forked child that goes
     * out of scope is automatically pruned from the cascade list on
     * the next ``cancel()`` / ``fork()``. A forked child also records a
     * weak self-reference in its existing child list. A posted signal emit
     * locks and captures that reference, retaining engine operation children
     * until the emit has executed.
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

        // The weak self-entry retains operation children through posted emits.
        // cancel() excludes this entry from parent-to-child propagation.
        {
            std::lock_guard<std::mutex> lk(child->children_mu_);
            child->children_.push_back(child);
        }

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
        if (stop_source_.stop_requested()) {
            child->cancel();
        }
        return child;
    }

private:
    void emit_if_bound(std::uint64_t binding) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            emit_finished_.wait(lk, [this, binding] {
                return !ex_ || binding != binding_generation_ || !emitting_;
            });
            if (!ex_ || binding != binding_generation_) return;
            emitting_ = true;
        }
        struct EmitCompletion {
            CancelToken& token;
            ~EmitCompletion() {
                {
                    std::lock_guard<std::mutex> lk(token.mu_);
                    token.emitting_ = false;
                }
                token.emit_finished_.notify_all();
            }
        } complete{*this};
        // Signal handlers may reenter bind_executor() or cancel(). Admission
        // is fenced above, but no token mutex may be held through a callback.
        sig_.emit(asio::cancellation_type::all);
    }

    std::shared_ptr<CancelToken> self_keep_alive_for_post() {
        std::lock_guard<std::mutex> lk(children_mu_);
        for (auto& candidate : children_) {
            if (auto self = candidate.lock(); self.get() == this) {
                return self;
            }
        }
        return {};
    }

    std::stop_source        stop_source_;
    mutable std::mutex       mu_;        // guards executor and emit admission
    std::condition_variable emit_finished_;
    std::uint64_t           binding_generation_ = 0;
    bool                    emitting_ = false;
    asio::any_io_executor    ex_;        // bound by engine before HTTP I/O
    asio::cancellation_signal sig_;      // for asio operation cancel

    // Hierarchical cascade list. Forked children retain a weak self-entry for
    // cancellation callbacks and posted emits; cancel() skips it when cascading.
    mutable std::mutex children_mu_;
    std::vector<std::weak_ptr<CancelToken>> children_;
};


// Keeps a token's executor binding scoped to the operation that drained it.
// Construct after the token is bound; destruction must occur before the
// corresponding io_context or thread_pool is torn down.
class CancelExecutorLease final {
public:
    explicit CancelExecutorLease(std::shared_ptr<CancelToken> token) noexcept
        : token_(std::move(token)) {}

    CancelExecutorLease(const CancelExecutorLease&) = delete;
    CancelExecutorLease& operator=(const CancelExecutorLease&) = delete;

    ~CancelExecutorLease() noexcept {
        if (token_) token_->unbind_executor();
    }

private:
    std::shared_ptr<CancelToken> token_;
};


// v1.0 (9d): the `current_cancel_token()` thread_local +
// `CurrentCancelTokenScope` RAII smuggling channel is gone.
// `RunContext::cancel_token` (on `NodeInput::ctx`) is the only cancel
// channel. Engine threads it through every dispatch.

}  // namespace neograph::graph
