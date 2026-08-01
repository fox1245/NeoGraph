#include <neograph/graph/cancel.h>

#include "core_progress.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace neograph::program::detail {
namespace {

std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return elapsed.count() < 0 ? 0 : static_cast<std::uint64_t>(elapsed.count());
}

std::uint64_t subtract_saturated(std::uint64_t available, std::uint64_t spent) noexcept {
    return spent >= available ? 0 : available - spent;
}

RunBudget settle_budget(const RunBudget& granted, const ProgramUsage& usage) {
    RunBudget remaining    = granted;
    remaining.wall_time_ms = subtract_saturated(granted.wall_time_ms, usage.wall_time_ms);
    remaining.model_tokens = subtract_saturated(granted.model_tokens, usage.model_tokens);
    remaining.monetary_microunits =
        subtract_saturated(granted.monetary_microunits, usage.monetary_microunits);
    remaining.max_concurrency = granted.max_concurrency;
    remaining.max_program_operations =
        subtract_saturated(granted.max_program_operations, usage.program_operations);
    remaining.max_core_steps       = subtract_saturated(granted.max_core_steps, usage.core_steps);
    remaining.max_dynamic_compiles = granted.max_dynamic_compiles;
    remaining.max_child_depth      = granted.max_child_depth;
    remaining.max_total_children   = granted.max_total_children;
    return remaining;
}

std::optional<CoreCheckpointIdentity> inspect_checkpoint(const RunControl& control,
                                                         std::string_view  checkpoint_id) {
    if (checkpoint_id.empty()) return std::nullopt;
    const auto checkpoint = control.checkpoints->load_by_id(std::string(checkpoint_id));
    if (!checkpoint || checkpoint->id != checkpoint_id) return std::nullopt;
    return CoreCheckpointIdentity{
        control.materialized->root->core_name, control.materialized->root->compiled_plan_identity,
        checkpoint->thread_id, checkpoint->id, checkpoint->schema_version};
}

bool checkpoint_matches(const RunControl&             control,
                        const CoreCheckpointIdentity& checkpoint) noexcept {
    return checkpoint.core_name == control.materialized->root->core_name &&
           checkpoint.core_generation_id == control.materialized->root->compiled_plan_identity &&
           checkpoint.core_thread_id == control.core_thread_id &&
           checkpoint.checkpoint_schema_version == graph::CHECKPOINT_SCHEMA_VERSION;
}

bool checkpoint_is_available(const RunControl& control, std::string_view checkpoint_id) noexcept {
    try {
        const auto checkpoint = inspect_checkpoint(control, checkpoint_id);
        return checkpoint && checkpoint_matches(control, *checkpoint);
    } catch (...) {
        return false;
    }
}

std::uint64_t executed_core_steps(const RunControl&                 control,
                                  std::string_view                  final_checkpoint_id,
                                  const std::optional<std::string>& source_checkpoint_id,
                                  graph::RunStatus                  status) {
    if (status == graph::RunStatus::StepLimit) {
        return control.granted_budget.max_core_steps;
    }
    const auto final_checkpoint = control.checkpoints->load_by_id(std::string(final_checkpoint_id));
    if (!final_checkpoint) return control.granted_budget.max_core_steps;

    std::int64_t start_step = 0;
    if (source_checkpoint_id) {
        const auto source = control.checkpoints->load_by_id(*source_checkpoint_id);
        if (!source) return control.granted_budget.max_core_steps;
        if (final_checkpoint_id == *source_checkpoint_id) return 0;
        start_step = source->step;
        if (source->interrupt_phase == graph::CheckpointPhase::After ||
            source->interrupt_phase == graph::CheckpointPhase::Completed ||
            source->interrupt_phase == graph::CheckpointPhase::Updated) {
            ++start_step;
        }
    }
    const auto end_step =
        static_cast<std::int64_t>(final_checkpoint->step) +
        (final_checkpoint->interrupt_phase == graph::CheckpointPhase::Before ? 0 : 1);
    if (end_step < start_step) return control.granted_budget.max_core_steps;
    return static_cast<std::uint64_t>(end_step - start_step);
}

std::uint64_t failed_core_steps(const RunControl& control,
                                std::uint64_t     attempt_core_steps) noexcept {
    return std::min(control.granted_budget.max_core_steps, attempt_core_steps);
}

