#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/mcp/hook_rpc.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace neograph;

namespace {
std::filesystem::path fixture_path() {
    return std::filesystem::path(__FILE__).parent_path() / "fixtures" / "mcp_stdio_slow.py";
}

const char* python_cmd() {
#ifdef _WIN32
    if (std::system("where python >nul 2>&1") == 0) return "python";
    if (std::system("where python3 >nul 2>&1") == 0) return "python3";
#else
    if (std::system("command -v python3 >/dev/null 2>&1") == 0) return "python3";
    if (std::system("command -v python >/dev/null 2>&1") == 0) return "python";
#endif
    return nullptr;
}

RpcRequest slow_request(std::chrono::steady_clock::time_point deadline) {
    return {json{{"jsonrpc", "2.0"}, {"id", "hook-test"}, {"method", "tools/call"},
                 {"params", {{"name", "echo"}, {"arguments", {{"delay_ms", 250}}}}}}.dump(),
            deadline, {}};
}

RpcRequest immediate_request() {
    return {json{{"jsonrpc", "2.0"}, {"id", "hook-test"}, {"method", "tools/list"},
                 {"params", json::object()}}.dump(),
            std::chrono::steady_clock::now() + std::chrono::seconds(1), {}};
}
} // namespace

TEST(McpHookRpcTransport, StdioTransportRetainsClient) {
    if (!python_cmd()) GTEST_SKIP() << "Python not available";
    ASSERT_TRUE(std::filesystem::exists(fixture_path()));

    auto client = std::make_shared<mcp::MCPClient>(
        std::vector<std::string>{python_cmd(), fixture_path().string()});
    auto transport = std::make_shared<mcp::StdioJsonRpcTransport>(client);
    client.reset(); // The transport, not a borrowed raw pointer, owns the client lifetime.

    const auto response = json::parse(async::run_sync(transport->request_async(immediate_request())));
    EXPECT_EQ(response["id"], "hook-test");
    EXPECT_TRUE(response.contains("error"));
}

TEST(McpHookRpcTransport, StdioTransportCancelsRequestAtDeadline) {
    if (!python_cmd()) GTEST_SKIP() << "Python not available";
    ASSERT_TRUE(std::filesystem::exists(fixture_path()));

    auto client = std::make_shared<mcp::MCPClient>(
        std::vector<std::string>{python_cmd(), fixture_path().string()});
    mcp::StdioJsonRpcTransport transport(std::move(client));

    EXPECT_THROW(async::run_sync(transport.request_async(
        slow_request(std::chrono::steady_clock::now() + std::chrono::milliseconds(25)))),
        std::runtime_error);
}
