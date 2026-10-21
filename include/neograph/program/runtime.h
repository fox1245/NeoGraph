/**
 * @file program/runtime.h
 * @brief Execution service for admitted immutable Programs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/configuration_failure.h>
#include <neograph/graph/store.h>
#include <neograph/host_admission.h>
#include <neograph/hook_runtime.h>
#include <neograph/program/catalog.h>
#include <neograph/program/core_tool_grant.h>
#include <neograph/program/core_provider_call.h>
#include <neograph/program/fork.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/handle.h>
#include <neograph/program/module.h>
#include <neograph/program/pending.h>
#include <neograph/program/replay.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/invocation.h>
#include <neograph/program/transition_store.h>

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {
struct ProgramInvocation {
    json                              input;
    RunBudget                         budget;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
    std::string                       requested_run_id;
    std::string                       parent_run_id;
    std::uint32_t                     child_depth = 0;
    /// Set only by the canonical RunInvocation entry point; never persisted as a projection.
    std::optional<RunInvocation>      canonical_request;
};

struct ProgramResume {
    json                              value;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
    std::string                       pending_id;
};

struct ProgramEffectResolution {
    std::string                       pending_id;
    ProgramEffectReconciliation       resolution = ProgramEffectReconciliation::Unknown;
    std::optional<json>                result;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
};

/** Already-admitted immutable target selected by the host for one live migration. */
struct ProgramGraphMigrationTarget {
    std::string                       target_program_version_id;
    std::string                       requested_run_id;
    std::shared_ptr<ProgramEventSink> events;
    /// Required whenever the target changes the pinned native Core identity.
    std::optional<GraphSemanticMigrationAdapter> semantic_adapter;
};

/**
 * Immutable data available when a host scheduler admits one Program attempt.
 *
 * The resolver receives this context exactly once per attempt.  The runtime
 * owns the resulting request's owner and operation identities, so a policy
 * cannot accidentally merge independent tenant attempts.
 */
struct ProgramHostAdmissionContext {
    std::string_view owner_scope;
    std::string_view program_version_id;
    std::string_view run_id;
    std::string_view operation_id;
    std::uint64_t    attempt = 0;
    RunBudget        granted_budget;
    std::uint32_t    child_depth = 0;
};

using ProgramHostAdmissionResolver =
    std::function<HostAdmissionRequest(const ProgramHostAdmissionContext&)>;

/** Exact durable runtime state that must be rebound before recovered work can dispatch. */
struct ProgramRuntimeRecoveryState {
    std::string                              owner_scope;
    std::string                              run_id;
    std::vector<ProgramContextPublication>   context_publications;
    std::vector<HookOutboxEntry>              hook_outbox_entries;
    std::vector<HookOutboxEntry>              inherited_hook_outbox_entries;
    std::optional<ProgramRuntimeStateTransferReceipt> transfer_receipt;
};

/**
 * Host-owned, idempotent recovery boundary. Implementations bind the latest
 * ContextEpoch to a run-local interposition controller and restore hook heads
 * into their durable journal. An inherited head preserves its source-run
 * provenance: an equal head or a valid durable descendant is accepted, while
 * an unrelated identity or head fails closed.
 */
using ProgramRuntimeRecoveryHandler =
    std::function<void(const ProgramRuntimeRecoveryState&)>;

using ProgramChildSynthesisGrantResolver = std::function<std::optional<ProgramChildSynthesisGrant>(
    std::string_view owner_scope, std::string_view parent_run_id, std::string_view grant_id)>;

struct ProgramChildQuotaConfig {
    /// Zero disables the corresponding global limit.
    std::uint64_t max_active_children = 0;
    std::uint64_t max_pending_spawn_requests = 0;
    std::uint64_t max_active_children_per_owner = 0;
    std::uint64_t max_pending_spawn_requests_per_owner = 0;
};

/**
 * What ProgramRuntime assumes about RuntimeConfig::executor (#557). Both members are detected for the executors the
 * runtime can see into, so a host that gives an io_context, a thread_pool, a strand of either, or the executor of an
 * async::HostLoop sets neither.
 */