void apply_checkpoint_incompatible(RunOutcome&                                  outcome,
                                   const std::shared_ptr<RunControl>&           control,
                                   std::string_view                             checkpoint_id,
                                   const std::optional<CoreCheckpointIdentity>& resume_checkpoint,
                                   std::string                                  message) {
    outcome.status  = ProgramTerminalStatus::CheckpointIncompatible;
    outcome.failure = ProgramFailure{"P_CHECKPOINT_INCOMPATIBLE",
                                     std::move(message),
                                     "root",
                                     "",
                                     0,
                                     json{{"checkpoint_id", std::string(checkpoint_id)}}};
    if (resume_checkpoint && resume_checkpoint->checkpoint_id == checkpoint_id &&
        checkpoint_matches(*control, *resume_checkpoint)) {
        outcome.checkpoint = resume_checkpoint;
    } else {
        outcome.checkpoint = CoreCheckpointIdentity{
            control->materialized->root->core_name,
            control->materialized->root->compiled_plan_identity, control->core_thread_id,
            std::string(checkpoint_id), graph::CHECKPOINT_SCHEMA_VERSION};
    }
}

RunOutcome failed_outcome(const std::shared_ptr<RunControl>& control,
                          std::string                        code,
                          std::string                        message,
                          std::string                        core_node = {},
                          std::uint32_t                      attempts  = 0) {
    RunOutcome outcome;
    outcome.status           = ProgramTerminalStatus::Failed;
    outcome.remaining_budget = control->granted_budget;
    outcome.failure          = ProgramFailure{std::move(code),      std::move(message), "root",
                                     std::move(core_node), attempts,           json::object()};
    return outcome;
}
std::uint64_t model_tokens(const std::shared_ptr<UsageAccumulator>& usage) noexcept {
    const auto total = usage->total_tokens_wide();
    return total <= 0 ? 0 : static_cast<std::uint64_t>(total);
}


bool apply_terminal_cause(RunOutcome& outcome, CancellationCause cause, std::string_view message) {
    if (cause == CancellationCause::None) return false;
    if (cause == CancellationCause::EventSink) {
        outcome.status = ProgramTerminalStatus::Failed;
        outcome.failure =
            ProgramFailure{"P_EVENT_SINK", std::string(message), "root", "", 0, json::object()};
        return true;
    }
    outcome.status  = cause == CancellationCause::Timeout ? ProgramTerminalStatus::TimedOut
                                                          : ProgramTerminalStatus::Cancelled;
    outcome.failure = ProgramFailure{
        cause == CancellationCause::Timeout ? "P_RUNTIME_TIMEOUT" : "P_RUNTIME_CANCELLED",
        std::string(message),
        "root",
        "",
        0,
        json::object()};
    return true;
}

CancellationCause terminal_cause_at_deadline(const std::shared_ptr<RunControl>&    control,
                                             std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        control->cancel(CancellationCause::Timeout);
    }
    return control->seal_terminal_cause();
}

void cancel_deadline_timer(const asio::any_io_executor&        executor,
                           std::shared_ptr<asio::steady_timer> timer) {
    asio::post(executor, [timer = std::move(timer)] {
        try {
            timer->cancel();
        } catch (...) {}
    });
}

constexpr std::string_view PROGRAM_PENDING_KIND_FIELD = "__neograph_program_pending_kind";

