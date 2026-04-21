// Wire-protocol coverage for the MCPClient stdio transport after
// Stage 3 / Semester 2.7.
//
// detail::StdioSession now exposes rpc_call_async() that drives the
// subprocess pipes through asio::posix::stream_descriptor instead of
// blocking ::read/::write. The sync rpc_call() path is unchanged
// (still mutex+blocking I/O for callers that want the simple shape).
// MCPClient::rpc_call_async stdio branch routes to the new async
// path so a single io_context can multiplex many MCP servers without
// dedicating a thread per session.
//
// Test fixture: a minimal stdlib-only Python script
// (tests/fixtures/mcp_stdio_echo.py) implements just enough JSON-RPC
// to round-trip initialize + tools/list + tools/call. It avoids
// fastmcp so the test runs anywhere Python 3 is installed.

#include <gtest/gtest.h>
#include <neograph/mcp/client.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace neograph;

namespace {

std::filesystem::path fixture_path() {
    // Tests run from the build dir; the source tree is two levels up
    // from build-nopg/. We can't rely on cwd so derive from __FILE__.
    std::filesystem::path here(__FILE__);
    return here.parent_path() / "fixtures" / "mcp_stdio_echo.py";
}

// Locate a usable Python interpreter — `python3` on POSIX, `python`
// on Windows where most installs don't expose the versioned name.
// Returned string is whatever `system()` will accept as argv[0].
const char* python_cmd() {
#ifdef _WIN32
    // `where python` on Windows shells; `where` returns 0 if found.
    if (std::system("where python >nul 2>&1") == 0)  return "python";
    if (std::system("where python3 >nul 2>&1") == 0) return "python3";
    return nullptr;
#else
    if (std::system("command -v python3 >/dev/null 2>&1") == 0) return "python3";
    if (std::system("command -v python  >/dev/null 2>&1") == 0) return "python";
    return nullptr;
#endif
}

bool python3_available() { return python_cmd() != nullptr; }

} // namespace

TEST(MCPStdioAsync, RpcCallAsyncRoundTripsThroughSubprocess) {
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing: " << fixture;

    mcp::MCPClient client({python_cmd(), fixture.string()});

    // Build init params outside the coroutine — nested brace-init list
    // inside a coroutine body triggers a GCC 13 ICE (build_special_
    // member_call). Same pattern that bit Sem 1.5 conn_pool work.
    json init_params;
    init_params["protocolVersion"] = "2025-03-26";
    init_params["capabilities"] = json::object();
    init_params["clientInfo"] = json::object();
    init_params["clientInfo"]["name"] = "test";
    init_params["clientInfo"]["version"] = "0";

    asio::io_context io;
    json initialize_result;
    json tools_result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            initialize_result = co_await client.rpc_call_async(
                "initialize", init_params);
            tools_result = co_await client.rpc_call_async(
                "tools/list", json::object());
        },
        asio::detached);
    io.run();

    EXPECT_TRUE(initialize_result.is_object());
    EXPECT_TRUE(tools_result.is_object());
    ASSERT_TRUE(tools_result.contains("tools"));
    auto tools = tools_result["tools"];
    ASSERT_TRUE(tools.is_array());
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0].value("name", std::string{}), "echo");
}

TEST(MCPStdioAsync, ConcurrentAsyncCallsOnSameSessionSerializeSafely) {
    // Awaitable-mutex regression (Sem 4 follow-up). Before the lock
    // migrated off std::mutex, two coroutines on the same single-
    // threaded io_context calling the same session would deadlock:
    // the second's lock_guard blocked the worker the first needed
    // to drive its async read completions. Now the channel-backed
    // lock lets the second suspend cooperatively.
    //
    // Three concurrent rpc_call_async invocations — if the lock
    // worked, all three complete; if it deadlocked, io.run() never
    // returns (test harness would hang and time out). We also
    // assert they all got valid results.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();
    ASSERT_TRUE(std::filesystem::exists(fixture));

    mcp::MCPClient client({python_cmd(), fixture.string()});
    json init_params;
    init_params["protocolVersion"] = "2025-03-26";
    init_params["capabilities"] = json::object();
    init_params["clientInfo"] = json::object();
    init_params["clientInfo"]["name"] = "test";
    init_params["clientInfo"]["version"] = "0";

    asio::io_context io;
    std::atomic<int> done{0};
    std::array<json, 3> results;

    // First: a sequential initialize so subsequent calls hit the
    // post-handshake state. (The echo fixture tolerates any order
    // but real MCP servers require initialize first.)
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            co_await client.rpc_call_async("initialize", init_params);
            // Now fan out three parallel calls on the same session.
            // Each nested co_spawn returns a deferred op for a
            // parallel_group... but we can also just co_await three
            // sequential — the point of the regression is that the
            // FIRST starts holding the lock and the SECOND/THIRD
            // suspend on it cooperatively. We verify via co_spawn of
            // siblings on the same io_context.
            for (int i = 0; i < 3; ++i) {
                asio::co_spawn(
                    io,
                    [&, i]() -> asio::awaitable<void> {
                        results[i] = co_await client.rpc_call_async(
                            "tools/list", json::object());
                        done.fetch_add(1, std::memory_order_relaxed);
                    },
                    asio::detached);
            }
        },
        asio::detached);
    io.run();

    EXPECT_EQ(done.load(), 3);
    for (const auto& r : results) {
        ASSERT_TRUE(r.is_object())
            << "one of the concurrent calls produced no result";
        EXPECT_TRUE(r.contains("tools"));
    }
}

TEST(MCPStdioAsync, SyncFacadeStillWorksAlongsideAsync) {
    // The sync rpc_call() path was left intact (Sem 2.7 only added the
    // async peer). Verify a sync initialize+get_tools+call_tool still
    // works end-to-end against the same fixture so existing examples
    // that haven't migrated stay green.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();

    mcp::MCPClient client({python_cmd(), fixture.string()});
    ASSERT_TRUE(client.initialize());

    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "echo");

    auto out = client.call_tool("echo", json{{"msg", "hello"}});
    EXPECT_TRUE(out.is_object());
    ASSERT_TRUE(out.contains("content"));
}