struct ProgramExecutorIdentity {
    /**
     * A host's event loop runs the executor, one turn at a time, and no thread of the process can wait for work on
     * it. A synchronous call that waits for a Program is then refused instead of hanging. Detected for the executor
     * of an async::HostLoop (async::is_host_pumped); set it for another executor that a host pumps.
     */
    bool host_pumped = false;
    /**
     * True when the calling thread is one the executor runs handlers on. A call that waits for a Program, made from
     * such a thread, waits for work only that thread can do, so it is refused. Detected for an executor over an
     * io_context or a thread_pool; set it for any other executor that has threads.
     */
    std::function<bool()> running_in_this_thread;
};

/**
 * Thrown by the ProgramRuntime constructor in a build without threads (NEOGRAPH_THREADS=OFF) when
 * RuntimeConfig::executor is not set: the runtime's own pools need threads that this build does not have. It is also
 * a graph::ConfigurationFailure: running it again reproduces it.
 */
class NEOGRAPH_PROGRAM_API ProgramExecutorRequired final : public std::runtime_error,
                                                           public graph::ConfigurationFailure {
public:
    ProgramExecutorRequired();
};

/**
 * Thrown by a synchronous call that would wait for a Program on a thread that cannot wait for it: the executor of
 * RuntimeConfig::executor is pumped by a host's event loop, the build has no threads, or the call is made from a
 * thread that runs that executor. Nothing is started or changed. alternative() names the awaitable form to use. It is
 * also a graph::ConfigurationFailure: running it again reproduces it.
 */
