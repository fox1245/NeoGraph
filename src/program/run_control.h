#pragma once

#include <neograph/graph/engine.h>
#include <neograph/hook_runtime.h>
#include <neograph/program/event.h>
#include <neograph/program/core_tool_grant.h>
#include <neograph/program/core_provider_call.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/handle.h>
#include <neograph/program/journal.h>
#include <neograph/program/module.h>
#include <neograph/program/result.h>
#include <neograph/program/transition_store.h>
#include <neograph/program/task_graph_fragment.h>
#include "catalog_access.h"
#include "../core/host_timer.h"
#include "lock_audit.h"
#include "operation_lock.h"
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <asio/strand.hpp>
#include <asio/system_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program::detail {

// A held Core resumes the same attempt. Settle its wall time on the attempt's
// monotonic clock, not by adding separately rounded journal-clock intervals.
inline std::uint64_t remaining_attempt_wall_time(
    std::uint64_t grant, std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point sampled) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(sampled - started).count();
    if (elapsed <= 0) return grant;
    const auto spent = static_cast<std::uint64_t>(elapsed);
    return spent >= grant ? 0 : grant - spent;
}

// The system-clock deadline `remaining` after `system_now`. Subtracting a `system_now` before 1970 (a negative count)
// from `max()` overflows a signed 64-bit count, whereas adding a non-negative `system_clock::duration` to a time point
// at or before the epoch cannot overflow `max()`.
inline std::chrono::system_clock::time_point deadline_after(std::chrono::system_clock::time_point system_now,
                                                            std::chrono::steady_clock::duration   remaining) noexcept {
    using clock       = std::chrono::system_clock;
    const auto latest = clock::time_point::max();
    if (system_now >= latest) return latest;
    if (remaining <= std::chrono::steady_clock::duration::zero()) return system_now;
    const auto remaining_system = std::chrono::duration_cast<clock::duration>(remaining);
    if (system_now > clock::time_point{} && remaining_system >= latest - system_now) return latest;
    return system_now + remaining_system;
}

enum class CancellationCause : std::uint8_t {
    None,
    User,
    Timeout,
    RuntimeShutdown,
    EventSink,
    ParentTerminal,
};
class EventSinkError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RunOutcome {
    ProgramTerminalStatus                 status = ProgramTerminalStatus::Failed;
    json                                  output;
    ProgramUsage                          usage;
    RunBudget                             remaining_budget{};
    std::optional<CoreCheckpointIdentity> checkpoint;
    std::optional<ProgramInterrupt>       interrupt;
    std::optional<ProgramFailure>         failure;
    std::vector<std::string>              execution_trace;
    std::optional<UsageAccumulator::AuthoritySnapshot> provider_budget_authority;
    std::vector<ProgramMonetaryRecord>    monetary_records;
};

/**
 * The interrupted runs whose cancellation was accepted and is not recorded yet. Where ProgramHandle::cancel() cannot wait
 * for the transition store it schedules the Cancelled state of an interrupted run as a task of the executor, and keeps
 * a claim on the run until that task ends. There is one claim per run, shared by the runtimes that use one transition
 * store: a second call finds the claim and schedules nothing, and a resume of the run, which would compete with that
 * task for the same record, is refused while the claim stands.
 */
class PendingCancellations : public std::enable_shared_from_this<PendingCancellations> {
public:
    using Key = std::pair<std::string, std::string>;

    /// Held by the task that records the cancellation; releases the run when it ends, or when the task is destroyed
    /// without having run (the executor went away).
    class Claim {
    public:
        Claim() noexcept = default;
        Claim(Claim&& other) noexcept : set_(std::move(other.set_)), key_(std::move(other.key_)) {}
        Claim& operator=(Claim&& other) noexcept {
            if (this != &other) {
                release();
                set_ = std::move(other.set_);
                key_ = std::move(other.key_);
            }
            return *this;
        }
        Claim(const Claim&)            = delete;
        Claim& operator=(const Claim&) = delete;
        ~Claim() { release(); }

        explicit operator bool() const noexcept { return static_cast<bool>(set_); }
        void     release() noexcept {
            if (auto set = std::move(set_)) set->end(key_);
            set_.reset();
        }

