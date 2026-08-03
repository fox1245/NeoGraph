#include <neograph/program/runtime.h>

#include "canonical_json.h"
#include "catalog_access.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Diagnostic runtime_diagnostic(std::string code,
                              std::string message,
                              json        witness = json::object()) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = std::move(code);
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"runtime", "", std::nullopt};
    diagnostic.message  = std::move(message);
    diagnostic.witness  = std::move(witness);
    return diagnostic;
}

[[noreturn]] void throw_runtime_diagnostic(std::string code,
                                           std::string message,
                                           json        witness = json::object()) {
    throw ProgramDiagnosticError(
        runtime_diagnostic(std::move(code), std::move(message), std::move(witness)));
}

std::string generate_run_id() {
    static std::mutex      random_mutex;
    static std::mt19937_64 random(std::random_device{}());
    std::lock_guard        lock(random_mutex);
    static constexpr char  hex[] = "0123456789abcdef";
    std::string            id    = "run-";
    id.reserve(36);
    for (int i = 0; i < 32; ++i)
        id.push_back(hex[random() & 0xf]);
    return id;
}

std::string core_thread_identity(std::string_view run_id, std::string_view generation_id) {
    std::string identity(run_id);
    identity.push_back('\0');
    identity.append("root");
    identity.push_back('\0');
    identity.append(generation_id);
    return detail::sha256_identity("program-core-thread/v1", identity);
}

void complete_attempt_launch_failure(const std::shared_ptr<detail::RunControl>& control,
                                     const std::optional<std::string>&          checkpoint_id,
                                     std::exception_ptr                         error) noexcept {
    std::string message = "Program execution coroutine failed";
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            message += ": ";
            message += exception.what();
        } catch (...) {
            message += " with a non-standard exception";
        }
    }

    detail::RunOutcome outcome;
    outcome.status                   = ProgramTerminalStatus::Failed;
    outcome.remaining_budget         = control->granted_budget;
    outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
    if (control->attempt == 1) outcome.remaining_budget.max_program_operations = 0;
    outcome.failure =
        ProgramFailure{"P_RUNTIME_CORE_FAILURE", std::move(message), "root", "", 0, json::object()};
    if (checkpoint_id) {
        outcome.checkpoint = CoreCheckpointIdentity{
            control->materialized->root->core_name,
            control->materialized->root->compiled_plan_identity, control->core_thread_id,
            *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
    }
    control->complete(std::move(outcome));
}

void spawn_run_attempt(asio::thread_pool&                         pool,
                       const std::shared_ptr<detail::RunControl>& control,
                       json                                       input,
                       std::optional<std::string>                 checkpoint_id) noexcept {
    try {
        asio::co_spawn(pool, detail::execute_run_attempt(control, std::move(input), checkpoint_id),
                       [control, checkpoint_id](std::exception_ptr error) {
                           if (error)
                               complete_attempt_launch_failure(control, checkpoint_id, error);
                       });
    } catch (...) {
        complete_attempt_launch_failure(control, checkpoint_id, std::current_exception());
    }
}
std::uint64_t budget_value(const RunBudget& budget, std::string_view resource) {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    throw_runtime_diagnostic("P_START_BUDGET", "Unknown declared budget resource",
                             json{{"resource", std::string(resource)}});
}

std::uint64_t ceiling_value(const BudgetLimits& budget, std::string_view resource) {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    return 0;
}

void validate_invocation(const detail::MaterializedProgram& materialized,
                         const ProgramInvocation&           invocation) {
    if (!invocation.input.is_object()) {
        throw_runtime_diagnostic("P_START_INPUT", "Program invocation input must be an object");
    }
    try {
        validate_contract_value(invocation.input, materialized.bundle.input_contract());
    } catch (const std::exception& error) {
        throw_runtime_diagnostic("P_START_INPUT_CONTRACT",
                                 "Program invocation input violates its admitted contract",
                                 json{{"detail", error.what()}});
    }
    const auto&    budget = invocation.budget;
    constexpr auto max_safe_wall_time_ms =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) / 2;
    if (budget.wall_time_ms == 0 || budget.wall_time_ms > max_safe_wall_time_ms ||
        budget.max_concurrency == 0 || budget.max_program_operations == 0 ||
        budget.max_core_steps == 0 ||
        budget.max_core_steps > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        budget.max_dynamic_compiles != 0 ||
        ((budget.max_child_depth == 0) != (budget.max_total_children == 0))) {
        throw_runtime_diagnostic(
            "P_START_BUDGET",
            "Program requires positive wall/Core/operation/concurrency limits and "
            "does not permit dynamic compilation or an unpaired child budget");
    }

    const auto ceiling = materialized.version.policy_snapshot().budget_ceiling();
    for (const auto& requirement : materialized.bundle.declared_budget_requirements()) {
        const auto granted = budget_value(budget, requirement.resource);
        const auto allowed = ceiling_value(ceiling, requirement.resource);
        if (granted < requirement.minimum || granted > requirement.maximum || granted > allowed) {
            throw_runtime_diagnostic("P_START_BUDGET",
                                     "Program invocation budget is outside its admitted bounds",
                                     json{{"resource", requirement.resource},
                                          {"minimum", requirement.minimum},
                                          {"maximum", requirement.maximum},
                                          {"policy_ceiling", allowed},
                                          {"granted", granted}});
        }
    }
}

ProgramJournalRecord initial_record(
    const detail::RunControl&             control,
    std::optional<CoreCheckpointIdentity> checkpoint = std::nullopt) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        "", control.run_id, control.program_version_id, control.bundle_id, 1,
        ProgramContinuation{"root", ContinuationState::Running, control.attempt},
        control.granted_budget, control.granted_budget, std::move(checkpoint), now_ms()});
}

ProgramTransitionPublication initial_publication(
    const detail::RunControl&               control,
    const ProgramEvent&                     started,
    std::optional<CoreCheckpointIdentity>   checkpoint           = std::nullopt,
    std::optional<ForkCompatibilityReceipt> fork_receipt         = std::nullopt,
    std::optional<std::string>              recorded_binding_set = std::nullopt,
    std::optional<ProgramPendingInput>      pending_input        = std::nullopt,
    std::optional<ProgramPendingEffect>     pending_effect       = std::nullopt) {
    auto                 journal = initial_record(control, checkpoint);
    ProgramRunRecordData data;
    data.owner_scope         = control.owner_scope;
    data.run_id              = control.run_id;
    data.program_version_id  = control.program_version_id;
    data.bundle_id           = control.bundle_id;
    data.binding_fingerprint = control.binding_fingerprint;
    data.invocation          = control.persisted_invocation;
    data.continuation        = journal.continuation;
    data.remaining_budget    = control.granted_budget;
    data.exact_checkpoint    = std::move(checkpoint);
    data.pending_input       = std::move(pending_input);
    data.pending_effect      = std::move(pending_effect);
    if (fork_receipt) {
        data.fork_source_run_id             = fork_receipt->source_run_id();
        data.fork_source_program_version_id = fork_receipt->source_program_version_id();
        data.fork_source_checkpoint_id      = fork_receipt->source_checkpoint_id();
    }
    data.fork_receipt                     = std::move(fork_receipt);
    data.recorded_binding_set_fingerprint = std::move(recorded_binding_set);
    data.journal_head                     = journal.id;
    data.event_sequence                   = started.sequence;
    data.created_at_ms                    = journal.timestamp_ms;
    data.updated_at_ms                    = journal.timestamp_ms;
    auto run_record                       = ProgramRunRecord::create(std::move(data));
    return ProgramTransitionPublication{std::move(run_record), std::move(journal), {started}, {}};
}

ProgramJournalRecord resumed_record(const detail::RunControl&   control,
                                    const ProgramJournalRecord& previous) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.id, control.run_id, control.program_version_id, control.bundle_id,
        previous.sequence + 1,
        ProgramContinuation{"root", ContinuationState::Running, control.attempt},
        previous.remaining_budget, previous.remaining_budget, previous.core_checkpoint, now_ms()});
}

