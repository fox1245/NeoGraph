/**
 * @file graph/run_context.h
 * @brief Per-run metadata exposed to graph nodes.
 */
#pragma once

#include <neograph/graph/cancel.h>
#include <neograph/graph/store.h>
#include <neograph/graph/types.h>
#include <neograph/graph/provider_call_broker.h>
#include <neograph/tool_dispatch.h>
#include <neograph/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
namespace sp { class NativeArchive; }

namespace neograph::graph {

struct ProviderOutcomes {
    mutable std::mutex mutex;
    std::vector<sp::runtime::Result> values;
    void add(sp::runtime::Result value) {
        std::lock_guard lock(mutex);
        values.push_back(std::move(value));
    }
    std::vector<sp::runtime::Result> snapshot() const {
        std::lock_guard lock(mutex);
        return values;
    }
};

/// Engine-owned inner-loop continuations, keyed by the compiled task slot.
/// Full messages and nonrenewable turn counts survive checkpoints and Sends.
struct ProviderLoopHistory {
    struct Entry {
        std::vector<sp::Message> messages;
        std::uint64_t turns = 0;
        bool client_calls_ready = false;
    };
    mutable std::mutex mutex;
    std::map<std::string, Entry> entries;
    Entry get(const std::string& task) const {
        std::lock_guard lock(mutex);
        const auto found = entries.find(task);
        return found == entries.end() ? Entry{} : found->second;
    }
    void set(const std::string& task, Entry entry) {
        std::lock_guard lock(mutex);
        entries.insert_or_assign(task, std::move(entry));
    }
    std::map<std::string, Entry> snapshot() const {
        std::lock_guard lock(mutex);
        return entries;
    }
};

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
    std::shared_ptr<ProviderOutcomes> provider_outcomes;
    std::shared_ptr<ProviderLoopHistory> provider_loop_history;
    std::function<void(const sp::Event&)> on_provider_event;
    std::shared_ptr<sp::NativeArchive> native_history_archive;

    /// Program-supplied token ceiling and terminal signal. Zero/null leave generic Core runs unchanged.
    std::uint64_t                     model_token_budget = 0;
    std::shared_ptr<std::atomic_bool> budget_exhausted;
    /// Durable Program run identity used by host-brokered journal effects.
    std::string run_id;
    /// Shared sibling-cancellation scope for budget-aware nodes.
    std::shared_ptr<CancelToken> budget_cancel_token;
    /// Host-only standalone currency; never recovered from caller JSON.
    std::shared_ptr<OwnedManagedBudgetLease> managed_budget_lease;
    std::shared_ptr<CheckpointStore> managed_budget_store;

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

    /// Shared host admission boundary for every tool call in this run.
    std::shared_ptr<ToolExecutionController> tool_execution_controller;

    /// Stable host/Program identity copied to each dispatched tool call.
    ToolExecutionIdentity tool_execution_identity;

};

/**
 * Populate the mediated Tool execution context for a node invocation.
 * Host-registered nodes that call dispatch_tool_calls must use this helper to
 * inherit the invocation's gate-adjacent effect broker and stable task slot.
 * Direct Tool::execute remains outside this boundary.
 */
NEOGRAPH_API ToolExecutionContext make_tool_execution_context(const RunContext& ctx);
NEOGRAPH_API std::shared_ptr<ProviderCallBroker> provider_call_broker(const RunContext& ctx);
inline ProviderCallIdentity make_provider_call_identity(const RunContext& ctx,
    const std::string& node, std::uint64_t ordinal = 0) {
    ProviderCallIdentity identity;
    identity.owner_scope = ctx.tool_execution_identity.owner_scope;
    identity.run_id = ctx.run_id;
    identity.thread_id = ctx.thread_id;
    identity.task_id = make_tool_execution_context(ctx).effect_task_id;
    identity.node_name = node;
    identity.call_ordinal = ordinal;
    identity.usage = ctx.usage;
    identity.model_token_budget = ctx.model_token_budget;
    identity.budget_exhausted = ctx.budget_exhausted;
    identity.budget_cancel_token = ctx.budget_cancel_token;
    identity.managed_budget_lease = ctx.managed_budget_lease;
    identity.managed_budget_store = ctx.managed_budget_store;
    return identity;
}

/// Fold exactly the actual nullable report, never a reservation or charge.
inline void record_usage(const RunContext& ctx, const sp::runtime::Result& result) {
    if (!result) throw std::runtime_error("Provider returned no outcome");
    if (ctx.usage && ctx.model_token_budget == 0 && !provider_call_broker(ctx))
        ctx.usage->add(outcome_usage(*result));
    if (ctx.provider_outcomes) ctx.provider_outcomes->add(result);
}

/// Owned failures must not erase the real outcome or its nullable usage.
inline asio::awaitable<sp::runtime::Result> observe_provider_result(
    const RunContext& ctx, asio::awaitable<sp::runtime::Result> operation) {
    try {
        co_return co_await std::move(operation);
    } catch (const ProviderOutcomeError& error) {
        record_usage(ctx, error.outcome());
        throw;
    } catch (const ProviderFailure& error) {
        record_usage(ctx, error.outcome());
        throw;
    }
}

}  // namespace neograph::graph
