/**
 * @file program/runtime.h
 * @brief Execution service for admitted immutable Programs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/host_admission.h>
#include <neograph/hook_runtime.h>
#include <neograph/program/catalog.h>
#include <neograph/program/fork.h>
#include <neograph/program/handle.h>
#include <neograph/program/module.h>
#include <neograph/program/pending.h>
#include <neograph/program/replay.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/invocation.h>
#include <neograph/program/transition_store.h>

#include <asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

struct ProgramChildQuotaConfig {
    /// Zero disables the corresponding global limit.
    std::uint64_t max_active_children = 0;
    std::uint64_t max_pending_spawn_requests = 0;
    std::uint64_t max_active_children_per_owner = 0;
    std::uint64_t max_pending_spawn_requests_per_owner = 0;
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
    /// Attach a runtime-only event sink without extending the canonical request.
    ProgramHandle start(RunInvocation invocation, std::shared_ptr<ProgramEventSink> events);
    /**
     * Start a top-level Program using exact recorded capability bindings. The
     * canonical invocation selects the owner and admitted ProgramVersion; the
     * optional sink is runtime-only and is never persisted in the request.
     */
    ProgramHandle start_recorded(RunInvocation invocation,
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
    ProgramHandle start_recorded(std::string_view      owner_scope,
                                 const ProgramVersion& version,
                                 ProgramInvocation     invocation,
                                 RecordedBindingSet    recorded);
    /**
     * Fork an exact checkpoint into the ProgramVersion selected by the
     * canonical top-level invocation. The optional sink is runtime-only.
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
     * only input.handoff crosses the generation boundary.
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

private:
    ProgramHandle start_resolved(std::string_view      owner_scope,
                                 const ProgramVersion& version,
                                 ProgramInvocation     invocation);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
