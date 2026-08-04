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
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_map>
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
                         const ProgramInvocation&           invocation,
                         bool                                allow_child_metadata = false) {
    if (!allow_child_metadata &&
        (!invocation.parent_run_id.empty() || invocation.child_depth != 0))
        throw_runtime_diagnostic("P_START_CHILD_METADATA",
                                 "Root Program invocation cannot carry child lineage metadata");
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


std::string contract_fingerprint(const ContractRecord& contract) {
    return detail::sha256_identity(
        "program-module-port-contract/v1",
        detail::canonical_json_bytes(
            json{{"schema_version", contract.schema_version}, {"schema", contract.schema}}));
}

bool contains_all(const std::vector<std::string>& available,
                  const std::vector<std::string>& requested) {
    return std::all_of(requested.begin(), requested.end(), [&](const auto& value) {
        return std::find(available.begin(), available.end(), value) != available.end();
    });
}

bool budget_leq(const RunBudget& budget, const BudgetLimits& ceiling) {
    return budget.wall_time_ms <= ceiling.wall_time_ms &&
           budget.model_tokens <= ceiling.model_tokens &&
           budget.monetary_microunits <= ceiling.monetary_microunits &&
           budget.max_concurrency <= ceiling.max_concurrency &&
           budget.max_program_operations <= ceiling.max_program_operations &&
           budget.max_core_steps <= ceiling.max_core_steps &&
           budget.max_dynamic_compiles <= ceiling.max_dynamic_compiles &&
           budget.max_child_depth <= ceiling.max_child_depth &&
           budget.max_total_children <= ceiling.max_total_children;
}

RunBudget child_reservation(const RunBudget& budget) {
    auto result              = budget;
    result.max_child_depth   = 0;
    if (result.max_total_children == std::numeric_limits<std::uint64_t>::max())
        throw_runtime_diagnostic("P_CHILD_BUDGET", "Child budget exceeds total-child range");
    ++result.max_total_children;
    return result;
}

bool budget_sum_fits(const RunBudget& parent,
                     const RunBudget& used,
                     const RunBudget& additional) {
    const auto fits = [](auto limit, auto consumed, auto requested) {
        return consumed <= limit && requested <= limit - consumed;
    };
    return fits(parent.wall_time_ms, used.wall_time_ms, additional.wall_time_ms) &&
           fits(parent.model_tokens, used.model_tokens, additional.model_tokens) &&
           fits(parent.monetary_microunits, used.monetary_microunits,
                additional.monetary_microunits) &&
           fits(parent.max_concurrency, used.max_concurrency, additional.max_concurrency) &&
           fits(parent.max_program_operations, used.max_program_operations,
                additional.max_program_operations) &&
           fits(parent.max_core_steps, used.max_core_steps, additional.max_core_steps) &&
           fits(parent.max_dynamic_compiles, used.max_dynamic_compiles,
                additional.max_dynamic_compiles) &&
           fits(parent.max_total_children, used.max_total_children,
                additional.max_total_children);
}

void validate_child_link(const detail::MaterializedProgram& materialized,
                         const ModuleLinkReceipt&          link,
                         const ProgramVersion&              requested_version,
                         const ProgramInvocation&           invocation,
                         const RunBudget&                   parent_budget,
                         std::uint32_t                      parent_depth,
                         const RunBudget&                   reserved_children) {
    if (link.owner_scope() != requested_version.ownership_scope())
        throw_runtime_diagnostic("P_CHILD_OWNER", "Child link owner scope is not admitted");
    if (link.child_program_version_id() != requested_version.id() ||
        link.child_bundle_id() != requested_version.bundle_id() ||
        link.child_program_version_id() != materialized.version.id() ||
        link.child_bundle_id() != materialized.bundle.id()) {
        throw_runtime_diagnostic("P_CHILD_IDENTITY",
                                 "Child link does not bind the resolved Program version");
    }
    if (link.dependency_merkle_root() != materialized.bundle.module_dependency_merkle_root())
        throw_runtime_diagnostic("P_CHILD_MODULE_ROOT",
                                 "Child link dependency closure does not match the bundle");
    if (link.child_input_contract_fingerprint() !=
            contract_fingerprint(materialized.bundle.input_contract()) ||
        link.child_output_contract_fingerprint() !=
            contract_fingerprint(materialized.bundle.output_contract())) {
        throw_runtime_diagnostic("P_CHILD_PORT_CONTRACT",
                                 "Child link ports do not match the admitted bundle contracts");
    }
    const auto& closure = materialized.bundle.capability_effect_closure();
    if (!contains_all(link.granted_capabilities(), closure.capabilities) ||
        !contains_all(link.granted_effects(), closure.effects)) {
        throw_runtime_diagnostic("P_CHILD_AUTHORITY",
                                 "Child link does not cover the executable capability closure");
    }
    const auto policy = materialized.version.policy_snapshot();
    if (!contains_all(policy.allowed_capabilities(), closure.capabilities) ||
        !contains_all(policy.allowed_effects(), closure.effects)) {
        throw_runtime_diagnostic("P_CHILD_AUTHORITY",
                                 "Child policy does not cover the executable closure");
    }
    if (!budget_leq(invocation.budget, link.budget()))
        throw_runtime_diagnostic("P_CHILD_BUDGET",
                                 "Child invocation exceeds its immutable link budget");
    if (parent_depth >= parent_budget.max_child_depth) {
        throw_runtime_diagnostic("P_CHILD_DEPTH", "Child depth budget is exhausted");
    }
    const auto remaining_depth =
        parent_budget.max_child_depth - static_cast<std::uint32_t>(parent_depth) - 1;
    if (invocation.budget.max_child_depth >
        std::min(link.budget().max_child_depth, remaining_depth)) {
        throw_runtime_diagnostic("P_CHILD_DEPTH",
                                 "Child invocation would retain an unbounded depth grant");
    }
    if (reserved_children.max_total_children >= parent_budget.max_total_children) {
        throw_runtime_diagnostic("P_CHILD_COUNT", "Parent child-count budget is exhausted");
    }
    const auto remaining_children =
        parent_budget.max_total_children - reserved_children.max_total_children - 1;
    if (invocation.budget.max_total_children >
        std::min(link.budget().max_total_children, remaining_children)) {
        throw_runtime_diagnostic("P_CHILD_COUNT",
                                 "Child invocation would exceed the remaining child budget");
    }
}
std::string child_run_id_for(std::string_view            parent_run_id,
                             const ModuleLinkReceipt&    link,
                             const ProgramInvocation&    invocation) {
    if (!invocation.requested_run_id.empty()) return invocation.requested_run_id;
    return detail::sha256_identity(
        "program-child-run/v1",
        detail::canonical_json_bytes(json{{"parent_run_id", std::string(parent_run_id)},
                                          {"link_id", link.id()},
                                          {"input", invocation.input},
                                          {"trace_id", invocation.trace_id}}));
}

bool checkpointless_replay_safe(const ProgramPlan& plan) {
    if (plan.nodes().empty()) return false;
    return std::all_of(plan.nodes().begin(), plan.nodes().end(),
                       [](const ProgramPlanNode& node) {
                           switch (node.operation()) {
                               case ProgramOperationKind::Sequence:
                               case ProgramOperationKind::Branch:
                               case ProgramOperationKind::Loop:
                               case ProgramOperationKind::Retry:
                               case ProgramOperationKind::Parallel:
                               case ProgramOperationKind::Race:
                               case ProgramOperationKind::Quorum:
                               case ProgramOperationKind::Map:
                               case ProgramOperationKind::Spawn:
                               case ProgramOperationKind::Await:
                               case ProgramOperationKind::Return:
                                   return true;
                               case ProgramOperationKind::CallCore:
                               case ProgramOperationKind::Emit:
                               case ProgramOperationKind::Checkpoint:
                               case ProgramOperationKind::Cancel:
                                   return false;
                           }
                           return false;
                       });
}

ProgramChildRecord child_record_for(std::string_view         child_run_id,
                                    const ModuleLinkReceipt& link,
                                    const ProgramInvocation& invocation,
                                    ProgramChildState        state) {
    return ProgramChildRecord{std::string(child_run_id),
                              link.id(),
                              link.serialize_canonical(),
                              ProgramPersistedInvocation{invocation.input,
                                                         invocation.budget,
                                                         invocation.trace_id,
                                                         invocation.parent_run_id,
                                                         invocation.child_depth},
                              state};
}

std::optional<ProgramChildRecord> find_child(const ProgramRunRecord& record,
                                             std::string_view        child_run_id) {
    const auto children = record.children();
    const auto found = std::find_if(children.begin(), children.end(), [&](const auto& child) {
        return child.child_run_id == child_run_id;
    });
    if (found == children.end()) return std::nullopt;
    return *found;
}
bool same_child_metadata(const ProgramChildRecord& lhs, const ProgramChildRecord& rhs) {
    return lhs.child_run_id == rhs.child_run_id && lhs.link_id == rhs.link_id &&
           lhs.link_receipt == rhs.link_receipt && lhs.invocation == rhs.invocation;
}

bool same_child_result(const std::optional<ProgramResult>& lhs,
                       const std::optional<ProgramResult>& rhs) {
    if (lhs.has_value() != rhs.has_value()) return false;
    return !lhs || lhs->serialize_canonical() == rhs->serialize_canonical();
}

bool valid_child_state_transition(ProgramChildState previous, ProgramChildState next) {
    if (previous == next) return true;
    if (previous == ProgramChildState::Publishing &&
        (next == ProgramChildState::Dispatched || next == ProgramChildState::Completed ||
         next == ProgramChildState::Cancelled || next == ProgramChildState::Failed))
        return true;
    if (previous == ProgramChildState::Dispatched &&
        (next == ProgramChildState::Completed || next == ProgramChildState::Cancelled ||
         next == ProgramChildState::Failed))
        return true;
    return false;
}

ProgramChildState child_state_for_result(ProgramTerminalStatus status) {
    switch (status) {
        case ProgramTerminalStatus::Completed:
            return ProgramChildState::Completed;
        case ProgramTerminalStatus::Cancelled:
            return ProgramChildState::Cancelled;
        case ProgramTerminalStatus::Interrupted:
        case ProgramTerminalStatus::AmbiguousEffect:
            // These are resumable/reconcilable child states, not terminal
            // failures.  Keep the durable join in-flight until resume or
            // reconciliation publishes the final outcome.
            return ProgramChildState::Dispatched;
        default:
            return ProgramChildState::Failed;
    }
}

std::vector<ProgramChildRecord> terminal_children(
    const std::vector<ProgramChildRecord>& children) {
    auto result = children;
    for (auto& child : result) {
        const bool in_flight = child.state == ProgramChildState::Publishing ||
                               child.state == ProgramChildState::Dispatched;
        if (in_flight)
            child.state = ProgramChildState::Cancelled;
        // An interrupted/ambiguous child carries a non-cancelled result while
        // it is resumable.  Parent cancellation supersedes that outcome, and
        // the cancelled join record must not retain a mismatched result.
        if (in_flight && child.terminal_result &&
            child.terminal_result->status() != ProgramTerminalStatus::Cancelled)
            child.terminal_result.reset();
    }
    return result;
}

struct ChildRecordPublication {
    ProgramTransitionPublishResult result = ProgramTransitionPublishResult::Conflict;
    std::optional<ProgramRunRecord> record;
};

ChildRecordPublication publish_child_record(const std::shared_ptr<detail::RunControl>& parent,
                                             ProgramChildRecord child);

ChildRecordPublication publish_child_record(
    std::string_view                              owner_scope,
    std::string_view                              parent_run_id,
    const std::shared_ptr<ProgramTransitionStore>& transitions,
    ProgramChildRecord                            child) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto previous = transitions->load(owner_scope, parent_run_id);
        if (!previous) return {};
        const auto existing = find_child(*previous, child.child_run_id);
        if (existing) {
            if (!same_child_metadata(*existing, child)) {
                throw_runtime_diagnostic(
                    "P_CHILD_CONFLICT",
                    "Durable child identity is already bound to different metadata",
                    json{{"child_run_id", child.child_run_id}});
            }
            if (existing->state == child.state &&
                same_child_result(existing->terminal_result, child.terminal_result)) {
                return {ProgramTransitionPublishResult::AlreadyPresent, previous};
            }
            // A duplicate dispatch can race the first caller after it has
            // advanced the durable relation.  Publishing is the pre-dispatch
            // state, so an already-dispatched/terminal child is an idempotent
            // success for this metadata-only operation; callers re-read the
            // child below and reuse its durable run instead of regressing it.
            if (child.state == ProgramChildState::Publishing) {
                return {ProgramTransitionPublishResult::AlreadyPresent, previous};
            }
            if (!valid_child_state_transition(existing->state, child.state)) {
                throw_runtime_diagnostic(
                    "P_CHILD_CONFLICT",
                    "Durable child state transition is not monotonic",
                    json{{"child_run_id", child.child_run_id},
                         {"from", std::string(to_string(existing->state))},
                         {"to", std::string(to_string(child.state))}});
            }
            const auto next_state = child.state;
            const auto next_result = child.terminal_result;
            child = *existing;
            child.state = next_state;
            child.terminal_result = next_result;
        }
        if (previous->continuation().state != ContinuationState::Running) return {};
        const auto previous_journal = transitions->latest(owner_scope, parent_run_id);
        if (!previous_journal || previous_journal->id != previous->journal_head()) return {};

        auto children = previous->children();
        if (existing) {
            auto found = std::find_if(children.begin(), children.end(), [&](const auto& value) {
                return value.child_run_id == child.child_run_id;
            });
            if (found != children.end()) *found = child;
        } else {
            children.push_back(child);
        }
        auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(),
            std::string(parent_run_id),
            previous->program_version_id(),
            previous->bundle_id(),
            previous_journal->sequence + 1,
            previous_journal->continuation,
            previous->remaining_budget(),
            previous_journal->inflight_reservation,
            previous->exact_checkpoint(),
            now_ms()});
        ProgramRunRecordData data;
        data.owner_scope                      = previous->owner_scope();
        data.run_id                           = previous->run_id();
        data.program_version_id               = previous->program_version_id();
        data.bundle_id                        = previous->bundle_id();
        data.binding_fingerprint              = previous->binding_fingerprint();
        data.invocation                       = previous->invocation();
        data.continuation                     = journal.continuation;
        data.remaining_budget                 = previous->remaining_budget();
        data.exact_checkpoint                 = previous->exact_checkpoint();
        data.pending_input                    = previous->pending_input();
        data.pending_effect                  = previous->pending_effect();
        data.terminal_result                  = previous->terminal_result();
        data.fork_receipt                     = previous->fork_receipt();
        data.fork_source_run_id               = previous->fork_source_run_id();
        data.fork_source_program_version_id   = previous->fork_source_program_version_id();
        data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
        data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
        data.children                         = std::move(children);
        data.journal_head                     = journal.id;
        data.event_sequence                   = previous->event_sequence();
        data.effect_sequence                 = previous->effect_sequence();
        data.created_at_ms                    = previous->created_at_ms();
        data.updated_at_ms                    = journal.timestamp_ms;
        auto publication = ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
        const auto result = transitions->compare_publish(
            owner_scope, previous->journal_head(), std::move(publication));
        if (result == ProgramTransitionPublishResult::Published ||
            result == ProgramTransitionPublishResult::AlreadyPresent) {
            return {result, transitions->load(owner_scope, parent_run_id)};
        }
    }
    return {};
}