class NEOGRAPH_PROGRAM_API ProgramBlockingCallRefused final : public std::runtime_error,
                                                              public graph::ConfigurationFailure {
public:
    ProgramBlockingCallRefused(std::string operation, std::string alternative, std::string reason);
    /// The refused call, for example "ProgramRuntime::start".
    const std::string& operation() const noexcept { return operation_; }
    /// The awaitable form that does the same without blocking, for example "ProgramRuntime::start_async".
    const std::string& alternative() const noexcept { return alternative_; }

private:
    std::string operation_;
    std::string alternative_;
};

 struct RuntimeConfig {
     std::shared_ptr<ProgramCatalog>         catalog;
     std::shared_ptr<graph::CheckpointStore> checkpoints;
     std::shared_ptr<graph::Store>           state_store;
     std::shared_ptr<ProgramTransitionStore> transitions;
     std::size_t                              scheduler_threads = 1;
     ProgramChildBindingResolver             child_binding_resolver;
     /**
      * Optional durable dynamic task-graph expansion boundary.  An absent
      * store or policy resolver makes expand_task_graph fail closed at runtime.
      */
     std::shared_ptr<TaskGraphFragmentStore> task_graph_fragments;
     TaskGraphExpansionPolicyResolver        task_graph_policy_resolver;
     /**
      * Optional shared host scheduler.  It becomes mandatory for every attempt
      * when configured; leaving either member unset preserves legacy direct
      * dispatch.
      */
     std::shared_ptr<HostAdmissionController> host_admission;
     ProgramHostAdmissionResolver             host_admission_resolver;
     /// Process-wide limits for admitted child publication and dispatch.
      ProgramChildQuotaConfig                  child_quota;
      /// Optional host-owned lifecycle observer. Programs never receive it.
      std::shared_ptr<HookRuntime>              hook_runtime;
      /// Required on reconnect when the run has durable context or hook state.
      ProgramRuntimeRecoveryHandler             runtime_recovery_handler;
      std::shared_ptr<ProgramSynthesisGateway>  child_synthesis_gateway;
      ProgramChildSynthesisGrantResolver        child_synthesis_grant_resolver;
      /// Host checkpoint consumer, called after durable publication or latest-checkpoint replay.
      /// Enqueue the move-only lease and return promptly; never block the scheduler here.
      /// Retaining the lease pauses this generation. Destroying it releases the generator.
      /// Explicit next_handoff requests take precedence. Programs cannot install this handler.
      std::function<void(ProgramHandle, ProgramHandoff)> checkpoint_handler;
      /// Opt in to reconnecting exact, already-published child runs when a
      /// recorded spawn/await command is unfinished after process loss. This
      /// never authorizes a new child or replay of an unknown external effect.
      bool recover_existing_child_commands = false;
      /// Rebound for each Program attempt and Core operation. Missing, stale or
      /// incomplete grants deny mediated tool calls before dispatch.
      ProgramCoreToolGrantResolver core_tool_grant_resolver;
      /// Optional host broker for each built-in Core provider call. When set,
      /// a missing or stale binding denies that operation before Core runs.
      ProgramCoreProviderCallResolver core_provider_call_resolver;
      /// Hosts with durable provider effects can forbid raw Core provider calls.
      /// A missing resolver or binding then fails the Core operation closed.
      bool require_core_provider_call_broker = false;
      /// True when core_provider_call_resolver returns brokers that journal
      /// every call durably, as SQLiteProgramProviderCallJournal::bind and
      /// bind_priced do, so a fork replays the provider calls of the paused
      /// invocation from that journal instead of sending them again. It
      /// changes nothing the runtime sends. When it is false, a fork whose
      /// paused attempt recorded provider calls in the paused operation
      /// reports P_FORK_PROVIDER_CALLS_NOT_JOURNALED in ProgramHandle::notices().
      bool core_provider_calls_journaled = false;
      /// Host-owned SDK custody for native provider histories and outcomes.
      /// Null never enables a portable/native fallback during durable restore.
      std::shared_ptr<sp::NativeArchive> native_history_archive;
      /// #543: a fork repeats the provider calls its paused invocation made, and
      /// a durable journal replays them (SQLiteProgramProviderCallJournal). When
      /// the journal has the source's call at the same position but can neither
      /// adopt it nor show that the call differs from it, the fork would send a
      /// call the provider has probably charged already. False, the default,
      /// refuses that call before it is sent (ProgramProviderForkResendRefusedError).
      /// True sends it, which the provider may charge again, and reports
      /// P_FORK_PROVIDER_CALL_RESENT in ProgramHandle::notices(). It does not
      /// override a source call that is the same call and whose outcome is
      /// unknown: that stays uncertain until it is reconciled.
      bool fork_may_resend_provider_calls = false;
      /**
       * #557: the executor every attempt, deadline timer and waiter of this runtime runs on; nothing runs inline,
       * and the runtime starts no thread of its own. Unset, a native build runs attempts on a pool of
       * `scheduler_threads` threads and deadlines on a pool of one, as before (the pools are one implementation of an
       * executor). It is required in a build without threads (NEOGRAPH_THREADS=OFF; see ProgramExecutorRequired),
       * where it is typically async::HostLoop::executor().
       *
       * A node attempt that needs its own worker, one with a timeout among them, runs on a strand of this executor
       * instead of the process-wide attempt pool, so a node body must not block the thread: a body that blocks holds
       * every other run that shares the executor.
       *
       * The runtime keeps the executor alive until its last attempt has finished. Destroying the runtime cancels its
       * runs and, on an injected executor, does not wait for them: keep running the executor until the handles are
       * terminal. On a host-pumped executor the synchronous calls (start, resume, fork, replace, migrate_graph, run
       * and the rest) and ProgramHandle::wait are refused with ProgramBlockingCallRefused; use the `_async` forms.
       * A hook_runtime blocks its caller and is refused on a host-pumped executor with the same error type.
       */
      asio::any_io_executor                     executor;
      ProgramExecutorIdentity                   executor_identity;
  };
/**
 * Experimental owner-scoped new-run selection. The activation is the exact
 * durable pointer observed at admission; the handle pins its admitted version
 * and does not consult that pointer again while executing.
 */
struct ProgramActiveRun {
    ProgramActivation activation;
    ProgramHandle handle;
};

