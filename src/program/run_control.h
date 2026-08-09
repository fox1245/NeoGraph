#pragma once

#include <neograph/graph/engine.h>
#include <neograph/program/event.h>
#include <neograph/program/journal.h>
#include <neograph/program/module.h>
#include <neograph/program/result.h>
#include <neograph/program/transition_store.h>
#include "catalog_access.h"
#include <asio/any_io_executor.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/system_executor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::program::detail {

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
};

class RunControl;
using ChildLaunchCallback =
    std::function<std::shared_ptr<RunControl>(std::string_view, json, std::string_view,
                                              std::string_view)>;
using ChildBindingValidationCallback = std::function<void(std::string_view, std::string_view)>;

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
               asio::any_io_executor                      deadline_executor,
               std::shared_ptr<graph::CheckpointStore>    checkpoints,
               std::shared_ptr<graph::Store>              state_store,
               std::shared_ptr<ProgramTransitionStore>    transitions);
    RunControl(ProgramRunRecord record,
               std::shared_ptr<ProgramTransitionStore> transitions);

    using CompletionCallback = std::function<void(const ProgramResult&)>;
    using TerminalCleanup = std::function<void()>;
    void set_completion_callback(CompletionCallback callback) noexcept;
    /**
     * Registers per-attempt cleanup that runs before the terminal transition
     * is published.  Admission leases use this to free host capacity before a
     * completed handle can start the next Program.
     */
    void add_terminal_cleanup(TerminalCleanup cleanup);
    void set_child_launch_callback(ChildLaunchCallback callback) noexcept;
    void set_child_binding_validation_callback(ChildBindingValidationCallback callback) noexcept;

    const std::string                                owner_scope;
    const std::string                                run_id;
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
    const asio::any_io_executor                      deadline_executor;
    const asio::strand<asio::system_executor>        waiter_strand;
    const std::string                                core_thread_id;
    const std::string                                trace_id;
    const std::shared_ptr<graph::CheckpointStore>    checkpoints;
    const std::shared_ptr<graph::Store>              state_store;
    const std::shared_ptr<ProgramTransitionStore>    transitions;
    const std::shared_ptr<graph::CancelToken>        cancel_token;
    const std::shared_ptr<std::atomic_bool>          budget_exhausted =
        std::make_shared<std::atomic_bool>(false);

    bool              cancel(CancellationCause cause) noexcept;
    CancellationCause cancellation_cause() const noexcept;
    CancellationCause seal_terminal_cause() noexcept;
    void              attach_child(const std::shared_ptr<RunControl>& child) noexcept;
    std::shared_ptr<RunControl> launch_child(std::string_view binding_name,
                                             json             input,
                                             std::string_view operation_id,
                                             std::string_view execution_key) const;
    void validate_child_binding(std::string_view binding_name, std::string_view operation_id) const;

    ProgramEvent stage_event(ProgramEventKind kind, ProgramEventPayload payload);
    ProgramEvent stage_event(std::string_view operation_id,
                             ProgramEventKind kind,
                             ProgramEventPayload payload);
    void         deliver_event(const ProgramEvent& event);
    void         emit(ProgramEventKind kind, ProgramEventPayload payload);
    void         emit(std::string_view operation_id,
                      ProgramEventKind kind,
                      ProgramEventPayload payload);
    void         complete(RunOutcome outcome) noexcept;
    /**
     * Atomically append one JavaScript yielded-command journal entry while
     * the attempt remains Running.  A terminal result may complete an
     * existing pending coordinate; the store rejects every other duplicate.
     */
    ProgramTransitionPublishResult publish_javascript_command(
        std::uint64_t              command_ordinal,
        JavaScriptCommand          command,
        std::optional<std::string> effect_identity,
        std::optional<json>        terminal_result,
        std::optional<RunBudget>   remaining_budget     = std::nullopt,
        std::optional<RunBudget>   inflight_reservation = std::nullopt);
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
    ProgramRunRecord                      snapshot() const;
    void              cancel_children(CancellationCause cause) noexcept;

    ProgramResult make_result(RunOutcome outcome) const;
    ProgramEvent  make_event(ProgramEventKind kind, ProgramEventPayload payload);
    ProgramEvent  preview_event(std::uint64_t sequence,
                                ProgramEventKind kind,
                                ProgramEventPayload payload) const;
    void          adopt_published_event(ProgramEvent event);

    struct AsyncWaiter {
        std::weak_ptr<asio::steady_timer> timer;
    };

    mutable std::mutex                    mutex_;
    mutable std::condition_variable       cv_;
    std::optional<ProgramResult>          result_;
    std::vector<ProgramEvent>             events_;
    std::optional<CoreCheckpointIdentity> latest_checkpoint_;
    std::shared_ptr<ProgramEventSink>     sink_;
    CompletionCallback                    completion_callback_;
    mutable std::vector<AsyncWaiter>        waiters_;
    std::vector<TerminalCleanup>           terminal_cleanups_;
    ChildLaunchCallback                   child_launch_callback_;
    ChildBindingValidationCallback          child_binding_validation_callback_;
    std::uint64_t                         next_sequence_      = 1;
    CancellationCause                     cancellation_cause_ = CancellationCause::None;
    bool                                  terminal_decided_   = false;
    bool                                  completion_claimed_ = false;
    std::vector<std::weak_ptr<RunControl>> children_;
};
struct CommandBudgetReservation {
    RunBudget remaining;
    RunBudget reservation;
};

CommandBudgetReservation reserve_command_resources(RunBudget available) noexcept;

asio::awaitable<void> execute_run_attempt(
    std::shared_ptr<RunControl> control,
    json                        input,
    std::optional<std::string>  checkpoint_id,
    std::optional<json>         javascript_core_resume_value = std::nullopt);

}  // namespace neograph::program::detail