ContinuationState continuation_state(ProgramTerminalStatus status) {
    switch (status) {
        case ProgramTerminalStatus::Completed:
            return ContinuationState::Completed;
        case ProgramTerminalStatus::Interrupted:
            return ContinuationState::Interrupted;
        case ProgramTerminalStatus::Cancelled:
            return ContinuationState::Cancelled;
        case ProgramTerminalStatus::BudgetExhausted:
            return ContinuationState::BudgetExhausted;
        case ProgramTerminalStatus::TimedOut:
            return ContinuationState::TimedOut;
        case ProgramTerminalStatus::CheckpointIncompatible:
            return ContinuationState::CheckpointIncompatible;
        case ProgramTerminalStatus::Failed:
            return ContinuationState::Failed;
        case ProgramTerminalStatus::AmbiguousEffect:
            return ContinuationState::AmbiguousEffect;
    }
    return ContinuationState::Failed;
}

enum class TerminalPublicationResult : std::uint8_t {
    Published,
    TransitionConflict,
    JournalFailure,
};

TerminalPublicationResult publish_terminal_record(const detail::RunControl& control,
                                                  const ProgramResult&      result) {
    for (int retry = 0; retry < 3; ++retry) {
        std::optional<ProgramRunRecord> previous;
        try {
            previous = control.transitions->load(control.owner_scope, control.run_id);
        } catch (...) {
            return TerminalPublicationResult::JournalFailure;
        }
        if (!previous || previous->continuation().state != ContinuationState::Running ||
            previous->continuation().attempt != control.attempt) {
            return TerminalPublicationResult::TransitionConflict;
        }

        std::optional<ProgramJournalRecord> previous_journal;
        try {
            previous_journal = control.transitions->latest(control.owner_scope, control.run_id);
        } catch (...) {
            return TerminalPublicationResult::JournalFailure;
        }
        if (!previous_journal || previous_journal->id != previous->journal_head()) {
            return TerminalPublicationResult::JournalFailure;
        }

        try {
            auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
                previous->journal_head(), control.run_id, control.program_version_id,
                control.bundle_id, previous_journal->sequence + 1,
                ProgramContinuation{"root", continuation_state(result.status()), control.attempt},
                result.remaining_budget(), RunBudget{}, result.checkpoint(), now_ms()});

            auto                 events = control.events_after(previous->event_sequence());
            ProgramRunRecordData data;
            data.owner_scope         = control.owner_scope;
            data.run_id              = control.run_id;
            data.program_version_id  = control.program_version_id;
            data.bundle_id           = control.bundle_id;
            data.binding_fingerprint = previous->binding_fingerprint();
            data.invocation          = previous->invocation();
            data.continuation        = journal.continuation;
            data.remaining_budget    = result.remaining_budget();
            data.exact_checkpoint    = result.checkpoint();
            data.pending_input       = previous->pending_input();
            data.pending_effect      = previous->pending_effect();
            if (const auto interrupt = result.interrupt()) {
                data.pending_input  = interrupt->pending_input;
                data.pending_effect = interrupt->pending_effect;
            }
            data.terminal_result                  = result;
            data.fork_receipt                     = previous->fork_receipt();
            data.fork_source_run_id               = previous->fork_source_run_id();
            data.fork_source_program_version_id   = previous->fork_source_program_version_id();
            data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
            data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
            data.journal_head                     = journal.id;
            data.event_sequence =
                events.empty() ? previous->event_sequence() : events.back().sequence;
            std::vector<ProgramEffectOutboxEntry> effects;
            data.effect_sequence = previous->effect_sequence();
            if (const auto interrupt = result.interrupt();
                interrupt && interrupt->pending_effect) {
                ++data.effect_sequence;
                effects.emplace_back(data.effect_sequence, *interrupt->pending_effect);
            }
            data.created_at_ms = previous->created_at_ms();
            data.updated_at_ms = journal.timestamp_ms;

            auto publication =
                ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                             std::move(journal),
                                             std::move(events),
                                             std::move(effects)};
            const auto published = control.transitions->compare_publish(
                control.owner_scope, previous->journal_head(), std::move(publication));
            if (published == ProgramTransitionPublishResult::Published ||
                published == ProgramTransitionPublishResult::AlreadyPresent) {
                return TerminalPublicationResult::Published;
            }
        } catch (...) {
            return TerminalPublicationResult::JournalFailure;
        }
    }
    return TerminalPublicationResult::TransitionConflict;
}

}  // namespace

