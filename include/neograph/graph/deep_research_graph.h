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
    int max_concurrent_researchers = 3;       ///< Cap per conduct_research fan-out batch.
    int max_researcher_iterations = 3;        ///< Inner LLM↔tools loop cap per researcher.
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
