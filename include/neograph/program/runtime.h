/**
 * @file program/runtime.h
 * @brief Execution service for admitted immutable Programs.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/store.h>
#include <neograph/program/catalog.h>
#include <neograph/program/fork.h>
#include <neograph/program/handle.h>
#include <neograph/program/module.h>
#include <neograph/program/pending.h>
#include <neograph/program/replay.h>
#include <neograph/program/transition_store.h>

#include <asio/awaitable.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {
struct ProgramInvocation {
    json                              input;
    RunBudget                         budget;
    std::string                       trace_id;
    std::shared_ptr<ProgramEventSink> events;
    std::string                       requested_run_id;
    std::string                       parent_run_id;
    std::uint32_t                     child_depth = 0;
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

struct RuntimeConfig {
    std::shared_ptr<ProgramCatalog>         catalog;
    std::shared_ptr<graph::CheckpointStore> checkpoints;
    std::shared_ptr<graph::Store>           state_store;
    std::shared_ptr<ProgramTransitionStore> transitions;
    std::size_t                             scheduler_threads = 1;
};

class NEOGRAPH_PROGRAM_API ProgramRuntime {
public:
    explicit ProgramRuntime(RuntimeConfig config);
    ProgramRuntime(ProgramRuntime&&) noexcept;
    ProgramRuntime& operator=(ProgramRuntime&&) noexcept;
    ProgramRuntime(const ProgramRuntime&)            = delete;
    ProgramRuntime& operator=(const ProgramRuntime&) = delete;
    ~ProgramRuntime();

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
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