namespace detail {

RunControl::RunControl(std::string                                owner,
                       std::string                                run,
                       std::uint64_t                              attempt_value,
                       std::shared_ptr<const MaterializedProgram> pinned,
                       std::string                                binding,
                       ProgramPersistedInvocation                 invocation,
                       std::string                                thread,
                       std::uint64_t                              event_sequence,
                       std::shared_ptr<ProgramEventSink>          sink,
                       asio::any_io_executor                      deadline_executor_value,
                       std::shared_ptr<graph::CheckpointStore>    checkpoint_store,
                       std::shared_ptr<graph::Store>              store,
                       std::shared_ptr<ProgramTransitionStore>    transition_store)
    : owner_scope(std::move(owner)),
      run_id(std::move(run)),
      program_version_id(pinned->version.id()),
      bundle_id(pinned->bundle.id()),
      binding_fingerprint(std::move(binding)),
      attempt(attempt_value),
      materialized(std::move(pinned)),
      persisted_invocation(std::move(invocation)),
      granted_budget(persisted_invocation.granted_budget),
      started_at(std::chrono::steady_clock::now()),
      deadline(started_at + std::chrono::milliseconds(granted_budget.wall_time_ms)),
      deadline_executor(std::move(deadline_executor_value)),
      waiter_strand(asio::make_strand(asio::system_executor{})),
      core_thread_id(std::move(thread)),
      trace_id(persisted_invocation.trace_id),
      checkpoints(std::move(checkpoint_store)),
      state_store(std::move(store)),
      transitions(std::move(transition_store)),
      cancel_token(std::make_shared<graph::CancelToken>()),
      sink_(std::move(sink)),
      next_sequence_(event_sequence + 1) {}

RunControl::RunControl(ProgramRunRecord                        record,
                       std::shared_ptr<ProgramTransitionStore> transition_store)
    : owner_scope(record.owner_scope()),
      run_id(record.run_id()),
      program_version_id(record.program_version_id()),
      bundle_id(record.bundle_id()),
      binding_fingerprint(record.binding_fingerprint()),
      attempt(record.continuation().attempt),
      materialized(),
      persisted_invocation(record.invocation()),
      granted_budget(persisted_invocation.granted_budget),
      started_at(std::chrono::steady_clock::now()),
      deadline(started_at),
      deadline_executor(asio::system_executor{}),
      waiter_strand(asio::make_strand(asio::system_executor{})),
      core_thread_id(record.exact_checkpoint() ? record.exact_checkpoint()->core_thread_id
                                               : std::string{}),
      trace_id(persisted_invocation.trace_id),
      checkpoints(),
      state_store(),
      transitions(std::move(transition_store)),
      cancel_token(std::make_shared<graph::CancelToken>()),
      result_(record.terminal_result()),
      events_(transitions->load_events(owner_scope, run_id, 0)),
      latest_checkpoint_(record.exact_checkpoint()),
      next_sequence_(record.event_sequence() + 1),
      terminal_decided_(result_.has_value()),
      completion_claimed_(result_.has_value()) {}

bool RunControl::cancel(CancellationCause cause) noexcept {
    try {
        const auto previous = transitions->load(owner_scope, run_id);
        if (previous && previous->continuation().state == ContinuationState::Interrupted) {
            auto                      pending_input  = previous->pending_input();
            auto                      pending_effect = previous->pending_effect();
            ProgramPendingDisposition disposition    = ProgramPendingDisposition::NotAwaiting;
            if (pending_input) {
                const auto update = pending_input->cancel();
                disposition       = update.disposition;
                pending_input     = update.value;
            } else if (pending_effect) {
                const auto update = pending_effect->cancel();
                disposition       = update.disposition;
                pending_effect    = update.value;
            }
            if (disposition == ProgramPendingDisposition::Duplicate) return false;
            if (disposition == ProgramPendingDisposition::Applied) {
                const auto previous_journal = transitions->latest(owner_scope, run_id);
                if (!previous_journal || previous_journal->id != previous->journal_head()) {
                    return false;
                }
                const auto        timestamp = now_ms();
                auto              journal   = ProgramJournalRecord::create(ProgramJournalRecordData{
                    previous->journal_head(), run_id, program_version_id, bundle_id,
                    previous_journal->sequence + 1,
                    ProgramContinuation{"root", ContinuationState::Cancelled, attempt},
                    previous->remaining_budget(), previous->remaining_budget(),
                    previous->exact_checkpoint(), timestamp});
                ProgramResultData result_data;
                result_data.status             = ProgramTerminalStatus::Cancelled;
                result_data.run_id             = run_id;
                result_data.program_version_id = program_version_id;
                result_data.bundle_id          = bundle_id;
                result_data.operation_id       = operation_id;
                result_data.attempt            = attempt;
                result_data.remaining_budget   = previous->remaining_budget();
                result_data.checkpoint         = previous->exact_checkpoint();
                auto       cancelled_result    = ProgramResult::create(std::move(result_data));
                const auto checkpoint          = previous->exact_checkpoint();
                auto       terminal            = ProgramEvent::create(
                    ProgramEvent{"", previous->event_sequence() + 1, timestamp, run_id,
                                 program_version_id, bundle_id, operation_id,
                                 checkpoint ? checkpoint->core_generation_id : std::string{},
                                 checkpoint ? checkpoint->core_thread_id : std::string{}, trace_id,
                                 attempt, ProgramEventKind::Terminal,
                                 ProgramTerminalEvent{ProgramTerminalStatus::Cancelled}});
                ProgramRunRecordData data;
                data.owner_scope                    = owner_scope;
                data.run_id                         = run_id;
                data.program_version_id             = program_version_id;
                data.bundle_id                      = bundle_id;
                data.binding_fingerprint            = previous->binding_fingerprint();
                data.invocation                     = previous->invocation();
                data.continuation                   = journal.continuation;
                data.remaining_budget               = previous->remaining_budget();
                data.exact_checkpoint               = previous->exact_checkpoint();
                data.pending_input                  = std::move(pending_input);
                data.pending_effect                 = std::move(pending_effect);
                data.terminal_result                = cancelled_result;
                data.fork_receipt                   = previous->fork_receipt();
                data.fork_source_run_id             = previous->fork_source_run_id();
                data.fork_source_program_version_id = previous->fork_source_program_version_id();
                data.fork_source_checkpoint_id      = previous->fork_source_checkpoint_id();
                data.recorded_binding_set_fingerprint =
                    previous->recorded_binding_set_fingerprint();
                data.journal_head    = journal.id;
                data.event_sequence  = terminal.sequence;
                data.effect_sequence = previous->effect_sequence();
                data.created_at_ms   = previous->created_at_ms();
                data.updated_at_ms   = timestamp;
                const auto published = transitions->compare_publish(
                    owner_scope, previous->journal_head(),
                    ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                                 std::move(journal),
                                                 {terminal},
                                                 {}});
                if (published != ProgramTransitionPublishResult::Published) {
                    return false;
                }
                {
                    std::lock_guard lock(mutex_);
                    cancellation_cause_ = cause;
                    terminal_decided_   = true;
                    completion_claimed_ = true;
                    result_             = std::move(cancelled_result);
                    events_.push_back(std::move(terminal));
                }
                cancel_token->cancel();
                return true;
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (result_ || terminal_decided_ || cancellation_cause_ != CancellationCause::None)
                return false;
            cancellation_cause_ = cause;
        }
        cancel_token->cancel();
        return true;
    } catch (...) {
        return false;
    }
}

CancellationCause RunControl::cancellation_cause() const noexcept {
    std::lock_guard lock(mutex_);
    return cancellation_cause_;
}

CancellationCause RunControl::seal_terminal_cause() noexcept {
    std::lock_guard lock(mutex_);
    terminal_decided_ = true;
    return cancellation_cause_;
}

ProgramEvent RunControl::make_event(ProgramEventKind kind, ProgramEventPayload payload) {
    ProgramEvent event;
    event.sequence           = next_sequence_++;
    event.timestamp_ms       = now_ms();
    event.run_id             = run_id;
    event.program_version_id = program_version_id;
    event.bundle_id          = bundle_id;
    event.operation_id       = operation_id;
    event.core_generation_id = materialized->root->compiled_plan_identity;
    event.core_run_id        = core_thread_id;
    event.trace_id           = trace_id;
    event.attempt            = attempt;
    event.kind               = kind;
    event.payload            = std::move(payload);
    return ProgramEvent::create(std::move(event));
}

ProgramEvent RunControl::stage_event(ProgramEventKind kind, ProgramEventPayload payload) {
    std::lock_guard lock(mutex_);
    auto            event = make_event(kind, std::move(payload));
    events_.push_back(event);
    return event;
}

void RunControl::deliver_event(const ProgramEvent& event) {
    std::shared_ptr<ProgramEventSink> sink;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
    }
    if (!sink) return;
    try {
        sink->on_event(event);
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
        }
        cancel(CancellationCause::EventSink);
        throw EventSinkError(error.what());
    } catch (...) {
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
        }
        cancel(CancellationCause::EventSink);
        throw EventSinkError("Program event sink threw");
    }
}

void RunControl::emit(ProgramEventKind kind, ProgramEventPayload payload) {
    const auto event = stage_event(kind, std::move(payload));
    deliver_event(event);
}

ProgramResult RunControl::make_result(RunOutcome outcome) const {
    return ProgramResult(ProgramResult::ConstructionData{
        outcome.status, run_id, program_version_id, bundle_id, operation_id, attempt,
        std::move(outcome.output), outcome.usage, outcome.remaining_budget,
        std::move(outcome.checkpoint), std::move(outcome.interrupt), std::move(outcome.failure),
        std::move(outcome.execution_trace)});
}

void ensure_terminal_checkpoint(const RunControl& control, RunOutcome& outcome) {
    if (outcome.checkpoint || !control.checkpoints || !control.materialized) return;

    graph::Checkpoint checkpoint;
    checkpoint.id               = graph::Checkpoint::generate_id();
    checkpoint.thread_id        = control.core_thread_id;
    checkpoint.channel_values   = outcome.output.is_null() ? json::object() : outcome.output;
    checkpoint.channel_versions = json::object();
    checkpoint.parent_id        = {};
    checkpoint.current_node     = {};
    checkpoint.next_nodes       = {};
    checkpoint.interrupt_phase  = graph::CheckpointPhase::Completed;
    checkpoint.metadata         = json{{"neograph_program", "terminal"}};
    checkpoint.step             = 0;
    checkpoint.timestamp        = now_ms();
    checkpoint.schema_version   = graph::CHECKPOINT_SCHEMA_VERSION;
    control.checkpoints->save(checkpoint);
    outcome.checkpoint = CoreCheckpointIdentity{
        control.materialized->root->core_name,
        control.materialized->root->compiled_plan_identity,
        control.core_thread_id,
        checkpoint.id,
        checkpoint.schema_version};
}

