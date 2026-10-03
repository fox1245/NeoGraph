#include "provider_example_support.h"
#include <sp/config_defaults.h>
#include <neograph/mcp/adoption.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/harness_host_agent.h>
#include <neograph/mcp/harness_mcp_backend.h>
#include <neograph/mcp/server.h>
#include <neograph/program/admission.h>
#ifdef NEOGRAPH_HARNESS_HAVE_HTTP
#include <neograph/mcp/http_server.h>

#include <openssl/crypto.h>
#include <openssl/sha.h>
#endif
#ifdef NEOGRAPH_HARNESS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/mcp/sqlite_harness_store.h>
#endif

#ifdef NEOGRAPH_HARNESS_HAVE_HTTP
#include <array>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>
#endif
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
namespace {

std::string environment(const char* primary, const char* fallback = nullptr) {
    if (const auto* value = std::getenv(primary); value && *value) return value;
    if (fallback) {
        if (const auto* value = std::getenv(fallback); value && *value) return value;
    }
    return {};
}

#ifdef NEOGRAPH_HARNESS_HAVE_HTTP
std::vector<std::string> split_csv(const std::string& value) {
    std::vector<std::string> result;
    std::istringstream       input(value);
    std::string              item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) result.push_back(std::move(item));
    }
    return result;
}

using TokenDigest = std::array<unsigned char, SHA256_DIGEST_LENGTH>;

TokenDigest digest(std::string_view value) {
    TokenDigest result{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), result.data());
    return result;
}

bool secure_equal(const TokenDigest& expected, std::string_view candidate) {
    const auto actual = digest(candidate);
    return CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) == 0;
}
#endif

// Private offline fixture contract: this callback performs no model inference.
// These caps are not facts about any hosted/public model.
constexpr std::uint64_t kSmokeInputTokens = 4096;
constexpr std::uint64_t kSmokeOutputTokens = 256;
constexpr const char* kSmokeModel = "harness-smoke";

