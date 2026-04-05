/**
 * @file graph/react_graph.h
 * @brief Convenience factory for creating standard ReAct agent graphs.
 */
#pragma once

#include <neograph/graph/engine.h>

namespace neograph::graph {

/**
 * @brief Create a standard ReAct (Reasoning + Acting) 2-node graph.
 *
 * Builds a graph with an LLM call node and a tool dispatch node wired
 * in a loop: llm_call -> tool_dispatch -> llm_call -> ... -> __end__.
 * Equivalent to Agent::run() but as a composable graph engine.
 *
 * @param provider LLM provider for making completions.
 * @param tools Vector of tools available to the agent (ownership transferred).
 * @param instructions Optional system prompt / instructions for the LLM.
 * @param model Optional model name override (empty = use provider default).
 * @return A compiled GraphEngine ready for execution.
 */
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");

} // namespace neograph::graph
