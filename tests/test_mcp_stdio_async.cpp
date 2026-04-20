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

bool python3_available() {
    return std::system("command -v python3 >/dev/null 2>&1") == 0;
}

} // namespace

TEST(MCPStdioAsync, RpcCallAsyncRoundTripsThroughSubprocess) {
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "fixture missing: " << fixture;

    mcp::MCPClient client({"python3", fixture.string()});

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

TEST(MCPStdioAsync, SyncFacadeStillWorksAlongsideAsync) {
    // The sync rpc_call() path was left intact (Sem 2.7 only added the
    // async peer). Verify a sync initialize+get_tools+call_tool still
    // works end-to-end against the same fixture so existing examples
    // that haven't migrated stay green.
    if (!python3_available()) {
        GTEST_SKIP() << "python3 not available";
    }
    auto fixture = fixture_path();

    mcp::MCPClient client({"python3", fixture.string()});
    ASSERT_TRUE(client.initialize());

    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "echo");

    auto out = client.call_tool("echo", json{{"msg", "hello"}});
    EXPECT_TRUE(out.is_object());
    ASSERT_TRUE(out.contains("content"));
}