ChildRecordPublication publish_child_record(const std::shared_ptr<detail::RunControl>& parent,
                                             ProgramChildRecord child) {
    return publish_child_record(parent->owner_scope, parent->run_id, parent->transitions,
                                std::move(child));
}

void publish_child_completion(const std::shared_ptr<ProgramTransitionStore>& transitions,
                              std::string_view                              owner_scope,
                              std::string_view                              parent_run_id,
                              std::string_view                              child_run_id,
                              const ProgramResult&                           result) noexcept {
    try {
        const auto record = transitions->load(owner_scope, parent_run_id);
        if (!record) return;
        const auto existing = find_child(*record, child_run_id);
        if (!existing) return;
        const auto state = child_state_for_result(result.status());
        if (existing->state == state && existing->terminal_result &&
            existing->terminal_result->serialize_canonical() == result.serialize_canonical())
            return;
        auto update = *existing;
        update.state = state;
        update.terminal_result = result;
        (void)publish_child_record(owner_scope, parent_run_id, transitions, std::move(update));
    } catch (...) {
    }
}

void bind_child_completion(const std::shared_ptr<detail::RunControl>& child,
                           std::string_view                              owner_scope,
                           std::string_view                              parent_run_id,
                           std::string_view                              child_run_id) {
    const auto transitions = child->transitions;
    const std::string owner(owner_scope);
    const std::string parent(parent_run_id);
    const std::string child_id(child_run_id);
    child->set_completion_callback(
        [transitions, owner, parent, child_id](const ProgramResult& result) {
            publish_child_completion(transitions, owner, parent, child_id, result);
        });
}

