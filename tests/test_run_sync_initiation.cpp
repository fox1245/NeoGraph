// What a blocking bridge's caller gets when the coroutine that the bridge started is destroyed without completing:
// the initiation of an operation that it awaits throws, or the operation destroys its completion handler without
// calling it. The first test pins what the vendored Asio does in that case; the others pin that async::run_sync and
// detail::run_sync_operation return to their caller with that exception, or with OperationAbandoned when there is
// none.
//
// A bridge that waits for the completion would never return here, so these tests run in an executable of their own
// whose ctest TIMEOUT ends such a test. They wait for nothing themselves: every bridge runs on the test's thread.

#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>

#include <asio/async_result.hpp>
#include <asio/awaitable.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

class InitiationFailure : public std::runtime_error {
public:
    InitiationFailure() : std::runtime_error("initiation failed") {}
};

class SideHandlerFailure : public std::runtime_error {
public:
    SideHandlerFailure() : std::runtime_error("side handler failed") {}
};

struct InitiationProbe {
    bool entered = false;
    bool caught = false;
    bool frame_destroyed = false;
    bool completed = false;
    bool completion_dropped = false;
};

// The completion of co_spawn: it notes whether it was called, or destroyed without being called.
class ProbeCompletion {
public:
    explicit ProbeCompletion(InitiationProbe& probe) : probe_(&probe) {}
    ProbeCompletion(ProbeCompletion&& other) noexcept : probe_(std::exchange(other.probe_, nullptr)) {}
    ProbeCompletion(const ProbeCompletion&) = delete;
    ProbeCompletion& operator=(const ProbeCompletion&) = delete;
    ProbeCompletion& operator=(ProbeCompletion&&) = delete;
    ~ProbeCompletion() {
        if (probe_) probe_->completion_dropped = true;
    }
    void operator()(std::exception_ptr) {
        std::exchange(probe_, nullptr)->completed = true;
    }

private:
    InitiationProbe* probe_;
};

struct FrameLifetime {
    InitiationProbe& probe;
    ~FrameLifetime() { probe.frame_destroyed = true; }
};

// A coroutine that awaits an operation whose initiation throws, with the completion token the engine's own
// operations use (a use_awaitable bound to the coroutine's executor).
asio::awaitable<void> await_throwing_initiation(InitiationProbe& probe) {
    FrameLifetime lifetime{probe};
    probe.entered = true;
    auto token = asio::bind_executor(co_await asio::this_coro::executor, asio::use_awaitable);
    try {
        co_await asio::async_initiate<decltype(token), void()>([](auto) { throw InitiationFailure{}; }, token);
    } catch (...) {
        probe.caught = true;
    }
}

// A coroutine that awaits an operation which destroys its completion handler without calling it, and throws
// nothing: an abandoned operation.
asio::awaitable<void> await_dropped_handler(InitiationProbe& probe) {
    FrameLifetime lifetime{probe};
    probe.entered = true;
    auto token = asio::bind_executor(co_await asio::this_coro::executor, asio::use_awaitable);
    try {
        co_await asio::async_initiate<decltype(token), void()>([](auto) {}, token);
    } catch (...) {
        probe.caught = true;
    }
}

// The same initiation reached after the coroutine has been suspended once, so that it runs from a handler of the
// bridge's context and not from the bridge's start of the coroutine.
asio::awaitable<void> await_throwing_initiation_after_a_suspension(InitiationProbe& probe) {
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    co_await await_throwing_initiation(probe);
}

asio::awaitable<int> value_after(asio::awaitable<void> operation) {
    co_await std::move(operation);
    co_return 42;
}

// A handler posted beside the coroutine throws first; the coroutine then reaches an initiation that throws. It does
// not let the cancellation that the bridge requests after the first failure end it before that initiation.
asio::awaitable<int> initiation_throws_after_side_handler(InitiationProbe& probe) {
    co_await asio::this_coro::throw_if_cancelled(false);
    const auto executor = co_await asio::this_coro::executor;
    asio::post(executor, [] { throw SideHandlerFailure{}; });
    co_await asio::post(executor, asio::use_awaitable);  // queued after the side handler
    co_await await_throwing_initiation(probe);
    co_return 42;
}

void expect_frame_gone_unresumed(const InitiationProbe& probe) {
    EXPECT_TRUE(probe.entered);
    EXPECT_FALSE(probe.caught) << "the awaiting coroutine saw the failure";
    EXPECT_TRUE(probe.frame_destroyed) << "the bridge returned while the coroutine's frame was alive";
}

}  // namespace