class NEOGRAPH_PROGRAM_API ProgramRuntime {
public:
    explicit ProgramRuntime(RuntimeConfig config);
    ProgramRuntime(ProgramRuntime&& other) noexcept;
    ProgramRuntime& operator=(ProgramRuntime&& other) noexcept;
    ProgramRuntime(const ProgramRuntime&)            = delete;
    ProgramRuntime& operator=(const ProgramRuntime&) = delete;
    ~ProgramRuntime();

    /**
     * Start the owner-scoped transport-neutral invocation contract. The
     * runtime resolves its admitted ProgramVersion by identity and derives its
     * runtime-only projection internally.
     */
    ProgramHandle start(RunInvocation invocation);
    /**
     * Select the current admitted activation exactly once for a new top-level
     * run. program_version_id must be empty: an explicit version request uses
     * start() instead. A missing/collected or mismatched activation fails before
     * publishing a run. Later CAS activation cannot redirect the pinned run.
     */
    ProgramActiveRun start_active(RunInvocation invocation,
                                  std::shared_ptr<ProgramEventSink> events = {});
    /// Attach a runtime-only event sink without extending the canonical request.
    ProgramHandle start(RunInvocation invocation, std::shared_ptr<ProgramEventSink> events);

    /**
     * Replay a retained terminal source without issuing a new spending grant.
     * Original immutable invocation permissions are authenticated separately
     * from the source remainder, which is transferred by one durable CAS.
     * Captured operations/calls do not regain operation or model credit; new
     * replay CPU/Core work is charged against that transferred remainder.
     */
    ProgramHandle replay_recorded(std::string_view source_run_id,
                                  RunInvocation invocation,
                                  RecordedBindingSet recorded,
                                  std::shared_ptr<ProgramEventSink> events = {});

    ProgramHandle start(std::string_view      owner_scope,
                        const ProgramVersion& version,
                        ProgramInvocation     invocation);

