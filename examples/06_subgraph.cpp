// NeoGraph Example 06: Subgraph (Hierarchical Graph Composition)
//
// An example of composing complex workflows hierarchically using subgraph nodes.
// Agents can be composed purely via JSON without any code changes.
//
// Scenario: Supervisor pattern
//   Main graph: supervisor → inner_react_agent (subgraph) → __end__
//   Subgraph  : llm → tools → llm (ReAct loop)
//
// No API key required (uses Mock Provider)
//
// Usage: ./example_subgraph

#include <neograph/neograph.h>
#include "provider_example_support.h"
#include <json/json.h>

#include <iostream>

// Offline fixture retains and inspects the child's complete tool history.
class SubgraphMockProvider : public neograph::Provider {
    std::shared_ptr<sp::runtime::Client> client_ = examples::make_local_client();
public:
    neograph::PreparedProviderRequest prepare(neograph::ProviderRequest request) override {
        auto history = std::make_shared<const std::vector<sp::Message>>(
            examples::request_messages(request));
        return prepare_local(client_, std::move(request),
            [history](const neograph::PreparedProviderRequest& prepared,
                      const std::function<void(const sp::Event&)>& observer)
                -> asio::awaitable<sp::runtime::Result> {
                bool looked_up = false;
                for (const auto& message : *history)
                    for (const auto& part : message.parts)
                        if (const auto* tool = std::get_if<sp::ToolResult>(&part))
                            looked_up |= tool->tool_use_id == "call_sub_001";
                sp::Completion completion;
                sp::Message reply;
                if (looked_up) {
                    reply.parts.emplace_back(sp::Text{
                        "NeoGraph is a graph agent engine written in C++. "
                        "It supports checkpointing, HITL, parallel execution, and subgraphs."});
                    completion.stop.kind = sp::StopKind::EndTurn;
                } else {
                    auto parsed = sp::json::parse(R"({"query":"NeoGraph features"})");
                    reply.parts.emplace_back(sp::ToolCall{
                        "call_sub_001", "lookup", sp::ToolCallKind::ClientExecuted,
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
    std::string get_name() const override { return "subgraph_mock"; }
};

// Mock tool
class LookupTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"lookup", "Look up information", neograph::json{{"type", "object"}}};
    }
    std::string execute(const neograph::json&) override {
        return R"({"result": "NeoGraph: C++ graph agent engine with checkpointing, HITL, parallel fan-out, subgraph composition."})";
    }
    std::string get_name() const override { return "lookup"; }
};

int main() {
    auto provider = std::make_shared<SubgraphMockProvider>();

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<LookupTool>());

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model = "fixture-subgraph";

    // JSON-based graph definition — subgraph included inline
    neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "supervisor_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            // Subgraph node: contains a ReAct loop internally
            {"inner_agent", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "inner_react"},
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
                    })}
                }}
                // input_map/output_map omitted -> identity mapping. Parent values
                // seed the child once; only child-produced ChannelWrite deltas
                // return, in order and with each write mode preserved.
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "inner_agent"}},
            {{"from", "inner_agent"}, {"to", "__end__"}}
        })}
    };

    neograph::graph::EngineResources resources;
    resources.tools = neograph::ToolSet(std::move(tools));
    auto engine     = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx}, std::move(resources));

    // Execute
    std::cout << "=== Subgraph (Supervisor Pattern) ===\n\n";

    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "What is NeoGraph?"}}
    })}};

    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            switch (event.type) {
                case neograph::graph::GraphEvent::Type::NODE_START:
                    std::cout << "[start] " << event.node_name << "\n";
                    break;
                case neograph::graph::GraphEvent::Type::NODE_END:
                    std::cout << "[done]  " << event.node_name << "\n";
                    break;
                case neograph::graph::GraphEvent::Type::LLM_TOKEN:
                    std::cout << event.data.get<std::string>() << std::flush;
                    break;
                default:
                    break;
            }
        });

    std::cout << "\n\nExecution trace (outer graph): ";
    for (const auto& n : result.execution_trace) std::cout << n << " → ";
    std::cout << "END\n";

    if (result.output.contains("final_response")) {
        std::cout << "\nFinal response: " << result.output["final_response"].get<std::string>() << "\n";
    }

    return 0;
}
