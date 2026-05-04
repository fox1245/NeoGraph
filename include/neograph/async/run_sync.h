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
        cancel->bind_executor(io.get_executor());
        asio::co_spawn(io, body(),
            asio::bind_cancellation_slot(cancel->slot(),
                                          asio::detached));
    } else {
        asio::co_spawn(io, body(), asio::detached);
    }

    io.run();

    if (err) std::rethrow_exception(err);
    return std::move(*result);
}

/// Void specialization — same semantics, no return value.
inline void run_sync(asio::awaitable<void> aw,
                     neograph::graph::CancelToken* cancel = nullptr) {
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
        cancel->bind_executor(io.get_executor());
        asio::co_spawn(io, body(),
            asio::bind_cancellation_slot(cancel->slot(),
                                          asio::detached));
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