    private:
        friend class PendingCancellations;
        Claim(std::shared_ptr<PendingCancellations> set, Key key) noexcept
            : set_(std::move(set)), key_(std::move(key)) {}
        std::shared_ptr<PendingCancellations> set_;
        Key                                   key_;
    };

    /// The claim on the run, or an empty claim when one stands already.
    Claim begin(std::string_view owner_scope, std::string_view run_id) {
        Key             key{std::string(owner_scope), std::string(run_id)};
        std::lock_guard lock(mutex_);
        if (!runs_.insert(key).second) return {};
        return Claim(shared_from_this(), std::move(key));
    }

    bool pending(std::string_view owner_scope, std::string_view run_id) const {
        std::lock_guard lock(mutex_);
        return runs_.count(Key{std::string(owner_scope), std::string(run_id)}) != 0;
    }

private:
    void end(const Key& key) noexcept {
        std::lock_guard lock(mutex_);
        runs_.erase(key);
    }

    mutable PureMutex mutex_;
    std::set<Key>     runs_;
};

/**
 * The executors and the blocking rules of one ProgramRuntime, shared by every RunControl it creates (#557). Without
 * RuntimeConfig::executor the three executors are the runtime's own pools (attempts on one, deadlines on another,
 * waiters on the system executor), as they always were. With it, all three are that executor.
 */
struct RuntimeScheduler {
    asio::any_io_executor attempts;
    asio::any_io_executor deadlines;
    asio::any_io_executor waiters;
    /// RuntimeConfig::executor was given.
    bool injected = false;
    /// The executor is pumped by a host's event loop, or the build has no threads to wait on.
    bool host_pumped = false;
    /// RuntimeConfig::executor as the host gave it; the three above are adapted from it.
    asio::any_io_executor host_executor;
    /// RuntimeConfig::executor_identity.running_in_this_thread; empty: found from host_executor when it is the
    /// executor of an io_context or a thread_pool, or a strand of either.
    std::function<bool()> running_in_this_thread;
    /// RuntimeConfig::javascript_stack_bytes, 0 resolved to the default: the stack of a JavaScript control source.
    std::size_t javascript_stack_bytes = 0;
    /// Every store the runtime reads is a Sync...Store, so its awaitable operations complete at once (the catalog's
    /// stores are not part of this answer: the catalog answers for itself).
    bool stores_inline = true;

    /// True when the calling thread runs the injected executor. Always false for the runtime's own pools.
    bool on_executor_thread() const;
    /// True when the calling thread runs a task of this runtime: a thread of the injected executor, or of the runtime's
    /// own attempt or deadline pool. A call that waits for a store must not be made there.
    bool runs_on_attempt_thread() const;
    /// Throws ProgramBlockingCallRefused when a synchronous call, which does store work on the calling thread and
    /// would hang a host loop, must not run here.
    void require_synchronous_call(std::string_view operation, std::string_view alternative) const;
    /// Throws ProgramBlockingCallRefused when a call that waits for a Program's work must not wait here: on a
    /// host-pumped executor, and on a thread that runs the injected executor.
    void require_wait(std::string_view operation, std::string_view alternative) const;

    /**
     * The synchronous API of the runtime: runs the coroutine body it shares with its awaitable form to completion on
     * the calling thread. Throws ProgramBlockingCallRefused, before the body starts, where waiting would hang the
     * thread: a host-pumped executor (require_synchronous_call), and, when a store is not synchronous, a thread
     * that runs the executor or a turn of a host's event loop. Stores that complete at once never wait.
     */
    template <typename R>
    R run_blocking(asio::awaitable<R> body, std::string_view operation, std::string_view alternative,
                   bool inline_stores) const {
        require_synchronous_call(operation, alternative);
        if (!inline_stores) {
            if (on_executor_thread())
                throw ProgramBlockingCallRefused(
                    std::string(operation), std::string(alternative),
                    "it is called on a thread that runs the runtime's executor");
            if (async::in_host_turn())
                throw ProgramBlockingCallRefused(
                    std::string(operation), std::string(alternative),
                    "it is called inside a turn of a host's event loop, which a blocked thread cannot turn");
        }
        return run_to_completion(std::move(body), operation);
    }
};


