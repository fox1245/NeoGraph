#include <neograph/async/host_loop.h>
#include <neograph/graph/cancel.h>
#include <neograph/program/compiler.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/sqlite_provider_call_broker.h>

#include "budget_overrun.h"
#include "canonical_json.h"
#include "node_retry_budget.h"
#include "core_progress.h"
#include "javascript.h"
#include "monetary.h"
#include "run_control.h"
#include "provider_failure.h"
#include "strand_join_signal.h"
#include "task_graph_scheduler.h"
#include "../core/tool_failure.h"
#include <asio/as_tuple.hpp>
#include <asio/bind_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
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
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
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

std::string safe_failure_message(std::string message) {
    try {
        validate_utf8(message);
        return message;
    } catch (const std::invalid_argument&) {
        // Exception::what() may contain bytes from the Windows active code page.
        // Preserve the diagnostic as ASCII escapes so terminal records stay durable.
        constexpr char hex[] = "0123456789ABCDEF";
        std::string escaped;
        escaped.reserve(message.size());
        for (unsigned char byte : message) {
            if (byte < 0x80) {
                escaped.push_back(static_cast<char>(byte));
            } else {
                escaped += "\\x";
                escaped.push_back(hex[byte >> 4]);
                escaped.push_back(hex[byte & 0x0f]);
            }
        }
        return escaped;
    }
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
    remaining.max_node_retries =
        detail::remaining_node_retries(granted.max_node_retries, usage.node_retries);
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
    bool requires_managed_budget(const std::string& thread_id) override {
        return inner_->requires_managed_budget(thread_id);
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
    asio::awaitable<bool> requires_managed_budget_async(std::string thread_id) override {
        co_return co_await inner_->requires_managed_budget_async(std::move(thread_id));
    }

    std::shared_ptr<graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const graph::ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override {
        if (!expected_checkpoint_id.empty()) {
            validate_source(load_by_id(expected_checkpoint_id), expected_checkpoint_id,
                            expected_checkpoint_commitment);
        }
        return inner_->acquire_managed_budget_lease(
            scope, expected_checkpoint_id, expected_checkpoint_commitment);
    }
    asio::awaitable<std::shared_ptr<graph::OwnedManagedBudgetLease>>
    acquire_managed_budget_lease_async(graph::ManagedBudgetLeaseScope scope,
        std::string expected_checkpoint_id, std::string expected_checkpoint_commitment) override {
        if (!expected_checkpoint_id.empty()) {
            auto source = co_await load_by_id_async(expected_checkpoint_id);
            validate_source(source, expected_checkpoint_id, expected_checkpoint_commitment);
        }
        co_return co_await inner_->acquire_managed_budget_lease_async(
            std::move(scope), std::move(expected_checkpoint_id),
            std::move(expected_checkpoint_commitment));
    }
    graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override {
        return inner_->begin_managed_budget_effect(
            lease, effect_id, exact_claim_amount, prepared_request_digest);
    }
    asio::awaitable<graph::ManagedBudgetEffectReceipt> begin_managed_budget_effect_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease, std::string effect_id,
        std::uint64_t exact_claim_amount, std::string prepared_request_digest) override {
        co_return co_await inner_->begin_managed_budget_effect_async(
            std::move(lease), std::move(effect_id), exact_claim_amount,
            std::move(prepared_request_digest));
    }
    void settle_managed_budget_effect(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
        const graph::ManagedBudgetEffectReceipt& effect, sp::runtime::Result genuine_outcome,
        const UsageAccumulator::AuthoritySnapshot& authority) override {
        inner_->settle_managed_budget_effect(lease, effect, std::move(genuine_outcome), authority);
    }
    asio::awaitable<void> settle_managed_budget_effect_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease, graph::ManagedBudgetEffectReceipt effect,
        sp::runtime::Result genuine_outcome, UsageAccumulator::AuthoritySnapshot authority) override {
        co_await inner_->settle_managed_budget_effect_async(
            std::move(lease), std::move(effect), std::move(genuine_outcome), std::move(authority));
    }
    void publish_managed_budget_checkpoint(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
        const graph::Checkpoint& checkpoint) override {
        validate(checkpoint);
        inner_->publish_managed_budget_checkpoint(lease, checkpoint);
    }
    asio::awaitable<void> publish_managed_budget_checkpoint_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease, graph::Checkpoint checkpoint) override {
        validate(checkpoint);
        co_await inner_->publish_managed_budget_checkpoint_async(std::move(lease), std::move(checkpoint));
    }
    void release_managed_budget_lease(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease) override {
        inner_->release_managed_budget_lease(lease);
    }
    asio::awaitable<void> release_managed_budget_lease_async(
        std::shared_ptr<graph::OwnedManagedBudgetLease> lease) override {
        co_await inner_->release_managed_budget_lease_async(std::move(lease));
    }
    bool retains_native_checkpoint() const noexcept override {
        return inner_->retains_native_checkpoint();
    }
    void publish_managed_budget_fork(const graph::Checkpoint& source,
                                    const graph::Checkpoint& forked) override {
        validate(source);
        validate(forked);
        inner_->publish_managed_budget_fork(source, forked);
    }
    asio::awaitable<void> publish_managed_budget_fork_async(
        graph::Checkpoint source, graph::Checkpoint forked) override {
        validate(source);
        validate(forked);
        co_await inner_->publish_managed_budget_fork_async(std::move(source), std::move(forked));
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
    void validate_source(const std::optional<graph::Checkpoint>& checkpoint,
                         const std::string& expected_checkpoint_id,
                         const std::string& expected_checkpoint_commitment) const {
        if (!checkpoint || checkpoint->id != expected_checkpoint_id ||
            graph::managed_budget_checkpoint_commitment(*checkpoint) != expected_checkpoint_commitment) {
            throw std::runtime_error("Exact managed budget source changed before acquisition");
        }
        validate(*checkpoint);
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
                                                   const std::optional<json>&   output,
                                                   bool                         exact_budget) {
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
    // A schema-2 task reserves its budget once per attempt, but each attempt holds the whole.
    const auto output_limit =
        exact_budget ? task.requested_budget.output_bytes : task.reserved_budget.output_bytes;
    if ((exact_budget || output_limit != 0) && output->dump().size() > output_limit) {
        return ProgramFailure{"P_TASK_OUTPUT_BUDGET",
                              "Task output exceeds its reserved output byte budget",
                              task.operation_id,
                              "",
                              0,
                              json{{"output_bytes", output->dump().size()},
                                   {"reserved_output_bytes", output_limit}}};
    }
    return std::nullopt;
}

// Whether the failure of a task's child may be tried again: the child failed with a Core node
// failure the classifier calls retryable, and not one that stopped retrying because a committed
// effect, the run's retry ceiling or the deadline forbids another attempt (the rule of the
// plan-level Retry operation, program_retryable).
bool retryable_task_failure(const std::optional<ProgramFailure>& failure) {
    if (!failure || !failure->witness.is_object() || !failure->witness.contains("node_failure"))
        return false;
    const auto record = failure->witness["node_failure"];
    if (!record.is_object() || !record.contains("retryable") || !record["retryable"].is_boolean() ||
        !record["retryable"].get<bool>())
        return false;
    const auto kind = record.contains("kind") && record["kind"].is_string()
                          ? record["kind"].get<std::string>()
                          : std::string();
    return kind != "effect_committed" && kind != "ceiling" && kind != "deadline";
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
    // Assignment recurses once per token and builds a value as deep as the
    // pointer; past json::max_parse_depth tokens it fails like a bad pointer.
    return tokens && tokens->size() <= json::max_parse_depth &&
           assign_json_pointer(target, *tokens, 0, std::move(value));
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
    // The checkpoint an interrupted CallCore paused at, which the run resumes
    // from. A paused join carries the one of the member it resumes (#380).
    std::optional<CoreCheckpointIdentity> interrupt_checkpoint;
    // A paused join: the recorded result of each member that settled, by
    // member operation id, including those of paused nested joins (#380). A
    // paused plan: the records its operations left for the attempt that
    // resumes it, by operation id (#384, #386).
    json settled_members = json::object();
    // A paused plan operation: how far it got, for its "entered" record
    // (#386). Set by the operation, consumed by the plan executor.
    std::optional<json> reentry;
};

// #397: a join member or plan branch that paused on an effect whose outcome is
// unknown: its Core call raised the reconciliation interrupt (#392). Like a
// Core step, a join ranks that pause above a sibling's failure: it pauses for
// reconciliation instead of failing, which would abandon the effect.
bool reconciliation_pause(const PlanExecution& paused) {
    if (paused.status != ProgramTerminalStatus::Interrupted || !paused.interrupt) return false;
    const auto& value = paused.interrupt->value;
    if (!value.is_object() || !value.contains("value")) return false;
    const auto payload = value.at("value");
    return payload.is_object() &&
           payload.value("kind", std::string()) == "tool_effect_reconciliation";
}

std::atomic<JoinMemberLaunchObserver> join_member_launch_observer{nullptr};

// A join member that settled before its join paused, replayed from its
// recorded result by the attempt that resumes the join (#380).
asio::awaitable<PlanExecution> replay_settled_member(PlanExecution settled) {
    co_return settled;
}

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
    PureMutex                             mutex;
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
        target.status               = source.status;
        target.interrupt            = std::move(source.interrupt);
        target.failure              = std::move(source.failure);
        target.interrupt_checkpoint = std::move(source.interrupt_checkpoint);
        for (const auto& [key, record] : source.settled_members.items())
            target.settled_members[key] = record;
    }
}

// #391: an await's deadline is absolute, so its start is wall-clock time that
// survives the process: Unix epoch milliseconds.
std::uint64_t unix_time_ms() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

// A recorded await start, as unsigned milliseconds.
std::uint64_t recorded_start_ms(const json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL", "Recorded await start is malformed");
}

// The time an await that started at `started_at` (Unix milliseconds) with
// `timeout_ms` has left at `now`; zero once its deadline has passed.
std::uint64_t await_remaining_ms(std::uint64_t started_at, std::uint64_t timeout_ms,
                                 std::uint64_t now) {
    const auto elapsed = now > started_at ? now - started_at : 0U;
    return elapsed >= timeout_ms ? 0U : timeout_ms - elapsed;
}

