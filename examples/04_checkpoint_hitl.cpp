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

#include <iostream>
#include <string>

// Mock Provider: returns different responses per step
class OrderProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        if (call_count_++ == 0) {
            // Step 1: tool call (order analysis)
            result.message.tool_calls = {{
                "call_001", "analyze_order",
                R"({"item": "MacBook Pro", "quantity": 1, "price": 2500000})"
            }};
        } else {
            // Step 2: final response
            result.message.content =
                "Your order has been confirmed.\n"
                "- Product: MacBook Pro\n"
                "- Quantity: 1\n"
                "- Amount: 2,500,000 KRW\n"
                "Payment has been completed. Thank you!";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }

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

    // Prepare tool pointers
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;

    // Checkpoint store (in-memory)
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx, store);
    engine->own_tools(std::move(tools));

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

    auto resumed = engine->resume(
        "order-001",
        neograph::json("Approved")
    );

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
        std::cout << "  [" << cp.interrupt_phase << "] step=" << cp.step
                  << " node=" << cp.current_node << " → " << cp.next_node << "\n";
    }

    return 0;
}