std::shared_ptr<sp::runtime::Client> smoke_client() {
    auto source = neograph::json::parse(sp::config_defaults::descriptor_policy_json);
    neograph::json defaults;
    for (const auto& family : source.at("families")) {
        if (family.at("family") == "openai.chat") {
            defaults = family.at("defaults");
            break;
        }
    }
    source.at("models").push_back({
        {"family", "openai.chat"}, {"model", kSmokeModel},
        {"defaults", std::move(defaults)}, {"input_limit", kSmokeInputTokens},
        {"output_limit", kSmokeOutputTokens}});
    auto admitted = sp::descriptor::load_policy(
        source.dump(), sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::make_shared<sp::runtime::Client>(examples::admitted_descriptor(
        "http://127.0.0.1:18080", "openai.chat", "/v1/chat/completions",
        "neograph-harness-smoke",
        std::get<sp::descriptor::PolicySnapshot>(std::move(admitted))));
}

class SmokeReviewProvider final : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = smoke_client();
public:
    std::string_view family() const noexcept override { return "openai.chat"; }
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        return prepare_local(client_, std::move(request),
            [](const neograph::PreparedProviderRequest&,
               const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                sp::Completion completion;
                completion.messages.push_back(examples::message(
                    sp::Role::Assistant, R"({"status":"ok","findings":[]})"));
                completion.stop.kind = sp::StopKind::EndTurn;
                // Exact local accounting: constructing this synthetic review
                // uses no model tokens, including either cache-price band.
                completion.usage.input_total = sp::Count{0, sp::Evidence::Derived};
                completion.usage.output_total = sp::Count{0, sp::Evidence::Derived};
                completion.usage.total = sp::Count{0, sp::Evidence::Derived};
                completion.usage.input_uncached = sp::Count{0, sp::Evidence::Derived};
                completion.usage.cache_read = sp::Count{0, sp::Evidence::Derived};
                completion.usage.cache_write = sp::Count{0, sp::Evidence::Derived};
                completion.usage.stage = sp::UsageStage::Final;
                examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string get_name() const override { return "harness-smoke-review"; }
};

neograph::mcp::McpAdoptionRequest load_adoption_approval(
    const std::filesystem::path& path, const neograph::mcp::McpDiscoveryReport& discovered) {
    if (path.empty() || std::filesystem::file_size(path) > 1024 * 1024)
        throw std::invalid_argument("MCP requires a bounded independent approval file");
    std::ifstream input(path);
    const auto document = neograph::json::parse(input);
    const auto& launch = document.at("launch");
    const auto& tools = document.at("tools");
    neograph::mcp::McpAdoptionRequest request;
    const auto server = tools.at("server_name").get<std::string>();
    const auto found = std::find_if(discovered.servers.begin(), discovered.servers.end(),
        [&](const auto& value) { return value.server_name == server; });
    if (found == discovered.servers.end()) throw std::invalid_argument("approved MCP server was not discovered");
    request.server = *found;
    const auto mode = launch.at("trust_mode").get<std::string>();
    if (mode != "pinned" && mode != "trusted_mutable") throw std::invalid_argument("unknown MCP trust mode");
    request.launch.trust_mode = mode == "pinned" ? neograph::mcp::McpTrustMode::pinned
                                                : neograph::mcp::McpTrustMode::trusted_mutable;
    request.launch.argv_no_credentials_attested = launch.at("argv_no_credentials_attested").get<bool>();
    request.launch.source_path = launch.at("source_path").get<std::string>();
    request.launch.source_content_hash = launch.at("source_content_hash").get<std::string>();
    request.launch.cwd = launch.at("cwd").get<std::string>();
    request.launch.executable = launch.at("executable").get<std::string>();
    request.launch.executable_identity = launch.at("executable_identity").get<std::string>();
    request.launch.argv_hash = launch.at("argv_hash").get<std::string>();
    request.launch.interpreter_identity = launch.at("interpreter_identity").get<std::string>();
    request.launch.package_identity = launch.at("package_identity").get<std::string>();
    request.tools.server_name = server;
    request.tools.launch_identity = tools.at("launch_identity").get<std::string>();
    request.tools.policy_version = tools.at("policy_version").get<std::string>();
    request.tools.selected_tools = tools.at("selected_tools").get<std::vector<std::string>>();
    for (auto it = tools.at("schema_hashes").begin(); it != tools.at("schema_hashes").end(); ++it)
        request.tools.schema_hashes.emplace(it.key(), it.value().get<std::string>());
    for (const auto& value : tools.at("manifest")) {
        neograph::mcp::McpCapabilityManifest manifest;
        manifest.tool = value.at("tool").get<std::string>();
        manifest.auto_execute = value.at("auto_execute").get<bool>();
        for (const auto& effect : value.at("effects")) manifest.effects.insert(effect.get<std::string>());
        manifest.argument_policy = value.at("argument_policy").get<std::string>();
        manifest.argument_predicate = value.at("argument_predicate");
        request.tools.manifest.push_back(std::move(manifest));
    }
    return request;
}

}  // namespace

int main(int argc, char** argv) {
    if (std::getenv("NEOGRAPH_HARNESS_HOST_DEPTH")) {
        std::cerr << "Nested NeoGraph Harness execution is disabled\n";
        return 2;
    }
    const bool smoke_mode = environment("NEOGRAPH_HARNESS_SMOKE") == "1";
    auto executor = environment("NEOGRAPH_HARNESS_EXECUTOR");
    auto host_model = environment("NEOGRAPH_HARNESS_HOST_MODEL");
    auto mcp_source = environment("NEOGRAPH_HARNESS_MCP_SOURCE");
    const auto mcp_approval = environment("NEOGRAPH_HARNESS_MCP_APPROVAL");
    bool status_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        if (flag == "--executor" && index + 1 < argc) executor = argv[++index];
        else if (flag == "--host-model" && index + 1 < argc) host_model = argv[++index];
        else if (flag == "--host-status") status_only = true;
        else {
            std::cerr << "Usage: neograph-harness-mcp [--executor provider|opencode|claude|codex]"
                         " [--host-model MODEL] [--host-status]\n";
            return 2;
        }
    }
    if (executor.empty()) executor = "provider";
    const bool host_mode = executor != "provider";
    if (host_mode && (executor != "opencode" && executor != "claude" && executor != "codex")) {
        std::cerr << "Select --executor provider, opencode, claude, or codex; auto is not supported\n";
        return 2;
    }
    if (host_mode && environment("NEOGRAPH_HARNESS_TRANSPORT") == "http") {
        std::cerr << "Host CLI delegation is local stdio only; HTTP needs explicit provider credentials\n";
        return 2;
    }
    neograph::mcp::McpDiscoveryReport mcp_discovery;
    if (!mcp_source.empty() || !mcp_approval.empty()) {
        if (environment("NEOGRAPH_HARNESS_TRANSPORT") == "http") {
            std::cerr << "Global MCP adoption is single-user local stdio only\n";
            return 2;
        }
        neograph::mcp::OpenCodeGlobalMcpConfig discovery;
        discovery.source_path = mcp_source;
        mcp_discovery = neograph::mcp::OpenCodeGlobalMcpDiscovery::discover(discovery);
        if (status_only) {
            for (const auto& server : mcp_discovery.servers)
                std::cerr << "MCP discovered (not started): " << server.server_name
                          << " source=" << server.source_id
                          << " config_hash=" << server.redacted_config_hash << '\n';
            for (const auto& rejection : mcp_discovery.rejected)
                std::cerr << "MCP rejected: " << rejection.server_name
                          << " reason=" << rejection.reason << '\n';
        }
    }
    neograph::mcp::HostAgentExecutorConfig agent_config;
    if (host_mode) {
        agent_config.host = executor;
        agent_config.model = host_model;
        agent_config.workspace = std::filesystem::current_path();
        const auto preflight = neograph::mcp::preflight_host_agent(agent_config);
        std::cerr << "Host: " << preflight.host << ", executable: "
                  << (preflight.executable.empty() ? "missing" : "available")
                  << ", model: " << preflight.model << ", version: " << preflight.version
                  << ", mode: local read-only CLI\n" << preflight.detail << '\n';
        if (status_only) return preflight.available ? 0 : 2;
        if (!preflight.available) return 2;
    } else if (status_only) {
        std::cerr << "Provider mode: direct API credentials (no host login preflight)\n";
        return 0;
    }
    const auto api_key = host_mode ? std::string{} :
                         environment("NEOGRAPH_HARNESS_API_KEY", "OPENROUTER_API_KEY");
    if (!host_mode && !smoke_mode && api_key.empty()) {
        std::cerr << "Set NEOGRAPH_HARNESS_API_KEY or OPENROUTER_API_KEY, or select a local host executor\n";
        return 2;
    }

    const std::string provider_base_url = "https://openrouter.ai";
    const std::string provider_model = smoke_mode ? kSmokeModel : examples::openrouter_model;
    std::shared_ptr<neograph::Provider> provider;
    if (smoke_mode && !host_mode) {
        provider = std::make_shared<SmokeReviewProvider>();
    } else if (!host_mode) {
        provider = examples::make_openrouter_provider(api_key, "chat");
    }
    constexpr const char* kProviderBindingIdentity =
        "sha256:7df70a8b692b53148480c9eb019db87cac5c2c7e1c53a351ff628651ab219c14";
    constexpr const char* kProviderImplementationDigest =
        "sha256:67ac4b0f2d57b2ca169623008f2fad2ff4ed726e05eb570a36f69fa4edad7847";
    constexpr const char* kHostImplementationDigest =
        "sha256:b3e0def356fbca55e33c78a7029232c413f9cfb17923de6735ce15f4f568e2b6";
    constexpr const char* kSupportModuleDigest =
        "sha256:a328e0824ca6438669e42ee0bb8c42634cc9613d66240cdd649c36b3b279030d";
    constexpr const char* kToolingModuleDigest =
        "sha256:aa8500648d5b30cecb1ef06dcf2eb861b126928de6715ff54bbe7857105b285f";

    neograph::mcp::HarnessServiceConfig harness_config;
    neograph::mcp::HarnessProgramHostConfig host_config;
    std::shared_ptr<neograph::mcp::HardenedMcpClientRegistry> adopted_mcp;
    if (!mcp_source.empty() || !mcp_approval.empty()) {
        if (host_mode)
            throw std::invalid_argument("adopted MCP capability execution requires the provider worker executor");
        adopted_mcp = std::make_shared<neograph::mcp::HardenedMcpClientRegistry>();
        adopted_mcp->adopt(load_adoption_approval(mcp_approval, mcp_discovery));
    }
    host_config.compiler_build_id = "neograph-harness-example-v1";
    host_config.provider_binding_identity =
        host_mode ? kHostImplementationDigest
                  : kProviderBindingIdentity;
    host_config.provider_host_configuration = host_mode ?
        neograph::json{{"executor", executor}, {"model", host_model.empty() ? "host default" : host_model},
                       {"mode", "local-read-only-cli"}} :
        neograph::json{{"base_url", provider_base_url},
                       {"model", provider_model}, {"provider", provider->get_name()}};
    host_config.snapshots.owner_scope = "neograph-harness-example";
    const neograph::program::ExecutableIdentity provider_identity{
        neograph::program::ExecutableKind::Provider, "harness.provider", "1.0.0",
        host_mode ? std::string(kHostImplementationDigest)
                  : std::string(kProviderImplementationDigest)};
    host_config.snapshots.registry.provider = neograph::mcp::HarnessProviderRegistration{
        {provider_identity, neograph::program::EffectMode::Brokered,
         host_mode ? "neograph-harness-local-host-worker" : "neograph-harness-example-provider",
         {}, {}, {}},
        {neograph::json{{"type", "object"}}, neograph::json{{"type", "object"}}}};
    host_config.snapshots.allowed_module_digests = {
        provider_identity.implementation_digest,
        kSupportModuleDigest,
        kToolingModuleDigest};
    host_config.snapshots.budget_ceiling = {
        86400000, 100000000, 100000000, 64, 10000, 1000, 1000,
        neograph::program::MAX_SUPPORTED_CHILD_DEPTH, 10000};
    host_config.checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    host_config.state_store = std::make_shared<neograph::graph::InMemoryStore>();
    harness_config.translation_defaults.provider = provider_identity;
    if (smoke_mode && !host_mode) {
        harness_config.translation_defaults.input_token_ceiling_per_round = kSmokeInputTokens;
        harness_config.translation_defaults.max_output_tokens = kSmokeOutputTokens;
    }
    if (host_mode) {
        host_config.worker_executor = neograph::mcp::make_host_agent_executor(agent_config);
    } else {
        neograph::mcp::HarnessProviderExecutorConfig executor_config;
        executor_config.provider = provider;
        executor_config.model = provider_model;
        if (adopted_mcp) {
            adopted_mcp->configure_harness(host_config, executor_config);
            harness_config.translation_defaults.read_only_effects =
                host_config.snapshots.allowed_effects;
        }
        host_config.worker_executor =
            neograph::mcp::make_provider_harness_executor(std::move(executor_config));
    }
#ifdef NEOGRAPH_HARNESS_HAVE_SQLITE
    if (const auto state_dir = environment("NEOGRAPH_HARNESS_STATE_DIR"); !state_dir.empty()) {
        std::filesystem::create_directories(state_dir);
        harness_config.record_store =
            std::make_shared<neograph::mcp::SqliteHarnessRecordStore>(state_dir + "/runs.db");
        host_config.checkpoints =
            std::make_shared<neograph::graph::SqliteCheckpointStore>(state_dir + "/checkpoints.db");
        harness_config.enable_experimental_tasks =
            environment("NEOGRAPH_HARNESS_EXPERIMENTAL_TASKS") == "1";
    }
#else
    if (!environment("NEOGRAPH_HARNESS_STATE_DIR").empty()) {
        std::cerr << "Durable Harness state requires NEOGRAPH_BUILD_SQLITE=ON\n";
        return 2;
    }
#endif
    auto harness_resources =
        neograph::mcp::make_harness_program_service_resources(host_config);
    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {
        {"name", "neograph-harness"},
        {"version", "0.3.0"},
    };
    server_config.instructions =
        "Compile a Harness request, start the retained artifact, poll compact "
        "status, then dereference result artifact URIs only when detail is needed.";

    const auto transport = environment("NEOGRAPH_HARNESS_TRANSPORT");
    if (transport.empty() || transport == "stdio") {
        neograph::mcp::HarnessService harness(std::move(harness_config), nullptr,
                                               std::move(harness_resources));
        neograph::mcp::MCPServer      server(std::move(server_config));
        harness.register_tools(server);
        server.run();
        return 0;
    }
    if (transport != "http") {
        std::cerr << "NEOGRAPH_HARNESS_TRANSPORT must be stdio or http\n";
        return 2;
    }

#ifdef NEOGRAPH_HARNESS_HAVE_HTTP
    neograph::mcp::MCPHttpServerConfig http_config;
    if (const auto host = environment("NEOGRAPH_HARNESS_HTTP_HOST"); !host.empty()) {
        http_config.host = host;
    }
    if (const auto port = environment("NEOGRAPH_HARNESS_HTTP_PORT"); !port.empty()) {
        try {
            http_config.port = std::stoi(port);
        } catch (const std::exception&) {
            std::cerr << "NEOGRAPH_HARNESS_HTTP_PORT must be an integer\n";
            return 2;
        }
    } else {
        http_config.port = 8080;
    }
    http_config.allowed_origins = split_csv(environment("NEOGRAPH_HARNESS_ALLOWED_ORIGINS"));
    if (const auto token = environment("NEOGRAPH_HARNESS_BEARER_TOKEN"); !token.empty()) {
        const auto expected = digest(token);
        http_config.bearer_authorizer =
            [expected](std::string_view candidate) -> std::optional<std::string> {
            return secure_equal(expected, candidate)
                       ? std::optional<std::string>("configured-bearer")
                       : std::nullopt;
        };
    }
    const auto listen_host = http_config.host;
    const auto listen_port = http_config.port;

    try {
        neograph::mcp::MCPHttpServer server(
            [harness_config, server_config, host_config](std::string_view owner_scope) {
                auto scoped_host = host_config;
                scoped_host.snapshots.owner_scope = std::string(owner_scope);
                auto harness = std::make_shared<neograph::mcp::HarnessService>(
                    harness_config, nullptr,
                    neograph::mcp::make_harness_program_service_resources(std::move(scoped_host)));
                auto session = std::make_unique<neograph::mcp::MCPServer>(server_config);
                harness->register_tools(*session);
                return neograph::mcp::MCPHttpServerSession{std::move(session), std::move(harness)};
            },
            std::move(http_config));
        std::cerr << "NeoGraph Harness MCP listening on http://" << listen_host << ':'
                  << listen_port << "/mcp\n";
        return server.start() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Cannot start Harness HTTP transport: " << error.what() << '\n';
        return 2;
    }
#else
    std::cerr << "HTTP transport requires NEOGRAPH_BUILD_MCP_HTTP_SERVER=ON\n";
    return 2;
#endif
}