void RunControl::complete(RunOutcome outcome) noexcept {
    try {
        {
            std::lock_guard lock(mutex_);
            if (completion_claimed_) return;
            completion_claimed_ = true;
            terminal_decided_   = true;
        }
        ensure_terminal_checkpoint(*this, outcome);

        std::vector<ProgramEvent> terminal_events;
        if (outcome.checkpoint) {
            terminal_events.push_back(stage_event(ProgramEventKind::CheckpointPublished,
                                                  ProgramCheckpointEvent{*outcome.checkpoint}));
        }
        terminal_events.push_back(
            stage_event(ProgramEventKind::Terminal, ProgramTerminalEvent{outcome.status}));

        auto       result       = make_result(outcome);
        const auto publication  = publish_terminal_record(*this, result);
        const bool is_published = publication == TerminalPublicationResult::Published;
        if (!is_published) {
            const auto first_unpublished_sequence = terminal_events.front().sequence;
            {
                std::lock_guard lock(mutex_);
                while (!events_.empty() && events_.back().sequence >= first_unpublished_sequence) {
                    events_.pop_back();
                }
            }

            RunOutcome failed;
            failed.status             = ProgramTerminalStatus::Failed;
            failed.remaining_budget   = outcome.remaining_budget;
            const bool journal_failed = publication == TerminalPublicationResult::JournalFailure;
            failed.failure            = ProgramFailure{
                journal_failed ? "P_JOURNAL_CONFLICT" : "P_TRANSITION_CONFLICT",
                journal_failed ? "Program journal finalization failed"
                               : "Atomic Program terminal transition publication failed",
                "root",
                "",
                0,
                json::object()};
            result = make_result(std::move(failed));
        } else {
            if (outcome.checkpoint) {
                std::lock_guard lock(mutex_);
                latest_checkpoint_ = outcome.checkpoint;
            }
            for (const auto& event : terminal_events) {
                try {
                    deliver_event(event);
                } catch (const EventSinkError&) {
                    break;
                }
            }
        }
        {
            std::lock_guard lock(mutex_);
            sink_.reset();
        }

        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
            waiters.swap(waiters_);
            cv_.notify_all();
        }
        asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
            for (const auto& waiter : waiters) {
                if (auto timer = waiter.timer.lock()) timer->cancel();
            }
        });
    } catch (...) {
        std::terminate();
    }
}

ProgramResult RunControl::wait() const {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return result_.has_value(); });
    return *result_;
}

asio::awaitable<ProgramResult> RunControl::wait_async() const {
    auto timer = std::make_shared<asio::steady_timer>(waiter_strand);
    timer->expires_at((asio::steady_timer::time_point::max)());
    co_await asio::co_spawn(
        waiter_strand,
        [this, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(mutex_);
                if (result_) co_return;
                waiters_.push_back(AsyncWaiter{timer});
            }
            asio::error_code error;
            co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);

    std::lock_guard lock(mutex_);
    if (!result_) throw std::runtime_error("Program wait ended before terminal publication");
    co_return* result_;
}

std::optional<ProgramResult> RunControl::try_result() const {
    std::lock_guard lock(mutex_);
    return result_;
}

std::vector<ProgramEvent> RunControl::events_after(std::uint64_t sequence) const {
    std::lock_guard           lock(mutex_);
    std::vector<ProgramEvent> result;
    const auto                first = std::upper_bound(
        events_.begin(), events_.end(), sequence,
        [](std::uint64_t value, const ProgramEvent& event) { return value < event.sequence; });
    result.assign(first, events_.end());
    return result;
}

std::optional<CoreCheckpointIdentity> RunControl::latest_checkpoint() const {
    std::lock_guard lock(mutex_);
    return latest_checkpoint_;
}

ProgramRunRecord RunControl::snapshot() const {
    const auto record = transitions->load(owner_scope, run_id);
    if (!record) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    return *record;
}

}  // namespace detail
namespace {

asio::awaitable<ProgramResult> wait_for_control(std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->wait_async();
}

}  // namespace

ProgramHandle::ProgramHandle(std::shared_ptr<detail::RunControl> control)
    : control_(std::move(control)) {}
ProgramHandle::~ProgramHandle() = default;
const std::string& ProgramHandle::run_id() const noexcept {
    return control_->run_id;
}
const std::string& ProgramHandle::program_version_id() const noexcept {
    return control_->program_version_id;
}
std::uint64_t ProgramHandle::attempt() const noexcept {
    return control_->attempt;
}
bool ProgramHandle::cancel() noexcept {
    return control_->cancel(detail::CancellationCause::User);
}
ProgramResult ProgramHandle::wait() const {
    return control_->wait();
}
asio::awaitable<ProgramResult> ProgramHandle::wait_async() const {
    return wait_for_control(control_);
}
std::optional<ProgramResult> ProgramHandle::try_result() const {
    return control_->try_result();
}
std::vector<ProgramEvent> ProgramHandle::events_after(std::uint64_t sequence) const {
    return control_->events_after(sequence);
}
std::optional<CoreCheckpointIdentity> ProgramHandle::latest_checkpoint() const {
    return control_->latest_checkpoint();
}
ProgramRunRecord ProgramHandle::snapshot() const {
    return control_->snapshot();
}
ProgramHandle ProgramHandle::resume(ProgramRuntime& runtime, ProgramResume resume_value) const {
    return runtime.resume(control_->owner_scope, control_->run_id, std::move(resume_value));
}
ProgramHandle ProgramHandle::reconcile(ProgramRuntime&         runtime,
                                       ProgramEffectResolution resolution) const {
    return runtime.reconcile(control_->owner_scope, control_->run_id, std::move(resolution));
}

struct ProgramRuntime::Impl {
    explicit Impl(RuntimeConfig runtime_config)
        : config(std::move(runtime_config)), pool(config.scheduler_threads) {}

    void register_control(const std::shared_ptr<detail::RunControl>& control) {
        std::lock_guard lock(mutex);
        if (stopping) throw std::runtime_error("ProgramRuntime is stopping");
        std::erase_if(controls, [](const std::weak_ptr<detail::RunControl>& weak) {
            const auto live = weak.lock();
            return !live || live->try_result().has_value();
        });
        controls.push_back(control);
    }

    std::shared_ptr<detail::RunControl> find_control(std::string_view owner_scope,
                                                     std::string_view run_id) {
        std::lock_guard lock(mutex);
        for (auto& weak : controls) {
            auto control = weak.lock();
            if (control && control->owner_scope == owner_scope && control->run_id == run_id) {
                return control;
            }
        }
        return {};
    }

    void shutdown() noexcept {
        try {
            std::vector<std::shared_ptr<detail::RunControl>> live;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                stopping = true;
                for (auto& weak : controls) {
                    if (auto control = weak.lock()) live.push_back(std::move(control));
                }
            }
            for (auto& control : live)
                control->cancel(detail::CancellationCause::RuntimeShutdown);
            pool.join();
            deadline_pool.join();
        } catch (...) {
            std::terminate();
        }
    }

    RuntimeConfig                                  config;
    asio::thread_pool                              pool;
    asio::thread_pool                              deadline_pool{1};
    std::mutex                                     mutex;
    bool                                           stopping = false;
    std::vector<std::weak_ptr<detail::RunControl>> controls;
};

ProgramRuntime::ProgramRuntime(RuntimeConfig config) {
    if (!config.catalog || !config.checkpoints || !config.transitions ||
        config.scheduler_threads == 0) {
        throw std::invalid_argument(
            "ProgramRuntime requires catalog, checkpoints, transitions, and at least one "
            "scheduler thread");
    }
    impl_ = std::make_unique<Impl>(std::move(config));
}

ProgramRuntime::ProgramRuntime(ProgramRuntime&&) noexcept = default;

ProgramRuntime& ProgramRuntime::operator=(ProgramRuntime&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) impl_->shutdown();
    impl_ = std::move(other.impl_);
    return *this;
}

ProgramRuntime::~ProgramRuntime() {
    if (impl_) impl_->shutdown();
}

ProgramHandle ProgramRuntime::start(std::string_view      owner_scope,
                                    const ProgramVersion& version,
                                    ProgramInvocation     invocation) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (version.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    const auto resolved = impl_->config.catalog->resolve_version(owner_scope, version.id());
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *resolved);
    validate_invocation(*pinned, invocation);
    const auto run_id =
        invocation.requested_run_id.empty() ? generate_run_id() : invocation.requested_run_id;
    const auto core_thread_id = core_thread_identity(run_id, pinned->root->compiled_plan_identity);
    const auto binding_fingerprint = capability_binding_receipt_root(
        pinned->version.core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{invocation.input, invocation.budget, invocation.trace_id};
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, 1, std::move(pinned), binding_fingerprint,
        std::move(persisted), core_thread_id, 0, std::move(invocation.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.transitions);

    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    const auto published = control->transitions->compare_publish(
        owner_scope, "", initial_publication(*control, started));
    if (published != ProgramTransitionPublishResult::Published) {
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }

    impl_->register_control(control);

    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status                                  = ProgramTerminalStatus::Failed;
        failed.remaining_budget                        = invocation.budget;
        failed.usage.program_operations                = 1;
        failed.remaining_budget.max_program_operations = 0;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }

    spawn_run_attempt(impl_->pool, control, std::move(invocation.input), std::nullopt);
    return ProgramHandle(std::move(control));
}