bool is_unstarted_child_dispatch(const std::shared_ptr<ProgramTransitionStore>& transitions,
                                 std::string_view                              owner_scope,
                                 std::string_view                              child_run_id) {
    const auto record = transitions->load(owner_scope, child_run_id);
    if (!record || record->continuation().state != ContinuationState::Running ||
        record->continuation().attempt != 1 || record->event_sequence() != 1 ||
        record->effect_sequence() != 0 || record->exact_checkpoint() ||
        record->pending_input() || record->pending_effect() || record->terminal_result())
        return false;
    const auto journal = transitions->latest(owner_scope, child_run_id);
    if (!journal || journal->sequence != 1 || journal->previous_id != "") return false;
    const auto events = transitions->load_events(owner_scope, child_run_id);
    return events.size() == 1 && events.front().sequence == 1 &&
           events.front().kind == ProgramEventKind::Started;
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
            data.children            = terminal_children(previous->children());
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
void RunControl::set_completion_callback(CompletionCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    completion_callback_ = std::move(callback);
}
void RunControl::set_child_launch_callback(ChildLaunchCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    child_launch_callback_ = std::move(callback);
}

std::shared_ptr<RunControl> RunControl::launch_child(std::string_view binding_name,
                                                     json             input,
                                                     std::string_view operation_id,
                                                     std::string_view execution_key) const {
    ChildLaunchCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = child_launch_callback_;
    }
    if (!callback)
        throw_runtime_diagnostic("P_CHILD_BINDING",
                                 "Program spawn has no configured child binding resolver",
                                 json{{"binding", std::string(binding_name)},
                                      {"operation_id", std::string(operation_id)}});
    try {
        return callback(binding_name, std::move(input), operation_id, execution_key);
    } catch (const ProgramDiagnosticError& error) {
        auto diagnostic = error.diagnostic();
        if (!diagnostic.witness.is_object()) diagnostic.witness = json::object();
        diagnostic.witness["operation_id"] = std::string(operation_id);
        throw ProgramDiagnosticError(std::move(diagnostic));
    }
}


void RunControl::cancel_children(CancellationCause cause) noexcept {
    std::vector<std::shared_ptr<RunControl>> children;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(children_, [](const auto& weak) { return weak.expired(); });
        children.reserve(children_.size());
        for (const auto& weak : children_)
            if (auto child = weak.lock()) children.push_back(std::move(child));
    }
    for (const auto& child : children) (void)child->cancel(cause);
}

void RunControl::attach_child(const std::shared_ptr<RunControl>& child) noexcept {
    if (!child || child.get() == this) return;
    CancellationCause cause = CancellationCause::ParentTerminal;
    bool              cancel_now;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(children_, [](const auto& weak) { return weak.expired(); });
        cancel_now = terminal_decided_ || cancellation_cause_ != CancellationCause::None ||
                     result_.has_value();
        if (!cancel_now) {
            children_.push_back(child);
            return;
        }
        if (cancellation_cause_ != CancellationCause::None) cause = cancellation_cause_;
    }
    (void)child->cancel(cause);
}
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
                    previous->remaining_budget(), RunBudget{},
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
                data.children                    = terminal_children(previous->children());
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
                ProgramResult     callback_result = cancelled_result;
                CompletionCallback callback;
                std::vector<AsyncWaiter> waiters;
                {
                    std::lock_guard lock(mutex_);
                    cancellation_cause_ = cause;
                    terminal_decided_   = true;
                    completion_claimed_ = true;
                    events_.push_back(std::move(terminal));
                    callback = completion_callback_;
                }
                cancel_token->cancel();
                cancel_children(cause);
                if (callback) {
                    try {
                        callback(callback_result);
                    } catch (...) {
                    }
                }
                {
                    std::lock_guard lock(mutex_);
                    result_ = std::move(cancelled_result);
                    waiters.swap(waiters_);
                    cv_.notify_all();
                }
                asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
                    for (const auto& waiter : waiters) {
                        if (auto timer = waiter.timer.lock()) timer->cancel();
                    }
                });
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
        cancel_children(cause);
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
    return make_event(operation_id, kind, std::move(payload));
}

ProgramEvent RunControl::make_event(std::string_view       event_operation_id,
                                   ProgramEventKind        kind,
                                   ProgramEventPayload     payload) {
    return preview_event(next_sequence_++, event_operation_id, kind, std::move(payload));
}

