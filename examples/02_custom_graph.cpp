// NeoGraph Example 02: Custom Graph Definition
//
// Demonstrates compiling and running a graph from JSON definition.
// Uses a mock provider for offline testing — no API key needed.
//
// Usage:
//   ./example_custom_graph

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>

#include <iostream>
#include <string>

// Mock provider: first call returns a tool call, second call returns final text
class MockProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams& /*params*/) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        if (call_count_++ == 0) {
            // First call: return a tool call
            result.message.content = "";
            result.message.tool_calls = {{
                "call_001", "calculator", R"({"expression": "2 + 3"})"
            }};
        } else {
            // Second call: return final answer
            result.message.content = "The answer is 5.";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& params,
        const neograph::StreamCallback& on_chunk) override {
        auto result = complete(params);
        if (on_chunk && !result.message.content.empty()) {
            on_chunk(result.message.content);
        }
        return result;
    }

    std::string get_name() const override { return "mock"; }
};

// Mock calculator tool
class MockCalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"calculator", "Evaluate math", neograph::json{{"type", "object"}}};
    }
    std::string execute(const neograph::json& args) override {
        return R"({"result": 5})";
    }
    std::string get_name() const override { return "calculator"; }
};

int main() {
    // 1. Create provider and tools
    auto provider = std::make_shared<MockProvider>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<MockCalculatorTool>());

    // 2. Create a ReAct graph (convenience function)
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools), "You are a calculator assistant.");

    // 3. Run with input
    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "What is 2 + 3?"}}
    })}};

    std::cout << "Running graph...\n";

    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            switch (event.type) {
                case neograph::graph::GraphEvent::Type::NODE_START:
                    std::cout << "[" << event.node_name << "] start\n";
                    break;
                case neograph::graph::GraphEvent::Type::NODE_END:
                    std::cout << "[" << event.node_name << "] end\n";
                    break;
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                    std::cout << event.data.get<std::string>() << std::flush;
                    break;
                default:
                    break;
            }
        });

    std::cout << "\n\nExecution trace: ";
    for (const auto& node : result.execution_trace) {
        std::cout << node << " -> ";
    }
    std::cout << "END\n";

    if (result.output.contains("final_response")) {
        std::cout << "Final response: " << result.output["final_response"] << "\n";
    }

    return 0;
}