ProgramInterrupt decode_core_interrupt(const RunControl& control, const graph::RunResult& result) {
    const auto generic = [&] {
        ProgramPendingInputData pending;
        pending.operation_id         = control.operation_id;
        pending.call_id              = control.run_id + ":" + control.operation_id + ":" +
                          std::to_string(control.attempt);
        pending.kind                 = ProgramPendingInputKind::Input;
        pending.result_schema        = json::object();
        pending.payload              = result.interrupt_value;
        pending.core_node            = result.interrupt_node;
        pending.core_interrupt_value = result.interrupt_value;
        return ProgramInterrupt{result.interrupt_node, result.interrupt_value,
                                ProgramPendingInput(std::move(pending)), std::nullopt};
    };

    if (!result.interrupt_value.is_object() ||
        !result.interrupt_value.contains("value") ||
        !result.interrupt_value.at("value").is_object()) {
        return generic();
    }
    auto pending_payload = result.interrupt_value.at("value");
    if (!pending_payload.contains(std::string(PROGRAM_PENDING_KIND_FIELD)) ||
        !pending_payload.at(std::string(PROGRAM_PENDING_KIND_FIELD)).is_string() ||
        !pending_payload.contains("call_id") || !pending_payload.at("call_id").is_string() ||
        !pending_payload.contains("result_schema") ||
        !pending_payload.at("result_schema").is_object()) {
        return generic();
    }

    const auto kind = pending_payload.at(std::string(PROGRAM_PENDING_KIND_FIELD))
                          .get<std::string>();
    const auto expiry = [&]() -> std::optional<std::uint64_t> {
        if (!pending_payload.contains("expires_at_unix_ms")) return std::nullopt;
        const auto& value = pending_payload.at("expires_at_unix_ms");
        if (!value.is_number_unsigned()) {
            throw std::invalid_argument(
                "Typed Program pending expires_at_unix_ms must be unsigned");
        }
        return value.get<std::uint64_t>();
    }();

    if (kind == "effect") {
        if (!pending_payload.contains("effect") || !pending_payload.at("effect").is_object()) {
            throw std::invalid_argument("Typed Program pending effect descriptor is missing");
        }
        const auto& effect = pending_payload.at("effect");
        ProgramPendingEffectData pending;
        pending.operation_id         = control.operation_id;
        pending.call_id              = pending_payload.at("call_id").get<std::string>();
        pending.effect_id            = effect.at("effect_id").get<std::string>();
        pending.result_schema        = pending_payload.at("result_schema");
        pending.payload              = pending_payload;
        pending.expires_at_unix_ms   = expiry;
        pending.effect_mode          = EffectMode::Brokered;
        pending.idempotency          =
            effect.value("idempotency", "unsupported") == "supported"
                ? ProgramEffectIdempotency::Idempotent
                : ProgramEffectIdempotency::NonIdempotent;
        pending.core_node            = result.interrupt_node;
        pending.core_interrupt_value = result.interrupt_value;
        return ProgramInterrupt{result.interrupt_node, result.interrupt_value, std::nullopt,
                                ProgramPendingEffect(std::move(pending))};
    }
    if (kind != "input" && kind != "capability_result") {
        throw std::invalid_argument("Unknown typed Program pending kind");
    }
    ProgramPendingInputData pending;
    pending.operation_id         = control.operation_id;
    pending.call_id              = pending_payload.at("call_id").get<std::string>();
    pending.kind                 = kind == "capability_result"
                                       ? ProgramPendingInputKind::CapabilityResult
                                       : ProgramPendingInputKind::Input;
    pending.result_schema        = pending_payload.at("result_schema");
    pending.payload              = pending_payload;
    pending.expires_at_unix_ms   = expiry;
    pending.core_node            = result.interrupt_node;
    pending.core_interrupt_value = result.interrupt_value;
    return ProgramInterrupt{result.interrupt_node, result.interrupt_value,
                            ProgramPendingInput(std::move(pending)), std::nullopt};
}

}  // namespace