ProgramHandle ProgramRuntime::start_recorded(std::string_view      owner_scope,
                                             const ProgramVersion& version,
                                             ProgramInvocation     invocation,
                                             RecordedBindingSet    recorded) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (version.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    recorded.validate_target(version);
    const auto recorded_fingerprint = recorded.fingerprint();
    auto pinned = detail::CatalogRuntimeAccess::pin_with_binding(
        *impl_->config.catalog, owner_scope, version.id(),
        std::move(recorded).release_owned_binding());
    if (pinned->version.id() != version.id()) {
        throw_runtime_diagnostic("P_REPLAY_BINDING",
                                 "Recorded capability binding was not accepted");
    }
    validate_invocation(*pinned, invocation);
    const auto run_id =
        invocation.requested_run_id.empty() ? generate_run_id() : invocation.requested_run_id;
    const auto core_thread_id = core_thread_identity(run_id, pinned->root->compiled_plan_identity);
    const auto binding_fingerprint = capability_binding_receipt_root(
        pinned->version.core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{invocation.input, invocation.budget, invocation.trace_id};
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, 1, std::move(pinned), binding_fingerprint,
        std::move(persisted), core_thread_id, 0, std::move(invocation.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.transitions);
    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    const auto published = control->transitions->compare_publish(
        owner_scope, "",
        initial_publication(*control, started, std::nullopt, std::nullopt, recorded_fingerprint));
    if (published != ProgramTransitionPublishResult::Published) {
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }
    impl_->register_control(control);
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status                                  = ProgramTerminalStatus::Failed;
        failed.remaining_budget                        = invocation.budget;
        failed.usage.program_operations                = 1;
        failed.remaining_budget.max_program_operations = 0;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }
    spawn_run_attempt(impl_->pool, control, std::move(invocation.input), std::nullopt);
    return ProgramHandle(std::move(control));
}

ProgramHandle ProgramRuntime::fork(std::string_view                owner_scope,
                                   ExactProgramCheckpointReference source,
                                   const ProgramVersion&           target,
                                   ProgramInvocation               invocation,
                                   ProgramResume                   resume_value) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (source.source_run_id.empty() || source.source_checkpoint_id.empty()) {
        throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND", "Program checkpoint was not found");
    }
    const auto source_record = impl_->config.transitions->load(owner_scope, source.source_run_id);
    if (!source_record || !source_record->exact_checkpoint() ||
        source_record->exact_checkpoint()->checkpoint_id != source.source_checkpoint_id) {
        throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND", "Program checkpoint was not found");
    }
    if (target.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    const auto source_version =
        impl_->config.catalog->resolve_version(owner_scope, source_record->program_version_id());
    const auto resolved_target = impl_->config.catalog->resolve_version(owner_scope, target.id());
    if (!source_version || !resolved_target) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto source_pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *source_version);
    auto target_pinned =
        detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *resolved_target);

    const auto checkpoint_identity = *source_record->exact_checkpoint();
    const auto loaded_checkpoint =
        impl_->config.checkpoints->load_by_id(checkpoint_identity.checkpoint_id);
    if (!loaded_checkpoint || loaded_checkpoint->id != checkpoint_identity.checkpoint_id ||
        loaded_checkpoint->thread_id != checkpoint_identity.core_thread_id ||
        loaded_checkpoint->schema_version != checkpoint_identity.checkpoint_schema_version) {
        throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND", "Program checkpoint was not found");
    }

    const auto source_definitions = source_pinned->bundle.sealed_core_definitions();
    const auto target_definitions = target_pinned->bundle.sealed_core_definitions();
    ExactForkCompatibilityFacts facts;
    facts.owner_scope                        = std::string(owner_scope);
    facts.source_owner_scope                 = source_record->owner_scope();
    facts.target_owner_scope                 = resolved_target->ownership_scope();
    facts.requested_source                   = source;
    facts.stored_source_run_id               = source_record->run_id();
    facts.source_program_version_id          = source_version->id();
    facts.target_program_version_id          = target.id();
    facts.resolved_target_program_version_id = resolved_target->id();
    facts.published_checkpoint               = checkpoint_identity;
    facts.loaded_checkpoint                  = *loaded_checkpoint;
    facts.source_core_plan       = source_version->core_materialization_receipt().plans.front();
    facts.target_core_plan       = resolved_target->core_materialization_receipt().plans.front();
    facts.source_core_definition = source_definitions.front();
    facts.target_core_definition = target_definitions.front();
    facts.source_continuation    = source_record->continuation();
    auto receipt                 = check_exact_fork_compatibility(std::move(facts));
    if (!receipt.compatible()) {
        throw ProgramForkCompatibilityError(std::move(receipt));
    }

    const auto remaining = source_record->remaining_budget();
    const auto requested = invocation.budget;
    const bool budget_within_source =
        requested.wall_time_ms <= remaining.wall_time_ms &&
        requested.model_tokens <= remaining.model_tokens &&
        requested.monetary_microunits <= remaining.monetary_microunits &&
        requested.max_concurrency <= remaining.max_concurrency &&
        requested.max_program_operations <= remaining.max_program_operations &&
        requested.max_core_steps <= remaining.max_core_steps &&
        requested.max_dynamic_compiles <= remaining.max_dynamic_compiles &&
        requested.max_child_depth <= remaining.max_child_depth &&
        requested.max_total_children <= remaining.max_total_children;
    if (!invocation.input.is_object() || !budget_within_source || requested.wall_time_ms == 0 ||
        requested.max_core_steps == 0) {
        throw_runtime_diagnostic(
            "P_FORK_BUDGET",
            "Fork continuation budget must be positive and within the source remainder");
    }
    try {
        validate_contract_value(invocation.input, target_pinned->bundle.input_contract());
    } catch (const std::exception& error) {
        throw_runtime_diagnostic("P_START_INPUT_CONTRACT",
                                 "Program invocation input violates its admitted contract",
                                 json{{"detail", error.what()}});
    }
    auto       fork_pending_input  = source_record->pending_input();
    auto       fork_pending_effect = source_record->pending_effect();
    const auto transition_time     = static_cast<std::uint64_t>(now_ms());
    if (fork_pending_input) {
        const auto update = fork_pending_input->submit(resume_value.pending_id, resume_value.value,
                                                       transition_time);
        if (update.disposition != ProgramPendingDisposition::Applied) {
            throw_runtime_diagnostic(update.diagnostic_code, update.message);
        }
        fork_pending_input = update.value;
    } else if (fork_pending_effect) {
        const auto update =
            fork_pending_effect->submit(resume_value.pending_id, fork_pending_effect->effect_id(),
                                        resume_value.value, transition_time);
        if (update.disposition != ProgramPendingDisposition::Applied) {
            throw_runtime_diagnostic(update.diagnostic_code, update.message);
        }
        fork_pending_effect = update.value;
    } else if (!resume_value.pending_id.empty()) {
        throw_runtime_diagnostic("P_PENDING_WRONG_ID",
                                 "Source run has no matching pending operation");
    }

    const auto run_id =
        invocation.requested_run_id.empty() ? generate_run_id() : invocation.requested_run_id;
    const auto target_thread_id =
        core_thread_identity(run_id, target_pinned->root->compiled_plan_identity);
    auto cloned_checkpoint      = *loaded_checkpoint;
    cloned_checkpoint.id        = graph::Checkpoint::generate_id();
    cloned_checkpoint.thread_id = target_thread_id;
    cloned_checkpoint.parent_id = checkpoint_identity.checkpoint_id;
    cloned_checkpoint.metadata["forked_from"] =
        json{{"thread_id", checkpoint_identity.core_thread_id},
             {"checkpoint_id", checkpoint_identity.checkpoint_id}};
    cloned_checkpoint.timestamp =
        std::max<std::int64_t>(now_ms(), loaded_checkpoint->timestamp + 1);
    impl_->config.checkpoints->save(cloned_checkpoint);
    const auto                   cloned_checkpoint_id = cloned_checkpoint.id;
    const CoreCheckpointIdentity target_checkpoint{
        target_pinned->root->core_name, target_pinned->root->compiled_plan_identity,
        target_thread_id, cloned_checkpoint_id, cloned_checkpoint.schema_version};
    const auto binding_fingerprint = capability_binding_receipt_root(
        resolved_target->core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{invocation.input, invocation.budget, invocation.trace_id};
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, source_record->continuation().attempt + 1,
        std::move(target_pinned), binding_fingerprint, std::move(persisted), target_thread_id, 0,
        std::move(invocation.events), impl_->deadline_pool.get_executor(),
        impl_->config.checkpoints, impl_->config.state_store, impl_->config.transitions);
    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    const auto published = control->transitions->compare_publish(
        owner_scope, "",
        initial_publication(*control, started, target_checkpoint, receipt, std::nullopt,
                            std::move(fork_pending_input), std::move(fork_pending_effect)));
    if (published != ProgramTransitionPublishResult::Published) {
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }
    impl_->register_control(control);
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = invocation.budget;
        failed.checkpoint       = target_checkpoint;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }
    spawn_run_attempt(impl_->pool, control, std::move(resume_value.value),
                      target_checkpoint.checkpoint_id);
    return ProgramHandle(std::move(control));
}

