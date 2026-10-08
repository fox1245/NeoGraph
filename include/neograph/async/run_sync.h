/**
 * @file async/run_sync.h
 * @brief Block the calling thread until an asio::awaitable completes.
 *
 * Bridge utility for synchronous callers of owned coroutine operations.
 * The Provider::dispatch() facade uses it to drain dispatch_async() while
 * retaining the same prepared request and genuine SDK outcome.
 *
 * The helper owns a private io_context for the duration of the call;
 * it does not share the caller's executor. This is intentional —
 * sharing would deadlock a single-threaded io_context that already
 * sits inside `run()`. Cost: a tiny io_context construction per call.
 * Fine for sync-facade use, not for hot loops.
 *
 * `run_sync_pool` is the N-worker variant. The default `run_sync`
 * drives a single-threaded io_context so `asio::experimental::
 * make_parallel_group` inside the awaitable serializes on one thread;
 * the pool variant spreads those branches across workers so sync
 * callers of a parallel-fan-out coroutine still get real CPU
 * parallelism. Per-call pool spin-up is *not* free (one std::thread
 * per worker), so engines with a hot super-step loop should own a
 * long-lived executor instead of calling this per `run()`.
 */
#pragma once

#include <neograph/graph/cancel.h>

#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/system_error.hpp>
#include <asio/thread_pool.hpp>
#include <asio/traits/static_query.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace neograph::async {

namespace detail {

/// The io_context an engine run is currently driving on this thread through
/// `run_sync_operation`, or null. That context is private to the call and is
/// run by the calling thread alone, so every handler on it is already
/// serialised. Fan-out code uses this to avoid wrapping each branch in a
/// strand that cannot add any ordering. A foreign or unknown context never
/// matches, which keeps the strand.
inline const asio::execution_context*& serial_io_context() noexcept {
    static thread_local const asio::execution_context* context = nullptr;
    return context;
}

class SerialIoContextScope {
public:
    explicit SerialIoContextScope(const asio::execution_context& context) noexcept
        : previous_(serial_io_context()) {
        serial_io_context() = &context;
    }
    ~SerialIoContextScope() { serial_io_context() = previous_; }
    SerialIoContextScope(const SerialIoContextScope&) = delete;
    SerialIoContextScope& operator=(const SerialIoContextScope&) = delete;

private:
    const asio::execution_context* previous_;
};

// A queued completion can run before the foreign thread's execute() returns:
// io_context's scheduler unlocks its queue before signalling its wake event.
// Keep those calls (and the final tracked-work release) alive through their
// entire scheduler tail, not merely until their handler has completed.
class SyncIoQuiescence {
public:
    explicit SyncIoQuiescence(const asio::execution_context& context) noexcept
        : context_(&context) {}

    ~SyncIoQuiescence() { wait(); }

    void wait() noexcept {
        // The caller-thread-only path needs no locking or reference counting.
        if (!foreign_used_.load(std::memory_order_acquire)) return;
        std::unique_lock lock(mutex_);
        finished_.wait(lock, [this] { return active_ == 0; });
    }

    class Call {
    public:
        explicit Call(SyncIoQuiescence* owner) noexcept
            : owner_(owner && serial_io_context() != owner->context_ ? owner : nullptr) {
            if (owner_) {
                std::lock_guard lock(owner_->mutex_);
                ++owner_->active_;
                owner_->foreign_used_.store(true, std::memory_order_release);
            }
        }

        ~Call() {
            if (owner_) {
                std::lock_guard lock(owner_->mutex_);
                if (--owner_->active_ == 0) {
                    // Notify under the lock: wait() must also outlive this
                    // notification, rather than racing its own event teardown.
                    owner_->finished_.notify_all();
                }
            }
        }

        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;

    private:
        SyncIoQuiescence* owner_;
    };

private:
    const asio::execution_context* context_;
    std::atomic<bool> foreign_used_{false};
    std::mutex mutex_;
    std::condition_variable finished_;
    std::size_t active_ = 0;
};

// Only used for a synchronous bridge's private io_context. Property adaptations
// retain the fence, including through any_io_executor and cancellation strands.
// The concrete io_context executor plus this borrowed pointer fit inline in
// any_io_executor; the bridge owns the fence until all admitted calls return.
template <typename Executor>
class SyncIoExecutor {
public:
    SyncIoExecutor(Executor executor, SyncIoQuiescence& owner) noexcept
        : executor_(std::move(executor)), owner_(&owner) {}

