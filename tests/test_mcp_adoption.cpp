#include <neograph/mcp/adoption.h>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

namespace {
using namespace neograph::mcp;

struct TempSource {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "neograph-mcp-adoption-test";
    std::filesystem::path file = dir / "opencode.json";
    TempSource() { std::filesystem::create_directories(dir); }
    ~TempSource() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    void write(const std::string& value) { std::ofstream out(file); out << value; }
};

TEST(McpAdoptionTest, DiscoveryDoesNotLaunchAndRedactsArguments) {
    TempSource source;
    source.write(R"({"mcp":{"fixture":{"type":"local","command":["/bin/echo","literal-token"],"cwd":"."}}})");
    OpenCodeGlobalMcpConfig config;
    config.source_path = source.file;
    const auto report = OpenCodeGlobalMcpDiscovery::discover(config);
    ASSERT_EQ(report.servers.size(), 1u);
    EXPECT_EQ(report.servers.front().source_id, "opencode:fixture");
    EXPECT_EQ(report.servers.front().argv.at(1), "literal-token");
    EXPECT_TRUE(report.servers.front().redacted_config_hash.rfind("sha256:", 0) == 0);
}

TEST(McpAdoptionTest, UnsupportedCredentialAndRemoteEntriesFailClosed) {
    TempSource source;
    source.write(R"({"mcp":{"secret":{"type":"local","command":["/bin/echo"],"environment":{"TOKEN":"value"}},"remote":{"type":"remote","url":"https://example.invalid"},"disabled":{"type":"local","enabled":false,"command":["/bin/echo"]}}})");
    OpenCodeGlobalMcpConfig config;
    config.source_path = source.file;
    const auto report = OpenCodeGlobalMcpDiscovery::discover(config);
    EXPECT_TRUE(report.servers.empty());
    ASSERT_EQ(report.rejected.size(), 3u);
    for (const auto& rejection : report.rejected) EXPECT_EQ(rejection.reason.find("value"), std::string::npos);
}

TEST(McpAdoptionTest, RegistryRequiresAttestationAndPolicy) {
    HardenedMcpClientRegistry registry;
    McpAdoptionRequest request;
    request.server.server_name = "fixture";
    request.server.enabled = true;
    request.server.argv = {"/bin/echo"};
    EXPECT_THROW(registry.adopt(request), std::invalid_argument);
}
} // namespace