class RunControl;
// The last two arguments are a task-graph budget override and whether it is exact. An
// inexact override (schema-1 fragments) reads 0 as "the link's whole ceiling"; an exact one
// (schema-2 fragments) reads 0 as nothing, as RunBudget does (#412).
// The child callbacks read stores, so they are awaitable (#557) and take their arguments by value.
using ChildLaunchCallback =
    std::function<asio::awaitable<std::shared_ptr<RunControl>>(std::string, json, std::string, std::string,
                                                               std::optional<TaskGraphBudget>, bool)>;
using ChildBindingValidationCallback = std::function<asio::awaitable<void>(std::string, std::string)>;
enum class ChildRecoveryMode { None, Synthesis, Existing };
using ChildRecoveryCallback =
    std::function<asio::awaitable<ChildRecoveryMode>(std::string, json, std::string)>;
using ExistingChildReconnectCallback =
    std::function<asio::awaitable<std::shared_ptr<RunControl>>(std::string, json, std::string)>;

struct AsyncWaiter {
    std::weak_ptr<async::HostTimer> timer;
};

struct HeldProgramHandoff {
    std::uint64_t       request_id = 0;
    ExactProgramHandoff handoff;
};

struct RecordedRunReplay {
    std::string source_run_id;
    UsageAccumulator::AuthoritySnapshot provider_authority;
    std::map<std::string, std::deque<ProgramOperationEvent>> operations;
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
};

class RunControl final : public std::enable_shared_from_this<RunControl> {
private:
    static asio::awaitable<ProgramResult>
    wait_async_with_control(std::shared_ptr<const RunControl> control);

public:
    RunControl(std::string                                owner_scope,
               std::string                                run_id,
               std::uint64_t                              attempt,
               std::shared_ptr<const MaterializedProgram> materialized,
               std::string                                binding_fingerprint,
               ProgramPersistedInvocation                 execution_invocation,
               RunInvocation                              invocation,
               std::string                                core_thread_id,
               std::uint64_t                              event_sequence,
               std::shared_ptr<ProgramEventSink>          sink,
               std::shared_ptr<const RuntimeScheduler>    scheduler,
               std::shared_ptr<graph::CheckpointStore>    checkpoints,
               std::shared_ptr<graph::Store>              state_store,
               std::shared_ptr<ProgramTransitionStore>    transitions,
               std::optional<ProgramExecutionLease>       execution_lease = std::nullopt,
               std::string                                logical_run_id  = {});
    /// A control for a run read back from the store; `events` are the run's events, which the caller has loaded.
    RunControl(ProgramRunRecord record,
               std::vector<ProgramEvent> events,
               std::shared_ptr<ProgramTransitionStore> transitions,
               std::shared_ptr<sp::NativeArchive> archive,
               std::shared_ptr<const RuntimeScheduler> scheduler);