    SyncIoExecutor(const SyncIoExecutor&) noexcept = default;
    SyncIoExecutor(SyncIoExecutor&& other) noexcept
        : executor_(std::move(other.executor_)),
          owner_(std::exchange(other.owner_, nullptr)) {}

    ~SyncIoExecutor() {
        if constexpr (asio::traits::static_query<
                          Executor, asio::execution::outstanding_work_t>::value() ==
                      asio::execution::outstanding_work.tracked) {
            SyncIoQuiescence::Call call(owner_);
            // Destroy the tracked executor under the fence. Its work_finished()
            // can let io.run() return before its scheduler stop/wake tail ends.
            auto released = std::move(executor_);
            static_cast<void>(released);
        }
    }

    SyncIoExecutor& operator=(SyncIoExecutor other) noexcept {
        using std::swap;
        swap(executor_, other.executor_);
        swap(owner_, other.owner_);
        return *this;
    }

    template <typename Function>
    void execute(Function&& function) const {
        SyncIoQuiescence::Call call(owner_);
        executor_.execute(std::forward<Function>(function));
    }

    template <typename Property>
    auto query(const Property& property) const
        noexcept(noexcept(asio::query(executor_, property)))
        -> decltype(asio::query(std::declval<const Executor&>(), property)) {
        return asio::query(executor_, property);
    }

    template <typename Property>
        requires asio::can_require<const Executor&, const Property&>::value
    auto require(const Property& property) const
        -> SyncIoExecutor<std::decay_t<asio::require_result_t<const Executor&, const Property&>>> {
        return {asio::require(executor_, property), *owner_};
    }

    template <typename Property>
        requires asio::can_prefer<const Executor&, const Property&>::value
    auto prefer(const Property& property) const
        -> SyncIoExecutor<std::decay_t<asio::prefer_result_t<const Executor&, const Property&>>> {
        return {asio::prefer(executor_, property), *owner_};
    }

    friend bool operator==(const SyncIoExecutor& left, const SyncIoExecutor& right) noexcept {
        return left.owner_ == right.owner_ && left.executor_ == right.executor_;
    }

    friend bool operator!=(const SyncIoExecutor& left, const SyncIoExecutor& right) noexcept {
        return !(left == right);
    }

private:
    Executor executor_;
    SyncIoQuiescence* owner_;
};

// Declare after the coroutine's captured state. Even an initiation failure
// destroys the actual context while its borrowed fence, TLS scope, cancellation
// signal and caller-owned coroutine captures are still alive.
class SyncIoRun {
public:
    explicit SyncIoRun(std::exception_ptr& operation_error)
        : io_(std::in_place), quiescence_(*io_), serial_scope_(*io_),
          operation_error_(operation_error) {}

    ~SyncIoRun() {
        if (io_) {
            quiescence_.wait();
            cleanup_signal_.slot().clear();
            io_.reset();
        }
    }

    auto executor() noexcept {
        return SyncIoExecutor(io_->get_executor(), quiescence_);
    }

    asio::cancellation_slot cleanup_slot() noexcept {
        return cleanup_signal_.slot();
    }

    template <typename Executor>
    void spawn(const Executor& executor, asio::awaitable<void> body,
               asio::cancellation_slot slot) {
        started_ = true;
        try {
            asio::co_spawn(executor, std::move(body),
                asio::bind_cancellation_slot(slot,
                    [this](std::exception_ptr error) {
                        completed_ = true;
                        if (error && !operation_error_) {
                            operation_error_ = std::move(error);
                        }
                    }));
        } catch (...) {
            started_ = false;
            remember(std::current_exception());
        }
    }

