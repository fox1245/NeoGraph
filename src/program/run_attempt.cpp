#include <neograph/program/diagnostic.h>
#include <neograph/graph/cancel.h>

#include "canonical_json.h"
#include "core_progress.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/bind_executor.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <mutex>

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

std::optional<CoreCheckpointIdentity> inspect_checkpoint_for(const RunControl& control,
                                                             std::string_view  checkpoint_id,
                                                             std::string_view  thread_id) {
    if (checkpoint_id.empty()) return std::nullopt;
    const auto checkpoint = control.checkpoints->load_by_id(std::string(checkpoint_id));
    if (!checkpoint || checkpoint->id != checkpoint_id) return std::nullopt;
    return CoreCheckpointIdentity{
        control.materialized->root->core_name, control.materialized->root->compiled_plan_identity,
        std::string(thread_id), checkpoint->id, checkpoint->schema_version};
}

std::string operation_thread_id(const RunControl& control, std::string_view operation_id) {
    if (operation_id == "root") return control.core_thread_id;
    std::string value(control.run_id);
    value.push_back('\0');
    value.append(operation_id);
    value.push_back('\0');
    value.append(control.materialized->root->compiled_plan_identity);
    return detail::sha256_identity("program-core-thread/v1", value);
}

std::optional<json> json_path_value(const json& value, std::string_view path) {
    if (path.empty()) return value;
    if (path.front() != '/') return std::nullopt;
    json current = value;
    std::size_t begin = 1;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        auto       part = std::string(path.substr(begin, end == std::string_view::npos
                                                           ? path.size() - begin
                                                           : end - begin));
        std::string decoded;
        decoded.reserve(part.size());
        for (std::size_t index = 0; index < part.size(); ++index) {
            if (part[index] == '~' && index + 1 < part.size()) {
                if (part[index + 1] == '0') {
                    decoded.push_back('~');
                    ++index;
                    continue;
                }
                if (part[index + 1] == '1') {
                    decoded.push_back('/');
                    ++index;
                    continue;
                }
            }
            decoded.push_back(part[index]);
        }
        try {
            if (current.is_object()) {
                if (!current.contains(decoded)) return std::nullopt;
                current = current.at(decoded);
            } else if (current.is_array()) {
                if (decoded.empty()) return std::nullopt;
                std::size_t consumed = 0;
                const auto  index    = std::stoull(decoded, &consumed);
                current = current.at(static_cast<std::size_t>(index));
            } else {
                return std::nullopt;
            }
        } catch (...) {
            return std::nullopt;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return current;
}

bool condition_matches(const json& state, const json& condition) {
    if (!condition.is_object()) return false;
    if (condition.contains("all")) {
        const auto values = condition["all"];
        if (!values.is_array() || values.empty()) return false;
        for (const auto& child : values)
            if (!condition_matches(state, child)) return false;
        return true;
    }
    if (condition.contains("any")) {
        const auto values = condition["any"];
        if (!values.is_array() || values.empty()) return false;
        for (const auto& child : values)
            if (condition_matches(state, child)) return true;
        return false;
    }
    if (condition.contains("not")) return !condition_matches(state, condition["not"]);
    if (!condition.contains("path") || !condition["path"].is_string()) return false;
    const auto found = json_path_value(state, condition["path"].get<std::string>());
    if (condition.contains("exists"))
        return found.has_value() == condition["exists"].get<bool>();
    if (condition.contains("equals")) return found && *found == condition["equals"];
    if (condition.contains("not_equals")) return !found || *found != condition["not_equals"];
    return false;
}

struct PlanExecution {
    ProgramTerminalStatus           status = ProgramTerminalStatus::Completed;
    json                            output = json::object();
    std::shared_ptr<RunControl>     spawned_child;
    std::optional<ProgramInterrupt> interrupt;
    std::optional<ProgramFailure>   failure;
    std::vector<std::string>        execution_trace;
    bool                            returned = false;
};

void append_trace(PlanExecution& target, PlanExecution&& source) {
    target.output = std::move(source.output);
    target.spawned_child = std::move(source.spawned_child);
    target.execution_trace.insert(target.execution_trace.end(),
                                  std::make_move_iterator(source.execution_trace.begin()),
                                  std::make_move_iterator(source.execution_trace.end()));
    target.returned = target.returned || source.returned;
    if (source.status != ProgramTerminalStatus::Completed) {
        target.status    = source.status;
        target.interrupt = std::move(source.interrupt);
        target.failure   = std::move(source.failure);
    }
}

PlanExecution plan_failure(ProgramTerminalStatus status,
                           std::string          code,
                           std::string          message,
                           std::string          operation_id) {
    PlanExecution result;
    result.status  = status;
    result.failure = ProgramFailure{std::move(code), std::move(message),
                                    std::move(operation_id), "", 0, json::object()};
    return result;
}

PlanExecution plan_result_from_child(const ProgramResult& child_result) {
    PlanExecution result;
    result.status          = child_result.status();
    result.output          = child_result.output();
    result.interrupt       = child_result.interrupt();
    result.failure         = child_result.failure();
    result.execution_trace = child_result.execution_trace();
    return result;
}
class NestedOperationFailure final : public std::runtime_error {
public:
    NestedOperationFailure(std::string operation_id,
                           std::string message,
                           std::string core_node = {},
                           std::uint32_t attempts = 0,
                           bool checkpoint_incompatible = false)
        : std::runtime_error(std::move(message)),
          operation_id(std::move(operation_id)),
          core_node(std::move(core_node)),
          attempts(attempts),
          checkpoint_incompatible(checkpoint_incompatible) {}

    std::string   operation_id;
    std::string   core_node;
    std::uint32_t attempts = 0;
    bool          checkpoint_incompatible = false;
};

[[noreturn]] void throw_runtime_diagnostic(std::string code,
                                           std::string message,
                                           json        witness = json::object()) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = std::move(code);
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"runtime", "", std::nullopt};
    diagnostic.message  = std::move(message);
    diagnostic.witness  = std::move(witness);
    throw ProgramDiagnosticError(std::move(diagnostic));
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
        if (checkpoint_id.empty()) return false;
        const auto loaded = control.checkpoints->load_by_id(std::string(checkpoint_id));
        if (!loaded) {
            // A transient read miss after the Core engine has consumed the
            // checkpoint is still a Core failure, not an identity mismatch.
            const auto latest = control.checkpoints->load_latest(control.core_thread_id);
            return latest && latest->id == checkpoint_id;
        }
        if (loaded->id != checkpoint_id) return false;
        const CoreCheckpointIdentity identity{
            control.materialized->root->core_name,
            control.materialized->root->compiled_plan_identity,
            loaded->thread_id,
            loaded->id,
            loaded->schema_version};
        return checkpoint_matches(control, identity);
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
asio::awaitable<void> cancel_after_timer(
    std::shared_ptr<asio::steady_timer> timer,
    std::shared_ptr<graph::CancelToken> token) {
    asio::error_code wait_error;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (!wait_error) token->cancel();
}