    /// Awaited by complete_async after the terminal state is published.
    using CompletionCallback = std::function<asio::awaitable<void>(ProgramResult)>;
    using TerminalCleanup = std::function<void()>;
    void set_completion_callback(CompletionCallback callback) noexcept;
    void set_child_launch_callback(ChildLaunchCallback callback) noexcept;
    /**
     * Registers per-attempt cleanup that runs before the terminal transition
     * is published.  Admission leases use this to free host capacity before a
     * completed handle can start the next Program.
     */
    void add_terminal_cleanup(TerminalCleanup cleanup);
    void                        add_logical_cleanup(TerminalCleanup cleanup);
    /// Hands the run's children, cleanups and completion callback to `successor`. `held` is the coordination lock when
    /// the caller holds it (see cancel_async).
    asio::awaitable<void>       retire_to(std::shared_ptr<RunControl> successor,
                                          std::shared_ptr<ReentrantOperationLock> held = {});
    std::shared_ptr<RunControl> following_successor() const;
    /**
     * Registers the host-owned policy/store boundary used by
     * expand_task_graph.  The callback is consulted only after the proposal
     * has been selected from the operation's typed source contract.
     */
    void set_task_graph_expansion_context(
        std::shared_ptr<TaskGraphFragmentStore> store,
        TaskGraphExpansionPolicyResolver        policy_resolver) noexcept;
    std::shared_ptr<TaskGraphFragmentStore> task_graph_fragments() const;
    std::optional<TaskGraphExpansionPolicy> task_graph_policy(
        std::string_view expansion_operation_id) const;
    void set_child_binding_validation_callback(ChildBindingValidationCallback callback) noexcept;
    void set_child_recovery_callback(ChildRecoveryCallback callback) noexcept;
    void set_existing_child_reconnect_callback(ExistingChildReconnectCallback callback) noexcept;
    asio::awaitable<std::shared_ptr<RunControl>> reconnect_existing_child(std::string binding,
                                                                          json        input,
                                                                          std::string operation) const;
    asio::awaitable<ChildRecoveryMode> can_recover_child(std::string binding,
                                                         json        input,
                                                         std::string operation) const;
    void set_hook_runtime(std::shared_ptr<HookRuntime> runtime) noexcept;
    /// The host's lifecycle runtime, which also emits the tool Hooks of the
    /// run's Core tool calls; null when the host configured none.
    std::shared_ptr<HookRuntime> hook_runtime() const;
    void set_core_tool_grant_resolver(ProgramCoreToolGrantResolver resolver);
    void set_core_tool_grant_awaitable_resolver(ProgramCoreToolGrantAwaitableResolver resolver);
    /// The grant for one Core operation. Awaits the host's store when the resolver is the awaitable one.
    asio::awaitable<std::optional<ProgramCoreToolGrant>> resolve_core_tool_grant(std::string operation_id) const;
    void set_core_provider_call_resolver(ProgramCoreProviderCallResolver resolver,
                                         bool required = false);
    std::shared_ptr<graph::ProviderCallBroker> resolve_core_provider_call_broker(
        std::string_view operation_id) const;
    /// The attempt's monetary account, passed to provider call resolvers.
    void set_monetary_account(std::shared_ptr<ProgramMonetaryAccount> account);
    /// The token account of an attempt settled without running again, passed
    /// to provider call resolvers (#381).
    void set_journaled_usage(std::shared_ptr<UsageAccumulator> usage);
    void emit_lifecycle_hook(HookPhase phase, const ProgramEvent& event,
                             std::optional<ProgramTerminalStatus> status = {},
                             bool observe_cancellation = true) const;

    const std::string                                owner_scope;
    const std::string                                run_id;
    const std::string                                logical_run_id;
    /// Set before dispatch from the immutable publication that created this generation.
    std::vector<ProgramChildRecord>                   inherited_children;
    const std::string                                program_version_id;
    const std::string                                bundle_id;
    const std::string                                binding_fingerprint;
    std::string                                       operation_id{"root"};
    const std::uint64_t                              attempt;
    const std::shared_ptr<const MaterializedProgram> materialized;
    const ProgramPersistedInvocation                 persisted_invocation;
    const RunInvocation                              invocation;
    const RunBudget                                  granted_budget;
    const std::chrono::steady_clock::time_point      started_at;
    const std::chrono::steady_clock::time_point      deadline;
    const std::shared_ptr<const RuntimeScheduler>    scheduler;
    const asio::any_io_executor                      deadline_executor;
    const asio::strand<asio::any_io_executor>        waiter_strand;
    const std::string                                core_thread_id;
    const std::string                                trace_id;
    const std::shared_ptr<graph::CheckpointStore>    checkpoints;
    const std::shared_ptr<graph::Store>              state_store;
    /// The runtime's transition store. Every operation is awaited (#557): nothing of the run waits for it.
    const std::shared_ptr<ProgramTransitionStore> transitions;
    ProgramTransitionStore& async_transitions() const { return *transitions; }
    const std::shared_ptr<graph::CancelToken>        cancel_token;
    const std::shared_ptr<std::atomic_bool>          budget_exhausted =
        std::make_shared<std::atomic_bool>(false);
    /// Installed only from an authenticated source publication, before execution.
    std::shared_ptr<RecordedRunReplay> recorded_replay;
    /// Runtime-owned custody installed before any Core execution starts.
    std::shared_ptr<sp::NativeArchive> native_history_archive;
    /// #404: set by reconnect on a typed plan that stopped before its first
    /// Core checkpoint. The attempt dispatches nothing: it returns the run for
    /// reconciliation of the plan operation that may have started, as a
    /// JavaScript callCore without a checkpoint is.
    bool recover_plan_without_checkpoint = false;
    /// #405: on the attempt a fork starts with, the source run whose provider
    /// calls the fork's operations repeat, so that a journaling broker replays
    /// them (ProgramCoreProviderCallContext::replay_source).
    struct ForkProviderReplay {
        std::string   run_id;
        std::string   program_version_id;
        std::string   generation_id;   ///< The source's compiled plan identity.
        std::string   root_thread_id;  ///< The source's root Core thread.
        /// The source's Core thread of the checkpoint the fork continues; a
        /// plan fork continues it on its own root thread.
        std::string   forked_thread_id;
        std::uint64_t max_attempt = 0; ///< The source's attempt at the fork.
    };
    std::optional<ForkProviderReplay> fork_provider_replay;
    /// #431: what ProgramHandle::notices() returns; set before the attempt starts.
    /// #543: a broker may add notices while the attempt runs (add_notice), so read
    /// them with notices_snapshot().
    std::vector<ProgramNotice> notices;
    void                       add_notice(ProgramNotice notice);
    std::vector<ProgramNotice> notices_snapshot() const;