ProgramHandle ProgramRuntime::reconnect(std::string_view owner_scope, std::string_view run_id) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program reconnect run_id must not be empty");
    if (auto control = impl_->find_control(owner_scope, run_id)) {
        return ProgramHandle(std::move(control));
    }

    const auto record = impl_->config.transitions->load(owner_scope, run_id);
    if (!record) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    if (record->continuation().state == ContinuationState::Running && !record->exact_checkpoint()) {
        throw_runtime_diagnostic(
            "P_RUN_RECOVERY_BLOCKED",
            "Running Program has no atomically published checkpoint and cannot be restarted");
    }
    const auto state           = record->continuation().state;
    const bool requires_result = state != ContinuationState::Running &&
                                 state != ContinuationState::Interrupted &&
                                 state != ContinuationState::AmbiguousEffect;
    if (requires_result && !record->terminal_result()) {
        throw_runtime_diagnostic("P_RUN_INVALID", "Stored terminal Program run is incomplete");
    }
    return ProgramHandle(std::make_shared<detail::RunControl>(*record, impl_->config.transitions));
}

ProgramHandle ProgramRuntime::resume(std::string_view owner_scope,
                                     std::string_view run_id,
                                     ProgramResume    resume_value) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program resume run_id must not be empty");

    const auto previous = impl_->config.transitions->load(owner_scope, run_id);
    if (!previous) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    if (previous->recorded_binding_set_fingerprint()) {
        throw_runtime_diagnostic(
            "P_REPLAY_EVIDENCE_REQUIRED",
            "Recorded replay continuation requires the exact owned evidence binding");
    }
    auto pending_input  = previous->pending_input();
    auto pending_effect = previous->pending_effect();
    if (previous->continuation().state != ContinuationState::Interrupted ||
        !previous->exact_checkpoint()) {
        const bool duplicate_input =
            pending_input && pending_input->state() == ProgramPendingState::Consumed &&
            pending_input->call_id() == resume_value.pending_id &&
            pending_input->consumed_result() &&
            *pending_input->consumed_result() == resume_value.value;
        const bool duplicate_effect =
            pending_effect && pending_effect->state() == ProgramPendingState::Consumed &&
            pending_effect->call_id() == resume_value.pending_id &&
            pending_effect->reconciled_result() &&
            *pending_effect->reconciled_result() == resume_value.value;
        if (duplicate_input || duplicate_effect) return reconnect(owner_scope, run_id);
        throw_runtime_diagnostic("P_RESUME_STATE", "Only an interrupted Program run may resume");
    }
    const auto                transition_time     = static_cast<std::uint64_t>(now_ms());
    ProgramPendingDisposition pending_disposition = ProgramPendingDisposition::Applied;
    std::string               pending_code;
    std::string               pending_message;
    if (pending_input) {
        const auto update =
            pending_input->submit(resume_value.pending_id, resume_value.value, transition_time);
        pending_disposition = update.disposition;
        pending_code        = update.diagnostic_code;
        pending_message     = update.message;
        pending_input       = update.value;
    } else if (pending_effect) {
        const auto update =
            pending_effect->submit(resume_value.pending_id, pending_effect->effect_id(),
                                   resume_value.value, transition_time);
        pending_disposition = update.disposition;
        pending_code        = update.diagnostic_code;
        pending_message     = update.message;
        pending_effect      = update.value;
    } else if (!resume_value.pending_id.empty()) {
        throw_runtime_diagnostic("P_PENDING_WRONG_ID",
                                 "Program run has no matching pending operation");
    }
    if (pending_disposition == ProgramPendingDisposition::Duplicate) {
        return reconnect(owner_scope, run_id);
    }
    if (pending_disposition == ProgramPendingDisposition::Expired) {
        const auto previous_journal = impl_->config.transitions->latest(owner_scope, run_id);
        if (!previous_journal || previous_journal->id != previous->journal_head()) {
            throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
        }
        const auto        checkpoint = *previous->exact_checkpoint();
        auto              journal    = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), std::string(run_id), previous->program_version_id(),
            previous->bundle_id(), previous_journal->sequence + 1,
            ProgramContinuation{"root", ContinuationState::Failed,
                                previous->continuation().attempt},
            previous->remaining_budget(), previous->remaining_budget(), checkpoint, now_ms()});
        ProgramResultData result_data;
        result_data.status             = ProgramTerminalStatus::Failed;
        result_data.run_id             = std::string(run_id);
        result_data.program_version_id = previous->program_version_id();
        result_data.bundle_id          = previous->bundle_id();
        result_data.operation_id       = "root";
        result_data.attempt            = previous->continuation().attempt;
        result_data.remaining_budget   = previous->remaining_budget();
        result_data.checkpoint         = checkpoint;
        result_data.failure =
            ProgramFailure{"P_PENDING_EXPIRED",
                           "Pending Program result expired before acceptance",
                           "root",
                           pending_input ? pending_input->core_node() : pending_effect->core_node(),
                           0,
                           json::object()};
        auto                 terminal_result = ProgramResult::create(std::move(result_data));
        auto                 terminal_event  = ProgramEvent::create(ProgramEvent{
            "", previous->event_sequence() + 1, now_ms(), std::string(run_id),
            previous->program_version_id(), previous->bundle_id(), "root",
            checkpoint.core_generation_id, checkpoint.core_thread_id, resume_value.trace_id,
            previous->continuation().attempt, ProgramEventKind::Terminal,
            ProgramTerminalEvent{ProgramTerminalStatus::Failed}});
        ProgramRunRecordData expired_data;
        expired_data.owner_scope                    = std::string(owner_scope);
        expired_data.run_id                         = std::string(run_id);
        expired_data.program_version_id             = previous->program_version_id();
        expired_data.bundle_id                      = previous->bundle_id();
        expired_data.binding_fingerprint            = previous->binding_fingerprint();
        expired_data.invocation                     = previous->invocation();
        expired_data.continuation                   = journal.continuation;
        expired_data.remaining_budget               = previous->remaining_budget();
        expired_data.exact_checkpoint               = checkpoint;
        expired_data.pending_input                  = pending_input;
        expired_data.pending_effect                 = pending_effect;
        expired_data.terminal_result                = terminal_result;
        expired_data.fork_receipt                   = previous->fork_receipt();
        expired_data.fork_source_run_id             = previous->fork_source_run_id();
        expired_data.fork_source_program_version_id = previous->fork_source_program_version_id();
        expired_data.fork_source_checkpoint_id      = previous->fork_source_checkpoint_id();
        expired_data.recorded_binding_set_fingerprint =
            previous->recorded_binding_set_fingerprint();
        expired_data.journal_head    = journal.id;
        expired_data.event_sequence  = terminal_event.sequence;
        expired_data.effect_sequence = previous->effect_sequence();
        expired_data.created_at_ms   = previous->created_at_ms();
        expired_data.updated_at_ms   = journal.timestamp_ms;
        const auto published         = impl_->config.transitions->compare_publish(
            owner_scope, previous->journal_head(),
            ProgramTransitionPublication{ProgramRunRecord::create(std::move(expired_data)),
                                         std::move(journal),
                                                 {terminal_event},
                                                 {}});
        if (published != ProgramTransitionPublishResult::Published &&
            published != ProgramTransitionPublishResult::AlreadyPresent) {
            throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
        }
        throw_runtime_diagnostic(pending_code, pending_message);
    }
    if (pending_disposition != ProgramPendingDisposition::Applied) {
        throw_runtime_diagnostic(pending_code, pending_message);
    }

    const auto version =
        impl_->config.catalog->resolve_version(owner_scope, previous->program_version_id());
    if (!version) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto       pinned     = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *version);
    const auto checkpoint = *previous->exact_checkpoint();
    const auto binding_fingerprint = capability_binding_receipt_root(
        version->core_materialization_receipt().capability_bindings);
    if (pinned->bundle.id() != previous->bundle_id() ||
        binding_fingerprint != previous->binding_fingerprint() ||
        pinned->root->compiled_plan_identity != checkpoint.core_generation_id ||
        pinned->root->core_name != checkpoint.core_name) {
        throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                 "Program resume identity binding is incompatible");
    }
    // Exercise the non-consuming lookup before the attempt CAS. The consuming
    // checks in execute_run_attempt and Core remain authoritative because the
    // checkpoint may change between this lookup and execution.
    (void)impl_->config.checkpoints->load_by_id(checkpoint.checkpoint_id);

    const auto previous_journal = impl_->config.transitions->latest(owner_scope, run_id);
    if (!previous_journal || previous_journal->id != previous->journal_head()) {
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
    }

    ProgramPersistedInvocation attempt_invocation{
        previous->invocation().input, previous->remaining_budget(), resume_value.trace_id};
    auto control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), std::string(run_id), previous->continuation().attempt + 1,
        std::move(pinned), previous->binding_fingerprint(), std::move(attempt_invocation),
        checkpoint.core_thread_id, previous->event_sequence(), std::move(resume_value.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.transitions);
    auto       journal = resumed_record(*control, *previous_journal);
    const auto started = control->stage_event(ProgramEventKind::Started,
                                              ProgramStartedEvent{previous->remaining_budget()});

    ProgramRunRecordData data;
    data.owner_scope                      = std::string(owner_scope);
    data.run_id                           = std::string(run_id);
    data.program_version_id               = previous->program_version_id();
    data.bundle_id                        = previous->bundle_id();
    data.binding_fingerprint              = previous->binding_fingerprint();
    data.invocation                       = previous->invocation();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = previous->remaining_budget();
    data.exact_checkpoint                 = checkpoint;
    data.pending_input                    = std::move(pending_input);
    data.pending_effect                   = std::move(pending_effect);
    data.fork_receipt                     = previous->fork_receipt();
    data.fork_source_run_id               = previous->fork_source_run_id();
    data.fork_source_program_version_id   = previous->fork_source_program_version_id();
    data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
    data.journal_head                     = journal.id;
    data.event_sequence                   = started.sequence;
    data.effect_sequence                  = previous->effect_sequence();
    data.created_at_ms                    = previous->created_at_ms();
    data.updated_at_ms                    = journal.timestamp_ms;

    auto publication = ProgramTransitionPublication{
        ProgramRunRecord::create(std::move(data)), std::move(journal), {started}, {}};
    const auto published = impl_->config.transitions->compare_publish(
        owner_scope, previous->journal_head(), std::move(publication));
    if (published != ProgramTransitionPublishResult::Published) {
        const auto                winner = impl_->config.transitions->load(owner_scope, run_id);
        ProgramPendingDisposition disposition = ProgramPendingDisposition::Conflict;
        if (winner && winner->pending_input()) {
            disposition = winner->pending_input()
                              ->submit(resume_value.pending_id, resume_value.value, transition_time)
                              .disposition;
        } else if (winner && winner->pending_effect()) {
            disposition =
                winner->pending_effect()
                    ->submit(resume_value.pending_id, winner->pending_effect()->effect_id(),
                             resume_value.value, transition_time)
                    .disposition;
        }
        if (disposition == ProgramPendingDisposition::Duplicate) {
            return reconnect(owner_scope, run_id);
        }
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
    }

    impl_->register_control(control);
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = previous->remaining_budget();
        failed.checkpoint       = checkpoint;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        control->complete(std::move(failed));
        return ProgramHandle(std::move(control));
    }

    spawn_run_attempt(impl_->pool, control, std::move(resume_value.value),
                      checkpoint.checkpoint_id);
    return ProgramHandle(std::move(control));
}

