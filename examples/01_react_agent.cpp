// NeoGraph Example 01: Simple ReAct Agent
//
// A minimal example that creates a ReAct agent with a calculator tool.
// The agent calls the LLM, detects tool calls, executes them, and loops.
//
// Usage:
//   OPENAI_API_KEY=sk-... ./example_react_agent

#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

#include <iostream>

// A simple calculator tool
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
        // Simple: just return a mock result for demonstration
        return R"({"result": 42, "expression": ")" + expr + "\"}";
    }

    std::string get_name() const override { return "calculator"; }
};

int main() {
    // 1. Create provider
    const char* api_key = std::getenv("OPENAI_API_KEY");
    if (!api_key) {
        std::cerr << "Set OPENAI_API_KEY environment variable\n";
        return 1;
    }

    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = api_key,
        .default_model = "gpt-4o-mini"
    });

    // 2. Create tools
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<CalculatorTool>());

    // 3. Create agent
    neograph::llm::Agent agent(
        std::move(provider),
        std::move(tools),
        "You are a helpful assistant with a calculator tool."
    );

    // 4. Run
    std::vector<neograph::ChatMessage> messages;
    messages.push_back({"user", "What is 15 * 28 + 7?"});

    std::cout << "User: What is 15 * 28 + 7?\n";
    std::cout << "Assistant: " << std::flush;

    auto response = agent.run_stream(messages,
        [](const std::string& token) { std::cout << token << std::flush; });

    std::cout << "\n";
    return 0;
}