constexpr std::string_view PROGRAM_PENDING_KIND_FIELD = "__neograph_program_pending_kind";

ProgramInterrupt decode_core_interrupt(const RunControl&      control,
                                       std::string_view       operation_id,
                                       const graph::RunResult& result) {
    const auto generic = [&] {
        ProgramPendingInputData pending;
        pending.operation_id = std::string(operation_id);
        pending.call_id      = control.run_id + ":" + std::string(operation_id) + ":" +
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
        pending.operation_id         = std::string(operation_id);
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
    pending.operation_id         = std::string(operation_id);
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
    const auto started_at = control->started_at;
    const auto deadline   = control->deadline;
    auto       usage      = std::make_shared<UsageAccumulator>();
    auto       core_progress = std::make_shared<AttemptCoreProgress>();
    std::uint64_t operation_count = 0;
    std::atomic<std::size_t> active_concurrency{0};
    std::atomic<std::size_t> peak_concurrency{0};
    if (std::chrono::steady_clock::now() >= deadline) control->cancel(CancellationCause::Timeout);

    auto timer = std::make_shared<asio::steady_timer>(control->deadline_executor);
    timer->expires_at(deadline);
    asio::co_spawn(
        control->deadline_executor,
        [control, timer]() -> asio::awaitable<void> {
            asio::error_code error;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (!error) control->cancel(CancellationCause::Timeout);
            co_return;
        },
        asio::detached);

    std::optional<CoreCheckpointIdentity> resume_checkpoint;
    RunOutcome                            outcome;
    try {
        if (checkpoint_id) {
            resume_checkpoint = inspect_checkpoint(*control, *checkpoint_id);
            const bool resume_valid =
                resume_checkpoint && checkpoint_matches(*control, *resume_checkpoint);
            if (!resume_valid) {
                outcome.remaining_budget = control->granted_budget;
                apply_checkpoint_incompatible(
                    outcome, control, *checkpoint_id, resume_checkpoint,
                    "Journal-published Core checkpoint changed after resume admission");
                cancel_deadline_timer(control->deadline_executor, std::move(timer));
                control->complete(std::move(outcome));
                co_return;
            }
        }

        const auto& typed_plan = control->materialized->bundle.typed_orchestration_plan();
        if (typed_plan.schema_version() != ProgramPlan::SCHEMA_VERSION)
            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program plan schema is unsupported");
        const auto root_id = typed_plan.root_id();
        if (!typed_plan.find(root_id))
            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program root operation is missing");

        std::mutex plan_mutex;
        std::optional<std::string> resume_id = checkpoint_id;
        std::optional<CoreCheckpointIdentity> last_root_checkpoint;
        std::map<std::string, std::uint64_t> spawn_occurrences;
        auto root_checkpoint = [&]() -> std::optional<CoreCheckpointIdentity> {
            std::lock_guard lock(plan_mutex);
            return last_root_checkpoint;
        };

        struct CoreInvocation {
            graph::RunResult                    result;
            std::optional<CoreCheckpointIdentity> checkpoint;
        };

        auto update_peak = [&](std::size_t current) {
            auto previous = peak_concurrency.load(std::memory_order_relaxed);
            while (previous < current &&
                   !peak_concurrency.compare_exchange_weak(previous, current,
                                                           std::memory_order_relaxed)) {}
        };
        auto charge_operation = [&](std::string_view operation_id)
            -> std::optional<PlanExecution> {
            std::lock_guard lock(plan_mutex);
            if (operation_count >= control->granted_budget.max_program_operations)
                return plan_failure(ProgramTerminalStatus::BudgetExhausted,
                                    "P_PROGRAM_OPERATION_BUDGET",
                                    "Program operation budget exhausted",
                                    std::string(operation_id));
            ++operation_count;
            return std::nullopt;
        };

        auto run_core = [&](std::string operation_id,
                            json        core_input,
                            std::optional<std::string> resume,
                            std::string thread_id,
                            std::shared_ptr<graph::CancelToken> operation_token)
            -> asio::awaitable<CoreInvocation> {
            graph::RunConfig config;
            config.thread_id = thread_id;
            config.input = resume ? json::object() : core_input;
            config.max_steps = static_cast<int>(
                std::min<std::uint64_t>(control->granted_budget.max_core_steps,
                                        static_cast<std::uint64_t>(
                                            std::numeric_limits<int>::max())));
            config.stream_mode = graph::StreamMode::ALL;
            config.cancel_token = std::move(operation_token);
            config.usage = usage;
            config.model_token_budget = control->granted_budget.model_tokens;
            config.budget_exhausted = control->budget_exhausted;

            graph::RunMetadata metadata;
            metadata.deadline = deadline;
            metadata.run_id = control->run_id;
            metadata.trace_id = control->trace_id;
            graph::RunResources resources{control->checkpoints, control->state_store};
            graph::GraphStreamCallback callback =
                [control, core_progress, operation_id](const graph::GraphEvent& event) {
                    core_progress->observe(event);
                    control->emit(operation_id, ProgramEventKind::Core,
                                  graph::to_typed_event(event));
                };
            graph::RunResult result;
            if (resume) {
                result = co_await control->materialized->root->engine->resume_from_async(
                    std::move(config), *resume, std::move(core_input), std::move(callback),
                    std::move(metadata), std::move(resources));
            } else {
                result = co_await control->materialized->root->engine->run_stream_async(
                    std::move(config), std::move(callback), std::move(metadata),
                    std::move(resources));
            }
            auto checkpoint = inspect_checkpoint_for(*control, result.checkpoint_id, thread_id);
            if (checkpoint && checkpoint->core_thread_id != thread_id) checkpoint.reset();
            co_return CoreInvocation{std::move(result), std::move(checkpoint)};
        };

        std::function<asio::awaitable<PlanExecution>(
            std::string, json, std::string, std::uint32_t,
            std::shared_ptr<graph::CancelToken>)> execute;
        execute = [&](std::string                         operation_id,
                      json                                state,
                      std::string                         thread_id,
                      std::uint32_t                       child_depth,
                      std::shared_ptr<graph::CancelToken> operation_token)
            -> asio::awaitable<PlanExecution> {
            const auto* found = typed_plan.find(operation_id);
            if (!found) {
                throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                         "Program operation reference is dangling",
                                         json{{"operation_id", operation_id}});
            }
            const auto& operation = *found;
            const auto& dispatch  = operation.dispatch();
            const auto op         = dispatch.operation;
            if (operation_token->is_cancelled()) {
                co_return plan_failure(ProgramTerminalStatus::Cancelled,
                                       "P_RUNTIME_CANCELLED",
                                       "Program operation cancelled before dispatch",
                                       operation_id);
            }
            if (op != ProgramOperationKind::CallCore) {
                if (auto failure = charge_operation(operation_id))
                    co_return std::move(*failure);
            }
            if (op == ProgramOperationKind::CallCore) {
                std::optional<std::string> resume;
                {
                    std::lock_guard lock(plan_mutex);
                    if (resume_id) {
                        resume = std::move(resume_id);
                        resume_id.reset();
                    } else {
                        if (operation_count >= control->granted_budget.max_program_operations)
                            co_return plan_failure(
                                ProgramTerminalStatus::BudgetExhausted, "P_PROGRAM_OPERATION_BUDGET",
                                "Program operation budget exhausted", operation_id);
                        ++operation_count;
                    }
                }
                // Concurrency is a run-wide resource, not merely a property of
                // the immediate parallel node.  Nested parallel/race plans can
                // otherwise pass their local branch-count checks and dispatch
                // more Core calls than the admitted grant.  Reserve the slot
                // immediately before the sole GraphEngine dispatch and release
                // it on every completion path.
                auto active = active_concurrency.load(std::memory_order_relaxed);
                while (active < control->granted_budget.max_concurrency &&
                       !active_concurrency.compare_exchange_weak(
                           active, active + 1, std::memory_order_relaxed,
                           std::memory_order_relaxed)) {}
                if (active >= control->granted_budget.max_concurrency)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                        "Program Core dispatch exceeds its admitted concurrency budget",
                        operation_id);
                update_peak(active + 1);
                CoreInvocation invocation;
                const auto resume_checkpoint_for_error = resume;
                try {
                    invocation = co_await run_core(operation_id, std::move(state),
                                                   std::move(resume), std::move(thread_id),
                                                   operation_token);
                } catch (const EventSinkError&) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    throw;
                } catch (const graph::CancelledException&) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    throw;
                } catch (const graph::NodeExecutionError& error) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    const bool checkpoint_incompatible =
                        resume_checkpoint_for_error &&
                        !checkpoint_is_available(*control, *resume_checkpoint_for_error);
                    throw NestedOperationFailure(
                        operation_id, error.what(), error.node_name(),
                        static_cast<std::uint32_t>(error.attempts()), checkpoint_incompatible);
                } catch (const std::exception& error) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    const bool checkpoint_incompatible =
                        resume_checkpoint_for_error &&
                        !checkpoint_is_available(*control, *resume_checkpoint_for_error);
                    throw NestedOperationFailure(operation_id, error.what(), {}, 0,
                                                 checkpoint_incompatible);
                } catch (...) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    const bool checkpoint_incompatible =
                        resume_checkpoint_for_error &&
                        !checkpoint_is_available(*control, *resume_checkpoint_for_error);
                    throw NestedOperationFailure(operation_id, "Unknown Core failure", {}, 0,
                                                 checkpoint_incompatible);
                }
                active_concurrency.fetch_sub(1, std::memory_order_relaxed);

                const auto core_status = invocation.result.status();
                PlanExecution result;
                result.output = std::move(invocation.result.output);
                result.execution_trace = std::move(invocation.result.execution_trace);
                if (invocation.checkpoint &&
                    invocation.checkpoint->core_thread_id == control->core_thread_id) {
                    std::lock_guard lock(plan_mutex);
                    last_root_checkpoint = invocation.checkpoint;
                }
                if (core_status == graph::RunStatus::Interrupted) {
                    result.status = ProgramTerminalStatus::Interrupted;
                    result.interrupt =
                        decode_core_interrupt(*control, operation_id, invocation.result);
                } else if (core_status == graph::RunStatus::StepLimit) {
                    result.status = ProgramTerminalStatus::BudgetExhausted;
                    result.failure = ProgramFailure{
                        "P_CORE_STEP_BUDGET", "Core step budget exhausted", operation_id,
                        invocation.result.interrupt_node, 0, json::object()};
                }
                co_return result;
            }
            if (op == ProgramOperationKind::Sequence) {
                PlanExecution result;
                result.output = std::move(state);
                for (const auto& child : dispatch.children) {
                    auto child_result =
                        co_await execute(child, result.output, thread_id,
                                         child_depth, operation_token);
                    append_trace(result, std::move(child_result));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                }
                co_return result;
            }

            if (op == ProgramOperationKind::Branch) {
                const auto selected = condition_matches(state, operation.condition())
                                          ? dispatch.then_id
                                          : dispatch.else_id;
                if (!selected) {
                    PlanExecution result;
                    result.output = std::move(state);
                    co_return result;
                }
                co_return co_await execute(*selected,
                                           std::move(state), std::move(thread_id), child_depth,
                                           operation_token);
            }

            if (op == ProgramOperationKind::Loop) {
                PlanExecution result;
                result.output = std::move(state);
                const auto maximum = *operation.max_iterations();
                for (std::uint64_t iteration = 0;
                     iteration < maximum &&
                     condition_matches(result.output, operation.condition());
                     ++iteration) {
                    auto body = co_await execute(*dispatch.body,
                                                 result.output, thread_id, child_depth,
                                                 operation_token);
                    append_trace(result, std::move(body));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                }
                if (condition_matches(result.output, operation.condition())) {
                    auto bounded = plan_failure(ProgramTerminalStatus::BudgetExhausted,
                                                "P_LOOP_BOUND", "Program loop bound exhausted",
                                                operation_id);
                    bounded.output          = std::move(result.output);
                    bounded.execution_trace = std::move(result.execution_trace);
                    result                  = std::move(bounded);
                }
                co_return result;
            }

            if (op == ProgramOperationKind::Retry) {
                const auto maximum = *operation.max_attempts();
                std::optional<PlanExecution> last_result;
                for (std::uint64_t attempt = 0; attempt < maximum; ++attempt) {
                    try {
                        auto attempt_result = co_await execute(
                            *dispatch.body, state, thread_id, child_depth,
                            operation_token);
                        if (attempt_result.status == ProgramTerminalStatus::Completed)
                            co_return attempt_result;
                        if (attempt_result.status == ProgramTerminalStatus::Interrupted ||
                            attempt_result.status == ProgramTerminalStatus::Cancelled ||
                            attempt_result.status == ProgramTerminalStatus::TimedOut ||
                            attempt_result.status == ProgramTerminalStatus::BudgetExhausted)
                            co_return attempt_result;
                        last_result = std::move(attempt_result);
                    } catch (const graph::CancelledException&) {
                        throw;
                    } catch (const NestedOperationFailure& error) {
                        if (error.checkpoint_incompatible) throw;
                        if (attempt + 1 == maximum) {
                            auto terminal = plan_failure(
                                ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                error.what(), error.operation_id);
                            terminal.failure->core_node = error.core_node;
                            terminal.failure->attempts = static_cast<std::uint32_t>(attempt + 1);
                            co_return terminal;
                        }
                    } catch (const graph::NodeExecutionError& error) {
                        if (attempt + 1 == maximum) {
                            auto terminal = plan_failure(
                                ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                error.what(), operation_id);
                            terminal.failure->core_node = error.node_name();
                            terminal.failure->attempts = static_cast<std::uint32_t>(attempt + 1);
                            co_return terminal;
                        }
                    } catch (const std::exception& error) {
                        if (attempt + 1 == maximum) {
                            auto terminal = plan_failure(
                                ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                error.what(), operation_id);
                            terminal.failure->attempts = static_cast<std::uint32_t>(attempt + 1);
                            co_return terminal;
                        }
                    }
                }
                if (last_result) co_return std::move(*last_result);
                co_return plan_failure(ProgramTerminalStatus::Failed, "P_RETRY_BOUND",
                                       "Program retry operation had no attempts", operation_id);
            }

            if (op == ProgramOperationKind::Parallel) {
                const auto& branches = dispatch.branches;
                if (branches.size() > control->granted_budget.max_concurrency)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                        "Program parallel fan-out exceeds its admitted concurrency budget",
                        operation_id);

                struct ParallelState {
                    std::mutex                                      mutex;
                    std::vector<std::optional<PlanExecution>>       results;
                    std::vector<std::shared_ptr<graph::CancelToken>> branch_tokens;
                    std::exception_ptr                              error;
                    std::size_t                                     remaining = 0;
                    std::shared_ptr<asio::steady_timer>             timer;
                };
                const auto executor = co_await asio::this_coro::executor;
                const auto completion_executor = asio::make_strand(executor);
                auto       parallel = std::make_shared<ParallelState>();
                parallel->results.resize(branches.size());
                parallel->remaining = branches.size();
                parallel->timer = std::make_shared<asio::steady_timer>(completion_executor);
                parallel->timer->expires_at((asio::steady_timer::time_point::max)());
                parallel->branch_tokens.reserve(branches.size());
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    parallel->branch_tokens.push_back(operation_token->fork());
                }
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    const auto& branch_id = branches[index];
                    const auto branch_thread =
                        index == 0 ? control->core_thread_id
                                   : operation_thread_id(*control, branch_id);
                    auto branch_token = parallel->branch_tokens[index];
                    asio::co_spawn(
                        executor,
                        execute(branch_id, state, branch_thread, child_depth, branch_token),
                        asio::bind_executor(
                            completion_executor,
                            [parallel, index](std::exception_ptr error, PlanExecution result) {
                                bool cancel_siblings = false;
                                bool all_done = false;
                                {
                                    std::lock_guard lock(parallel->mutex);
                                    if (error) {
                                        parallel->error = error;
                                        cancel_siblings = true;
                                    } else {
                                        parallel->results[index] = std::move(result);
                                    }
                                    all_done = --parallel->remaining == 0;
                                }
                                if (cancel_siblings) {
                                    for (const auto& token : parallel->branch_tokens)
                                        if (token) token->cancel();
                                }
                                if (all_done) parallel->timer->cancel();
                            }));
                }
                co_await asio::co_spawn(
                    completion_executor,
                    [parallel]() -> asio::awaitable<void> {
                        asio::error_code error;
                        co_await parallel->timer->async_wait(
                            asio::redirect_error(asio::use_awaitable, error));
                    },
                    asio::use_awaitable);

                std::exception_ptr                        parallel_error;
                std::vector<std::optional<PlanExecution>> parallel_results;
                {
                    std::lock_guard lock(parallel->mutex);
                    parallel_error = parallel->error;
                    parallel_results = std::move(parallel->results);
                }
                if (parallel_error) std::rethrow_exception(parallel_error);

                PlanExecution result;
                result.output = json::array();
                for (auto& branch : parallel_results) {
                    if (!branch)
                        throw_runtime_diagnostic(
                            "P_RUNTIME_PARALLEL", "Parallel branch completed without a result");
                    result.output.push_back(branch->output);
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(branch->execution_trace.begin()),
                        std::make_move_iterator(branch->execution_trace.end()));
                    if (branch->returned) result.returned = true;
                    if (result.status == ProgramTerminalStatus::Completed &&
                        branch->status != ProgramTerminalStatus::Completed) {
                        result.status = branch->status;
                        result.interrupt = std::move(branch->interrupt);
                        result.failure = std::move(branch->failure);
                    }
                }
                co_return result;
            }

            if (op == ProgramOperationKind::Race) {
                const auto& branches = dispatch.branches;
                if (branches.size() != 2)
                    co_return plan_failure(ProgramTerminalStatus::Failed, "P_RACE_ARITY",
                                           "Program race currently requires exactly two branches",
                                           operation_id);

                const auto& first = branches[0];
                const auto& second = branches[1];
                // A race winner is the first completion; if both branches become
                // ready in one scheduler turn, declaration order is the tie break.
                // Wait for the losing coroutine to observe cancellation before this
                // operation returns, so no detached branch outlives the run handle.
                struct RaceState {
                    std::mutex                                      mutex;
                    std::array<std::optional<PlanExecution>, 2>    results;
                    std::array<std::exception_ptr, 2>              errors;
                    std::array<bool, 2>                            finished{};
                    std::vector<std::shared_ptr<graph::CancelToken>> tokens;
                    std::optional<std::size_t>                      winner;
                    std::size_t                                     completed = 0;
                    bool                                            selection_scheduled = false;
                    std::shared_ptr<asio::steady_timer>            timer;
                };
                const auto executor = co_await asio::this_coro::executor;
                const auto completion_executor = asio::make_strand(executor);
                auto race = std::make_shared<RaceState>();
                race->timer = std::make_shared<asio::steady_timer>(completion_executor);
                race->timer->expires_at((asio::steady_timer::time_point::max)());
                race->tokens.push_back(operation_token->fork());
                race->tokens.push_back(operation_token->fork());
                const std::array<std::string, 2> ids{first, second};
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    const auto branch_thread =
                        index == 0 ? control->core_thread_id
                                   : operation_thread_id(*control, ids[index]);
                    asio::co_spawn(
                        executor,
                        execute(ids[index], state, branch_thread, child_depth, race->tokens[index]),
                        asio::bind_executor(
                            completion_executor,
                            [race, index, completion_executor](std::exception_ptr error,
                                                               PlanExecution result) {
                                bool schedule_selection = false;
                                bool all_done = false;
                                {
                                    std::lock_guard lock(race->mutex);
                                    race->errors[index] = error;
                                    if (!error) race->results[index] = std::move(result);
                                    race->finished[index] = true;
                                    ++race->completed;
                                    // Defer selection by one strand turn. Completions that were
                                    // already ready with this one are then visible together, so
                                    // their deterministic tie break is declaration order rather
                                    // than cross-worker handler arrival order.
                                    if (!race->winner && !race->selection_scheduled) {
                                        race->selection_scheduled = true;
                                        schedule_selection = true;
                                    }
                                    all_done = race->winner &&
                                               race->completed == race->tokens.size();
                                }
                                if (schedule_selection) {
                                    asio::post(
                                        completion_executor, [race]() {
                                            std::optional<std::size_t> winner;
                                            bool                       cancel_loser = false;
                                            bool                       all_done = false;
                                            {
                                                std::lock_guard lock(race->mutex);
                                                if (!race->winner) {
                                                    for (std::size_t candidate = 0;
                                                         candidate < race->finished.size();
                                                         ++candidate) {
                                                        if (race->finished[candidate]) {
                                                            race->winner = candidate;
                                                            cancel_loser = true;
                                                            break;
                                                        }
                                                    }
                                                }
                                                winner = race->winner;
                                                all_done = winner &&
                                                           race->completed == race->tokens.size();
                                            }
                                            if (cancel_loser) {
                                                for (std::size_t sibling = 0;
                                                     sibling < race->tokens.size(); ++sibling)
                                                    if (sibling != *winner)
                                                        race->tokens[sibling]->cancel();
                                            }
                                            if (all_done) race->timer->cancel();
                                        });
                                } else if (all_done) {
                                    race->timer->cancel();
                                }
                            }));
                }
                co_await asio::co_spawn(
                    completion_executor,
                    [race]() -> asio::awaitable<void> {
                        asio::error_code error;
                        co_await race->timer->async_wait(
                            asio::redirect_error(asio::use_awaitable, error));
                    },
                    asio::use_awaitable);

                std::optional<std::size_t>                   winner;
                std::array<std::optional<PlanExecution>, 2> results;
                std::array<std::exception_ptr, 2>           errors;
                {
                    std::lock_guard lock(race->mutex);
                    winner = race->winner;
                    results = std::move(race->results);
                    errors = std::move(race->errors);
                }
                if (!winner)
                    throw_runtime_diagnostic("P_RUNTIME_RACE", "Race completed without a winner");
                const auto index = *winner;
                if (errors[index]) std::rethrow_exception(errors[index]);
                if (!results[index])
                    throw_runtime_diagnostic("P_RUNTIME_RACE", "Race winner has no result");
                co_return std::move(*results[index]);
            }
            if (op == ProgramOperationKind::Quorum) {
                const auto& branches = dispatch.branches;
                const auto required = *operation.min_success();
                PlanExecution result;
                result.output = json::array();
                std::uint64_t successes = 0;
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    auto branch_result = co_await execute(
                        branches[index], state,
                        operation_thread_id(*control, branches[index]),
                        child_depth, operation_token);
                    const bool completed =
                        branch_result.status == ProgramTerminalStatus::Completed;
                    if (completed) {
                        result.output.push_back(branch_result.output);
                        ++successes;
                    }
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                    if (successes >= required) co_return result;
                    const auto remaining = branches.size() - index - 1;
                    if (successes + remaining < required) co_return result;
                }
                co_return plan_failure(ProgramTerminalStatus::Failed, "P_QUORUM_UNREACHABLE",
                                       "Program quorum could not reach its required successes",
                                       operation_id);
            }

            if (op == ProgramOperationKind::Map) {
                PlanExecution result;
                result.output = json::array();
                for (const auto& item : operation.items()) {
                    auto mapped = co_await execute(*dispatch.body, item,
                                                   operation_thread_id(*control, operation_id),
                                                   child_depth, operation_token);
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(mapped.execution_trace.begin()),
                        std::make_move_iterator(mapped.execution_trace.end()));
                    result.returned = result.returned || mapped.returned;
                    if (mapped.status != ProgramTerminalStatus::Completed) {
                        result.status    = mapped.status;
                        result.interrupt = std::move(mapped.interrupt);
                        result.failure   = std::move(mapped.failure);
                        co_return result;
                    }
                    result.output.push_back(std::move(mapped.output));
                }
                co_return result;
            }

            if (op == ProgramOperationKind::Spawn) {
                if (!operation.child_binding())
                    co_return plan_failure(ProgramTerminalStatus::Failed, "P_CHILD_BINDING",
                                           "Program spawn has no lowered child binding",
                                           operation_id);
                std::string execution_key;
                {
                    std::lock_guard lock(plan_mutex);
                    const auto base = thread_id + ":" + operation_id;
                    const auto ordinal = spawn_occurrences[base]++;
                    execution_key = base + ":" + std::to_string(ordinal);
                }
                auto child = control->launch_child(*operation.child_binding(), std::move(state),
                                                   operation_id, execution_key);
                PlanExecution result;
                result.output = json{{"child_run_id", child->run_id},
                                     {"program_version_id", child->program_version_id}};
                result.spawned_child = std::move(child);
                co_return result;
            }

            if (op == ProgramOperationKind::Cancel) {
                control->cancel(CancellationCause::User);
                PlanExecution result;
                result.status = ProgramTerminalStatus::Cancelled;
                result.output = std::move(state);
                result.failure = ProgramFailure{
                    "P_RUNTIME_CANCELLED",
                    operation.reason().value_or("Program cancelled by plan"),
                    operation_id,
                    "",
                    0,
                    json{{"scope", operation.scope().value_or("run")}}};
                co_return result;
            }

            if (op == ProgramOperationKind::Await) {
                const auto child_id = *dispatch.body;
                const auto* child_operation = typed_plan.find(child_id);
                if (child_operation && child_operation->operation() == ProgramOperationKind::Spawn) {
                    auto launched = co_await execute(child_id, std::move(state), std::move(thread_id),
                                                     child_depth, operation_token);
                    if (launched.status != ProgramTerminalStatus::Completed) co_return launched;
                    if (!launched.spawned_child) {
                        co_return plan_failure(ProgramTerminalStatus::Failed, "P_AWAIT_HANDLE",
                                               "Program await body did not produce a child handle",
                                               operation_id);
                    }
                    auto child = std::move(launched.spawned_child);
                    if (!operation.timeout_ms()) {
                        co_return plan_result_from_child(co_await child->wait_async());
                    }

                    const auto executor = co_await asio::this_coro::executor;
                    auto timeout = std::make_shared<asio::steady_timer>(executor);
                    timeout->expires_after(std::chrono::milliseconds(*operation.timeout_ms()));
                    auto timeout_operation = [timeout]() -> asio::awaitable<void> {
                        asio::error_code error;
                        co_await timeout->async_wait(
                            asio::redirect_error(asio::use_awaitable, error));
                    };
                    // Use the same first-completion group as inline await. A
                    // timer cancelled before a later async_wait registration
                    // does not wake that future wait, whereas the group owns
                    // both registrations before either branch can settle.
                    auto [order, child_error, child_result, timeout_error] =
                        co_await asio::experimental::make_parallel_group(
                                     asio::co_spawn(executor, child->wait_async(), asio::deferred),
                                     asio::co_spawn(executor, timeout_operation(), asio::deferred))
                            .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
                    if (order[0] == 0) {
                        if (child_error) std::rethrow_exception(child_error);
                        co_return plan_result_from_child(child_result);
                    }
                    (void)timeout_error;
                    if (const auto terminal = child->try_result())
                        co_return plan_result_from_child(*terminal);
                    (void)child->cancel(CancellationCause::ParentTerminal);
                    co_return plan_failure(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                           "Program await operation timed out", operation_id);
                }
                if (!operation.timeout_ms()) {
                    co_return co_await execute(child_id, std::move(state),
                                               std::move(thread_id), child_depth, operation_token);
                }
                const auto executor = co_await asio::this_coro::executor;
                auto timeout = std::make_shared<asio::steady_timer>(executor);
                timeout->expires_after(std::chrono::milliseconds(*operation.timeout_ms()));
                auto child_token = operation_token->fork();
                auto timeout_operation = cancel_after_timer(timeout, child_token);
                // Await races the child completion (including a child error)
                // against the timeout.  The awaitable `||` operator waits for
                // one *successful* operation, which would mask an immediate
                // Core failure until the timeout and misclassify it as a
                // timeout.  The explicit parallel group preserves the first
                // completion and lets the outer attempt classify the child
                // exception normally.
                auto [order, child_error, child_result, timeout_error] =
                    co_await asio::experimental::make_parallel_group(
                                 asio::co_spawn(
                                     executor,
                                     execute(child_id, std::move(state), std::move(thread_id),
                                             child_depth, child_token),
                                     asio::deferred),
                                 asio::co_spawn(executor, std::move(timeout_operation),
                                                asio::deferred))
                        .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
                if (order[0] == 0) {
                    if (child_error) std::rethrow_exception(child_error);
                    co_return std::move(child_result);
                }
                (void)timeout_error;
                child_token->cancel();
                co_return plan_failure(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                       "Program await operation timed out", operation_id);
            }
            if (op == ProgramOperationKind::Checkpoint) {
                if (dispatch.body) {
                    auto body = co_await execute(*dispatch.body,
                                                 state, thread_id, child_depth, operation_token);
                    if (body.status != ProgramTerminalStatus::Completed) co_return body;
                    state = std::move(body.output);
                    PlanExecution result = std::move(body);
                    if (const auto checkpoint = root_checkpoint()) {
                        auto event = control->stage_event(
                            operation_id,
                            ProgramEventKind::CheckpointPublished,
                            ProgramCheckpointEvent{*checkpoint});
                        control->deliver_event(event);
                    }
                    co_return result;
                }
                PlanExecution result;
                result.output = std::move(state);
                if (const auto checkpoint = root_checkpoint()) {
                    auto event = control->stage_event(
                        operation_id,
                        ProgramEventKind::CheckpointPublished,
                        ProgramCheckpointEvent{*checkpoint});
                    control->deliver_event(event);
                }
                co_return result;
            }
            if (op == ProgramOperationKind::Emit) {
                control->emit(operation_id, ProgramEventKind::Emit,
                              ProgramEmitEvent{operation_id, operation.value()});
                PlanExecution result;
                result.output = std::move(state);
                co_return result;
            }

            if (op == ProgramOperationKind::Return) {
                PlanExecution result;
                result.output = operation.value();
                result.returned = true;
                co_return result;
            }

            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program operation is unsupported",
                                     json{{"operation", std::string(to_string(op))}});
        };

        auto plan_result =
            co_await execute(root_id, std::move(input), control->core_thread_id, 0,
                             control->cancel_token);
        const auto terminal_cause = terminal_cause_at_deadline(control, deadline);
        outcome.output = std::move(plan_result.output);
        outcome.execution_trace = std::move(plan_result.execution_trace);
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = core_progress->steps();
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        outcome.checkpoint = last_root_checkpoint;

        const auto cancellation_message =
            plan_result.failure && !plan_result.failure->message.empty()
                ? plan_result.failure->message
                : std::string("Program attempt was cancelled");
        if (apply_terminal_cause(outcome, terminal_cause, cancellation_message)) {
        } else if (plan_result.status != ProgramTerminalStatus::Completed) {
            outcome.status = plan_result.status;
            outcome.interrupt = std::move(plan_result.interrupt);
            outcome.failure = std::move(plan_result.failure);
        } else if (!outcome.checkpoint && resume_checkpoint) {
            outcome.checkpoint = resume_checkpoint;
        } else if (outcome.usage.core_steps > control->granted_budget.max_core_steps ||
                   outcome.usage.model_tokens > control->granted_budget.model_tokens ||
                   outcome.usage.wall_time_ms > control->granted_budget.wall_time_ms) {
            outcome.status = ProgramTerminalStatus::BudgetExhausted;
        } else {
            try {
                validate_contract_value(outcome.output,
                                        control->materialized->bundle.output_contract());
                outcome.status = ProgramTerminalStatus::Completed;
            } catch (const std::exception& error) {
                outcome.status = ProgramTerminalStatus::Failed;
                outcome.failure = ProgramFailure{
                    "P_OUTPUT_CONTRACT", "Program output violates its admitted contract",
                    root_id, "", 0, json{{"detail", error.what()}}};
            }
        }
        if (outcome.failure) {
            auto& witness = outcome.failure->witness;
            if (!witness.is_object()) witness = json::object();
            if (!witness.contains("source_pointer")) {
                const auto* found = typed_plan.find(outcome.failure->operation_id);
                if (found && !found->dispatch().source_pointer.empty())
                    witness["source_pointer"] = found->dispatch().source_pointer;
            }
        }
    } catch (const EventSinkError& error) {
        outcome = failed_outcome(control, "P_EVENT_SINK", error.what());
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
    } catch (const graph::CancelledException& error) {
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        auto cause = terminal_cause_at_deadline(control, deadline);
        if (cause == CancellationCause::None) cause = CancellationCause::User;
        apply_terminal_cause(outcome, cause, error.what());
    } catch (const NestedOperationFailure& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(),
                                 error.operation_id, error.attempts);
        if (outcome.failure) {
            outcome.failure->operation_id = error.operation_id;
            outcome.failure->core_node     = error.core_node;
        }
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        const auto cause = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(outcome, cause, error.what()) &&
            error.checkpoint_incompatible && checkpoint_id) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
    } catch (const graph::NodeExecutionError& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(), error.node_name(),
                                 static_cast<std::uint32_t>(error.attempts()));
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
    } catch (const ProgramDiagnosticError& error) {
        const auto& diagnostic = error.diagnostic();
        std::string operation_id = "root";
        if (diagnostic.witness.is_object() &&
            diagnostic.witness.contains("operation_id") &&
            diagnostic.witness["operation_id"].is_string()) {
            operation_id = diagnostic.witness["operation_id"].get<std::string>();
        }
        outcome = failed_outcome(control, diagnostic.code, diagnostic.message);
        if (outcome.failure) outcome.failure->operation_id = operation_id;
        if (outcome.failure) outcome.failure->witness = diagnostic.witness;
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline),
                             diagnostic.message);
    } catch (const std::exception& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what());
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.model_tokens = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        const auto cause = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(outcome, cause, error.what()) && checkpoint_id &&
            !checkpoint_is_available(*control, *checkpoint_id)) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
    } catch (...) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", "Unknown Core failure");
        outcome.usage.wall_time_ms = elapsed_ms(started_at);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline),
                             "Unknown Core failure");
    }

    if (outcome.failure) {
        auto& witness = outcome.failure->witness;
        if (!witness.is_object()) witness = json::object();
        if (!witness.contains("source_pointer")) {
            try {
                const auto& typed_plan = control->materialized->bundle.typed_orchestration_plan();
                const auto* operation = typed_plan.find(outcome.failure->operation_id);
                if (operation && !operation->dispatch().source_pointer.empty())
                    witness["source_pointer"] = operation->dispatch().source_pointer;
            } catch (...) {
                // Admission already validates the typed plan; preserve the terminal
                // failure if a malformed legacy bundle reaches this boundary.
            }
        }
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