    /// Cancels the run. Where the run is interrupted this publishes its Cancelled state, which is a store write: a
    /// coroutine awaits cancel_async; the synchronous cancel is for a thread that may wait, and schedules the write as a
    /// task of the executor where it may not (see the definition).
    /**
     * `held` is the process-wide coordination lock when the caller already holds it: a replacement, a migration or a
     * reconnect that cancels a run, or the cancellation of an ancestor. That lock is not reentrant for a coroutine, so
     * a nested cancellation must not take it again (and it passes it on to the children it cancels).
     * When a store call fails, the run is stopped in memory and the result is whatever that gives (false for an
     * interrupted run). `failure`, when given, receives the exception, which the result cannot carry.
     */
    asio::awaitable<bool> cancel_async(CancellationCause cause,
                                       std::shared_ptr<ReentrantOperationLock> held = {},
                                       std::exception_ptr* failure = nullptr);
    /// The synchronous cancellation: the result says whether it accepted the cancellation (see ProgramHandle::cancel).
    bool              cancel(CancellationCause cause) noexcept;
    /// Stops an executing run in memory at once and leaves the work that needs the store to a task of the executor.
    /// With `durable`, that includes the Cancelled state of an interrupted run, which the task writes. True when the
    /// call stopped an executing run, or scheduled that task.
    bool              cancel_without_waiting(CancellationCause cause, bool durable) noexcept;
    /// The runtimes of one transition store share these claims (see PendingCancellations); set once, when the control
    /// is registered.
    void              set_pending_cancellations(std::shared_ptr<PendingCancellations> pending);
    /// Answers, for a control, whether a later attempt of the same run executes in this process; set when the control
    /// is registered. A handle of an attempt that the run has left has nothing to cancel while that attempt executes.
    void              set_executing_attempt_check(std::function<bool(const RunControl*)> check);
    bool              superseded_by_executing_attempt() const noexcept;
    CancellationCause cancellation_cause() const noexcept;
    CancellationCause seal_terminal_cause() noexcept;
    /// Makes `child` one of this run's children. A child of a run that is already terminal or cancelling is cancelled
    /// at once, which can write its state to the store; `held` is the coordination lock when the caller holds it.
    asio::awaitable<void> attach_child(std::shared_ptr<RunControl> child,
                                       std::shared_ptr<ReentrantOperationLock> held = {});
    asio::awaitable<std::shared_ptr<RunControl>> launch_child(
        std::string binding_name,
        json        input,
        std::string operation_id,
        std::string execution_key,
        std::optional<TaskGraphBudget> budget_override = std::nullopt,
        bool                           exact_budget    = false) const;
    asio::awaitable<void> validate_child_binding(std::string binding_name, std::string operation_id) const;

