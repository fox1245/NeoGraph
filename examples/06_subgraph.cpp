// NeoGraph Example 06: a subgraph inside a graph
//
// A subgraph node holds a whole graph of its own. The outer graph treats it as one node, so a
// workflow can be built from parts that are written and tested separately.
//
// Scenario: a supervisor delegates to a ReAct agent.
//   outer graph: __start__ -> inner_agent (subgraph) -> __end__
//   inner graph: llm -> tools -> llm (a ReAct loop with one lookup tool)
//
// The inner graph shares the `messages` channel with the outer one (identity mapping), so the
// question goes in and the answer comes out. The provider is scripted (scripted_provider.h): it
// calls the lookup tool once and then answers, so no API key is needed.
//
// Usage: ./example_subgraph
//
// smoke-expect: Execution trace (outer graph): inner_agent → END
// smoke-expect: Final response: NeoGraph is a graph agent engine written in C++.
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include "scripted_provider.h"

#include <iostream>

class LookupTool : public neograph::Tool {
public:
    neograph::ChatTool get_definition() const override {
        return {"lookup", "Look up information",
                neograph::json{{"type", "object"},
                               {"properties", {{"query", {{"type", "string"}}}}}}};
    }
    std::string execute(const neograph::json&) override {
        return R"({"result": "It supports checkpointing, HITL, parallel execution and subgraphs."})";
    }
    std::string get_name() const override { return "lookup"; }
};

int main() {
    // The "model": look the topic up, then answer using what the tool returned.
    auto provider = std::make_shared<examples::ScriptedProvider>(
        [](const examples::ScriptedTurn& turn) {
            if (!turn.has_tool_result())
                return examples::call_tool("lookup", {{"query", "NeoGraph features"}});
            const auto observation = neograph::json::parse(turn.last_tool_result());
            return examples::say("NeoGraph is a graph agent engine written in C++. " +
                                 observation.value("result", std::string("")));
        });

    neograph::graph::NodeContext ctx;
    ctx.provider = provider;
    ctx.model    = "scripted-model";

    // The inner graph is plain JSON inside the `definition` of a subgraph node.
    const neograph::json definition = {
        {"schema_version", neograph::graph::TOPOLOGY_SCHEMA_VERSION},
        {"name", "supervisor_graph"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"inner_agent", {
                {"type", "subgraph"},
                {"definition", {
                    {"name", "inner_react"},
                    {"channels", {{"messages", {{"reducer", "append"}}}}},
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
                // No input_map/output_map: identity mapping. The parent's values seed the child
                // once; only what the child writes comes back, in order.
            }}
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", "inner_agent"}},
            {{"from", "inner_agent"}, {"to", "__end__"}}
        })}
    };

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<LookupTool>());
    neograph::graph::EngineResources resources;
    resources.tools = neograph::ToolSet(std::move(tools));
    auto engine     = neograph::graph::GraphEngine::build(
        definition, neograph::graph::EngineConfig{.node_context = ctx}, std::move(resources));

    neograph::graph::RunConfig config;
    config.input = {{"messages", neograph::json::array({
        {{"role", "user"}, {"content", "What is NeoGraph?"}}
    })}};

    std::cout << "=== Subgraph (supervisor pattern) ===\n";
    auto result = engine->run_stream(config, [](const neograph::graph::GraphEvent& event) {
        using Type = neograph::graph::GraphEvent::Type;
        if (event.type == Type::NODE_START) std::cout << "[start] " << event.node_name << "\n";
        if (event.type == Type::NODE_END)   std::cout << "[done]  " << event.node_name << "\n";
        if (event.type == Type::LLM_TOKEN)  std::cout << event.data.get<std::string>() << std::flush;
    });

    std::cout << "\n\nExecution trace (outer graph): ";
    for (const auto& node : result.execution_trace) std::cout << node << " → ";
    std::cout << "END\n";
    std::cout << "\nFinal response: " << result.output["final_response"].get<std::string>() << "\n";
    return 0;
}
