#include <neograph/graph/react_graph.h>

namespace neograph::graph {

std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions,
    const std::string& model, ProviderControls controls) {

    // JSON definition equivalent to the Agent::run() ReAct loop:
    //   __start__ -> llm -> (has_tool_calls ? tools : __end__)
    //                         tools -> llm  (loop back)
    json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "react_agent"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"llm",   {{"type", "llm_call"}}},
            {"tools", {{"type", "tool_dispatch"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "llm"}, {"type", "conditional"},
             {"condition", "has_tool_calls"},
             {"routes", {{"true", "tools"}, {"false", "__end__"}}}},
            {{"from", "tools"}, {"to", "llm"}}
        })}
    };

    // Share one owned collection between context, compiled graph and engine.

    NodeContext ctx;
    ctx.provider     = std::move(provider);
    ctx.tools        = ToolSet(std::move(tools));
    ctx.model        = model;
    ctx.instructions = instructions;
    ctx.provider_controls = std::move(controls);

    auto engine = GraphEngine::compile(definition, ctx);
    return engine;
}

} // namespace neograph::graph
