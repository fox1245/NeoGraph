#include <gtest/gtest.h>

#include <neograph/mcp/client.h>
#include <neograph/mcp/harness_mcp_backend.h>

#include <filesystem>
#include <memory>

namespace {

const char* python_cmd() {
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

TEST(HarnessMcpBackendTest, CallsConfiguredDownstreamServer) {
    auto fixture = std::filesystem::path(__FILE__).parent_path()
        / "fixtures" / "mcp_stdio_echo.py";
    auto client = std::make_shared<neograph::mcp::MCPClient>(
        std::vector<std::string>{python_cmd(), fixture.string()});
    ASSERT_TRUE(client->initialize("harness-mcp-backend-test"));
    auto executor = neograph::mcp::make_mcp_harness_capability_executor(
        {{"repo", client}});

    neograph::json tool = {
        {"id", "repo.echo"},
        {"executor", {{"kind", "mcp"}, {"server_ref", "repo"},
                       {"tool", "echo"}}},
    };
    auto result = executor(tool, {{"msg", "hello"}},
        std::make_shared<neograph::graph::CancelToken>());

    ASSERT_TRUE(result.contains("content"));
    ASSERT_EQ(result["content"].size(), 1u);
    EXPECT_NE(result["content"][0]["text"].get<std::string>().find("hello"),
              std::string::npos);
}

TEST(HarnessMcpBackendTest, RejectsUnknownServerReference) {
    auto executor = neograph::mcp::make_mcp_harness_capability_executor({});
    neograph::json tool = {
        {"id", "repo.echo"},
        {"executor", {{"kind", "mcp"}, {"server_ref", "missing"}}},
    };
    EXPECT_THROW(executor(tool, neograph::json::object(),
                         std::make_shared<neograph::graph::CancelToken>()),
                 std::runtime_error);
}

} // namespace
