// NeoGraph Example 13: OpenAI Responses API via SchemaProvider
//
// Same ReAct loop as example 01, but wired to the OpenAI /v1/responses
// endpoint through the schema-driven SchemaProvider. Demonstrates that a
// completely different OpenAI API (input[] instead of messages[], flat
// function_call items, output[] response, SSE event streaming) is supported
// by swapping one built-in schema name — no provider subclass required.
//
// Usage:
//   echo 'OPENAI_API_KEY=sk-...' > .env
//   ./example_openai_responses
// (auto-loads .env from the cwd or any parent directory.)

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

    // SchemaProvider with the built-in "openai_responses" schema.
    // This routes every request through /v1/responses and parses the
    // output[] array response format.
    neograph::llm::SchemaProvider::Config config;
    config.schema_path = "openai_responses";
    config.api_key = api_key;
    config.default_model = "gpt-4o-mini";
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
