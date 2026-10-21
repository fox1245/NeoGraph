/**
 * @file program/store_call.h
 * @brief How a Program store call is awaited, and how a synchronous caller waits for one (#556).
 *
 * The Program store interfaces return `asio::awaitable`. A store whose backend is synchronous (the in-memory store, SQLite,
 * PostgreSQL) derives from the matching `Sync...Store` class, which implements the awaitable interface over the store's
 * synchronous `..._sync` operations: at once on the calling thread, or, for a backend that blocks for long, on a thread of
 * a bounded pool. A caller that cannot suspend uses the matching `Blocking...Store`: it calls a synchronous store's
 * operation directly, and otherwise drives the awaitable on a private io_context and waits for it, unless its policy
 * says that waiting here would hang the thread that has to complete the call.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/blocking.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace neograph::async {
/// Declared in <neograph/async/host_loop.h>.
NEOGRAPH_API bool in_host_turn() noexcept;
}  // namespace neograph::async

namespace neograph::program {

/** When a `Blocking...Store` may wait for a store call that does not complete at once. */
struct BlockingStorePolicy {
    /**
     * Called before the facade blocks its thread on a store that is not synchronous. A non-empty answer refuses the
     * call: the facade throws ProgramBlockingCallRefused with that text as the reason. The Program runtime sets it so
     * that a call made on a thread that runs the runtime's executor, or on a host loop, is refused instead of hanging.
     * Empty: the facade waits, except inside a turn of an async::HostLoop, where every facade refuses whatever its
     * policy says (async::in_host_turn): the store could complete only in a later turn.
     */
    std::function<std::string()> refuse;

    /**
     * Call even a synchronous store through its awaitable operations, and wait for them as for any other store,
     * instead of calling its `..._sync` operations directly. The conformance suite sets it to run a store through
     * the awaitable form; the Program runtime does not.
     */
    bool await_synchronous_stores = false;
};

namespace detail {

/**
 * Runs `work` on a thread of a bounded pool and resumes the awaiting coroutine on its own executor. The pool is the one
 * that the checkpoint stores use for a synchronous backend. Throws SynchronousCheckpointStoreUnavailable in a build
 * without threads.
 */
NEOGRAPH_PROGRAM_API asio::awaitable<void> offload_to_blocking_pool(std::function<void()> work);

/**
 * Runs the synchronous operation `fn` on a thread of the bounded pool and resumes the awaiting coroutine on its own
 * executor. `fn` owns everything it needs, so the work stays valid if the awaiting coroutine is destroyed first.
 */
template <typename R, typename Fn>
asio::awaitable<R> offload_sync_call(Fn fn) {
    auto owned = std::make_shared<Fn>(std::move(fn));
    if constexpr (std::is_void_v<R>) {
        co_await offload_to_blocking_pool([owned] { (*owned)(); });
        co_return;
    } else {
        auto result = std::make_shared<std::optional<R>>();
        co_await offload_to_blocking_pool([owned, result] { result->emplace((*owned)()); });
        co_return std::move(**result);
    }
}

/** An awaitable that throws `error` when it is awaited: an operation the store does not support. */
template <typename R>
asio::awaitable<R> failed_store_call(std::exception_ptr error) {
    std::rethrow_exception(std::move(error));
    if constexpr (!std::is_void_v<R>) co_return R{};  // never reached; this is what makes the function a coroutine
}

template <typename R>
asio::awaitable<std::optional<R>> box_store_call(asio::awaitable<R> call) {
    co_return std::optional<R>(co_await std::move(call));
}

[[noreturn]] inline void refuse_blocking_store_call(std::string_view operation, std::string reason) {
    throw ProgramBlockingCallRefused(std::string(operation), {}, std::move(reason));
}

inline void check_blocking_store_call(const BlockingStorePolicy& policy, std::string_view operation) {
    if (policy.refuse)
        if (auto reason = policy.refuse(); !reason.empty()) refuse_blocking_store_call(operation, std::move(reason));
    // A view that has no policy (the catalog's, the module resolver's, the grant resolver's) still must not block the
    // thread that turns a host's event loop.
    if (async::in_host_turn())
        refuse_blocking_store_call(operation,
                                   "it is called inside a turn of a host's event loop, which a blocked thread cannot turn");
}

/**
 * Waits for `call` on a private io_context of the calling thread. The wait ends when the call does: Asio keeps the
 * context running for as long as the coroutine exists, so a store that completes only through another event loop would
 * never end it. That wait is refused before the call starts (check_blocking_store_call), not detected afterwards.
 */
template <typename R>
R block_on_store(asio::awaitable<R> call, const BlockingStorePolicy& policy, std::string_view operation) {
    check_blocking_store_call(policy, operation);
    asio::io_context   io(1);
    std::optional<R>   value;
    std::exception_ptr failure;
    bool               finished = false;
    asio::co_spawn(io, box_store_call<R>(std::move(call)),
                   [&](std::exception_ptr error, std::optional<R> result) {
                       failure  = std::move(error);
                       value    = std::move(result);
                       finished = true;
                   });
    io.run();
    // run() returns only when the call is done (the context stays busy for as long as the coroutine exists), so this is
    // a defence: an unfinished call is never reported as a result.
    if (!finished)
        refuse_blocking_store_call(operation, "the store call did not finish on this thread");
    if (failure) std::rethrow_exception(failure);
    return std::move(*value);
}

inline void block_on_store(asio::awaitable<void> call, const BlockingStorePolicy& policy, std::string_view operation) {
    check_blocking_store_call(policy, operation);
    asio::io_context   io(1);
    std::exception_ptr failure;
    bool               finished = false;
    asio::co_spawn(io, std::move(call), [&](std::exception_ptr error) {
        failure  = std::move(error);
        finished = true;
    });
    io.run();
    // run() returns only when the call is done (the context stays busy for as long as the coroutine exists), so this is
    // a defence: an unfinished call is never reported as a result.
    if (!finished)
        refuse_blocking_store_call(operation, "the store call did not finish on this thread");
    if (failure) std::rethrow_exception(failure);
}

}  // namespace detail
}  // namespace neograph::program