    /**
     * Start an admitted child through an immutable ModuleLinkReceipt. The
     * parent-child attachment is durably published before the child dispatch;
     * retries use the persisted child identity and are idempotent.
     */
    ProgramHandle start_child(std::string_view         owner_scope,
                              const ProgramHandle&     parent,
                              const ModuleLinkReceipt& link,
                              const ProgramVersion&    version,
                              ProgramInvocation        invocation);
    /// Debit one successor compilation at this runtime's held checkpoint by lineage CAS.
    /// Persist intent before calling and record the outcome. The lease's journal head is
    /// refreshed after the debit; its checkpoint identity is retained. An old head fails,
    /// and an ambiguous acknowledgement never permits a free retry.
    ProgramSynthesisReservation reserve_synthesis(std::string_view                owner_scope,
                                                  const ProgramHandle&            source,
                                                  ProgramHandoff&                 handoff,
                                                  const ProgramSynthesisProposal& proposal,
                                                  std::string_view expected_lineage_head);
    /** Host-only synthesis at a held top-level checkpoint, bound to this exact run generation. */
    ProgramChildSynthesisRecord prepare_child_synthesis(std::string_view                owner_scope,
                                                        const ProgramHandle&            parent,
                                                        const ProgramHandoff&           handoff,
                                                        const ProgramSynthesisProposal& proposal,
                                                        const ProgramChildSynthesisGrant& grant,
                                                        std::string binding_name);
    /** Finish durable stages before reconnect; uncommitted compile/validation requires
     * reconciliation. */
    ProgramChildSynthesisRecord recover_child_synthesis(std::string_view owner_scope,
                                                        std::string_view parent_run_id,
                                                        std::string_view proposal_id);
    ProgramHandle reconnect(std::string_view owner_scope, std::string_view run_id);
    /**
     * Reconcile the durable child join records for a parent. Existing terminal
     * and in-flight child handles are returned; publishing records are resumed
     * through their persisted immutable receipt and invocation.
     */
    std::vector<ProgramHandle> recover_children(std::string_view owner_scope,
                                                std::string_view parent_run_id);
    ProgramHandle resume(std::string_view owner_scope,
                         std::string_view run_id,
                         ProgramResume   resume);
    ProgramHandle reconcile(std::string_view          owner_scope,
                            std::string_view          run_id,
                            ProgramEffectResolution  resolution);
    /**
     * Fork an exact checkpoint into the ProgramVersion selected by the
     * canonical top-level invocation. The optional sink is runtime-only.
     *
     * The invocation's budget is the fork's share of the source's remaining
     * budget. Wall time, tokens, money, operations, Core steps, dynamic
     * compiles and child budgets are amounts: the share is taken out of what
     * the source has left. max_concurrency is a limit: the fork carries its
     * own, at most the source's, and the source keeps its own. A share larger
     * than the remainder is refused with P_FORK_BUDGET, and one that would
     * leave the source without wall_time_ms or max_core_steps, which every
     * resume needs, with P_FORK_SOURCE_BUDGET. Neither changes the source.
     */
    ProgramHandle fork(ExactProgramCheckpointReference source,
                       RunInvocation                 invocation,
                       ProgramResume                 resume,
                       std::shared_ptr<ProgramEventSink> events = {});
    ProgramHandle fork(std::string_view                  owner_scope,
                       ExactProgramCheckpointReference   source,
                       const ProgramVersion&             target,
                       ProgramInvocation                 invocation,
                       ProgramResume                     resume);
    /**
     * Replace the active source generation at one exact completed ng.checkpoint.
     * The target is already admitted and starts fresh from invocation.input;
     * application state crosses through input.handoff. Nested agents retain
     * their logical parent, child relations, and nonrenewable budget. A live
     * family replacement requires this runtime's held checkpoint.
     */
    ProgramHandle replace(ExactProgramHandoffReference source,
                           RunInvocation                invocation,
                           std::shared_ptr<ProgramEventSink> events = {});
    /** Replace a source that is held by a host-owned checkpoint lease. */
    ProgramHandle replace(ProgramHandoff&&             source,
                          RunInvocation                 invocation,
                          std::shared_ptr<ProgramEventSink> events = {});
    ProgramHandle replace(std::string_view                owner_scope,
                           ExactProgramHandoffReference     source,
                           const ProgramVersion&            target,
                           ProgramInvocation                invocation);
    ProgramHandle replace(std::string_view      owner_scope,
                           ProgramHandoff&&       source,
                           const ProgramVersion&  target,
                           ProgramInvocation      invocation);
    /**
     * Stop one live native root Core call at its next durable super-step,
     * atomically publish an admitted successor generation, and exact-resume it.
     */
    ProgramHandle migrate_graph(const ProgramHandle&          source,
                                ProgramGraphMigrationTarget target);

    asio::awaitable<ProgramResult> run_async(std::string owner_scope,
                                             ProgramVersion version,
                                             ProgramInvocation invocation);
    asio::awaitable<ProgramResult> resume_async(std::string owner_scope,
                                                std::string run_id,
                                                ProgramResume resume);
    ProgramResult run(std::string_view      owner_scope,
                      const ProgramVersion& version,
                      ProgramInvocation     invocation);