    template <typename Cancel>
    std::size_t drain(Cancel&& cancel) {
        for (;;) {
            std::size_t handled = 0;
            try {
                handled = io_->run();
            } catch (...) {
                // A side handler can throw while the owned coroutine is still
                // suspended. Preserve that failure, request cancellation, and
                // finish the frame before its slot binding/captures disappear.
                remember(std::current_exception());
                try {
                    cancel();
                } catch (...) {
                    // An ill-behaved cancellation callback must not make us
                    // abandon the still-owned frame. Drain its real completion
                    // and preserve the first escaping failure.
                    remember(std::current_exception());
                }
                // A throwing handler may also have stopped the context.
                // Restart before resuming drainage of its remaining work.
                io_->restart();
                continue;
            }
            if (!started_ || completed_) return handled;
            // A handler may explicitly stop the private context. That is not
            // completion of the operation owned by this blocking bridge.
            io_->restart();
        }
    }

    std::size_t drain() {
        return drain([this] { cleanup_signal_.emit(asio::cancellation_type::all); });
    }

    // Call only after every token lease has closed cancellation admission.
    void finish() {
        cleanup_signal_.slot().clear();
        quiescence_.wait();
        // Posts admitted after the first run exhausted are queued on a stopped
        // context. Invalidated token emits still own tracked strand invokers:
        // drain them before destroying the context or its borrowed fence.
        // Owned completion is not queue exhaustion. A finite caller-thread
        // cleanup handler may stop the context with more notifications queued;
        // each nonempty pass must be followed by a restarted run. A zero pass
        // waits for legitimate outstanding work, including foreign tracked
        // executor releases. Out-of-band foreign context.stop() is not owned
        // by this caller-thread bridge, just like other raw context escapes.
        do {
            io_->restart();
        } while (drain() != 0);
        quiescence_.wait();
        io_.reset();
        if (error_) std::rethrow_exception(error_);
    }

private:
    void remember(std::exception_ptr error) noexcept {
        if (error && !error_) error_ = std::move(error);
    }

    std::optional<asio::io_context> io_;
    SyncIoQuiescence quiescence_;
    SerialIoContextScope serial_scope_;
    asio::cancellation_signal cleanup_signal_;
    std::exception_ptr error_;
    std::exception_ptr& operation_error_;
    bool started_ = false;
    bool completed_ = false;
};

// Drive an engine operation with two cancellation scopes. The operation token
// is exposed through RunContext to node/provider code; the private execution
// child is reserved for the wrapper's co_spawn. Asio cancellation signals have
// one mutable slot, so binding both layers to the same token lets a nested
// consumer replace the wrapper handler and can leave its awaitable frame
// clearing freed slot state during teardown.
template <typename T>
T run_sync_operation(
    asio::awaitable<T> aw,
    std::shared_ptr<neograph::graph::CancelToken> operation) {
    if (operation) {
        operation->throw_if_cancelled("run_sync operation entry");
    }

    std::optional<T> result;
    std::exception_ptr err;

    auto body = [&](std::shared_ptr<neograph::graph::CancelToken> execution)
        -> asio::awaitable<void> {
        try {
            if (execution) execution->throw_if_cancelled("run_sync operation execution entry");
            result.emplace(co_await std::move(aw));
        } catch (...) {
            err = std::current_exception();
        }
        co_return;
    };
    SyncIoRun run(err);
    const auto executor = run.executor();

    if (operation) {
        // Keep the token visible to the operation's direct consumers while
        // reserving a distinct signal for the wrapper's awaitable frame.
        neograph::graph::CancelExecutorLease operation_lease(operation);
        const auto operation_executor = operation->bind_executor(executor);
        auto execution = operation->fork();
        neograph::graph::CancelExecutorLease execution_lease(execution);
        const auto execution_executor = execution->bind_executor(operation_executor);
        asio::post(execution_executor, [execution_executor, execution, &body, &run] {
            run.spawn(execution_executor, body(execution), execution->slot());
        });
        run.drain([&execution] { execution->cancel(); });
    } else {
        run.spawn(executor, body(std::shared_ptr<neograph::graph::CancelToken>{}),
                  run.cleanup_slot());
        run.drain();
    }
    run.finish();

    if (err) {
        if (operation && operation->is_cancelled()) {
            try {
                std::rethrow_exception(err);
            } catch (const asio::system_error& error) {
                if (error.code() == asio::error::operation_aborted) {
                    throw neograph::graph::CancelledException(
                        "run_sync operation aborted");
                }
            } catch (...) {
            }
        }
        std::rethrow_exception(err);
    }
    if (!result && operation && operation->is_cancelled()) {
        throw neograph::graph::CancelledException(
            "run_sync operation aborted before entry");
    }
    return std::move(*result);
}

} // namespace detail

