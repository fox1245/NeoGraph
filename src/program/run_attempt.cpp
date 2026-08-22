#include <neograph/graph/cancel.h>
#include <neograph/program/compiler.h>
#include <neograph/program/diagnostic.h>

#include "canonical_json.h"
#include "core_progress.h"
#include "javascript.h"
#include "run_control.h"
#include <asio/as_tuple.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>
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
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program::detail {
namespace {
static_assert(NATIVE_CONTROL_IMPORT_SLOT_MIN == JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE + 1);

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

class ExactCheckpointContentStore final : public graph::CheckpointStore {
public:
    ExactCheckpointContentStore(std::shared_ptr<graph::CheckpointStore> inner,
                                std::string checkpoint_id,
                                std::string content_id)
        : inner_(std::move(inner)),
          checkpoint_id_(std::move(checkpoint_id)),
          content_id_(std::move(content_id)) {
        if (!inner_ || checkpoint_id_.empty() || content_id_.empty()) {
            throw std::invalid_argument(
                "Exact checkpoint content store requires a complete binding");
        }
    }

    void save(const graph::Checkpoint& checkpoint) override {
        validate(checkpoint);
        inner_->save(checkpoint);
    }
    std::optional<graph::Checkpoint> load_latest(const std::string& thread_id) override {
        auto checkpoint = inner_->load_latest(thread_id);
        validate(checkpoint);
        return checkpoint;
    }
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override {
        auto checkpoint = inner_->load_by_id(id);
        if (id == checkpoint_id_ && !checkpoint) {
            throw std::runtime_error("Exact checkpoint disappeared before consuming load");
        }
        validate(checkpoint);
        return checkpoint;
    }
    std::vector<graph::Checkpoint> list(const std::string& thread_id, int limit) override {
        auto checkpoints = inner_->list(thread_id, limit);
        for (const auto& checkpoint : checkpoints) validate(checkpoint);
        return checkpoints;
    }
    void delete_thread(const std::string& thread_id) override {
        inner_->delete_thread(thread_id);
    }

    asio::awaitable<void> save_async(const graph::Checkpoint& checkpoint) override {
        validate(checkpoint);
        co_await inner_->save_async(checkpoint);
    }
    asio::awaitable<std::optional<graph::Checkpoint>> load_latest_async(
        const std::string& thread_id) override {
        auto checkpoint = co_await inner_->load_latest_async(thread_id);
        validate(checkpoint);
        co_return checkpoint;
    }
    asio::awaitable<std::optional<graph::Checkpoint>> load_by_id_async(
        const std::string& id) override {
        auto checkpoint = co_await inner_->load_by_id_async(id);
        if (id == checkpoint_id_ && !checkpoint) {
            throw std::runtime_error("Exact checkpoint disappeared before consuming load");
        }
        validate(checkpoint);
        co_return checkpoint;
    }
    asio::awaitable<std::vector<graph::Checkpoint>> list_async(
        const std::string& thread_id,
        int limit) override {
        auto checkpoints = co_await inner_->list_async(thread_id, limit);
        for (const auto& checkpoint : checkpoints) validate(checkpoint);
        co_return checkpoints;
    }
    asio::awaitable<void> delete_thread_async(const std::string& thread_id) override {
        co_await inner_->delete_thread_async(thread_id);
    }

    void put_writes(const std::string& thread_id,
                    const std::string& parent_checkpoint_id,
                    const graph::PendingWrite& write) override {
        inner_->put_writes(thread_id, parent_checkpoint_id, write);
    }
    std::vector<graph::PendingWrite> get_writes(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override {
        return inner_->get_writes(thread_id, parent_checkpoint_id);
    }
    void clear_writes(const std::string& thread_id,
                      const std::string& parent_checkpoint_id) override {
        inner_->clear_writes(thread_id, parent_checkpoint_id);
    }
    asio::awaitable<void> put_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id,
        const graph::PendingWrite& write) override {
        co_await inner_->put_writes_async(thread_id, parent_checkpoint_id, write);
    }
    asio::awaitable<std::vector<graph::PendingWrite>> get_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override {
        co_return co_await inner_->get_writes_async(thread_id, parent_checkpoint_id);
    }
    asio::awaitable<void> clear_writes_async(
        const std::string& thread_id,
        const std::string& parent_checkpoint_id) override {
        co_await inner_->clear_writes_async(thread_id, parent_checkpoint_id);
    }

private:
    void validate(const graph::Checkpoint& checkpoint) const {
        if (checkpoint.id == checkpoint_id_ &&
            graph_migration_checkpoint_content_id(checkpoint) != content_id_) {
            throw std::runtime_error(
                "Exact checkpoint content changed before consuming load");
        }
    }
    void validate(const std::optional<graph::Checkpoint>& checkpoint) const {
        if (checkpoint) validate(*checkpoint);
    }

    std::shared_ptr<graph::CheckpointStore> inner_;
    std::string checkpoint_id_;
    std::string content_id_;
};

bool has_resource_reservation(const RunBudget& reservation) noexcept {
    return reservation.wall_time_ms != 0 || reservation.model_tokens != 0 ||
           reservation.monetary_microunits != 0 || reservation.max_core_steps != 0;
}

RunBudget settle_resource_reservation(const RunBudget&    available,
                                      const RunBudget&    reservation,
                                      const ProgramUsage& verified_usage) {
    RunBudget settled = available;
    settled.wall_time_ms +=
        subtract_saturated(reservation.wall_time_ms, verified_usage.wall_time_ms);
    settled.model_tokens +=
        subtract_saturated(reservation.model_tokens, verified_usage.model_tokens);
    settled.monetary_microunits +=
        subtract_saturated(reservation.monetary_microunits, verified_usage.monetary_microunits);
    settled.max_core_steps +=
        subtract_saturated(reservation.max_core_steps, verified_usage.core_steps);
    return settled;
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
    if (!checkpoint || checkpoint->id != checkpoint_id || checkpoint->thread_id != thread_id)
        return std::nullopt;
    return CoreCheckpointIdentity{
        control.materialized->root->core_name, control.materialized->root->compiled_plan_identity,
        checkpoint->thread_id, checkpoint->id, checkpoint->schema_version};
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
std::string task_attempt_identity(std::string_view fragment_id,
                                  std::string_view task_id,
                                  std::uint32_t    attempt) {
    return detail::sha256_identity(
        "program-task-graph-attempt/v1",
        detail::canonical_json_bytes(json{{"fragment_id", std::string(fragment_id)},
                                          {"task_id", std::string(task_id)},
                                          {"attempt", attempt}}));
}

std::optional<json> json_path_value(const json& value, std::string_view path) {
    if (path.empty()) return value;
    if (path.front() != '/') return std::nullopt;
    json        current = value;
    std::size_t begin   = 1;
    while (begin <= path.size()) {
        const auto end  = path.find('/', begin);
        auto       part = std::string(
            path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin));
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
                current              = current.at(static_cast<std::size_t>(index));
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
std::optional<ProgramFailure> validate_task_output(const CompiledTaskGraphTask& task,
                                                   const std::optional<json>&   output) {
    if (!output) {
        if (task.output_artifacts.empty()) return std::nullopt;
        return ProgramFailure{"P_TASK_OUTPUT_CONTRACT",
                              "Completed task did not produce its declared output artifacts",
                              task.operation_id,
                              "",
                              0,
                              json::object()};
    }
    for (const auto& artifact : task.output_artifacts) {
        for (const auto& field : artifact.fields) {
            if (!json_path_value(*output, field)) {
                return ProgramFailure{"P_TASK_OUTPUT_CONTRACT",
                                      "Task output is missing a declared artifact field",
                                      task.operation_id,
                                      "",
                                      0,
                                      json{{"artifact", artifact.artifact}, {"field", field}}};
            }
        }
    }
    if (task.reserved_budget.output_bytes != 0 &&
        output->dump().size() > task.reserved_budget.output_bytes) {
        return ProgramFailure{"P_TASK_OUTPUT_BUDGET",
                              "Task output exceeds its reserved output byte budget",
                              task.operation_id,
                              "",
                              0,
                              json{{"output_bytes", output->dump().size()},
                                   {"reserved_output_bytes", task.reserved_budget.output_bytes}}};
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> decode_json_pointer(std::string_view path) {
    if (path.empty()) return std::vector<std::string>{};
    if (path.front() != '/') return std::nullopt;

    std::vector<std::string> tokens;
    std::size_t              begin = 1;
    while (begin <= path.size()) {
        const auto end = path.find('/', begin);
        const auto part =
            path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        std::string decoded;
        decoded.reserve(part.size());
        for (std::size_t index = 0; index < part.size(); ++index) {
            if (part[index] != '~') {
                decoded.push_back(part[index]);
                continue;
            }
            if (index + 1 == part.size() || (part[index + 1] != '0' && part[index + 1] != '1'))
                return std::nullopt;
            decoded.push_back(part[index + 1] == '0' ? '~' : '/');
            ++index;
        }
        tokens.push_back(std::move(decoded));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return tokens;
}

bool json_array_index(std::string_view token, std::size_t& result) {
    if (token.empty()) return false;
    std::size_t value = 0;
    for (const char character : token) {
        if (character < '0' || character > '9') return false;
        const auto digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    result = value;
    return true;
}

bool assign_json_pointer(json&&                          target,
                         const std::vector<std::string>& tokens,
                         std::size_t                     index,
                         json                            value) {
    if (index == tokens.size()) {
        target = std::move(value);
        return true;
    }
    if (target.is_null()) {
        std::size_t array_index = 0;
        target = json_array_index(tokens[index], array_index) ? json::array() : json::object();
    }
    if (target.is_object())
        return assign_json_pointer(target[tokens[index]], tokens, index + 1, std::move(value));
    if (!target.is_array()) return false;

    std::size_t array_index = 0;
    if (!json_array_index(tokens[index], array_index) || array_index > target.size()) return false;
    if (array_index == target.size()) target.push_back(nullptr);
    try {
        return assign_json_pointer(target.at(array_index), tokens, index + 1, std::move(value));
    } catch (...) {
        return false;
    }
}

bool assign_json_pointer(json&                           target,
                         const std::vector<std::string>& tokens,
                         std::size_t                     index,
                         json                            value) {
    return assign_json_pointer(std::move(target), tokens, index, std::move(value));
}

bool assign_json_path(json& target, std::string_view path, json value) {
    const auto tokens = decode_json_pointer(path);
    return tokens && assign_json_pointer(target, *tokens, 0, std::move(value));
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
    if (condition.contains("exists")) return found.has_value() == condition["exists"].get<bool>();
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

[[noreturn]] void throw_runtime_diagnostic(std::string code,
                                           std::string message,
                                           json        witness = json::object());

/**
 * Owns children launched by one structured JavaScript scope.  RunControl
 * already owns the process-wide child scheduler and durable parent relation;
 * this small scope object only gives a join member a cancellation owner so a
 * losing member cannot leave a detached child behind.
 */
struct JavaScriptScopeState final {
    std::mutex                             mutex;
    std::vector<std::weak_ptr<RunControl>> children;
    bool                                   cancelled          = false;
    CancellationCause                      cancellation_cause = CancellationCause::ParentTerminal;

    void attach(const std::shared_ptr<RunControl>& child) noexcept {
        if (!child) return;
        bool              cancel_now = false;
        CancellationCause cause      = CancellationCause::ParentTerminal;
        {
            std::lock_guard lock(mutex);
            std::erase_if(children, [](const auto& weak) { return weak.expired(); });
            cancel_now = cancelled;
            if (cancel_now) cause = cancellation_cause;
            if (!cancel_now) children.push_back(child);
        }
        if (cancel_now) (void)child->cancel(cause);
    }

    void cancel_children(CancellationCause cause) noexcept {
        std::vector<std::shared_ptr<RunControl>> live;
        {
            std::lock_guard lock(mutex);
            cancelled          = true;
            cancellation_cause = cause;
            std::erase_if(children, [](const auto& weak) { return weak.expired(); });
            live.reserve(children.size());
            for (const auto& weak : children)
                if (auto child = weak.lock()) live.push_back(std::move(child));
        }
        for (const auto& child : live)
            (void)child->cancel(cause);
    }
};

constexpr std::size_t kMaxJavaScriptStructuredScopeDepth = 32;

std::size_t javascript_command_tree_size(const JavaScriptCommand& command, std::size_t depth = 0) {
    if (depth > kMaxJavaScriptStructuredScopeDepth)
        throw_runtime_diagnostic("P_JS_SCOPE_LIMIT",
                                 "JavaScript structured-concurrency nesting exceeds its bound");
    std::size_t total     = 1;
    const auto  arguments = command.arguments();
    const auto  add       = [&](const JavaScriptCommand& child) {
        const auto child_size = javascript_command_tree_size(child, depth + 1);
        if (child_size > std::numeric_limits<std::size_t>::max() - total)
            throw_runtime_diagnostic("P_JS_SCOPE_LIMIT",
                                            "JavaScript structured-concurrency command tree is too large");
        total += child_size;
    };
    if (command.kind() == JavaScriptCommandKind::Await) {
        add(JavaScriptCommand::from_json(arguments.at("command")));
    } else if (command.kind() == JavaScriptCommandKind::Join) {
        const auto& members = arguments.at("members");
        for (const auto& encoded : members)
            add(JavaScriptCommand::from_json(encoded));
    }
    return total;
}

void append_trace(PlanExecution& target, PlanExecution&& source) {
    target.output        = std::move(source.output);
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
                           std::string           code,
                           std::string           message,
                           std::string           operation_id) {
    PlanExecution result;
    result.status  = status;
    result.failure = ProgramFailure{
        std::move(code), std::move(message), std::move(operation_id), "", 0, json::object()};
    return result;
}

struct JavaScriptCallCoreCommand {
    std::string name;
    json        input;
};

JavaScriptCallCoreCommand parse_javascript_call_core_command(const JavaScriptCommand& command) {
    if (command.protocol_version() != JavaScriptCommand::PROTOCOL_VERSION) {
        throw_runtime_diagnostic("P_JS_CONTROL_COMMAND",
                                 "JavaScript control command protocol_version must be 1");
    }
    if (command.kind() != JavaScriptCommandKind::CallCore) {
        throw_runtime_diagnostic("P_JS_CONTROL_COMMAND",
                                 "JavaScript control command kind is not admitted by this Program");
    }
    if (command.import_slot() != JAVASCRIPT_IMPORT_SLOT_CALL_CORE) {
        throw_runtime_diagnostic("P_JS_CONTROL_COMMAND",
                                 "JavaScript control command import_slot is not admitted");
    }
    const auto arguments = command.arguments();
    if (!arguments.contains("name") || !arguments.at("name").is_string() ||
        arguments.at("name").get<std::string>().empty()) {
        throw_runtime_diagnostic("P_JS_CONTROL_COMMAND",
                                 "JavaScript control call_core command requires a nonempty name");
    }
    if (!arguments.contains("input")) {
        throw_runtime_diagnostic("P_JS_CONTROL_COMMAND",
                                 "JavaScript control call_core command requires input");
    }
    return JavaScriptCallCoreCommand{arguments.at("name").get<std::string>(),
                                     arguments.at("input")};
}

std::string javascript_command_operation_id(std::uint64_t command_ordinal) {
    // Keep the yielded-command operation below the root operation so a
    // terminal ProgramFailure can retain the normal operation-id invariant.
    return "root.javascript." + std::to_string(command_ordinal);
}

std::string javascript_command_effect_identity(const RunControl&        control,
                                               std::uint64_t            command_ordinal,
                                               const JavaScriptCommand& command) {
    json payload = json::object();
    payload["bundle_id"]      = control.bundle_id;
    payload["command_ordinal"] = command_ordinal;
    payload["command"]         = command.to_json();
    return detail::sha256_identity(
        "program-javascript-command-effect/v1",
        detail::canonical_json_bytes(payload));
}

std::optional<std::string> javascript_call_core_operation_for_thread(
    const RunControl&        control,
    const JavaScriptCommand& command,
    std::string              operation_id,
    std::string_view         core_thread_id,
    std::size_t              depth = 0) {
    if (depth > kMaxJavaScriptStructuredScopeDepth) return std::nullopt;
    if (command.kind() == JavaScriptCommandKind::CallCore) {
        return operation_thread_id(control, operation_id) == core_thread_id
                   ? std::optional<std::string>{std::move(operation_id)}
                   : std::nullopt;
    }
    const auto arguments = command.arguments();
    if (command.kind() == JavaScriptCommandKind::Await) {
        return javascript_call_core_operation_for_thread(
            control, JavaScriptCommand::from_json(arguments.at("command")), operation_id + "/await",
            core_thread_id, depth + 1);
    }
    if (command.kind() == JavaScriptCommandKind::Join) {
        const auto& members = arguments.at("members");
        for (std::size_t index = 0; index < members.size(); ++index) {
            auto found = javascript_call_core_operation_for_thread(
                control, JavaScriptCommand::from_json(members.at(index)),
                operation_id + "/member/" + std::to_string(index), core_thread_id, depth + 1);
            if (found) return found;
        }
    }
    return std::nullopt;
}
std::uint64_t native_invocation_id(const RunControl& control,
                                   std::string_view  operation_id,
                                   std::uint32_t     import_slot) {
    const auto identity = detail::sha256_identity(
        "program-native-invocation/v1",
        detail::canonical_json_bytes(json{{"run_id", control.run_id},
                                          {"attempt", control.attempt},
                                          {"operation_id", std::string(operation_id)},
                                          {"import_slot", import_slot}}));
    std::uint64_t result = 0;
    // sha256 identities are `sha256:` followed by lowercase hexadecimal.
    for (std::size_t index = 7; index < 23; ++index) {
        const char digit = identity[index];
        result           = (result << 4U) |
                 static_cast<std::uint64_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
    }
    return result == 0 ? 1 : result;
}

ProgramPendingEffect javascript_command_pending_effect(const RunControl&        control,
                                                       std::uint64_t            command_ordinal,
                                                       const JavaScriptCommand& command,
                                                       std::string_view         effect_identity) {
    const auto               command_json = command.to_json();
    ProgramPendingEffectData pending;
    pending.operation_id         = javascript_command_operation_id(command_ordinal);
    pending.call_id              = std::string(effect_identity);
    pending.effect_id            = std::string(effect_identity);
    pending.result_schema        = json::object();
    pending.payload              = json{{"bundle_id", control.bundle_id},
                                        {"command_ordinal", command_ordinal},
                                        {"command", command_json},
                                        {"effect_identity", std::string(effect_identity)}};
    pending.effect_mode          = EffectMode::Brokered;
    pending.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    pending.core_node            = "javascript";
    pending.core_interrupt_value = command_json;
    return ProgramPendingEffect(std::move(pending));
}

json javascript_command_terminal_result(const PlanExecution& result,
                                        std::string_view     status,
                                        const ProgramUsage&  usage) {
    json failure = nullptr;
    if (result.failure) {
        failure = json{{"code", result.failure->code},
                       {"message", result.failure->message},
                       {"operation_id", result.failure->operation_id},
                       {"core_node", result.failure->core_node},
                       {"attempts", result.failure->attempts},
                       {"witness", result.failure->witness}};
    }
    return json{{"status", status},
                {"output", result.output},
                {"failure", std::move(failure)},
                {"execution_trace", result.execution_trace},
                {"usage", json{{"wall_time_ms", usage.wall_time_ms},
                               {"model_tokens", usage.model_tokens},
                               {"monetary_microunits", usage.monetary_microunits},
                               {"program_operations", usage.program_operations},
                               {"core_steps", usage.core_steps},
                               {"peak_concurrency", usage.peak_concurrency}}}};
}

std::uint64_t javascript_result_uint64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "Recorded JavaScript command result has an invalid usage field",
                                 json{{"field", key}});
    }
    return value.at(key).get<std::uint64_t>();
}

ProgramUsage parse_javascript_command_usage(const json& value) {
    if (!value.is_object()) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "Recorded JavaScript command result usage is not an object");
    }
    detail::reject_unknown_fields(value, "Recorded JavaScript command result usage",
                                  {"wall_time_ms", "model_tokens", "monetary_microunits",
                                   "program_operations", "core_steps", "peak_concurrency"});
    const auto peak = javascript_result_uint64(value, "peak_concurrency");
    if (peak > std::numeric_limits<std::uint32_t>::max()) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "Recorded JavaScript command result peak concurrency is out of range");
    }
    return ProgramUsage{javascript_result_uint64(value, "wall_time_ms"),
                        javascript_result_uint64(value, "model_tokens"),
                        javascript_result_uint64(value, "monetary_microunits"),
                        javascript_result_uint64(value, "program_operations"),
                        javascript_result_uint64(value, "core_steps"),
                        static_cast<std::uint32_t>(peak)};
}

struct JavaScriptRecordedCommandResult {
    ProgramTerminalStatus         status = ProgramTerminalStatus::Failed;
    json                          output = json::object();
    ProgramUsage                  usage;
    std::optional<ProgramFailure> failure;
    std::vector<std::string>      execution_trace;
};

JavaScriptRecordedCommandResult decode_javascript_command_result(const json& value) {
    if (!value.is_object()) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "Recorded JavaScript command terminal result is not an object");
    }
    detail::reject_unknown_fields(value, "Recorded JavaScript command terminal result",
                                  {"status", "output", "failure", "execution_trace", "usage"});
    if (!value.contains("status") || !value.at("status").is_string()) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "Recorded JavaScript command terminal result has an unknown status");
    }
    ProgramTerminalStatus status;
    try {
        status = program_terminal_status_from_string(value.at("status").get<std::string>());
    } catch (const std::exception&) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "Recorded JavaScript command terminal result has an unknown status");
    }
    if (status == ProgramTerminalStatus::Interrupted ||
        status == ProgramTerminalStatus::AmbiguousEffect ||
        status == ProgramTerminalStatus::CheckpointIncompatible) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "Recorded JavaScript command result is not terminal at the command boundary");
    }
    if (!value.contains("output") || !value.contains("execution_trace") ||
        !value.at("execution_trace").is_array() || !value.contains("usage")) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "Recorded JavaScript command terminal result is incomplete");
    }
    JavaScriptRecordedCommandResult result;
    result.status = status;
    result.output = detail::owned_json_copy(value.at("output"));
    if (value.contains("failure") && !value.at("failure").is_null()) {
        const auto& failure = value.at("failure");
        detail::reject_unknown_fields(
            failure, "Recorded JavaScript command failure",
            {"code", "message", "operation_id", "core_node", "attempts", "witness"});
        if (!failure.is_object() || !failure.contains("code") || !failure.at("code").is_string() ||
            !failure.contains("message") || !failure.at("message").is_string() ||
            !failure.contains("operation_id") || !failure.at("operation_id").is_string() ||
            !failure.contains("core_node") || !failure.at("core_node").is_string() ||
            !failure.contains("attempts") || !failure.at("attempts").is_number_unsigned() ||
            !failure.contains("witness")) {
            throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                     "Recorded JavaScript command failure is malformed");
        }
        const auto attempts = failure.at("attempts").get<std::uint64_t>();
        if (attempts > std::numeric_limits<std::uint32_t>::max()) {
            throw_runtime_diagnostic(
                "P_JS_COMMAND_JOURNAL_MISMATCH",
                "Recorded JavaScript command failure attempts are out of range");
        }
        result.failure = ProgramFailure{failure.at("code").get<std::string>(),
                                        failure.at("message").get<std::string>(),
                                        failure.at("operation_id").get<std::string>(),
                                        failure.at("core_node").get<std::string>(),
                                        static_cast<std::uint32_t>(attempts),
                                        detail::owned_json_copy(failure.at("witness"))};
    }
    if (result.status != ProgramTerminalStatus::Completed && !result.failure) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "Recorded failed JavaScript command has no failure");
    }
    for (const auto& node : value.at("execution_trace")) {
        if (!node.is_string()) {
            throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                     "Recorded JavaScript command execution trace is invalid");
        }
        result.execution_trace.push_back(node.get<std::string>());
    }
    result.usage = parse_javascript_command_usage(value.at("usage"));
    return result;
}

