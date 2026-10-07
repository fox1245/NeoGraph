#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/provider_call_broker.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::graph::detail {

struct SubgraphWriteJournal {
    std::vector<ChannelWrite> writes;
    std::string parent_call_id;
};

// Execution-only state deliberately lives outside the public RunContext ABI.
// A scope binds it to the context object while a node coroutine is active.
struct RunContextRuntime {
    std::shared_ptr<CheckpointStore> checkpoint_store;
    std::shared_ptr<SubgraphWriteJournal> subgraph_write_journal;
    /// Persisted when this graph directly owns an opt-in stateful child.
    std::string graph_invocation_id;
    std::shared_ptr<ProviderCallBroker> provider_call_broker;
    std::shared_ptr<ToolEffectBroker> tool_effect_broker;
    ToolEffectGrantIdentity tool_effect_grant;
    bool is_resume = false;
};

/// What a RunContext is bound to while a scope below is alive: the state the
/// whole run shares, plus the task id of the node invocation the context
/// belongs to (empty for the run's own context). It is owned by its scope
/// object, so registering a binding never allocates.
struct RuntimeBinding {
    std::shared_ptr<const RunContextRuntime> run;
    std::string invocation_id;
    /// Binding the same context had before this one; restored on exit.
    const RuntimeBinding* previous = nullptr;
};

/// Read-only handle to the binding of one RunContext. It stays valid while
/// the scope that installed the binding is alive, which covers every use from
/// inside the node (or run) the context was created for.
class RuntimeView {
public:
    RuntimeView() = default;
    explicit operator bool() const noexcept { return run_ != nullptr; }
    const RunContextRuntime* operator->() const noexcept { return run_; }
    std::string_view invocation_id() const noexcept { return invocation_id_; }

private:
    friend RuntimeView runtime_for(const RunContext& context);
    const RunContextRuntime* run_ = nullptr;
    std::string_view invocation_id_;
};

RuntimeView runtime_for(const RunContext& context);

void append_applied_writes(const RunContext& context,
                           const std::vector<ChannelWrite>& writes);

json checkpoint_metadata_for(const RunContext& context);
json managed_budget_scope_metadata(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
CheckpointPhase checkpoint_resume_phase(const Checkpoint& checkpoint);

void restore_subgraph_write_journal(
    const Checkpoint& checkpoint,
    const std::shared_ptr<SubgraphWriteJournal>& journal,
    const std::shared_ptr<sp::NativeArchive>& archive);

/// Binds a run's own context to the state the whole run shares.
class ScopedRunContextRuntime {
public:
    ScopedRunContextRuntime(
        const RunContext& context,
        std::shared_ptr<const RunContextRuntime> runtime);
    ~ScopedRunContextRuntime();

    ScopedRunContextRuntime(const ScopedRunContextRuntime&) = delete;
    ScopedRunContextRuntime& operator=(const ScopedRunContextRuntime&) = delete;

private:
    const RunContext* context_ = nullptr;
    RuntimeBinding binding_;
    bool installed_ = false;
};

/// Binds `context` (the RunContext one node invocation receives) to the
/// run-wide state `parent` is bound to, plus the task id of that invocation.
/// A context must be bound by at most one invocation scope at a time; if a
/// context is rebound, the scopes must end in reverse order.
class ScopedInvocationRuntime {
public:
    ScopedInvocationRuntime(const RunContext& parent, const RunContext& context,
                            std::string_view invocation_id);
    ~ScopedInvocationRuntime();

    ScopedInvocationRuntime(const ScopedInvocationRuntime&) = delete;
    ScopedInvocationRuntime& operator=(const ScopedInvocationRuntime&) = delete;

private:
    const RunContext* context_ = nullptr;
    RuntimeBinding binding_;
};

}  // namespace neograph::graph::detail
