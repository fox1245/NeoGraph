// A probe of the toolchain: the destructor of a coroutine's local runs by the time the awaiting caller observes that
// the coroutine has ended. The tests cover a coroutine that rethrows an exception it caught around a co_await, one that
// co_returns after catching one, one that rethrows an exception thrown before any co_await (#673), and one whose
// local is declared inside a try block whose exception is caught in the same coroutine.
// MSVC 19.51 with /fsanitize=address fails the tests of the shapes that end in an exception or catch one in a try
// block that declares a local, and the same compiler without the sanitizer passes all of them. The MSVC AddressSanitizer
// job runs this file first, and when it fails the job leaves out the tests that scripts/msvc_asan_unwind_dependent_tests.txt
// lists, because they depend on destruction during exception unwinding.
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

asio::awaitable<std::optional<int>> local_declared_inside_the_try(bool* destroyed, bool suspend_before_throw) {
    std::optional<int> result;
    try {
        LocalGuard guard{destroyed};
        result.emplace(co_await throwing_awaitable(suspend_before_throw));
    } catch (...) {
        co_return std::nullopt;
    }
    co_return result;
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

// A local declared inside the try block, with the exception caught inside the coroutine: the ordinary unwinding to a
// handler of the same coroutine.
TEST(CoroutineLocalDestructionTest, DestructorRunsWhenTheExceptionIsCaughtInsideTheCoroutine) {
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
                returned_value         = co_await local_declared_inside_the_try(&destroyed, suspend_before_throw);
                caller_resumed         = true;
                destroyed_when_resumed = destroyed;
            },
            asio::detached);
        io.run();
        EXPECT_TRUE(caller_resumed);
        EXPECT_FALSE(returned_value.has_value());
        EXPECT_TRUE(destroyed_when_resumed);
    }
}
