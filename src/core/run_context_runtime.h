#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/provider_call_broker.h>

#include <memory>
#include <string>
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
    std::string invocation_id;
    /// Persisted when this graph directly owns an opt-in stateful child.
    std::string graph_invocation_id;
    std::shared_ptr<ProviderCallBroker> provider_call_broker;
    std::shared_ptr<ToolEffectBroker> tool_effect_broker;
    ToolEffectGrantIdentity tool_effect_grant;
    bool is_resume = false;
};

std::shared_ptr<const RunContextRuntime> runtime_for(const RunContext& context);

std::shared_ptr<const RunContextRuntime> runtime_for_invocation(
    const RunContext& context, const std::string& invocation_id);

void append_applied_writes(const RunContext& context,
                           const std::vector<ChannelWrite>& writes);

json checkpoint_metadata_for(const RunContext& context);
json managed_budget_scope_metadata(const std::shared_ptr<OwnedManagedBudgetLease>& lease);
CheckpointPhase checkpoint_resume_phase(const Checkpoint& checkpoint);

void restore_subgraph_write_journal(
    const Checkpoint& checkpoint,
    const std::shared_ptr<SubgraphWriteJournal>& journal,
    const std::shared_ptr<sp::NativeArchive>& archive);

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
    std::shared_ptr<const RunContextRuntime> runtime_;
    std::shared_ptr<const RunContextRuntime> previous_;
    bool installed_ = false;
};

}  // namespace neograph::graph::detail
