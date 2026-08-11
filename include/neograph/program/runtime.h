/**
 * @file program/runtime.h
 * @brief Execution service for admitted immutable Programs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/host_admission.h>
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