    /**
     * Awaitable forms of the operations above (#557). Each one does what its synchronous namesake does, on this
     * runtime's executor, and completes on the executor of the coroutine that awaits it. They take their arguments by
     * value, so nothing they read can dangle across a suspension; the exceptions are the ProgramHandoff that
     * reserve_synthesis_async and prepare_child_synthesis_async refer to, which the caller keeps alive until the call
     * completes (reserve_synthesis_async refreshes its journal head), and the ProgramRuntime itself, which must
     * outlive the call as it does for the synchronous form.
     *
     * On a host-pumped executor the synchronous forms are refused (ProgramBlockingCallRefused). These are the way to
     * use the runtime there. resume_async returns the Program's result, as it always has; resume_handle_async is the
     * form that returns the handle, as `resume` does.
     */
    asio::awaitable<ProgramHandle> start_async(RunInvocation invocation,
                                               std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramActiveRun> start_active_async(RunInvocation invocation,
                                                         std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramHandle> start_async(std::string      owner_scope,
                                               ProgramVersion   version,
                                               ProgramInvocation invocation);
    asio::awaitable<ProgramHandle> replay_recorded_async(std::string                       source_run_id,
                                                         RunInvocation                     invocation,
                                                         RecordedBindingSet                recorded,
                                                         std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramHandle> start_child_async(std::string         owner_scope,
                                                     ProgramHandle       parent,
                                                     ModuleLinkReceipt   link,
                                                     ProgramVersion      version,
                                                     ProgramInvocation   invocation);
    asio::awaitable<ProgramSynthesisReservation> reserve_synthesis_async(
        std::string owner_scope, ProgramHandle source, ProgramHandoff& handoff,
        ProgramSynthesisProposal proposal, std::string expected_lineage_head);
    asio::awaitable<ProgramChildSynthesisRecord> prepare_child_synthesis_async(
        std::string owner_scope, ProgramHandle parent, const ProgramHandoff& handoff,
        ProgramSynthesisProposal proposal, ProgramChildSynthesisGrant grant, std::string binding_name);
    asio::awaitable<ProgramChildSynthesisRecord> recover_child_synthesis_async(
        std::string owner_scope, std::string parent_run_id, std::string proposal_id);
    asio::awaitable<ProgramHandle> reconnect_async(std::string owner_scope, std::string run_id);
    asio::awaitable<std::vector<ProgramHandle>> recover_children_async(std::string owner_scope,
                                                                        std::string parent_run_id);
    asio::awaitable<ProgramHandle> resume_handle_async(std::string owner_scope,
                                                       std::string run_id,
                                                       ProgramResume resume);
    asio::awaitable<ProgramHandle> reconcile_async(std::string owner_scope,
                                                   std::string run_id,
                                                   ProgramEffectResolution resolution);
    asio::awaitable<ProgramHandle> fork_async(ExactProgramCheckpointReference source,
                                              RunInvocation                 invocation,
                                              ProgramResume                 resume,
                                              std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramHandle> fork_async(std::string                     owner_scope,
                                              ExactProgramCheckpointReference source,
                                              ProgramVersion                  target,
                                              ProgramInvocation               invocation,
                                              ProgramResume                   resume);
    asio::awaitable<ProgramHandle> replace_async(ExactProgramHandoffReference source,
                                                 RunInvocation                invocation,
                                                 std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramHandle> replace_async(ProgramHandoff                 source,
                                                 RunInvocation                  invocation,
                                                 std::shared_ptr<ProgramEventSink> events = {});
    asio::awaitable<ProgramHandle> replace_async(std::string                    owner_scope,
                                                 ExactProgramHandoffReference   source,
                                                 ProgramVersion                 target,
                                                 ProgramInvocation              invocation);
    asio::awaitable<ProgramHandle> replace_async(std::string                    owner_scope,
                                                 ProgramHandoff                 source,
                                                 ProgramVersion                 target,
                                                 ProgramInvocation              invocation);
    asio::awaitable<ProgramHandle> migrate_graph_async(ProgramHandle              source,
                                                       ProgramGraphMigrationTarget target);

private:
    struct Impl;
    struct Borrowed {};
    /// A view of the same runtime for work that outlives a call: shares the state, never shuts it down.
    ProgramRuntime(std::shared_ptr<Impl> impl, Borrowed) noexcept;
    ProgramRuntime borrowed() const noexcept;

    asio::awaitable<ProgramHandle> migrate_graph_core(ProgramHandle source, ProgramGraphMigrationTarget target);
    ProgramHandle start_resolved(std::string_view      owner_scope,
                                 const ProgramVersion& version,
                                 ProgramInvocation     invocation,
                                 bool                  active_selection = false);

    std::shared_ptr<Impl> impl_;
    bool                  borrowed_ = false;
};

}  // namespace neograph::program