ProgramEvent RunControl::preview_event(std::uint64_t       sequence,
                                       ProgramEventKind     kind,
                                       ProgramEventPayload payload) const {
    return preview_event(sequence, operation_id, kind, std::move(payload));
}

ProgramEvent RunControl::preview_event(std::uint64_t       sequence,
                                       std::string_view    event_operation_id,
                                       ProgramEventKind    kind,
                                       ProgramEventPayload payload) const {
    if (!materialized)
        throw std::runtime_error("Cannot create a live Program event without materialization");
    ProgramEvent event;
    event.sequence           = sequence;
    event.timestamp_ms       = now_ms();
    event.run_id             = run_id;
    event.program_version_id = program_version_id;
    event.bundle_id          = bundle_id;
    event.operation_id       = std::string(event_operation_id);
    event.core_generation_id = materialized->root->compiled_plan_identity;
    event.core_run_id        = core_thread_id;
    event.trace_id           = trace_id;
    event.attempt            = attempt;
    event.kind               = kind;
    event.payload            = std::move(payload);
    return ProgramEvent::create(std::move(event));
}

void RunControl::adopt_published_event(ProgramEvent event) {
    std::lock_guard lock(mutex_);
    if (event.sequence < next_sequence_) {
        const auto found = std::find_if(events_.begin(), events_.end(), [&](const auto& existing) {
            return existing.id == event.id;
        });
        if (found != events_.end()) return;
        throw std::logic_error("Published Program event sequence was already consumed");
    }
    if (event.sequence != next_sequence_)
        throw std::logic_error("Published Program event sequence is not contiguous");
    events_.push_back(std::move(event));
    ++next_sequence_;
}

ProgramEvent RunControl::stage_event(ProgramEventKind kind, ProgramEventPayload payload) {
    return stage_event(operation_id, kind, std::move(payload));
}

ProgramEvent RunControl::stage_event(std::string_view       event_operation_id,
                                     ProgramEventKind        kind,
                                     ProgramEventPayload     payload) {
    std::lock_guard lock(mutex_);
    auto            event = make_event(event_operation_id, kind, std::move(payload));
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
        bool cancel_run = false;
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
            // Once the terminal transition has been durably published, an
            // observer failure must not rewrite the committed result through
            // the interrupted-pending cancellation path.
            cancel_run = !terminal_decided_;
        }
        if (cancel_run) cancel(CancellationCause::EventSink);
        throw EventSinkError(error.what());
    } catch (...) {
        bool cancel_run = false;
        {
            std::lock_guard lock(mutex_);
            if (sink_ == sink) sink_.reset();
            cancel_run = !terminal_decided_;
        }
        if (cancel_run) cancel(CancellationCause::EventSink);
        throw EventSinkError("Program event sink threw");
    }
}

void RunControl::emit(ProgramEventKind kind, ProgramEventPayload payload) {
    emit(operation_id, kind, std::move(payload));
}