    ProgramEvent stage_event(ProgramEventKind kind, ProgramEventPayload payload);
    ProgramEvent stage_event(std::string_view operation_id,
                             ProgramEventKind kind,
                             ProgramEventPayload payload);
    void         deliver_event(const ProgramEvent& event);
    void         emit(ProgramEventKind kind, ProgramEventPayload payload);
    void         emit(std::string_view operation_id,
                      ProgramEventKind kind,
                      ProgramEventPayload payload);
    /// Publishes the run's terminal state (awaiting the transition store) and delivers its result. Never throws.
    asio::awaitable<void> complete_async(RunOutcome outcome);
    /// Deliver an internal failure when terminalization itself throws.
    void         fail_closed(std::exception_ptr error) noexcept;
    /**
     * Atomically append one JavaScript yielded-command journal entry while
     * the attempt remains Running.  A terminal result may complete an
     * existing pending coordinate, and a paused join may record its settled
     * members against its pending coordinate, and an await its start (#391);
     * the store rejects every other duplicate. The pending entries of one
     * coordinate are cumulative: each carries the settled members and await
     * starts of the entries before it.
     */
    /** #395: reserves the resources of the command the attempt resumes, on a
     *  run whose first publication could hold no reservation (a fork). */
    asio::awaitable<ProgramTransitionPublishResult> publish_command_reservation(
        RunBudget remaining_budget, RunBudget inflight_reservation);
    asio::awaitable<ProgramTransitionPublishResult> publish_javascript_command(
        std::uint64_t              command_ordinal,
        JavaScriptCommand          command,
        std::optional<std::string> effect_identity,
        std::optional<json>        terminal_result,
        std::optional<RunBudget>   remaining_budget     = std::nullopt,
        std::optional<RunBudget>   inflight_reservation = std::nullopt,
        std::optional<json>        settled_members      = std::nullopt,
        std::optional<json>        await_started_at     = std::nullopt);
    /** Durably consumes one nonrenewable dynamic-compilation unit before dispatch. */
    asio::awaitable<bool> consume_dynamic_compile();
    std::atomic<std::uint64_t> dynamic_compile_ceiling{UINT64_MAX};
    void                       limit_dynamic_compiles(std::uint64_t remaining) noexcept {
        auto old = dynamic_compile_ceiling.load();
        while (old > remaining && !dynamic_compile_ceiling.compare_exchange_weak(old, remaining)) {}
    }
    ProgramEvent make_event(std::string_view operation_id,
                            ProgramEventKind kind,
                            ProgramEventPayload payload);
    ProgramEvent preview_event(std::uint64_t sequence,
                               std::string_view operation_id,
                               ProgramEventKind kind,
                               ProgramEventPayload payload) const;

    ProgramResult                         wait() const;
    asio::awaitable<ProgramResult>        wait_async() const;
    std::optional<ProgramResult>          try_result() const;
    std::vector<ProgramEvent>             events_after(std::uint64_t sequence) const;
    std::optional<CoreCheckpointIdentity> latest_checkpoint() const;
    /// What the paused plan of this attempt recorded for the attempt that
    /// resumes it (#384, #386); its Interrupted record carries them.
    void                                  set_settled_branches(json branches);
    std::optional<json>                   settled_branches() const;
    std::optional<ExactProgramHandoff>    latest_handoff() const;
    asio::awaitable<std::optional<ExactProgramHandoff>> latest_handoff_async() const;
    void set_handoff_coordination_mutex(
        std::shared_ptr<ReentrantOperationLock> coordination_mutex,
        std::function<bool(const RunControl*)> admission);
    /// The synchronous form takes the process-wide coordination lock by blocking the thread; a coroutine awaits it
    /// (request_handoff_async), because the lock may be held across a suspension of another coroutine.
    std::uint64_t                         request_handoff();
    asio::awaitable<std::uint64_t>        request_handoff_async();
    HeldProgramHandoff                    wait_handoff(std::uint64_t request_id);
    asio::awaitable<HeldProgramHandoff>   wait_handoff_async(std::uint64_t request_id);
    bool                                  has_active_handoff_request() const noexcept;
    bool                                          has_held_handoff() const noexcept;
    asio::awaitable<void>                 reach_latest_handoff_if_requested();
    std::function<asio::awaitable<void>(std::uint64_t)> checkpoint_handler;
    asio::awaitable<void>                 hold_latest_handoff_if_requested(std::uint64_t ordinal);
    void                                  release_handoff(std::uint64_t request_id) noexcept;
    std::shared_ptr<graph::GraphSafePointRequest> graph_safe_point_request() const;
    std::uint64_t request_graph_migration();
    asio::awaitable<std::uint64_t> request_graph_migration_async();
    GraphMigrationCapsule wait_graph_migration(std::uint64_t request_id);
    /// As wait_graph_migration, without blocking a thread (#557).
    asio::awaitable<GraphMigrationCapsule> wait_graph_migration_async(std::uint64_t request_id);
    asio::awaitable<void> hold_graph_migration(
        const graph::GraphSafePoint& safe_point,
        RunBudget remaining_budget);
    bool has_active_graph_migration_request() const noexcept;
    bool graph_migration_matches(std::uint64_t request_id,
                                  std::string_view capsule_id) const noexcept;
    std::optional<ProgramExecutionLease> execution_lease() const;
    void set_execution_lease(ProgramExecutionLease lease);
    void clear_execution_lease(std::string_view lease_id) const noexcept;
    void release_graph_migration(std::uint64_t request_id) noexcept;
    asio::awaitable<void> release_graph_migration_async(std::uint64_t request_id);
    ProgramRunRecord                      snapshot() const;
    asio::awaitable<ProgramRunRecord>     snapshot_async() const;
    void              cancel_children(CancellationCause cause) noexcept;
    void              cancel_children_in_background(CancellationCause cause) noexcept;
    bool              schedule_interrupted_cancellation(CancellationCause cause) noexcept;
    static asio::awaitable<void> record_scheduled_cancellation(std::shared_ptr<RunControl> self,
                                                               CancellationCause cause,
                                                               PendingCancellations::Claim claim);
    /// Adds the P_CANCELLATION_NOT_RECORDED notice, unless another path has ended the run since.
    void              report_unrecorded_cancellation(std::exception_ptr failure) noexcept;
    asio::awaitable<void> cancel_children_async(CancellationCause cause,
                                                std::shared_ptr<ReentrantOperationLock> held = {});

