// NeoGraph Example 33: OpenAI Responses API over WebSocket
//
// Direct one-request smoke test for OpenAI's WebSocket transport at
// wss://api.openai.com/v1/responses. It bypasses Agent::run_stream() so the
// first and only model request is the WebSocket response.create exchange.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_openai_responses_ws

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>
#include <string>

int main() {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        const std::string model = "gpt-4o-mini";

        neograph::llm::SchemaProvider::Config config;
        config.schema_path = "openai_responses";
        config.api_key = api_key;
        config.base_url_override = "https://api.openai.com";
        config.default_model = model;
        config.timeout_seconds = 60;
        config.use_websocket = true;
        auto provider = neograph::llm::SchemaProvider::create(config);

        neograph::CompletionParams params;
        params.model = model;
        params.messages.push_back({"user", "Reply with exactly: WebSocket OK"});
        params.max_tokens = 32;
        params.timeout_seconds = 60;

        std::cout << "User: Reply with exactly: WebSocket OK\n";
        std::cout << "Assistant: " << std::flush;

        std::string streamed;
        auto completion = provider->complete_stream(
            params,
            [&](const std::string& token) {
                streamed += token;
                std::cout << token << std::flush;
            });

        std::cout << "\n";
        if (streamed.empty() || completion.message.content != streamed) {
            std::cerr << "OpenAI Responses WebSocket stream was incomplete\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
