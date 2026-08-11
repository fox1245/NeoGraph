#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/server.h>
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
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

class SmokeReviewProvider final : public neograph::Provider {
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion completion;
        completion.message.role    = "assistant";
        completion.message.content = R"({"status":"ok","findings":[]})";
        return completion;
    }

    std::string get_name() const override { return "harness-smoke-review"; }
};

}  // namespace

int main() {
    const bool smoke_mode = environment("NEOGRAPH_HARNESS_SMOKE") == "1";
    const auto api_key    = environment("NEOGRAPH_HARNESS_API_KEY", "OPENROUTER_API_KEY");
    if (!smoke_mode && api_key.empty()) {
        std::cerr << "Set NEOGRAPH_HARNESS_API_KEY or OPENROUTER_API_KEY\n";
        return 2;
    }

    neograph::llm::OpenAIProvider::Config provider_config;
    provider_config.api_key         = api_key;
    provider_config.base_url        = "https://openrouter.ai/api";
    provider_config.default_model   = "deepseek/deepseek-v4-flash-0731";
    provider_config.provider_routing = {{"zdr", true}};
    std::shared_ptr<neograph::Provider> provider;
    if (smoke_mode) {
        provider                      = std::make_shared<SmokeReviewProvider>();
        provider_config.default_model = "harness-smoke";
    } else {
        provider = neograph::llm::OpenAIProvider::create_shared(provider_config);
    }

    constexpr const char* kProviderBindingIdentity =
        "sha256:7df70a8b692b53148480c9eb019db87cac5c2c7e1c53a351ff628651ab219c14";
    constexpr const char* kProviderImplementationDigest =
        "sha256:67ac4b0f2d57b2ca169623008f2fad2ff4ed726e05eb570a36f69fa4edad7847";
    constexpr const char* kSupportModuleDigest =
        "sha256:a328e0824ca6438669e42ee0bb8c42634cc9613d66240cdd649c36b3b279030d";
    constexpr const char* kToolingModuleDigest =
        "sha256:aa8500648d5b30cecb1ef06dcf2eb861b126928de6715ff54bbe7857105b285f";

    neograph::mcp::HarnessProviderExecutorConfig executor_config;
    executor_config.provider = provider;
    executor_config.model    = provider_config.default_model;

    neograph::mcp::HarnessServiceConfig harness_config;
    neograph::mcp::HarnessProgramHostConfig host_config;
    host_config.worker_executor =
        neograph::mcp::make_provider_harness_executor(std::move(executor_config));
    host_config.compiler_build_id        = "neograph-harness-example-v1";
    host_config.provider_binding_identity = kProviderBindingIdentity;
    host_config.provider_host_configuration = {
        {"base_url", provider_config.base_url},
        {"model", provider_config.default_model},
        {"provider", provider->get_name()},
    };
    host_config.snapshots.owner_scope = "neograph-harness-example";
    const neograph::program::ExecutableIdentity provider_identity{
        neograph::program::ExecutableKind::Provider, "harness.provider", "1.0.0",
        std::string(kProviderImplementationDigest)};
    host_config.snapshots.registry.provider = neograph::mcp::HarnessProviderRegistration{
        {provider_identity, neograph::program::EffectMode::Brokered,
         "neograph-harness-example-provider", {}, {}, {}},
        {neograph::json{{"type", "object"}}, neograph::json{{"type", "object"}}}};
    host_config.snapshots.allowed_module_digests = {
        provider_identity.implementation_digest,
        kSupportModuleDigest,
        kToolingModuleDigest};
    host_config.snapshots.budget_ceiling = {86400000, 100000000, 100000000,
                                            64, 10000, 1000, 1000, 64, 10000};
    host_config.checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    host_config.state_store = std::make_shared<neograph::graph::InMemoryStore>();
    harness_config.translation_defaults.provider = provider_identity;
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
