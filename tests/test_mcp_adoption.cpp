#include <neograph/mcp/adoption.h>

#include <neograph/mcp/client.h>
#include <neograph/graph/cancel.h>
#include <neograph/provider.h>
#include <neograph/graph/checkpoint.h>
#include "fixtures/typed_provider.h"
#include <neograph/graph/store.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using namespace neograph::mcp;

struct TempSource {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("neograph-mcp-adoption-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path file = dir / "opencode" / "opencode.json";
    std::string previous_xdg = std::getenv("XDG_CONFIG_HOME") ? std::getenv("XDG_CONFIG_HOME") : "";
    TempSource() {
        std::filesystem::create_directories(file.parent_path());
        std::filesystem::permissions(dir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::filesystem::permissions(file.parent_path(), std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#ifndef _WIN32
        ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#else
        ::_putenv_s("XDG_CONFIG_HOME", dir.string().c_str());
#endif
    }
    ~TempSource() {
#ifndef _WIN32
        if (previous_xdg.empty()) ::unsetenv("XDG_CONFIG_HOME");
        else ::setenv("XDG_CONFIG_HOME", previous_xdg.c_str(), 1);
#else
        ::_putenv_s("XDG_CONFIG_HOME", previous_xdg.c_str());
#endif
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }
    void write(const std::string& value) {
        { std::ofstream out(file); out << value; }
        // Source custody must not depend on the process umask.
        std::filesystem::permissions(file,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }
};

TEST(McpAdoptionTest, DiscoveryDoesNotLaunchAndRedactsArguments) {
    TempSource source;
    source.write(R"({"mcp":{"fixture":{"type":"local","command":["/bin/echo","literal-token"],"cwd":"."}}})");
    OpenCodeGlobalMcpConfig config;
    config.source_path = source.file;
    const auto report = OpenCodeGlobalMcpDiscovery::discover(config);
    ASSERT_EQ(report.servers.size(), 1u);
    EXPECT_TRUE(report.rejected.empty());
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
    std::set<std::string> rejected_names;
    for (const auto& rejection : report.rejected) {
        rejected_names.insert(rejection.server_name);
        EXPECT_EQ(rejection.reason.find("value"), std::string::npos);
    }
    EXPECT_EQ(rejected_names, (std::set<std::string>{"disabled", "remote", "secret"}));
}

TEST(McpAdoptionTest, RegistryRequiresAttestationAndPolicy) {
    HardenedMcpClientRegistry registry;
    McpAdoptionRequest request;
    request.server.server_name = "fixture";
    request.server.enabled = true;
    request.server.argv = {"/bin/echo"};
    EXPECT_THROW(registry.adopt(request), std::invalid_argument);
}

TEST(McpAdoptionTest, RejectsExternalReferencesAtEveryStringLeaf) {
    TempSource source;
    for (const auto* argument : {"env:TOKEN", "file:/secret", "--token={env:TOKEN}",
                                 "prefix{file:/secret}suffix", "${env:TOKEN}"}) {
        source.write(neograph::json{{"mcp", {{"fixture", {
            {"type", "local"}, {"command", {"/bin/echo", argument}}}}}}}.dump());
        OpenCodeGlobalMcpConfig config;
        config.source_path = source.file;
        const auto report = OpenCodeGlobalMcpDiscovery::discover(config);
        EXPECT_TRUE(report.servers.empty()) << argument;
        ASSERT_EQ(report.rejected.size(), 1u) << argument;
        EXPECT_EQ(report.rejected.front().server_name, "fixture") << argument;
    }
}

TEST(McpAdoptionTest, RejectsProjectSourceMasqueradingAsUserGlobal) {
    TempSource source;
    const auto project = source.dir / "project.json";
    std::ofstream(project) << R"({"mcp":{"fixture":{"command":["/bin/echo"]}}})";
    OpenCodeGlobalMcpConfig config;
    config.source_path = project;
    EXPECT_TRUE(OpenCodeGlobalMcpDiscovery::discover(config).servers.empty());
}

#ifndef _WIN32
TEST(McpAdoptionTest, GroupWritableSourceIsRejectedBeforeEntryDiscovery) {
    TempSource source;
    source.write(R"({"mcp":{"fixture":{"type":"local","command":["/bin/echo"]}}})");
    OpenCodeGlobalMcpConfig config;
    config.source_path = source.file;
    const auto private_report = OpenCodeGlobalMcpDiscovery::discover(config);
    ASSERT_EQ(private_report.servers.size(), 1u);
    ASSERT_TRUE(private_report.rejected.empty());

    std::filesystem::permissions(source.file, std::filesystem::perms::group_write,
                                 std::filesystem::perm_options::add);
    const auto report = OpenCodeGlobalMcpDiscovery::discover(config);
    EXPECT_TRUE(report.servers.empty());
    ASSERT_EQ(report.rejected.size(), 1u);
    EXPECT_TRUE(report.rejected.front().server_name.empty());
    EXPECT_TRUE(report.source_content_hash.empty());
}

struct LocalMcpFixture {
    TempSource source;
    std::filesystem::path script = source.dir / "fixture.py";
    std::filesystem::path calls = source.dir / "calls";
    std::filesystem::path drift = source.dir / "drift";
    std::filesystem::path hanging = source.dir / "hang";
    neograph::json definition = {{"name", "lookup"}, {"description", "Bounded lookup"},
        {"inputSchema", {{"type", "object"}}}};

    LocalMcpFixture() {
        std::ofstream(script) << R"PY(import json,sys,pathlib,signal,time
signal.alarm(6)
root=pathlib.Path(sys.argv[1])
for line in sys.stdin:
 r=json.loads(line)
 if 'id' not in r: continue
 method=r['method']
 if method=='initialize':
  result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'fixture','version':'1'}}
 elif method=='tools/list':
  if (root/'hang').exists(): time.sleep(5)
  result={'tools':[{'name':'lookup','description':'Changed' if (root/'drift').exists() else 'Bounded lookup','inputSchema':{'type':'object'}}]}
 elif method=='tools/call':
  with (root/'calls').open('a') as f: f.write(json.dumps(r['params'])+'\n')
  result={'content':[],'structuredContent':{'value':'approved'},'isError':False}
 else: result={}
 print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':result}),flush=True)
)PY";
        source.write(neograph::json{{"mcp", {{"fixture", {{"type", "local"},
            {"command", {"/usr/bin/python3", script.string(), source.dir.string()}}}}}}}.dump());
    }
    McpAdoptionRequest request(McpTrustMode mode = McpTrustMode::trusted_mutable) {
        OpenCodeGlobalMcpConfig config;
        config.source_path = source.file;
        auto report = OpenCodeGlobalMcpDiscovery::discover(config);
        if (report.servers.size() != 1) throw std::runtime_error("fixture discovery failed");
        McpAdoptionRequest result;
        result.server = report.servers.front();
        result.launch = make_mcp_launch_approval(result.server, mode, true);
        result.tools = {"fixture", result.launch.executable_identity, {"lookup"},
            {{"lookup", mcp_tool_schema_hash(ToolDefinition::from_json(definition).to_json())}},
            {{"lookup", true, {"data_read"}, "exact-arguments-v1",
                {{"allowed_arguments", neograph::json::array({{{"query", "SELECT approved"}}})}}}},
            "mcp-local-v1"};
        return result;
    }
    neograph::json tool() const {
        return {{"id", "mcp.fixture.lookup"}, {"executor",
            {{"kind", "mcp"}, {"server_ref", "fixture"}, {"tool", "lookup"}}}};
    }
};

TEST(McpAdoptionTest, ValuePolicyDeniesMutationWithoutDispatch) {
    LocalMcpFixture fixture;
    HardenedMcpClientRegistry registry;
    registry.adopt(fixture.request());
    auto execute = registry.capability_executor();
    EXPECT_ANY_THROW(execute(fixture.tool(), {{"query", "DROP TABLE approved"}}, {}));
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
    EXPECT_EQ(execute(fixture.tool(), {{"query", "SELECT approved"}}, {}).at("value"), "approved");
}

TEST(McpAdoptionTest, GenericReadOnlyClaimIsRejectedBeforeLaunch) {
    LocalMcpFixture fixture;
    auto request = fixture.request();
    request.tools.manifest.front().argument_policy = "local-read-only";
    request.tools.manifest.front().argument_predicate = neograph::json::object();
    HardenedMcpClientRegistry registry;
    EXPECT_THROW(registry.adopt(request), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
}

TEST(McpAdoptionTest, PinnedScriptBytesCannotChangeAfterApproval) {
    LocalMcpFixture fixture;
    auto request = fixture.request(McpTrustMode::pinned);
    std::ofstream(fixture.script, std::ios::app) << "\n# changed executable script bytes\n";
    HardenedMcpClientRegistry registry;
    EXPECT_THROW(registry.adopt(request), std::invalid_argument);
}

TEST(McpAdoptionTest, RevocationClosesRetainedClientAndTool) {
    LocalMcpFixture fixture;
    HardenedMcpClientRegistry registry;
    registry.adopt(fixture.request());
    auto client = registry.clients().at("fixture");
    auto tools = client->get_tools();
    registry.revoke("fixture");
    EXPECT_ANY_THROW(client->call_tool("lookup", {{"query", "SELECT approved"}}));
    EXPECT_ANY_THROW(tools.front()->execute({{"query", "SELECT approved"}}));
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
}

TEST(McpAdoptionTest, SchemaDriftStalesAndDrainsRetainedClient) {
    LocalMcpFixture fixture;
    HardenedMcpClientRegistry registry;
    registry.adopt(fixture.request());
    auto client = registry.clients().at("fixture");
    std::ofstream(fixture.drift) << "changed";
    EXPECT_ANY_THROW(registry.capability_executor()(fixture.tool(), {{"query", "SELECT approved"}}, {}));
    EXPECT_FALSE(registry.contains("fixture"));
    EXPECT_ANY_THROW(client->call_tool("lookup", {{"query", "SELECT approved"}}));
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
}

TEST(McpAdoptionTest, CancellationInterruptsSchemaRefreshWithoutDispatch) {
    LocalMcpFixture fixture;
    HardenedMcpClientRegistry registry;
    registry.adopt(fixture.request());
    std::ofstream(fixture.hanging) << "hang";
    auto cancel = std::make_shared<neograph::graph::CancelToken>();
    std::thread trigger([cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel->cancel(); });
    const auto started = std::chrono::steady_clock::now();
    EXPECT_ANY_THROW(registry.capability_executor()(fixture.tool(), {{"query", "SELECT approved"}}, cancel));
    trigger.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
}

TEST(McpAdoptionTest, RetainedExecutorLosesAuthorityWhenRegistryIsDestroyed) {
    LocalMcpFixture fixture;
    HarnessCapabilityExecutor execute;
    {
        HardenedMcpClientRegistry registry;
        registry.adopt(fixture.request());
        execute = registry.capability_executor();
    }
    EXPECT_ANY_THROW(execute(fixture.tool(), {{"query", "SELECT approved"}}, {}));
    EXPECT_FALSE(std::filesystem::exists(fixture.calls));
}

class AdoptedToolProvider final : public neograph::test::LocalProvider {
public:
    AdoptedToolProvider()
        : LocalProvider([calls = std::make_shared<unsigned>(0)](
              neograph::ProviderRequest request, const neograph::PreparedProviderRequest&,
              const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            auto message = neograph::test::message("");
            if (++*calls == 1)
                message.parts = {sp::ToolCall{
                    "approved-call", "mcp.fixture.lookup", sp::ToolCallKind::ClientExecuted,
                    neograph::test::document(R"({"query":"SELECT approved"})")}};
            else {
                const sp::ToolResult* adopted_result = nullptr;
                for (const auto& history : std::get<sp::chat::Request>(request.payload).canonical_messages)
                    for (const auto& part : history.parts)
                        if (const auto* result = std::get_if<sp::ToolResult>(&part);
                            result && result->tool_use_id == "approved-call")
                            adopted_result = result;
                if (!adopted_result || adopted_result->is_error)
                    throw std::runtime_error("adopted tool result did not reach provider history");
                const auto value = neograph::json::parse(adopted_result->content).at("value");
                message = neograph::test::message(neograph::json{
                    {"status", "ok"}, {"findings", neograph::json::array({value})}}.dump());
            }
            co_return neograph::test::success(
                std::vector<sp::Message>{std::move(message)}, neograph::test::usage(0, 0, 0));
        }, "adoption-fixture", neograph::test::bounded_client("test-model", 16384)) {}
};

TEST(McpAdoptionTest, PinnedAdoptedToolRunsThroughCompileStartGet) {
    LocalMcpFixture fixture;
    HardenedMcpClientRegistry registry;
    registry.adopt(fixture.request(McpTrustMode::pinned));
    auto provider = std::make_shared<AdoptedToolProvider>();
    HarnessProviderExecutorConfig provider_config;
    provider_config.provider = provider;
    provider_config.model = "test-model";
    HarnessProgramHostConfig host;
    const auto hash = [](char value) { return "sha256:" + std::string(64, value); };
    const neograph::program::ExecutableIdentity identity{
        neograph::program::ExecutableKind::Provider, "harness.provider", "1.0.0", hash('d')};
    host.compiler_build_id = "adoption-test";
    host.provider_binding_identity = hash('c');
    host.snapshots.owner_scope = "adoption-test";
    host.snapshots.registry.provider = HarnessProviderRegistration{
        {identity, neograph::program::EffectMode::Brokered, "test-provider", {}, {}, {}},
        {neograph::json{{"type", "object"}}, neograph::json{{"type", "object"}}}};
    host.snapshots.allowed_module_digests = {hash('a'), hash('b'), hash('d')};
    host.snapshots.budget_ceiling = {10000, 1000000, 100000, 8, 1000, 100, 100, 8, 100};
    host.checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    host.state_store = std::make_shared<neograph::graph::InMemoryStore>();
    registry.configure_harness(host, provider_config);
    host.worker_executor = make_provider_harness_executor(std::move(provider_config));
    HarnessServiceConfig config;
    config.translation_defaults.provider = identity;
    config.translation_defaults.read_only_effects = {"data_read"};
    HarnessService service(config, nullptr, make_harness_program_service_resources(std::move(host)));
    const neograph::json request = {
        {"task", {{"objective", "Read approved value"}, {"acceptance", neograph::json::array({"approved"})}}},
        {"harness", {{"mode", "preset"}, {"preset", "fanout_judge"}}},
        {"workers", neograph::json::array({{{"id", "reader"}, {"instructions", "Return findings"},
            {"tools", neograph::json::array({"mcp.fixture.lookup"})},
            {"output_schema", {{"type", "object"}}}}})},
        {"tool_catalog", registry.tool_catalog()},
        {"budgets", {{"max_steps", 10}, {"timeout_seconds", 5}, {"max_parallel_workers", 1},
                     {"max_program_operations", 4}, {"max_worker_retries", 0}}}};
    const auto compiled = service.compile(request);
    ASSERT_TRUE(compiled.at("ok").get<bool>()) << compiled.dump();
    const auto started = service.start({{"artifact_id", compiled.at("artifact_id")}});
    ASSERT_TRUE(started.at("started").get<bool>()) << started.dump();
    neograph::json terminal;
    for (unsigned poll = 0; poll < 500; ++poll) {
        terminal = service.get(started.at("run_id").get<std::string>());
        if (terminal.value("status", "") != "queued" && terminal.value("status", "") != "running") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(terminal.value("status", ""), "completed") << terminal.dump();
    ASSERT_EQ(terminal.at("result").at("outcome"), "ok") << terminal.dump();
    EXPECT_EQ(terminal.at("result").at("valid_workers"), 1);
    EXPECT_EQ(terminal.at("result").at("findings"), neograph::json::array({"approved"}));
    std::ifstream calls(fixture.calls);
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(calls, line)));
    EXPECT_EQ(neograph::json::parse(line).at("arguments").at("query"), "SELECT approved");
    EXPECT_FALSE(static_cast<bool>(std::getline(calls, line)));
}
#endif
} // namespace
