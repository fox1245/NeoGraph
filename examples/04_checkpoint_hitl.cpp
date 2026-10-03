// NeoGraph Example 04: Checkpointing + Human-in-the-Loop (HITL)
//
// A HITL workflow example that interrupts execution before a specific node
// and resumes after user approval.
//
// Scenario: Order processing agent
//   1. LLM analyzes the order contents
//   2. Requests user approval before executing payment (interrupt_before)
//   3. Proceeds with payment after user approval
//
// No API key required (uses Mock Provider)
//
// Usage: ./example_checkpoint_hitl

#include <neograph/neograph.h>
#include <neograph/graph/react_graph.h>
#include "provider_example_support.h"
#include <json/json.h>

#include <iostream>
#include <string>

// Approval changes graph control flow, never the tool-call/result history.
class OrderProvider : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const neograph::PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                bool confirmed = false;
                for (const auto& message : *history)
                    for (const auto& part : message.parts)
                        if (const auto* tool = std::get_if<sp::ToolResult>(&part))
                            confirmed |= tool->tool_use_id == "call_001";
                sp::Completion completion;
                sp::Message reply;
                if (confirmed) {
                    reply.parts.emplace_back(sp::Text{
                        "Your order has been confirmed.\n"
                        "- Product: MacBook Pro\n- Quantity: 1\n"
                        "- Amount: 2,500,000 KRW\n"
                        "Payment has been completed. Thank you!"});
                    completion.stop.kind = sp::StopKind::EndTurn;
                } else {
                    auto parsed = sp::json::parse(
                        R"({"item":"MacBook Pro","quantity":1,"price":2500000})");
                    reply.parts.emplace_back(sp::ToolCall{
                        "call_001", "analyze_order", sp::ToolCallKind::ClientExecuted,
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
    std::string get_name() const override { return "order_mock"; }
};

// Order analysis tool
class AnalyzeOrderTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"analyze_order", "Analyze an order and return confirmation details",
                neograph::json{{"type", "object"}}};
    }
    std::string execute(const neograph::json& args) override {
        return R"({"status": "confirmed", "item": "MacBook Pro", "total": 2500000, "currency": "KRW"})";
    }
    std::string get_name() const override { return "analyze_order"; }
};

int main() {
    auto provider = std::make_shared<OrderProvider>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<AnalyzeOrderTool>());

    // Define graph via JSON — interrupt before the tools node
    neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "order_workflow"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}
        })},
        // Key: interrupt before tools execution
        {"interrupt_before", neograph::json::array({"tools"})}
    };

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "fixture-order";

    // Checkpoint store (in-memory)
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    neograph::graph::EngineResources resources;
    resources.tools = neograph::ToolSet(std::move(tools));
    auto engine     = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx, .checkpoint_store = store},
        std::move(resources));

    // === First run: up to interrupt ===
    std::cout << "=== Phase 1: Waiting for approval after order analysis ===\n\n";

    neograph::graph::RunConfig config;
    config.thread_id = "order-001";
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "Order 1 MacBook Pro"}}
    })}};

    auto result = engine->run(config);

    if (result.interrupted) {
        std::cout << "Interrupted! Node: " << result.interrupt_node << "\n";
        std::cout << "Checkpoint ID: " << result.checkpoint_id << "\n";
        std::cout << "Execution trace: ";
        for (const auto& n : result.execution_trace) std::cout << n << " → ";
        std::cout << "PAUSED\n\n";

        // Simulate requesting user approval
        std::cout << ">>> Proceed with payment? (simulation: approved) <<<\n\n";
    }

    // === Second run: resume after approval ===
    std::cout << "=== Phase 2: Resume after approval ===\n\n";

    // Null resume approves the paused node without inserting a user turn
    // between the pending assistant tool call and its result.
    auto resumed = engine->resume("order-001", neograph::json(nullptr));

    std::cout << "Execution trace: ";
    for (const auto& n : resumed.execution_trace) std::cout << n << " → ";
    std::cout << "END\n\n";

    if (resumed.output.contains("final_response")) {
        std::cout << "Final response:\n" << resumed.output["final_response"].get<std::string>() << "\n";
    }

    // Checkpoint history
    auto checkpoints = store->list("order-001");
    std::cout << "\n=== Checkpoint history (" << checkpoints.size() << " entries) ===\n";
    for (const auto& cp : checkpoints) {
        std::cout << "  [" << to_string(cp.interrupt_phase) << "] step=" << cp.step
                  << " node=" << cp.current_node << " → ";
        for (size_t i = 0; i < cp.next_nodes.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << cp.next_nodes[i];
        }
        std::cout << "\n";
    }

    return 0;
}
