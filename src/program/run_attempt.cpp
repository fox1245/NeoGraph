#include <neograph/program/diagnostic.h>
#include <neograph/graph/cancel.h>

#include "canonical_json.h"
#include "core_progress.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
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
using namespace asio::experimental::awaitable_operators;

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
    json                             output = json::object();
    std::optional<ProgramInterrupt> interrupt;
    std::optional<ProgramFailure>    failure;
    std::vector<std::string>         execution_trace;
    bool                             returned = false;
};

void append_trace(PlanExecution& target, PlanExecution&& source) {
    target.output = std::move(source.output);
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
asio::awaitable<void> cancel_after_timer(
    std::shared_ptr<asio::steady_timer> timer,
    std::shared_ptr<graph::CancelToken> token) {
    asio::error_code wait_error;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (!wait_error) token->cancel();
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
    const auto started_at = control->started_at;
    const auto deadline   = control->deadline;
    auto       usage      = std::make_shared<UsageAccumulator>();
    auto       core_progress = std::make_shared<AttemptCoreProgress>();
    std::uint64_t operation_count = 0;
    std::atomic<std::size_t> total_children{0};
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

        const auto plan_record = control->materialized->bundle.orchestration_plan();
        if (plan_record.schema_version != 1 || !plan_record.plan.is_object() ||
            !plan_record.plan.contains("root") ||
            !plan_record.plan["root"].is_string() ||
            !plan_record.plan.contains("operations") ||
            !plan_record.plan["operations"].is_array()) {
            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program plan is malformed");
        }
        std::map<std::string, json> operations;
        for (const auto& operation : plan_record.plan["operations"]) {
            if (!operation.is_object() || !operation.contains("id") ||
                !operation["id"].is_string() || !operation.contains("op") ||
                !operation["op"].is_string()) {
                throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                         "Admitted Program operation is malformed");
            }
            const auto id = operation["id"].get<std::string>();
            if (!operations.emplace(id, operation).second)
                throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program operation id is duplicated",
                                         json{{"operation_id", id}});
        }
        const auto root_id = plan_record.plan["root"].get<std::string>();
        if (!operations.contains(root_id))
            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program root operation is missing");

        std::mutex plan_mutex;
        std::optional<std::string> resume_id = checkpoint_id;
        std::optional<CoreCheckpointIdentity> last_root_checkpoint;
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
            (void)operation_id;
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
                [control, core_progress](const graph::GraphEvent& event) {
                    core_progress->observe(event);
                    control->emit(ProgramEventKind::Core, graph::to_typed_event(event));
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
            const auto found = operations.find(operation_id);
            if (found == operations.end()) {
                throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                         "Program operation reference is dangling",
                                         json{{"operation_id", operation_id}});
            }
            const auto operation = found->second;
            const auto op = operation["op"].get<std::string>();
            if (operation_token->is_cancelled()) {
                co_return plan_failure(ProgramTerminalStatus::Cancelled,
                                       "P_RUNTIME_CANCELLED",
                                       "Program operation cancelled before dispatch",
                                       operation_id);
            }
            if (op != "call_core") {
                if (auto failure = charge_operation(operation_id))
                    co_return std::move(*failure);
            }
            if (op == "call_core") {
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
                const auto active =
                    active_concurrency.fetch_add(1, std::memory_order_relaxed) + 1;
                update_peak(active);
                CoreInvocation invocation;
                try {
                    invocation = co_await run_core(operation_id, std::move(state), std::move(resume),
                                                   std::move(thread_id), operation_token);
                } catch (...) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    throw;
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
                    result.interrupt = decode_core_interrupt(*control, invocation.result);
                } else if (core_status == graph::RunStatus::StepLimit) {
                    result.status = ProgramTerminalStatus::BudgetExhausted;
                    result.failure = ProgramFailure{
                        "P_CORE_STEP_BUDGET", "Core step budget exhausted", operation_id,
                        invocation.result.interrupt_node, 0, json::object()};
                }
                co_return result;
            }
            if (op == "sequence") {
                PlanExecution result;
                result.output = std::move(state);
                for (const auto& child : operation["children"]) {
                    auto child_result =
                        co_await execute(child.get<std::string>(), result.output, thread_id,
                                         child_depth, operation_token);
                    append_trace(result, std::move(child_result));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                }
                co_return result;
            }

            if (op == "branch") {
                const auto selected =
                    condition_matches(state, operation["condition"]) ? "then" : "else";
                if (!operation.contains(selected)) {
                    PlanExecution result;
                    result.output = std::move(state);
                    co_return result;
                }
                co_return co_await execute(operation[selected].get<std::string>(),
                                           std::move(state), std::move(thread_id), child_depth,
                                           operation_token);
            }

            if (op == "loop") {
                PlanExecution result;
                result.output = std::move(state);
                const auto maximum = operation["max_iterations"].get<std::uint64_t>();
                for (std::uint64_t iteration = 0;
                     iteration < maximum &&
                     condition_matches(result.output, operation["condition"]);
                     ++iteration) {
                    auto body = co_await execute(operation["body"].get<std::string>(),
                                                 result.output, thread_id, child_depth,
                                                 operation_token);
                    append_trace(result, std::move(body));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                }
                if (condition_matches(result.output, operation["condition"])) {
                    result = plan_failure(ProgramTerminalStatus::BudgetExhausted,
                                          "P_LOOP_BOUND", "Program loop bound exhausted",
                                          operation_id);
                }
                co_return result;
            }

            if (op == "retry") {
                const auto maximum = operation["max_attempts"].get<std::uint64_t>();
                std::optional<PlanExecution> last_result;
                for (std::uint64_t attempt = 0; attempt < maximum; ++attempt) {
                    try {
                        auto attempt_result = co_await execute(
                            operation["body"].get<std::string>(), state, thread_id, child_depth,
                            operation_token);
                        if (attempt_result.status == ProgramTerminalStatus::Completed)
                            co_return attempt_result;
                        if (attempt_result.status == ProgramTerminalStatus::Interrupted ||
                            attempt_result.status == ProgramTerminalStatus::Cancelled ||
                            attempt_result.status == ProgramTerminalStatus::TimedOut ||
                            attempt_result.status == ProgramTerminalStatus::BudgetExhausted)
                            co_return attempt_result;
                        last_result = std::move(attempt_result);
                    } catch (...) {
                        if (attempt + 1 == maximum) throw;
                    }
                }
                if (last_result) co_return std::move(*last_result);
                co_return plan_failure(ProgramTerminalStatus::Failed, "P_RETRY_BOUND",
                                       "Program retry operation had no attempts", operation_id);
            }

            if (op == "parallel") {
                const auto branches = operation["branches"];
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
                    std::shared_ptr<asio::steady_timer>              timer;
                };
                const auto executor = co_await asio::this_coro::executor;
                auto       parallel = std::make_shared<ParallelState>();
                parallel->results.resize(branches.size());
                parallel->remaining = branches.size();
                parallel->timer = std::make_shared<asio::steady_timer>(executor);
                parallel->timer->expires_at((asio::steady_timer::time_point::max)());
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    const auto branch_id = branches[index].get<std::string>();
                    const auto branch_thread =
                        index == 0 ? control->core_thread_id
                                   : operation_thread_id(*control, branch_id);
                    auto branch_token = operation_token->fork();
                    parallel->branch_tokens.push_back(branch_token);
                    asio::co_spawn(
                        executor,
                        execute(branch_id, state, branch_thread, child_depth, branch_token),
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
                        });
                }
                asio::error_code wait_error;
                co_await parallel->timer->async_wait(
                    asio::redirect_error(asio::use_awaitable, wait_error));
                if (parallel->error) std::rethrow_exception(parallel->error);

                PlanExecution result;
                result.output = json::array();
                for (auto& branch : parallel->results) {
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

            if (op == "race") {
                const auto branches = operation["branches"];
                if (branches.size() != 2)
                    co_return plan_failure(ProgramTerminalStatus::Failed, "P_RACE_ARITY",
                                           "Program race currently requires exactly two branches",
                                           operation_id);

                const auto first = branches[0].get<std::string>();
                const auto second = branches[1].get<std::string>();
                auto winner = co_await (
                    execute(first, state, control->core_thread_id, child_depth,
                            operation_token->fork()) ||
                    execute(second, state, operation_thread_id(*control, second), child_depth,
                            operation_token->fork()));
                if (winner.index() == 0) co_return std::move(std::get<0>(winner));
                co_return std::move(std::get<1>(winner));
            }
            if (op == "quorum") {
                const auto branches = operation["branches"];
                const auto required = operation["min_success"].get<std::uint64_t>();
                PlanExecution result;
                result.output = json::array();
                std::uint64_t successes = 0;
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    auto branch_result = co_await execute(
                        branches[index].get<std::string>(), state,
                        operation_thread_id(*control, branches[index].get<std::string>()),
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

            if (op == "map") {
                PlanExecution result;
                result.output = json::array();
                for (const auto& item : operation["items"]) {
                    auto mapped = co_await execute(operation["body"].get<std::string>(), item,
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

            if (op == "spawn") {
                if (child_depth >= control->granted_budget.max_child_depth)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CHILD_BUDGET",
                        "Program child depth budget exhausted", operation_id);
                const auto child_limit = control->granted_budget.max_total_children;
                auto previous = total_children.load(std::memory_order_relaxed);
                while (previous < child_limit &&
                       !total_children.compare_exchange_weak(
                           previous, previous + 1, std::memory_order_relaxed)) {
                }
                if (previous >= child_limit)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CHILD_BUDGET",
                        "Program child budget exhausted", operation_id);
                auto child_token = operation_token->fork();
                co_return co_await execute(operation["body"].get<std::string>(),
                                           std::move(state),
                                           operation_thread_id(*control, operation_id),
                                           child_depth + 1, std::move(child_token));
            }

            if (op == "cancel") {
                control->cancel(CancellationCause::User);
                PlanExecution result;
                result.status = ProgramTerminalStatus::Cancelled;
                result.output = std::move(state);
                co_return result;
            }

            if (op == "await") {
                const auto child_id = operation["body"].get<std::string>();
                if (!operation.contains("timeout_ms")) {
                    co_return co_await execute(child_id, std::move(state),
                                               std::move(thread_id), child_depth, operation_token);
                }
                const auto executor = co_await asio::this_coro::executor;
                auto timeout = std::make_shared<asio::steady_timer>(executor);
                timeout->expires_after(std::chrono::milliseconds(
                    operation["timeout_ms"].get<std::uint64_t>()));
                auto child_token = operation_token->fork();
                auto timeout_operation = cancel_after_timer(timeout, child_token);
                auto winner = co_await (
                    execute(child_id, std::move(state), std::move(thread_id), child_depth,
                            child_token) ||
                    std::move(timeout_operation));
                if (winner.index() == 0) co_return std::move(std::get<0>(winner));
                child_token->cancel();
                co_return plan_failure(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                       "Program await operation timed out", operation_id);
            }
            if (op == "checkpoint") {
                if (operation.contains("body")) {
                    auto body = co_await execute(operation["body"].get<std::string>(),
                                                 state, thread_id, child_depth, operation_token);
                    if (body.status != ProgramTerminalStatus::Completed) co_return body;
                    state = std::move(body.output);
                    PlanExecution result = std::move(body);
                    if (const auto checkpoint = root_checkpoint()) {
                        auto event = control->stage_event(
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
                        ProgramEventKind::CheckpointPublished,
                        ProgramCheckpointEvent{*checkpoint});
                    control->deliver_event(event);
                }
                co_return result;
            }

            if (op == "emit") {
                control->emit(ProgramEventKind::Emit,
                              ProgramEmitEvent{operation_id, operation["value"]});
                PlanExecution result;
                result.output = std::move(state);
                co_return result;
            }

            if (op == "return") {
                PlanExecution result;
                result.output = operation["value"];
                result.returned = true;
                co_return result;
            }

            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program operation is unsupported",
                                     json{{"operation", op}});
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

        if (apply_terminal_cause(outcome, terminal_cause, "Program attempt was cancelled")) {
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