/// Run @p aw to completion on a fresh single-threaded io_context
/// and return its result. Any exception thrown inside the coroutine
/// is rethrown on the caller's thread.
///
/// The awaitable should use `co_await asio::this_coro::executor` to
/// obtain an executor for nested operations; that executor will be
/// the temporary io_context created here.
///
/// When @p cancel is non-null, the wrapper binds its private execution child's
/// slot. Parent cancellation cascades to that child; cleanup does not grant the
/// child authority to cancel its parent. Provider dispatch supplies the explicit
/// ``ProviderRequest::cancel_token``; there is no ambient cancellation authority.
///
/// Executor-bound objects, tracked work and coroutines must remain inside this
/// call's lifetime. Finite caller-thread stop handlers are drained; escaping the
/// raw context to stop it from another thread is not supported. Completion alone
/// is not scheduler quiescence: foreign posting tails and late cancellation
/// notifications are drained before the private context is destroyed.
template <typename T>
T run_sync(asio::awaitable<T> aw,
           neograph::graph::CancelToken* cancel = nullptr) {
    // v0.3.2: short-circuit if the parent token is already cancelled.
    // Without this, the retry loop in NodeExecutor would re-call
    // a provider dispatch after a first cancel, fresh run_sync would
    // bind its slot AFTER add_cancel_hook fired its post-emit (the
    // emit-before-bind race), the cancel signal would be lost, and
    // the new HTTP request would run to completion — burning billable
    // tokens on every retry attempt. Throwing CancelledException
    // eagerly closes that loop: the executor's retry loop catches it
    // (alongside NodeInterrupt) and skips retries, so a second
    // cancelled call never hits the wire.
    if (cancel && cancel->is_cancelled()) {
        throw neograph::graph::CancelledException("run_sync entry");
    }

    std::optional<T> result;
    std::exception_ptr err;

    auto body = [&](std::shared_ptr<neograph::graph::CancelToken> child)
        -> asio::awaitable<void> {
        try {
            if (child) child->throw_if_cancelled("run_sync execution entry");
            result.emplace(co_await std::move(aw));
        } catch (...) {
            err = std::current_exception();
        }
        co_return;
    };
    detail::SyncIoRun run(err);
    const auto executor = run.executor();

    if (cancel) {
        // v0.4 PR 3: fork a child token for this nested run_sync.
        // The child has its own ``cancellation_signal``, bound to
        // this io_context's executor; ``parent.cancel()`` cascades
        // to every live child, so concurrent nested run_syncs
        // (such as multi-Send fan-out workers bridging HTTP
        // coroutines) all get their HTTP sockets torn down.
        // The pre-v0.3.1 design bound the parent's single
        // signal to io.get_executor() — last writer won, so only
        // one of N concurrent workers received cancel and the rest
        // streamed to completion. v0.3.1's ``add_cancel_hook`` list
        // was a workaround on top of that single-signal model;
        // ``fork()`` replaces it with a structural primitive.
        //
        // Lifetime: ``child`` is a ``shared_ptr`` so a late-firing
        // cascade (parent racing with body completion) doesn't UAF.
        // The parent stores a ``weak_ptr`` to the child, so once
        // ``child`` goes out of scope at the end of this function
        // the parent's children_ list naturally drops it.
        //
        // Eager-cancel race: if the parent was already cancelled
        // before ``fork()``, the child's polling flag is pre-set;
        // ``bind_executor`` then fires the child's signal at the
        // first co_await checkpoint of body(). No "emit-vs-bind"
        // window like the pre-v0.3.2 hook design, so the eager
        // ``is_cancelled()`` short-circuit at function entry stays
        // strictly as a tiny optimization (skip io_context
        // construction altogether).
        auto child = cancel->fork();
        neograph::graph::CancelExecutorLease child_lease(child);
        const auto child_executor = child->bind_executor(executor);
        asio::post(child_executor, [child_executor, child, &body, &run] {
            run.spawn(child_executor, body(child), child->slot());
        });
        run.drain([&child] { child->cancel(); });
        // ``child`` goes out of scope at end of block → parent's
        // weak_ptr expires → next parent.cancel()/fork() prunes it.

    } else {
        run.spawn(executor, body(std::shared_ptr<neograph::graph::CancelToken>{}),
                  run.cleanup_slot());
        run.drain();
    }
    run.finish();

    // Preserve the public bridge's existing typed cancellation translation,
    // but make every error decision after admission closure and final drain.
    if (err && cancel && cancel->is_cancelled()) {
        throw neograph::graph::CancelledException("run_sync inner abort");
    }

    if (err) std::rethrow_exception(err);
    return std::move(*result);
}

