// NeoGraph Example 33: OpenRouter Responses API transport toggle
//
// Same ReAct loop as example 13, but the provider is explicitly configured
// for OpenRouter's Responses-compatible endpoint. Toggle is a single Config
// field — no caller code changes.
//
// OpenRouter's Responses endpoint currently documents SSE. This example keeps
// the WebSocket flag visible and disabled so the active path is reproducible.
// The connection still exercises the same schema-driven agent path.
//
// Usage:
//   echo 'OPENROUTER_API_KEY=sk-or-...' > .env
//   ./example_openai_responses_ws
//
// Troubleshooting — if you get
//   "openai-responses ws: server closed before response.completed (close=1000)"
// the TLS handshake + Upgrade succeeded but the server sent no events
// before closing normally. Most likely causes, in order of frequency:
//   1. The OpenRouter account/model route does not expose the requested
//      transport yet; HTTP/SSE on the same key may still work.
//   2. The selected DeepSeek route does not support the requested transport.
//   3. A required provider header or capability has changed; validate the
//      HTTP Responses example first to isolate transport from account issues.
// Validate the same key/model with the HTTP example (13_openai_responses)
// first to isolate transport vs account issues.

#include <neograph/neograph.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/llm/agent.h>

#include <cppdotenv/dotenv.hpp>

#include <iostream>

class CalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {
            "calculator",
            "Evaluate a mathematical expression. Input: {\"expression\": \"2 + 3 * 4\"}",
            neograph::json{
                {"type", "object"},
                {"properties", {
                    {"expression", {{"type", "string"}, {"description", "Math expression to evaluate"}}}
                }},
                {"required", neograph::json::array({"expression"})}
            }
        };
    }

    std::string execute(const neograph::json& args) override {
        auto expr = args.value("expression", "");
        return R"({"result": 42, "expression": ")" + expr + "\"}";
    }

    std::string get_name() const override { return "calculator"; }
};

int main() {
    cppdotenv::auto_load_dotenv();

    try {
        const char* api_key = std::getenv("OPENROUTER_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENROUTER_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        neograph::llm::SchemaProvider::Config config;
        config.schema_path   = "openai_responses";
        config.api_key       = api_key;
        config.base_url_override = "https://openrouter.ai/api";
        config.default_model = "~deepseek/deepseek-v4-flash-latest";
        config.provider_routing = {{"zdr", true}};
        // OpenRouter Responses currently documents SSE, not WebSocket.
        config.use_websocket = false;
        auto provider = neograph::llm::SchemaProvider::create(config);

        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<CalculatorTool>());

        neograph::llm::Agent agent(
            std::move(provider),
            std::move(tools),
            "You are a helpful assistant with a calculator tool."
        );
        agent.set_tool_detection_timeout_seconds(180);

        std::vector<neograph::ChatMessage> messages;
        messages.push_back({"user", "What is 15 * 28 + 7?"});

        std::cout << "User: What is 15 * 28 + 7?\n";
        std::cout << "Assistant: " << std::flush;

        auto response = agent.run_stream(messages,
            [](const std::string& token) { std::cout << token << std::flush; });

        std::cout << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
}
