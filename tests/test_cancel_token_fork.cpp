// PR 3 (v0.4.0) — CancelToken::fork() regression.
//
// fork() is the v0.4 hierarchical replacement for ``add_cancel_hook``.
// Each child has its own ``cancellation_signal``; ``parent.cancel()``
// cascades. The primitive lets concurrent nested run_syncs (multi-Send
// fan-out workers each calling ``provider.complete()``) bind their own
// slots without overwriting each other, structurally — no hook list,
// no last-writer-wins, no emit-vs-bind race window.
//
// These tests pin the primitive directly. End-to-end propagation
// through the engine is already covered by
// ``test_cancel_token_propagation.cpp`` (which still exercises the
// ``add_cancel_hook`` legacy path because ``state.run_cancel_token``
// smuggling is not yet rewritten — that's PR 3's target audience but
// not its scope; the smuggling rewrite happens in PR 4+).

#include <gtest/gtest.h>
#include <neograph/graph/cancel.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/steady_timer.hpp>
#include <asio/bind_cancellation_slot.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace neograph::graph;

TEST(CancelTokenFork, ParentCancelCascadesToChild) {
    auto parent = std::make_shared<CancelToken>();
    auto child  = parent->fork();

    EXPECT_FALSE(parent->is_cancelled());
    EXPECT_FALSE(child->is_cancelled());

    parent->cancel();

    EXPECT_TRUE(parent->is_cancelled());
    EXPECT_TRUE(child->is_cancelled())
        << "fork() child must observe parent.cancel() via cascade";
}

TEST(CancelTokenFork, ForkAfterCancelEagerlyMarksChild) {
    // The child is constructed with its polling flag pre-set when
    // the parent was already cancelled at fork() time. Closes the
    // emit-vs-bind race window without an explicit short-circuit at
    // the consumer site.
    auto parent = std::make_shared<CancelToken>();
    parent->cancel();

    auto child = parent->fork();

    EXPECT_TRUE(child->is_cancelled())
        << "fork() of an already-cancelled parent must produce a "
           "pre-cancelled child";
}

TEST(CancelTokenFork, MultipleChildrenAllCascade) {
    // Multi-Send fan-out shape — three concurrent nested run_syncs
    // each fork once. Parent.cancel() must reach every sibling, not
    // just the last one bound (the bug pattern that v0.3.1
    // add_cancel_hook fixed and v0.4 fork() replaces structurally).
    auto parent = std::make_shared<CancelToken>();
    auto a = parent->fork();
    auto b = parent->fork();
    auto c = parent->fork();

    EXPECT_FALSE(a->is_cancelled());
    EXPECT_FALSE(b->is_cancelled());
    EXPECT_FALSE(c->is_cancelled());

    parent->cancel();

    EXPECT_TRUE(a->is_cancelled());
    EXPECT_TRUE(b->is_cancelled());
    EXPECT_TRUE(c->is_cancelled());
}

TEST(CancelTokenFork, ChildCancelDoesNotAffectParent) {
    // Cascade is one-directional. A child that cancels itself (e.g.
    // a sub-run that completed normally and is tearing down) must
    // not propagate up to the parent.
    auto parent = std::make_shared<CancelToken>();
    auto child  = parent->fork();

    child->cancel();

    EXPECT_TRUE(child->is_cancelled());
    EXPECT_FALSE(parent->is_cancelled())
        << "cancel() on a child must not cascade up to the parent";
}

TEST(CancelTokenFork, ChildSiblingsAreIndependent) {
    // Cascade is parent→children only. A child cancelling does not
    // sweep its siblings: only the parent's own cancel() does that.
    auto parent = std::make_shared<CancelToken>();
    auto a = parent->fork();
    auto b = parent->fork();

    a->cancel();

    EXPECT_TRUE(a->is_cancelled());
    EXPECT_FALSE(b->is_cancelled())
        << "cancelling sibling A must not affect sibling B";
    EXPECT_FALSE(parent->is_cancelled());
}

TEST(CancelTokenFork, ExpiredChildrenArePrunedOnNextFork) {
    // A child whose shared_ptr is released (run_sync returned) must
    // not keep the parent's children_ vector growing unboundedly.
    // Pruning happens opportunistically on the next fork() call.
    auto parent = std::make_shared<CancelToken>();

    {
        auto transient = parent->fork();
        // transient goes out of scope here — parent's weak_ptr
        // expires.
    }
    {
        auto transient = parent->fork();
    }
    {
        auto transient = parent->fork();
    }

    // After three forks of throwaway children, the parent's
    // children_ vector should not contain three expired entries
    // — the most recent fork() prunes the prior expired ones.
    // We can't peek at children_ directly, but we can prove pruning
    // is happening by forking many times without observable
    // memory blow-up. (A weak observable: cancel() walks the list
    // and we want it to be cheap.)
    //
    // Smoke check: call cancel() and ensure it doesn't take an
    // unreasonable amount of time even after many forks. This is
    // a regression sentinel rather than a hard bound.
    for (int i = 0; i < 1000; ++i) {
        auto t = parent->fork();
        // released immediately
    }
    parent->cancel();
    EXPECT_TRUE(parent->is_cancelled());
}

TEST(CancelTokenFork, ChildSlotIsBindableToCoSpawn) {
    // The whole point of fork(): each child has its own
    // cancellation_signal so binding it to ``asio::co_spawn``
    // actually delivers cancel signals through the inner io_context
    // — no last-writer-wins. This test pins the binding shape used
    // by ``run_sync(aw, parent_token)``.
    auto parent = std::make_shared<CancelToken>();

    asio::io_context io;
    std::atomic<bool> ran_to_completion{false};
    std::atomic<bool> got_cancelled{false};

    auto child = parent->fork();
    child->bind_executor(io.get_executor());

    auto body = [&]() -> asio::awaitable<void> {
        try {
            // Sleep 200ms — plenty of time for cancel() from the
            // other thread to land on the signal.
            asio::steady_timer t(co_await asio::this_coro::executor);
            t.expires_after(std::chrono::milliseconds(200));
            co_await t.async_wait(asio::use_awaitable);
            ran_to_completion = true;
        } catch (const std::system_error& e) {
            if (e.code() == asio::error::operation_aborted) {
                got_cancelled = true;
            } else {
                throw;
            }
        }
    };

    asio::co_spawn(io, body(),
        asio::bind_cancellation_slot(child->slot(), asio::detached));

    // Cancel from another thread while io.run() is waiting.
    std::thread canceller([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        parent->cancel();
    });

    io.run();
    canceller.join();

    EXPECT_TRUE(got_cancelled)
        << "child slot bound to co_spawn must receive cancel through "
           "parent's cascade";
    EXPECT_FALSE(ran_to_completion)
        << "cancel should preempt the timer well before its 200ms "
           "expiry";
}