/// Void specialization — same semantics, no return value.
inline void run_sync(asio::awaitable<void> aw,
                     neograph::graph::CancelToken* cancel = nullptr) {
    // v0.3.2: same eager short-circuit as the templated peer above.
    // See that overload's comment for the retry/cost-leak rationale.
    if (cancel && cancel->is_cancelled()) {
        throw neograph::graph::CancelledException("run_sync entry");
    }

    std::exception_ptr err;

    auto body = [&](std::shared_ptr<neograph::graph::CancelToken> child)
        -> asio::awaitable<void> {
        try {
            if (child) child->throw_if_cancelled("run_sync execution entry");
            co_await std::move(aw);
        } catch (...) {
            err = std::current_exception();
        }
    };
    detail::SyncIoRun run(err);
    const auto executor = run.executor();

    if (cancel) {
        // v0.4 PR 3: fork a child token. See the templated peer
        // above for the full rationale; this is the bit-for-bit
        // void specialization.
        auto child = cancel->fork();
        neograph::graph::CancelExecutorLease child_lease(child);
        const auto child_executor = child->bind_executor(executor);
        asio::post(child_executor, [child_executor, child, &body, &run] {
            run.spawn(child_executor, body(child), child->slot());
        });
        run.drain([&child] { child->cancel(); });
    } else {
        run.spawn(executor, body(std::shared_ptr<neograph::graph::CancelToken>{}),
                  run.cleanup_slot());
        run.drain();
    }
    run.finish();

    if (err && cancel && cancel->is_cancelled()) {
        throw neograph::graph::CancelledException("run_sync inner abort");
    }

    if (err) std::rethrow_exception(err);
}

/// Run @p aw to completion on a fresh N-worker asio::thread_pool and
/// return its result. Unlike `run_sync`, inner `make_parallel_group`
/// branches execute on separate worker threads, so a sync caller of
/// a parallel-fan-out coroutine still sees real CPU parallelism.
///
/// `n_threads` is clamped to at least 1. Pool construction spawns
/// one std::thread per worker; cost is non-trivial for hot paths.
template <typename T>
T run_sync_pool(asio::awaitable<T> aw, std::size_t n_threads) {
    asio::thread_pool pool(n_threads > 0 ? n_threads : 1);
    std::optional<T> result;
    std::exception_ptr err;

    auto body = [&]() -> asio::awaitable<void> {
        try {
            result.emplace(co_await std::move(aw));
        } catch (...) {
            err = std::current_exception();
        }
        co_return;
    };
    asio::co_spawn(pool.get_executor(), body(), asio::detached);

    pool.join();

    if (err) std::rethrow_exception(err);
    return std::move(*result);
}

/// Void specialization — same semantics, no return value.
inline void run_sync_pool(asio::awaitable<void> aw, std::size_t n_threads) {
    asio::thread_pool pool(n_threads > 0 ? n_threads : 1);
    std::exception_ptr err;

    auto body = [&]() -> asio::awaitable<void> {
        try {
            co_await std::move(aw);
        } catch (...) {
            err = std::current_exception();
        }
    };
    asio::co_spawn(pool.get_executor(), body(), asio::detached);

    pool.join();

    if (err) std::rethrow_exception(err);
}

} // namespace neograph::async
