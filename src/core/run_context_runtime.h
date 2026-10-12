#pragma once

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/provider_call_broker.h>
#include <neograph/graph/scheduler.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

/// #385: the state the current super-step started from, which a Send that
/// interrupts saves as its NodeInterrupt checkpoint so that resume re-enters
/// the whole step. The executor captures it, before it applies their writes,
/// only in a step whose nodes emitted Sends. Only the run's own coroutine
/// reads and writes it.
struct StepStartState {
    /// Whether to capture it: the run saves checkpoints.
    bool enabled = false;
    /// GraphState::serialize_runtime() at the start of the step; reset by the
    /// engine at every step.
    std::optional<json> state;
};

/// #385: the super-step a batch of Sends belongs to. A Send that interrupts
/// saves the step's ready nodes as the checkpoint's next nodes, with the
/// barrier state the step started from, and records the ready nodes' results
/// when no checkpoint precedes the step.
struct SendStep {
    const std::vector<std::string>& ready;
    const std::vector<NodeResult>&  results;
    const BarrierState&             barrier_state;
};

/// #388: the NodeInterrupts of the current super-step, recorded by the
/// executor, in ready order and then Send order, right before it saves the
/// step's NodeInterrupt checkpoint and rethrows the first one; the engine
/// reports them. Only the run's own coroutine reads and writes it.
struct StepInterrupts {
    std::vector<PendingInterrupt> list;
    /// #386: the NodeInterrupt checkpoint the executor saved for them, which
    /// the run reports; empty when it saved none. The thread's latest
    /// checkpoint in a store can be another run's.
    std::string checkpoint_id;
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
    /// #385: the state the current step started from; null outside a run.
    std::shared_ptr<StepStartState> step_start;
    /// #388: the NodeInterrupts of the current step; null outside a run.
    std::shared_ptr<StepInterrupts> step_interrupts;
    /// #388, #393, #396: the invocations of `answered_step`, the step a
    /// resume re-enters, that have an answer, by task id, with the value each
    /// receives (none when its answer carried none): the paused invocation of
    /// a NodeInterrupt checkpoint, or each gated node whose interrupt_before
    /// gate the step had answered. Every other invocation of the run, in that
    /// step or a later one, runs without a resume value. Empty: no invocation
    /// receives one (a resume from any other checkpoint). Values never sit on
    /// the run's own RunContext.
    std::map<std::string, std::optional<json>, std::less<>> answers;
    int                                                      answered_step = -1;
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

/// #388: RunResult::interrupt_value of a NodeInterrupt: its reason, its type
/// and, when it carries one, its payload.
json interrupt_value_of(const NodeInterrupt& interrupt);

/// #388: `metadata` with the pending interrupts of a NodeInterrupt checkpoint
/// under "_neograph"/"interrupts", when there are several. A checkpoint with
/// one keeps its bytes: its single interrupt is the one paused invocation.
/// With `listed` (#392: a pause that holds back a failure of its step), one is
/// listed too, because the paused invocations then include the failed one,
/// which is not an interrupt.
json with_pending_interrupts(json metadata, const std::vector<PendingInterrupt>& interrupts,
                             bool listed = false);

/// #388: the ids of the pending interrupts a NodeInterrupt checkpoint
/// recorded (with_pending_interrupts), or nothing when it recorded none.
std::optional<std::vector<std::string>> recorded_interrupt_ids(const json& metadata);

/// #388: the task ids of the invocations that a resume of a NodeInterrupt
/// checkpoint at `step` runs instead of replaying, in dispatch order: the
/// ready nodes without a pending write, or, once every ready node replays,
/// the Sends they emitted without one. Its first is the step's first paused
/// invocation. Defined with the executor, which dispatches them.
std::vector<std::string> paused_task_ids(int step, const std::vector<std::string>& ready,
                                         const std::unordered_map<std::string, NodeResult>& replay);

/// #393: the task id of static node `node` in super-step `step` ("s3:review"),
/// the id the executor dispatches it under. Defined with the executor.
std::string static_task_id(int step, const std::string& node);

/// #396: an answered interrupt_before gate of a step: the gated node's task id
/// and the value the resume that answered it supplied, if any.
struct GateAnswer {
    std::string         id;
    std::optional<json> value;
};

/// #396: `metadata` with the gates its step already had answered, under
/// "_neograph"/"answered_gates", when there are any. The Before checkpoint of
/// a step's next unanswered gate carries them, so that the step runs once
/// every gate of it is answered and each gated node receives its own answer.
json with_gate_answers(json metadata, const std::vector<GateAnswer>& answers);

/// #396: the answered gates a Before checkpoint carries (with_gate_answers).
std::vector<GateAnswer> recorded_gate_answers(const json& metadata);
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

/// Node attempts dispatched off the orchestrator (#364, #367, #368) that are
/// still running. An attempt that did not stop by its stop deadline outlives
/// its run, and it still runs one of its engine's nodes. The engine shares one
/// of these with its executor; when it is destroyed while such an attempt runs,
/// it hands its nodes (and the tools they use) over with adopt(), and the last
/// attempt to finish releases them. Nothing waits: a destructor that blocked
/// would deadlock a host whose node needs a lock the destroying thread holds
/// (the Python GIL, for one).
class AttemptQuiescence {
public:
    void enter() {
        std::lock_guard lock(mutex_);
        ++active_;
    }
    void leave() {
        std::vector<std::shared_ptr<void>> released;
        {
            std::lock_guard lock(mutex_);
            if (active_ > 0 && --active_ == 0) released.swap(remains_);
        }
        // Released here, outside the lock, on the last attempt's thread.
    }
    /// Keeps `remains` alive until no dispatched attempt is running.
    void adopt(std::shared_ptr<void> remains) {
        {
            std::lock_guard lock(mutex_);
            if (active_ > 0) {
                remains_.push_back(std::move(remains));
                return;
            }
        }
        // Nothing runs any more: released here.
    }
    std::size_t active() const {
        std::lock_guard lock(mutex_);
        return active_;
    }

private:
    mutable std::mutex                 mutex_;
    std::size_t                        active_ = 0;
    std::vector<std::shared_ptr<void>> remains_;
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