    ProgramResult make_result(RunOutcome outcome) const;
    ProgramEvent  make_event(ProgramEventKind kind, ProgramEventPayload payload);
    ProgramEvent  preview_event(std::uint64_t sequence,
                                ProgramEventKind kind,
                                ProgramEventPayload payload) const;
    void adopt_published_event(ProgramEvent event);

private:
    mutable PureMutex                     mutex_;
    /// Guards notices; a broker adds one from the thread of its call (#543).
    mutable PureMutex                     notices_mutex_;
    mutable PureConditionVariable        cv_;
    std::optional<ProgramResult>           result_;
    std::vector<ProgramEvent>              events_;
    std::optional<CoreCheckpointIdentity>  latest_checkpoint_;
    std::optional<json>                    settled_branches_;
    std::shared_ptr<ProgramEventSink>      sink_;
    CompletionCallback                     completion_callback_;
    mutable std::vector<AsyncWaiter>       waiters_;
    std::vector<TerminalCleanup>           terminal_cleanups_;
    std::vector<TerminalCleanup>                  logical_cleanups_;
    std::shared_ptr<RunControl>                   successor_;
    bool                                          follow_successor_ = false;
    ChildLaunchCallback                     child_launch_callback_;
    ChildRecoveryCallback                         child_recovery_callback_;
    ExistingChildReconnectCallback                existing_child_reconnect_callback_;
    ChildBindingValidationCallback           child_binding_validation_callback_;
    std::shared_ptr<HookRuntime>               hook_runtime_;
    ProgramCoreToolGrantResolver                core_tool_grant_resolver_;
    ProgramCoreToolGrantAwaitableResolver       core_tool_grant_awaitable_resolver_;
    ProgramCoreProviderCallResolver             core_provider_call_resolver_;
    bool                                        require_core_provider_call_broker_ = false;
    std::shared_ptr<ProgramMonetaryAccount>     monetary_account_;
    std::shared_ptr<UsageAccumulator>           journaled_usage_;
    std::atomic<bool>                          hook_runtime_enabled_{false};
    std::shared_ptr<TaskGraphFragmentStore>  task_graph_fragments_;
    TaskGraphExpansionPolicyResolver         task_graph_policy_resolver_;
    std::uint64_t                            next_sequence_ = 1;
    CancellationCause                        cancellation_cause_ = CancellationCause::None;
    bool                                     terminal_decided_ = false;
    bool                                     completion_claimed_ = false;
    std::vector<std::weak_ptr<RunControl>> children_;
    mutable PureConditionVariable             handoff_cv_;
    std::shared_ptr<ReentrantOperationLock>   handoff_coordination_mutex_;
    std::shared_ptr<PendingCancellations>     pending_cancellations_;
    std::function<bool(const RunControl*)>    executing_attempt_check_;
    std::function<bool(const RunControl*)>    handoff_admission_;
    std::uint64_t                            next_handoff_request_id_ = 1;
    std::uint64_t                            handoff_request_id_ = 0;
    std::optional<ExactProgramHandoff>       held_handoff_;
    std::weak_ptr<async::HostTimer>        handoff_release_waiter_;
    mutable std::vector<AsyncWaiter>         handoff_waiters_;
    mutable PureConditionVariable             graph_migration_cv_;
    mutable std::vector<AsyncWaiter>         graph_migration_waiters_;
    std::shared_ptr<graph::GraphSafePointRequest> graph_safe_point_request_;
    std::uint64_t                            next_graph_migration_request_id_ = 1;
    std::uint64_t                            graph_migration_request_id_ = 0;
    std::uint64_t                            finished_graph_migration_request_id_ = 0;
    std::optional<GraphMigrationCapsule>      held_graph_migration_;
    mutable std::optional<ProgramExecutionLease> execution_lease_;
    std::string                              graph_migration_error_;
    std::weak_ptr<async::HostTimer>         graph_migration_release_waiter_;