void RunControl::emit(std::string_view       event_operation_id,
                      ProgramEventKind        kind,
                      ProgramEventPayload     payload) {
    const auto event = stage_event(event_operation_id, kind, std::move(payload));
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
        const auto current_cause = cancellation_cause();
        cancel_children(current_cause == CancellationCause::None
                            ? CancellationCause::ParentTerminal
                            : current_cause);
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

        ProgramResult     callback_result = result;
        CompletionCallback callback;
        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            callback = completion_callback_;
        }
        if (callback) {
            try {
                callback(callback_result);
            } catch (...) {
            }
        }
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
    ProgramRuntime* owner = nullptr;

    void configure_child_launcher(const std::shared_ptr<detail::RunControl>& control) {
        if (!config.child_binding_resolver || !owner) return;
        const auto weak_control = std::weak_ptr<detail::RunControl>(control);
        control->set_child_launch_callback(
            [this, weak_control](std::string_view binding_name, json input,
                                 std::string_view operation_id, std::string_view execution_key) {
                const auto parent = weak_control.lock();
                if (!parent) throw std::runtime_error("Parent Program control expired");
                const auto resolved = config.child_binding_resolver(
                    parent->owner_scope, parent->program_version_id, binding_name);
                if (!resolved)
                    throw_runtime_diagnostic(
                        "P_CHILD_BINDING", "Verified child binding was not resolved",
                        json{{"binding", std::string(binding_name)},
                             {"operation_id", std::string(operation_id)}});
                if (resolved->receipt.child_name() != binding_name)
                    throw_runtime_diagnostic(
                        "P_CHILD_BINDING",
                        "Verified child binding does not match its immutable link receipt",
                        json{{"binding", std::string(binding_name)},
                             {"receipt_child_name", resolved->receipt.child_name()},
                             {"operation_id", std::string(operation_id)}});
                const auto& limits = resolved->receipt.budget();
                RunBudget budget{limits.wall_time_ms,
                                 limits.model_tokens,
                                 limits.monetary_microunits,
                                 limits.max_concurrency,
                                 limits.max_program_operations,
                                 limits.max_core_steps,
                                 limits.max_dynamic_compiles,
                                 limits.max_child_depth,
                                 limits.max_total_children};
                const auto parent_depth = parent->persisted_invocation.child_depth;
                if (parent_depth < parent->granted_budget.max_child_depth) {
                    budget.max_child_depth =
                        std::min(budget.max_child_depth,
                                 parent->granted_budget.max_child_depth - parent_depth - 1);
                } else {
                    budget.max_child_depth = 0;
                }
                if (parent->granted_budget.max_total_children > 0) {
                    budget.max_total_children =
                        std::min(budget.max_total_children,
                                 parent->granted_budget.max_total_children - 1);
                } else {
                    budget.max_total_children = 0;
                }
                const auto child_run_id = detail::sha256_identity(
                    "program-dsl-child/v1",
                    detail::canonical_json_bytes(
                        json{{"owner_scope", parent->owner_scope},
                             {"parent_run_id", parent->run_id},
                             {"parent_program_version_id", parent->program_version_id},
                             {"link_id", resolved->receipt.id()},
                             {"operation_id", std::string(operation_id)},
                             {"execution_key", std::string(execution_key)}}));
                ProgramInvocation invocation{std::move(input),
                                             budget,
                                             parent->trace_id + ":" +
                                                 std::string(operation_id) + ":" +
                                                 std::string(execution_key),
                                             {},
                                             child_run_id,
                                             parent->run_id,
                                             parent->persisted_invocation.child_depth + 1};
                ProgramHandle child = owner->start_child(
                    parent->owner_scope, ProgramHandle(parent), resolved->receipt,
                    resolved->version, std::move(invocation));
                return child.control_;
            });
    }

    void register_control(const std::shared_ptr<detail::RunControl>& control) {
        configure_child_launcher(control);
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


    bool reserve_child(std::string_view parent_run_id,
                       const RunBudget& parent_budget,
                       const RunBudget& child_budget) {
        const auto additional = child_reservation(child_budget);
        std::lock_guard lock(mutex);
        auto&           used = child_reservations[std::string(parent_run_id)];
        if (!budget_sum_fits(parent_budget, used, additional)) return false;
        used.wall_time_ms += additional.wall_time_ms;
        used.model_tokens += additional.model_tokens;
        used.monetary_microunits += additional.monetary_microunits;
        used.max_concurrency += additional.max_concurrency;
        used.max_program_operations += additional.max_program_operations;
        used.max_core_steps += additional.max_core_steps;
        used.max_dynamic_compiles += additional.max_dynamic_compiles;
        used.max_total_children += additional.max_total_children;
        return true;
    }

    void release_child(const std::string& parent_run_id, const RunBudget& child_budget) noexcept {
        const auto released = child_reservation(child_budget);
        std::lock_guard lock(mutex);
        const auto found = child_reservations.find(parent_run_id);
        if (found == child_reservations.end()) return;
        auto& used = found->second;
        const auto subtract = [](auto& value, auto amount) {
            value = value >= amount ? value - amount : 0;
        };
        subtract(used.wall_time_ms, released.wall_time_ms);
        subtract(used.model_tokens, released.model_tokens);
        subtract(used.monetary_microunits, released.monetary_microunits);
        subtract(used.max_concurrency, released.max_concurrency);
        subtract(used.max_program_operations, released.max_program_operations);
        subtract(used.max_core_steps, released.max_core_steps);
        subtract(used.max_dynamic_compiles, released.max_dynamic_compiles);
        subtract(used.max_total_children, released.max_total_children);
        if (used == RunBudget{}) child_reservations.erase(found);
    }

    RunBudget reserved_children(std::string_view parent_run_id) {
        std::lock_guard lock(mutex);
        const auto      found = child_reservations.find(std::string(parent_run_id));
        return found == child_reservations.end() ? RunBudget{} : found->second;
    }
    void hydrate_child_reservations(const ProgramRunRecord& parent) {
        RunBudget persisted{};
        const auto add = [](RunBudget& target, const RunBudget& value) {
            target.wall_time_ms += value.wall_time_ms;
            target.model_tokens += value.model_tokens;
            target.monetary_microunits += value.monetary_microunits;
            target.max_concurrency += value.max_concurrency;
            target.max_program_operations += value.max_program_operations;
            target.max_core_steps += value.max_core_steps;
            target.max_dynamic_compiles += value.max_dynamic_compiles;
            target.max_total_children += value.max_total_children;
        };
        for (const auto& child : parent.children()) {
            const auto reservation = child_reservation(child.invocation.granted_budget);
            if (!budget_sum_fits(parent.remaining_budget(), persisted, reservation)) {
                throw_runtime_diagnostic(
                    "P_CHILD_BUDGET",
                    "Persisted child reservations exceed the parent remainder",
                    json{{"parent_run_id", parent.run_id()}});
            }
            add(persisted, reservation);
        }
        std::lock_guard lock(mutex);
        const auto [found, inserted] =
            child_reservations.emplace(parent.run_id(), persisted);
        if (!inserted) {
            found->second.wall_time_ms = std::max(found->second.wall_time_ms, persisted.wall_time_ms);
            found->second.model_tokens =
                std::max(found->second.model_tokens, persisted.model_tokens);
            found->second.monetary_microunits =
                std::max(found->second.monetary_microunits, persisted.monetary_microunits);
            found->second.max_concurrency =
                std::max(found->second.max_concurrency, persisted.max_concurrency);
            found->second.max_program_operations =
                std::max(found->second.max_program_operations, persisted.max_program_operations);
            found->second.max_core_steps =
                std::max(found->second.max_core_steps, persisted.max_core_steps);
            found->second.max_dynamic_compiles =
                std::max(found->second.max_dynamic_compiles, persisted.max_dynamic_compiles);
            found->second.max_total_children =
                std::max(found->second.max_total_children, persisted.max_total_children);
        }
    }
    void shutdown() noexcept {
        try {
            std::vector<std::shared_ptr<detail::RunControl>> live;
            {
                std::lock_guard lock(mutex);
                if (stopping) return;
                stopping = true;
                for (auto& weak : controls) {
                    if (auto control = weak.lock(); control && !control->try_result())
                        live.push_back(std::move(control));
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
    std::unordered_map<std::string, RunBudget> child_reservations;
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
    impl_->owner = this;
}

ProgramRuntime::ProgramRuntime(ProgramRuntime&& other) noexcept : impl_(std::move(other.impl_)) {
    if (impl_) impl_->owner = this;
}

ProgramRuntime& ProgramRuntime::operator=(ProgramRuntime&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) impl_->shutdown();
    impl_ = std::move(other.impl_);
    if (impl_) impl_->owner = this;
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

ProgramHandle ProgramRuntime::start_child(std::string_view         owner_scope,
                                          const ProgramHandle&     parent,
                                          const ModuleLinkReceipt& link,
                                          const ProgramVersion&    version,
                                          ProgramInvocation        invocation) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (!parent.control_) throw std::invalid_argument("Parent Program handle is empty");
    if (parent.control_->transitions.get() != impl_->config.transitions.get())
        throw_runtime_diagnostic("P_CHILD_PARENT", "Parent handle belongs to another runtime");
    if (parent.control_->owner_scope != owner_scope || link.owner_scope() != owner_scope)
        throw_runtime_diagnostic("P_CHILD_OWNER", "Child owner scope is not admitted");

    const auto parent_record = parent.snapshot();
    if (parent_record.continuation().state != ContinuationState::Running)
        throw_runtime_diagnostic("P_CHILD_PARENT", "Child can only attach to a running parent");
    if (version.ownership_scope() != owner_scope)
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    const auto resolved = impl_->config.catalog->resolve_version(owner_scope, version.id());
    if (!resolved) throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    auto pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *resolved);

    const auto parent_depth = parent.control_->persisted_invocation.child_depth;
    if (parent_depth == std::numeric_limits<std::uint32_t>::max())
        throw_runtime_diagnostic("P_CHILD_DEPTH", "Child depth exceeds uint32 range");
    const std::string parent_run_id(parent.run_id());
    invocation.parent_run_id = parent_run_id;
    invocation.child_depth  = parent_depth + 1;
    const auto run_id        = child_run_id_for(parent_run_id, link, invocation);
    invocation.requested_run_id = run_id;
    if (const auto occupied = impl_->config.transitions->load(owner_scope, run_id)) {
        const ProgramPersistedInvocation expected{
            invocation.input, invocation.budget, invocation.trace_id,
            invocation.parent_run_id, invocation.child_depth};
        if (occupied->program_version_id() != version.id() ||
            occupied->bundle_id() != version.bundle_id() ||
            occupied->invocation() != expected) {
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT",
                "Requested child run id is already bound to another Program invocation",
                json{{"child_run_id", run_id}});
        }
    }
    impl_->hydrate_child_reservations(parent_record);
    const auto existing = find_child(parent_record, run_id);
    if (existing) {
        const auto expected = child_record_for(run_id, link, invocation, existing->state);
        if (!same_child_metadata(*existing, expected)) {
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT",
                "Durable child identity is already bound to different metadata",
                json{{"child_run_id", run_id}});
        }
        try {
            const auto persisted_link = ModuleLinkReceipt::parse(existing->link_receipt);
            if (persisted_link.id() != link.id() ||
                persisted_link.owner_scope() != owner_scope ||
                existing->invocation.parent_run_id != parent_run_id ||
                existing->invocation.child_depth != parent_depth + 1) {
                throw_runtime_diagnostic(
                    "P_CHILD_CONFLICT",
                    "Durable child receipt does not preserve its parent lineage",
                    json{{"child_run_id", run_id}});
            }
        } catch (const ProgramDiagnosticError&) {
            throw;
        } catch (const std::exception& error) {
            throw_runtime_diagnostic(
                "P_CHILD_RECEIPT",
                "Durable child receipt is malformed",
                json{{"child_run_id", run_id}, {"detail", error.what()}});
        }
    } else {
        const auto reserved = impl_->reserved_children(parent_run_id);
        validate_child_link(*pinned, link, version, invocation, parent_record.remaining_budget(),
                            parent_depth, reserved);
        validate_invocation(*pinned, invocation, true);
    }

    bool reservation_active = false;
    if (!existing) {
        if (!impl_->reserve_child(parent_run_id, parent_record.remaining_budget(),
                                  invocation.budget)) {
            throw_runtime_diagnostic("P_CHILD_BUDGET",
                                     "Child budget exceeds the parent's unspent allocation");
        }
        reservation_active = true;
        try {
            const auto publishing = child_record_for(
                run_id, link, invocation, ProgramChildState::Publishing);
            const auto attached = publish_child_record(parent.control_, publishing);
            if (!attached.record) {
                impl_->release_child(parent_run_id, invocation.budget);
                reservation_active = false;
                throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                         "Parent-child attachment lost its transition CAS");
            }
            if (attached.result == ProgramTransitionPublishResult::AlreadyPresent)
                impl_->release_child(parent_run_id, invocation.budget);
            reservation_active = false;
        } catch (...) {
            if (reservation_active) impl_->release_child(parent_run_id, invocation.budget);
            throw;
        }
    }

    bool replay_initial_dispatch = false;
    if (const auto child_record = impl_->config.transitions->load(owner_scope, run_id)) {
        const auto durable_parent =
            impl_->config.transitions->load(owner_scope, parent_run_id);
        const auto durable_child = durable_parent ? find_child(*durable_parent, run_id)
                                                  : std::optional<ProgramChildRecord>{};
        replay_initial_dispatch =
            ((existing && existing->state == ProgramChildState::Publishing) ||
             (durable_child && durable_child->state == ProgramChildState::Publishing)) &&
            is_unstarted_child_dispatch(impl_->config.transitions, owner_scope, run_id);
        if (auto live = impl_->find_control(owner_scope, run_id)) {
            bind_child_completion(live, owner_scope, parent_run_id, run_id);
            parent.control_->attach_child(live);
            return ProgramHandle(std::move(live));
        }
        if (child_record->continuation().state == ContinuationState::Running &&
            !replay_initial_dispatch) {
            auto reconnected = reconnect(owner_scope, run_id);
            bind_child_completion(reconnected.control_, owner_scope, parent_run_id, run_id);
            parent.control_->attach_child(reconnected.control_);
            if (const auto result = reconnected.try_result())
                publish_child_completion(impl_->config.transitions, owner_scope, parent_run_id,
                                         run_id, *result);
            return reconnected;
        }
        if (child_record->continuation().state != ContinuationState::Running) {
            auto reconnected = reconnect(owner_scope, run_id);
            bind_child_completion(reconnected.control_, owner_scope, parent_run_id, run_id);
            parent.control_->attach_child(reconnected.control_);
            if (const auto result = reconnected.try_result())
                publish_child_completion(impl_->config.transitions, owner_scope, parent_run_id,
                                         run_id, *result);
            return reconnected;
        }
    }

    std::shared_ptr<detail::RunControl> control;
    try {
        const auto core_thread_id =
            core_thread_identity(run_id, pinned->root->compiled_plan_identity);
        const auto binding_fingerprint = capability_binding_receipt_root(
            pinned->version.core_materialization_receipt().capability_bindings);
        ProgramPersistedInvocation persisted{invocation.input,
                                             invocation.budget,
                                             invocation.trace_id,
                                             invocation.parent_run_id,
                                             invocation.child_depth};
        control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), run_id, 1, std::move(pinned), binding_fingerprint,
            std::move(persisted), core_thread_id, 0, std::move(invocation.events),
            impl_->deadline_pool.get_executor(), impl_->config.checkpoints,
            impl_->config.state_store, impl_->config.transitions);
        bind_child_completion(control, owner_scope, parent_run_id, run_id);
        ProgramEvent started;
        if (replay_initial_dispatch) {
            const auto events = impl_->config.transitions->load_events(owner_scope, run_id);
            if (events.size() != 1 || events.front().kind != ProgramEventKind::Started ||
                events.front().sequence != 1) {
                throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                         "Durable child publication is incomplete",
                                         json{{"child_run_id", run_id}});
            }
            started = events.front();
            control->adopt_published_event(started);
        } else {
            started =
                control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
            const auto published = control->transitions->compare_publish(
                owner_scope, "", initial_publication(*control, started));
            if (published != ProgramTransitionPublishResult::Published &&
                published != ProgramTransitionPublishResult::AlreadyPresent) {
                auto winner = impl_->config.transitions->load(owner_scope, run_id);
                if (!winner) {
                    throw_runtime_diagnostic("P_RUN_CONFLICT",
                                             "Requested child Program run id is unavailable");
                }
                auto reconnected = reconnect(owner_scope, run_id);
                bind_child_completion(reconnected.control_, owner_scope, parent_run_id, run_id);
                parent.control_->attach_child(reconnected.control_);
                if (const auto result = reconnected.try_result())
                    publish_child_completion(impl_->config.transitions, owner_scope, parent_run_id,
                                             run_id, *result);
                return reconnected;
            }
        }

        // The durable relation is already present; this transition only records that
        // dispatch crossed the publication boundary.
        const auto dispatched = publish_child_record(
            parent.control_,
            child_record_for(run_id, link, invocation, ProgramChildState::Dispatched));
        const auto dispatched_record = dispatched.record
                                           ? find_child(*dispatched.record, run_id)
                                           : std::optional<ProgramChildRecord>{};
        if (!dispatched_record || dispatched_record->state != ProgramChildState::Dispatched) {
            if (dispatched_record && dispatched_record->state != ProgramChildState::Publishing) {
                auto reconnected = reconnect(owner_scope, run_id);
                bind_child_completion(reconnected.control_, owner_scope, parent_run_id, run_id);
                parent.control_->attach_child(reconnected.control_);
                if (const auto result = reconnected.try_result())
                    publish_child_completion(impl_->config.transitions, owner_scope, parent_run_id,
                                             run_id, *result);
                return reconnected;
            }
            const auto current_parent =
                impl_->config.transitions->load(owner_scope, parent_run_id);
            if (current_parent &&
                current_parent->continuation().state != ContinuationState::Running)
                (void)control->cancel(detail::CancellationCause::ParentTerminal);
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Parent-child dispatch lost its transition CAS",
                                     json{{"child_run_id", run_id}});
        }

        parent.control_->attach_child(control);
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
    } catch (...) {
        throw;
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

    // Core shape compatibility is only one part of the P5 contract.  Prove
    // the immutable version transition before cloning any checkpoint or
    // mutating either transition store.
    const auto migration_plan =
        MigrationPlan::between(*source_version, source_pinned->bundle, *resolved_target,
                               target_pinned->bundle);
    if (!migration_plan.is_compatible()) {
        throw_runtime_diagnostic(
            "P_FORK_MIGRATION_INCOMPATIBLE",
            "Program fork migration plan is not compatible",
            json{{"migration_plan_id", migration_plan.id()},
                 {"compatibility", std::string(to_string(migration_plan.compatibility()))},
                 {"blockers", migration_plan.blockers()}});
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
    if (!budget_leq(invocation.budget,
                   target_pinned->version.policy_snapshot().budget_ceiling())) {
        throw_runtime_diagnostic("P_FORK_BUDGET",
                                 "Fork continuation budget exceeds target authority ceiling");
    }
    const auto& target_ceiling =
        target_pinned->version.policy_snapshot().budget_ceiling();
    // A fork carries a *remainder*, so the initial admission minimum may
    // already have been consumed by the source run. Its upper bounds and
    // authority ceiling still constrain the target continuation.
    for (const auto& requirement : target_pinned->bundle.declared_budget_requirements()) {
        const auto granted = budget_value(invocation.budget, requirement.resource);
        const auto allowed = ceiling_value(target_ceiling, requirement.resource);
        if (granted > requirement.maximum || granted > allowed) {
            throw_runtime_diagnostic(
                "P_FORK_BUDGET",
                "Fork continuation budget exceeds target admitted bounds",
                json{{"resource", requirement.resource},
                     {"maximum", requirement.maximum},
                     {"policy_ceiling", allowed},
                     {"granted", granted}});
        }
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
    if (run_id == source_record->run_id()) {
        throw_runtime_diagnostic(
            "P_FORK_SAME_RUN",
            "Fork target run id must differ from the exact source run");
    }
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
    auto publication = initial_publication(
        *control, started, target_checkpoint, receipt, std::nullopt,
        std::move(fork_pending_input), std::move(fork_pending_effect));
    publication.migration_plan = migration_plan;
    const auto published =
        control->transitions->compare_publish(owner_scope, "", std::move(publication));
    if (published != ProgramTransitionPublishResult::Published) {

        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }
    std::optional<MigrationPlan> durable_plan;
    try {
        durable_plan = control->transitions->load_migration_plan(owner_scope, run_id);
    } catch (const std::exception& error) {
        throw_runtime_diagnostic(
            "P_FORK_MIGRATION_PROOF",
            "Fork publication migration proof could not be read back",
            json{{"run_id", run_id}, {"detail", error.what()}});
    }
    if (!durable_plan || durable_plan->id() != migration_plan.id()) {
        throw_runtime_diagnostic(
            "P_FORK_MIGRATION_PROOF",
            "Fork publication did not durably retain its migration proof",
            json{{"run_id", run_id},
                 {"expected_plan_id", migration_plan.id()},
                 {"stored_plan_id", durable_plan ? durable_plan->id() : std::string{}}});
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

std::vector<ProgramHandle> ProgramRuntime::recover_children(std::string_view owner_scope,
                                                              std::string_view parent_run_id) {
    if (owner_scope.empty())
        throw std::invalid_argument("Program owner scope must not be empty");
    if (parent_run_id.empty())
        throw std::invalid_argument("Program child recovery parent_run_id must not be empty");

    const auto parent_record = impl_->config.transitions->load(owner_scope, parent_run_id);
    if (!parent_record)
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program parent run was not found");
    auto parent = reconnect(owner_scope, parent_run_id);

    std::vector<ProgramHandle> recovered;
    for (const auto& persisted : parent_record->children()) {
        const auto link = [&] {
            try {
                return ModuleLinkReceipt::parse(persisted.link_receipt);
            } catch (const std::exception& error) {
                throw_runtime_diagnostic(
                    "P_CHILD_RECEIPT", "Durable child receipt is malformed",
                    json{{"child_run_id", persisted.child_run_id}, {"detail", error.what()}});
            }
        }();

        if (link.owner_scope() != owner_scope || link.id() != persisted.link_id ||
            persisted.invocation.parent_run_id != parent_run_id) {
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT", "Durable child receipt does not preserve parent lineage",
                json{{"child_run_id", persisted.child_run_id}});
        }

        if (persisted.state == ProgramChildState::Completed ||
            persisted.state == ProgramChildState::Cancelled ||
            persisted.state == ProgramChildState::Failed) {
            auto child = reconnect(owner_scope, persisted.child_run_id);
            parent.control_->attach_child(child.control_);
            if (const auto result = child.try_result())
                publish_child_completion(impl_->config.transitions, owner_scope, parent_run_id,
                                         persisted.child_run_id, *result);
            recovered.push_back(std::move(child));
            continue;
        }

        const auto version = impl_->config.catalog->resolve_version(
            owner_scope, link.child_program_version_id());
        if (!version)
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND",
                                     "Program version required for child recovery was not found",
                                     json{{"child_run_id", persisted.child_run_id}});
        ProgramInvocation invocation{persisted.invocation.input,
                                      persisted.invocation.granted_budget,
                                      persisted.invocation.trace_id,
                                      {},
                                      persisted.child_run_id,
                                      persisted.invocation.parent_run_id,
                                      persisted.invocation.child_depth};
        auto child = start_child(owner_scope, parent, link, *version, std::move(invocation));
        recovered.push_back(std::move(child));
    }
    return recovered;
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
    if (const auto fork = record->fork_receipt()) {
        // A fork is recoverable only when the exact migration proof survived
        // with the target snapshot.  Recompute the content-addressed proof as
        // well, so a store cannot silently substitute another compatible plan
        // with the same source/target version pair.
        std::optional<MigrationPlan> stored_plan;
        try {
            stored_plan = impl_->config.transitions->load_migration_plan(owner_scope, run_id);
        } catch (const std::exception& error) {
            throw_runtime_diagnostic(
                "P_FORK_MIGRATION_PROOF",
                "Fork recovery migration proof is unreadable",
                json{{"run_id", std::string(run_id)}, {"detail", error.what()}});
        }
        const auto source_version = impl_->config.catalog->resolve_version(
            owner_scope, fork->source_program_version_id());
        const auto target_version =
            impl_->config.catalog->resolve_version(owner_scope, record->program_version_id());
        if (!stored_plan || !source_version || !target_version || !fork->compatible() ||
            fork->owner_scope() != owner_scope || !record->fork_source_run_id() ||
            *record->fork_source_run_id() != fork->source_run_id() ||
            !record->fork_source_program_version_id() ||
            *record->fork_source_program_version_id() != fork->source_program_version_id() ||
            !record->fork_source_checkpoint_id() ||
            *record->fork_source_checkpoint_id() != fork->source_checkpoint_id() ||
            fork->target_program_version_id() != record->program_version_id() ||
            stored_plan->owner_scope() != owner_scope || !stored_plan->is_compatible() ||
            stored_plan->source_version_id() != fork->source_program_version_id() ||
            stored_plan->target_version_id() != record->program_version_id()) {
            throw_runtime_diagnostic(
                "P_FORK_MIGRATION_PROOF",
                "Fork recovery requires a durable compatible migration proof",
                json{{"run_id", std::string(run_id)},
                     {"source_version_id", fork->source_program_version_id()},
                     {"target_version_id", record->program_version_id()}});
        }
        const auto expected_plan = impl_->config.catalog->plan_migration(
            owner_scope, source_version->id(), target_version->id());
        if (!expected_plan.is_compatible() || expected_plan.id() != stored_plan->id()) {
            throw_runtime_diagnostic(
                "P_FORK_MIGRATION_PROOF",
                "Durable fork migration proof does not match admitted versions",
                json{{"run_id", std::string(run_id)},
                     {"stored_plan_id", stored_plan->id()},
                     {"expected_plan_id", expected_plan.id()}});
        }
    }
    const auto state = record->continuation().state;
    if (state == ContinuationState::Running) {
        const auto version =
            impl_->config.catalog->resolve_version(owner_scope, record->program_version_id());
        if (!version) {
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND",
                                     "Program version required for recovery was not found");
        }
        auto pinned = detail::CatalogRuntimeAccess::pin(*impl_->config.catalog, *version);
        const auto binding_fingerprint = capability_binding_receipt_root(
            version->core_materialization_receipt().capability_bindings);
        if (pinned->bundle.id() != record->bundle_id() ||
            binding_fingerprint != record->binding_fingerprint()) {
            throw_runtime_diagnostic("P_RUN_RECOVERY_IDENTITY",
                                     "Running Program recovery identity is incompatible");
        }

        const auto checkpoint = record->exact_checkpoint();
        std::string recovered_thread_id;
        std::optional<std::string> resume_checkpoint_id;
        if (checkpoint) {
            if (pinned->root->compiled_plan_identity != checkpoint->core_generation_id ||
                pinned->root->core_name != checkpoint->core_name) {
                throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                         "Running Program recovery identity is incompatible");
            }
            const auto stored_checkpoint =
                impl_->config.checkpoints->load_by_id(checkpoint->checkpoint_id);
            if (!stored_checkpoint || stored_checkpoint->thread_id != checkpoint->core_thread_id ||
                stored_checkpoint->schema_version != checkpoint->checkpoint_schema_version ||
                stored_checkpoint->schema_version != graph::CHECKPOINT_SCHEMA_VERSION) {
                throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                         "Running Program recovery checkpoint is absent or invalid");
            }
            recovered_thread_id = checkpoint->core_thread_id;
            resume_checkpoint_id = checkpoint->checkpoint_id;
        } else {
            if (!checkpointless_replay_safe(pinned->bundle.typed_orchestration_plan())) {
                throw_runtime_diagnostic(
                    "P_RUN_RECOVERY_BLOCKED",
                    "Running Program has no checkpoint and is not replay-safe without Core dispatch",
                    json{{"run_id", std::string(run_id)}});
            }
            recovered_thread_id =
                core_thread_identity(run_id, pinned->root->compiled_plan_identity);
        }

        ProgramPersistedInvocation invocation{
            record->invocation().input,
            record->remaining_budget(),
            record->invocation().trace_id,
            record->invocation().parent_run_id,
            record->invocation().child_depth};
        auto control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), std::string(run_id), record->continuation().attempt,
            std::move(pinned), record->binding_fingerprint(), std::move(invocation),
            std::move(recovered_thread_id), record->event_sequence(),
            std::shared_ptr<ProgramEventSink>{},
            impl_->deadline_pool.get_executor(), impl_->config.checkpoints,
            impl_->config.state_store, impl_->config.transitions);
        if (!record->invocation().parent_run_id.empty())
            bind_child_completion(control, owner_scope, record->invocation().parent_run_id,
                                  run_id);
        if (!record->invocation().parent_run_id.empty()) {
            if (auto parent = impl_->find_control(owner_scope,
                                                  record->invocation().parent_run_id))
                parent->attach_child(control);
        }
        impl_->register_control(control);
        spawn_run_attempt(impl_->pool, control, record->invocation().input,
                          std::move(resume_checkpoint_id));
        return ProgramHandle(std::move(control));
    }
    const bool requires_result = state != ContinuationState::Interrupted &&
                                 state != ContinuationState::AmbiguousEffect;
    if (requires_result && !record->terminal_result()) {
        throw_runtime_diagnostic("P_RUN_INVALID", "Stored terminal Program run is incomplete");
    }
    auto control = std::make_shared<detail::RunControl>(*record, impl_->config.transitions);
    if (!record->invocation().parent_run_id.empty())
        bind_child_completion(control, owner_scope, record->invocation().parent_run_id, run_id);
    return ProgramHandle(std::move(control));
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
            previous->remaining_budget(), RunBudget{}, checkpoint, now_ms()});
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
        expired_data.children                     = terminal_children(previous->children());
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
        if (!previous->invocation().parent_run_id.empty())
            publish_child_completion(impl_->config.transitions, owner_scope,
                                     previous->invocation().parent_run_id, run_id,
                                     terminal_result);
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
        previous->invocation().input,
        previous->remaining_budget(),
        resume_value.trace_id,
        previous->invocation().parent_run_id,
        previous->invocation().child_depth};
    auto control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), std::string(run_id), previous->continuation().attempt + 1,
        std::move(pinned), previous->binding_fingerprint(), std::move(attempt_invocation),
        checkpoint.core_thread_id, previous->event_sequence(), std::move(resume_value.events),
        impl_->deadline_pool.get_executor(), impl_->config.checkpoints, impl_->config.state_store,
        impl_->config.transitions);
    if (!previous->invocation().parent_run_id.empty())
        bind_child_completion(control, owner_scope, previous->invocation().parent_run_id, run_id);
    if (!previous->invocation().parent_run_id.empty()) {
        if (auto parent = impl_->find_control(owner_scope, previous->invocation().parent_run_id))
            parent->attach_child(control);
    }
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
    data.children                       = previous->children();
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
        completed ? previous->remaining_budget() : RunBudget{}, checkpoint, now_ms()});

    std::shared_ptr<detail::RunControl> live_control;
    std::optional<ProgramResult>        terminal;
    std::vector<ProgramEvent>           outbox;
    if (completed) {
        ProgramPersistedInvocation invocation{previous->invocation().input,
                                              previous->remaining_budget(), resolution.trace_id,
                                              previous->invocation().parent_run_id,
                                              previous->invocation().child_depth};
        live_control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), std::string(run_id), next_attempt, std::move(pinned),
            previous->binding_fingerprint(), std::move(invocation), checkpoint.core_thread_id,
            previous->event_sequence(), std::move(resolution.events),
            impl_->deadline_pool.get_executor(), impl_->config.checkpoints,
            impl_->config.state_store, impl_->config.transitions);
        if (!previous->invocation().parent_run_id.empty())
            bind_child_completion(live_control, owner_scope,
                                  previous->invocation().parent_run_id, run_id);
        if (!previous->invocation().parent_run_id.empty()) {
            if (auto parent = impl_->find_control(owner_scope,
                                                  previous->invocation().parent_run_id))
                parent->attach_child(live_control);
        }
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
    data.children                       = previous->children();
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
    if (!completed) {
        auto reconnected = reconnect(owner_scope, run_id);
        if (const auto result = reconnected.try_result()) {
            if (!previous->invocation().parent_run_id.empty())
                publish_child_completion(impl_->config.transitions, owner_scope,
                                         previous->invocation().parent_run_id, run_id, *result);
        }
        return reconnected;
    }

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