json javascript_control_response(json output) {
    if (!output.is_object()) return output;
    const auto channels = output.find("channels");
    if (channels == output.end() || !channels.value().is_object()) return output;

    // Core output remains canonical under `channels`; expose non-conflicting
    // channel values as ergonomic aliases for JavaScript control code.
    std::vector<std::pair<std::string, json>> aliases;
    aliases.reserve(channels.value().size());
    for (const auto& [name, channel] : channels.value().items()) {
        if (output.contains(name) || !channel.is_object() || !channel.contains("value")) continue;
        aliases.emplace_back(name, channel.at("value"));
    }
    for (auto& [name, value] : aliases)
        output[name] = std::move(value);
    return output;
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
    NestedOperationFailure(std::string   operation_id,
                           std::string   message,
                           std::string   core_node               = {},
                           std::uint32_t attempts                = 0,
                           bool          checkpoint_incompatible = false)
        : std::runtime_error(std::move(message)),
          operation_id(std::move(operation_id)),
          core_node(std::move(core_node)),
          attempts(attempts),
          checkpoint_incompatible(checkpoint_incompatible) {}

    std::string   operation_id;
    std::string   core_node;
    std::uint32_t attempts                = 0;
    bool          checkpoint_incompatible = false;
};

[[noreturn]] void throw_runtime_diagnostic(std::string code, std::string message, json witness) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Seal;
    diagnostic.code     = std::move(code);
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.primary  = {"runtime", "", std::nullopt};
    diagnostic.message  = std::move(message);
    diagnostic.witness  = std::move(witness);
    throw ProgramDiagnosticError(std::move(diagnostic));
}

bool checkpoint_matches_generation(const RunControl&             control,
                                   const CoreCheckpointIdentity& checkpoint) noexcept {
    return checkpoint.core_name == control.materialized->root->core_name &&
           checkpoint.core_generation_id == control.materialized->root->compiled_plan_identity &&
           checkpoint.checkpoint_schema_version == graph::CHECKPOINT_SCHEMA_VERSION;
}