    /// Wakes the blocked thread and takes the coroutines that wait for a graph migration state change; the caller
    /// passes them to wake_async_waiters after it unlocks mutex_.
    void notify_graph_migration_locked(std::vector<AsyncWaiter>& async_waiters);
    void wake_async_waiters(std::vector<AsyncWaiter> waiters) const;
    void abort_handoff() noexcept;
    void abort_graph_migration() noexcept;
    asio::awaitable<void> abort_graph_migration_async();
    void renew_graph_safe_point_request_locked();
    /// What request_handoff and request_graph_migration do once the coordination lock is held.
    std::uint64_t arm_handoff(const std::function<bool(const RunControl*)>& admission);
    std::uint64_t arm_graph_migration(const std::function<bool(const RunControl*)>& admission);
};
struct CommandBudgetReservation {
    RunBudget remaining;
    RunBudget reservation;
};

CommandBudgetReservation reserve_command_resources(RunBudget available) noexcept;

/// The remaining budget that the journal of a JavaScript run holds beneath the reservation of a command, or nothing when
/// the journal holds no reservation (a Program without a JavaScript `main()` never reserves one).
///
/// The grant of an attempt that starts with a reservation held, as the attempt of a resume or of a recovery does,
/// includes that reservation. A run that ends before the command has settled it has no usage to settle it with: it must
/// not publish the grant as its remaining budget, which would return the reservation unmeasured, and the transition
/// validator refuses that. It ends with this remainder instead, and the reservation is not returned.
///
/// Awaits the transition store, and throws what the store throws.
asio::awaitable<std::optional<RunBudget>> held_command_remainder(const RunControl& control);

/// The effect identity of JavaScript command `command_ordinal` of a bundle, as
/// the command journal records it.
std::string javascript_command_effect_identity(std::string_view         bundle_id,
                                               std::uint64_t            command_ordinal,
                                               const JavaScriptCommand& command);

// A compare-and-publish of a run's record loses when another publication of the same run was written between the read of
// the head and the write: a child that is attached, dispatched or joined, or the command record of another member of a
// join. Each lost compare is such a publication, so a retry reads a newer head, and a run with several children in
// flight loses more than a few in a row (#614). The bound only stops a store that never accepts.
inline constexpr int kRunPublicationAttempts = 64;

/// Test seam: when set, called on the thread that launches each member of a
/// JavaScript join, right before the launch, with the member's index and
/// whether it replays a recorded result. A test may hold a launch to force the
/// order in which members settle. Null outside tests.
using JoinMemberLaunchObserver = void (*)(std::size_t member_index, bool replayed);
void set_join_member_launch_observer_for_testing(JoinMemberLaunchObserver observer) noexcept;

asio::awaitable<void> execute_run_attempt(
    std::shared_ptr<RunControl> control,
    json                        input,
    std::optional<std::string>  checkpoint_id,
    std::optional<json>         javascript_core_resume_value = std::nullopt);

}  // namespace neograph::program::detail