ProgramHandle ProgramRuntime::reconcile(std::string_view        owner_scope,
                                        std::string_view        run_id,
                                        ProgramEffectResolution resolution) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program reconcile run_id must not be empty");
    const auto previous = impl_->config.transitions->load(owner_scope, run_id);
    if (!previous) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    auto pending = previous->pending_effect();
    if (!pending || !previous->exact_checkpoint()) {
        throw_runtime_diagnostic("P_EFFECT_NOT_AMBIGUOUS",
                                 "Program run has no pending effect to reconcile");
    }
    const auto                 transition_time = static_cast<std::uint64_t>(now_ms());
    ProgramPendingEffectUpdate update          = [&] {
        if (previous->continuation().state == ContinuationState::Interrupted &&
            resolution.resolution == ProgramEffectReconciliation::Unknown) {
            if (resolution.pending_id != pending->call_id()) {
                return ProgramPendingEffectUpdate{
                    ProgramPendingDisposition::WrongPendingId, *pending, "P_PENDING_ID_MISMATCH",
                    "Submitted call identity does not match the exact pending effect"};
            }
            if (resolution.result) {
                return ProgramPendingEffectUpdate{ProgramPendingDisposition::SchemaMismatch,
                                                  *pending, "P_EFFECT_RECONCILIATION_INVALID",
                                                  "Unknown effect outcome cannot carry a result"};
            }
            return pending->mark_outcome_unknown(transition_time);
        }
        return pending->reconcile(resolution.pending_id, pending->effect_id(),
                                           resolution.resolution, resolution.result, transition_time);
    }();
    if (update.disposition == ProgramPendingDisposition::Duplicate) {
        return reconnect(owner_scope, run_id);
    }
    if (!update.state_changed()) {
        throw_runtime_diagnostic(update.diagnostic_code, update.message);
    }

    const auto checkpoint = *previous->exact_checkpoint();
    const bool completed  = update.value.state() == ProgramPendingState::Consumed &&
                           update.value.reconciliation() == ProgramEffectReconciliation::Completed;
    const bool ambiguous = update.value.state() == ProgramPendingState::Ambiguous;
    const bool expired   = update.value.state() == ProgramPendingState::Expired;
    const bool reconciliation_failed =
        update.value.state() == ProgramPendingState::Consumed &&
        update.value.reconciliation() == ProgramEffectReconciliation::Failed;
    const bool failed = expired || reconciliation_failed;
    if (!completed && !ambiguous && !failed) {
        throw_runtime_diagnostic(update.diagnostic_code, update.message);
    }

    std::shared_ptr<const detail::MaterializedProgram> pinned;
    if (completed) {
        if (previous->recorded_binding_set_fingerprint()) {
            throw_runtime_diagnostic(
                "P_REPLAY_EVIDENCE_REQUIRED",
                "Recorded replay continuation requires the exact owned evidence binding");
        }
        const auto version =
            impl_->config.catalog->resolve_version(owner_scope, previous->program_version_id());
        if (!version) {
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
        }
        pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *version);
        if (pinned->bundle.id() != previous->bundle_id() ||
            capability_binding_receipt_root(
                version->core_materialization_receipt().capability_bindings) !=
                previous->binding_fingerprint() ||
            pinned->root->compiled_plan_identity != checkpoint.core_generation_id ||
            pinned->root->core_name != checkpoint.core_name) {
            throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                     "Program reconciliation checkpoint identity is incompatible");
        }
        const auto stored = impl_->config.checkpoints->load_by_id(checkpoint.checkpoint_id);
        if (!stored || stored->thread_id != checkpoint.core_thread_id ||
            stored->schema_version != checkpoint.checkpoint_schema_version ||
            stored->schema_version != graph::CHECKPOINT_SCHEMA_VERSION) {
            throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                     "Program reconciliation checkpoint is absent or incompatible");
        }
    }

    const auto previous_journal = impl_->config.transitions->latest(owner_scope, run_id);
    if (!previous_journal || previous_journal->id != previous->journal_head()) {
        throw_runtime_diagnostic("P_RECONCILE_CONFLICT",
                                 "Program reconciliation lost the transition CAS");
    }
    const auto next_attempt =
        completed ? previous->continuation().attempt + 1 : previous->continuation().attempt;
    const auto next_state = completed   ? ContinuationState::Running
                            : ambiguous ? ContinuationState::AmbiguousEffect
                                        : ContinuationState::Failed;
    auto       journal    = ProgramJournalRecord::create(ProgramJournalRecordData{
        previous->journal_head(), std::string(run_id), previous->program_version_id(),
        previous->bundle_id(), previous_journal->sequence + 1,
        ProgramContinuation{"root", next_state, next_attempt}, previous->remaining_budget(),
        previous->remaining_budget(), checkpoint, now_ms()});

    std::shared_ptr<detail::RunControl> live_control;
    std::optional<ProgramResult>        terminal;
    std::vector<ProgramEvent>           outbox;
    if (completed) {
        ProgramPersistedInvocation invocation{previous->invocation().input,
                                              previous->remaining_budget(), resolution.trace_id};
        live_control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), std::string(run_id), next_attempt, std::move(pinned),
            previous->binding_fingerprint(), std::move(invocation), checkpoint.core_thread_id,
            previous->event_sequence(), std::move(resolution.events),
            impl_->deadline_pool.get_executor(), impl_->config.checkpoints,
            impl_->config.state_store, impl_->config.transitions);
        outbox.push_back(live_control->stage_event(
            ProgramEventKind::Started, ProgramStartedEvent{previous->remaining_budget()}));
    } else {
        ProgramResultData result_data;
        result_data.status =
            ambiguous ? ProgramTerminalStatus::AmbiguousEffect : ProgramTerminalStatus::Failed;
        result_data.run_id             = std::string(run_id);
        result_data.program_version_id = previous->program_version_id();
        result_data.bundle_id          = previous->bundle_id();
        result_data.operation_id       = "root";
        result_data.attempt            = next_attempt;
        result_data.remaining_budget   = previous->remaining_budget();
        result_data.checkpoint         = checkpoint;
        if (ambiguous) {
            result_data.interrupt =
                ProgramInterrupt{update.value.core_node(), update.value.core_interrupt_value(),
                                 std::nullopt, update.value};
        } else {
            result_data.failure =
                ProgramFailure{expired ? "P_PENDING_EXPIRED" : "P_EFFECT_RECONCILED_FAILED",
                               expired ? "Pending effect expired before a result was accepted"
                                       : "Ambiguous effect was reconciled as failed",
                               "root",
                               update.value.core_node(),
                               0,
                               json{{"effect_id", update.value.effect_id()}}};
        }
        terminal = ProgramResult::create(std::move(result_data));
        const auto status =
            ambiguous ? ProgramTerminalStatus::AmbiguousEffect : ProgramTerminalStatus::Failed;
        outbox.push_back(ProgramEvent::create(ProgramEvent{
            "", previous->event_sequence() + 1, now_ms(), std::string(run_id),
            previous->program_version_id(), previous->bundle_id(), "root",
            checkpoint.core_generation_id, checkpoint.core_thread_id, resolution.trace_id,
            next_attempt, ProgramEventKind::Terminal, ProgramTerminalEvent{status}}));
    }

    ProgramRunRecordData data;
    data.owner_scope                      = std::string(owner_scope);
    data.run_id                           = std::string(run_id);
    data.program_version_id               = previous->program_version_id();
    data.bundle_id                        = previous->bundle_id();
    data.binding_fingerprint              = previous->binding_fingerprint();
    data.invocation                       = previous->invocation();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = previous->remaining_budget();
    data.exact_checkpoint                 = checkpoint;
    data.pending_input                    = previous->pending_input();
    data.pending_effect                   = update.value;
    data.terminal_result                  = terminal;
    data.fork_receipt                     = previous->fork_receipt();
    data.fork_source_run_id               = previous->fork_source_run_id();
    data.fork_source_program_version_id   = previous->fork_source_program_version_id();
    data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
    data.journal_head                     = journal.id;
    data.event_sequence                   = outbox.back().sequence;
    data.effect_sequence                  = previous->effect_sequence();
    data.created_at_ms                    = previous->created_at_ms();
    data.updated_at_ms                    = journal.timestamp_ms;

    const auto published = impl_->config.transitions->compare_publish(
        owner_scope, previous->journal_head(),
        ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal), outbox, {}});
    if (published != ProgramTransitionPublishResult::Published) {
        const auto winner = impl_->config.transitions->load(owner_scope, run_id);
        if (winner && winner->pending_effect()) {
            if (resolution.resolution == ProgramEffectReconciliation::Unknown &&
                !resolution.result &&
                winner->pending_effect()->call_id() == resolution.pending_id &&
                winner->pending_effect()->state() == ProgramPendingState::Ambiguous) {
                return reconnect(owner_scope, run_id);
            }
            const auto duplicate = winner->pending_effect()->reconcile(
                resolution.pending_id, pending->effect_id(), resolution.resolution,
                resolution.result, transition_time);
            if (duplicate.disposition == ProgramPendingDisposition::Duplicate) {
                return reconnect(owner_scope, run_id);
            }
        }
        throw_runtime_diagnostic("P_RECONCILE_CONFLICT",
                                 "Program reconciliation lost the transition CAS");
    }
    if (!completed) return reconnect(owner_scope, run_id);

    impl_->register_control(live_control);
    try {
        live_control->deliver_event(outbox.front());
    } catch (const std::exception& error) {
        detail::RunOutcome failure;
        failure.status           = ProgramTerminalStatus::Failed;
        failure.remaining_budget = previous->remaining_budget();
        failure.checkpoint       = checkpoint;
        failure.failure =
            ProgramFailure{"P_EVENT_SINK", error.what(), "root", "", 0, json::object()};
        live_control->complete(std::move(failure));
        return ProgramHandle(std::move(live_control));
    }
    spawn_run_attempt(impl_->pool, live_control, *resolution.result, checkpoint.checkpoint_id);
    return ProgramHandle(std::move(live_control));
}

asio::awaitable<ProgramResult> ProgramRuntime::run_async(std::string       owner_scope,
                                                         ProgramVersion    version,
                                                         ProgramInvocation invocation) {
    auto               handle = start(owner_scope, version, std::move(invocation));
    co_return co_await handle.wait_async();
}

asio::awaitable<ProgramResult> ProgramRuntime::resume_async(std::string   owner_scope,
                                                            std::string   run_id,
                                                            ProgramResume resume_value) {
    auto               handle = resume(owner_scope, run_id, std::move(resume_value));
    co_return co_await handle.wait_async();
}

ProgramResult ProgramRuntime::run(std::string_view      owner_scope,
                                  const ProgramVersion& version,
                                  ProgramInvocation     invocation) {
    return start(owner_scope, version, std::move(invocation)).wait();
}

}  // namespace neograph::program
