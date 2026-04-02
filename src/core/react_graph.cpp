#include <neograph/graph/react_graph.h>

namespace neograph::graph {

std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions,
    const std::string& model) {

    // JSON definition equivalent to the Agent::run() ReAct loop:
    //   __start__ -> llm -> (has_tool_calls ? tools : __end__)
    //                         tools -> llm  (loop back)
    json definition = {
        {"name", "react_agent"},
        {"channels", {
            {"messages", {{"type", "list"}, {"reducer", "append"}}}
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

    // Build NodeContext
    std::vector<Tool*> tool_ptrs;
    tool_ptrs.reserve(tools.size());
    for (auto& t : tools) {
        tool_ptrs.push_back(t.get());
    }

    NodeContext ctx;
    ctx.provider     = std::move(provider);
    ctx.tools        = std::move(tool_ptrs);
    ctx.model        = model;
    ctx.instructions = instructions;

    auto engine = GraphEngine::compile(definition, ctx);
    engine->own_tools(std::move(tools));
    return engine;
}

} // namespace neograph::graph