asio::awaitable<void> execute_run_attempt(std::shared_ptr<RunControl> control,
                                          json                        input,
                                          std::optional<std::string>  checkpoint_id) {
    const auto started_at    = control->started_at;
    const auto deadline      = control->deadline;
    auto       usage         = std::make_shared<UsageAccumulator>();
    auto       core_progress = std::make_shared<AttemptCoreProgress>();
    if (std::chrono::steady_clock::now() >= deadline) {
        control->cancel(CancellationCause::Timeout);
    }
    auto timer = std::make_shared<asio::steady_timer>(control->deadline_executor);
    timer->expires_at(deadline);
    asio::co_spawn(
        control->deadline_executor,
        [control, timer]() -> asio::awaitable<void> {
            asio::error_code error;
            co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (!error) control->cancel(CancellationCause::Timeout);
            co_return;
        },
        asio::detached);

    std::optional<CoreCheckpointIdentity> resume_checkpoint;
    RunOutcome                            outcome;
    try {
        if (checkpoint_id) {
            resume_checkpoint = inspect_checkpoint(*control, *checkpoint_id);
            if (!resume_checkpoint || !checkpoint_matches(*control, *resume_checkpoint)) {
                outcome.remaining_budget = control->granted_budget;
                apply_checkpoint_incompatible(
                    outcome, control, *checkpoint_id, resume_checkpoint,
                    "Journal-published Core checkpoint changed after resume admission");
                cancel_deadline_timer(control->deadline_executor, std::move(timer));
                control->complete(std::move(outcome));
                co_return;
            }
        }
        graph::RunConfig config;
        config.thread_id = control->core_thread_id;
        config.input     = checkpoint_id ? json::object() : std::move(input);
        config.max_steps = static_cast<int>(
            std::min<std::uint64_t>(control->granted_budget.max_core_steps,
                                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
        config.stream_mode  = graph::StreamMode::ALL;
        config.cancel_token = control->cancel_token;
        config.usage        = usage;
        config.model_token_budget = control->granted_budget.model_tokens;
        config.budget_exhausted   = control->budget_exhausted;

        graph::RunMetadata metadata;
        metadata.deadline = deadline;
        metadata.run_id = control->run_id;
        metadata.trace_id = control->trace_id;
        graph::RunResources        resources{control->checkpoints, control->state_store};
        graph::GraphStreamCallback callback = [control,
                                               core_progress](const graph::GraphEvent& event) {
            core_progress->observe(event);
            control->emit(ProgramEventKind::Core, graph::to_typed_event(event));
        };

        graph::RunResult core_result;
        if (checkpoint_id) {
            core_result = co_await control->materialized->root->engine->resume_from_async(
                std::move(config), *checkpoint_id, std::move(input), std::move(callback),
                std::move(metadata), std::move(resources));
        } else {
            core_result = co_await control->materialized->root->engine->run_stream_async(
                std::move(config), std::move(callback), std::move(metadata), std::move(resources));
        }

        const auto core_status     = core_result.status();
        const auto terminal_cause  = terminal_cause_at_deadline(control, deadline);
        outcome.output             = std::move(core_result.output);
        outcome.execution_trace    = std::move(core_result.execution_trace);
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.core_steps =
            executed_core_steps(*control, core_result.checkpoint_id, checkpoint_id, core_status);
        outcome.usage.peak_concurrency = 1;
        outcome.remaining_budget       = settle_budget(control->granted_budget, outcome.usage);
        outcome.checkpoint             = inspect_checkpoint(*control, core_result.checkpoint_id);

        if (apply_terminal_cause(outcome, terminal_cause, "Program attempt was cancelled")) {
        } else if (!outcome.checkpoint || !checkpoint_matches(*control, *outcome.checkpoint)) {
            outcome.status  = ProgramTerminalStatus::CheckpointIncompatible;
            outcome.failure = ProgramFailure{"P_CHECKPOINT_INCOMPATIBLE",
                                             "Core returned an absent or incompatible checkpoint",
                                             "root",
                                             core_result.interrupt_node,
                                             0,
                                             json{{"checkpoint_id", core_result.checkpoint_id}}};
            if (!outcome.checkpoint && checkpoint_id) {
                outcome.checkpoint = CoreCheckpointIdentity{
                    control->materialized->root->core_name,
                    control->materialized->root->compiled_plan_identity, control->core_thread_id,
                    *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
            }
        } else if (core_status == graph::RunStatus::Interrupted) {
            outcome.status    = ProgramTerminalStatus::Interrupted;
            outcome.interrupt = decode_core_interrupt(*control, core_result);
        } else if (core_status == graph::RunStatus::StepLimit ||
                   outcome.usage.core_steps > control->granted_budget.max_core_steps ||
                   outcome.usage.model_tokens > control->granted_budget.model_tokens ||
                   outcome.usage.wall_time_ms > control->granted_budget.wall_time_ms) {
            outcome.status = ProgramTerminalStatus::BudgetExhausted;
        } else {
            try {
                validate_contract_value(outcome.output,
                                        control->materialized->bundle.output_contract());
                outcome.status = ProgramTerminalStatus::Completed;
            } catch (const std::exception& error) {
                outcome.status  = ProgramTerminalStatus::Failed;
                outcome.failure = ProgramFailure{
                    "P_OUTPUT_CONTRACT",
                    "Program output violates its admitted contract",
                    "root",
                    "",
                    0,
                    json{{"detail", error.what()}}};
            }
        }
    } catch (const EventSinkError& error) {
        outcome                    = failed_outcome(control, "P_EVENT_SINK", error.what());
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = 1;
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
    } catch (const graph::CancelledException& error) {
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = 1;
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        auto cause                       = terminal_cause_at_deadline(control, deadline);
        if (cause == CancellationCause::None) cause = CancellationCause::User;
        apply_terminal_cause(outcome, cause, error.what());
    } catch (const graph::NodeExecutionError& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(), error.node_name(),
                                 static_cast<std::uint32_t>(error.attempts()));
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = 1;
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
    } catch (const std::exception& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what());
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = 1;
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        const auto cause                 = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(outcome, cause, error.what()) && checkpoint_id &&
            !checkpoint_is_available(*control, *checkpoint_id)) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
    } catch (...) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", "Unknown Core failure");
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = 1;
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline),
                             "Unknown Core failure");
    }

    if (control->budget_exhausted->load(std::memory_order_acquire) &&
        control->cancellation_cause() == CancellationCause::None) {
        outcome.status = ProgramTerminalStatus::BudgetExhausted;
        outcome.failure.reset();
    }

    if (!outcome.checkpoint && checkpoint_id) {
        if (resume_checkpoint && resume_checkpoint->checkpoint_id == *checkpoint_id &&
            checkpoint_matches(*control, *resume_checkpoint)) {
            outcome.checkpoint = resume_checkpoint;
        } else {
            outcome.checkpoint = CoreCheckpointIdentity{
                control->materialized->root->core_name,
                control->materialized->root->compiled_plan_identity, control->core_thread_id,
                *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
        }
    }

    cancel_deadline_timer(control->deadline_executor, std::move(timer));

    control->complete(std::move(outcome));
    co_return;
}

}  // namespace neograph::program::detail
