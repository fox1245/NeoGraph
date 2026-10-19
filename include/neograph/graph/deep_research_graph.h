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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neograph::graph {

/// @brief Configuration knobs for the Deep Research graph.
///
/// The graph names no model and sends no sampling of its own: `model` is required, and `controls` holds what every
/// step sends (only the members that are set). Leave `controls.temperature` unset for a model that refuses it
/// (gpt-5*, o3, Claude 4.7 and later). The iteration and concurrency defaults are small, to stay inside low
/// rate-limit tiers; raise them where the provider allows more.
struct DeepResearchConfig {
    std::string model;                        ///< The model every step calls. Required.
    int max_supervisor_iterations = 2;        ///< Supervisor planning rounds.
    int max_concurrent_researchers = 2;       ///< Cap per conduct_research fan-out batch.
    int max_researcher_iterations = 2;        ///< Inner LLM↔tools loop cap per researcher.

    /// Controls every step sends with its model call (temperature, reasoning effort, routing, ...), beside its
    /// output cap. Unset members are not sent. `controls.max_output_tokens` is ignored: the caps below apply.
    ProviderControls controls{};
    /// The output cap of each step. A supervisor, researcher, compression or report step that stops at its cap
    /// having written nothing asks again with twice the cap, up to `max_output_budget`.
    std::uint64_t brief_max_output_tokens      = 800;
    std::uint64_t clarify_max_output_tokens    = 200;
    std::uint64_t supervisor_max_output_tokens = 2048;
    std::uint64_t researcher_max_output_tokens = 2048;
    std::uint64_t compress_max_output_tokens   = 2048;
    std::uint64_t report_max_output_tokens     = 4096;
    std::uint64_t max_output_budget            = 16384;
    /// The node retry policy of every step that calls the model: by default twice, after 15 s and 30 s. It retries
    /// an ordinary failure of the step. The provider runtime retries a transient provider failure (a rate limit, an
    /// overload) itself, and a refused request (a wrong model id, a refused parameter) fails the run.
    RetryPolicy retry{2, 15'000, 2.0f, 45'000};

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

    /// When true, inserts a `clarify` node between `__start__` and
    /// `brief`. The node asks the LLM whether the user's query is
    /// specific enough to research; if not it throws NodeInterrupt
    /// with a clarifying question, expecting the caller to resume()
    /// with the user's answer. On resume the answer is appended to
    /// the user_query and the run proceeds to the brief node. This is
    /// the analogue of langchain-ai/open_deep_research's
    /// `clarify_with_user` step.
    bool enable_clarification = false;
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
 * @param provider  LLM provider; `cfg.model` names the model it calls.
 * @param tools     Search/fetch tools made available to every researcher.
 *                  Typical set: `web_search`, `fetch_url`.
 * @param cfg       Iteration budgets and model id.
 */
std::unique_ptr<GraphEngine> create_deep_research_graph(
    std::shared_ptr<Provider>                provider,
    std::vector<std::unique_ptr<Tool>>       tools,
    DeepResearchConfig                       cfg = {});

} // namespace neograph::graph
