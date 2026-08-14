// NeoGraph Example 13: OpenRouter Responses API over SSE
//
// Minimal one-request SSE smoke test for OpenRouter's `/api/v1/responses`
// endpoint through the schema-driven SchemaProvider. This intentionally calls
// SchemaProvider::complete_stream() directly instead of Agent::run_stream(),
// whose first request is a non-streaming tool-detection pass. Every token
// printed here therefore comes from the Responses SSE path.
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_openrouter_responses_sse
// (auto-loads .env from the cwd or any parent directory.)

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <string>

int main() {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        const std::string model = "~deepseek/deepseek-v4-flash-latest";
        const std::string prompt =
            "In two short sentences, explain why streaming LLM responses "
            "improves perceived latency.";

        neograph::llm::SchemaProvider::Config config;
        config.schema_path = "openai_responses";
        config.api_key = api_key;
        config.base_url_override = "https://openrouter.ai/api";
        config.default_model = model;
        config.timeout_seconds = 180;
        config.provider_routing = {{"zdr", true}};
        auto provider = neograph::llm::SchemaProvider::create(config);

        neograph::CompletionParams params;
        params.model = model;
        params.messages.push_back({"user", prompt});
        params.temperature = 0.0f;
        params.max_tokens = 512;
        params.timeout_seconds = 180;

        std::cout << "User: " << prompt << "\n";
        std::cout << "Assistant: " << std::flush;

        provider->complete_stream(
            params,
            [](const std::string& token) { std::cout << token << std::flush; });

        std::cout << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
