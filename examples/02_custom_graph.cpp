// NeoGraph Example 02: a ReAct graph from create_react_graph()
//
// One call builds the llm -> tools -> llm graph around a provider and a tool; run_stream()
// prints node events and streamed text. The provider is a scripted stand-in
// (scripted_provider.h): it asks the calculator once, then answers from the tool result, so
// no API key is needed. Examples 04, 06 and 07 write the graph definition as JSON instead.
//
// Usage: ./example_custom_graph
//
// smoke-expect: Final response: "The answer is 5."
// smoke-expect: Execution trace: llm -> tools -> llm -> END
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>
#include "scripted_provider.h"

#include <iostream>
#include <string>

// A calculator that really adds: "2 + 3" -> {"result":"5"}.
class CalculatorTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"calculator", "Add two integers written as 'a + b'",
                neograph::json{{"type", "object"},
                               {"properties", {{"expression", {{"type", "string"}}}}},
                               {"required", neograph::json::array({"expression"})}}};
    }
    std::string execute(const neograph::json& args) override {
        const std::string expression = args.value("expression", "");
        const auto        plus       = expression.find('+');
        if (plus == std::string::npos) return neograph::json{{"error", "expected 'a + b'"}}.dump();
        const int sum = std::stoi(expression.substr(0, plus)) + std::stoi(expression.substr(plus + 1));
        return neograph::json{{"result", std::to_string(sum)}}.dump();
    }
    std::string get_name() const override { return "calculator"; }
};

int main() {
    // 1. A provider and a tool. The script decides what the "model" says: ask the calculator
    //    first, then answer from the tool result.
    auto provider = std::make_shared<examples::ScriptedProvider>(
        [](const examples::ScriptedTurn& turn) {
            if (!turn.has_tool_result())
                return examples::call_tool("calculator", {{"expression", "2 + 3"}});
            const auto observation = neograph::json::parse(turn.last_tool_result());
            return examples::say("The answer is " + observation.value("result", std::string("?")) + ".");
        });
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<CalculatorTool>());

    // 2. One call builds the whole ReAct graph.
    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools), "You are a calculator assistant.", "scripted-model");

    // 3. Run it and watch the nodes.
    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "What is 2 + 3?"}}
    })}};

    std::cout << "Running graph...\n";
    auto result = engine->run_stream(config, [](const neograph::graph::GraphEvent& event) {
        using Type = neograph::graph::GraphEvent::Type;
        if (event.type == Type::NODE_START) std::cout << "[" << event.node_name << "] start\n";
        if (event.type == Type::NODE_END)   std::cout << "[" << event.node_name << "] end\n";
        if (event.type == Type::LLM_TOKEN)  std::cout << event.data.get<std::string>() << std::flush;
    });

    std::cout << "\n\nExecution trace: ";
    for (const auto& node : result.execution_trace) std::cout << node << " -> ";
    std::cout << "END\n";
    std::cout << "Final response: " << result.output["final_response"] << "\n";
    return 0;
}
