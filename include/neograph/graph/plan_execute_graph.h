/**
 * @file graph/plan_execute_graph.h
 * @brief Factory for the Plan-and-Execute agent pattern.
 *
 * Produces a graph with three phases:
 *
 *   __start__ → planner → [plan_empty? responder : executor]
 *                         executor → [plan_empty? responder : executor]
 *                         responder → __end__
 *
 * Phase roles:
 *   - planner:   Calls the LLM with @p planner_prompt. The model must
 *                reply with a JSON array of strings (one step per item);
 *                the node tolerates fenced ```json blocks and leading
 *                prose. The array is written to the `plan` channel.
 *   - executor:  Pops the next step off `plan`, runs an inner ReAct loop
 *                against that single step using @p executor_prompt and
 *                the shared tool set, and appends {step, result} to the
 *                `past_steps` channel.
 *   - responder: Composes a final answer from the original objective and
 *                the accumulated `past_steps`, using @p responder_prompt.
 *                The text lands on the `final_response` channel and is
 *                also appended to `messages` as an assistant turn.
 *
 * The factory registers its three custom node types and the `plan_empty`
 * condition the first time it is called (idempotent via std::call_once).
 */
#pragma once

#include <neograph/graph/engine.h>

namespace neograph::graph {

/**
 * @brief Build a Plan-and-Execute graph.
 *
 * @param provider           LLM provider shared by every phase.
 * @param tools              Tools the executor may invoke (ownership transferred).
 * @param planner_prompt     System prompt for the planner phase. Should instruct
 *                           the model to respond with a JSON array of steps.
 * @param executor_prompt    System prompt for the single-step executor.
 * @param responder_prompt   System prompt for the final synthesis phase.
 * @param model              Model name. Empty falls back to the provider default.
 * @param max_step_iterations Max tool-call iterations inside the executor per step
 *                           (safety bound on the inner ReAct loop).
 * @return A compiled GraphEngine.
 */
std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model = "",
    int max_step_iterations = 5);

} // namespace neograph::graph
