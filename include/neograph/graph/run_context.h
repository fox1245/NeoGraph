/**
 * @file graph/run_context.h
 * @brief Per-run metadata exposed to graph nodes.
 */
#pragma once

#include <neograph/graph/cancel.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>
#include <neograph/tool_dispatch.h>
#include <neograph/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace neograph::graph {

/**
 * @brief Per-run dispatch metadata threaded through the engine and executor.
 *
 * GraphNode::run(NodeInput) consumes this through NodeInput::ctx. Workers that
 * need an isolated copy take it by value; the common path takes it by const
 * reference.
 */
struct RunContext {
    /// Operation-scoped cooperative cancellation handle.
    std::shared_ptr<CancelToken> cancel_token;

    /// Shared token accounting sink for this run and its subgraphs.
    std::shared_ptr<UsageAccumulator> usage;

    /// Program-supplied token ceiling and terminal signal. Zero/null leave generic Core runs unchanged.
    std::uint64_t                     model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    /// Durable Program run identity used by host-brokered journal effects.
    std::string run_id;
    /// Shared sibling-cancellation scope for budget-aware nodes.
    std::shared_ptr<CancelToken> budget_cancel_token;

    /// Absolute monotonic-clock deadline supplied through RunMetadata, when set.
    std::optional<std::chrono::steady_clock::time_point> deadline;

    /// Per-run trace correlator supplied through RunMetadata.
    std::string trace_id;

    /// Mirrors RunConfig::thread_id.
    std::string thread_id;

    /// Engine-assigned identity for this run or resume call. Used only for
    /// execution-local cache isolation; it is not derived from user data.
    std::uint64_t cache_execution_id = 0;

    /// Current super-step index.
    int step = 0;

    /// Mirrors RunConfig::stream_mode.
    StreamMode stream_mode = StreamMode::ALL;

    /// Value supplied to GraphEngine::resume(); empty on a fresh run.
    std::optional<json> resume_value;

    /// Optional cross-thread shared memory.
    std::shared_ptr<Store> store;

    /// Effective tool policy for this invocation. It includes any inherited
    /// parent policy and this engine's own policy, so a subgraph cannot weaken
    /// a parent gate by using a different GraphEngine instance.
    ToolGate tool_gate;

};

/// @brief Fold a completion's token usage into the run's running total.
inline void record_usage(const RunContext& ctx, const ChatCompletion& completion) {
    if (ctx.usage) ctx.usage->add(completion.usage);
}

}  // namespace neograph::graph
