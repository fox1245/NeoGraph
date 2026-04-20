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
 */
#pragma once

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

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
template <typename T>
T run_sync(asio::awaitable<T> aw) {
    asio::io_context io;
    std::optional<T> result;
    std::exception_ptr err;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                result.emplace(co_await std::move(aw));
            } catch (...) {
                err = std::current_exception();
            }
            co_return;
        },
        asio::detached);

    io.run();

    if (err) std::rethrow_exception(err);
    return std::move(*result);
}

/// Void specialization — same semantics, no return value.
inline void run_sync(asio::awaitable<void> aw) {
    asio::io_context io;
    std::exception_ptr err;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                co_await std::move(aw);
            } catch (...) {
                err = std::current_exception();
            }
        },
        asio::detached);

    io.run();

    if (err) std::rethrow_exception(err);
}

} // namespace neograph::async
