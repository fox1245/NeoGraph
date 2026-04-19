/**
 * @file graph/deep_research_graph.h
 * @brief Deep Research agent — NeoGraph port of langchain-ai/open_deep_research.
 *
 * A supervisor/researcher pattern: a lead agent plans research topics,
 * fans out parallel researcher agents (each running its own ReAct loop
 * against web-search tools), compresses their findings, and synthesises
 * a final markdown report.
 *
 * Unlike LangGraph's Python implementation, this runs as a single
 * compiled C++ graph. The supervisor issues `ConductResearch` tool-calls
 * which the dispatcher node converts into parallel `Send`s to a
 * researcher node that embeds the LLM + tool loop inline.
 */
#pragma once

#include <neograph/graph/engine.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <memory>
#include <string>
#include <vector>

namespace neograph::graph {

/// @brief Configuration knobs for the Deep Research graph.
struct DeepResearchConfig {
    std::string model = "claude-sonnet-4-5";  ///< Claude model identifier.
    int max_supervisor_iterations = 2;        ///< Supervisor planning rounds (keep ≤ 3 for low-tier Anthropic quotas).
    int max_concurrent_researchers = 2;       ///< Cap per conduct_research fan-out batch. Default 2 so 3× parallel researchers don't collectively exceed the 30K-tokens-per-minute tier-1 Anthropic limit; raise on higher tiers.
    int max_researcher_iterations = 2;        ///< Inner LLM↔tools loop cap per researcher.

    /// When true, inserts a `human_review` node between `final_report`
    /// and `__end__`. The node throws NodeInterrupt on first execution
    /// so the engine saves a checkpoint and the caller can show the
    /// report to a human; on resume it inspects the latest user message
    /// in the `messages` channel and either:
    ///   * routes to `__end__` if the message is empty / "approve" / "ok"
    ///   * appends the message to `supervisor_messages` and routes back
    ///     to `supervisor` (with `supervisor_iterations` reset) so the
    ///     agent can address the feedback in another research round.
    /// Pairs naturally with PostgresCheckpointStore for cross-process
    /// resume — see examples/26_postgres_react_hitl.
    bool enable_human_review = false;
};

/**
 * @brief Build the Deep Research graph.
 *
 * Expected channels on the initial `RunConfig::input`:
 *   - `user_query` (string): the research question.
 *
 * On completion, `result.output["channels"]["final_report"]` holds the
 * markdown report.
 *
 * @param provider  LLM provider (SchemaProvider with the "claude" schema).
 * @param tools     Search/fetch tools made available to every researcher.
 *                  Typical set: `web_search`, `fetch_url`.
 * @param cfg       Iteration budgets and model id.
 */
std::unique_ptr<GraphEngine> create_deep_research_graph(
    std::shared_ptr<Provider>                provider,
    std::vector<std::unique_ptr<Tool>>       tools,
    DeepResearchConfig                       cfg = {});

} // namespace neograph::graph