PlanExecution plan_failure(ProgramTerminalStatus status,
                           std::string           code,
                           std::string           message,
                           std::string           operation_id) {
    PlanExecution result;
    result.status  = status;
    result.failure = ProgramFailure{std::move(code), safe_failure_message(std::move(message)),
                                    std::move(operation_id), "", 0, json::object()};
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
    return detail::javascript_command_effect_identity(control.bundle_id, command_ordinal, command);
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
// Whether a Core thread belongs to a command that finished before
// `ordinal`. The run keeps the checkpoint of the last Core call it published;
// when the process stops inside a later command, that checkpoint is the
// earlier command's and gives no authority to resume the pending one.
bool checkpoint_of_a_completed_command(
    const RunControl&                                        control,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& commands,
    std::uint64_t                                            ordinal,
    std::string_view                                         core_thread_id) {
    for (const auto& entry : commands) {
        if (entry.command_ordinal() >= ordinal || !entry.completed()) continue;
        if (javascript_call_core_operation_for_thread(
                control, entry.command(), javascript_command_operation_id(entry.command_ordinal()),
                core_thread_id))
            return true;
    }
    return false;
}
// #402: the callCore operations of a command, through ng.await and the
// members of a join, each with whether it provably never started: it sits in
// an ng.await with a timeout whose start the command journal does not record
// (#391 records it before the await dispatches anything). False when the
// command holds work of another kind (a native call, a child, an emit),
// which a checkpoint cannot recover.
bool collect_call_core_operations(const JavaScriptCommand& command, std::string operation_id,
                                  const json& await_starts, bool unstarted,
                                  std::vector<std::pair<std::string, bool>>& out,
                                  std::size_t depth = 0) {
    if (depth > kMaxJavaScriptStructuredScopeDepth) return false;
    if (command.kind() == JavaScriptCommandKind::CallCore) {
        out.emplace_back(std::move(operation_id), unstarted);
        return true;
    }
    const auto arguments = command.arguments();
    if (command.kind() == JavaScriptCommandKind::Await) {
        const bool timed = arguments.contains("timeout_ms") && !arguments.at("timeout_ms").is_null();
        const bool recorded = await_starts.is_object() && await_starts.contains(operation_id);
        return collect_call_core_operations(JavaScriptCommand::from_json(arguments.at("command")),
                                            operation_id + "/await", await_starts,
                                            unstarted || (timed && !recorded), out, depth + 1);
    }
    if (command.kind() == JavaScriptCommandKind::Join) {
        const auto& members = arguments.at("members");
        for (std::size_t index = 0; index < members.size(); ++index)
            if (!collect_call_core_operations(JavaScriptCommand::from_json(members.at(index)),
                                              operation_id + "/member/" + std::to_string(index),
                                              await_starts, unstarted, out, depth + 1))
                return false;
        return true;
    }
    return false;
}

// #402: how the callCore operations of a pending command continue when the
// process that dispatched it stopped. Each one with a Core checkpoint
// continues from the latest checkpoint of its own Core thread (`saved`, by
// operation id), unless that checkpoint is an interrupt_before or
// interrupt_after pause: resuming it would pass the gate without its answer.
// One that provably never started runs as new work (`unstarted`). Nullopt
// when some call may have started but none can continue from a checkpoint,
// or when the command holds work other than callCore: the command is
// reconciled.
struct JavaScriptRecovery {
    std::map<std::string, std::string> saved;
    std::set<std::string>              unstarted;
};
std::optional<JavaScriptRecovery> javascript_recovery_checkpoints(const RunControl&         control,
                                                                  const JavaScriptCommand& command,
                                                                  const std::string& operation_id,
                                                                  const json& await_starts) {
    if (!control.checkpoints) return std::nullopt;
    std::vector<std::pair<std::string, bool>> calls;
    if (!collect_call_core_operations(command, operation_id, await_starts, false, calls) ||
        calls.empty())
        return std::nullopt;
    JavaScriptRecovery recovery;
    bool               may_have_started = false;
    for (const auto& [call, unstarted] : calls) {
        if (unstarted) {
            recovery.unstarted.insert(call);
            continue;
        }
        may_have_started = true;
        const auto latest = control.checkpoints->load_latest(operation_thread_id(control, call));
        if (!latest || latest->interrupt_phase == graph::CheckpointPhase::Before ||
            latest->interrupt_phase == graph::CheckpointPhase::After)
            continue;
        recovery.saved.emplace(call, latest->id);
    }
    if (may_have_started && recovery.saved.empty()) return std::nullopt;
    return recovery;
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

// #404: the reconciliation a plan operation that may have started before the
// process stopped asks for. Its result is the operation's output.
ProgramPendingEffect plan_operation_pending_effect(const RunControl&  control,
                                                   const std::string& operation_id) {
    const auto               effect_identity = detail::sha256_identity(
        "program-plan-operation-effect/v1",
        detail::canonical_json_bytes(json{{"bundle_id", control.bundle_id},
                                          {"run_id", control.run_id},
                                          {"operation_id", operation_id}}));
    ProgramPendingEffectData pending;
    pending.operation_id         = operation_id;
    pending.call_id              = effect_identity;
    pending.effect_id            = effect_identity;
    pending.result_schema        = json::object();
    pending.payload              = json{{"operation_id", operation_id}};
    pending.effect_mode          = EffectMode::Brokered;
    pending.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    pending.core_node            = "plan";
    pending.core_interrupt_value = json{{"operation_id", operation_id}};
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

JavaScriptRecordedCommandResult decode_javascript_command_result(
    const json& value, const RunControl& control) {
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
        if (detail::has_provider_outcome(*result.failure)) {
            const auto& custody = result.failure->witness.at("provider_outcome_custody");
            const auto& observed_attempt = custody.at("attempt");
            if (!observed_attempt.is_number_unsigned() || observed_attempt.get<std::uint64_t>() == 0)
                throw std::invalid_argument("Recorded provider outcome has no original attempt custody");
            const auto source_run = control.recorded_replay
                ? control.recorded_replay->source_run_id : control.run_id;
            detail::restore_provider_failure(
                *result.failure,
                detail::provider_failure_custody(
                    control.owner_scope, source_run, control.program_version_id, control.bundle_id,
                    result.failure->operation_id, observed_attempt.get<std::uint64_t>()),
                control.native_history_archive);
        }
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
// Also a std::nested_exception: it is constructed in the handler of the Core
// failure it reports, so nested_ptr() is that failure (a NodeExecutionError
// with the executor's record), and the node failure classifier walks it.
class NestedOperationFailure final : public std::runtime_error, public std::nested_exception {
public:
    NestedOperationFailure(std::string   operation_id,
                           std::string   message,
                           std::string   core_node               = {},
                           std::uint32_t attempts                = 0,
                           bool          checkpoint_incompatible = false,
                           std::exception_ptr cause = {})
        : std::runtime_error(std::move(message)),
          operation_id(std::move(operation_id)),
          core_node(std::move(core_node)),
          attempts(attempts),
          checkpoint_incompatible(checkpoint_incompatible), cause(std::move(cause)) {}

    std::string   operation_id;
    std::string   core_node;
    std::uint32_t attempts                = 0;
    bool          checkpoint_incompatible = false;
    std::exception_ptr cause;
};

// #364: the executor's record of a Core node failure, through any
// NestedOperationFailure wrapping it; empty for other failures.
std::optional<graph::NodeFailure> node_failure_in(std::exception_ptr error) {
    for (int depth = 0; error && depth < 64; ++depth) {
        try {
            std::rethrow_exception(error);
        } catch (const graph::NodeExecutionError& failure) {
            if (const auto* record = graph::node_failure_of(failure)) return *record;
            return std::nullopt;
        } catch (const NestedOperationFailure& failure) {
            error = failure.nested_ptr();
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// #364: a Core node failure fails the Program with its classified code (an
// author code, NG.TaskFailed, NG.ProviderFailed, ...) instead of
// P_RUNTIME_CORE_FAILURE, and its structured record as witness.node_failure.
// A failure without an executor record keeps its code.
void apply_node_failure(ProgramFailure& failure, const std::exception_ptr& cause) {
    const auto record = node_failure_in(cause);
    if (!record) return;
    failure.code = record->code;
    if (!failure.witness.is_object()) failure.witness = json::object();
    failure.witness["node_failure"] = json{{"node", record->node},
                                           {"code", record->code},
                                           {"kind", graph::to_string(record->kind)},
                                           {"attempts", record->attempts},
                                           {"retryable", record->retryable},
                                           {"routable", record->routable}};
}

// #364: whether the Program Retry operation may run a failed body again. The
// node failure classifier decides, from the executor's record when there is
// one: only ordinary failures and attempt timeouts are retried, never a
// committed or uncertain effect, a provider, configuration, logic or budget
// failure, a node past its visit limit, or a node that stopped retrying at the
// run's retry ceiling or because a backoff would cross the run's deadline.
bool program_retryable(const std::exception_ptr& error, const graph::CancelToken* token) {
    if (const auto record = node_failure_in(error))
        return record->retryable && record->kind != graph::NodeFailureKind::Ceiling &&
               record->kind != graph::NodeFailureKind::Deadline;
    return ::neograph::detail::classify_node_failure(error, token).retryable;
}

sp::runtime::Result provider_outcome_in(std::exception_ptr cause) {
    while (cause) {
        try {
            std::rethrow_exception(cause);
        } catch (const ProviderFailure& error) {
            return error.outcome();
        } catch (const ProviderOutcomeError& error) {
            return error.outcome();
        } catch (const graph::NodeExecutionError& error) {
            cause = error.cause();
        } catch (const NestedOperationFailure& error) {
            cause = error.cause;
        } catch (...) {
            return {};
        }
    }
    return {};
}

json provider_error_witness(const std::exception_ptr& cause) {
    if (!cause) return nullptr;
    try {
        std::rethrow_exception(cause);
    } catch (const ProviderFailure& error) {
        return json{{"kind", "provider_failure"},
                    {"message", safe_failure_message(error.what())}};
    } catch (const ProgramProviderOutcomePersistenceError& error) {
        json witness{{"kind", "program_provider_persistence"},
                     {"message", safe_failure_message(error.what())},
                     {"cause", provider_error_witness(error.cause())}};
        if (error.observer_error())
            witness["observer"] = provider_error_witness(error.observer_error());
        return witness;
    } catch (const ProviderObserverError& error) {
        return json{{"kind", "provider_observer"},
                    {"error_kind", static_cast<std::uint64_t>(error.error_kind())},
                    {"message", safe_failure_message(error.what())},
                    {"cause", provider_error_witness(error.cause())}};
    } catch (const ProviderOutcomeError& error) {
        json witness{{"kind", "provider_outcome"}, {"message", safe_failure_message(error.what())},
                     {"cause", provider_error_witness(error.cause())}};
        if (const auto* nested = dynamic_cast<const std::nested_exception*>(&error);
            nested && nested->nested_ptr())
            witness["secondary"] = provider_error_witness(nested->nested_ptr());
        return witness;
    } catch (const graph::NodeExecutionError& error) {
        return json{{"kind", "node"}, {"message", safe_failure_message(error.what())},
                    {"cause", provider_error_witness(error.cause())}};
    } catch (const NestedOperationFailure& error) {
        return json{{"kind", "operation"}, {"message", safe_failure_message(error.what())},
                    {"cause", provider_error_witness(error.cause)}};
    } catch (const std::exception& error) {
        json witness{{"kind", "exception"}, {"message", safe_failure_message(error.what())}};
        if (const auto* nested = dynamic_cast<const std::nested_exception*>(&error);
            nested && nested->nested_ptr())
            witness["cause"] = provider_error_witness(nested->nested_ptr());
        return witness;
    } catch (...) {
        return json{{"kind", "non_standard_exception"}};
    }
}

void retain_provider_failure(ProgramFailure& failure, const std::exception_ptr& cause) {
    if (auto outcome = provider_outcome_in(cause)) {
        failure.provider_outcome = std::move(outcome);
        failure.provider_cause = cause;
        if (!failure.witness.is_object()) failure.witness = json::object();
        failure.witness["provider_error"] = provider_error_witness(cause);
    }
}

// #399: the result of a plan branch whose Core node failed, which the branch
// threw as a NestedOperationFailure: the failure the run would have reported
// had it ended there, now the branch's own Failed result. Empty for anything
// else the branch threw (cancellation, a checkpoint that is gone, ...), which
// still ends the run.
std::optional<PlanExecution> failed_branch_of(const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const NestedOperationFailure& failure) {
        if (failure.checkpoint_incompatible) return std::nullopt;
        auto branch = plan_failure(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                   failure.what(), failure.operation_id);
        branch.failure->core_node = failure.core_node;
        branch.failure->attempts  = failure.attempts;
        retain_provider_failure(*branch.failure, failure.cause);
        apply_node_failure(*branch.failure, error);
        return branch;
    } catch (...) {
        return std::nullopt;
    }
}

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

// A Core run that stopped at a barrier which can no longer fire did not
// complete; its witness lists each stalled join and its missing members.
json stalled_barriers_of(const graph::RunResult& result) {
    if (!result.barrier_stalled()) return json();
    const json& output = result.output;  // const access never inserts keys
    return output["_neograph"]["barrier_stalled"];
}

// #364: a Core run that ended at a failure exit failed with the exit's code.
json failure_exit_of(const graph::RunResult& result) {
    if (!result.failure_exited()) return json();
    const json& output = result.output;  // const access never inserts keys
    return output["_neograph"]["failure_exit"];
}

ProgramFailure failure_exit_failure(std::string operation_id, const json& exit) {
    auto node = exit.value("node", std::string());
    auto code = exit.value("code", std::string(graph::kAbortedCode));
    return ProgramFailure{std::move(code),
                          "Core run ended at failure exit '" + node + "'",
                          std::move(operation_id),
                          std::move(node),
                          0,
                          json{{"failure_exit", exit}}};
}

ProgramFailure barrier_stall_failure(std::string operation_id, json stalls) {
    std::string node;
    if (stalls.is_array() && !stalls.empty() && stalls[0].is_object())
        node = stalls[0].value("node", std::string());
    return ProgramFailure{"P_CORE_BARRIER_STALLED",
                          "Core run stopped at a barrier that can no longer fire, "
                          "without reaching __end__",
                          std::move(operation_id),
                          std::move(node),
                          0,
                          json{{"stalled_barriers", std::move(stalls)}}};
}

RunOutcome failed_outcome(const std::shared_ptr<RunControl>& control,
                          std::string                        code,
                          std::string                        message,
                          std::string                        core_node = {},
                          std::uint32_t                      attempts  = 0) {
    RunOutcome outcome;
    outcome.status           = ProgramTerminalStatus::Failed;
    outcome.remaining_budget = control->granted_budget;
    outcome.failure = ProgramFailure{std::move(code), safe_failure_message(std::move(message)),
                                     "root", std::move(core_node), attempts, json::object()};
    return outcome;
}
std::uint64_t model_tokens(const std::shared_ptr<UsageAccumulator>& usage) noexcept {
    const auto total = usage->total_tokens_wide();
    return total <= 0 ? 0 : static_cast<std::uint64_t>(total);
}

bool apply_terminal_cause(const std::shared_ptr<RunControl>& control,
                          RunOutcome&                        outcome,
                          CancellationCause                  cause,
                          std::string_view                   message) {
    if (cause == CancellationCause::None) return false;
    auto provider_outcome = outcome.failure ? std::move(outcome.failure->provider_outcome)
                                           : sp::runtime::Result{};
    auto provider_cause = outcome.failure ? std::move(outcome.failure->provider_cause)
                                         : std::exception_ptr{};
    json causal_witness = json::object();
    if (outcome.failure && outcome.failure->witness.is_object() &&
        outcome.failure->witness.contains("provider_error"))
        causal_witness["provider_error"] = std::move(outcome.failure->witness["provider_error"]);
    if (cause == CancellationCause::EventSink) {
        outcome.status = ProgramTerminalStatus::Failed;
        outcome.failure = ProgramFailure{"P_EVENT_SINK", safe_failure_message(std::string(message)),
                                         "root", "", 0, std::move(causal_witness),
                                         std::move(provider_outcome), std::move(provider_cause)};
        return true;
    }
    auto failure_message = safe_failure_message(std::string(message));
    if (cause == CancellationCause::Timeout) {
        // A timeout names the budget that expired and how far the run took it
        // (#443); the text of whatever the cancellation interrupted stays in
        // the witness (#370). A run that completes past its deadline ends the
        // same way (see completed_run_overrun()).
        causal_witness["cause_message"] = std::move(failure_message);
        outcome.status  = ProgramTerminalStatus::TimedOut;
        outcome.failure = wall_time_failure(control->granted_budget.wall_time_ms,
                                            outcome.usage.wall_time_ms, std::move(causal_witness));
        outcome.failure->provider_outcome = std::move(provider_outcome);
        outcome.failure->provider_cause   = std::move(provider_cause);
        return true;
    }
    outcome.status  = ProgramTerminalStatus::Cancelled;
    outcome.failure = ProgramFailure{
        "P_RUNTIME_CANCELLED",
        std::move(failure_message),
        "root",
        "",
        0,
        std::move(causal_witness), std::move(provider_outcome), std::move(provider_cause)};
    return true;
}

CancellationCause terminal_cause_at_deadline(const std::shared_ptr<RunControl>&    control,
                                             std::chrono::steady_clock::time_point deadline) {
    if (std::chrono::steady_clock::now() >= deadline) {
        control->cancel(CancellationCause::Timeout);
    }
    return control->seal_terminal_cause();
}

void cancel_deadline_timer(const asio::any_io_executor&      executor,
                           std::shared_ptr<async::HostTimer> timer) {
    // HostTimer::cancel also takes the deadline back from a host loop, so no host timer stays armed (#557).
    asio::post(executor, [timer = std::move(timer)] {
        try {
            timer->cancel();
        } catch (...) {}
    });
}
asio::awaitable<void> cancel_after_timer(std::shared_ptr<async::HostTimer> timer,
                                         std::shared_ptr<graph::CancelToken> token,
                                         std::shared_ptr<std::atomic<bool>> expired) {
    asio::error_code wait_error;
    co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, wait_error));
    if (!wait_error) {
        // Cancelling the child can complete its branch before this timer's
        // co_spawn completion is delivered. Record the cause first.
        expired->store(true, std::memory_order_release);
        token->cancel();
    }
}

constexpr std::string_view PROGRAM_PENDING_KIND_FIELD = "__neograph_program_pending_kind";

// #388: the pending value carries the Core interrupt's id when the Core step
// paused on several, so that resuming the run answers exactly that interrupt;
// with one, it carries none and keeps its bytes. A replayed question
// (replay_branch_record) passes the id it recorded.
std::string core_interrupt_id_of(const graph::RunResult& result) {
    return result.interrupts.size() > 1 ? result.interrupt_id : std::string{};
}

ProgramInterrupt decode_core_interrupt(const RunControl&          control,
                                       std::string_view           operation_id,
                                       const graph::RunResult&    result,
                                       std::optional<std::string> recorded_interrupt_id =
                                           std::nullopt) {
    const auto core_interrupt_id =
        recorded_interrupt_id ? *recorded_interrupt_id : core_interrupt_id_of(result);
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
        pending.core_interrupt_id    = core_interrupt_id;
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
        pending.core_interrupt_id    = core_interrupt_id;
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
    pending.core_interrupt_id    = core_interrupt_id;
    return ProgramInterrupt{result.interrupt_node, result.interrupt_value,
                            ProgramPendingInput(std::move(pending)), std::nullopt};
}

// A paused plan records what the attempt that resumes it replays, by
// operation id (#384, #386):
//   {"kind": "settled", ...}  a parallel branch that finished: its result,
//                             with the codec of a JavaScript join's settled
//                             members (#380);
//   {"kind": "paused", ...}   a call that paused on a question other than
//                             the one the run pauses on: its question and
//                             checkpoint;
//   {"kind": "entered", ...}  an operation the run was executing: it is
//                             re-entered without being charged again, and
//                             continues where it was (the next child of a
//                             sequence, the iteration of a loop, the attempt
//                             of a retry, the item of a map, the winner of a
//                             race), with the state that position had. The
//                             root's record also holds the plan's input.
// The cursor of an entered record, bounds-checked.
std::uint64_t reentry_position(const json& record, const char* field, std::uint64_t bound) {
    if (!record.contains(field) || !record.at(field).is_number_unsigned() ||
        record.at(field).get<std::uint64_t>() >= bound)
        throw_runtime_diagnostic("P_PLAN_REPLAY_RECORD",
                                 "Recorded plan operation position is malformed",
                                 json{{"field", field}});
    return record.at(field).get<std::uint64_t>();
}

json settled_branch_record(const PlanExecution& branch) {
    return json{{"kind", "settled"},
                {"returned", branch.returned},
                {"result", javascript_command_terminal_result(branch, to_string(branch.status),
                                                              ProgramUsage{})}};
}

// #398: the settled record of a branch that finished, its provider evidence
// stored under this run's custody, which a run continuing this one re-binds
// to its own. Empty when the evidence cannot be stored: the branch then runs
// again on resume.
std::optional<json> settled_branch_record_of(PlanExecution& branch, const RunControl& control) {
    if (branch.failure && branch.failure->provider_outcome) {
        try {
            detail::persist_provider_failure(
                *branch.failure,
                detail::provider_failure_custody(control.owner_scope, control.run_id,
                                                 control.program_version_id, control.bundle_id,
                                                 branch.failure->operation_id, control.attempt),
                control.native_history_archive);
        } catch (...) {
            return std::nullopt;
        }
    }
    return settled_branch_record(branch);
}

// Empty when the branch has no Core call that can ask again by itself.
std::optional<json> paused_branch_record(const PlanExecution& branch) {
    if (!branch.interrupt || !branch.interrupt_checkpoint) return std::nullopt;
    const auto& interrupt = *branch.interrupt;
    std::string operation_id;
    std::string core_interrupt_id;
    if (interrupt.pending_input) {
        operation_id      = interrupt.pending_input->operation_id();
        core_interrupt_id = interrupt.pending_input->core_interrupt_id();
    } else if (interrupt.pending_effect) {
        operation_id      = interrupt.pending_effect->operation_id();
        core_interrupt_id = interrupt.pending_effect->core_interrupt_id();
    }
    if (operation_id.empty()) return std::nullopt;
    const auto& checkpoint = *branch.interrupt_checkpoint;
    auto record = json{{"kind", "paused"},
                {"operation_id", operation_id},
                {"core_node", interrupt.core_node},
                {"value", interrupt.value},
                {"checkpoint", json{{"core_name", checkpoint.core_name},
                                    {"core_generation_id", checkpoint.core_generation_id},
                                    {"core_thread_id", checkpoint.core_thread_id},
                                    {"checkpoint_id", checkpoint.checkpoint_id},
                                    {"checkpoint_schema_version",
                                     checkpoint.checkpoint_schema_version}}}};
    // #388: only when its Core step paused on several interrupts.
    if (!core_interrupt_id.empty()) record["core_interrupt_id"] = core_interrupt_id;
    return record;
}

// A settled or paused record as its operation sees it again: one that
// finished returns its result again; a paused call asks its question again,
// at its checkpoint.
PlanExecution replay_branch_record(const json& record, const RunControl& control) {
    const auto malformed = [] {
        throw_runtime_diagnostic("P_PLAN_REPLAY_RECORD",
                                 "Recorded plan operation is malformed");
    };
    if (!record.is_object() || !record.contains("kind") || !record.at("kind").is_string())
        malformed();
    const auto kind = record.at("kind").get<std::string>();
    if (kind == "settled") {
        detail::reject_unknown_fields(record, "Recorded plan operation",
                                      {"kind", "returned", "result"});
        if (!record.contains("returned") || !record.at("returned").is_boolean() ||
            !record.contains("result"))
            malformed();
        auto          recorded = decode_javascript_command_result(record.at("result"), control);
        PlanExecution settled;
        settled.status   = recorded.status;
        settled.output   = std::move(recorded.output);
        settled.failure  = std::move(recorded.failure);
        settled.returned = record.at("returned").get<bool>();
        // Its nodes ran, and were reported, in an earlier attempt (#374).
        return settled;
    }
    if (kind != "paused") malformed();
    detail::reject_unknown_fields(record, "Recorded plan operation",
                                  {"kind", "operation_id", "core_node", "value", "checkpoint",
                                   "core_interrupt_id"});
    if (record.contains("core_interrupt_id") && !record.at("core_interrupt_id").is_string())
        malformed();
    for (const auto* field : {"operation_id", "core_node"})
        if (!record.contains(field) || !record.at(field).is_string()) malformed();
    if (!record.contains("value") || !record.contains("checkpoint") ||
        !record.at("checkpoint").is_object())
        malformed();
    const auto& stored = record.at("checkpoint");
    detail::reject_unknown_fields(stored, "Recorded plan call checkpoint",
                                  {"core_name", "core_generation_id", "core_thread_id",
                                   "checkpoint_id", "checkpoint_schema_version"});
    for (const auto* field : {"core_name", "core_generation_id", "core_thread_id", "checkpoint_id"})
        if (!stored.contains(field) || !stored.at(field).is_string()) malformed();
    if (!stored.contains("checkpoint_schema_version") ||
        !stored.at("checkpoint_schema_version").is_number_unsigned())
        malformed();
    CoreCheckpointIdentity checkpoint{
        stored.at("core_name").get<std::string>(),
        stored.at("core_generation_id").get<std::string>(),
        stored.at("core_thread_id").get<std::string>(),
        stored.at("checkpoint_id").get<std::string>(),
        static_cast<std::uint32_t>(stored.at("checkpoint_schema_version").get<std::uint64_t>())};
    if (!checkpoint_matches_generation(control, checkpoint))
        throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                 "Recorded plan call checkpoint is incompatible");
    graph::RunResult question;
    question.interrupt_node  = record.at("core_node").get<std::string>();
    question.interrupt_value = detail::owned_json_copy(record.at("value"));
    PlanExecution paused;
    paused.status = ProgramTerminalStatus::Interrupted;
    paused.interrupt = decode_core_interrupt(
        control, record.at("operation_id").get<std::string>(), question,
        record.contains("core_interrupt_id")
            ? std::optional<std::string>(record.at("core_interrupt_id").get<std::string>())
            : std::optional<std::string>(std::string{}));
    paused.interrupt_checkpoint = std::move(checkpoint);
    return paused;
}

}  // namespace
std::string javascript_command_effect_identity(std::string_view         bundle_id,
                                               std::uint64_t            command_ordinal,
                                               const JavaScriptCommand& command) {
    json payload = json::object();
    payload["bundle_id"]       = std::string(bundle_id);
    payload["command_ordinal"] = command_ordinal;
    payload["command"]         = command.to_json();
    return detail::sha256_identity(
        "program-javascript-command-effect/v1",
        detail::canonical_json_bytes(payload));
}

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

asio::awaitable<std::optional<RunBudget>> held_command_remainder(const RunControl& control) {
    if (!control.materialized->bundle.control_source()) co_return std::nullopt;
    const auto held = co_await control.async_transitions().latest(control.owner_scope, control.run_id);
    if (held && has_resource_reservation(held->inflight_reservation)) co_return held->remaining_budget;
    co_return std::nullopt;
}

void set_join_member_launch_observer_for_testing(JoinMemberLaunchObserver observer) noexcept {
    join_member_launch_observer.store(observer, std::memory_order_release);
}

asio::awaitable<void> execute_run_attempt(std::shared_ptr<RunControl> control,
                                          json                        input,
                                          std::optional<std::string>  checkpoint_id,
                                          std::optional<json> javascript_core_resume_value) {
    const auto               started_at            = control->started_at;
    const auto               deadline              = control->deadline;
    auto                     usage                 = std::make_shared<UsageAccumulator>();
    if (control->recorded_replay) {
        usage->restore_authority(std::move(control->recorded_replay->provider_authority));
        usage->seal_observation();
    }
    // Recorded replay never spends money: its provider outcomes are captured.
    const auto monetary = control->recorded_replay
        ? std::shared_ptr<ProgramMonetaryAccount>{}
        : std::make_shared<ProgramMonetaryAccount>(control->granted_budget.monetary_microunits,
                                                   control->attempt);
    control->set_monetary_account(monetary);
    // #402: a journaling broker charges this attempt, when it binds an
    // operation, the tokens of the calls the attempt journaled before a
    // restart, as it charges their money: a recovered call that replays from
    // its checkpoint never invokes the broker again.
    if (!control->recorded_replay) control->set_journaled_usage(usage);
    const auto monetary_used = [&monetary]() -> std::uint64_t {
        return monetary ? monetary->used() : 0;
    };
    auto                     core_progress         = std::make_shared<AttemptCoreProgress>();
    std::uint64_t            operation_count       = 0;
    std::uint64_t            dynamic_compile_count = 0;
    std::atomic<std::size_t> active_concurrency{0};
    std::atomic<std::size_t> peak_concurrency{0};
    // #364: RunBudget::max_node_retries, shared by every Core call of the
    // attempt; its use is reported as ProgramUsage::node_retries.
    const auto node_retry_ceiling =
        control->granted_budget.max_node_retries
            ? std::make_shared<graph::NodeRetryCeiling>(*control->granted_budget.max_node_retries)
            : std::shared_ptr<graph::NodeRetryCeiling>{};
    const auto node_retries_used = [&node_retry_ceiling]() noexcept -> std::uint64_t {
        return node_retry_ceiling ? node_retry_ceiling->used() : 0;
    };
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
        // #419: a command reserves no retries, and the remainder it journaled
        // carries the ceiling over as the attempt was granted it, so the
        // attempt's retries come off the ceiling here, once.
        unreserved.node_retries = node_retries_used();
        return settle_budget(remaining, unreserved);
    };
    const auto apply_durable_javascript_disposition = [&](RunOutcome& terminal) -> asio::awaitable<void> {
        if (unreconciled_javascript_remaining) {
            terminal.remaining_budget =
                settle_unreserved_javascript_wall(*unreconciled_javascript_remaining);
        }
        if (!active_javascript_command) co_return;
        try {
            const auto latest = (co_await control->transitions->latest(control->owner_scope, control->run_id));
            if (!latest || !has_resource_reservation(latest->inflight_reservation)) co_return;
            if (terminal.failure && terminal.failure->provider_outcome) {
                // Publication failed after a known provider outcome. Retain
                // its evidence and all still-held command authority.
                terminal.remaining_budget = latest->remaining_budget;
                terminal.status = ProgramTerminalStatus::Failed;
                terminal.interrupt.reset();
                co_return;
            }
            terminal.status = ProgramTerminalStatus::AmbiguousEffect;
            terminal.failure.reset();
            terminal.interrupt = ProgramInterrupt{
                "javascript", active_javascript_command->command.to_json(), std::nullopt,
                javascript_command_pending_effect(*control, active_javascript_command->ordinal,
                                                  active_javascript_command->command,
                                                  active_javascript_command->effect_identity)
                    .mark_outcome_unknown(static_cast<std::uint64_t>(latest->timestamp_ms)).value};
        } catch (...) {
            // The already-published reservation remains durable when the
            // transition store itself cannot be read during terminalization.
        }
    };
    if (std::chrono::steady_clock::now() >= deadline) (void)co_await control->cancel_async(CancellationCause::Timeout);

    auto timer = std::make_shared<async::HostTimer>(control->deadline_executor);
    if (deadline < std::chrono::steady_clock::time_point::max()) {
        timer->arm_at(deadline);
        asio::co_spawn(
            control->deadline_executor,
            [control, timer]() -> asio::awaitable<void> {
                asio::error_code error;
                co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
                if (!error) (void)co_await control->cancel_async(CancellationCause::Timeout);
                co_return;
            },
            asio::detached);
    } else {
        timer->arm_never();
    }

    std::optional<CoreCheckpointIdentity> resume_checkpoint;
    std::optional<std::string>            resume_checkpoint_content_id;
    RunOutcome                            outcome;
    // The handlers below cannot suspend: they note that the durable command disposition is owed, and it is read after them.
    bool                                  needs_durable_disposition = false;
    // #443: the plan completed over a budget, and `outcome` has the record of it.
    bool                                  completed_over_budget = false;
    try {
        const auto control_source = control->materialized->bundle.control_source();
        // A resume of an interrupted callCore reserves the capacity of the call in the journal, and the call settles
        // the reservation with the usage it measured; the attempt that recovers a run stopped inside a command finds the
        // reservation of that command held. The grant of this attempt includes the reservation, but a run that ends
        // before the command has settled it must not return it unmeasured: it ends with the remainder the journal
        // holds, and the reservation stays unreturned.
        if (control_source) unreconciled_javascript_remaining = co_await held_command_remainder(*control);
        if (checkpoint_id) {
            resume_checkpoint = inspect_checkpoint(*control, *checkpoint_id);
            if (const auto durable = co_await control->async_transitions().load(
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
                // No node of the call this attempt continues has run: it ends with the remainder beneath the reservation.
                co_await apply_durable_javascript_disposition(outcome);
                cancel_deadline_timer(control->deadline_executor, std::move(timer));
                (co_await control->complete_async(std::move(outcome)));
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

        PureMutex                            plan_mutex;
        OperationLock                        dynamic_compile_turn;
        std::optional<std::string>            resume_id = checkpoint_id;
        std::optional<CoreCheckpointIdentity> last_root_checkpoint;
        // #384, #386: a resumed plan continues the Core call its pending
        // question names, with the value it was resumed with, and replays
        // everything else the paused run recorded (settled_branches). Every
        // other operation runs on the state it had the first time. Records are
        // used once (plan_mutex).
        std::optional<std::string> resume_operation;
        std::optional<json>        resume_answer;
        json                       replay_records = json::object();
        std::set<std::string>      replayed_records;
        if (!checkpoint_id && !control_source && !control->recorded_replay) {
            // #404: a plan operation reconciled as completed replays its result.
            const auto durable = co_await control->async_transitions().load(control->owner_scope, control->run_id);
            if (durable && durable->pending_effect() &&
                durable->pending_effect()->core_node() == "plan") {
                if (const auto settled = durable->settled_branches()) replay_records = *settled;
            }
        }
        if (checkpoint_id && !control_source && !control->recorded_replay) {
            const auto durable = co_await control->async_transitions().load(control->owner_scope, control->run_id);
            if (durable && durable->exact_checkpoint() &&
                durable->exact_checkpoint()->checkpoint_id == *checkpoint_id) {
                if (const auto pending = durable->pending_input())
                    resume_operation = pending->operation_id();
                else if (const auto effect = durable->pending_effect())
                    resume_operation = effect->operation_id();
                if (resume_operation && !typed_plan.find(*resume_operation))
                    resume_operation.reset();
                if (const auto settled = durable->settled_branches()) replay_records = *settled;
            }
        }
        // #388: the Core interrupt that the resumed question names, when its
        // Core step paused on several: the resume of that checkpoint answers
        // exactly it. Taken once (plan_mutex).
        std::optional<std::pair<std::string, std::string>> resume_core_interrupt;
        if (checkpoint_id && !control->recorded_replay) {
            const auto durable = co_await control->async_transitions().load(control->owner_scope, control->run_id);
            if (durable && durable->exact_checkpoint() &&
                durable->exact_checkpoint()->checkpoint_id == *checkpoint_id) {
                std::string id;
                if (const auto pending = durable->pending_input())
                    id = pending->core_interrupt_id();
                else if (const auto effect = durable->pending_effect())
                    id = effect->core_interrupt_id();
                if (!id.empty()) resume_core_interrupt = std::pair{*checkpoint_id, std::move(id)};
            }
        }
        auto take_replay_record = [&](const std::string& operation_id) -> std::optional<json> {
            std::lock_guard lock(plan_mutex);
            if (replayed_records.count(operation_id)) return std::nullopt;
            const auto record = replay_records.find(operation_id);
            if (record == replay_records.end()) return std::nullopt;
            replayed_records.insert(operation_id);
            return detail::owned_json_copy(record.value());
        };
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

        std::function<asio::awaitable<std::optional<PlanExecution>>(const JavaScriptCommand&, std::string,
                                                                    std::size_t)>
            validate_javascript_command;
        validate_javascript_command = [&](const JavaScriptCommand& command,
                                          std::string              operation_id,
                                          std::size_t scope_depth) -> asio::awaitable<std::optional<PlanExecution>> {
            const auto fail = [&](ProgramTerminalStatus status, std::string code,
                                  std::string message) {
                return std::optional<PlanExecution>(
                    plan_failure(status, std::move(code), std::move(message), operation_id));
            };
            if (scope_depth > kMaxJavaScriptStructuredScopeDepth)
                co_return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
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
                co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                            "JavaScript command protocol or import slot is not admitted");
            if (command.kind() == JavaScriptCommandKind::HostCapability) {
                const auto native =
                    control->materialized->native_bindings.find(command.import_slot());
                if (native == control->materialized->native_bindings.end())
                    co_return fail(
                        ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_BINDING",
                        "JavaScript host capability import slot is absent from the admitted run");
                try {
                    const auto input = command.arguments().at("input");
                    validate_contract_value(input, native->second.binding.metadata().input_contract,
                                            "Native binding input");
                    if (detail::canonical_json_bytes(input).size() >
                        native->second.binding.metadata().resource_declaration.max_input_bytes)
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                    "Native binding input exceeds its admitted byte limit");
                } catch (const std::exception& error) {
                    co_return fail(ProgramTerminalStatus::Failed, "P_JS_HOST_CAPABILITY_INPUT",
                                error.what());
                }
            }

            const auto arguments = command.arguments();
            try {
                if (command.kind() == JavaScriptCommandKind::CallCore) {
                    if (arguments.at("name").get<std::string>() !=
                        control->materialized->root->core_name)
                        co_return fail(
                            ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                            "JavaScript command references a Core outside the admitted root");
                } else if (command.kind() == JavaScriptCommandKind::Spawn) {
                    (co_await control->validate_child_binding(
                        arguments.at("child_binding").get<std::string>(), operation_id));
                } else if (command.kind() == JavaScriptCommandKind::Await) {
                    const auto child = JavaScriptCommand::from_json(arguments.at("command"));
                    if (auto child_failure = (co_await validate_javascript_command(
                            child, operation_id + "/await", scope_depth + 1)))
                        co_return child_failure;
                } else if (command.kind() == JavaScriptCommandKind::Join) {
                    const auto  mode            = arguments.at("mode").get<std::string>();
                    const auto& encoded_members = arguments.at("members");
                    if ((mode != "all" && mode != "race" && mode != "quorum") ||
                        !encoded_members.is_array() || encoded_members.empty() ||
                        encoded_members.size() > 1'000'000u)
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE_LIMIT",
                                    "JavaScript join member bounds are invalid");
                    if (mode == "race" && encoded_members.size() < 2)
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                    "JavaScript race requires at least two members");
                    if (mode == "quorum") {
                        const auto required =
                            arguments.at("required_successes").get<std::uint64_t>();
                        if (required == 0 || required > encoded_members.size())
                            co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND",
                                        "JavaScript quorum bounds are invalid");
                    }
                    if (arguments.contains("max_in_flight")) {
                        const auto max_in_flight =
                            arguments.at("max_in_flight").get<std::uint64_t>();
                        if (max_in_flight == 0 ||
                            max_in_flight > control->granted_budget.max_concurrency)
                            co_return fail(ProgramTerminalStatus::BudgetExhausted,
                                        "P_CONCURRENCY_BUDGET",
                                        "JavaScript join max_in_flight exceeds its admitted "
                                        "concurrency budget");
                    }
                    for (std::size_t index = 0; index < encoded_members.size(); ++index) {
                        const auto member = JavaScriptCommand::from_json(encoded_members.at(index));
                        if (auto member_failure = (co_await validate_javascript_command(
                                member, operation_id + "/member/" + std::to_string(index),
                                scope_depth + 1)))
                            co_return member_failure;
                    }
                } else if (command.kind() == JavaScriptCommandKind::CancelScope) {
                    const auto scope = arguments.at("scope").get<std::string>();
                    if (scope != "run" && scope != "root" && scope != "current" &&
                        scope != "nested")
                        co_return fail(ProgramTerminalStatus::Failed, "P_JS_SCOPE",
                                    "JavaScript cancelScope references an unknown scope");
                }
            } catch (const ProgramDiagnosticError& error) {
                co_return fail(ProgramTerminalStatus::Failed, error.diagnostic().code,
                            error.diagnostic().message);
            } catch (const std::exception& error) {
                co_return fail(ProgramTerminalStatus::Failed, "P_JS_CONTROL_COMMAND", error.what());
            }
            co_return std::nullopt;
        };

        // #402: the checkpoints a recovered command's calls continue from;
        // their content is not the run's exact checkpoint.
        std::set<std::string> recovery_checkpoint_ids;
        // #404: a plan that stopped before it published any checkpoint. Its
        // calls run again in plan order; the n-th call on a Core thread
        // continues the n-th Core run that thread holds (each run's
        // checkpoints chain from its first, which has no parent) from its
        // latest checkpoint. A call whose run saved nothing may have started:
        // the host reconciles it. So does one whose latest checkpoint is an
        // interrupt_before or interrupt_after pause, which a resume would pass.
        std::map<std::string, std::size_t> recovery_calls_on_thread;
        const auto plan_recovery_checkpoint =
            [&](const std::string& thread) -> std::optional<std::string> {
            std::size_t index = 0;
            {
                std::lock_guard lock(plan_mutex);
                index = recovery_calls_on_thread[thread]++;
            }
            const auto history = control->checkpoints->list(thread, 100000);
            std::vector<const graph::Checkpoint*> runs;
            for (auto stored = history.rbegin(); stored != history.rend(); ++stored) {
                if (stored->parent_id.empty() || runs.empty()) runs.push_back(&*stored);
                else runs.back() = &*stored;
            }
            if (index >= runs.size()) return std::nullopt;
            const auto* latest = runs[index];
            if (latest->interrupt_phase == graph::CheckpointPhase::Before ||
                latest->interrupt_phase == graph::CheckpointPhase::After)
                return std::nullopt;
            {
                std::lock_guard lock(plan_mutex);
                recovery_checkpoint_ids.insert(latest->id);
            }
            return latest->id;
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
                config.model_token_budget = control->recorded_replay
                    ? control->invocation.budget.model_tokens : control->granted_budget.model_tokens;
                // A grant is an amount, and 0 grants nothing: Core holds every model call against the ceiling,
                // 0 included, and sends none under it. A recorded replay sends none, so its ceiling is only read.
                config.model_token_budget_bounded = !control->recorded_replay;
                config.budget_exhausted   = control->budget_exhausted;
                config.native_history_archive = control->native_history_archive;

                graph::RunMetadata metadata;
                metadata.deadline    = deadline;
                metadata.run_id      = control->run_id;
                metadata.owner_scope = control->owner_scope;
                metadata.trace_id    = control->trace_id;
                // Join members and await timers share this scheduler.
                metadata.shared_scheduler = true;
                // #557: with a host's executor there is no process-wide attempt pool to dispatch node attempts to.
                if (control->scheduler->injected) metadata.dispatch_executor = control->scheduler->attempts;
                metadata.node_retry_ceiling = node_retry_ceiling;
                if (next_resume) {
                    std::lock_guard lock(plan_mutex);
                    if (resume_core_interrupt && resume_core_interrupt->first == *next_resume) {
                        metadata.resume_interrupt_id = std::move(resume_core_interrupt->second);
                        resume_core_interrupt.reset();
                    }
                }
                auto operation_checkpoints = control->checkpoints;
                if (next_resume && resume_checkpoint_content_id &&
                    !recovery_checkpoint_ids.count(*next_resume)) {
                    operation_checkpoints =
                        std::make_shared<ExactCheckpointContentStore>(
                            control->checkpoints, *next_resume,
                            *resume_checkpoint_content_id);
                }
                graph::RunResources resources{operation_checkpoints, control->state_store};
                resources.provider_call_broker =
                    control->resolve_core_provider_call_broker(operation_id);
                // Core tool calls cross the host's tool Hooks like any other.
                resources.hook_runtime = control->hook_runtime();
                const auto grant = (co_await control->resolve_core_tool_grant(operation_id));
                if (grant && grant->owner_scope == control->owner_scope &&
                    grant->program_version_id == control->program_version_id &&
                    grant->run_id == control->run_id &&
                    grant->operation_id == operation_id &&
                    grant->attempt == control->attempt && !grant->grant_id.empty() &&
                    grant->binding_fingerprint == control->binding_fingerprint &&
                    grant->gate && grant->controller && grant->effect_broker) {
                    resources.tool_gate = grant->gate;
                    resources.tool_execution_controller = grant->controller;
                    resources.tool_effect_broker = grant->effect_broker;
                    resources.tool_effect_grant = {
                        grant->program_version_id, grant->operation_id,
                        grant->grant_id, grant->attempt};
                    resources.tool_effect_grant.binding_fingerprint = grant->binding_fingerprint;
                } else {
                    // A capability receipt binds code, not authority for an effect.
                    // Native nodes calling Tool::execute directly remain a separate
                    // trusted-code boundary; this gate covers mediated dispatch.
                    resources.tool_gate = [](ToolCall, ToolGateContext)
                        -> asio::awaitable<ToolDecision> {
                        co_return ToolDecision::deny("Program Core tool grant is absent or stale");
                    };
                }
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
                consumed.monetary_microunits = monetary_used();
                consumed.program_operations = operation_count;
                consumed.core_steps = core_progress->steps();
                consumed.peak_concurrency =
                    peak_concurrency.load(std::memory_order_relaxed);
                consumed.node_retries = node_retries_used();
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
            explicit ExpansionBatchState(const asio::any_io_executor& completion_executor)
                : join(completion_executor) {}
            PureMutex                                   mutex;
            std::vector<std::optional<ExpansionTaskRun>> results;
            std::vector<std::shared_ptr<RunControl>>     children;
            std::size_t                                  remaining = 0;
            StrandJoinSignal                             join;
            bool                                         failed = false;
            bool                                         stop_launching = false;
        };

        struct ExpansionRecordState {
            explicit ExpansionRecordState(const CompiledTaskGraphFragment& value)
                : record{value, {}, 0, false, false} {}
            // Held across the fragment store's calls and across the suspension of an awaiting store, so it is
            // an OperationLock and not a mutex (operation_lock.h).
            OperationLock                           lock;
            TaskGraphFragmentRecord                 record;
            std::shared_ptr<TaskGraphFragmentStore> store;
        };
        auto execute_expand = [&](const ProgramPlanNode& operation, json state,
                                  std::shared_ptr<graph::CancelToken> operation_token)
            -> asio::awaitable<PlanExecution> {
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
                    const auto recovered = co_await store->find_published_lineage(
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
                    } catch (const TaskGraphProposalError& error) {
                        // The planner's mistakes are data the host can act on: name the first
                        // few diagnostics by code and pointer.
                        json diagnostics = json::array();
                        for (const auto& diagnostic : error.diagnostics()) {
                            if (diagnostics.size() >= 16) break;
                            diagnostics.push_back(json{{"code", diagnostic.code},
                                                       {"pointer", diagnostic.primary.json_pointer}});
                        }
                        throw_runtime_diagnostic(
                            "P_EXPAND_REJECTED",
                            std::string("expand_task_graph proposal was rejected: ") + error.what(),
                            json{{"operation_id", operation.id()},
                                 {"diagnostics", std::move(diagnostics)}});
                    } catch (const std::exception& error) {
                        throw_runtime_diagnostic(
                            "P_EXPAND_REJECTED",
                            std::string("expand_task_graph proposal was rejected: ") + error.what(),
                            json{{"operation_id", operation.id()}});
                    }
                }();
                const auto recovered =
                    co_await store->find_published(control->owner_scope, control->run_id, operation.id(),
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
                        // consume_dynamic_compile publishes a transition, so expansions take turns
                        // on an OperationLock; plan_mutex guards the counter and nothing else.
                        const auto turn = co_await dynamic_compile_turn.lock_async();
                        bool       within_limit = false;
                        {
                            std::lock_guard lock(plan_mutex);
                            within_limit = dynamic_compile_count < compile_limit;
                        }
                        const bool consumed = within_limit && co_await control->consume_dynamic_compile();
                        if (!consumed) {
                            co_return plan_failure(
                                ProgramTerminalStatus::BudgetExhausted, "P_DYNAMIC_COMPILE_BUDGET",
                                "expand_task_graph dynamic compilation budget exhausted",
                                operation.id());
                        }
                        std::lock_guard lock(plan_mutex);
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
                        const auto publication = co_await store->publish(durable->record);
                        if (publication == TaskGraphPublishResult::Conflict ||
                            publication == TaskGraphPublishResult::Missing) {
                            co_return plan_failure(
                                ProgramTerminalStatus::Failed, "P_EXPAND_PUBLICATION",
                                "expand_task_graph fragment publication conflicted",
                                operation.id());
                        }
                        const auto loaded = co_await store->load(compiled_fragment->fragment_id());
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
            // A schema-2 fragment is held to what its aggregate counted (#412).
            const bool exact_budget =
                fragment.schema_version() >= CompiledTaskGraphFragment::SCHEMA_VERSION_2;

            auto failure_json = [](const std::optional<ProgramFailure>& failure) {
                if (!failure) return json::object();
                return json{{"code", failure->code},
                            {"message", failure->message},
                            {"operation_id", failure->operation_id},
                            {"core_node", failure->core_node},
                            {"attempts", failure->attempts},
                            {"witness", failure->witness}};
            };
            auto find_task = [&](std::string_view task_id) -> const CompiledTaskGraphTask& {
                for (const auto& task : fragment.tasks())
                    if (task.task_id == task_id) return task;
                throw_runtime_diagnostic("P_EXPAND_RECORD", "Published fragment task is missing",
                                         json{{"task_id", std::string(task_id)}});
            };
            auto update_task = [&](std::string task_id, TaskGraphTaskState task_state,
                                   std::uint32_t attempt, std::optional<json> output,
                                   std::optional<json>        failure,
                                   std::optional<std::string> attempt_identity,
                                   std::optional<std::string> child_run_id) -> asio::awaitable<void> {
                // Cancellation prevents launch, not settlement of already-dispatched evidence.
                if (task_state != TaskGraphTaskState::Cancelled &&
                    ((task_state == TaskGraphTaskState::Active &&
                      (operation_token->is_cancelled() ||
                       control->cancellation_cause() != CancellationCause::None)) ||
                     control->try_result().has_value()))
                    co_return;
                const auto record_lock = co_await durable->lock.lock_async();
                if (task_state != TaskGraphTaskState::Cancelled &&
                    ((task_state == TaskGraphTaskState::Active &&
                      (operation_token->is_cancelled() ||
                       control->cancellation_cause() != CancellationCause::None)) ||
                     control->try_result().has_value()))
                    co_return;
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
                const auto result   = co_await durable->store->compare_update(
                    durable->record.fragment.fragment_id(), durable->record.revision, candidate);
                if (result != TaskGraphPublishResult::Published &&
                    result != TaskGraphPublishResult::AlreadyPresent)
                    throw_runtime_diagnostic(
                        "P_EXPAND_CAS", "Published fragment task update lost its revision race",
                        json{{"task_id", std::string(task_id)},
                             {"expected_revision", durable->record.revision}});
                if (result == TaskGraphPublishResult::AlreadyPresent) {
                    const auto winner =
                        co_await durable->store->load(durable->record.fragment.fragment_id());
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
            // Tasks whose last attempt failed in a way that may be retried: the failure to keep
            // in the record while the next attempt runs. Not durable: a crash before the next
            // attempt starts relaunches the same attempt, whose child already holds the failure.
            std::map<std::string, json, std::less<>>               retry_failures;
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

            // The task coroutines borrow the expansion's locals. From the first dispatch
            // through its drain, scoped cancellation is handled by operation_token instead
            // of allowing Asio cancellation to unwind those borrowed locals.
            co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

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
                // Literals are assigned after the bindings; a target never has both.
                for (const auto& literal : task.input_literals) {
                    if (!assign_json_path(child_input, literal.to.field, literal.value)) {
                        result.failure =
                            ProgramFailure{"P_TASK_INPUT",
                                           "Task input literal could not be materialized",
                                           task.operation_id,
                                           "",
                                           0,
                                           json{{"target_field", literal.to.field}}};
                        co_return result;
                    }
                }
                if (operation_token->is_cancelled() ||
                    control->cancellation_cause() != CancellationCause::None) {
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
                    auto child = (co_await control->launch_child(task.child_binding, std::move(child_input),
                                                       operation.id(), result.attempt_identity,
                                                       exact_budget ? task.requested_budget
                                                                    : task.reserved_budget,
                                                       exact_budget));
                    result.child_run_id = child->run_id;
                    bool cancel_child = false;
                    {
                        std::lock_guard lock(batch_state->mutex);
                        cancel_child = batch_state->stop_launching ||
                                       operation_token->is_cancelled();
                        if (!cancel_child) batch_state->children.push_back(child);
                    }
                    if (cancel_child) (void)child->cancel(CancellationCause::ParentTerminal);
                    const auto child_result = co_await child->wait_async();
                    result.status           = child_result.status();
                    result.output           = child_result.output();
                    result.failure          = child_result.failure();
                    if (result.status == ProgramTerminalStatus::Completed && !result.failure) {
                        result.failure = validate_task_output(task, result.output, exact_budget);
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

                if (operation_token->is_cancelled() ||
                    control->cancellation_cause() != CancellationCause::None) {
                    for (const auto& task : fragment.tasks()) {
                        if (states[task.task_id] == TaskGraphTaskState::Completed ||
                            states[task.task_id] == TaskGraphTaskState::Failed ||
                            states[task.task_id] == TaskGraphTaskState::Cancelled)
                            continue;
                        states[task.task_id] = TaskGraphTaskState::Cancelled;
                        const auto failure   = json{{"code", "P_TASK_CANCELLED"},
                                                    {"message", "Parent expansion was cancelled"},
                                                    {"operation_id", task.operation_id}};
                        co_await update_task(task.task_id, TaskGraphTaskState::Cancelled,
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
                {
                    std::vector<detail::SchedulerTask> view;
                    view.reserve(fragment.tasks().size());
                    for (const auto& task : fragment.tasks())
                        view.push_back({task.task_id, &task.depends_on, states[task.task_id]});
                    const auto ready_set = detail::compute_ready(view);
                    for (const auto index : ready_set.cascade_failed) {
                        const auto& task = fragment.tasks()[index];
                        states[task.task_id] = TaskGraphTaskState::Failed;
                        const auto failure =
                            json{{"code", "P_TASK_DEPENDENCY"},
                                 {"message", "Task dependency did not complete successfully"},
                                 {"operation_id", task.operation_id}};
                        co_await update_task(task.task_id, TaskGraphTaskState::Failed,
                                    attempts[task.task_id], std::nullopt, failure,
                                    attempt_identities[task.task_id],
                                    child_run_ids.contains(task.task_id)
                                        ? std::optional<std::string>{child_run_ids[task.task_id]}
                                        : std::nullopt);
                        failures[task.task_id] = failure;
                    }
                    for (const auto index : ready_set.ready)
                        ready.push_back(&fragment.tasks()[index]);
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
                    if (operation_token->is_cancelled()) break;
                    const auto count = std::min<std::size_t>(batch_width, ready.size() - offset);
                    const auto executor            = co_await asio::this_coro::executor;
                    const auto completion_executor = asio::make_strand(executor);
                    auto       batch_state =
                        std::make_shared<ExpansionBatchState>(completion_executor);
                    batch_state->results.resize(count);
                    batch_state->remaining = count;
                    const auto cancel_scope = [scope = batch_state.get(), &completion_executor] {
                        std::vector<std::shared_ptr<RunControl>> children;
                        {
                            std::lock_guard lock(scope->mutex);
                            scope->stop_launching = true;
                            children.swap(scope->children);
                        }
                        if (!children.empty())
                            asio::post(completion_executor, [children = std::move(children)] {
                                for (const auto& child : children)
                                    (void)child->cancel(CancellationCause::ParentTerminal);
                            });
                    };
                    std::optional<std::stop_callback<decltype(cancel_scope)>> scoped_cancellation;
                    if (operation_token != control->cancel_token)
                        scoped_cancellation.emplace(operation_token->stop_token(), cancel_scope);

                    std::size_t index = 0;
                    std::exception_ptr launch_error;
                    try {
                        while (index < count) {
                            if (operation_token->is_cancelled()) break;
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
                                co_await update_task(
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
                                ++index;  // This synchronous producer has already settled.
                                if (complete)
                                    asio::post(completion_executor,
                                               [batch_state] { batch_state->join.signal(); });
                                continue;
                            }
                            const bool retrying = retry_failures.contains(task->task_id);
                            if (task_attempt == 0 || retrying) ++task_attempt;
                            auto& identity = attempt_identities[task->task_id];
                            if (identity.empty() || retrying)
                                identity = task_attempt_identity(fragment.fragment_id(),
                                                                 task->task_id, task_attempt);
                            states[task->task_id] = TaskGraphTaskState::Active;
                            // A retried attempt's record keeps the failure of the attempt before
                            // it, until its own outcome replaces it.
                            co_await update_task(task->task_id, TaskGraphTaskState::Active, task_attempt,
                                        std::nullopt,
                                        retrying ? std::optional<json>{retry_failures[task->task_id]}
                                                 : std::nullopt,
                                        identity,
                                        child_run_ids.contains(task->task_id)
                                            ? std::optional<std::string>{child_run_ids[task->task_id]}
                                            : std::nullopt);
                            if (retrying) {
                                failures[task->task_id] = retry_failures[task->task_id];
                                retry_failures.erase(task->task_id);
                            }
                            asio::co_spawn(
                                executor, run_task(*task, batch_state),
                                asio::bind_executor(
                                    completion_executor,
                                    [batch_state, index](std::exception_ptr error,
                                                         ExpansionTaskRun result) {
                                        bool complete = false;
                                        {
                                            std::lock_guard lock(batch_state->mutex);
                                            if (error) {
                                                result.status  = ProgramTerminalStatus::Failed;
                                                result.failure =
                                                    ProgramFailure{"P_TASK_DISPATCH",
                                                                   "Task coroutine failed",
                                                                   result.task_id,
                                                                   "",
                                                                   0,
                                                                   json::object()};
                                            }
                                            batch_state->results[index] = std::move(result);
                                            complete = --batch_state->remaining == 0;
                                        }
                                        if (complete) batch_state->join.signal();
                                    }));
                            ++index;
                        }
                    } catch (...) {
                        launch_error = std::current_exception();
                        std::vector<std::shared_ptr<RunControl>> children;
                        {
                            std::lock_guard lock(batch_state->mutex);
                            batch_state->stop_launching = true;
                            children.swap(batch_state->children);
                        }
                        for (const auto& child : children)
                            (void)child->cancel(CancellationCause::ParentTerminal);
                    }
                    if (index != count) {
                        bool complete = false;
                        {
                            std::lock_guard lock(batch_state->mutex);
                            batch_state->remaining -= count - index;
                            complete = batch_state->remaining == 0;
                        }
                        if (complete)
                            asio::post(completion_executor,
                                       [batch_state] { batch_state->join.signal(); });
                    }
                    co_await asio::co_spawn(
                        completion_executor,
                        [batch_state]() -> asio::awaitable<void> {
                            co_await batch_state->join.wait();
                        },
                        asio::use_awaitable);
                    if (launch_error) std::rethrow_exception(launch_error);
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
                            failures.erase(task_result.task_id);  // a retried task's last failure
                            co_await update_task(task_result.task_id, TaskGraphTaskState::Completed,
                                        task_result.attempt, task_result.output, std::nullopt,
                                        task_result.attempt_identity, child_id);
                        } else if (task_result.status == ProgramTerminalStatus::Failed &&
                                   retryable_task_failure(task_result.failure) &&
                                   task_result.attempt <
                                       find_task(task_result.task_id).max_attempts &&
                                   !operation_token->is_cancelled() &&
                                   control->cancellation_cause() == CancellationCause::None) {
                            // Terminal states are absorbing, so nothing is written for an attempt
                            // that will be retried. The task is dispatched again, with the next
                            // attempt number, by the next scan; its Active record carries this
                            // failure.
                            states[task_result.task_id]         = TaskGraphTaskState::Pending;
                            retry_failures[task_result.task_id] = failure_json(task_result.failure);
                        } else {
                            const auto cancelled =
                                task_result.status == ProgramTerminalStatus::Cancelled ||
                                task_result.status == ProgramTerminalStatus::TimedOut;
                            states[task_result.task_id] = cancelled ? TaskGraphTaskState::Cancelled
                                                                    : TaskGraphTaskState::Failed;
                            const auto failure          = failure_json(task_result.failure);
                            failures[task_result.task_id] = failure;
                            co_await update_task(task_result.task_id, states[task_result.task_id],
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
                                co_await update_task(
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
                const auto record_lock = co_await durable->lock.lock_async();
                auto       candidate   = durable->record;
                candidate.terminal     = true;
                candidate.published       = true;
                candidate.revision        = durable->record.revision + 1;
                const auto result         = co_await durable->store->compare_update(
                    durable->record.fragment.fragment_id(), durable->record.revision, candidate);
                if (result != TaskGraphPublishResult::Published &&
                    result != TaskGraphPublishResult::AlreadyPresent)
                    throw_runtime_diagnostic(
                        "P_EXPAND_CAS", "Dynamic fragment terminal update lost its revision race");
                if (result == TaskGraphPublishResult::AlreadyPresent) {
                    const auto winner =
                        co_await durable->store->load(durable->record.fragment.fragment_id());
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
        // One operation. `entered` is its record when the paused run this
        // attempt resumes was executing it (#386).
        std::function<asio::awaitable<PlanExecution>(std::string, json, std::string, std::uint32_t,
                                                     std::shared_ptr<graph::CancelToken>,
                                                     const json*)>
            run_operation;
        run_operation = [&](std::string operation_id, json state, std::string thread_id,
                            std::uint32_t                       child_depth,
                            std::shared_ptr<graph::CancelToken> operation_token,
                            const json* entered) -> asio::awaitable<PlanExecution> {
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
            const ProgramOperationEvent coordinate{
                std::string(to_string(op)),
                detail::sha256_identity("program-operation-input/v1",
                                       detail::canonical_json_bytes(state))};
            if (control->recorded_replay) {
                std::lock_guard lock(plan_mutex);
                const auto found_coordinate =
                    control->recorded_replay->operations.find(operation_id);
                if (found_coordinate == control->recorded_replay->operations.end() ||
                    found_coordinate->second.empty() ||
                    found_coordinate->second.front() != coordinate)
                    co_return plan_failure(ProgramTerminalStatus::Failed,
                        "P_REPLAY_COORDINATES", "Operation has no exact captured source coordinate",
                        operation_id);
                found_coordinate->second.pop_front();
            }
            // A re-entered operation was charged, and reported, when it started.
            if (op != ProgramOperationKind::CallCore && !entered) {
                if (!control->recorded_replay)
                    if (auto failure = charge_operation(operation_id)) co_return std::move(*failure);
                control->emit(operation_id, ProgramEventKind::OperationStarted, coordinate);
            }
            if (op == ProgramOperationKind::CallCore) {
                std::optional<std::string> resume;
                bool                       recovered_call = false;
                if (control->recover_plan_without_checkpoint && !control->recorded_replay) {
                    // #404: continue the call's own Core run, answering nothing,
                    // or have the host reconcile a call that saved nothing.
                    auto continued = plan_recovery_checkpoint(thread_id);
                    if (!continued) {
                        PlanExecution held;
                        held.status    = ProgramTerminalStatus::Interrupted;
                        held.interrupt = ProgramInterrupt{
                            "plan", json{{"operation_id", operation_id}}, std::nullopt,
                            plan_operation_pending_effect(*control, operation_id)};
                        co_return held;
                    }
                    resume         = std::move(continued);
                    recovered_call = true;
                    state          = json();
                }
                {
                    std::lock_guard lock(plan_mutex);
                    if (!recovered_call && resume_id &&
                        (!resume_operation || *resume_operation == operation_id)) {
                        resume = std::move(resume_id);
                        resume_id.reset();
                        // #386: the answered call continues its checkpoint, on
                        // that checkpoint's Core thread, with the value the run
                        // was resumed with; the plan keeps its own state.
                        if (resume_checkpoint) thread_id = resume_checkpoint->core_thread_id;
                        if (resume_answer) state = detail::owned_json_copy(*resume_answer);
                    } else if (!control->recorded_replay) {
                        if (operation_count >= control->granted_budget.max_program_operations)
                            co_return plan_failure(ProgramTerminalStatus::BudgetExhausted,
                                                   "P_PROGRAM_OPERATION_BUDGET",
                                                   "Program operation budget exhausted",
                                                   operation_id);
                        ++operation_count;
                    }
                }
                control->emit(operation_id, ProgramEventKind::OperationStarted, coordinate);
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
                                                 checkpoint_incompatible, error.cause());
                } catch (const std::exception& error) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    const bool checkpoint_incompatible =
                        resume_checkpoint_for_error &&
                        !checkpoint_is_available(*control, *resume_checkpoint_for_error);
                    throw NestedOperationFailure(operation_id, error.what(), {}, 0,
                                                 checkpoint_incompatible, std::current_exception());
                } catch (...) {
                    active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                    const bool checkpoint_incompatible =
                        resume_checkpoint_for_error &&
                        !checkpoint_is_available(*control, *resume_checkpoint_for_error);
                    throw NestedOperationFailure(operation_id, "Unknown Core failure", {}, 0,
                                                 checkpoint_incompatible, std::current_exception());
                }
                active_concurrency.fetch_sub(1, std::memory_order_relaxed);

                const auto    core_status = invocation.result.status();
                const auto    stalls      = stalled_barriers_of(invocation.result);
                const auto    exit        = failure_exit_of(invocation.result);
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
                    result.interrupt_checkpoint = invocation.checkpoint;
                } else if (core_status == graph::RunStatus::StepLimit) {
                    result.status  = ProgramTerminalStatus::BudgetExhausted;
                    result.failure = ProgramFailure{"P_CORE_STEP_BUDGET",
                                                    "Core step budget exhausted",
                                                    operation_id,
                                                    invocation.result.interrupt_node,
                                                    0,
                                                    json::object()};
                } else if (core_status == graph::RunStatus::BarrierStalled) {
                    result.status  = ProgramTerminalStatus::Failed;
                    result.failure = barrier_stall_failure(operation_id, stalls);
                } else if (core_status == graph::RunStatus::FailureExit) {
                    result.status  = ProgramTerminalStatus::Failed;
                    result.failure = failure_exit_failure(operation_id, exit);
                }
                co_return result;
            }
            if (op == ProgramOperationKind::Sequence) {
                const auto&   children = dispatch.children;
                PlanExecution result;
                result.output     = std::move(state);
                std::size_t first = 0;
                // #386: a resumed sequence continues at the child it paused in,
                // with that child's input.
                if (entered && entered->contains("next")) {
                    first = static_cast<std::size_t>(
                        reentry_position(*entered, "next", children.size()));
                    if (entered->contains("state"))
                        result.output = detail::owned_json_copy(entered->at("state"));
                }
                for (std::size_t index = first; index < children.size(); ++index) {
                    auto child_result = co_await execute(children[index], result.output, thread_id,
                                                         child_depth, operation_token);
                    std::optional<json> reentry;
                    if (child_result.status == ProgramTerminalStatus::Interrupted) {
                        reentry = json{{"next", index}};
                        if (index > 0) (*reentry)["state"] = result.output;
                    }
                    append_trace(result, std::move(child_result));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned) {
                        result.reentry = std::move(reentry);
                        co_return result;
                    }
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
                result.output       = std::move(state);
                const auto maximum  = *operation.max_iterations();
                // #386: a resumed loop continues the iteration it paused in, with
                // that iteration's input; its condition held then.
                const bool resumed  = entered && entered->contains("iteration");
                std::uint64_t first = 0;
                if (resumed) {
                    first = reentry_position(*entered, "iteration", maximum);
                    if (entered->contains("state"))
                        result.output = detail::owned_json_copy(entered->at("state"));
                }
                for (std::uint64_t iteration = first; iteration < maximum; ++iteration) {
                    if (!(resumed && iteration == first) &&
                        !condition_matches(result.output, operation.condition()))
                        break;
                    auto body = co_await execute(*dispatch.body, result.output, thread_id,
                                                 child_depth, operation_token);
                    std::optional<json> reentry;
                    if (body.status == ProgramTerminalStatus::Interrupted) {
                        reentry = json{{"iteration", iteration}};
                        if (iteration > 0) (*reentry)["state"] = result.output;
                    }
                    append_trace(result, std::move(body));
                    if (result.status != ProgramTerminalStatus::Completed || result.returned) {
                        result.reentry = std::move(reentry);
                        co_return result;
                    }
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
                // #386: a resumed retry continues the attempt it paused in; the
                // attempts before it failed and are not run again.
                const std::uint64_t first = entered && entered->contains("attempt")
                                                ? reentry_position(*entered, "attempt", maximum)
                                                : 0;
                for (std::uint64_t attempt = first; attempt < maximum; ++attempt) {
                    try {
                        auto attempt_result = co_await execute(*dispatch.body, state, thread_id,
                                                               child_depth, operation_token);
                        if (attempt_result.status == ProgramTerminalStatus::Completed)
                            co_return attempt_result;
                        if (attempt_result.status == ProgramTerminalStatus::Interrupted && attempt > 0)
                            attempt_result.reentry = json{{"attempt", attempt}};
                        if (attempt_result.status == ProgramTerminalStatus::Interrupted ||
                            attempt_result.status == ProgramTerminalStatus::Cancelled ||
                            attempt_result.status == ProgramTerminalStatus::TimedOut ||
                            attempt_result.status == ProgramTerminalStatus::BudgetExhausted)
                            co_return attempt_result;
                        if (attempt_result.failure && attempt_result.failure->provider_outcome)
                            co_return attempt_result;
                        last_result = std::move(attempt_result);
                    } catch (const graph::CancelledException&) {
                        throw;
                    } catch (const NestedOperationFailure& error) {
                        // #364: the node failure classifier decides; a failure
                        // it does not retry fails as if there were no Retry.
                        if (error.checkpoint_incompatible ||
                            !program_retryable(std::current_exception(), operation_token.get()))
                            throw;
                        if (attempt + 1 == maximum) {
                            auto terminal = plan_failure(ProgramTerminalStatus::Failed,
                                                         "P_RUNTIME_CORE_FAILURE", error.what(),
                                                         error.operation_id);
                            terminal.failure->core_node = error.core_node;
                            terminal.failure->attempts  = static_cast<std::uint32_t>(attempt + 1);
                            apply_node_failure(*terminal.failure, std::current_exception());
                            co_return terminal;
                        }
                    } catch (const graph::NodeExecutionError& error) {
                        if (!program_retryable(std::current_exception(), operation_token.get()))
                            throw;
                        if (attempt + 1 == maximum) {
                            auto terminal =
                                plan_failure(ProgramTerminalStatus::Failed,
                                             "P_RUNTIME_CORE_FAILURE", error.what(), operation_id);
                            terminal.failure->core_node = error.node_name();
                            terminal.failure->attempts  = static_cast<std::uint32_t>(attempt + 1);
                            apply_node_failure(*terminal.failure, std::current_exception());
                            co_return terminal;
                        }
                    } catch (const std::exception& error) {
                        if (!program_retryable(std::current_exception(), operation_token.get()))
                            throw;
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

                // #384: a branch's interrupt pauses the parallel. The attempt
                // that resumes the run replays what the pause recorded (see
                // execute): a branch that finished returns its result without
                // running or being charged again, a call that paused on another
                // question asks it again, and the branch that holds the
                // answered call continues.
                struct ParallelState {
                    PureMutex                                       mutex;
                    std::vector<std::optional<PlanExecution>>        results;
                    std::vector<std::shared_ptr<graph::CancelToken>> branch_tokens;
                    std::exception_ptr                               error;
                    std::optional<std::size_t>                       error_index;
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
                // wake-up and leave the parent waiting forever. The channel is not
                // thread-safe: it is sent and received only on its strand.
                parallel->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);
                parallel->branch_tokens.reserve(branches.size());
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    parallel->branch_tokens.push_back(operation_token->fork());
                }
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    const auto& branch_id = branches[index];
                    // #386: branch 0 continues on the parallel's own Core
                    // thread, every other branch on its own, so no two
                    // concurrent branches, at any depth, share one.
                    const auto branch_thread =
                        index == 0 ? thread_id : operation_thread_id(*control, branch_id);
                    auto branch_token = parallel->branch_tokens[index];
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
                                        // The first error stops the join; a
                                        // sibling's cancellation never
                                        // replaces it (#379).
                                        if (!parallel->error) {
                                            parallel->error       = error;
                                            parallel->error_index = index;
                                        }
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
                // Received on the strand the branches send on, not on this
                // coroutine's scheduler thread: a receive started there, while
                // the last branch sends, could miss the signal for good.
                co_await asio::co_spawn(
                    completion_executor,
                    [parallel]() -> asio::awaitable<void> {
                        const auto [error, ignored] = co_await parallel->completion->async_receive(
                            asio::as_tuple(asio::use_awaitable));
                        (void)error;
                        (void)ignored;
                    },
                    asio::use_awaitable);

                std::exception_ptr                        parallel_error;
                std::optional<std::size_t>                error_index;
                std::vector<std::optional<PlanExecution>> parallel_results;
                {
                    std::lock_guard lock(parallel->mutex);
                    parallel_error   = parallel->error;
                    error_index      = parallel->error_index;
                    parallel_results = std::move(parallel->results);
                }
                // #397: a branch whose Core node fails throws, which stops the
                // parallel at once and cancels the other branches. When a
                // branch had already paused on an effect whose outcome is
                // unknown, the parallel pauses on it instead: the failure is
                // recorded, and ends the run once the parallel is resumed, and
                // a branch the failure cancelled runs again.
                std::vector<bool> rerun(parallel_results.size(), false);
                if (parallel_error) {
                    const auto reconciling_branch = std::find_if(
                        parallel_results.begin(), parallel_results.end(),
                        [](const std::optional<PlanExecution>& branch) {
                            return branch && reconciliation_pause(*branch);
                        });
                    std::optional<PlanExecution> failed_branch;
                    if (reconciling_branch != parallel_results.end() && error_index &&
                        !operation_token->is_cancelled())
                        failed_branch = failed_branch_of(parallel_error);
                    if (!failed_branch) std::rethrow_exception(parallel_error);
                    parallel_results[*error_index] = std::move(*failed_branch);
                    for (std::size_t index = 0; index < parallel_results.size(); ++index) {
                        if (parallel_results[index]) continue;
                        rerun[index] = true;
                        PlanExecution cancelled_branch;
                        cancelled_branch.status = ProgramTerminalStatus::Cancelled;
                        parallel_results[index] = std::move(cancelled_branch);
                    }
                }

                // The parallel ends by the rule of a Core step (#392, #397),
                // whatever the branches' order: a cancelled branch ends it;
                // otherwise a branch paused on an effect whose outcome is
                // unknown pauses it, even beside a failed branch, on the first
                // such branch; otherwise a branch that fails ends it, paused
                // branches or not; otherwise a paused branch pauses it, on the
                // first paused branch in declaration order, and records the
                // others (#384). Whenever it pauses, a branch paused on an
                // uncertain effect is asked before an ordinary question (#401).
                std::optional<std::size_t> cancelled;
                std::optional<std::size_t> failed;
                std::optional<std::size_t> paused;
                std::optional<std::size_t> reconciling;
                for (std::size_t index = 0; index < parallel_results.size(); ++index) {
                    const auto& branch = parallel_results[index];
                    if (!branch)
                        throw_runtime_diagnostic("P_RUNTIME_PARALLEL",
                                                 "Parallel branch completed without a result");
                    if (branch->status == ProgramTerminalStatus::Interrupted) {
                        if (!paused) paused = index;
                        if (!reconciling && reconciliation_pause(*branch)) reconciling = index;
                    } else if (branch->status == ProgramTerminalStatus::Cancelled) {
                        if (!cancelled && !rerun[index]) cancelled = index;
                    } else if (branch->status != ProgramTerminalStatus::Completed && !failed) {
                        failed = index;
                    }
                }
                if (cancelled) failed = cancelled;
                const bool pause = !cancelled && paused && (!failed || reconciling);
                if (pause && reconciling) paused = reconciling;
                PlanExecution result;
                result.output = json::array();
                for (std::size_t index = 0; index < parallel_results.size(); ++index) {
                    auto& branch = *parallel_results[index];
                    // A paused parallel lists the results so far.
                    result.output.push_back(
                        pause && branch.status != ProgramTerminalStatus::Completed ? json(nullptr)
                                                                                  : branch.output);
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(branch.execution_trace.begin()),
                        std::make_move_iterator(branch.execution_trace.end()));
                    if (branch.returned) result.returned = true;
                    if (!pause) continue;
                    if (branch.status != ProgramTerminalStatus::Interrupted) {
                        // #398: with its provider evidence, as a quorum records it.
                        if (!rerun[index])
                            if (auto record = settled_branch_record_of(branch, *control))
                                result.settled_members[branches[index]] = std::move(*record);
                        continue;
                    }
                    for (const auto& [key, record] : branch.settled_members.items())
                        result.settled_members[key] = record;
                    if (index == *paused) continue;
                    // The call it paused on asks again on resume (#386: by the
                    // call's operation id, so that the branch around it is
                    // re-entered). Without a record the call runs again.
                    if (auto record = paused_branch_record(branch)) {
                        const auto call = record->at("operation_id").get<std::string>();
                        result.settled_members[call] = std::move(*record);
                    }
                }
                if (const auto decided = pause ? paused : failed) {
                    auto& branch                = *parallel_results[*decided];
                    result.status               = branch.status;
                    result.interrupt            = std::move(branch.interrupt);
                    result.failure              = std::move(branch.failure);
                    result.interrupt_checkpoint = std::move(branch.interrupt_checkpoint);
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
                // #386: a race that paused on its winner's question continues
                // that branch alone; the loser was cancelled.
                if (entered && entered->contains("winner")) {
                    const auto winner = static_cast<std::size_t>(
                        reentry_position(*entered, "winner", branches.size()));
                    auto result = co_await execute(
                        branches[winner], std::move(state),
                        winner == 0 ? thread_id : operation_thread_id(*control, branches[winner]),
                        child_depth, operation_token);
                    if (result.status == ProgramTerminalStatus::Interrupted)
                        result.reentry = json{{"winner", winner}};
                    co_return result;
                }
                // A race winner is the first completion; if both branches become
                // ready in one scheduler turn, declaration order is the tie break.
                // Wait for the losing coroutine to observe cancellation before this
                // operation returns, so no detached branch outlives the run handle.
                struct RaceState {
                    PureMutex                                       mutex;
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
                // settle before this parent reaches async_receive. The channel is
                // not thread-safe: it is sent and received only on its strand.
                race->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);
                race->tokens.push_back(operation_token->fork());
                race->tokens.push_back(operation_token->fork());
                const std::array<std::string, 2> ids{first, second};
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    // #386: branch 0 continues on the race's own Core thread.
                    const auto branch_thread =
                        index == 0 ? thread_id : operation_thread_id(*control, ids[index]);
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
                // Received on the strand the branches send on (see parallel).
                co_await asio::co_spawn(
                    completion_executor,
                    [race]() -> asio::awaitable<void> {
                        const auto [error, ignored] = co_await race->completion->async_receive(
                            asio::as_tuple(asio::use_awaitable));
                        (void)error;
                        (void)ignored;
                    },
                    asio::use_awaitable);

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
                if (results[index]->status == ProgramTerminalStatus::Interrupted)
                    results[index]->reentry = json{{"winner", index}};
                co_return std::move(*results[index]);
            }
            if (op == ProgramOperationKind::Quorum) {
                const auto&   branches = dispatch.branches;
                const auto    required = *operation.min_success();
                PlanExecution result;
                result.output           = json::array();
                std::uint64_t successes = 0;
                const auto    unreachable = [&]() {
                    auto failed = plan_failure(ProgramTerminalStatus::Failed,
                                               "P_QUORUM_UNREACHABLE",
                                               "Program quorum could not reach its required successes",
                                               operation_id);
                    failed.execution_trace = std::move(result.execution_trace);
                    return failed;
                };
                // #387: the quorum follows the join/interrupt contract of #380
                // and #384. Its branches run in declaration order. A branch
                // that pauses counts as a success it may still reach, so it
                // pauses the quorum unless the branches that finished already
                // decide it, and no further branch starts. The pause records
                // each branch that finished by its own end ("settled", #386);
                // the attempt that resumes the run replays them without
                // running or charging them again, continues the paused
                // branch's call with the resume value, and goes on from there.
                std::vector<std::pair<std::string, PlanExecution>> finished;
                for (std::size_t index = 0; index < branches.size(); ++index) {
                    // #399: a branch whose Core node fails is a failed branch,
                    // not the run's failure; the quorum decides by its rule.
                    PlanExecution      branch_result;
                    std::exception_ptr branch_error;
                    try {
                        branch_result = co_await execute(
                            branches[index], state, operation_thread_id(*control, branches[index]),
                            child_depth, operation_token);
                    } catch (const NestedOperationFailure&) {
                        branch_error = std::current_exception();
                    }
                    if (branch_error) {
                        auto failed = failed_branch_of(branch_error);
                        if (!failed || operation_token->is_cancelled())
                            std::rethrow_exception(branch_error);
                        branch_result = std::move(*failed);
                    }
                    const auto remaining = branches.size() - index - 1;
                    result.execution_trace.insert(result.execution_trace.end(),
                                                  branch_result.execution_trace.begin(),
                                                  branch_result.execution_trace.end());
                    if (branch_result.status == ProgramTerminalStatus::Interrupted) {
                        // Already met or out of reach, the quorum would have
                        // ended before this branch; it pauses on it.
                        // #397: an effect whose outcome is unknown pauses
                        // the quorum even when it is out of reach.
                        if (successes + 1 + remaining < required &&
                            !reconciliation_pause(branch_result))
                            co_return unreachable();
                        PlanExecution paused;
                        paused.status               = ProgramTerminalStatus::Interrupted;
                        paused.output               = std::move(result.output);
                        paused.execution_trace      = std::move(result.execution_trace);
                        paused.interrupt            = std::move(branch_result.interrupt);
                        paused.interrupt_checkpoint = std::move(branch_result.interrupt_checkpoint);
                        for (const auto& [key, record] : branch_result.settled_members.items())
                            paused.settled_members[key] = record;
                        for (auto& [branch_id, settled] : finished) {
                            // Without its evidence the branch runs again on resume.
                            if (auto record = settled_branch_record_of(settled, *control))
                                paused.settled_members[branch_id] = std::move(*record);
                        }
                        co_return paused;
                    }
                    // Only a branch's own end is kept; one cancelled with the
                    // run, for example, starts again on resume.
                    if (branch_result.status == ProgramTerminalStatus::Completed ||
                        branch_result.status == ProgramTerminalStatus::Failed ||
                        branch_result.status == ProgramTerminalStatus::BudgetExhausted ||
                        branch_result.status == ProgramTerminalStatus::TimedOut)
                        finished.emplace_back(branches[index], branch_result);
                    if (branch_result.status == ProgramTerminalStatus::Completed) {
                        result.output.push_back(std::move(branch_result.output));
                        ++successes;
                    }
                    if (successes >= required) co_return result;
                    // Out of reach: it fails, as a JavaScript quorum does, instead
                    // of completing with fewer successes than it requires.
                    if (successes + remaining < required) co_return unreachable();
                }
                co_return unreachable();
            }

            if (op == ProgramOperationKind::Map) {
                const auto    items = operation.items();
                PlanExecution result;
                result.output     = json::array();
                std::size_t first = 0;
                // #386: a resumed map continues the item it paused in, after the
                // results of the items before it.
                if (entered && entered->contains("item")) {
                    first = static_cast<std::size_t>(
                        reentry_position(*entered, "item", items.size()));
                    if (entered->contains("outputs"))
                        result.output = detail::owned_json_copy(entered->at("outputs"));
                    result.returned = entered->value("returned", false);
                }
                for (std::size_t index = first; index < items.size(); ++index) {
                    auto mapped = co_await execute(*dispatch.body,
                                                   detail::owned_json_copy(items[index]),
                                                   operation_thread_id(*control, operation_id),
                                                   child_depth, operation_token);
                    result.execution_trace.insert(
                        result.execution_trace.end(),
                        std::make_move_iterator(mapped.execution_trace.begin()),
                        std::make_move_iterator(mapped.execution_trace.end()));
                    result.returned = result.returned || mapped.returned;
                    if (mapped.status != ProgramTerminalStatus::Completed) {
                        result.status               = mapped.status;
                        result.interrupt            = std::move(mapped.interrupt);
                        result.failure              = std::move(mapped.failure);
                        result.interrupt_checkpoint = std::move(mapped.interrupt_checkpoint);
                        result.settled_members      = std::move(mapped.settled_members);
                        if (result.status == ProgramTerminalStatus::Interrupted)
                            result.reentry = json{{"item", index},
                                                  {"outputs", result.output},
                                                  {"returned", result.returned}};
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
                    explicit ParallelMapState(const asio::any_io_executor& completion_executor)
                        : join(completion_executor) {}
                    PureMutex                                mutex;
                    std::vector<std::optional<PlanExecution>> results;
                    std::vector<std::shared_ptr<RunControl>>  children;
                    std::exception_ptr                        error;
                    std::optional<std::size_t>                first_fail_fast_failure;
                    std::size_t                               next_item         = 0;
                    std::size_t                               workers_remaining = 0;
                    bool                                      stop_launching    = false;
                    StrandJoinSignal                          join;
                };

                const auto executor = co_await asio::this_coro::executor;
                const auto                     completion_executor = asio::make_strand(executor);
                const auto worker_count = std::min<std::size_t>(map.max_in_flight, items.size());
                auto parallel_map = std::make_shared<ParallelMapState>(completion_executor);
                parallel_map->results.resize(items.size());
                parallel_map->workers_remaining = worker_count;

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

                // Shield the complete borrowing lifetime, not just the eventual timer wait.
                // The exact operation token below still stops dispatch and cancels children.
                co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

                const auto cancel_scope = [scope = parallel_map.get(), &completion_executor] {
                    std::vector<std::shared_ptr<RunControl>> children;
                    {
                        std::lock_guard lock(scope->mutex);
                        scope->stop_launching = true;
                        children.swap(scope->children);
                    }
                    if (!children.empty())
                        asio::post(completion_executor, [children = std::move(children)] {
                            for (const auto& child : children)
                                (void)child->cancel(CancellationCause::ParentTerminal);
                        });
                };
                std::optional<std::stop_callback<decltype(cancel_scope)>> scoped_cancellation;
                // Root cancellation already cascades through RunControl::attach_child.
                if (operation_token != control->cancel_token)
                    scoped_cancellation.emplace(operation_token->stop_token(), cancel_scope);

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
                                            auto child = (co_await control->launch_child(
                                                map.child_binding, std::move(child_input),
                                                operation_id,
                                                execution_key_prefix + ":" +
                                                    std::to_string(item_index),
                                                std::nullopt));
                                            bool cancel_child = false;
                                            {
                                                std::lock_guard lock(parallel_map->mutex);
                                                cancel_child = parallel_map->stop_launching ||
                                                               operation_token->is_cancelled();
                                                if (!cancel_child)
                                                    parallel_map->children.push_back(child);
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

                std::size_t workers_started = 0;
                try {
                    for (; workers_started < worker_count && !operation_token->is_cancelled();
                         ++workers_started) {
                        asio::co_spawn(
                            executor, worker(),
                            asio::bind_executor(
                                completion_executor,
                                [parallel_map, cancel_children](std::exception_ptr error) {
                                    std::vector<std::shared_ptr<RunControl>> children_to_cancel;
                                    bool all_done = false;
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
                                    if (all_done) parallel_map->join.signal();
                                }));
                    }
                } catch (...) {
                    std::vector<std::shared_ptr<RunControl>> children_to_cancel;
                    {
                        std::lock_guard lock(parallel_map->mutex);
                        if (!parallel_map->error) parallel_map->error = std::current_exception();
                        parallel_map->stop_launching = true;
                        children_to_cancel.swap(parallel_map->children);
                    }
                    cancel_children(children_to_cancel);
                }
                if (workers_started != worker_count) {
                    bool all_done = false;
                    {
                        std::lock_guard lock(parallel_map->mutex);
                        parallel_map->stop_launching = true;
                        parallel_map->workers_remaining -= worker_count - workers_started;
                        all_done = parallel_map->workers_remaining == 0;
                    }
                    if (all_done)
                        asio::post(completion_executor,
                                   [parallel_map] { parallel_map->join.signal(); });
                }

                co_await asio::co_spawn(
                    completion_executor,
                    [parallel_map]() -> asio::awaitable<void> {
                        co_await parallel_map->join.wait();
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
                auto result =
                    co_await execute_expand(operation, std::move(state), operation_token);
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
                auto child = (co_await control->launch_child(*operation.child_binding(), std::move(state),
                                                   operation_id, execution_key, std::nullopt));
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

                    const auto executor       = co_await asio::this_coro::executor;
                    auto       timeout        = std::make_shared<async::HostTimer>(executor);
                    const auto await_deadline =
                        deadline_after(std::chrono::steady_clock::now(), *operation.timeout_ms());
                    if (await_deadline < std::chrono::steady_clock::time_point::max()) {
                        timeout->arm_at(await_deadline);
                    } else {
                        timeout->arm_never();
                    }
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
                // #391: the deadline is absolute. An await the paused run was
                // executing keeps the start its record holds, and has only the
                // time that remains; none left is a timeout, before anything
                // is dispatched.
                const auto now_unix = unix_time_ms();
                const auto started_at = entered && entered->contains("started_at_unix_ms")
                                            ? recorded_start_ms(entered->at("started_at_unix_ms"))
                                            : now_unix;
                const auto remaining =
                    await_remaining_ms(started_at, *operation.timeout_ms(), now_unix);
                if (remaining == 0)
                    co_return plan_failure(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                           "Program await operation timed out", operation_id);
                const auto executor       = co_await asio::this_coro::executor;
                auto       timeout        = std::make_shared<async::HostTimer>(executor);
                const auto await_deadline = deadline_after(std::chrono::steady_clock::now(), remaining);
                if (await_deadline < std::chrono::steady_clock::time_point::max()) {
                    timeout->arm_at(await_deadline);
                } else {
                    timeout->arm_never();
                }
                auto child_token       = operation_token->fork();
                auto timeout_expired   = std::make_shared<std::atomic<bool>>(false);
                auto timeout_operation = cancel_after_timer(timeout, child_token, timeout_expired);
                // Await races the child completion (including a child error)
                // against the timeout.  The awaitable `||` operator waits for
                // one *successful* operation, which would mask an immediate
                // Core failure until the timeout and misclassify it as a
                // timeout.  The explicit parallel group preserves the first
                // completion and lets the outer attempt classify the child
                // exception normally.
                // Register the deadline before starting child work. co_spawn can
                // enter a synchronous host publication before its initiation returns;
                // that publication may need cancellation to release its wait.
                auto [order, timeout_error, child_error, child_result] =
                    co_await asio::experimental::make_parallel_group(
                        asio::co_spawn(executor, std::move(timeout_operation), asio::deferred),
                        asio::co_spawn(executor,
                                       execute(child_id, std::move(state), std::move(thread_id),
                                               child_depth, child_token),
                                       asio::deferred))
                        .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
                if (order[0] == 1 && !timeout_expired->load(std::memory_order_acquire)) {
                    if (child_error) std::rethrow_exception(child_error);
                    // A paused await records when it started (#391).
                    if (child_result.status == ProgramTerminalStatus::Interrupted)
                        child_result.reentry = json{{"started_at_unix_ms", started_at}};
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
        // #386: an operation the paused run finished, or a call it left asking
        // another question, replays its record; one it was executing is
        // re-entered from its record; every other one runs. An operation that
        // pauses records how far it got, and the root the plan's input.
        execute = [&](std::string operation_id, json state, std::string thread_id,
                      std::uint32_t                       child_depth,
                      std::shared_ptr<graph::CancelToken> operation_token)
            -> asio::awaitable<PlanExecution> {
            const auto record  = take_replay_record(operation_id);
            const bool entered = record && record->is_object() && record->contains("kind") &&
                                 record->at("kind") == json("entered");
            if (record && !entered) co_return replay_branch_record(*record, *control);
            if (entered && record->contains("input"))
                state = detail::owned_json_copy(record->at("input"));
            const auto* found = typed_plan.find(operation_id);
            const bool  call  = found && found->dispatch().operation == ProgramOperationKind::CallCore;
            std::optional<json> root_input;
            if (!call && operation_id == root_id) root_input = state;
            const auto key    = operation_id;
            auto       result = co_await run_operation(std::move(operation_id), std::move(state),
                                                       std::move(thread_id), child_depth,
                                                       std::move(operation_token),
                                                       entered ? &*record : nullptr);
            if (result.status == ProgramTerminalStatus::Interrupted && !call) {
                json position{{"kind", "entered"}};
                if (result.reentry)
                    for (const auto& [field, value] : result.reentry->items()) position[field] = value;
                if (root_input) position["input"] = std::move(*root_input);
                result.settled_members[key] = std::move(position);
            }
            result.reentry.reset();
            co_return result;
        };

        // Core nodes this attempt ran, across every JavaScript command. Each
        // result reports exactly these, whichever command ends the attempt; a
        // command completed by an earlier attempt was reported then (#374).
        std::vector<std::string> javascript_trace;
        auto execute_javascript_control =
            [&](JavaScriptGenerator& session) -> asio::awaitable<PlanExecution> {
            const auto executor = co_await                     asio::this_coro::executor;
            std::optional<std::pair<std::string, std::string>> exact_core_resume;
            // The settled members of the paused join this attempt resumes (#380).
            json resumed_join_members = json::object();
            // #391: when each await of the current command started, in Unix
            // milliseconds by operation id: recorded in its command journal by
            // an earlier attempt, or by this one when the await starts.
            PureMutex     await_starts_mutex;
            OperationLock await_start_publication;
            json          await_starts = json::object();
            bool supplied_core_resume_required = false;
            bool supplied_core_resume_consumed = false;
            // #402: the command a reconnect recovers continues each of its calls
            // from that call's own latest Core checkpoint. A call it reaches
            // without one may have started before the process stopped: the
            // command is returned for reconciliation instead.
            std::map<std::string, std::string> recovery_core_resumes;
            std::set<std::string>              recovery_unstarted_calls;
            bool                               recovery_active               = false;
            bool                               recovery_reached_unsaved_call = false;
            std::optional<ProgramInterrupt>    recovery_reconciliation;

            using JsCommandExecutor = std::function<asio::awaitable<PlanExecution>(
                const JavaScriptCommand&, std::string, std::shared_ptr<graph::CancelToken>,
                std::shared_ptr<JavaScriptScopeState>, std::size_t, bool, bool)>;
            JsCommandExecutor execute_command;

            execute_command = [&](const JavaScriptCommand& command, std::string operation_id,
                                  std::shared_ptr<graph::CancelToken>   operation_token,
                                  std::shared_ptr<JavaScriptScopeState> scope,
                                  std::size_t                           scope_depth,
                                  bool reserve_budget,
                                  bool reconnect_child_only) -> asio::awaitable<PlanExecution> {
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
                            (co_await validate_javascript_command(command, operation_id, scope_depth)))
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
                    async::HostTimer completion_wait(executor);

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
                        completion_wait.arm_after(std::min(
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
                    std::optional<std::string> recovered_resume;
                    if (recovery_active &&
                        !(exact_core_resume && exact_core_resume->first == operation_id)) {
                        bool unsaved = false;
                        {
                            std::lock_guard lock(plan_mutex);
                            const auto found = recovery_core_resumes.find(operation_id);
                            if (found != recovery_core_resumes.end()) {
                                recovered_resume = found->second;
                                recovery_core_resumes.erase(found);
                            } else if (recovery_unstarted_calls.count(operation_id)) {
                                // It never started: it runs as new work.
                            } else {
                                recovery_reached_unsaved_call = true;
                                unsaved                       = true;
                            }
                        }
                        if (unsaved) {
                            PlanExecution held;
                            held.status    = ProgramTerminalStatus::Interrupted;
                            held.interrupt = recovery_reconciliation;
                            co_return held;
                        }
                    }
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
                    if (recovered_resume) {
                        // #402: continue the call where it stopped, answering nothing.
                        command_resume = std::move(recovered_resume);
                        core_input     = json();
                    } else if (command_resume && javascript_core_resume_value) {
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
                        retain_provider_failure(*result.failure, error.cause());
                        apply_node_failure(*result.failure, std::current_exception());
                        co_return result;
                    } catch (const std::exception& error) {
                        active_concurrency.fetch_sub(1, std::memory_order_relaxed);
                        auto result = fail(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                           error.what());
                        retain_provider_failure(*result.failure, std::current_exception());
                        co_return result;
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

                    // Classify before the output (which carries the status) is moved.
                    const auto    core_status = invocation.result.status();
                    const auto    stalls      = stalled_barriers_of(invocation.result);
                    const auto    exit        = failure_exit_of(invocation.result);
                    PlanExecution result;
                    result.output =
                        javascript_control_response(std::move(invocation.result.output));
                    result.execution_trace = std::move(invocation.result.execution_trace);
                    if (core_status == graph::RunStatus::Interrupted) {
                        result.status = ProgramTerminalStatus::Interrupted;
                        result.interrupt =
                            decode_core_interrupt(*control, operation_id, invocation.result);
                        result.interrupt_checkpoint = invocation.checkpoint;
                    } else if (core_status == graph::RunStatus::StepLimit) {
                        result = fail(ProgramTerminalStatus::BudgetExhausted, "P_CORE_STEP_BUDGET",
                                      "Core step budget exhausted");
                    } else if (core_status == graph::RunStatus::BarrierStalled) {
                        result.status  = ProgramTerminalStatus::Failed;
                        result.failure = barrier_stall_failure(operation_id, stalls);
                    } else if (core_status == graph::RunStatus::FailureExit) {
                        result.status  = ProgramTerminalStatus::Failed;
                        result.failure = failure_exit_failure(operation_id, exit);
                    }
                    co_return result;
                }

                if (command.kind() == JavaScriptCommandKind::Spawn) {
                    const auto binding = arguments.at("child_binding").get<std::string>();
                    auto child = reconnect_child_only
                                     ? (co_await control->reconnect_existing_child(
                                           binding, arguments.at("input"), operation_id))
                                     : (co_await control->launch_child(binding, arguments.at("input"),
                                                             operation_id, operation_id));
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
                                                                 scope_depth + 1, false,
                                                                 reconnect_child_only);
                        if (launched.status != ProgramTerminalStatus::Completed) co_return launched;
                        if (!launched.spawned_child) co_return std::move(launched);
                        co_return plan_result_from_child(
                            co_await launched.spawned_child->wait_async());
                    };
                    if (!timeout) co_return co_await run_awaited(operation_token);

                    // #391: the deadline is absolute. The await's start is
                    // recorded in the command journal when it first starts,
                    // and an attempt that resumes or recovers the command has
                    // only the time that remains; none left is a timeout,
                    // before anything is dispatched.
                    const auto now_unix = unix_time_ms();
                    std::uint64_t started_at = now_unix;
                    {
                        // The publication reads the command's head and appends to it, so two awaits
                        // that start together record their starts one after the other. The mutex
                        // guards the map only; the store is never reached while it is held.
                        const auto publication = co_await await_start_publication.lock_async();
                        bool       recorded    = false;
                        {
                            std::lock_guard lock(await_starts_mutex);
                            if (await_starts.contains(operation_id)) {
                                started_at = recorded_start_ms(await_starts[operation_id]);
                                recorded   = true;
                            }
                        }
                        if (!recorded && !control->recorded_replay && active_javascript_command) {
                            const auto& active    = *active_javascript_command;
                            auto publish_call_1 = control->publish_javascript_command(
                                active.ordinal, active.command, active.effect_identity,
                                std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                json{{operation_id, started_at}});
                            const auto  published = co_await std::move(publish_call_1);
                            if (published != ProgramTransitionPublishResult::Published &&
                                published != ProgramTransitionPublishResult::AlreadyPresent)
                                co_return fail(ProgramTerminalStatus::Failed, "P_JS_COMMAND_JOURNAL",
                                               "Could not durably record the start of a "
                                               "JavaScript await");
                            std::lock_guard lock(await_starts_mutex);
                            await_starts[operation_id] = started_at;
                        }
                    }
                    const auto remaining = await_remaining_ms(started_at, *timeout, now_unix);
                    if (remaining == 0)
                        co_return fail(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                       "JavaScript await operation timed out");

                    struct AwaitState {
                        PureMutex                          mutex;
                        std::optional<PlanExecution>        result;
                        std::exception_ptr                  error;
                        bool                                finished  = false;
                        bool                                timed_out = false;
                        std::chrono::steady_clock::time_point deadline;
                        std::shared_ptr<async::HostTimer> timer;
                        std::shared_ptr<asio::experimental::channel<void(asio::error_code, int)>>
                            completion;
                    };
                    auto       await_state         = std::make_shared<AwaitState>();
                    const auto completion_executor = asio::make_strand(executor);
                    await_state->timer =
                        std::make_shared<async::HostTimer>(completion_executor);
                    await_state->deadline =
                        deadline_after(std::chrono::steady_clock::now(), remaining);
                    if (await_state->deadline < std::chrono::steady_clock::time_point::max()) {
                        await_state->timer->arm_at(await_state->deadline);
                    } else {
                        await_state->timer->arm_never();
                    }
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
                                        // Completing after the deadline is a timeout,
                                        // whether or not the timer has fired yet.
                                        if (std::chrono::steady_clock::now() >=
                                            await_state->deadline)
                                            await_state->timed_out = true;
                                        await_state->error = error;
                                        if (!error) await_state->result = std::move(result);
                                        signal = true;
                                    }
                                }
                                await_state->timer->cancel();
                                if (signal)
                                    await_state->completion->try_send(asio::error_code(), 1);
                            }));
                    asio::co_spawn(
                        completion_executor,
                        [await_state, completion_executor, scope,
                         awaited_token]() -> asio::awaitable<void> {
                            {
                                std::lock_guard lock(await_state->mutex);
                                if (await_state->finished) co_return;
                            }
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

                    co_await asio::co_spawn(
                        completion_executor,
                        [await_state]() -> asio::awaitable<void> {
                            const auto [error, ignored] =
                                co_await await_state->completion->async_receive(
                                    asio::as_tuple(asio::use_awaitable));
                            (void)error;
                            (void)ignored;
                        },
                        asio::use_awaitable);
                    std::optional<PlanExecution> awaited_result;
                    std::exception_ptr           awaited_error;
                    bool                         timed_out = false;
                    {
                        std::lock_guard lock(await_state->mutex);
                        awaited_result = std::move(await_state->result);
                        awaited_error  = await_state->error;
                        timed_out      = await_state->timed_out;
                    }
                    // A child cancelled by the timeout may fail with an error; the
                    // timeout is still the outcome.
                    if (timed_out)
                        co_return fail(ProgramTerminalStatus::TimedOut, "P_AWAIT_TIMEOUT",
                                       "JavaScript await operation timed out");
                    if (awaited_error) std::rethrow_exception(awaited_error);
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

                // A member that interrupts pauses the join (#380). The join
                // launches no further member, lets the members already running
                // finish and keeps every result they reach. If those results
                // end the join by its own rule (a fail-fast failure, a race
                // winner, a quorum met or out of reach), it ends that way;
                // otherwise it is Interrupted on its first paused member in
                // declaration order, and the members that settled are recorded
                // in the command journal. The attempt that resumes the join
                // replays them from that record, continues the paused member
                // with the resume value and runs every other member again.
                std::vector<std::optional<PlanExecution>> replayed(members.size());
                std::vector<std::optional<json>>          replay_records(members.size());
                for (std::size_t index = 0; index < members.size(); ++index) {
                    const auto member_operation = operation_id + "/member/" + std::to_string(index);
                    const auto record           = resumed_join_members.find(member_operation);
                    if (record == resumed_join_members.end()) continue;
                    auto recorded = decode_javascript_command_result(record.value(), *control);
                    PlanExecution settled;
                    settled.status  = recorded.status;
                    settled.output  = std::move(recorded.output);
                    settled.failure = std::move(recorded.failure);
                    // Its nodes ran, and were reported, in an earlier attempt (#374).
                    replayed[index]       = std::move(settled);
                    replay_records[index] = record.value();
                }

                struct JoinState {
                    PureMutex                                         mutex;
                    std::vector<std::optional<PlanExecution>>          results;
                    std::vector<std::exception_ptr>                    errors;
                    std::vector<bool>                                  started;
                    std::vector<bool>                                  finished;
                    std::vector<std::shared_ptr<graph::CancelToken>>   tokens;
                    std::vector<std::shared_ptr<JavaScriptScopeState>> scopes;
                    // #400: the members in launch order (the members this attempt
                    // replays, then the member it answers, then the others, each
                    // in declaration order), and the position of the next one.
                    std::vector<std::size_t>                           order;
                    std::size_t                                        next                = 0;
                    std::size_t                                        active              = 0;
                    std::size_t                                        completed           = 0;
                    bool                                               terminal            = false;
                    bool                                               paused              = false;
                    bool                                               selection_scheduled = false;
                    bool                       initial_launches_registered                 = false;
                    std::optional<std::size_t> winner;
                    std::vector<std::size_t>   finish_order;
                    std::vector<std::size_t>   quorum_members;  // successes that met it
                    // #400: the member this attempt answers, and the members it
                    // replays. A decision is held while the answered member runs.
                    std::vector<bool>          replayed;
                    std::optional<std::size_t> answered;
                    bool                       held = false;
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
                state->replayed.resize(members.size(), false);
                for (std::size_t index = 0; index < members.size(); ++index) {
                    state->replayed[index] = replayed[index].has_value();
                    if (!exact_core_resume) continue;
                    const auto member_operation =
                        operation_id + "/member/" + std::to_string(index);
                    if (exact_core_resume->first == member_operation ||
                        exact_core_resume->first.starts_with(member_operation + "/"))
                        state->answered = index;
                }
                // #400: a member this attempt replays finished before the pause,
                // so it settles before any member that runs now. The replayed
                // members launch first, then the member the resume answers,
                // then the others. The replays reach the completion strand
                // first and complete without leaving it, so the order in which
                // members settle (a race's winner, a fail-fast join's failure)
                // does not depend on how fast the members that run are. The
                // first launches are every replay, which runs nothing, and up
                // to max_in_flight others, the answered member among them.
                std::size_t replay_count = 0;
                for (std::size_t index = 0; index < members.size(); ++index)
                    if (state->replayed[index]) state->order.push_back(index), ++replay_count;
                if (state->answered && !state->replayed[*state->answered])
                    state->order.push_back(*state->answered);
                for (std::size_t index = 0; index < members.size(); ++index)
                    if (!state->replayed[index] && index != state->answered)
                        state->order.push_back(index);
                const auto first_launches =
                    replay_count + std::min<std::size_t>(cap, members.size() - replay_count);
                const auto completion_executor = asio::make_strand(executor);
                state->completion =
                    std::make_shared<asio::experimental::channel<void(asio::error_code, int)>>(
                        completion_executor, 1);
                // An interrupted member is paused: it has not settled.
                const auto interrupted = [](const std::exception_ptr&           error,
                                            const std::optional<PlanExecution>& result) {
                    return !error && result && result->status == ProgramTerminalStatus::Interrupted;
                };
                // The join has nothing left to wait for once it is decided, or
                // every member finished, or it is paused and none is running.
                const auto settled = [](const JoinState& join, std::size_t member_count) {
                    if (!join.initial_launches_registered || join.active != 0) return false;
                    if (join.terminal) return true;
                    return !join.selection_scheduled &&
                           (join.completed == member_count || join.paused);
                };

                // A member can complete before the initial launch loop finishes. Completion
                // handlers therefore retain this dispatcher rather than a reference to a
                // stack-local std::function.
                auto launch_member = std::make_shared<std::function<void(std::size_t)>>();
                *launch_member     = [&](std::size_t index) {
                    if (const auto observer =
                            join_member_launch_observer.load(std::memory_order_acquire))
                        observer(index, replayed[index].has_value());
                    {
                        std::lock_guard lock(state->mutex);
                        state->started[index] = true;
                        ++state->active;
                        state->tokens[index] = operation_token->fork();
                        state->scopes[index] = std::make_shared<JavaScriptScopeState>();
                    }
                    asio::co_spawn(
                        completion_executor,
                        replayed[index]
                            ? replay_settled_member(*replayed[index])
                            : execute_command(members[index],
                                              operation_id + "/member/" + std::to_string(index),
                                              state->tokens[index], state->scopes[index],
                                              scope_depth + 1, false, false),
                        asio::bind_executor(
                            completion_executor,
                            [state, index, launch_member, &members, mode, collect, required, cap,
                             operation_token, completion_executor, interrupted,
                             settled](std::exception_ptr error, PlanExecution result) {
                                bool                     post_selection = false;
                                bool                     cancel_all     = false;
                                std::vector<std::size_t> to_launch;
                                bool                     signal_completion = false;
                                {
                                    std::lock_guard lock(state->mutex);
                                    state->errors[index] = error;
                                    if (!error) state->results[index] = std::move(result);
                                    state->finished[index] = true;
                                    state->finish_order.push_back(index);
                                    ++state->completed;
                                    if (state->active != 0) --state->active;

                                    const bool paused =
                                        interrupted(state->errors[index], state->results[index]);
                                    const bool member_failed =
                                        !paused && (error || state->results[index]->status !=
                                                                 ProgramTerminalStatus::Completed);
                                    if (paused) state->paused = true;
                                    // #400: a replayed member settles at once. While the
                                    // member this attempt answers runs, the decision it
                                    // would take is held, so that member consumes the
                                    // answer and settles its effect first. It is taken
                                    // when the answered member finishes.
                                    const bool answering =
                                        state->answered && !state->finished[*state->answered];
                                    const bool hold =
                                        answering && (state->replayed[index] || state->held);
                                    const bool release = state->held && state->answered &&
                                                         index == *state->answered;
                                    if (release) state->held = false;
                                    if (operation_token->is_cancelled() && !state->terminal) {
                                        state->terminal = true;
                                        cancel_all      = true;
                                    } else if (mode == "race" && (!paused || release) &&
                                               !state->terminal) {
                                        if (hold) {
                                            state->held = true;
                                        } else if (!state->selection_scheduled) {
                                            state->selection_scheduled = true;
                                            post_selection             = true;
                                        }
                                    } else if (mode == "all" && !collect &&
                                               (member_failed || release) && !state->terminal) {
                                        if (hold) {
                                            state->held = true;
                                        } else if (!state->selection_scheduled) {
                                            state->selection_scheduled = true;
                                            post_selection             = true;
                                        }
                                    } else if (mode == "quorum" && !state->terminal) {
                                        std::size_t successes = 0;
                                        std::size_t waiting   = 0;
                                        for (std::size_t i = 0; i < state->results.size(); ++i) {
                                            if (!state->finished[i]) continue;
                                            if (interrupted(state->errors[i], state->results[i]))
                                                ++waiting;
                                            else if (state->results[i] &&
                                                     state->results[i]->status ==
                                                         ProgramTerminalStatus::Completed)
                                                ++successes;
                                        }
                                        // A paused member may still succeed once it resumes.
                                        const bool decided =
                                            successes >= required ||
                                            successes + waiting +
                                                    (members.size() - state->completed) <
                                                required;
                                        if (decided && hold) {
                                            state->held = true;
                                        } else if (decided) {
                                            if (successes >= required) {
                                                for (std::size_t i = 0;
                                                     i < state->results.size(); ++i)
                                                    if (state->finished[i] && state->results[i] &&
                                                        state->results[i]->status ==
                                                            ProgramTerminalStatus::Completed)
                                                        state->quorum_members.push_back(i);
                                            }
                                            state->terminal = true;
                                            cancel_all      = true;
                                        }
                                    }

                                    if (state->initial_launches_registered && !state->terminal &&
                                        !state->paused && !state->selection_scheduled &&
                                        !state->held &&
                                        !post_selection && state->next < state->order.size()) {
                                        while (state->next < state->order.size() &&
                                               state->active < cap)
                                            to_launch.push_back(state->order[state->next++]);
                                    }
                                    signal_completion =
                                        to_launch.empty() && settled(*state, members.size());
                                }
                                if (post_selection) {
                                    asio::post(completion_executor, [state, mode, collect,
                                                                     operation_token, interrupted,
                                                                     settled,
                                                                     member_count =
                                                                         members.size()]() {
                                        std::vector<std::size_t> to_cancel;
                                        bool                     signal_completion = false;
                                        {
                                            std::lock_guard lock(state->mutex);
                                            if (!state->terminal) {
                                                // Settle on the first member to finish; a
                                                // paused member has not finished.
                                                std::optional<std::size_t> selected;
                                                for (const auto i : state->finish_order) {
                                                    if (interrupted(state->errors[i],
                                                                    state->results[i]))
                                                        continue;
                                                    const bool failed =
                                                        state->errors[i] || !state->results[i] ||
                                                        state->results[i]->status !=
                                                            ProgramTerminalStatus::Completed;
                                                    if (mode == "race" || (!collect && failed)) {
                                                        selected = i;
                                                        break;
                                                    }
                                                }
                                                if (selected) {
                                                    state->winner   = selected;
                                                    state->terminal = true;
                                                    for (std::size_t i = 0;
                                                         i < state->started.size(); ++i)
                                                        if (state->started[i] && i != *selected)
                                                            to_cancel.push_back(i);
                                                } else {
                                                    state->selection_scheduled = false;
                                                }
                                            }
                                            signal_completion = settled(*state, member_count);
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
                state->next = first_launches;
                for (std::size_t position = 0; position < first_launches; ++position)
                    (*launch_member)(state->order[position]);
                std::vector<std::size_t> deferred_launches;
                bool                     signal_completion = false;
                {
                    std::lock_guard lock(state->mutex);
                    // A fast member may finish while this loop is still registering its
                    // siblings. Do not permit that completion to close the join early.
                    state->initial_launches_registered = true;
                    if (!state->terminal && !state->paused && !state->selection_scheduled &&
                        !state->held && state->next < state->order.size()) {
                        while (state->next < state->order.size() && state->active < cap)
                            deferred_launches.push_back(state->order[state->next++]);
                    }
                    signal_completion =
                        deferred_launches.empty() && settled(*state, members.size());
                }
                for (const auto index : deferred_launches)
                    (*launch_member)(index);
                // Sent on the channel's strand, as every member's completion is:
                // the channel is not thread-safe.
                if (signal_completion)
                    asio::post(completion_executor,
                               [state] { (void)state->completion->try_send(asio::error_code(), 1); });

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
                std::vector<std::size_t>                  quorum_members;
                bool                                      decided = false;
                bool                                      paused  = false;
                {
                    std::lock_guard lock(state->mutex);
                    results        = std::move(state->results);
                    errors         = std::move(state->errors);
                    winner         = state->winner;
                    quorum_members = state->quorum_members;
                    decided        = state->terminal;
                    paused         = state->paused;
                }
                // #397: the join ends by the rule of a Core step (#392). A
                // cancelled join ends cancelled. Otherwise a member paused on
                // an effect whose outcome is unknown pauses the join, on the
                // first such member, when a member's failure would end it or
                // has ended it; a failed member is recorded and replays on
                // resume. A join decided by a success (a race winner, a quorum
                // met) ends that way, as with any paused member (#380).
                // #401: a collecting ng.all decides nothing before every
                // member settled, so it pauses on a paused member beside a
                // failure; in every join that pauses, a member paused on an
                // uncertain effect is asked before an ordinary question.
                std::optional<std::size_t> reconciling;
                bool                       member_failed = false;
                for (std::size_t i = 0; i < results.size(); ++i) {
                    if (errors[i]) {
                        member_failed = true;
                        continue;
                    }
                    if (!results[i]) continue;
                    if (reconciliation_pause(*results[i])) {
                        if (!reconciling) reconciling = i;
                    } else if (results[i]->status != ProgramTerminalStatus::Completed &&
                               results[i]->status != ProgramTerminalStatus::Interrupted &&
                               results[i]->status != ProgramTerminalStatus::Cancelled) {
                        member_failed = true;
                    }
                }
                const bool decided_by_success =
                    (mode == "race" && winner && !errors[*winner] && results[*winner] &&
                     results[*winner]->status == ProgramTerminalStatus::Completed) ||
                    (mode == "quorum" && !quorum_members.empty());
                const bool hold_for_reconciliation =
                    reconciling && !operation_token->is_cancelled() && !decided_by_success &&
                    (decided || member_failed);
                if (paused && (!decided || hold_for_reconciliation)) {
                    // The join is Interrupted on its first member paused on
                    // an uncertain effect, else on its first paused member
                    // (#401). Its output holds the results so far in
                    // declaration order.
                    PlanExecution paused_join;
                    paused_join.status = ProgramTerminalStatus::Interrupted;
                    paused_join.output = json::array();
                    std::optional<std::size_t> resumed_member = reconciling;
                    for (std::size_t i = 0; i < results.size(); ++i) {
                        const auto& member = results[i];
                        paused_join.output.push_back(
                            member && member->status == ProgramTerminalStatus::Completed
                                ? member->output
                                : json(nullptr));
                        if (!member) continue;
                        paused_join.execution_trace.insert(paused_join.execution_trace.end(),
                                                           member->execution_trace.begin(),
                                                           member->execution_trace.end());
                        if (member->status == ProgramTerminalStatus::Interrupted) {
                            if (!resumed_member) resumed_member = i;
                            // A paused nested join keeps the members that settled in it.
                            for (const auto& [key, record] : member->settled_members.items())
                                paused_join.settled_members[key] = record;
                            continue;
                        }
                        // Only a member's own end is kept; one cancelled with the
                        // run, for example, starts again on resume.
                        if (member->status != ProgramTerminalStatus::Completed &&
                            member->status != ProgramTerminalStatus::Failed &&
                            member->status != ProgramTerminalStatus::BudgetExhausted &&
                            member->status != ProgramTerminalStatus::TimedOut)
                            continue;
                        const auto member_operation =
                            operation_id + "/member/" + std::to_string(i);
                        if (replay_records[i]) {
                            paused_join.settled_members[member_operation] = *replay_records[i];
                            continue;
                        }
                        auto recorded = *member;
                        if (recorded.failure && recorded.failure->provider_outcome) {
                            try {
                                detail::persist_provider_failure(
                                    *recorded.failure,
                                    detail::provider_failure_custody(
                                        control->owner_scope, control->run_id,
                                        control->program_version_id, control->bundle_id,
                                        recorded.failure->operation_id, control->attempt),
                                    control->native_history_archive);
                            } catch (...) {
                                // Without its evidence the member runs again on resume.
                                continue;
                            }
                        }
                        paused_join.settled_members[member_operation] =
                            javascript_command_terminal_result(
                                recorded, to_string(recorded.status), ProgramUsage{});
                    }
                    auto& resumed = *results[*resumed_member];
                    paused_join.interrupt            = std::move(resumed.interrupt);
                    paused_join.interrupt_checkpoint = std::move(resumed.interrupt_checkpoint);
                    co_return paused_join;
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
                    // A quorum settles on the successes that met it; members that
                    // finish while the rest are cancelled do not join the result.
                    std::size_t successes = 0;
                    for (std::size_t i = 0; i < results.size(); ++i) {
                        if (!quorum_members.empty() &&
                            std::find(quorum_members.begin(), quorum_members.end(), i) ==
                                quorum_members.end())
                            continue;
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

                // A fail-fast join reports the member whose failure stopped
                // it; siblings it cancelled never replace that failure (#379).
                // Collect mode stops on no member and reports the first
                // failure in declaration order, as parallel_map does.
                std::optional<std::size_t> first_failure =
                    collect ? std::optional<std::size_t>{} : winner;
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
                    std::vector<ProgramJavaScriptCommandJournalEntry> durable_commands;
                    if (!control->recorded_replay)
                        durable_commands = co_await control->async_transitions().load_javascript_commands(
                            control->owner_scope, control->run_id);
                    const auto& recorded_commands = control->recorded_replay
                        ? control->recorded_replay->commands : durable_commands;
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
                auto effect_id =
                    javascript_command_effect_identity(*control, ordinal, command_value);
                std::vector<ProgramJavaScriptCommandJournalEntry> durable_prior_commands;
                if (!control->recorded_replay)
                    durable_prior_commands = co_await control->async_transitions().load_javascript_commands(
                        control->owner_scope, control->run_id);
                const auto& prior_commands = control->recorded_replay
                    ? control->recorded_replay->commands : durable_prior_commands;
                bool       child_resume_authorized      = false;
                auto       child_recovery_mode = ChildRecoveryMode::None;
                const auto can_resume_child = [&]() -> asio::awaitable<ChildRecoveryMode> {
                    auto        candidate       = command_value;
                    auto        child_operation = operation_id;
                    std::size_t depth           = 0;
                    while (candidate.kind() == JavaScriptCommandKind::Await) {
                        if (++depth > kMaxJavaScriptStructuredScopeDepth)
                            co_return ChildRecoveryMode::None;
                        candidate =
                            JavaScriptCommand::from_json(candidate.arguments().at("command"));
                        child_operation += "/await";
                    }
                    if (candidate.kind() != JavaScriptCommandKind::Spawn)
                        co_return ChildRecoveryMode::None;
                    const auto args = candidate.arguments();
                    co_return (co_await control->can_recover_child(args.at("child_binding").get<std::string>(),
                                                      args.at("input"), child_operation));
                };
                const auto prior = std::find_if(
                    prior_commands.rbegin(), prior_commands.rend(),
                    [&](const auto& entry) { return entry.command_ordinal() == ordinal; });
                if (control->recorded_replay) {
                    if (prior == prior_commands.rend() || !prior->completed() ||
                        !prior->effect_identity())
                        throw_runtime_diagnostic("P_REPLAY_EVIDENCE_REQUIRED",
                            "Recorded replay cannot dispatch an uncaptured command");
                }
                bool      exact_resume_authorized = false;
                bool      checkpoint_resume_authorized = false;
                bool      recovery_resume_authorized = false;
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
                        auto recorded =
                            decode_javascript_command_result(*prior->terminal_result(), *control);
                        if (control->recorded_replay) {
                            const auto durable_before_dispatch =
                                co_await control->async_transitions().load(control->owner_scope, control->run_id);
                            if (!durable_before_dispatch)
                                throw_runtime_diagnostic("P_REPLAY_COMMAND",
                                    "Captured command lost its running snapshot before replay");
                            CommandBudgetReservation reservation{
                                durable_before_dispatch->remaining_budget(), RunBudget{}};
                            // Replay owns new CPU work, not new provider or Program currency.
                            // Its measured command settlement must also authenticate any
                            // newly produced Core checkpoint in the same result CAS.
                            reservation.reservation.wall_time_ms =
                                reservation.remaining.wall_time_ms;
                            reservation.reservation.max_core_steps =
                                reservation.remaining.max_core_steps;
                            reservation.remaining.wall_time_ms = 0;
                            reservation.remaining.max_core_steps = 0;
                            auto publish_call_2 = control->publish_javascript_command(
                                ordinal, command_value, effect_id, std::nullopt,
                                reservation.remaining, reservation.reservation);
                            const auto pending = co_await std::move(publish_call_2);
                            if (pending != ProgramTransitionPublishResult::Published &&
                                pending != ProgramTransitionPublishResult::AlreadyPresent)
                                throw_runtime_diagnostic("P_REPLAY_COMMAND",
                                    "Captured command target coordinate lost its CAS");
                            // If result publication fails, the owned work stays spent;
                            // do not refund its reservation or invent an external effect.
                            unreconciled_javascript_remaining = reservation.remaining;
                            const auto core_before = core_progress->steps();
                            PlanExecution replayed;
                            if (recorded.status == ProgramTerminalStatus::Completed &&
                                (command_value.kind() == JavaScriptCommandKind::CallCore ||
                                 command_value.kind() == JavaScriptCommandKind::Checkpoint)) {
                                replayed = co_await execute_command(
                                    command_value, operation_id, control->cancel_token,
                                    root_scope, 0, false, false);
                                if (replayed.status != recorded.status || replayed.output != recorded.output)
                                    throw_runtime_diagnostic("P_REPLAY_OUTCOME",
                                        "Captured Core replay differs from its exact source observation");
                            } else {
                                replayed.status = recorded.status;
                                replayed.output = std::move(recorded.output);
                                replayed.failure = std::move(recorded.failure);
                                replayed.execution_trace = std::move(recorded.execution_trace);
                            }
                            if (replayed.failure && replayed.failure->provider_outcome)
                                detail::persist_provider_failure(
                                    *replayed.failure,
                                    detail::provider_failure_custody(
                                        control->owner_scope, control->run_id, control->program_version_id,
                                        control->bundle_id, replayed.failure->operation_id, control->attempt),
                                    control->native_history_archive);
                            ProgramUsage replay_work;
                            replay_work.wall_time_ms = elapsed_ms(started_at);
                            replay_work.core_steps = core_progress->steps();
                            replay_work.peak_concurrency = peak_concurrency.load(std::memory_order_relaxed);
                            auto remaining = settle_budget(control->granted_budget, replay_work);
                            auto command_work = replay_work;
                            command_work.wall_time_ms = subtract_saturated(
                                replay_work.wall_time_ms, settled_javascript_wall_time_ms);
                            command_work.core_steps = subtract_saturated(
                                replay_work.core_steps, core_before);
                            auto publish_call_3 = control->publish_javascript_command(
                                ordinal, command_value, effect_id,
                                javascript_command_terminal_result(replayed, to_string(replayed.status), command_work),
                                remaining, RunBudget{});
                            const auto completed = co_await std::move(publish_call_3);
                            if (completed != ProgramTransitionPublishResult::Published &&
                                completed != ProgramTransitionPublishResult::AlreadyPresent)
                                throw_runtime_diagnostic("P_REPLAY_COMMAND",
                                    "Captured command result lost its exact target CAS");
                            unreconciled_javascript_remaining = remaining;
                            settled_javascript_wall_time_ms = replay_work.wall_time_ms;
                            javascript_trace.insert(javascript_trace.end(),
                                replayed.execution_trace.begin(), replayed.execution_trace.end());
                            if (replayed.status != ProgramTerminalStatus::Completed)
                                co_return replayed;
                            if (command_value.kind() == JavaScriptCommandKind::Checkpoint)
                                co_await control->hold_latest_handoff_if_requested(ordinal);
                            response = std::move(replayed.output);
                            continue;
                        }
                        if (recorded.status != ProgramTerminalStatus::Completed) {
                            PlanExecution replayed;
                            replayed.status          = recorded.status;
                            replayed.output          = std::move(recorded.output);
                            replayed.failure         = std::move(recorded.failure);
                            replayed.execution_trace = std::move(recorded.execution_trace);
                            co_return replayed;
                        }
                        if (const auto durable_replay =
                                co_await control->async_transitions().load(control->owner_scope, control->run_id)) {
                            unreconciled_javascript_remaining = durable_replay->remaining_budget();
                        }
                        if (command_value.kind() == JavaScriptCommandKind::Checkpoint) {
                            co_await control->hold_latest_handoff_if_requested(ordinal);
                        }
                        response = std::move(recorded.output);
                        continue;
                    }

                    // #391: an await of this command that started in an
                    // earlier attempt keeps its recorded start.
                    if (const auto starts = prior->await_started_at()) {
                        std::lock_guard lock(await_starts_mutex);
                        await_starts = *starts;
                    }
                    child_recovery_mode = co_await can_resume_child();
                    if (command_value.kind() == JavaScriptCommandKind::Checkpoint) {
                        if (auto failure =
                                (co_await validate_javascript_command(command_value, operation_id, 0)))
                            co_return std::move(*failure);
                        // A checkpoint only returns its journaled value and
                        // stages internal events. No external dispatch needs
                        // reconciliation when its result publication was lost.
                        checkpoint_resume_authorized = true;
                    } else if (child_recovery_mode != ChildRecoveryMode::None) {
                        if (auto failure =
                                (co_await validate_javascript_command(command_value, operation_id, 0)))
                            co_return std::move(*failure);
                        child_resume_authorized = true;
                    } else if (checkpoint_id && resume_checkpoint &&
                               !checkpoint_of_a_completed_command(
                                   *control, prior_commands, ordinal,
                                   resume_checkpoint->core_thread_id)) {
                        if (auto failure =
                                (co_await validate_javascript_command(command_value, operation_id, 0)))
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
                        // The member this attempt resumes may have finished, and the
                        // join recorded it as settled, before the process stopped: its
                        // answer is spent, and recovery replays it like every other
                        // settled member instead of resuming its Core again.
                        const auto settled = prior->settled_members();
                        if (!settled || !settled->contains(*resume_operation)) {
                            exact_core_resume = std::pair{*resume_operation, *checkpoint_id};
                            supplied_core_resume_required = true;
                        }
                        exact_resume_authorized = true;
                        if (settled) {
                            const auto member_prefix = operation_id + "/member/";
                            for (const auto& [member, record] : settled->items()) {
                                if (!member.starts_with(member_prefix))
                                    throw_runtime_diagnostic(
                                        "P_JS_COMMAND_JOURNAL_MISMATCH",
                                        "Recorded join members do not match the paused join",
                                        json{{"command_ordinal", ordinal}, {"member", member}});
                            }
                            resumed_join_members = *settled;
                        }
                    } else if (auto saved = javascript_recovery_checkpoints(
                                   *control, command_value, operation_id,
                                   prior->await_started_at().value_or(json::object()))) {
                        // #402: the command's calls that saved a Core checkpoint
                        // continue from it; one that saved none is reconciled.
                        if (auto failure =
                                (co_await validate_javascript_command(command_value, operation_id, 0)))
                            co_return std::move(*failure);
                        {
                            std::lock_guard lock(plan_mutex);
                            for (const auto& [call, checkpoint] : saved->saved)
                                recovery_checkpoint_ids.insert(checkpoint);
                            recovery_core_resumes    = std::move(saved->saved);
                            recovery_unstarted_calls = std::move(saved->unstarted);
                        }
                        recovery_active               = true;
                        recovery_reached_unsaved_call = false;
                        recovery_reconciliation       = ProgramInterrupt{
                            "javascript", command_value.to_json(), std::nullopt,
                            javascript_command_pending_effect(*control, ordinal, command_value,
                                                              effect_id)};
                        recovery_resume_authorized = true;
                    } else {
                        // Already-published non-Core work is never dispatch
                        // authority. It must be explicitly reconciled.
                        const auto durable_pending =
                            co_await control->async_transitions().load(control->owner_scope, control->run_id);
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
                if (exact_resume_authorized || child_resume_authorized ||
                    checkpoint_resume_authorized || recovery_resume_authorized) {
                    auto durable_resumed =
                        co_await control->async_transitions().latest(control->owner_scope, control->run_id);
                    // #395: the first attempt of a fork resumes the command its
                    // source paused in, and reserves that command's resources
                    // itself: a fork's first publication holds none. A
                    // recovered attempt finds the reservation already made.
                    if (exact_resume_authorized && durable_resumed &&
                        !has_resource_reservation(durable_resumed->inflight_reservation) &&
                        durable_resumed->sequence == 1) {
                        const auto record =
                            co_await control->async_transitions().load(control->owner_scope, control->run_id);
                        if (record && record->fork_receipt()) {
                            auto reservation =
                                reserve_command_resources(durable_resumed->remaining_budget);
                            (void)co_await control->publish_command_reservation(
                                std::move(reservation.remaining),
                                std::move(reservation.reservation));
                            durable_resumed =
                                co_await control->async_transitions().latest(control->owner_scope, control->run_id);
                        }
                    }
                    if (!durable_resumed ||
                        !has_resource_reservation(durable_resumed->inflight_reservation)) {
                        throw_runtime_diagnostic(
                            "P_JS_COMMAND_JOURNAL_MISMATCH",
                            "Exact Core resume has no durable command resource reservation");
                    }
                    resource_reservation = durable_resumed->inflight_reservation;
                    unreconciled_javascript_remaining = durable_resumed->remaining_budget;
                    active_javascript_command =
                        ActiveJavaScriptCommand{ordinal, command_value, effect_id};
                }

                std::uint64_t operations_before = 0;
                {
                    std::lock_guard lock(plan_mutex);
                    operations_before = operation_count;
                }
                std::size_t command_count = 0;
                if (!exact_resume_authorized && !child_resume_authorized &&
                    !checkpoint_resume_authorized && !recovery_resume_authorized) {
                    try {
                        command_count = javascript_command_tree_size(command_value);
                    } catch (const ProgramDiagnosticError& error) {
                        co_return plan_failure(ProgramTerminalStatus::Failed,
                                               error.diagnostic().code, error.diagnostic().message,
                                               operation_id);
                    }
                    if (auto failure = reserve_operations(operation_id, command_count))
                        co_return std::move(*failure);
                    if (auto failure = (co_await validate_javascript_command(command_value, operation_id, 0)))
                        co_return std::move(*failure);

                    const auto durable_before_dispatch =
                        co_await control->async_transitions().load(control->owner_scope, control->run_id);
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
                    auto publish_call_4 = control->publish_javascript_command(
                        ordinal, command_value, effect_id, std::nullopt, pending_remaining,
                        resource_reservation);
                    const auto pending_published = co_await std::move(publish_call_4);
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
                const auto    money_before       = monetary_used();
                const auto    steps_before       = core_progress->steps();
                const auto    command_started_at = std::chrono::steady_clock::now();
                PlanExecution command_result;
                // A handler cannot await: the read of the durable remainder waits for the end of the chain.
                bool reload_remaining_after_child_recovery = false;
                try {
                    command_result = co_await execute_command(
                        command_value, operation_id, control->cancel_token, root_scope, 0, false,
                        child_recovery_mode == ChildRecoveryMode::Existing);
                } catch (const EventSinkError& error) {
                    command_result = plan_failure(ProgramTerminalStatus::Failed, "P_EVENT_SINK",
                                                  error.what(), operation_id);
                } catch (const graph::CancelledException& error) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Cancelled, "P_RUNTIME_CANCELLED",
                                     error.what(), operation_id);
                } catch (const ProgramDiagnosticError& error) {
                    if (child_recovery_mode == ChildRecoveryMode::Existing &&
                        error.diagnostic().code == "P_CHILD_RECOVERY") {
                        reload_remaining_after_child_recovery = true;
                        active_javascript_command.reset();
                        command_result.status = ProgramTerminalStatus::Interrupted;
                        command_result.interrupt = ProgramInterrupt{
                            "javascript", command_value.to_json(), std::nullopt,
                            javascript_command_pending_effect(*control, ordinal, command_value,
                                                              effect_id)};
                    } else {
                        command_result =
                            plan_failure(ProgramTerminalStatus::Failed, error.diagnostic().code,
                                         error.diagnostic().message, operation_id);
                    }
                } catch (const std::exception& error) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                     error.what(), operation_id);
                    retain_provider_failure(*command_result.failure, std::current_exception());
                } catch (...) {
                    command_result =
                        plan_failure(ProgramTerminalStatus::Failed, "P_RUNTIME_CORE_FAILURE",
                                     "Unknown JavaScript command failure", operation_id);
                }
                if (reload_remaining_after_child_recovery) {
                    if (const auto pending = co_await control->async_transitions().load(
                            control->owner_scope, control->run_id))
                        unreconciled_javascript_remaining = pending->remaining_budget();
                }
                resumed_join_members = json::object();
                {
                    std::lock_guard lock(await_starts_mutex);
                    await_starts = json::object();
                }
                if (recovery_active) {
                    const bool reconcile = recovery_reached_unsaved_call;
                    {
                        std::lock_guard lock(plan_mutex);
                        recovery_core_resumes.clear();
                        recovery_unstarted_calls.clear();
                    }
                    recovery_active               = false;
                    recovery_reached_unsaved_call = false;
                    if (reconcile) {
                        // #402: a call of the recovered command had no checkpoint
                        // to continue from. What the command's other calls did
                        // again stays in their checkpoints; the command is
                        // reconciled as a whole.
                        if (const auto durable_pending =
                                co_await control->async_transitions().load(control->owner_scope, control->run_id))
                            unreconciled_javascript_remaining = durable_pending->remaining_budget();
                        active_javascript_command.reset();
                        javascript_trace.insert(javascript_trace.end(),
                                                command_result.execution_trace.begin(),
                                                command_result.execution_trace.end());
                        result.status    = ProgramTerminalStatus::Interrupted;
                        result.interrupt = std::move(recovery_reconciliation);
                        co_return result;
                    }
                }
                javascript_trace.insert(javascript_trace.end(),
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
                    subtract_saturated(monetary_used(), money_before),
                    subtract_saturated(operations_after, operations_before),
                    subtract_saturated(core_progress->steps(), steps_before),
                    static_cast<std::uint32_t>(
                        std::min<std::size_t>(peak_concurrency.load(std::memory_order_relaxed),
                                              std::numeric_limits<std::uint32_t>::max()))};
                settled_javascript_wall_time_ms += command_usage.wall_time_ms;

                if (command_result.status == ProgramTerminalStatus::Interrupted) {
                    // A join resumes the member it paused on, whichever member
                    // wrote the last checkpoint (#380).
                    if (command_result.interrupt_checkpoint) {
                        std::lock_guard lock(plan_mutex);
                        last_root_checkpoint = command_result.interrupt_checkpoint;
                    }
                    // Record the members of a paused join that settled, so the
                    // attempt that resumes it replays them instead of running
                    // and charging them again (#380).
                    if (!command_result.settled_members.empty() && !control->recorded_replay) {
                        auto publish_call_5 = control->publish_javascript_command(
                            ordinal, command_value, effect_id, std::nullopt, std::nullopt,
                            std::nullopt, command_result.settled_members);
                        const auto recorded = co_await std::move(publish_call_5);
                        if (recorded != ProgramTransitionPublishResult::Published &&
                            recorded != ProgramTransitionPublishResult::AlreadyPresent) {
                            throw_runtime_diagnostic(
                                "P_JS_COMMAND_JOURNAL",
                                "Could not durably record the settled members of a paused join",
                                json{{"command_ordinal", ordinal}, {"effect_identity", effect_id}});
                        }
                    }
                    if (root_checkpoint()) {
                        unreconciled_javascript_remaining.reset();
                    }
                    co_return command_result;
                }

                if (command_result.failure && command_result.failure->provider_outcome) {
                    try {
                        detail::persist_provider_failure(
                            *command_result.failure,
                            detail::provider_failure_custody(
                                control->owner_scope, control->run_id, control->program_version_id,
                                control->bundle_id, command_result.failure->operation_id,
                                control->attempt),
                            control->native_history_archive);
                    } catch (...) {
                        std::throw_with_nested(ProviderOutcomeError(
                            "Program command provider evidence could not be persisted",
                            command_result.failure->provider_outcome,
                            command_result.failure->provider_cause));
                    }
                }
                const auto terminal_result = javascript_command_terminal_result(
                    command_result, to_string(command_result.status), command_usage);
                auto durable = co_await control->async_transitions().load(control->owner_scope, control->run_id);
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
                    auto publish_call_6 = control->publish_javascript_command(
                        ordinal, command_value, effect_id, terminal_result, remaining, RunBudget{});
                    published = co_await std::move(publish_call_6);
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
                    co_await control->hold_latest_handoff_if_requested(ordinal);
                }
                response = std::move(command_result.output);
            }
        };

        PlanExecution plan_result;
        if (control_source) {
            try {
                JavaScriptCompileLimits javascript_limits;
                javascript_limits.max_stack_bytes = control->scheduler->javascript_stack_bytes;
                const auto now                    = std::chrono::steady_clock::now();
                const auto remaining              = deadline > now
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                    : std::chrono::milliseconds::zero();
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
                // A command that fails reports no Core nodes of its own: its
                // Core threw. A failed attempt therefore lists every node that
                // completed in it, as a define()-only attempt does (#359).
                plan_result.execution_trace =
                    plan_result.status == ProgramTerminalStatus::Completed ||
                            plan_result.status == ProgramTerminalStatus::Interrupted
                        ? std::move(javascript_trace)
                        : core_progress->completed_nodes();
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
            // #386: a resumed plan runs again from its root on the plan's own
            // input and root Core thread; the value it was resumed with goes to
            // the call it answers, and what the paused run finished replays.
            auto plan_input = std::move(input);
            if (checkpoint_id) {
                resume_answer = std::move(plan_input);
                plan_input    = detail::owned_json_copy(control->persisted_invocation.input);
            }
            plan_result = co_await execute(
                root_id, std::move(plan_input),
                program_root_core_thread_id(control->run_id,
                                            control->materialized->root->compiled_plan_identity),
                0, control->cancel_token);
            // A paused plan resumes the call it paused on, whichever call wrote
            // the last checkpoint, and its Interrupted record carries what it
            // recorded (#384, #386).
            if (plan_result.status == ProgramTerminalStatus::Interrupted) {
                if (plan_result.interrupt_checkpoint) {
                    std::lock_guard lock(plan_mutex);
                    last_root_checkpoint = plan_result.interrupt_checkpoint;
                }
                control->set_settled_branches(std::move(plan_result.settled_members));
            }
        }

        const auto terminal_cause        = terminal_cause_at_deadline(control, deadline);
        outcome.output                   = std::move(plan_result.output);
        outcome.execution_trace          = std::move(plan_result.execution_trace);
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = core_progress->steps();
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
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
        outcome.failure = std::move(plan_result.failure);
        // #420, #443: a plan that completed can still be over a budget, because
        // usage is only settled once it has. The run ends with the failure
        // record of the budget it is over, in the shape of a run that the
        // budget stopped on the way (see budget_overrun.h): wall time ends it
        // TimedOut, as the deadline does, and the other budgets
        // BudgetExhausted. A grant of 0 allows nothing.
        auto overrun = plan_result.status == ProgramTerminalStatus::Completed
            ? completed_run_overrun(control->granted_budget, outcome.usage,
                                    !control->recorded_replay, monetary && monetary->exceeded())
            : std::optional<BudgetOverrun>{};
        if (apply_terminal_cause(control, outcome, terminal_cause, cancellation_message)) {
        } else if (plan_result.status != ProgramTerminalStatus::Completed) {
            outcome.status    = plan_result.status;
            outcome.interrupt = std::move(plan_result.interrupt);
        } else if (overrun) {
            outcome.status        = overrun->status;
            outcome.failure       = std::move(overrun->failure);
            completed_over_budget = true;
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
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(control, outcome, terminal_cause_at_deadline(control, deadline), error.what());
        needs_durable_disposition = true;
    } catch (const graph::CancelledException& error) {
        outcome.execution_trace          = core_progress->completed_nodes();
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        outcome.checkpoint               = control->latest_checkpoint();
        auto cause                       = terminal_cause_at_deadline(control, deadline);
        if (cause == CancellationCause::None) cause = CancellationCause::User;
        apply_terminal_cause(control, outcome, cause, error.what());
        needs_durable_disposition = true;
    } catch (const NestedOperationFailure& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(),
                                 error.operation_id, error.attempts);
        if (outcome.failure) {
            outcome.failure->operation_id = error.operation_id;
            outcome.failure->core_node    = error.core_node;
            retain_provider_failure(*outcome.failure, error.cause);
            apply_node_failure(*outcome.failure, std::current_exception());
        }
        outcome.execution_trace          = core_progress->completed_nodes();
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        const auto cause                 = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(control, outcome, cause, error.what()) && error.checkpoint_incompatible &&
            checkpoint_id) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
        needs_durable_disposition = true;
    } catch (const graph::NodeExecutionError& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what(), error.node_name(),
                                 static_cast<std::uint32_t>(error.attempts()));
        retain_provider_failure(*outcome.failure, error.cause());
        apply_node_failure(*outcome.failure, std::current_exception());
        outcome.execution_trace          = core_progress->completed_nodes();
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(control, outcome, terminal_cause_at_deadline(control, deadline), error.what());
        needs_durable_disposition = true;
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
        outcome.execution_trace          = core_progress->completed_nodes();
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(control, outcome, terminal_cause_at_deadline(control, deadline),
                             diagnostic.message);
        needs_durable_disposition = true;
    } catch (const std::exception& error) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", error.what());
        retain_provider_failure(*outcome.failure, std::current_exception());
        outcome.execution_trace          = core_progress->completed_nodes();
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.model_tokens       = model_tokens(usage);
        outcome.usage.monetary_microunits = monetary_used();
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        const auto cause                 = terminal_cause_at_deadline(control, deadline);
        if (!apply_terminal_cause(control, outcome, cause, error.what()) && checkpoint_id &&
            !checkpoint_is_available(*control, *checkpoint_id)) {
            apply_checkpoint_incompatible(outcome, control, *checkpoint_id, resume_checkpoint,
                                          error.what());
        }
        needs_durable_disposition = true;
    } catch (...) {
        outcome = failed_outcome(control, "P_RUNTIME_CORE_FAILURE", "Unknown Core failure");
        outcome.usage.wall_time_ms       = elapsed_ms(started_at);
        outcome.usage.program_operations = operation_count;
        outcome.usage.core_steps         = failed_core_steps(*control, core_progress->steps());
        outcome.usage.peak_concurrency   = peak_concurrency.load(std::memory_order_relaxed);
        outcome.usage.node_retries       = node_retries_used();
        outcome.remaining_budget         = settle_budget(control->granted_budget, outcome.usage);
        apply_terminal_cause(control, outcome, terminal_cause_at_deadline(control, deadline),
                             "Unknown Core failure");
        needs_durable_disposition = true;
    }

    if (needs_durable_disposition) co_await apply_durable_javascript_disposition(outcome);

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

    outcome.remaining_budget.max_dynamic_compiles = std::min<std::uint64_t>(
        control->dynamic_compile_ceiling.load(),
        subtract_saturated(control->granted_budget.max_dynamic_compiles, dynamic_compile_count));

    // #420: the model-token budget ends a run in one shape. A reply that took
    // the run over the budget stops it at the next step boundary with no failure
    // of its own, and a call whose reservation does not fit is refused before it
    // is sent with a failure that carries the provider's evidence. Both end the
    // run BudgetExhausted with the code of the failure classifier's budget class.
    // A run that completed over a budget already has its record, with every
    // budget it exceeded (#443): it is not rewritten, and a run that completed
    // past its deadline stays TimedOut.
    if (control->budget_exhausted->load(std::memory_order_acquire) &&
        control->cancellation_cause() == CancellationCause::None && !completed_over_budget) {
        const auto granted = control->granted_budget.model_tokens;
        const auto used    = outcome.usage.model_tokens;
        if (!(outcome.failure && outcome.failure->provider_outcome)) {
            outcome.status  = ProgramTerminalStatus::BudgetExhausted;
            outcome.failure = model_token_budget_failure(granted, used);
        } else if (outcome.failure->code ==
                   graph::node_failure_code(graph::NodeFailureClass::BudgetExhausted)) {
            outcome.status = ProgramTerminalStatus::BudgetExhausted;
            auto& witness  = outcome.failure->witness;
            if (!witness.is_object()) witness = json::object();
            witness["model_tokens"] = budget_witness(granted, used);
        }
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
    if (control->recorded_replay) {
        // Provider accounting is inherited once; playback is not new provider
        // spend. Only actual new replay CPU/Core work consumes this envelope.
        outcome.remaining_budget.model_tokens = control->granted_budget.model_tokens;
        if (!control->materialized->bundle.control_source()) {
            for (const auto& [operation, coordinates] : control->recorded_replay->operations) {
                if (!coordinates.empty()) {
                    outcome.status = ProgramTerminalStatus::Failed;
                    outcome.failure = ProgramFailure{"P_REPLAY_COORDINATES",
                        "Recorded replay did not consume every captured operation coordinate",
                        operation, "", 0, json::object()};
                    break;
                }
            }
        }
    }
    if (monetary) {
        // A refused call returns a synthetic not-sent failure; name it by its
        // P_MONEY_* code. A budget too small for the reservation exhausts the run.
        if (outcome.failure && outcome.failure->provider_outcome) {
            if (const auto refusal = monetary->refusal_for(outcome.failure->provider_outcome)) {
                outcome.failure->code    = refusal->code;
                outcome.failure->message = refusal->message;
                if (!outcome.failure->witness.is_object()) outcome.failure->witness = json::object();
                outcome.failure->witness["monetary"] = refusal->witness;
                if (refusal->budget && outcome.status == ProgramTerminalStatus::Failed)
                    outcome.status = ProgramTerminalStatus::BudgetExhausted;
            }
        }
        outcome.monetary_records = monetary->records();
    } else if (control->recorded_replay) {
        outcome.remaining_budget.monetary_microunits = control->granted_budget.monetary_microunits;
    }
    // #443: the record of a run that a budget stopped on the way says how the
    // run stands against every budget it is over.
    if ((outcome.status == ProgramTerminalStatus::BudgetExhausted ||
         outcome.status == ProgramTerminalStatus::TimedOut) &&
        outcome.failure)
        complete_budget_record(*outcome.failure, control->granted_budget, outcome.usage,
                               !control->recorded_replay, monetary && monetary->exceeded());
    outcome.provider_budget_authority = usage->authority_snapshot();
    cancel_deadline_timer(control->deadline_executor, std::move(timer));
    (co_await control->complete_async(std::move(outcome)));
    co_return;
}

}  // namespace neograph::program::detail