// What the vendored Asio does with an exception that the initiation of an awaited operation throws: the awaiting
// coroutine never sees it. It leaves run() of the context whose handler resumed the coroutine; the coroutine's
// frames are destroyed in a later handler without being resumed; and the completion handler of co_spawn is
// destroyed without being called, with no work left on the context. A bridge that waited only for that completion
// would have nothing left that could deliver it.
TEST(AsioInitiationFailure, LeavesRunAndDropsTheAwaitingCoroutineWithoutCompletion) {
    InitiationProbe probe;
    asio::io_context io;
    asio::co_spawn(io, await_throwing_initiation(probe), ProbeCompletion(probe));
    EXPECT_THROW(io.run(), InitiationFailure);
    EXPECT_TRUE(probe.entered);
    EXPECT_FALSE(probe.caught) << "the exception reached the awaiting coroutine";
    io.restart();
    io.run();
    EXPECT_TRUE(io.stopped()) << "work is left on the context";
    EXPECT_TRUE(probe.frame_destroyed);
    EXPECT_FALSE(probe.caught);
    EXPECT_FALSE(probe.completed) << "the completion of co_spawn was called";
    EXPECT_TRUE(probe.completion_dropped);
}

TEST(RunSyncInitiationFailure, ValueBridgeRethrowsIt) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::run_sync(value_after(await_throwing_initiation(probe))), InitiationFailure);
    expect_frame_gone_unresumed(probe);
}

TEST(RunSyncInitiationFailure, VoidBridgeRethrowsIt) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::run_sync(await_throwing_initiation(probe)), InitiationFailure);
    expect_frame_gone_unresumed(probe);
}

TEST(RunSyncInitiationFailure, OperationBridgeRethrowsIt) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::detail::run_sync_operation(value_after(await_throwing_initiation(probe)), nullptr),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
}

TEST(RunSyncInitiationFailure, ValueBridgeWithACancelTokenRethrowsItWithoutCancellingTheParent) {
    InitiationProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(value_after(await_throwing_initiation(probe)), &parent),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSyncInitiationFailure, VoidBridgeWithACancelTokenRethrowsItWithoutCancellingTheParent) {
    InitiationProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(await_throwing_initiation(probe), &parent), InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSyncInitiationFailure, OperationBridgeWithAnOperationTokenRethrowsItWithoutCancellingTheOperation) {
    InitiationProbe probe;
    auto operation = std::make_shared<neograph::graph::CancelToken>();
    EXPECT_THROW(neograph::async::detail::run_sync_operation(value_after(await_throwing_initiation(probe)), operation),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(operation->is_cancelled());
}

// With a token the bridge starts the coroutine from a handler of its context, so an initiation in the coroutine's
// first steps throws through that start; one reached after a suspension throws from a later handler.
TEST(RunSyncInitiationFailure, ValueBridgeWithACancelTokenRethrowsOneReachedAfterASuspension) {
    InitiationProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(value_after(await_throwing_initiation_after_a_suspension(probe)), &parent),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSyncInitiationFailure, VoidBridgeWithACancelTokenRethrowsOneReachedAfterASuspension) {
    InitiationProbe probe;
    neograph::graph::CancelToken parent;
    EXPECT_THROW(neograph::async::run_sync(await_throwing_initiation_after_a_suspension(probe), &parent),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(parent.is_cancelled());
}

TEST(RunSyncInitiationFailure, OperationBridgeWithAnOperationTokenRethrowsOneReachedAfterASuspension) {
    InitiationProbe probe;
    auto operation = std::make_shared<neograph::graph::CancelToken>();
    EXPECT_THROW(neograph::async::detail::run_sync_operation(
                     value_after(await_throwing_initiation_after_a_suspension(probe)), operation),
                 InitiationFailure);
    expect_frame_gone_unresumed(probe);
    EXPECT_FALSE(operation->is_cancelled());
}

// The bridge reports the first failure that left its context, as it does when two side handlers throw.
TEST(RunSyncInitiationFailure, ASideHandlerThatFailedFirstIsTheFailureReported) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::run_sync(initiation_throws_after_side_handler(probe)), SideHandlerFailure);
    expect_frame_gone_unresumed(probe);
}

TEST(RunSyncInitiationFailure, VoidBridgeReportsAnAbandonedOperation) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::run_sync(await_dropped_handler(probe)), neograph::async::OperationAbandoned);
    expect_frame_gone_unresumed(probe);
}

TEST(RunSyncInitiationFailure, ValueBridgeReportsAnAbandonedOperation) {
    InitiationProbe probe;
    EXPECT_THROW(neograph::async::run_sync(value_after(await_dropped_handler(probe))),
                 neograph::async::OperationAbandoned);
    expect_frame_gone_unresumed(probe);
}
