/**
 * @file async/run_sync.h
 * @brief Block the calling thread until an asio::awaitable completes.
 *
 * Stage 3 / Semester 2.1 bridge utility. Lets sync code call an
 * awaitable without an ambient io_context. Used by the default
 * `Provider::complete()` implementation to delegate to
 * `complete_async()` when a subclass overrode only the async path.
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
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>

#include <cstddef>
#include <exception>
#include <optional>
#include <utility>

namespace neograph::async {

/// Run @p aw to completion on a fresh single-threaded io_context
/// and return its result. Any exception thrown inside the coroutine
/// is rethrown on the caller's thread.
///
/// The awaitable should use `co_await asio::this_coro::executor` to
/// obtain an executor for nested operations; that executor will be
/// the temporary io_context created here.
///
/// v0.3+: when @p cancel is non-null, the inner ``co_spawn`` binds
/// ``cancel->slot()`` so a concurrent ``cancel->cancel()`` aborts
/// the coroutine — including any in-flight ``co_await`` on a socket
/// op. Used by ``Provider::complete()`` to propagate
/// ``CompletionParams::cancel_token`` (or the thread-local current
/// token set by the engine before each node dispatch) into the LLM
/// HTTP request, so a cancelled run stops billable work mid-call.
template <typename T>
T run_sync(asio::awaitable<T> aw,
           neograph::graph::CancelToken* cancel = nullptr) {
    // v0.3.2: short-circuit if the parent token is already cancelled.
    // Without this, the retry loop in NodeExecutor would re-call
    // Provider::complete after a first cancel, fresh run_sync would
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

    asio::io_context io;
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

    if (cancel) {
        // v0.4 PR 3: fork a child token for this nested run_sync.
        // The child has its own ``cancellation_signal``, bound to
        // this io_context's executor; ``parent.cancel()`` cascades
        // to every live child, so concurrent nested run_syncs
        // (multi-Send fan-out workers each calling
        // ``provider.complete()``) all get their HTTP sockets torn
        // down. The pre-v0.3.1 design bound the parent's single
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
        child->bind_executor(io.get_executor());
        asio::co_spawn(io, body(),
            asio::bind_cancellation_slot(child->slot(),
                                          asio::detached));
        io.run();
        // ``child`` goes out of scope at end of block → parent's
        // weak_ptr expires → next parent.cancel()/fork() prunes it.

        // v0.3.2: if the inner co_spawn completed because of a cancel
        // (asio::system_error operation_aborted from a torn-down HTTP
        // socket), surface it as the typed CancelledException so the
        // executor's retry loop can short-circuit instead of treating
        // it as a transient runtime_error.
        if (err && cancel->is_cancelled()) {
            throw neograph::graph::CancelledException("run_sync inner abort");
        }
    } else {
        asio::co_spawn(io, body(), asio::detached);
        io.run();
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

    asio::io_context io;
    std::exception_ptr err;

    auto body = [&]() -> asio::awaitable<void> {
        try {
            co_await std::move(aw);
        } catch (...) {
            err = std::current_exception();
        }
    };

    if (cancel) {
        // v0.4 PR 3: fork a child token. See the templated peer
        // above for the full rationale; this is the bit-for-bit
        // void specialization.
        auto child = cancel->fork();
        child->bind_executor(io.get_executor());
        asio::co_spawn(io, body(),
            asio::bind_cancellation_slot(child->slot(),
                                          asio::detached));
        io.run();
        if (err && cancel->is_cancelled()) {
            throw neograph::graph::CancelledException("run_sync inner abort");
        }
        if (err) std::rethrow_exception(err);
        return;
    } else {
        asio::co_spawn(io, body(), asio::detached);
    }

    io.run();

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

    asio::co_spawn(
        pool.get_executor(),
        [&]() -> asio::awaitable<void> {
            try {
                result.emplace(co_await std::move(aw));
            } catch (...) {
                err = std::current_exception();
            }
            co_return;
        },
        asio::detached);

    pool.join();

    if (err) std::rethrow_exception(err);
    return std::move(*result);
}

/// Void specialization — same semantics, no return value.
inline void run_sync_pool(asio::awaitable<void> aw, std::size_t n_threads) {
    asio::thread_pool pool(n_threads > 0 ? n_threads : 1);
    std::exception_ptr err;

    asio::co_spawn(
        pool.get_executor(),
        [&]() -> asio::awaitable<void> {
            try {
                co_await std::move(aw);
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);

    pool.join();

    if (err) std::rethrow_exception(err);
}

} // namespace neograph::async
