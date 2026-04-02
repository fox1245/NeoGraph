#pragma once

#include <neograph/graph/engine.h>

namespace neograph::graph {

// Convenience: creates a standard ReAct 2-node graph (llm -> tools -> loop).
// Equivalent to Agent::run() but as a graph engine.
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");

} // namespace neograph::graph
