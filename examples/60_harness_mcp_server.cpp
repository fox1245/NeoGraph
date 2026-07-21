#include <neograph/llm/openai_provider.h>
#include <neograph/mcp/harness.h>
#include <neograph/mcp/server.h>
#ifdef NEOGRAPH_HARNESS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#endif

#include <cstdlib>
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

class SmokeReviewProvider final : public neograph::Provider {
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion completion;
        completion.message.role = "assistant";
        completion.message.content = R"({"status":"ok","findings":[]})";
        return completion;
    }

    std::string get_name() const override { return "harness-smoke-review"; }
};

} // namespace

int main() {
    const bool smoke_mode = environment("NEOGRAPH_HARNESS_SMOKE") == "1";
    const auto api_key = environment("NEOGRAPH_HARNESS_API_KEY", "OPENAI_API_KEY");
    if (!smoke_mode && api_key.empty()) {
        std::cerr << "Set NEOGRAPH_HARNESS_API_KEY or OPENAI_API_KEY\n";
        return 2;
    }

    neograph::llm::OpenAIProvider::Config provider_config;
    provider_config.api_key = api_key;
    if (const auto base_url = environment("NEOGRAPH_HARNESS_BASE_URL"); !base_url.empty()) {
        provider_config.base_url = base_url;
    }
    if (const auto model = environment("NEOGRAPH_HARNESS_MODEL"); !model.empty()) {
        provider_config.default_model = model;
    }
    std::shared_ptr<neograph::Provider> provider;
    if (smoke_mode) {
        provider = std::make_shared<SmokeReviewProvider>();
        provider_config.default_model = "harness-smoke";
    } else {
        provider = neograph::llm::OpenAIProvider::create_shared(provider_config);
    }

    neograph::mcp::HarnessProviderExecutorConfig executor_config;
    executor_config.provider = std::move(provider);
    executor_config.model = provider_config.default_model;
    neograph::mcp::HarnessServiceConfig harness_config;
    harness_config.worker_executor =
        neograph::mcp::make_provider_harness_executor(std::move(executor_config));
#ifdef NEOGRAPH_HARNESS_HAVE_SQLITE
    if (const auto state_dir = environment("NEOGRAPH_HARNESS_STATE_DIR"); !state_dir.empty()) {
        harness_config.record_store =
            std::make_shared<neograph::mcp::FileHarnessRecordStore>(state_dir);
        harness_config.checkpoint_store =
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
    neograph::mcp::HarnessService harness(std::move(harness_config));

    neograph::mcp::MCPServerConfig server_config;
    server_config.server_info = {
        {"name", "neograph-harness"},
        {"version", "0.3.0"},
    };
    server_config.instructions =
        "Compile a Harness request, start the retained artifact, poll compact "
        "status, then dereference result artifact URIs only when detail is needed.";
    neograph::mcp::MCPServer server(std::move(server_config));
    harness.register_tools(server);
    server.run();
    return 0;
}
