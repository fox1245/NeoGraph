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

#include <iostream>

// Mock Provider
class SubgraphMockProvider : public neograph::Provider {
    int call_count_ = 0;
public:
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";

        if (call_count_++ == 0) {
            result.message.tool_calls = {{
                "call_sub_001", "lookup",
                R"({"query": "NeoGraph features"})"
            }};
        } else {
            result.message.content = "NeoGraph is a graph agent engine written in C++. "
                "It supports checkpointing, HITL, parallel execution, and subgraphs.";
        }
        return result;
    }

    neograph::ChatCompletion complete_stream(
        const neograph::CompletionParams& p, const neograph::StreamCallback& cb) override {
        auto r = complete(p);
        if (cb && !r.message.content.empty()) cb(r.message.content);
        return r;
    }
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

    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.tools = tool_ptrs;

    // JSON-based graph definition — subgraph included inline
    neograph::json definition = {
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
                // input_map/output_map omitted → auto-mapped by same-name channels
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "inner_agent"}},
            {{"from", "inner_agent"}, {"to", "__end__"}}
        })}
    };

    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));

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
