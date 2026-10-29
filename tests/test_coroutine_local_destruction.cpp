// A coroutine local's destructor has run by the time an awaiting caller observes the coroutine's exit, in three
// shapes: the coroutine rethrows an exception it caught around a co_await, it co_returns after catching one, and it
// rethrows an exception that was thrown before any co_await (#673). The test runs on every platform and in the
// sanitizer builds, so a compiler that ends a coroutine without running its locals' destructors fails here.
#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct LocalGuard {
    bool*       destroyed = nullptr;
    std::string payload   = "coroutine-local-guard";
    ~LocalGuard() {
        if (destroyed != nullptr) *destroyed = true;
    }
};

asio::awaitable<int> throwing_awaitable(bool suspend_before_throw) {
    if (suspend_before_throw) {
        const auto executor = co_await asio::this_coro::executor;
        co_await asio::post(executor, asio::use_awaitable);
    }
    throw std::runtime_error("awaited operation threw");
    co_return 0;
}

asio::awaitable<std::optional<int>> rethrow_after_caught_co_await(bool* destroyed, bool suspend_before_throw) {
    LocalGuard         guard{destroyed};
    std::optional<int> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await throwing_awaitable(suspend_before_throw));
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

asio::awaitable<std::optional<int>> co_return_after_caught_co_await(bool* destroyed, bool suspend_before_throw) {
    LocalGuard         guard{destroyed};
    std::optional<int> result;
    std::exception_ptr failure;
    try {
        result.emplace(co_await throwing_awaitable(suspend_before_throw));
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) co_return std::nullopt;
    co_return result;
}

asio::awaitable<std::optional<int>> rethrow_after_throw_before_any_co_await(bool* destroyed) {
    LocalGuard         guard{destroyed};
    std::exception_ptr failure;
    try {
        throw std::runtime_error("threw before any co_await");
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) std::rethrow_exception(failure);
    co_return std::nullopt;
}

}  // namespace

TEST(CoroutineLocalDestructionTest, DestructorRunsWhenCoroutineRethrowsAfterCatchAroundCoAwait) {
    for (const bool suspend_before_throw : {false, true}) {
        SCOPED_TRACE(suspend_before_throw ? "suspended co_await" : "immediate co_await");
        asio::io_context io;
        bool             destroyed             = false;
        bool             caller_saw_exception  = false;
        bool             destroyed_when_caught = false;
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                try {
                    (void)co_await rethrow_after_caught_co_await(&destroyed, suspend_before_throw);
                } catch (const std::runtime_error&) {
                    caller_saw_exception  = true;
                    destroyed_when_caught = destroyed;
                }
            },
            asio::detached);
        io.run();
        EXPECT_TRUE(caller_saw_exception);
        EXPECT_TRUE(destroyed_when_caught);
        EXPECT_TRUE(destroyed);
    }
}

TEST(CoroutineLocalDestructionTest, DestructorRunsWhenCoroutineCoReturnsAfterCatchAroundCoAwait) {
    for (const bool suspend_before_throw : {false, true}) {
        SCOPED_TRACE(suspend_before_throw ? "suspended co_await" : "immediate co_await");
        asio::io_context   io;
        bool               destroyed              = false;
        bool               caller_resumed         = false;
        bool               destroyed_when_resumed = false;
        std::optional<int> returned_value         = 42;
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
                returned_value         = co_await co_return_after_caught_co_await(&destroyed, suspend_before_throw);
                caller_resumed         = true;
                destroyed_when_resumed = destroyed;
            },
            asio::detached);
        io.run();
        EXPECT_TRUE(caller_resumed);
        EXPECT_FALSE(returned_value.has_value());
        EXPECT_TRUE(destroyed_when_resumed);
        EXPECT_TRUE(destroyed);
    }
}

TEST(CoroutineLocalDestructionTest, DestructorRunsWhenCoroutineRethrowsAfterThrowBeforeAnyCoAwait) {
    asio::io_context io;
    bool             destroyed             = false;
    bool             caller_saw_exception  = false;
    bool             destroyed_when_caught = false;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            try {
                (void)co_await rethrow_after_throw_before_any_co_await(&destroyed);
            } catch (const std::runtime_error&) {
                caller_saw_exception  = true;
                destroyed_when_caught = destroyed;
            }
        },
        asio::detached);
    io.run();
    EXPECT_TRUE(caller_saw_exception);
    EXPECT_TRUE(destroyed_when_caught);
    EXPECT_TRUE(destroyed);
}