bool checkpoint_matches(const RunControl&             control,
                        const CoreCheckpointIdentity& checkpoint) noexcept {
    return checkpoint_matches_generation(control, checkpoint) &&
           checkpoint.core_thread_id == control.core_thread_id;
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
        const CoreCheckpointIdentity identity{control.materialized->root->core_name,
                                              control.materialized->root->compiled_plan_identity,
                                              loaded->thread_id, loaded->id,
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
asio::awaitable<void> cancel_after_timer(std::shared_ptr<asio::steady_timer> timer,
                                         std::shared_ptr<graph::CancelToken> token) {
    asio::error_code wait_error;
    co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (!wait_error) token->cancel();
}

constexpr std::string_view PROGRAM_PENDING_KIND_FIELD = "__neograph_program_pending_kind";

ProgramInterrupt decode_core_interrupt(const RunControl&       control,
                                       std::string_view        operation_id,
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

    if (!result.interrupt_value.is_object() || !result.interrupt_value.contains("value") ||
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

    const auto kind =
        pending_payload.at(std::string(PROGRAM_PENDING_KIND_FIELD)).get<std::string>();
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
        const auto&              effect = pending_payload.at("effect");
        ProgramPendingEffectData pending;
        pending.operation_id         = std::string(operation_id);
        pending.call_id              = pending_payload.at("call_id").get<std::string>();
        pending.effect_id            = effect.at("effect_id").get<std::string>();
        pending.result_schema        = pending_payload.at("result_schema");
        pending.payload              = pending_payload;
        pending.expires_at_unix_ms   = expiry;
        pending.effect_mode          = EffectMode::Brokered;
        pending.idempotency          = effect.value("idempotency", "unsupported") == "supported"
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
    pending.operation_id  = std::string(operation_id);
    pending.call_id       = pending_payload.at("call_id").get<std::string>();
    pending.kind          = kind == "capability_result" ? ProgramPendingInputKind::CapabilityResult
                                                        : ProgramPendingInputKind::Input;
    pending.result_schema = pending_payload.at("result_schema");
    pending.payload       = pending_payload;
    pending.expires_at_unix_ms   = expiry;
    pending.core_node            = result.interrupt_node;
    pending.core_interrupt_value = result.interrupt_value;
    return ProgramInterrupt{result.interrupt_node, result.interrupt_value,
                            ProgramPendingInput(std::move(pending)), std::nullopt};
}

}  // namespace
CommandBudgetReservation reserve_command_resources(RunBudget available) noexcept {
    RunBudget reservation;
    reservation.wall_time_ms        = available.wall_time_ms;
    reservation.model_tokens        = available.model_tokens;
    reservation.monetary_microunits = available.monetary_microunits;
    reservation.max_core_steps      = available.max_core_steps;
    available.wall_time_ms          = 0;
    available.model_tokens          = 0;
    available.monetary_microunits   = 0;
    available.max_core_steps        = 0;
    return CommandBudgetReservation{std::move(available), std::move(reservation)};
}

asio::awaitable<void> execute_run_attempt(std::shared_ptr<RunControl> control,
                                          json                        input,
                                          std::optional<std::string>  checkpoint_id,
                                          std::optional<json> javascript_core_resume_value) {
    const auto               started_at            = control->started_at;
    const auto               deadline              = control->deadline;
    auto                     usage                 = std::make_shared<UsageAccumulator>();
    auto                     core_progress         = std::make_shared<AttemptCoreProgress>();
    std::uint64_t            operation_count       = 0;
    std::uint64_t            dynamic_compile_count = 0;
    std::atomic<std::size_t> active_concurrency{0};
    std::atomic<std::size_t> peak_concurrency{0};
    std::optional<RunBudget> unreconciled_javascript_remaining;
    struct ActiveJavaScriptCommand {
        std::uint64_t     ordinal = 0;
        JavaScriptCommand command;
        std::string       effect_identity;
    };
    std::optional<ActiveJavaScriptCommand> active_javascript_command;
    std::uint64_t                          settled_javascript_wall_time_ms = 0;
    const auto settle_unreserved_javascript_wall = [&](RunBudget remaining) {
        ProgramUsage unreserved;
        unreserved.wall_time_ms =
            subtract_saturated(elapsed_ms(started_at), settled_javascript_wall_time_ms);
        return settle_budget(remaining, unreserved);
    };
    const auto apply_durable_javascript_disposition = [&](RunOutcome& terminal) noexcept {
        if (unreconciled_javascript_remaining) {
            terminal.remaining_budget =
                settle_unreserved_javascript_wall(*unreconciled_javascript_remaining);
        }
        if (!active_javascript_command) return;
        try {
            const auto latest = control->transitions->latest(control->owner_scope, control->run_id);
            if (!latest || !has_resource_reservation(latest->inflight_reservation)) return;
            terminal.status = ProgramTerminalStatus::AmbiguousEffect;
            terminal.failure.reset();
            terminal.interrupt = ProgramInterrupt{
                "javascript", active_javascript_command->command.to_json(), std::nullopt,
                javascript_command_pending_effect(*control, active_javascript_command->ordinal,
                                                  active_javascript_command->command,
                                                  active_javascript_command->effect_identity)};
        } catch (...) {
            // The already-published reservation remains durable when the
            // transition store itself cannot be read during terminalization.
        }
    };
    if (std::chrono::steady_clock::now() >= deadline) control->cancel(CancellationCause::Timeout);

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
    std::optional<std::string>            resume_checkpoint_content_id;
    RunOutcome                            outcome;
    try {
        const auto control_source = control->materialized->bundle.control_source();
        if (checkpoint_id) {
            resume_checkpoint = inspect_checkpoint(*control, *checkpoint_id);
            if (const auto durable = control->transitions->load(
                    control->owner_scope, control->run_id);
                durable && durable->exact_checkpoint() &&
                durable->exact_checkpoint()->checkpoint_id == *checkpoint_id) {
                resume_checkpoint_content_id = durable->exact_checkpoint_content_id();
            }
            bool content_valid = true;
            if (resume_checkpoint_content_id) {
                const auto stored = control->checkpoints->load_by_id(*checkpoint_id);
                content_valid = stored &&
                    graph_migration_checkpoint_content_id(*stored) ==
                        *resume_checkpoint_content_id;
            }
            const bool resume_valid =
                resume_checkpoint &&
                content_valid &&
                (control_source ? checkpoint_matches_generation(*control, *resume_checkpoint)
                                : checkpoint_matches(*control, *resume_checkpoint));
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
            throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                     "Admitted Program plan schema is unsupported");
        const auto root_id = typed_plan.root_id();
        if (!typed_plan.find(root_id))
            throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                     "Admitted Program root operation is missing");

        std::mutex                            plan_mutex;
        std::optional<std::string>            resume_id = checkpoint_id;
        std::optional<CoreCheckpointIdentity> last_root_checkpoint;
        std::map<std::string, std::uint64_t>  spawn_occurrences;
        auto root_checkpoint = [&]() -> std::optional<CoreCheckpointIdentity> {
            std::lock_guard lock(plan_mutex);
            return last_root_checkpoint;
        };

        struct CoreInvocation {
            graph::RunResult                      result;
            std::optional<CoreCheckpointIdentity> checkpoint;
        };

        auto update_peak = [&](std::size_t current) {
            auto previous = peak_concurrency.load(std::memory_order_relaxed);
            while (previous < current && !peak_concurrency.compare_exchange_weak(
                                             previous, current, std::memory_order_relaxed)) {}
        };
        auto charge_operation = [&](std::string_view operation_id) -> std::optional<PlanExecution> {
            std::lock_guard lock(plan_mutex);
            if (operation_count >= control->granted_budget.max_program_operations)
                return plan_failure(
                    ProgramTerminalStatus::BudgetExhausted, "P_PROGRAM_OPERATION_BUDGET",
                    "Program operation budget exhausted", std::string(operation_id));
            ++operation_count;
            return std::nullopt;
        };
        auto reserve_operations = [&](std::string_view operation_id,
                                      std::size_t      count) -> std::optional<PlanExecution> {
            std::lock_guard lock(plan_mutex);
            const auto      limit = control->granted_budget.max_program_operations;
            if (count > limit || operation_count > limit - count)
                return plan_failure(
                    ProgramTerminalStatus::BudgetExhausted, "P_PROGRAM_OPERATION_BUDGET",
                    "Program operation budget exhausted", std::string(operation_id));
            operation_count += count;
            return std::nullopt;
        };

        std::function<std::optional<PlanExecution>(const JavaScriptCommand&, std::string,
                                                   std::size_t)>
            validate_javascript_command;
        validate_javascript_command = [&](const JavaScriptCommand& command,
                                          std::string              operation_id,
                                          std::size_t scope_depth) -> std::optional<PlanExecution> {
            const auto fail = [&](ProgramTerminalStatus status, std::string code,
                                  std::string message) {
                return std::optional<PlanExecution>(
                    plan_failure(status, std::move(code), std::move(message), operation_id));
            };
            if (scope_depth > kMaxJavaScriptStructuredScopeDepth)
                return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
                            "JavaScript structured-concurrency nesting exceeds its bound");

            const auto expected_slot = [](JavaScriptCommandKind kind) -> std::uint32_t {
                switch (kind) {
                    case JavaScriptCommandKind::CallCore:
                        return JAVASCRIPT_IMPORT_SLOT_CALL_CORE;
                    case JavaScriptCommandKind::Spawn:
                        return JAVASCRIPT_IMPORT_SLOT_SPAWN;
                    case JavaScriptCommandKind::Await:
                        return JAVASCRIPT_IMPORT_SLOT_AWAIT;
                    case JavaScriptCommandKind::Join:
                        return JAVASCRIPT_IMPORT_SLOT_JOIN;
                    case JavaScriptCommandKind::Emit:
                        return JAVASCRIPT_IMPORT_SLOT_EMIT;
                    case JavaScriptCommandKind::Checkpoint:
                        return JAVASCRIPT_IMPORT_SLOT_CHECKPOINT;
                    case JavaScriptCommandKind::CancelScope:
                        return JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE;
                    case JavaScriptCommandKind::HostCapability:
                        return std::numeric_limits<std::uint32_t>::max();
                }
                return std::numeric_limits<std::uint32_t>::max();
            };
            if (command.protocol_version() != JavaScriptCommand::PROTOCOL_VERSION ||
                (command.kind() != JavaScriptCommandKind::HostCapability &&
                 command.import_slot() != expected_slot(command.kind())))
                return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                            "JavaScript command protocol or import slot is not admitted");
            if (command.kind() == JavaScriptCommandKind::HostCapability) {
                const auto native =
                    control->materialized->native_bindings.find(command.import_slot());
                if (native == control->materialized->native_bindings.end())
                    return fail(
                        ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_BINDING",
                        "JavaScript host capability import slot is absent from the admitted run");
                try {
                    const auto input = command.arguments().at("input");
                    validate_contract_value(input, native->second.binding.metadata().input_contract,
                                            "Native binding input");
                    if (detail::canonical_json_bytes(input).size() >
                        native->second.binding.metadata().resource_declaration.max_input_bytes)
                        return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                    "Native binding input exceeds its admitted byte limit");
                } catch (const std::exception& error) {
                    return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                error.what());
                }
            }

            const auto arguments = command.arguments();
            try {
                if (command.kind() == JavaScriptCommandKind::CallCore) {
                    if (arguments.at("name").get<std::string>() !=
                        control->materialized->root->core_name)
                        return fail(
                            ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                            "JavaScript command references a Core outside the admitted root");
                } else if (command.kind() == JavaScriptCommandKind::Spawn) {
                    control->validate_child_binding(
                        arguments.at("child_binding").get<std::string>(), operation_id);
                } else if (command.kind() == JavaScriptCommandKind::Await) {
                    const auto child = JavaScriptCommand::from_json(arguments.at("command"));
                    if (auto child_failure = validate_javascript_command(
                            child, operation_id + "/await", scope_depth + 1))
                        return child_failure;
                } else if (command.kind() == JavaScriptCommandKind::Join) {
                    const auto  mode            = arguments.at("mode").get<std::string>();
                    const auto& encoded_members = arguments.at("members");
                    if ((mode != "all" && mode != "race" && mode != "quorum") ||
                        !encoded_members.is_array() || encoded_members.empty() ||
                        encoded_members.size() > 1'000'000u)
                        return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
                                    "JavaScript join member bounds are invalid");
                    if (mode == "race" && encoded_members.size() < 2)
                        return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                    "JavaScript race requires at least two members");
                    if (mode == "quorum") {
                        const auto required =
                            arguments.at("required_successes").get<std::uint64_t>();
                        if (required == 0 || required > encoded_members.size())
                            return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                        "JavaScript quorum bounds are invalid");
                    }
                    if (arguments.contains("max_in_flight")) {
                        const auto max_in_flight =
                            arguments.at("max_in_flight").get<std::uint64_t>();
                        if (max_in_flight == 0 ||
                            max_in_flight > control->granted_budget.max_concurrency)
                            return fail(ProgramTerminalStatus::BudgetExhausted,
                                        "P_CONCURRENCY_BUDGET",
                                        "JavaScript join max_in_flight exceeds its admitted "
                                        "concurrency budget");
                    }
                    for (std::size_t index = 0; index < encoded_members.size(); ++index) {
                        const auto member = JavaScriptCommand::from_json(encoded_members.at(index));
                        if (auto member_failure = validate_javascript_command(
                                member, operation_id + "/member/" + std::to_string(index),
                                scope_depth + 1))
                            return member_failure;
                    }
                } else if (command.kind() == JavaScriptCommandKind::CancelScope) {
                    const auto scope = arguments.at("scope").get<std::string>();
                    if (scope != "run" && scope != "root" && scope != "current" &&
                        scope != "nested")
                        return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE",
                                    "JavaScript cancelScope references an unknown scope");
                }
            } catch (const ProgramDiagnosticError& error) {
                return fail(ProgramTerminalStatus::Failed, error.diagnostic().code,
                            error.diagnostic().message);
            } catch (const std::exception& error) {
                return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND", error.what());
            }
            return std::nullopt;
        };

        auto run_core = [&](std::string operation_id, json core_input,
                             std::optional<std::string> resume, std::string thread_id,
                             std::shared_ptr<graph::CancelToken> operation_token)
            -> asio::awaitable<CoreInvocation> {
            auto next_resume = std::move(resume);
            auto next_input = std::move(core_input);
            for (;;) {
                graph::RunConfig config;
                config.thread_id          = thread_id;
                config.input              = next_resume ? json::object() : next_input;
                config.max_steps          = static_cast<int>(std::min<std::uint64_t>(
                    subtract_saturated(control->granted_budget.max_core_steps,
                                       core_progress->steps()),
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())));
                config.stream_mode        = graph::StreamMode::ALL;
                config.cancel_token       = operation_token;
                config.usage              = usage;
                config.model_token_budget = control->granted_budget.model_tokens;
                config.budget_exhausted   = control->budget_exhausted;

                graph::RunMetadata metadata;
                metadata.deadline    = deadline;
                metadata.run_id      = control->run_id;
                metadata.owner_scope = control->owner_scope;
                metadata.trace_id    = control->trace_id;
                auto operation_checkpoints = control->checkpoints;
                if (next_resume && resume_checkpoint_content_id) {
                    operation_checkpoints =
                        std::make_shared<ExactCheckpointContentStore>(
                            control->checkpoints, *next_resume,
                            *resume_checkpoint_content_id);
                }
                graph::RunResources resources{operation_checkpoints, control->state_store};
                graph::GraphStreamCallback callback =
                    [control, core_progress, operation_id](const graph::GraphEvent& event) {
                        core_progress->observe(event);
                        control->emit(operation_id, ProgramEventKind::Core,
                                      graph::to_typed_event(event));
                    };
                auto safe_point_request = control->graph_safe_point_request();
                graph::RunResult result;
                if (next_resume) {
                    if (safe_point_request) {
                        result = co_await control->materialized->root->engine
                                     ->resume_from_until_safe_point_async(
                                         std::move(config), *next_resume,
                                         safe_point_request, std::move(next_input),
                                         std::move(callback), std::move(metadata),
                                         std::move(resources));
                    } else {
                        result = co_await control->materialized->root->engine->resume_from_async(
                            std::move(config), *next_resume, std::move(next_input),
                            std::move(callback), std::move(metadata), std::move(resources));
                    }
                } else if (safe_point_request) {
                    result = co_await control->materialized->root->engine
                                 ->run_until_safe_point_async(
                                     std::move(config), safe_point_request,
                                     std::move(callback), std::move(metadata),
                                     std::move(resources));
                } else {
                    result = co_await control->materialized->root->engine->run_stream_async(
                        std::move(config), std::move(callback), std::move(metadata),
                        std::move(resources));
                }

                auto checkpoint = inspect_checkpoint_for(*control, result.checkpoint_id, thread_id);
                if (checkpoint && checkpoint->core_thread_id != thread_id) checkpoint.reset();
                if (result.status() != graph::RunStatus::SafePoint) {
                    co_return CoreInvocation{std::move(result), std::move(checkpoint)};
                }
                const auto captured = safe_point_request
                    ? safe_point_request->safe_point()
                    : std::nullopt;
                if (!captured || !checkpoint ||
                    checkpoint->checkpoint_id != captured->checkpoint().id) {
                    throw std::runtime_error(
                        "Graph safe point did not retain its exact durable checkpoint");
                }

                ProgramUsage consumed;
                consumed.wall_time_ms = elapsed_ms(started_at);
                consumed.model_tokens = model_tokens(usage);
                consumed.program_operations = operation_count;
                consumed.core_steps = core_progress->steps();
                consumed.peak_concurrency =
                    peak_concurrency.load(std::memory_order_relaxed);
                co_await control->hold_graph_migration(
                    *captured, settle_budget(control->granted_budget, consumed));
                operation_token->throw_if_cancelled("graph migration safe point");
                next_resume = captured->checkpoint().id;
                resume_checkpoint_content_id =
                    graph_migration_checkpoint_content_id(captured->checkpoint());
                next_input = json::object();
            }
        };

        struct ExpansionTaskRun {
            std::string                   task_id;
            std::uint32_t                 attempt = 0;
            std::string                   attempt_identity;
            std::optional<std::string>    child_run_id;
            ProgramTerminalStatus         status = ProgramTerminalStatus::Failed;
            json                          output = json::object();
            std::optional<ProgramFailure> failure;
        };

        struct ExpansionBatchState {
            std::mutex                                   mutex;
            std::vector<std::optional<ExpansionTaskRun>> results;
            std::vector<std::shared_ptr<RunControl>>     children;
            std::size_t                                  remaining = 0;
            std::shared_ptr<asio::steady_timer>          timer;
            bool                                         failed = false;
        };

        struct ExpansionRecordState {
            explicit ExpansionRecordState(const CompiledTaskGraphFragment& value)
                : record{value, {}, 0, false, false} {}
            std::mutex                              mutex;
            TaskGraphFragmentRecord                 record;
            std::shared_ptr<TaskGraphFragmentStore> store;
        };
        auto execute_expand = [&](const ProgramPlanNode& operation,
                                  json                   state) -> asio::awaitable<PlanExecution> {
            const auto& spec = operation.expand_task_graph();
            if (!spec) {
                co_return plan_failure(ProgramTerminalStatus::Failed, "P_EXPAND_PLAN",
                                       "expand_task_graph has no typed expansion contract",
                                       operation.id());
            }
            const auto                            store = control->task_graph_fragments();
            std::shared_ptr<ExpansionRecordState> durable;
            if (store) {
                try {
                    const auto recovered = store->find_published_lineage(
                        control->owner_scope, control->run_id, operation.id());
                    if (recovered) {
                        const auto& fragment = recovered->fragment;
                        if (!recovered->published ||
                            fragment.owner_scope() != control->owner_scope ||
                            fragment.parent_run_id() != control->run_id ||
                            fragment.expansion_operation_id() != operation.id() ||
                            fragment.parent_program_version_id() != control->program_version_id ||
                            fragment.child_depth() != control->persisted_invocation.child_depth) {
                            co_return plan_failure(
                                ProgramTerminalStatus::Failed, "P_EXPAND_RECOVERY",
                                "Stored task graph fragment does not match the parent operation",
                                operation.id());
                        }
                        durable         = std::make_shared<ExpansionRecordState>(fragment);
                        durable->record = *recovered;
                        durable->store  = store;
                    }
                } catch (const std::exception& error) {
                    co_return plan_failure(
                        ProgramTerminalStatus::Failed, "P_EXPAND_RECOVERY",
                        std::string("Stored task graph fragment recovery failed: ") + error.what(),
                        operation.id());
                }
            }
            if (!durable) {
                const auto policy = control->task_graph_policy(operation.id());
                if (!store || !policy) {
                    co_return plan_failure(
                        ProgramTerminalStatus::Failed, "P_EXPAND_UNAVAILABLE",
                        "expand_task_graph requires a durable fragment store and host policy",
                        operation.id());
                }
                json        proposal;
                const auto& source = spec->proposal_source;
                if (source.contains("inline")) {
                    proposal = source.at("inline");
                } else {
                    const auto selected =
                        json_path_value(state, source.at("field").get<std::string>());
                    if (!selected || !selected->is_object()) {
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_EXPAND_SOURCE",
                            "expand_task_graph proposal source did not resolve to an object",
                            operation.id());
                    }
                    proposal = *selected;
                }

                const auto bounded = [](std::uint64_t requested, std::uint64_t ceiling) {
                    return ceiling == 0 ? requested : std::min(requested, ceiling);
                };
                auto limits      = policy->limits;
                limits.max_tasks = bounded(spec->max_tasks, policy->limits.max_tasks);
                limits.max_edges = bounded(spec->max_edges, policy->limits.max_edges);
                limits.max_depth = bounded(spec->max_depth, policy->limits.max_depth);
                if (limits.max_tasks == 0 || limits.max_edges == 0 || limits.max_depth == 0) {
                    co_return plan_failure(ProgramTerminalStatus::BudgetExhausted, "P_EXPAND_LIMIT",
                                           "expand_task_graph has no positive host policy ceiling",
                                           operation.id());
                }
                limits.max_tasks =
                    std::min(limits.max_tasks, control->granted_budget.max_total_children == 0
                                                   ? limits.max_tasks
                                                   : control->granted_budget.max_total_children);

                TaskGraphProposalOptions proposal_options;
                proposal_options.source_id          = control->run_id + ":" + operation.id();
                proposal_options.limits             = limits;
                proposal_options.template_allowlist = policy->template_allowlist;

                TaskGraphProposal parsed_proposal = [&]() -> TaskGraphProposal {
                    try {
                        return TaskGraphProposal::parse(proposal, proposal_options);
                    } catch (const std::exception& error) {
                        throw_runtime_diagnostic(
                            "P_EXPAND_REJECTED",
                            std::string("expand_task_graph proposal was rejected: ") + error.what(),
                            json{{"operation_id", operation.id()}});
                    }
                }();
                const auto recovered =
                    store->find_published(control->owner_scope, control->run_id, operation.id(),
                                          parsed_proposal.proposal_hash());
                if (recovered) {
                    if (!recovered->published) {
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_EXPAND_RECOVERY",
                            "Stored task graph fragment is not durably published", operation.id());
                    }
                    durable         = std::make_shared<ExpansionRecordState>(recovered->fragment);
                    durable->record = *recovered;
                    durable->store  = store;
                } else {
                    const auto compile_limit = std::min<std::uint64_t>(
                        spec->max_dynamic_compiles, control->granted_budget.max_dynamic_compiles);
                    {
                        std::lock_guard lock(plan_mutex);
                        if (dynamic_compile_count >= compile_limit ||
                            !control->consume_dynamic_compile()) {
                            co_return plan_failure(
                                ProgramTerminalStatus::BudgetExhausted, "P_DYNAMIC_COMPILE_BUDGET",
                                "expand_task_graph dynamic compilation budget exhausted",
                                operation.id());
                        }
                        ++dynamic_compile_count;
                    }

                    TaskGraphFragmentCompileOptions compile_options;
                    compile_options.owner_scope               = control->owner_scope;
                    compile_options.parent_run_id             = control->run_id;
                    compile_options.expansion_operation_id    = operation.id();
                    compile_options.parent_program_version_id = control->program_version_id;
                    compile_options.child_depth = control->persisted_invocation.child_depth;
                    compile_options.limits      = limits;
                    compile_options.limits.max_tasks =
                        std::min(compile_options.limits.max_tasks, spec->max_tasks);
                    compile_options.limits.max_edges =
                        std::min(compile_options.limits.max_edges, spec->max_edges);
                    compile_options.limits.max_depth =
                        std::min(compile_options.limits.max_depth, spec->max_depth);
                    compile_options.remaining_budget = BudgetLimits{
                        control->granted_budget.wall_time_ms,
                        control->granted_budget.model_tokens,
                        control->granted_budget.monetary_microunits,
                        std::min<std::uint32_t>(control->granted_budget.max_concurrency,
                                                spec->max_concurrency),
                        control->granted_budget.max_program_operations,
                        control->granted_budget.max_core_steps,
                        static_cast<std::uint32_t>(
                            std::min<std::uint64_t>(control->granted_budget.max_dynamic_compiles,
                                                    spec->max_dynamic_compiles)),
                        control->granted_budget.max_child_depth,
                        std::min<std::uint64_t>(control->granted_budget.max_total_children,
                                                spec->max_total_children)};
                    compile_options.template_allowlist = policy->template_allowlist;
                    compile_options.parent_capabilities =
                        control->materialized->bundle.capability_effect_closure().capabilities;
                    compile_options.parent_effects =
                        control->materialized->bundle.capability_effect_closure().effects;
                    compile_options.host_capabilities = policy->host_capabilities;
                    compile_options.host_effects      = policy->host_effects;

                    std::optional<CompiledTaskGraphFragment> compiled_fragment;
                    try {
                        compiled_fragment.emplace(
                            TaskGraphFragmentCompiler::compile(parsed_proposal, compile_options));
                    } catch (const std::exception& error) {
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_EXPAND_REJECTED",
                            std::string("expand_task_graph proposal was rejected: ") + error.what(),
                            operation.id());
                    }

                    durable = std::make_shared<ExpansionRecordState>(*compiled_fragment);
                    for (const auto& task : compiled_fragment->tasks()) {
                        durable->record.tasks.push_back(TaskGraphTaskRecord{
                            task.task_id, task.operation_id, TaskGraphTaskState::Pending, 0,
                            std::nullopt, std::nullopt});
                    }
                    try {
                        const auto publication = store->publish(durable->record);
                        if (publication == TaskGraphPublishResult::Conflict ||
                            publication == TaskGraphPublishResult::Missing) {
                            co_return plan_failure(
                                ProgramTerminalStatus::Failed, "P_EXPAND_PUBLICATION",
                                "expand_task_graph fragment publication conflicted",
                                operation.id());
                        }
                        const auto loaded = store->load(compiled_fragment->fragment_id());
                        if (!loaded || !loaded->published) {
                            co_return plan_failure(
                                ProgramTerminalStatus::Failed, "P_EXPAND_PUBLICATION",
                                "expand_task_graph fragment was not durably published",
                                operation.id());
                        }
                        durable->record = *loaded;
                        durable->store  = store;
                    } catch (const std::exception& error) {
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_EXPAND_PUBLICATION",
                            std::string("expand_task_graph publication failed: ") + error.what(),
                            operation.id());
                    }
                }
            }
            const auto& fragment = durable->record.fragment;

            auto failure_json = [](const std::optional<ProgramFailure>& failure) {
                if (!failure) return json::object();
                return json{{"code", failure->code},
                            {"message", failure->message},
                            {"operation_id", failure->operation_id},
                            {"core_node", failure->core_node},
                            {"attempts", failure->attempts},
                            {"witness", failure->witness}};
            };
            auto update_task = [&](std::string_view task_id, TaskGraphTaskState task_state,
                                   std::uint32_t attempt, std::optional<json> output,
                                   std::optional<json>        failure,
                                   std::optional<std::string> attempt_identity,
                                   std::optional<std::string> child_run_id) {
                if (task_state != TaskGraphTaskState::Cancelled &&
                    (control->cancellation_cause() != CancellationCause::None ||
                     control->try_result().has_value()))
                    return;
                std::lock_guard lock(durable->mutex);
                if (task_state != TaskGraphTaskState::Cancelled &&
                    (control->cancellation_cause() != CancellationCause::None ||
                     control->try_result().has_value()))
                    return;
                auto candidate = durable->record;
                auto found =
                    std::find_if(candidate.tasks.begin(), candidate.tasks.end(),
                                 [task_id](const auto& task) { return task.task_id == task_id; });
                if (found == candidate.tasks.end())
                    throw_runtime_diagnostic("P_EXPAND_RECORD",
                                             "Published fragment task identity is missing",
                                             json{{"task_id", std::string(task_id)}});
                if (attempt < found->attempt)
                    throw_runtime_diagnostic("P_EXPAND_ATTEMPT",
                                             "Task attempt identity moved backwards",
                                             json{{"task_id", std::string(task_id)},
                                                  {"previous_attempt", found->attempt},
                                                  {"requested_attempt", attempt}});
                found->state   = task_state;
                found->attempt = attempt;
                if (attempt_identity) found->attempt_identity = std::move(*attempt_identity);
                if (child_run_id) found->child_run_id = std::move(child_run_id);
                found->output       = std::move(output);
                found->failure      = std::move(failure);
                candidate.revision  = durable->record.revision + 1;
                candidate.published = true;
                const auto result   = durable->store->compare_update(
                    durable->record.fragment.fragment_id(), durable->record.revision, candidate);
                if (result != TaskGraphPublishResult::Published &&
                    result != TaskGraphPublishResult::AlreadyPresent)
                    throw_runtime_diagnostic(
                        "P_EXPAND_CAS", "Published fragment task update lost its revision race",
                        json{{"task_id", std::string(task_id)},
                             {"expected_revision", durable->record.revision}});
                if (result == TaskGraphPublishResult::AlreadyPresent) {
                    const auto winner =
                        durable->store->load(durable->record.fragment.fragment_id());
                    if (!winner)
                        throw_runtime_diagnostic("P_EXPAND_CAS",
                                                 "Published task update winner disappeared",
                                                 json{{"task_id", std::string(task_id)}});
                    durable->record = *winner;
                } else {
                    durable->record = std::move(candidate);
                }
            };

            std::map<std::string, TaskGraphTaskState, std::less<>> states;
            std::map<std::string, std::uint32_t, std::less<>>      attempts;
            std::map<std::string, std::string, std::less<>>        attempt_identities;
            std::map<std::string, std::string, std::less<>>        child_run_ids;
            std::map<std::string, json, std::less<>>               outputs;
            std::map<std::string, json, std::less<>>               failures;
            for (const auto& record : durable->record.tasks) {
                states[record.task_id]   = record.state;
                attempts[record.task_id] = record.attempt;
                if (record.attempt != 0) {
                    const auto expected = task_attempt_identity(fragment.fragment_id(),
                                                                record.task_id, record.attempt);
                    if (!record.attempt_identity.empty() && record.attempt_identity != expected) {
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_EXPAND_RECORD",
                            "Published task record has an unstable attempt identity",
                            operation.id());
                    }
                    attempt_identities[record.task_id] =
                        record.attempt_identity.empty() ? expected : record.attempt_identity;
                }
                if (record.child_run_id) child_run_ids[record.task_id] = *record.child_run_id;
                if (record.output) outputs[record.task_id] = *record.output;
                if (record.failure) failures[record.task_id] = *record.failure;
            }

            auto make_output = [&]() {
                json result{{"fragment_id", durable->record.fragment.fragment_id()},
                            {"tasks", json::array()}};
                for (const auto& task : fragment.tasks()) {
                    json item{{"task_id", task.task_id},
                              {"operation_id", task.operation_id},
                              {"state", std::string(to_string(states[task.task_id]))}};
                    if (attempts[task.task_id] != 0) item["attempt"] = attempts[task.task_id];
                    if (child_run_ids.contains(task.task_id))
                        item["child_run_id"] = child_run_ids[task.task_id];
                    if (outputs.contains(task.task_id)) item["output"] = outputs[task.task_id];
                    if (failures.contains(task.task_id)) item["failure"] = failures[task.task_id];
                    result["tasks"].push_back(std::move(item));
                }
                return result;
            };

            if (durable->record.terminal) {
                PlanExecution replay;
                replay.output = make_output();
                for (const auto& task : fragment.tasks()) {
                    if (states[task.task_id] == TaskGraphTaskState::Failed) {
                        replay.status  = ProgramTerminalStatus::Failed;
                        replay.failure = ProgramFailure{"P_TASK_FAILED",
                                                        "A dynamic task failed",
                                                        task.operation_id,
                                                        "",
                                                        0,
                                                        failures[task.task_id]};
                        break;
                    }
                    if (states[task.task_id] == TaskGraphTaskState::Cancelled) {
                        replay.status  = ProgramTerminalStatus::Cancelled;
                        replay.failure = ProgramFailure{"P_TASK_CANCELLED",
                                                        "Dynamic task graph was cancelled",
                                                        operation.id(),
                                                        "",
                                                        0,
                                                        failures[task.task_id]};
                    }
                }
                co_return replay;
            }

            auto run_task = [&](const CompiledTaskGraphTask&                task,
                                const std::shared_ptr<ExpansionBatchState>& batch_state)
                -> asio::awaitable<ExpansionTaskRun> {
                ExpansionTaskRun result;
                result.task_id          = task.task_id;
                result.attempt          = attempts[task.task_id];
                result.attempt_identity = attempt_identities[task.task_id];
                json child_input        = state;
                if (!child_input.is_object()) child_input = json::object();
                for (const auto& binding : task.input_bindings) {
                    const auto producer = outputs.find(binding.from.task_id);
                    if (producer == outputs.end()) {
                        result.failure =
                            ProgramFailure{"P_TASK_INPUT",
                                           "Task dependency output is unavailable",
                                           task.operation_id,
                                           "",
                                           0,
                                           json{{"producer_task_id", binding.from.task_id}}};
                        co_return result;
                    }
                    const auto value = json_path_value(producer->second, binding.from.field);
                    if (!value || !assign_json_path(child_input, binding.to.field, *value)) {
                        result.failure =
                            ProgramFailure{"P_TASK_INPUT",
                                           "Task input binding could not be materialized",
                                           task.operation_id,
                                           "",
                                           0,
                                           json{{"producer_task_id", binding.from.task_id},
                                                {"source_field", binding.from.field},
                                                {"target_field", binding.to.field}}};
                        co_return result;
                    }
                }
                if (control->cancellation_cause() != CancellationCause::None) {
                    result.status  = ProgramTerminalStatus::Cancelled;
                    result.failure = ProgramFailure{"P_TASK_CANCELLED",
                                                    "Parent expansion was cancelled",
                                                    task.operation_id,
                                                    "",
                                                    0,
                                                    json::object()};
                    co_return result;
                }
                try {
                    auto child = control->launch_child(task.child_binding, std::move(child_input),
                                                       operation.id(), result.attempt_identity,
                                                       task.reserved_budget);
                    result.child_run_id = child->run_id;
                    {
                        std::lock_guard lock(batch_state->mutex);
                        batch_state->children.push_back(child);
                    }
                    const auto child_result = co_await child->wait_async();
                    result.status           = child_result.status();
                    result.output           = child_result.output();
                    result.failure          = child_result.failure();
                    if (result.status == ProgramTerminalStatus::Completed && !result.failure) {
                        result.failure = validate_task_output(task, result.output);
                        if (result.failure) result.status = ProgramTerminalStatus::Failed;
                    }
                } catch (const ProgramDiagnosticError& error) {
                    const auto& diagnostic = error.diagnostic();
                    result.failure         = ProgramFailure{
                        diagnostic.code,   diagnostic.message, task.operation_id, "", 0,
                        diagnostic.witness};
                } catch (const std::exception& error) {
                    result.failure = ProgramFailure{
                        "P_TASK_DISPATCH", error.what(), task.operation_id, "", 0, json::object()};
                }
                co_return result;
            };

            for (const auto& task : fragment.tasks()) {
                if (states[task.task_id] == TaskGraphTaskState::Completed) continue;
                if (states[task.task_id] == TaskGraphTaskState::Failed ||
                    states[task.task_id] == TaskGraphTaskState::Cancelled) {
                    continue;
                }
                states[task.task_id] = TaskGraphTaskState::Pending;
            }

            while (true) {
                bool all_terminal = true;
                for (const auto& task : fragment.tasks()) {
                    const auto state_value = states[task.task_id];
                    if (state_value != TaskGraphTaskState::Completed &&
                        state_value != TaskGraphTaskState::Failed &&
                        state_value != TaskGraphTaskState::Cancelled) {
                        all_terminal = false;
                        break;
                    }
                }
                if (all_terminal) break;

                if (control->cancellation_cause() != CancellationCause::None) {
                    for (const auto& task : fragment.tasks()) {
                        if (states[task.task_id] == TaskGraphTaskState::Completed ||
                            states[task.task_id] == TaskGraphTaskState::Failed ||
                            states[task.task_id] == TaskGraphTaskState::Cancelled)
                            continue;
                        states[task.task_id] = TaskGraphTaskState::Cancelled;
                        const auto failure   = json{{"code", "P_TASK_CANCELLED"},
                                                    {"message", "Parent expansion was cancelled"},
                                                    {"operation_id", task.operation_id}};
                        update_task(task.task_id, TaskGraphTaskState::Cancelled,
                                    attempts[task.task_id], std::nullopt, failure,
                                    attempt_identities[task.task_id],
                                    child_run_ids.contains(task.task_id)
                                        ? std::optional<std::string>{child_run_ids[task.task_id]}
                                        : std::nullopt);
                        failures[task.task_id] = failure;
                    }
                    break;
                }

                std::vector<const CompiledTaskGraphTask*> ready;
                for (const auto& task : fragment.tasks()) {
                    if (states[task.task_id] != TaskGraphTaskState::Pending &&
                        states[task.task_id] != TaskGraphTaskState::Active)
                        continue;
                    bool dependency_failed  = false;
                    bool dependency_waiting = false;
                    for (const auto& dependency : task.depends_on) {
                        const auto dependency_state = states[dependency];
                        if (dependency_state == TaskGraphTaskState::Failed ||
                            dependency_state == TaskGraphTaskState::Cancelled) {
                            dependency_failed = true;
                            break;
                        }
                        if (dependency_state != TaskGraphTaskState::Completed)
                            dependency_waiting = true;
                    }
                    if (dependency_failed) {
                        states[task.task_id] = TaskGraphTaskState::Failed;
                        const auto failure =
                            json{{"code", "P_TASK_DEPENDENCY"},
                                 {"message", "Task dependency did not complete successfully"},
                                 {"operation_id", task.operation_id}};
                        update_task(task.task_id, TaskGraphTaskState::Failed,
                                    attempts[task.task_id], std::nullopt, failure,
                                    attempt_identities[task.task_id],
                                    child_run_ids.contains(task.task_id)
                                        ? std::optional<std::string>{child_run_ids[task.task_id]}
                                        : std::nullopt);
                        failures[task.task_id] = failure;
                        continue;
                    }
                    if (!dependency_waiting) ready.push_back(&task);
                }
                if (ready.empty()) {
                    const auto resolved = std::all_of(
                        fragment.tasks().begin(), fragment.tasks().end(), [&](const auto& task) {
                            const auto state_value = states[task.task_id];
                            return state_value == TaskGraphTaskState::Completed ||
                                   state_value == TaskGraphTaskState::Failed ||
                                   state_value == TaskGraphTaskState::Cancelled;
                        });
                    if (resolved) break;
                    co_return plan_failure(ProgramTerminalStatus::Failed, "P_EXPAND_SCHEDULER",
                                           "Published task graph has no dependency-ready task",
                                           operation.id());
                }

                const auto batch_width = std::max<std::uint32_t>(
                    1, std::min<std::uint32_t>({spec->max_concurrency,
                                                control->granted_budget.max_concurrency,
                                                static_cast<std::uint32_t>(ready.size())}));
                std::size_t offset = 0;
                while (offset < ready.size()) {
                    const auto count = std::min<std::size_t>(batch_width, ready.size() - offset);
                    auto       batch_state = std::make_shared<ExpansionBatchState>();
                    batch_state->results.resize(count);
                    batch_state->remaining         = count;
                    const auto executor            = co_await asio::this_coro::executor;
                    const auto completion_executor = asio::make_strand(executor);
                    batch_state->timer = std::make_shared<asio::steady_timer>(completion_executor);
                    batch_state->timer->expires_at((asio::steady_timer::time_point::max)());
                    for (std::size_t index = 0; index < count; ++index) {
                        const auto* task         = ready[offset + index];
                        auto&       task_attempt = attempts[task->task_id];
                        if (task_attempt == std::numeric_limits<std::uint32_t>::max()) {
                            states[task->task_id] = TaskGraphTaskState::Failed;
                            const auto failure =
                                ProgramFailure{"P_TASK_RETRY_EXHAUSTED",
                                               "Task graph retry identity limit was exhausted",
                                               task->operation_id,
                                               "",
                                               0,
                                               json{{"operation_id", task->operation_id}}};
                            failures[task->task_id] = failure_json(failure);
                            update_task(
                                task->task_id, TaskGraphTaskState::Failed, task_attempt,
                                std::nullopt, failures[task->task_id],
                                attempt_identities[task->task_id],
                                child_run_ids.contains(task->task_id)
                                    ? std::optional<std::string>{child_run_ids[task->task_id]}
                                    : std::nullopt);
                            ExpansionTaskRun exhausted;
                            exhausted.task_id          = task->task_id;
                            exhausted.attempt          = task_attempt;
                            exhausted.attempt_identity = attempt_identities[task->task_id];
                            exhausted.status           = ProgramTerminalStatus::Failed;
                            exhausted.failure          = failure;
                            bool complete              = false;
                            {
                                std::lock_guard lock(batch_state->mutex);
                                batch_state->results[index] = std::move(exhausted);
                                complete                    = --batch_state->remaining == 0;
                            }
                            if (complete) batch_state->timer->cancel();
                            continue;
                        }
                        if (task_attempt == 0) ++task_attempt;
                        auto& identity = attempt_identities[task->task_id];
                        if (identity.empty())
                            identity = task_attempt_identity(fragment.fragment_id(), task->task_id,
                                                             task_attempt);
                        states[task->task_id] = TaskGraphTaskState::Active;
                        update_task(task->task_id, TaskGraphTaskState::Active, task_attempt,
                                    std::nullopt, std::nullopt, identity,
                                    child_run_ids.contains(task->task_id)
                                        ? std::optional<std::string>{child_run_ids[task->task_id]}
                                        : std::nullopt);
                        asio::co_spawn(
                            executor, run_task(*task, batch_state),
                            asio::bind_executor(
                                completion_executor, [batch_state, index](std::exception_ptr error,
                                                                          ExpansionTaskRun result) {
                                    bool complete = false;
                                    {
                                        std::lock_guard lock(batch_state->mutex);
                                        if (error) {
                                            result.status  = ProgramTerminalStatus::Failed;
                                            result.failure = ProgramFailure{"P_TASK_DISPATCH",
                                                                            "Task coroutine failed",
                                                                            result.task_id,
                                                                            "",
                                                                            0,
                                                                            json::object()};
                                        }
                                        batch_state->results[index] = std::move(result);
                                        complete                    = --batch_state->remaining == 0;
                                    }
                                    if (complete) batch_state->timer->cancel();
                                }));
                    }
                    co_await asio::co_spawn(
                        completion_executor,
                        [batch_state]() -> asio::awaitable<void> {
                            asio::error_code error;
                            co_await         batch_state->timer->async_wait(
                                asio::redirect_error(asio::use_awaitable, error));
                        },
                        asio::use_awaitable);
                    std::vector<std::optional<ExpansionTaskRun>> results;
                    {
                        std::lock_guard lock(batch_state->mutex);
                        results = std::move(batch_state->results);
                    }
                    for (auto& maybe_result : results) {
                        if (!maybe_result) continue;
                        auto&      task_result = *maybe_result;
                        const bool completed =
                            task_result.status == ProgramTerminalStatus::Completed &&
                            !task_result.failure;
                        const auto child_id = task_result.child_run_id;
                        if (child_id) child_run_ids[task_result.task_id] = *child_id;
                        if (completed) {
                            states[task_result.task_id]  = TaskGraphTaskState::Completed;
                            outputs[task_result.task_id] = task_result.output;
                            update_task(task_result.task_id, TaskGraphTaskState::Completed,
                                        task_result.attempt, task_result.output, std::nullopt,
                                        task_result.attempt_identity, child_id);
                        } else {
                            const auto cancelled =
                                task_result.status == ProgramTerminalStatus::Cancelled ||
                                task_result.status == ProgramTerminalStatus::TimedOut;
                            states[task_result.task_id] = cancelled ? TaskGraphTaskState::Cancelled
                                                                    : TaskGraphTaskState::Failed;
                            const auto failure          = failure_json(task_result.failure);
                            failures[task_result.task_id] = failure;
                            update_task(task_result.task_id, states[task_result.task_id],
                                        task_result.attempt, std::nullopt, failure,
                                        task_result.attempt_identity, child_id);
                        }
                    }
                    bool failed = false;
                    for (const auto& task : fragment.tasks()) {
                        failed = failed || states[task.task_id] == TaskGraphTaskState::Failed ||
                                 states[task.task_id] == TaskGraphTaskState::Cancelled;
                    }
                    if (failed && spec->failure_policy == ProgramTaskGraphFailurePolicy::FailFast) {
                        for (const auto& task : fragment.tasks()) {
                            if (states[task.task_id] == TaskGraphTaskState::Pending ||
                                states[task.task_id] == TaskGraphTaskState::Active) {
                                states[task.task_id] = TaskGraphTaskState::Cancelled;
                                const auto failure =
                                    json{{"code", "P_TASK_FAIL_FAST"},
                                         {"message",
                                          "Task graph fail-fast policy cancelled pending work"},
                                         {"operation_id", task.operation_id}};
                                failures[task.task_id] = failure;
                                update_task(
                                    task.task_id, TaskGraphTaskState::Cancelled,
                                    attempts[task.task_id], std::nullopt, failure,
                                    attempt_identities[task.task_id],
                                    child_run_ids.contains(task.task_id)
                                        ? std::optional<std::string>{child_run_ids[task.task_id]}
                                        : std::nullopt);
                            }
                        }
                    }
                    offset += count;
                    if (failed && spec->failure_policy == ProgramTaskGraphFailurePolicy::FailFast)
                        break;
                }
            }

            bool                          failed    = false;
            bool                          cancelled = false;
            std::optional<ProgramFailure> first_failure;
            for (const auto& task : fragment.tasks()) {
                if (states[task.task_id] == TaskGraphTaskState::Failed) {
                    failed = true;
                    if (!first_failure)
                        first_failure = ProgramFailure{"P_TASK_FAILED",
                                                       "A dynamic task failed",
                                                       task.operation_id,
                                                       "",
                                                       0,
                                                       failures[task.task_id]};
                } else if (states[task.task_id] == TaskGraphTaskState::Cancelled) {
                    cancelled = true;
                }
            }
            {
                std::lock_guard lock(durable->mutex);
                auto            candidate = durable->record;
                candidate.terminal        = true;
                candidate.published       = true;
                candidate.revision        = durable->record.revision + 1;
                const auto result         = durable->store->compare_update(
                    durable->record.fragment.fragment_id(), durable->record.revision, candidate);
                if (result != TaskGraphPublishResult::Published &&
                    result != TaskGraphPublishResult::AlreadyPresent)
                    throw_runtime_diagnostic(
                        "P_EXPAND_CAS", "Dynamic fragment terminal update lost its revision race");
                if (result == TaskGraphPublishResult::AlreadyPresent) {
                    const auto winner =
                        durable->store->load(durable->record.fragment.fragment_id());
                    if (!winner)
                        throw_runtime_diagnostic("P_EXPAND_CAS",
                                                 "Published terminal fragment disappeared");
                    durable->record = *winner;
                } else {
                    durable->record = std::move(candidate);
                }
            }
            PlanExecution result;
            result.output = make_output();
            if (failed) {
                result.status  = ProgramTerminalStatus::Failed;
                result.failure = std::move(first_failure);
            } else if (cancelled) {
                result.status  = ProgramTerminalStatus::Cancelled;
                result.failure = ProgramFailure{
                    "P_TASK_CANCELLED", "Dynamic task graph was cancelled", operation.id(), "", 0,
                    json::object()};
            }
            co_return result;
        };

        std::function<asio::awaitable<PlanExecution>(std::string, json, std::string, std::uint32_t,
                                                     std::shared_ptr<graph::CancelToken>)>
            execute;
        execute = [&](std::string operation_id, json state, std::string thread_id,
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
            const auto  op        = dispatch.operation;
            if (operation_token->is_cancelled()) {
                co_return plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                       "Program operation cancelled before dispatch", operation_id);
            }
            if (op != ProgramOperationKind::CallCore) {
                if (auto failure = charge_operation(operation_id)) co_return std::move(*failure);
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
                            co_return plan_failure(ProgramTerminalStatus::BudgetExhausted,
                                                   "P_PROGRAM_OPERATION_BUDGET",
                                                   "Program operation budget exhausted",
                                                   operation_id);
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
                       !active_concurrency.compare_exchange_weak(active, active + 1,
                                                                 std::memory_order_relaxed,
                                                                 std::memory_order_relaxed)) {}
                if (active >= control->granted_budget.max_concurrency)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                        "Program Core dispatch exceeds its admitted concurrency budget",
                        operation_id);
                update_peak(active + 1);
                CoreInvocation invocation;
                const auto     resume_checkpoint_for_error = resume;
                try {
                    invocation =
                        co_await run_core(operation_id, std::move(state), std::move(resume),
                                          std::move(thread_id), operation_token);
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
                    throw NestedOperationFailure(operation_id, error.what(), error.node_name(),
                                                 static_cast<std::uint32_t>(error.attempts()),
                                                 checkpoint_incompatible);
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

                const auto    core_status = invocation.result.status();
                PlanExecution result;
                result.output          = std::move(invocation.result.output);
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
                    result.status  = ProgramTerminalStatus::BudgetExhausted;
                    result.failure = ProgramFailure{"P_CORE_STEP_BUDGET",
                                                    "Core step budget exhausted",
                                                    operation_id,
                                                    invocation.result.interrupt_node,
                                                    0,
                                                    json::object()};
                }
                co_return result;
            }
            if (op == ProgramOperationKind::Sequence) {
                PlanExecution result;
                result.output = std::move(state);
                for (const auto& child : dispatch.children) {
                    auto child_result = co_await execute(child, result.output, thread_id,
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
                co_return co_await execute(*selected, std::move(state), std::move(thread_id),
                                           child_depth, operation_token);
            }

            if (op == ProgramOperationKind::Loop) {
                PlanExecution result;
                result.output      = std::move(state);
                const auto maximum = *operation.max_iterations();
                for (std::uint64_t iteration = 0;
                     iteration < maximum && condition_matches(result.output, operation.condition());
                     ++iteration) {
                    auto body = co_await execute(*dispatch.body, result.output, thread_id,
                                                 child_depth, operation_token);
                    append_trace(result, std::move(body));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned)
                        co_return result;
                }
                if (condition_matches(result.output, operation.condition())) {
                    auto bounded =
                        plan_failure(ProgramTerminalStatus::BudgetExhausted, "P_LOOP_BOUND",
                                     "Program loop bound exhausted", operation_id);
                    bounded.output          = std::move(result.output);
                    bounded.execution_trace = std::move(result.execution_trace);
                    result                  = std::move(bounded);
                }
                co_return result;
            }

            if (op == ProgramOperationKind::Retry) {
                const auto                   maximum = *operation.max_attempts();
                std::optional<PlanExecution> last_result;
                for (std::uint64_t attempt = 0; attempt < maximum; ++attempt) {
                    try {
                        auto attempt_result = co_await execute(*dispatch.body, state, thread_id,
                                                               child_depth, operation_token);
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
                            auto terminal = plan_failure(ProgramTerminalStatus::Failed,
                                                         "P_RUNTIME_CORE_FAILURE", error.what(),
                                                         error.operation_id);
                            terminal.failure->core_node = error.core_node;
                            terminal.failure->attempts  = static_cast<std::uint32_t>(attempt + 1);
                            co_return terminal;
                        }
                    } catch (const graph::NodeExecutionError& error) {
                        if (attempt + 1 == maximum) {
                            auto terminal =
                                plan_failure(ProgramTerminalStatus::Failed,
                                             "P_RUNTIME_CORE_FAILURE", error.what(), operation_id);
                            terminal.failure->core_node = error.node_name();
                            terminal.failure->attempts  = static_cast<std::uint32_t>(attempt + 1);
                            co_return terminal;
                        }
                    } catch (const std::exception& error) {
                        if (attempt + 1 == maximum) {
                            auto terminal =
                                plan_failure(ProgramTerminalStatus::Failed,
                                             "P_RUNTIME_CORE_FAILURE", error.what(), operation_id);
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
                    std::mutex                                       mutex;
                    std::vector<std::optional<PlanExecution>>        results;
                    std::vector<std::shared_ptr<graph::CancelToken>> branch_tokens;
                    std::exception_ptr                               error;
                    std::size_t                                      remaining = 0;
                    std::shared_ptr<asio::experimental::channel<void(asio::error_code, int)>>
                        completion;
                };
                const auto executor = co_await asio::this_coro::executor;
                const auto                     completion_executor = asio::make_strand(executor);
                auto                           parallel = std::make_shared<ParallelState>();
                parallel->results.resize(branches.size());
                parallel->remaining = branches.size();
                // `try_send` retains this signal when every branch settles before
                // the parent begins receiving it. A cancelled timer would lose that
                // wake-up and leave the parent waiting forever.
                parallel->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);
                parallel->branch_tokens.reserve(branches.size());
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    parallel->branch_tokens.push_back(operation_token->fork());
                }
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    const auto& branch_id     = branches[index];
                    const auto  branch_thread = index == 0
                                                    ? control->core_thread_id
                                                    : operation_thread_id(*control, branch_id);
                    auto        branch_token  = parallel->branch_tokens[index];
                    asio::co_spawn(
                        executor,
                        execute(branch_id, state, branch_thread, child_depth, branch_token),
                        asio::bind_executor(
                            completion_executor,
                            [parallel, index](std::exception_ptr error, PlanExecution result) {
                                bool cancel_siblings = false;
                                bool all_done        = false;
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
                                if (all_done)
                                    (void)parallel->completion->try_send(asio::error_code(), 1);
                            }));
                }
                const auto [completion_error, ignored] =
                    co_await parallel->completion->async_receive(
                        asio::as_tuple(asio::use_awaitable));
                (void)completion_error;
                (void)ignored;

                std::exception_ptr                        parallel_error;
                std::vector<std::optional<PlanExecution>> parallel_results;
                {
                    std::lock_guard lock(parallel->mutex);
                    parallel_error   = parallel->error;
                    parallel_results = std::move(parallel->results);
                }
                if (parallel_error) std::rethrow_exception(parallel_error);

                PlanExecution result;
                result.output = json::array();
                for (auto& branch : parallel_results) {
                    if (!branch)
                        throw_runtime_diagnostic("P_RUNTIME_PARALLEL",
                                                 "Parallel branch completed without a result");
                    result.output.push_back(branch->output);
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(branch->execution_trace.begin()),
                        std::make_move_iterator(branch->execution_trace.end()));
                    if (branch->returned) result.returned = true;
                    if (result.status == ProgramTerminalStatus::Completed &&
                        branch->status != ProgramTerminalStatus::Completed) {
                        result.status    = branch->status;
                        result.interrupt = std::move(branch->interrupt);
                        result.failure   = std::move(branch->failure);
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

                const auto& first  = branches[0];
                const auto& second = branches[1];
                // A race winner is the first completion; if both branches become
                // ready in one scheduler turn, declaration order is the tie break.
                // Wait for the losing coroutine to observe cancellation before this
                // operation returns, so no detached branch outlives the run handle.
                struct RaceState {
                    std::mutex                                       mutex;
                    std::array<std::optional<PlanExecution>, 2>      results;
                    std::array<std::exception_ptr, 2>                errors;
                    std::array<bool, 2>                              finished{};
                    std::vector<std::shared_ptr<graph::CancelToken>> tokens;
                    std::optional<std::size_t>                       winner;
                    std::size_t                                      completed           = 0;
                    bool                                             selection_scheduled = false;
                    std::shared_ptr<asio::experimental::channel<void(asio::error_code, int)>>
                        completion;
                };
                const auto executor = co_await asio::this_coro::executor;
                const auto                     completion_executor = asio::make_strand(executor);
                auto                           race                = std::make_shared<RaceState>();
                // A buffered completion signal remains observable if both branches
                // settle before this parent reaches async_receive.
                race->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);
                race->tokens.push_back(operation_token->fork());
                race->tokens.push_back(operation_token->fork());
                const std::array<std::string, 2> ids{first, second};
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    const auto branch_thread = index == 0
                                                   ? control->core_thread_id
                                                   : operation_thread_id(*control, ids[index]);
                    asio::co_spawn(
                        executor,
                        execute(ids[index], state, branch_thread, child_depth, race->tokens[index]),
                        asio::bind_executor(completion_executor, [race, index, completion_executor](
                                                                     std::exception_ptr error,
                                                                     PlanExecution      result) {
                            bool schedule_selection = false;
                            bool all_done           = false;
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
                                    schedule_selection        = true;
                                }
                                all_done = race->winner && race->completed == race->tokens.size();
                            }
                            if (schedule_selection) {
                                asio::post(completion_executor, [race]() {
                                    std::optional<std::size_t> winner;
                                    bool                       cancel_loser = false;
                                    bool                       all_done     = false;
                                    {
                                        std::lock_guard lock(race->mutex);
                                        if (!race->winner) {
                                            for (std::size_t candidate = 0;
                                                 candidate < race->finished.size(); ++candidate) {
                                                if (race->finished[candidate]) {
                                                    race->winner = candidate;
                                                    cancel_loser = true;
                                                    break;
                                                }
                                            }
                                        }
                                        winner   = race->winner;
                                        all_done = winner && race->completed == race->tokens.size();
                                    }
                                    if (cancel_loser) {
                                        for (std::size_t sibling = 0; sibling < race->tokens.size();
                                             ++sibling)
                                            if (sibling != *winner) race->tokens[sibling]->cancel();
                                    }
                                    if (all_done)
                                        (void)race->completion->try_send(asio::error_code(), 1);
                                });
                            } else if (all_done) {
                                (void)race->completion->try_send(asio::error_code(), 1);
                            }
                        }));
                }
                const auto [completion_error, ignored] =
                    co_await race->completion->async_receive(asio::as_tuple(asio::use_awaitable));
                (void)completion_error;
                (void)ignored;

                std::optional<std::size_t>                  winner;
                std::array<std::optional<PlanExecution>, 2> results;
                std::array<std::exception_ptr, 2>           errors;
                {
                    std::lock_guard lock(race->mutex);
                    winner  = race->winner;
                    results = std::move(race->results);
                    errors  = std::move(race->errors);
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
                const auto&   branches = dispatch.branches;
                const auto    required = *operation.min_success();
                PlanExecution result;
                result.output           = json::array();
                std::uint64_t successes = 0;
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    auto branch_result = co_await execute(
                        branches[index], state, operation_thread_id(*control, branches[index]),
                        child_depth, operation_token);
                    const bool completed = branch_result.status == ProgramTerminalStatus::Completed;
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

            if (op == ProgramOperationKind::ParallelMap) {
                const auto& map_spec = operation.parallel_map();
                if (!map_spec || map_spec->max_in_flight == 0 || map_spec->max_output_bytes == 0)
                    throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                             "parallel_map has an invalid bounded map contract",
                                             json{{"operation_id", operation_id}});
                const auto& map = *map_spec;

                const auto cancellation = [&]() {
                    PlanExecution cancelled = plan_failure(
                        ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                        "Program cancelled while executing parallel_map", operation_id);
                    cancelled.output = json::array();
                    return cancelled;
                };

                json items;
                if (map.item_source == ProgramParallelMapItemSource::Literal) {
                    items = map.literal_items;
                } else {
                    const auto selected = json_path_value(state, map.input_field);
                    if (!selected || !selected->is_array())
                        co_return plan_failure(
                            ProgramTerminalStatus::Failed, "P_PARALLEL_MAP_SOURCE",
                            "parallel_map input item source did not resolve to an array",
                            operation_id);
                    items = std::move(*selected);
                }
                if (!items.is_array())
                    throw_runtime_diagnostic("P_RUNTIME_PLAN",
                                             "parallel_map literal item source is not an array",
                                             json{{"operation_id", operation_id}});
                if (items.size() > map.max_items)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_PARALLEL_MAP_BOUND",
                        "parallel_map item source exceeds its immutable max_items bound",
                        operation_id);
                if (map.max_in_flight > control->granted_budget.max_concurrency)
                    co_return plan_failure(
                        ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                        "parallel_map fan-out exceeds its admitted concurrency budget",
                        operation_id);
                if (operation_token->is_cancelled()) co_return cancellation();

                PlanExecution result;
                result.output = json::array();
                if (items.empty()) {
                    if (result.output.dump().size() > map.max_output_bytes)
                        co_return plan_failure(
                            ProgramTerminalStatus::BudgetExhausted, "P_PARALLEL_MAP_OUTPUT",
                            "parallel_map output exceeds its immutable byte bound", operation_id);
                    co_return result;
                }

                struct ParallelMapState {
                    std::mutex                                mutex;
                    std::vector<std::optional<PlanExecution>> results;
                    std::vector<std::shared_ptr<RunControl>>  children;
                    std::exception_ptr                        error;
                    std::optional<std::size_t>                first_fail_fast_failure;
                    std::size_t                               next_item         = 0;
                    std::size_t                               workers_remaining = 0;
                    bool                                      stop_launching    = false;
                    std::shared_ptr<asio::steady_timer>       timer;
                };

                const auto executor = co_await asio::this_coro::executor;
                const auto                     completion_executor = asio::make_strand(executor);
                const auto worker_count = std::min<std::size_t>(map.max_in_flight, items.size());
                auto       parallel_map = std::make_shared<ParallelMapState>();
                parallel_map->results.resize(items.size());
                parallel_map->workers_remaining = worker_count;
                parallel_map->timer = std::make_shared<asio::steady_timer>(completion_executor);
                parallel_map->timer->expires_at((asio::steady_timer::time_point::max)());

                std::string execution_key_prefix;
                {
                    std::lock_guard lock(plan_mutex);
                    const auto      base    = thread_id + ":" + operation_id + ":parallel-map";
                    const auto      ordinal = spawn_occurrences[base]++;
                    execution_key_prefix    = base + ":" + std::to_string(ordinal);
                }

                auto cancel_children =
                    [](const std::vector<std::shared_ptr<RunControl>>& children) {
                        for (const auto& child : children)
                            if (child) (void)child->cancel(CancellationCause::ParentTerminal);
                    };

                auto worker = [&, parallel_map]() -> asio::awaitable<void> {
                    while (true) {
                        std::size_t item_index = 0;
                        {
                            std::lock_guard lock(parallel_map->mutex);
                            if (parallel_map->stop_launching || operation_token->is_cancelled() ||
                                parallel_map->next_item == items.size())
                                break;
                            item_index = parallel_map->next_item++;
                        }

                        PlanExecution mapped;
                        const auto    selected =
                            json_path_value(items.at(item_index), map.input_from_field);
                        if (!selected) {
                            mapped = plan_failure(
                                ProgramTerminalStatus::Failed, "P_PARALLEL_MAP_INPUT",
                                "parallel_map input binding source did not resolve", operation_id);
                        } else {
                            json child_input = json::object();
                            if (!assign_json_path(child_input, map.input_to_field,
                                                  std::move(*selected))) {
                                mapped = plan_failure(
                                    ProgramTerminalStatus::Failed, "P_PARALLEL_MAP_INPUT",
                                    "parallel_map input binding target could not be assigned",
                                    operation_id);
                            } else {
                                if (operation_token->is_cancelled()) {
                                    mapped = cancellation();
                                } else {
                                    bool reserved = false;
                                    auto active =
                                        active_concurrency.load(std::memory_order_relaxed);
                                    while (active < control->granted_budget.max_concurrency) {
                                        if (active_concurrency.compare_exchange_weak(
                                                active, active + 1, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
                                            reserved = true;
                                            update_peak(active + 1);
                                            break;
                                        }
                                    }
                                    if (!reserved) {
                                        mapped = plan_failure(
                                            ProgramTerminalStatus::BudgetExhausted,
                                            "P_CONCURRENCY_BUDGET",
                                            "parallel_map child dispatch exceeds its admitted "
                                            "concurrency budget",
                                            operation_id);
                                    } else if (operation_token->is_cancelled()) {
                                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                                        mapped = cancellation();
                                    } else {
                                        try {
                                            auto child = control->launch_child(
                                                map.child_binding, std::move(child_input),
                                                operation_id,
                                                execution_key_prefix + ":" +
                                                    std::to_string(item_index),
                                                std::nullopt);
                                            bool cancel_child = false;
                                            {
                                                std::lock_guard lock(parallel_map->mutex);
                                                parallel_map->children.push_back(child);
                                                cancel_child = parallel_map->stop_launching;
                                            }
                                            if (cancel_child)
                                                (void)child->cancel(
                                                    CancellationCause::ParentTerminal);
                                            mapped = plan_result_from_child(
                                                co_await child->wait_async());
                                        } catch (...) {
                                            active_concurrency.fetch_sub(1,
                                                                         std::memory_order_relaxed);
                                            throw;
                                        }
                                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                                    }
                                    if (mapped.status == ProgramTerminalStatus::Completed) {
                                        const auto output =
                                            json_path_value(mapped.output, map.output_from_field);
                                        if (!output) {
                                            auto failure =
                                                plan_failure(ProgramTerminalStatus::Failed,
                                                             "P_PARALLEL_MAP_OUTPUT",
                                                             "parallel_map output binding source "
                                                             "did not resolve",
                                                             operation_id);
                                            failure.execution_trace =
                                                std::move(mapped.execution_trace);
                                            mapped = std::move(failure);
                                        } else {
                                            mapped.output = std::move(*output);
                                        }
                                    }
                                }
                            }
                        }

                        const bool failed = mapped.status != ProgramTerminalStatus::Completed;
                        std::vector<std::shared_ptr<RunControl>> children_to_cancel;
                        {
                            std::lock_guard lock(parallel_map->mutex);
                            parallel_map->results[item_index] = std::move(mapped);
                            if (failed &&
                                map.failure_policy == ProgramParallelMapFailurePolicy::FailFast &&
                                !parallel_map->stop_launching) {
                                parallel_map->stop_launching          = true;
                                parallel_map->first_fail_fast_failure = item_index;
                                children_to_cancel                    = parallel_map->children;
                            }
                        }
                        cancel_children(children_to_cancel);
                    }
                    co_return;
                };

                for (std::size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                    asio::co_spawn(
                        executor, worker(),
                        asio::bind_executor(completion_executor, [parallel_map, cancel_children](
                                                                     std::exception_ptr error) {
                            std::vector<std::shared_ptr<RunControl>> children_to_cancel;
                            bool                                     all_done = false;
                            {
                                std::lock_guard lock(parallel_map->mutex);
                                if (error && !parallel_map->error) {
                                    parallel_map->error          = error;
                                    parallel_map->stop_launching = true;
                                    children_to_cancel           = parallel_map->children;
                                }
                                all_done = --parallel_map->workers_remaining == 0;
                            }
                            cancel_children(children_to_cancel);
                            if (all_done) parallel_map->timer->cancel();
                        }));
                }

                co_await asio::co_spawn(
                    completion_executor,
                    [parallel_map]() -> asio::awaitable<void> {
                        asio::error_code error;
                        co_await         parallel_map->timer->async_wait(
                            asio::redirect_error(asio::use_awaitable, error));
                    },
                    asio::use_awaitable);

                std::exception_ptr                        parallel_error;
                std::vector<std::optional<PlanExecution>> map_results;
                std::optional<std::size_t>                first_fail_fast_failure;
                {
                    std::lock_guard lock(parallel_map->mutex);
                    parallel_error          = parallel_map->error;
                    map_results             = std::move(parallel_map->results);
                    first_fail_fast_failure = parallel_map->first_fail_fast_failure;
                }
                if (parallel_error) std::rethrow_exception(parallel_error);
                if (operation_token->is_cancelled()) {
                    for (auto& mapped : map_results) {
                        if (!mapped) continue;
                        result.execution_trace.insert(
                            result.execution_trace.end(),
                            std::make_move_iterator(mapped->execution_trace.begin()),
                            std::make_move_iterator(mapped->execution_trace.end()));
                    }
                    auto cancelled            = cancellation();
                    cancelled.execution_trace = std::move(result.execution_trace);
                    co_return cancelled;
                }

                std::optional<std::size_t> first_failure =
                    map.failure_policy == ProgramParallelMapFailurePolicy::FailFast
                        ? first_fail_fast_failure
                        : std::optional<std::size_t>{};
                std::uint64_t output_bytes = 2;  // '[' and ']'
                std::size_t   output_items = 0;
                for (std::size_t index = 0; index < map_results.size(); ++index) {
                    auto& mapped = map_results[index];
                    if (!mapped) continue;
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(mapped->execution_trace.begin()),
                        std::make_move_iterator(mapped->execution_trace.end()));
                    if (mapped->status != ProgramTerminalStatus::Completed && !first_failure)
                        first_failure = index;

                    json       output          = mapped->status == ProgramTerminalStatus::Completed
                                                     ? std::move(mapped->output)
                                                     : json();
                    const auto encoded_size    = static_cast<std::uint64_t>(output.dump().size());
                    const auto separator_bytes = output_items == 0 ? 0ULL : 1ULL;
                    if (output_bytes > map.max_output_bytes ||
                        separator_bytes > map.max_output_bytes - output_bytes ||
                        encoded_size > map.max_output_bytes - output_bytes - separator_bytes) {
                        auto failure = plan_failure(
                            ProgramTerminalStatus::BudgetExhausted, "P_PARALLEL_MAP_OUTPUT",
                            "parallel_map output exceeds its immutable byte bound", operation_id);
                        failure.output          = json::array();
                        failure.execution_trace = std::move(result.execution_trace);
                        co_return failure;
                    }
                    output_bytes += separator_bytes + encoded_size;
                    ++output_items;
                    result.output.push_back(std::move(output));
                }

                if (!first_failure) co_return result;
                const auto& failed = map_results[*first_failure];
                if (!failed)
                    throw_runtime_diagnostic(
                        "P_RUNTIME_PARALLEL_MAP",
                        "parallel_map selected a missing failed item result",
                        json{{"operation_id", operation_id}, {"item_index", *first_failure}});
                result.status    = failed->status;
                result.interrupt = std::move(failed->interrupt);
                result.failure   = std::move(failed->failure);
                if (!result.failure) {
                    result.failure = ProgramFailure{"P_PARALLEL_MAP_CHILD",
                                                    "Mapped child did not complete",
                                                    operation_id,
                                                    "",
                                                    0,
                                                    json::object()};
                } else {
                    auto& witness = result.failure->witness;
                    if (!witness.is_object()) witness = json::object();
                    witness["item_index"] = static_cast<std::uint64_t>(*first_failure);
                    if (result.failure->operation_id != operation_id)
                        witness["child_operation_id"] = result.failure->operation_id;
                    result.failure->operation_id = operation_id;
                }
                co_return result;
            }

            if (op == ProgramOperationKind::ExpandTaskGraph) {
                auto result = co_await execute_expand(operation, std::move(state));
                if (result.failure && result.failure->operation_id != operation_id) {
                    auto& witness = result.failure->witness;
                    if (!witness.is_object()) witness = json::object();
                    witness["child_operation_id"] = result.failure->operation_id;
                    result.failure->operation_id  = operation_id;
                }
                result.execution_trace.push_back(operation_id);
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
                    const auto      base    = thread_id + ":" + operation_id;
                    const auto      ordinal = spawn_occurrences[base]++;
                    execution_key           = base + ":" + std::to_string(ordinal);
                }
                auto child = control->launch_child(*operation.child_binding(), std::move(state),
                                                   operation_id, execution_key, std::nullopt);
                PlanExecution result;
                result.output        = json{{"child_run_id", child->run_id},
                                            {"program_version_id", child->program_version_id}};
                result.spawned_child = std::move(child);
                co_return result;
            }

            if (op == ProgramOperationKind::Cancel) {
                control->cancel(CancellationCause::User);
                PlanExecution result;
                result.status = ProgramTerminalStatus::Cancelled;
                result.output = std::move(state);
                result.failure =
                    ProgramFailure{"P_RUNTIME_CANCELLED",
                                   operation.reason().value_or("Program cancelled by plan"),
                                   operation_id,
                                   "",
                                   0,
                                   json{{"scope", operation.scope().value_or("run")}}};
                co_return result;
            }

            if (op == ProgramOperationKind::Await) {
                const auto  child_id        = *dispatch.body;
                const auto* child_operation = typed_plan.find(child_id);
                if (child_operation &&
                    child_operation->operation() == ProgramOperationKind::Spawn) {
                    auto         launched =
                        co_await execute(child_id, std::move(state), std::move(thread_id),
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
                    auto timeout        = std::make_shared<asio::steady_timer>(executor);
                    timeout->expires_after(std::chrono::milliseconds(*operation.timeout_ms()));
                    auto timeout_operation = [timeout]() -> asio::awaitable<void> {
                        asio::error_code error;
                        co_await         timeout->async_wait(
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
                    co_return co_await execute(child_id, std::move(state), std::move(thread_id),
                                               child_depth, operation_token);
                }
                const auto executor = co_await asio::this_coro::executor;
                auto timeout        = std::make_shared<asio::steady_timer>(executor);
                timeout->expires_after(std::chrono::milliseconds(*operation.timeout_ms()));
                auto child_token       = operation_token->fork();
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
                        asio::co_spawn(executor,
                                       execute(child_id, std::move(state), std::move(thread_id),
                                               child_depth, child_token),
                                       asio::deferred),
                        asio::co_spawn(executor, std::move(timeout_operation), asio::deferred))
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
                    auto body = co_await execute(*dispatch.body, state, thread_id, child_depth,
                                                 operation_token);
                    if (body.status != ProgramTerminalStatus::Completed) co_return body;
                    state                = std::move(body.output);
                    PlanExecution result = std::move(body);
                    if (const auto checkpoint = root_checkpoint()) {
                        auto event = control->stage_event(operation_id,
                                                          ProgramEventKind::CheckpointPublished,
                                                          ProgramCheckpointEvent{*checkpoint});
                        control->deliver_event(event);
                    }
                    co_return result;
                }
                PlanExecution result;
                result.output = std::move(state);
                if (const auto checkpoint = root_checkpoint()) {
                    auto event =
                        control->stage_event(operation_id, ProgramEventKind::CheckpointPublished,
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
                result.output   = operation.value();
                result.returned = true;
                co_return result;
            }

            throw_runtime_diagnostic("P_RUNTIME_PLAN", "Admitted Program operation is unsupported",
                                     json{{"operation", std::string(to_string(op))}});
        };

        auto execute_javascript_control =
            [&](JavaScriptGenerator& session) -> asio::awaitable<PlanExecution> {
            const auto executor = co_await                     asio::this_coro::executor;
            std::optional<std::pair<std::string, std::string>> exact_core_resume;
            bool supplied_core_resume_required = false;
            bool supplied_core_resume_consumed = false;

            using JsCommandExecutor = std::function<asio::awaitable<PlanExecution>(
                const JavaScriptCommand&, std::string, std::shared_ptr<graph::CancelToken>,
                std::shared_ptr<JavaScriptScopeState>, std::size_t, bool)>;
            JsCommandExecutor execute_command;

            execute_command = [&](const JavaScriptCommand& command, std::string operation_id,
                                  std::shared_ptr<graph::CancelToken>   operation_token,
                                  std::shared_ptr<JavaScriptScopeState> scope,
                                  std::size_t                           scope_depth,
                                  bool reserve_budget) -> asio::awaitable<PlanExecution> {
                if (scope_depth > kMaxJavaScriptStructuredScopeDepth)
                    co_return plan_failure(
                        ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
                        "JavaScript structured-concurrency nesting exceeds its bound",
                        operation_id);
                if (reserve_budget) {
                    std::size_t command_count = 0;
                    try {
                        command_count = javascript_command_tree_size(command);
                    } catch (const ProgramDiagnosticError& error) {
                        co_return plan_failure(ProgramTerminalStatus::Failed,
                                               error.diagnostic().code, error.diagnostic().message,
                                               operation_id);
                    }
                    if (auto failure = reserve_operations(operation_id, command_count))
                        co_return std::move(*failure);
                    if (auto failure =
                            validate_javascript_command(command, operation_id, scope_depth))
                        co_return std::move(*failure);
                }
                if (operation_token->is_cancelled())
                    co_return plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                           "JavaScript command scope was cancelled before dispatch",
                                           operation_id);

                const auto arguments = command.arguments();
                const auto fail      = [&](ProgramTerminalStatus status, std::string code,
                                      std::string message) {
                    return plan_failure(status, std::move(code), std::move(message), operation_id);
                };
                const auto expected_slot = [](JavaScriptCommandKind kind) -> std::uint32_t {
                    switch (kind) {
                        case JavaScriptCommandKind::CallCore:
                            return JAVASCRIPT_IMPORT_SLOT_CALL_CORE;
                        case JavaScriptCommandKind::Spawn:
                            return JAVASCRIPT_IMPORT_SLOT_SPAWN;
                        case JavaScriptCommandKind::Await:
                            return JAVASCRIPT_IMPORT_SLOT_AWAIT;
                        case JavaScriptCommandKind::Join:
                            return JAVASCRIPT_IMPORT_SLOT_JOIN;
                        case JavaScriptCommandKind::Emit:
                            return JAVASCRIPT_IMPORT_SLOT_EMIT;
                        case JavaScriptCommandKind::Checkpoint:
                            return JAVASCRIPT_IMPORT_SLOT_CHECKPOINT;
                        case JavaScriptCommandKind::CancelScope:
                            return JAVASCRIPT_IMPORT_SLOT_CANCEL_SCOPE;
                        case JavaScriptCommandKind::HostCapability:
                            return std::numeric_limits<std::uint32_t>::max();
                    }
                    return std::numeric_limits<std::uint32_t>::max();
                };
                if (command.protocol_version() != JavaScriptCommand::PROTOCOL_VERSION ||
                    (command.kind() != JavaScriptCommandKind::HostCapability &&
                     command.import_slot() != expected_slot(command.kind())))
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                   "JavaScript command protocol or import slot is not admitted");
                if (command.kind() == JavaScriptCommandKind::HostCapability &&
                    !control->materialized->native_bindings.contains(command.import_slot()))
                    co_return fail(
                        ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_BINDING",
                        "JavaScript host capability import slot is absent from the admitted run");

                if (command.kind() == JavaScriptCommandKind::HostCapability) {
                    const auto native =
                        control->materialized->native_bindings.find(command.import_slot());
                    const auto native_started_at = std::chrono::steady_clock::now();
                    const auto native_deadline   = deadline;
                    if (native_started_at >= native_deadline)
                        co_return fail(ProgramTerminalStatus::TimedOut, "P_RUNTIME_TIMEOUT",
                                       "Native host capability reached its granted run deadline "
                                       "before dispatch");

                    NativeInvocation invocation;
                    try {
                        invocation = native->second.binding.invoke(
                            native_invocation_id(*control, operation_id, command.import_slot()),
                            arguments.at("input"));
                    } catch (const std::exception& error) {
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                       error.what());
                    } catch (...) {
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                       "Native binding invocation setup failed unexpectedly");
                    }

                    if (std::chrono::steady_clock::now() >= native_deadline) {
                        invocation.cancel();
                        co_return fail(ProgramTerminalStatus::TimedOut, "P_RUNTIME_TIMEOUT",
                                       "Native host capability exceeded its granted run deadline");
                    }
                    asio::steady_timer completion_wait(executor);

                    while (!invocation.finished()) {
                        if (operation_token->is_cancelled() ||
                            control->cancel_token->is_cancelled()) {
                            invocation.cancel();
                            co_return fail(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                           "Native host capability was cancelled");
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= native_deadline) {
                            invocation.cancel();
                            co_return fail(
                                ProgramTerminalStatus::TimedOut, "P_RUNTIME_TIMEOUT",
                                "Native host capability exceeded its granted run deadline");
                        }
                        completion_wait.expires_after(std::min(
                            std::chrono::milliseconds(1),
                            std::chrono::duration_cast<std::chrono::milliseconds>(native_deadline -
                                                                                  now)));
                        asio::error_code wait_error;
                        co_await         completion_wait.async_wait(
                            asio::redirect_error(asio::use_awaitable, wait_error));
                    }
                    if (std::chrono::steady_clock::now() >= native_deadline) {
                        invocation.cancel();
                        co_return fail(
                            ProgramTerminalStatus::TimedOut, "P_RUNTIME_TIMEOUT",
                            "Native host capability completed after its granted run deadline");
                    }

                    const auto terminal = invocation.result();
                    if (!terminal)
                        co_return fail(ProgramTerminalStatus::Failed, "P_NATIVE_RESULT_MISSING",
                                       "Native host capability completed without a result");
                    if (terminal->status == NativeInvocationStatus::Success) {
                        PlanExecution result;
                        result.output = terminal->value;
                        co_return result;
                    }
                    if (terminal->status == NativeInvocationStatus::Cancelled)
                        co_return fail(ProgramTerminalStatus::Cancelled,
                                       terminal->diagnostic_code.empty()
                                           ? "P_NATIVE_CANCELLED"
                                           : terminal->diagnostic_code,
                                       terminal->diagnostic_message);
                    co_return fail(ProgramTerminalStatus::Failed,
                                   terminal->diagnostic_code.empty() ? "P_NATIVE_FAILURE"
                                                                     : terminal->diagnostic_code,
                                   terminal->diagnostic_message);
                }

                if (command.kind() == JavaScriptCommandKind::CallCore) {
                    const auto parsed = parse_javascript_call_core_command(command);
                    if (parsed.name != control->materialized->root->core_name)
                        co_return fail(
                            ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                            "JavaScript command references a Core outside the admitted root");
                    auto active = active_concurrency.load(std::memory_order_relaxed);
                    while (active < control->granted_budget.max_concurrency &&
                           !active_concurrency.compare_exchange_weak(active, active + 1,
                                                                     std::memory_order_relaxed,
                                                                     std::memory_order_relaxed)) {}
                    if (active >= control->granted_budget.max_concurrency)
                        co_return fail(
                            ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                            "Program Core dispatch exceeds its admitted concurrency budget");
                    update_peak(active + 1);

                    const auto thread_id = operation_thread_id(*control, operation_id);
                    std::optional<std::string> command_resume;
                    if (exact_core_resume && exact_core_resume->first == operation_id) {
                        command_resume                = exact_core_resume->second;
                        supplied_core_resume_consumed = true;
                    }
                    auto core_input = std::move(parsed.input);
                    if (command_resume && javascript_core_resume_value) {
                        core_input = std::move(*javascript_core_resume_value);
                        javascript_core_resume_value.reset();
                    }
                    CoreInvocation invocation;
                    try {
                        invocation = co_await run_core(operation_id, std::move(core_input),
                                                       std::move(command_resume), thread_id,
                                                       operation_token);
                    } catch (const EventSinkError&) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        throw;
                    } catch (const graph::CancelledException& error) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        if (operation_token->is_cancelled())
                            co_return fail(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                           error.what());
                        throw;
                    } catch (const graph::NodeExecutionError& error) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        PlanExecution result      = fail(ProgramTerminalStatus::Failed,
                                                         "P_RUNTIME_CORE_FAILURE", error.what());
                        result.failure->core_node = error.node_name();
                        result.failure->attempts  = static_cast<std::uint32_t>(error.attempts());
                        co_return result;
                    } catch (const std::exception& error) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        co_return fail(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                       error.what());
                    } catch (...) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        co_return fail(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                       "Unknown Core failure");
                    }
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    if (invocation.checkpoint) {
                        {
                            std::lock_guard lock(plan_mutex);
                            last_root_checkpoint = invocation.checkpoint;
                        }
                        (void)control->stage_event(operation_id,
                                                   ProgramEventKind::CheckpointPublished,
                                                   ProgramCheckpointEvent{*invocation.checkpoint});
                    }

                    PlanExecution result;
                    result.output =
                        javascript_control_response(std::move(invocation.result.output));
                    result.execution_trace = std::move(invocation.result.execution_trace);
                    const auto core_status = invocation.result.status();
                    if (core_status == graph::RunStatus::Interrupted) {
                        result.status = ProgramTerminalStatus::Interrupted;
                        result.interrupt =
                            decode_core_interrupt(*control, operation_id, invocation.result);
                    } else if (core_status == graph::RunStatus::StepLimit) {
                        result = fail(ProgramTerminalStatus::BudgetExhausted, "P_CORE_STEP_BUDGET",
                                      "Core step budget exhausted");
                    }
                    co_return result;
                }

                if (command.kind() == JavaScriptCommandKind::Spawn) {
                    const auto binding = arguments.at("child_binding").get<std::string>();
                    auto child = control->launch_child(binding, arguments.at("input"), operation_id,
                                                       operation_id);
                    scope->attach(child);
                    PlanExecution result;
                    result.output        = json{{"child_run_id", child->run_id},
                                                {"program_version_id", child->program_version_id}};
                    result.spawned_child = std::move(child);
                    co_return result;
                }

                if (command.kind() == JavaScriptCommandKind::Await) {
                    const auto child = JavaScriptCommand::from_json(arguments.at("command"));
                    const auto timeout =
                        arguments.contains("timeout_ms")
                            ? std::optional<std::uint64_t>{arguments.at("timeout_ms")
                                                               .get<std::uint64_t>()}
                            : std::nullopt;
                    auto run_awaited = [&, child,
                                        operation_id](std::shared_ptr<graph::CancelToken> token)
                        -> asio::awaitable<PlanExecution> {
                        auto launched = co_await execute_command(child, operation_id + "/await",
                                                                 std::move(token), scope,
                                                                 scope_depth + 1, false);
                        if (launched.status != ProgramTerminalStatus::Completed) co_return launched;
                        if (!launched.spawned_child) co_return std::move(launched);
                        co_return plan_result_from_child(
                            co_await launched.spawned_child->wait_async());
                    };
                    if (!timeout) co_return co_await run_awaited(operation_token);

                    struct AwaitState {
                        std::mutex                          mutex;
                        std::optional<PlanExecution>        result;
                        std::exception_ptr                  error;
                        bool                                finished  = false;
                        bool                                timed_out = false;
                        std::shared_ptr<asio::steady_timer> timer;
                        std::shared_ptr<asio::experimental::channel<void(asio::error_code, int)>>
                            completion;
                    };
                    auto       await_state         = std::make_shared<AwaitState>();
                    const auto completion_executor = asio::make_strand(executor);
                    await_state->timer             = std::make_shared<asio::steady_timer>(executor);
                    await_state->timer->expires_after(std::chrono::milliseconds(*timeout));
                    await_state->completion =
                        std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                            completion_executor, 1);
                    auto awaited_token = operation_token->fork();

                    asio::co_spawn(
                        executor, run_awaited(awaited_token),
                        asio::bind_executor(
                            completion_executor,
                            [await_state](std::exception_ptr error, PlanExecution result) {
                                bool signal = false;
                                {
                                    std::lock_guard lock(await_state->mutex);
                                    if (!await_state->finished) {
                                        await_state->finished = true;
                                        await_state->error    = error;
                                        if (!error) await_state->result = std::move(result);
                                        signal = true;
                                    }
                                }
                                await_state->timer->cancel();
                                if (signal)
                                    await_state->completion->try_send(asio::error_code(), 1);
                            }));
                    asio::co_spawn(
                        executor,
                        [await_state, completion_executor, scope,
                         awaited_token]() -> asio::awaitable<void> {
                            asio::error_code error;
                            co_await         await_state->timer->async_wait(
                                asio::redirect_error(asio::use_awaitable, error));
                            if (error) co_return;
                            asio::post(completion_executor, [await_state, scope, awaited_token] {
                                bool cancel = false;
                                {
                                    std::lock_guard lock(await_state->mutex);
                                    if (!await_state->finished) {
                                        await_state->timed_out = true;
                                        cancel                 = true;
                                    }
                                }
                                if (cancel) {
                                    scope->cancel_children(CancellationCause::Timeout);
                                    awaited_token->cancel();
                                }
                            });
                            co_return;
                        },
                        asio::detached);

                    const auto [completion_error, ignored] =
                        co_await await_state->completion->async_receive(
                            asio::as_tuple(asio::use_awaitable));
                    (void)completion_error;
                    (void)ignored;
                    std::optional<PlanExecution> awaited_result;
                    std::exception_ptr           awaited_error;
                    bool                         timed_out = false;
                    {
                        std::lock_guard lock(await_state->mutex);
                        awaited_result = std::move(await_state->result);
                        awaited_error  = await_state->error;
                        timed_out      = await_state->timed_out;
                    }
                    if (awaited_error) std::rethrow_exception(awaited_error);
                    if (timed_out)
                        co_return fail(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                       "JavaScript await operation timed out");
                    if (!awaited_result)
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_AWAIT",
                                       "JavaScript await completed without a result");
                    co_return std::move(*awaited_result);
                }

                if (command.kind() == JavaScriptCommandKind::Emit) {
                    control->emit(operation_id, ProgramEventKind::Emit,
                                  ProgramEmitEvent{operation_id, arguments.at("value")});
                    PlanExecution result;
                    result.output = arguments.at("value");
                    co_return result;
                }
                if (command.kind() == JavaScriptCommandKind::Checkpoint) {
                    if (const auto checkpoint = root_checkpoint()) {
                        (void)control->stage_event(operation_id,
                                                   ProgramEventKind::CheckpointPublished,
                                                   ProgramCheckpointEvent{*checkpoint});
                    }
                    PlanExecution result;
                    result.output = arguments.at("value");
                    co_return result;
                }
                if (command.kind() == JavaScriptCommandKind::CancelScope) {
                    const auto scope_name = arguments.at("scope").get<std::string>();
                    if (scope_name != "run" && scope_name != "root" && scope_name != "current" &&
                        scope_name != "nested")
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE",
                                       "JavaScript cancelScope references an unknown scope");
                    scope->cancel_children(CancellationCause::User);
                    if (scope_name == "run" || scope_name == "root") {
                        (void)control->cancel(CancellationCause::User);
                    }
                    co_return fail(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                   arguments.contains("reason")
                                       ? arguments.at("reason").get<std::string>()
                                       : "JavaScript scope cancelled");
                }

                if (command.kind() != JavaScriptCommandKind::Join)
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                   "JavaScript command kind is not implemented");

                const auto                     mode = arguments.at("mode").get<std::string>();
                const auto&                    encoded_members = arguments.at("members");
                std::vector<JavaScriptCommand> members;
                members.reserve(encoded_members.size());
                for (const auto& encoded : encoded_members)
                    members.push_back(JavaScriptCommand::from_json(encoded));
                if (members.empty() || members.size() > 1'000'000u)
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
                                   "JavaScript join member count exceeds its bound");

                std::uint64_t max_in_flight = control->granted_budget.max_concurrency;
                if (arguments.contains("max_in_flight"))
                    max_in_flight = arguments.at("max_in_flight").get<std::uint64_t>();
                if (max_in_flight == 0 || max_in_flight > control->granted_budget.max_concurrency)
                    co_return fail(
                        ProgramTerminalStatus::BudgetExhausted, "P_CONCURRENCY_BUDGET",
                        "JavaScript join max_in_flight exceeds its admitted concurrency budget");
                const auto cap =
                    std::min<std::size_t>(members.size(), static_cast<std::size_t>(max_in_flight));
                const auto failure_policy =
                    arguments.value("failure_policy", mode == "quorum" ? "collect" : "fail_fast");
                if (failure_policy != "fail_fast" && failure_policy != "collect")
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                   "JavaScript join failure policy is unsupported");
                const bool collect = mode == "quorum" || failure_policy == "collect";
                const auto required =
                    mode == "quorum" ? arguments.at("required_successes").get<std::uint64_t>() : 0;
                if ((mode == "race" && members.size() < 2) ||
                    (mode == "quorum" && (required == 0 || required > members.size())))
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                   "JavaScript join bounds are invalid");

                struct JoinState {
                    std::mutex                                         mutex;
                    std::vector<std::optional<PlanExecution>>          results;
                    std::vector<std::exception_ptr>                    errors;
                    std::vector<bool>                                  started;
                    std::vector<bool>                                  finished;
                    std::vector<std::shared_ptr<graph::CancelToken>>   tokens;
                    std::vector<std::shared_ptr<JavaScriptScopeState>> scopes;
                    std::size_t                                        next                = 0;
                    std::size_t                                        active              = 0;
                    std::size_t                                        completed           = 0;
                    bool                                               terminal            = false;
                    bool                                               selection_scheduled = false;
                    bool                       initial_launches_registered                 = false;
                    std::optional<std::size_t> winner;
                    std::shared_ptr<asio::experimental::channel<void(asio::error_code, int)>>
                        completion;
                };
                auto state = std::make_shared<JoinState>();
                state->results.resize(members.size());
                state->errors.resize(members.size());
                state->started.resize(members.size(), false);
                state->finished.resize(members.size(), false);
                state->tokens.resize(members.size());
                state->scopes.resize(members.size());
                const auto completion_executor = asio::make_strand(executor);
                state->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);

                // A member can complete before the initial launch loop finishes. Completion
                // handlers therefore retain this dispatcher rather than a reference to a
                // stack-local std::function.
                auto launch_member = std::make_shared<std::function<void(std::size_t)>>();
                *launch_member     = [&](std::size_t index) {
                    {
                        std::lock_guard lock(state->mutex);
                        state->started[index] = true;
                        ++state->active;
                        state->tokens[index] = operation_token->fork();
                        state->scopes[index] = std::make_shared<JavaScriptScopeState>();
                    }
                    asio::co_spawn(
                        completion_executor,
                        execute_command(
                            members[index], operation_id + "/member/" + std::to_string(index),
                            state->tokens[index], state->scopes[index], scope_depth + 1, false),
                        asio::bind_executor(
                            completion_executor,
                            [state, index, launch_member, &members, mode, collect, required, cap,
                             operation_token,
                             completion_executor](std::exception_ptr error, PlanExecution result) {
                                bool                     post_selection = false;
                                bool                     cancel_all     = false;
                                std::vector<std::size_t> to_launch;
                                bool                     signal_completion = false;
                                {
                                    std::lock_guard lock(state->mutex);
                                    state->errors[index] = error;
                                    if (!error) state->results[index] = std::move(result);
                                    state->finished[index] = true;
                                    ++state->completed;
                                    if (state->active != 0) --state->active;

                                    const auto member_failed = [&] {
                                        if (error) return true;
                                        return state->results[index]->status !=
                                               ProgramTerminalStatus::Completed;
                                    };
                                    if (operation_token->is_cancelled() && !state->terminal) {
                                        state->terminal = true;
                                        cancel_all      = true;
                                    } else if (mode == "race" && !state->terminal) {
                                        if (!state->selection_scheduled) {
                                            state->selection_scheduled = true;
                                            post_selection             = true;
                                        }
                                    } else if (mode == "all" && !collect && member_failed() &&
                                               !state->terminal) {
                                        if (!state->selection_scheduled) {
                                            state->selection_scheduled = true;
                                            post_selection             = true;
                                        }
                                    } else if (mode == "quorum" && !state->terminal) {
                                        std::size_t successes = 0;
                                        for (std::size_t i = 0; i < state->results.size(); ++i)
                                            if (state->finished[i] && state->results[i] &&
                                                state->results[i]->status ==
                                                    ProgramTerminalStatus::Completed)
                                                ++successes;
                                        if (successes >= required ||
                                            successes + (members.size() - state->completed) <
                                                required) {
                                            state->terminal = true;
                                            cancel_all      = true;
                                        }
                                    }

                                    if (state->initial_launches_registered && !state->terminal &&
                                        !state->selection_scheduled && !post_selection &&
                                        state->next < members.size()) {
                                        while (state->next < members.size() && state->active < cap)
                                            to_launch.push_back(state->next++);
                                    }
                                    if (state->initial_launches_registered && to_launch.empty() &&
                                        state->terminal && state->active == 0)
                                        signal_completion = true;
                                    if (state->initial_launches_registered && to_launch.empty() &&
                                        !state->terminal && !state->selection_scheduled &&
                                        !post_selection && state->completed == members.size() &&
                                        state->active == 0)
                                        signal_completion = true;
                                }
                                if (post_selection) {
                                    asio::post(completion_executor, [state, mode, collect,
                                                                     operation_token]() {
                                        std::vector<std::size_t> to_cancel;
                                        bool                     signal_completion = false;
                                        {
                                            std::lock_guard lock(state->mutex);
                                            if (!state->terminal) {
                                                std::optional<std::size_t> selected;
                                                for (std::size_t i = 0; i < state->finished.size();
                                                     ++i) {
                                                    if (!state->finished[i]) continue;
                                                    const bool failed =
                                                        state->errors[i] || !state->results[i] ||
                                                        state->results[i]->status !=
                                                            ProgramTerminalStatus::Completed;
                                                    if ((mode == "race" || (!collect && failed)) &&
                                                        (!selected || i < *selected))
                                                        selected = i;
                                                }
                                                if (selected) {
                                                    state->winner   = selected;
                                                    state->terminal = true;
                                                    for (std::size_t i = 0;
                                                         i < state->started.size(); ++i)
                                                        if (state->started[i] && i != *selected)
                                                            to_cancel.push_back(i);
                                                }
                                            }
                                            if (state->initial_launches_registered &&
                                                state->terminal && state->active == 0)
                                                signal_completion = true;
                                        }
                                        for (const auto index : to_cancel) {
                                            if (state->scopes[index])
                                                state->scopes[index]->cancel_children(
                                                    CancellationCause::ParentTerminal);
                                            if (state->tokens[index])
                                                state->tokens[index]->cancel();
                                        }
                                        if (signal_completion)
                                            state->completion->try_send(asio::error_code(), 1);
                                    });
                                }
                                if (cancel_all) {
                                    std::vector<std::size_t> to_cancel;
                                    {
                                        std::lock_guard lock(state->mutex);
                                        for (std::size_t i = 0; i < state->started.size(); ++i)
                                            if (state->started[i]) to_cancel.push_back(i);
                                    }
                                    for (const auto member_index : to_cancel) {
                                        if (state->scopes[member_index])
                                            state->scopes[member_index]->cancel_children(
                                                CancellationCause::ParentTerminal);
                                        if (state->tokens[member_index])
                                            state->tokens[member_index]->cancel();
                                    }
                                }
                                for (const auto index : to_launch)
                                    (*launch_member)(index);
                                if (signal_completion)
                                    state->completion->try_send(asio::error_code(), 1);
                            }));
                };
                state->next = cap;
                for (std::size_t index = 0; index < cap; ++index)
                    (*launch_member)(index);
                std::vector<std::size_t> deferred_launches;
                bool                     signal_completion = false;
                {
                    std::lock_guard lock(state->mutex);
                    // A fast member may finish while this loop is still registering its
                    // siblings. Do not permit that completion to close the join early.
                    state->initial_launches_registered = true;
                    if (!state->terminal && !state->selection_scheduled &&
                        state->next < members.size()) {
                        while (state->next < members.size() && state->active < cap)
                            deferred_launches.push_back(state->next++);
                    }
                    if (deferred_launches.empty() && state->terminal && state->active == 0)
                        signal_completion = true;
                    if (deferred_launches.empty() && !state->terminal &&
                        !state->selection_scheduled && state->completed == members.size() &&
                        state->active == 0)
                        signal_completion = true;
                }
                for (const auto index : deferred_launches)
                    (*launch_member)(index);
                if (signal_completion) state->completion->try_send(asio::error_code(), 1);

                co_await asio::co_spawn(
                    completion_executor,
                    [state]() -> asio::awaitable<void> {
                        const auto [error, ignored] = co_await state->completion->async_receive(
                            asio::as_tuple(asio::use_awaitable));
                        (void)error;
                        (void)ignored;
                    },
                    asio::use_awaitable);

                PlanExecution joined;
                joined.output = json::array();
                std::vector<std::optional<PlanExecution>> results;
                std::vector<std::exception_ptr>           errors;
                std::optional<std::size_t>                winner;
                {
                    std::lock_guard lock(state->mutex);
                    results = std::move(state->results);
                    errors  = std::move(state->errors);
                    winner  = state->winner;
                }
                const auto make_member_failure =
                    [&](std::size_t index, const std::exception_ptr& error,
                        const std::optional<PlanExecution>& member) -> PlanExecution {
                    if (member) return *member;
                    std::string message = "JavaScript join member failed";
                    if (error) {
                        try {
                            std::rethrow_exception(error);
                        } catch (const std::exception& exception) {
                            message += ": ";
                            message += exception.what();
                        } catch (...) {}
                    }
                    return plan_failure(ProgramTerminalStatus::Failed, "P_JS_JOIN_MEMBER",
                                        std::move(message),
                                        operation_id + "/member/" + std::to_string(index));
                };
                if (mode == "race") {
                    if (!winner)
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_JOIN",
                                       "JavaScript race completed without a winner");
                    joined = make_member_failure(*winner, errors[*winner], results[*winner]);
                    co_return joined;
                }
                if (mode == "quorum") {
                    std::size_t successes = 0;
                    for (std::size_t i = 0; i < results.size(); ++i) {
                        if (results[i] && results[i]->status == ProgramTerminalStatus::Completed) {
                            ++successes;
                            joined.output.push_back(results[i]->output);
                            joined.execution_trace.insert(joined.execution_trace.end(),
                                                          results[i]->execution_trace.begin(),
                                                          results[i]->execution_trace.end());
                        }
                    }
                    if (successes < required) {
                        co_return fail(ProgramTerminalStatus::Failed, "P_QUORUM_UNREACHABLE",
                                       "JavaScript quorum could not reach its required successes");
                    }
                    co_return joined;
                }

                std::optional<std::size_t> first_failure;
                for (std::size_t i = 0; i < results.size(); ++i) {
                    if (results[i]) {
                        joined.output.push_back(results[i]->output);
                        joined.execution_trace.insert(joined.execution_trace.end(),
                                                      results[i]->execution_trace.begin(),
                                                      results[i]->execution_trace.end());
                        if (results[i]->status != ProgramTerminalStatus::Completed &&
                            !first_failure)
                            first_failure = i;
                    } else {
                        joined.output.push_back(nullptr);
                        if (!first_failure) first_failure = i;
                    }
                }
                if (first_failure) {
                    joined        = make_member_failure(*first_failure, errors[*first_failure],
                                                        results[*first_failure]);
                    joined.output = json::array();
                    // Collect mode retains one declaration-ordered value per
                    // member, including null for a member with no result.
                    for (const auto& member : results)
                        joined.output.push_back(member && member->status ==
                                                              ProgramTerminalStatus::Completed
                                                    ? member->output
                                                    : json(nullptr));
                }
                co_return joined;
            };

            PlanExecution       result;
            std::optional<json> response;
            std::uint64_t       command_sequence = 0;
            const auto          root_scope       = std::make_shared<JavaScriptScopeState>();
            for (;;) {
                if (control->cancel_token->is_cancelled()) {
                    co_return plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                           "JavaScript control command cancelled before dispatch",
                                           "root.javascript");
                }
                // Join members may complete on a strand callback. Resume the
                // single QuickJS generator from the runtime executor so every
                // JS turn starts on a fresh scheduler continuation; members
                // remain C++-only and can still overlap on the pool.
                co_await   asio::post(executor, asio::use_awaitable);
                const auto step = session.next(std::move(response));
                if (control->cancel_token->is_cancelled()) {
                    co_return plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                           "JavaScript control command cancelled before dispatch",
                                           "javascript");
                }
                if (step.done) {
                    if (supplied_core_resume_required && !supplied_core_resume_consumed) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL_MISMATCH",
                            "Exact Core resume checkpoint did not match its pending command");
                    }
                    const auto recorded_commands = control->transitions->load_javascript_commands(
                        control->owner_scope, control->run_id);
                    for (std::size_t index = 0; index < recorded_commands.size(); ++index) {
                        const auto& recorded = recorded_commands[index];
                        if (recorded.bundle_id() != control->bundle_id ||
                            recorded.command_ordinal() > command_sequence) {
                            throw_runtime_diagnostic(
                                "P_JS_COMMAND_JOURNAL_MISMATCH",
                                "JavaScript command journal contains an unconsumed entry",
                                json{{"command_ordinal", recorded.command_ordinal()},
                                     {"command_sequence", command_sequence},
                                     {"coordinate_id", recorded.coordinate_id()}});
                        }
                        if (recorded.pending()) {
                            const auto completed = std::find_if(
                                recorded_commands.begin() + static_cast<std::ptrdiff_t>(index + 1),
                                recorded_commands.end(), [&](const auto& later) {
                                    return later.command_ordinal() == recorded.command_ordinal() &&
                                           later.completed();
                                });
                            if (completed == recorded_commands.end()) {
                                throw_runtime_diagnostic(
                                    "P_JS_COMMAND_JOURNAL_MISMATCH",
                                    "JavaScript command journal contains an unconsumed entry",
                                    json{{"command_ordinal", recorded.command_ordinal()},
                                         {"command_sequence", command_sequence},
                                         {"coordinate_id", recorded.coordinate_id()}});
                            }
                        }
                    }
                    result.output   = step.value;
                    result.returned = true;
                    co_return result;
                }
                if (!step.command) {
                    throw_runtime_diagnostic(
                        "P_JS_CONTROL_COMMAND",
                        "JavaScript control yield must be a sealed ng command value",
                        json{{"source_site", "unknown"}});
                }
                const auto command_value = *step.command;
                const auto ordinal       = ++command_sequence;
                const auto operation_id  = javascript_command_operation_id(ordinal);
                const auto effect_id =
                    javascript_command_effect_identity(*control, ordinal, command_value);
                const auto prior_commands = control->transitions->load_javascript_commands(
                    control->owner_scope, control->run_id);
                const auto prior = std::find_if(
                    prior_commands.rbegin(), prior_commands.rend(),
                    [&](const auto& entry) { return entry.command_ordinal() == ordinal; });
                bool      exact_resume_authorized = false;
                RunBudget resource_reservation;

                if (prior != prior_commands.rend()) {
                    if (prior->bundle_id() != control->bundle_id ||
                        detail::canonical_json_bytes(prior->command().to_json()) !=
                            detail::canonical_json_bytes(command_value.to_json()) ||
                        prior->effect_identity() != std::optional<std::string>(effect_id)) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL_MISMATCH",
                            "Recorded JavaScript command coordinate or payload does not match",
                            json{{"command_ordinal", ordinal},
                                 {"expected_coordinate", prior->coordinate_id()},
                                 {"actual_coordinate",
                                  ProgramJavaScriptCommandJournalEntry(
                                      ProgramJavaScriptCommandJournalEntryData{
                                          prior->sequence(), control->bundle_id, ordinal,
                                          command_value, effect_id, std::nullopt})
                                      .coordinate_id()}});
                    }
                    if (prior->completed()) {
                        const auto recorded =
                            decode_javascript_command_result(*prior->terminal_result());
                        result.execution_trace.insert(result.execution_trace.end(),
                                                      recorded.execution_trace.begin(),
                                                      recorded.execution_trace.end());
                        if (recorded.status != ProgramTerminalStatus::Completed) {
                            PlanExecution replayed;
                            replayed.status          = recorded.status;
                            replayed.output          = std::move(recorded.output);
                            replayed.failure         = std::move(recorded.failure);
                            replayed.execution_trace = std::move(recorded.execution_trace);
                            co_return replayed;
                        }
                        if (const auto durable_replay =
                                control->transitions->load(control->owner_scope, control->run_id)) {
                            unreconciled_javascript_remaining = durable_replay->remaining_budget();
                        }
                        if (command_value.kind() == JavaScriptCommandKind::Checkpoint) {
                            co_await control->hold_latest_handoff_if_requested();
                        }
                        response = std::move(recorded.output);
                        continue;
                    }

                    if (checkpoint_id && resume_checkpoint) {
                        if (auto failure =
                                validate_javascript_command(command_value, operation_id, 0))
                            co_return std::move(*failure);
                        const auto resume_operation = javascript_call_core_operation_for_thread(
                            *control, command_value, operation_id,
                            resume_checkpoint->core_thread_id);
                        if (!resume_operation) {
                            co_return plan_failure(
                                ProgramTerminalStatus::CheckpointIncompatible,
                                "P_CHECKPOINT_INCOMPATIBLE",
                                "Exact Core resume checkpoint does not match a pending "
                                "JavaScript callCore coordinate",
                                operation_id);
                        }
                        exact_core_resume = std::pair{*resume_operation, *checkpoint_id};
                        supplied_core_resume_required = true;
                        exact_resume_authorized       = true;
                    } else {
                        // Already-published non-Core work is never dispatch
                        // authority. It must be explicitly reconciled.
                        const auto durable_pending =
                            control->transitions->load(control->owner_scope, control->run_id);
                        if (durable_pending)
                            unreconciled_javascript_remaining = durable_pending->remaining_budget();
                        result.status = ProgramTerminalStatus::Interrupted;
                        result.interrupt =
                            ProgramInterrupt{"javascript", command_value.to_json(), std::nullopt,
                                             javascript_command_pending_effect(
                                                 *control, ordinal, command_value, effect_id)};
                        co_return result;
                    }
                }
                if (exact_resume_authorized) {
                    const auto durable_resumed =
                        control->transitions->latest(control->owner_scope, control->run_id);
                    if (!durable_resumed ||
                        !has_resource_reservation(durable_resumed->inflight_reservation)) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL_MISMATCH",
                            "Exact Core resume has no durable command resource reservation");
                    }
                    resource_reservation = durable_resumed->inflight_reservation;
                    active_javascript_command =
                        ActiveJavaScriptCommand{ordinal, command_value, effect_id};
                }

                std::uint64_t operations_before = 0;
                {
                    std::lock_guard lock(plan_mutex);
                    operations_before = operation_count;
                }
                std::size_t command_count = 0;
                if (!exact_resume_authorized) {
                    try {
                        command_count = javascript_command_tree_size(command_value);
                    } catch (const ProgramDiagnosticError& error) {
                        co_return plan_failure(ProgramTerminalStatus::Failed,
                                               error.diagnostic().code, error.diagnostic().message,
                                               operation_id);
                    }
                    if (auto failure = reserve_operations(operation_id, command_count))
                        co_return std::move(*failure);
                    if (auto failure = validate_javascript_command(command_value, operation_id, 0))
                        co_return std::move(*failure);

                    const auto durable_before_dispatch =
                        control->transitions->load(control->owner_scope, control->run_id);
                    if (!durable_before_dispatch) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL",
                            "JavaScript command lost its running snapshot before dispatch");
                    }
                    const ProgramUsage operation_reservation{
                        0, 0, 0, static_cast<std::uint64_t>(command_count), 0, 0};
                    const auto after_operations = settle_budget(
                        durable_before_dispatch->remaining_budget(), operation_reservation);
                    auto reservation             = reserve_command_resources(after_operations);
                    resource_reservation         = reservation.reservation;
                    const auto pending_remaining = reservation.remaining;
                    const auto pending_published = control->publish_javascript_command(
                        ordinal, command_value, effect_id, std::nullopt, pending_remaining,
                        resource_reservation);
                    if (pending_published == ProgramTransitionPublishResult::AlreadyPresent) {
                        unreconciled_javascript_remaining = pending_remaining;
                        result.status                     = ProgramTerminalStatus::Interrupted;
                        result.interrupt =
                            ProgramInterrupt{"javascript", command_value.to_json(), std::nullopt,
                                             javascript_command_pending_effect(
                                                 *control, ordinal, command_value, effect_id)};
                        co_return result;
                    }
                    if (pending_published != ProgramTransitionPublishResult::Published) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL",
                            "Could not durably publish the JavaScript command before dispatch",
                            json{{"command_ordinal", ordinal}, {"effect_identity", effect_id}});
                    }
                    unreconciled_javascript_remaining = pending_remaining;
                    active_javascript_command =
                        ActiveJavaScriptCommand{ordinal, command_value, effect_id};
                }

                const auto    model_before       = model_tokens(usage);
                const auto    steps_before       = core_progress->steps();
                const auto    command_started_at = std::chrono::steady_clock::now();
                PlanExecution command_result;
                try {
                    command_result = co_await execute_command(
                        command_value, operation_id, control->cancel_token, root_scope, 0, false);
                } catch (const EventSinkError& error) {
                    command_result = plan_failure(ProgramTerminalStatus::Failed, "P_EVENT_SINK",
                                                  error.what(), operation_id);
                } catch (const graph::CancelledException& error) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                     error.what(), operation_id);
                } catch (const ProgramDiagnosticError& error) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Failed, error.diagnostic().code,
                                     error.diagnostic().message, operation_id);
                } catch (const std::exception& error) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                     error.what(), operation_id);
                } catch (...) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                     "Unknown JavaScript command failure", operation_id);
                }
                result.execution_trace.insert(result.execution_trace.end(),
                                              command_result.execution_trace.begin(),
                                              command_result.execution_trace.end());

                std::uint64_t operations_after = 0;
                {
                    std::lock_guard lock(plan_mutex);
                    operations_after = operation_count;
                }
                const ProgramUsage command_usage{
                    std::min(elapsed_ms(command_started_at), resource_reservation.wall_time_ms),
                    subtract_saturated(model_tokens(usage), model_before),
                    0,
                    subtract_saturated(operations_after, operations_before),
                    subtract_saturated(core_progress->steps(), steps_before),
                    static_cast<std::uint32_t>(
                        std::min<std::size_t>(peak_concurrency.load(std::memory_order_relaxed),
                                              std::numeric_limits<std::uint32_t>::max()))};
                settled_javascript_wall_time_ms += command_usage.wall_time_ms;

                if (command_result.status == ProgramTerminalStatus::Interrupted) {
                    if (root_checkpoint()) {
                        unreconciled_javascript_remaining.reset();
                    }
                    co_return command_result;
                }

                const auto terminal_result = javascript_command_terminal_result(
                    command_result, to_string(command_result.status), command_usage);
                auto durable = control->transitions->load(control->owner_scope, control->run_id);
                if (!durable) {
                    throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL",
                                             "JavaScript command result lost its running snapshot");
                }
                auto settlement_usage               = command_usage;
                settlement_usage.program_operations = 0;
                const auto remaining =
                    has_resource_reservation(resource_reservation)
                        ? settle_resource_reservation(durable->remaining_budget(),
                                                      resource_reservation, settlement_usage)
                        : settle_budget(durable->remaining_budget(), settlement_usage);
                ProgramTransitionPublishResult published;
                try {
                    published = control->publish_javascript_command(
                        ordinal, command_value, effect_id, terminal_result, remaining, RunBudget{});
                } catch (const EventSinkError&) {
                    unreconciled_javascript_remaining = remaining;
                    active_javascript_command.reset();
                    throw;
                }
                if (published != ProgramTransitionPublishResult::Published &&
                    published != ProgramTransitionPublishResult::AlreadyPresent) {
                    throw_runtime_diagnostic(
                        "P_JS_COMMAND_JOURNAL",
                        "Could not durably publish the JavaScript command result",
                        json{{"command_ordinal", ordinal}, {"effect_identity", effect_id}});
                }
                unreconciled_javascript_remaining = remaining;
                active_javascript_command.reset();
                if (command_result.status != ProgramTerminalStatus::Completed)
                    co_return command_result;
                if (command_value.kind() == JavaScriptCommandKind::Checkpoint) {
                    co_await control->hold_latest_handoff_if_requested();
                }
                response = std::move(command_result.output);
            }
        };

        PlanExecution plan_result;
        if (control_source) {
            try {
                JavaScriptCompileLimits javascript_limits;
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                javascript_limits.max_wall_time_ms =
                    remaining.count() > 0 ? static_cast<std::uint64_t>(remaining.count()) : 1;
                auto session = JavaScriptGenerator::open(
                    *control_source, std::move(input), javascript_limits,
                    [token = control->cancel_token] { return token->is_cancelled(); }, deadline);
                if (!session) {
                    throw_runtime_diagnostic(
                        "P_JS_CONTROL_MAIN",
                        "Admitted JavaScript control source no longer exports main(input)");
                }
                plan_result = co_await execute_javascript_control(*session);
            } catch (const JavaScriptCompileError& error) {
                if (error.code() == "P_JS_TIMEOUT") {
                    control->cancel(CancellationCause::Timeout);
                } else if (error.code() == "P_JS_CANCELLED" &&
                           control->cancellation_cause() == CancellationCause::None) {
                    control->cancel(CancellationCause::User);
                }
                throw_runtime_diagnostic(error.code(), error.what(), error.witness());
            }
        } else {
            plan_result = co_await execute(root_id, std::move(input), control->core_thread_id, 0,
                                           control->cancel_token);
        }

        const auto terminal_cause        = terminal_cause_at_deadline(control, deadline);
        outcome.output                   = std::move(plan_result.output);
        outcome.execution_trace          = std::move(plan_result.execution_trace);
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = core_progress->steps();
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        outcome.checkpoint               = last_root_checkpoint;
        if (!outcome.checkpoint && resume_checkpoint) outcome.checkpoint = resume_checkpoint;
        if (unreconciled_javascript_remaining) {
            outcome.remaining_budget =
                settle_unreserved_javascript_wall(*unreconciled_javascript_remaining);
        }

        const auto cancellation_message =
            plan_result.failure && !plan_result.failure->message.empty()
                ? plan_result.failure->message
                : std::string("Program attempt was cancelled");
        if (apply_terminal_cause(outcome, terminal_cause, cancellation_message)) {
        } else if (plan_result.status != ProgramTerminalStatus::Completed) {
            outcome.status    = plan_result.status;
            outcome.interrupt = std::move(plan_result.interrupt);
            outcome.failure   = std::move(plan_result.failure);
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
                outcome.status  = ProgramTerminalStatus::Failed;
                outcome.failure = ProgramFailure{"P_OUTPUT_CONTRACT",
                                                 "Program output violates its admitted contract",
                                                 root_id,
                                                 "",
                                                 0,
                                                 json{{"detail", error.what()}}};
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
        outcome                          = failed_outcome(control, "P_EVENT_SINK", error.what());
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
        apply_durable_javascript_disposition(outcome);
    } catch (const graph::CancelledException& error) {
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        outcome.checkpoint               = control->latest_checkpoint();
        auto cause                       = terminal_cause_at_deadline(control, deadline);
        if (cause == CancellationCause::None) cause = CancellationCause::User;
        apply_terminal_cause(outcome, cause, error.what());
        apply_durable_javascript_disposition(outcome);
    } catch (const NestedOperationFailure& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(),
                                 error.operation_id, error.attempts);
        if (outcome.failure) {
            outcome.failure->operation_id = error.operation_id;
            outcome.failure->core_node    = error.core_node;
        }
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        const auto cause                 = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(outcome, cause, error.what()) && error.checkpoint_incompatible &&
            checkpoint_id) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
        apply_durable_javascript_disposition(outcome);
    } catch (const graph::NodeExecutionError& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(), error.node_name(),
                                 static_cast<std::uint32_t>(error.attempts()));
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline), error.what());
        apply_durable_javascript_disposition(outcome);
    } catch (const ProgramDiagnosticError& error) {
        const auto& diagnostic   = error.diagnostic();
        std::string operation_id = "root";
        if (diagnostic.witness.is_object() && diagnostic.witness.contains("operation_id") &&
            diagnostic.witness["operation_id"].is_string()) {
            operation_id = diagnostic.witness["operation_id"].get<std::string>();
        }
        outcome = failed_outcome(control, diagnostic.code, diagnostic.message);
        if (outcome.failure) outcome.failure->operation_id = operation_id;
        if (outcome.failure) outcome.failure->witness = diagnostic.witness;
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline),
                             diagnostic.message);
        apply_durable_javascript_disposition(outcome);
    } catch (const std::exception& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what());
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        const auto cause                 = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(outcome, cause, error.what()) && checkpoint_id &&
            !checkpoint_is_available(*control, *checkpoint_id)) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
        apply_durable_javascript_disposition(outcome);
    } catch (...) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", "Unknown Core failure");
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(outcome, terminal_cause_at_deadline(control, deadline),
                             "Unknown Core failure");
        apply_durable_javascript_disposition(outcome);
    }

    if (outcome.failure) {
        auto& witness = outcome.failure->witness;
        if (!witness.is_object()) witness = json::object();
        if (!witness.contains("source_pointer")) {
            try {
                const auto& typed_plan = control->materialized->bundle.typed_orchestration_plan();
                const auto* operation  = typed_plan.find(outcome.failure->operation_id);
                if (operation && !operation->dispatch().source_pointer.empty())
                    witness["source_pointer"] = operation->dispatch().source_pointer;
            } catch (...) {
                // Admission already validates the typed plan; preserve the terminal
                // failure if a malformed legacy bundle reaches this boundary.
            }
        }
    }

    outcome.remaining_budget.max_dynamic_compiles = subtract_saturated(
        control->granted_budget.max_dynamic_compiles, dynamic_compile_count);

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
