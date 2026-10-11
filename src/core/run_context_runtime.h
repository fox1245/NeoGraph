#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/provider_call_broker.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::graph {
class NodeRetryCeiling;
}

namespace neograph::graph::detail {

class FailureRunState;

/// How a run stopped, once that is known: a barrier stall's witness, or the
/// {node, code} of a failure exit; null until then. Every checkpoint the run
/// saves afterwards carries it (#383). Only the run's own coroutine reads and
/// writes it.
struct RunStatusMarks {
    json barrier_stall;
    json failure_exit;
};

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
    /// #364: the run-level retry ceiling, shared with subgraphs; may be null.
    std::shared_ptr<NodeRetryCeiling> node_retry_ceiling;
    /// #364: visit counters of this graph run; null without max_visits.
    std::shared_ptr<FailureRunState> failure_state;
    /// #383: how this graph run stopped, once known; null outside a run.
    std::shared_ptr<RunStatusMarks> status_marks;
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

/// When a checkpoint is saved. One saved during a super-step (a
/// NodeInterrupt) resumes that step; one saved between steps (a gate, a step
/// commit, a budget stop) resumes the next.
enum class CheckpointTiming { DuringStep, BetweenSteps };

/// The run metadata of a checkpoint, the same on every save path (#383): the
/// subgraph identity and write journal, how the run stopped once known, and
/// the max_visits counters a resume continues from (from the start of the
/// step during a step, as they are between steps).
json checkpoint_metadata_for(const RunContext& context, CheckpointTiming timing);
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

/// The mediated Tool effects of one node attempt (#364). The attempt's
/// ToolEffectBroker records every call it settles, including the calls of any
/// subgraph the attempt runs, so the retry loop knows whether the attempt
/// committed a non-idempotent effect or left one uncertain.
class AttemptEffectLog {
public:
    struct Entry {
        std::string thread_id;
        std::string task_id;
        std::uint64_t ordinal = 0;
        std::string tool;
        ToolTerminalStatus status = ToolTerminalStatus::Failed;
    };

    /// @param committed the call succeeded under a policy that does not rule
    ///        out a non-idempotent effect.
    /// @param uncertain the call's effect may or may not have happened.
    void record(Entry entry, bool committed, bool uncertain);

    bool committed() const noexcept { return committed_.load(std::memory_order_acquire); }
    bool uncertain() const noexcept { return uncertain_.load(std::memory_order_acquire); }
    /// Every recorded call, ordered by (thread, task, ordinal) so the order
    /// does not depend on which call of a batch settled first.
    std::vector<Entry> entries() const;

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::atomic<bool> committed_{false};
    std::atomic<bool> uncertain_{false};
};

/// The effect namespace of attempt `attempt` of the invocation `task_id`:
/// the task id itself for attempt 0, so existing journals keep their slots,
/// and `task_id + "#a" + attempt` after it.
std::string attempt_effect_namespace(std::string_view task_id, int attempt);

/// Binds `context` (the RunContext one node invocation receives) to the
/// run-wide state `parent` is bound to, plus the task id of that invocation.
/// A context must be bound by at most one invocation scope at a time; if a
/// context is rebound, the scopes must end in reverse order.
class ScopedInvocationRuntime {
public:
    /// With `record_effects`, and when the run has a ToolEffectBroker, the
    /// context sees that broker through an AttemptEffectLog recorder.
    ScopedInvocationRuntime(const RunContext& parent, const RunContext& context,
                            std::string_view invocation_id, bool record_effects = false);
    ~ScopedInvocationRuntime();

    ScopedInvocationRuntime(const ScopedInvocationRuntime&) = delete;
    ScopedInvocationRuntime& operator=(const ScopedInvocationRuntime&) = delete;

    /// The log of this attempt's mediated effects; null unless effects are
    /// recorded and the run has a ToolEffectBroker.
    const std::shared_ptr<AttemptEffectLog>& effects() const noexcept { return effects_; }

private:
    const RunContext* context_ = nullptr;
    RuntimeBinding binding_;
    std::shared_ptr<AttemptEffectLog> effects_;
};

}  // namespace neograph::graph::detail
