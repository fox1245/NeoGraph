// NeoGraph Example 33: OpenAI Responses API over WebSocket
//
// Same ReAct loop as example 13, but the streaming path runs over
// wss://api.openai.com/v1/responses instead of HTTP/SSE. Toggle is
// a single Config field — no caller code changes.
//
// Per OpenAI's docs (developers.openai.com/api/docs/guides/websocket-mode)
// this transport claims ~40% lower end-to-end latency on agentic
// rollouts with 20+ tool calls, because the connection-local response
// state cache avoids a full HTTP setup per turn.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_openai_responses_ws
//
// Troubleshooting — if you get
//   "openai-responses ws: server closed before response.completed (close=1000)"
// the TLS handshake + Upgrade succeeded but the server sent no events
// before closing normally. Most likely causes, in order of frequency:
//   1. Your API key / org doesn't have WebSocket-mode access yet (the
//      feature is gated; HTTP /v1/responses on the same key may still
//      work).
//   2. The selected model isn't WS-enabled. Try a frontier model the
//      docs example uses rather than gpt-4o-mini.
//   3. A required beta header has been added since this example
//      shipped; check the live docs and pass it via a future Config
//      field if so.
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
        const char* api_key = std::getenv("OPENAI_API_KEY");
        if (!api_key) {
            std::cerr << "Set OPENAI_API_KEY environment variable "
                         "(or put it in .env beside the binary)\n";
            return 1;
        }

        neograph::llm::SchemaProvider::Config config;
        config.schema_path   = "openai_responses";
        config.api_key       = api_key;
        config.default_model = "gpt-4o-mini";
        // The single line that swaps SSE for WebSocket transport.
        config.use_websocket = true;

        auto provider = neograph::llm::SchemaProvider::create(config);

        std::vector<std::unique_ptr<neograph::Tool>> tools;
        tools.push_back(std::make_unique<CalculatorTool>());

        neograph::llm::Agent agent(
            std::move(provider),
            std::move(tools),
            "You are a helpful assistant with a calculator tool."
        );

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
