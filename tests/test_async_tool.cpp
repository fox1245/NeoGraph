// Stage 3 / Sem 4.2 — AsyncTool adapter regression.
//
// AsyncTool lets users implement Tool::execute as a coroutine
// (execute_async returning awaitable<string>) while preserving the
// sync Tool contract that ToolDispatchNode and the rest of the
// engine call against. The sync execute() wraps execute_async via
// run_sync, so each invocation gets a private io_context and the
// adapter is safe from any thread.

#include <gtest/gtest.h>
#include <neograph/tool.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>

using namespace neograph;

namespace {

class EchoTool : public AsyncTool {
public:
    std::atomic<int> calls{0};

    ChatTool get_definition() const override {
        ChatTool t;
        t.name = "echo";
        t.description = "Echo argument back";
        t.parameters = json::object();
        return t;
    }
    std::string get_name() const override { return "echo"; }

    asio::awaitable<std::string> execute_async(const json& args) override {
        ++calls;
        co_return args.value("msg", std::string{"(none)"});
    }
};

class TimerTool : public AsyncTool {
public:
    ChatTool get_definition() const override {
        ChatTool t;
        t.name = "timer";
        t.description = "Wait then return marker";
        t.parameters = json::object();
        return t;
    }
    std::string get_name() const override { return "timer"; }

    asio::awaitable<std::string> execute_async(const json& args) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(args.value("ms", 5)));
        co_await t.async_wait(asio::use_awaitable);
        co_return "ok";
    }
};

class ThrowingTool : public AsyncTool {
public:
    ChatTool get_definition() const override {
        ChatTool t;
        t.name = "boom";
        t.description = "Always throws";
        t.parameters = json::object();
        return t;
    }
    std::string get_name() const override { return "boom"; }

    asio::awaitable<std::string> execute_async(const json&) override {
        throw std::runtime_error("intentional");
        co_return std::string{};  // unreachable, keeps it a coroutine
    }
};

} // namespace

TEST(AsyncTool, SyncExecuteRunsCoroutineToCompletion) {
    EchoTool t;
    auto out = t.execute(json{{"msg", "hi"}});
    EXPECT_EQ(out, "hi");
    EXPECT_EQ(t.calls.load(), 1);
}

TEST(AsyncTool, AwaitableUsableDirectlyFromCoroutine) {
    // GCC 13 ICE workaround: build the json arg outside the coroutine
    // body. Nested brace-init inside a coroutine trips
    // build_special_member_call (call.cc:11096) — same shape as the
    // bite in test_mcp_stdio_async (Sem 2.7).
    EchoTool t;
    json args;
    args["msg"] = "direct";

    asio::io_context io;
    std::string got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            got = co_await t.execute_async(args);
        },
        asio::detached);
    io.run();
    EXPECT_EQ(got, "direct");
}

TEST(AsyncTool, TimerAwaitInsideToolWorksThroughSyncExecute) {
    // The async work performs an asio::steady_timer wait — it would
    // dangle without a backing io_context. The sync execute() spins
    // up a private one via run_sync, so the timer fires correctly.
    TimerTool t;
    auto start = std::chrono::steady_clock::now();
    auto out = t.execute(json{{"ms", 30}});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(out, "ok");
    EXPECT_GE(elapsed, 25);  // some scheduling slack
}

TEST(AsyncTool, ExceptionPropagatesThroughSyncExecute) {
    ThrowingTool t;
    EXPECT_THROW(t.execute(json::object()), std::runtime_error);
}
