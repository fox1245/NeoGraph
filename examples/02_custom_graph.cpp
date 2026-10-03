// NeoGraph Example 02: Custom Graph Definition
//
// Demonstrates compiling and running a graph from JSON definition.
// Uses a mock provider for offline testing — no API key needed.
//
// Usage:
//   ./example_custom_graph

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>
#include "provider_example_support.h"
#include <json/json.h>

#include <iostream>
#include <string>

// Offline fixture responds to the actual ordered tool history, not call count.
class MockProvider : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const neograph::PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                bool calculated = false;
                for (const auto& message : *history)
                    for (const auto& part : message.parts)
                        if (const auto* tool = std::get_if<sp::ToolResult>(&part))
                            calculated |= tool->tool_use_id == "call_001";
                sp::Completion completion;
                sp::Message reply;
                if (calculated) {
                    reply.parts.emplace_back(sp::Text{"The answer is 5."});
                    completion.stop.kind = sp::StopKind::EndTurn;
                } else {
                    auto parsed = sp::json::parse(R"({"expression":"2 + 3"})");
                    reply.parts.emplace_back(sp::ToolCall{
                        "call_001", "calculator", sp::ToolCallKind::ClientExecuted,
                        std::make_shared<const sp::json::Document>(
                            std::get<sp::json::Document>(std::move(parsed)))});
                    completion.stop.kind = sp::StopKind::ToolUse;
                }
                completion.messages.push_back(std::move(reply));
                if (prepared.mode() == neograph::ProviderMode::Stream)
                    examples::emit_local_events(completion, observer);
                co_return std::make_shared<const sp::Outcome>(std::move(completion));
            });
    }
    std::string_view family() const noexcept override { return "openai.chat"; }
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
        provider, std::move(tools), "You are a calculator assistant.", "fixture-calculator");

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
