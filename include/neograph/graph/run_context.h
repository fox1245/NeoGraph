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

#include <chrono>
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

    /// Reserved absolute monotonic-clock deadline.
    std::optional<std::chrono::steady_clock::time_point> deadline;

    /// Reserved per-run trace correlator.
    std::string trace_id;

    /// Mirrors RunConfig::thread_id.
    std::string thread_id;

    /// Current super-step index.
    int step = 0;

    /// Mirrors RunConfig::stream_mode.
    StreamMode stream_mode = StreamMode::ALL;

    /// Value supplied to GraphEngine::resume(); empty on a fresh run.
    std::optional<json> resume_value;

    /// Optional cross-thread shared memory.
    std::shared_ptr<Store> store;

    /// Engine-wide tool policy, empty when no gate is configured.
    ToolGate tool_gate;
};

/// @brief Fold a completion's token usage into the run's running total.
inline void record_usage(const RunContext& ctx, const ChatCompletion& completion) {
    if (ctx.usage) ctx.usage->add(completion.usage);
}

}  // namespace neograph::graph
