#include <neograph/program/runtime.h>
#include <neograph/program/compiler.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/schema.h>
#include <neograph/async/host_loop.h>
#include <neograph/async/run_sync.h>
#include <neograph/build_config.h>
#include <neograph/provider.h>

#include "canonical_json.h"
#include "node_retry_budget.h"
#include "catalog_access.h"
#include "monetary.h"
#include "lock_audit.h"
#include "operation_lock.h"
#include "provider_failure.h"
#include "run_control.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/dispatch.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/prefer.hpp>
#include <asio/strand.hpp>
#include <asio/redirect_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <type_traits>
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

std::string replacement_run_id(const ExactProgramHandoffReference& source,
                               std::string_view                     target_version_id,
                               const json&                          target_input) {
    return detail::sha256_identity(
        "program-replacement-run/v1",
        detail::canonical_json_bytes(
            json{{"source_run_id", source.source_run_id},
                 {"source_journal_head", source.source_journal_head},
                 {"checkpoint_coordinate_id", source.command_coordinate_id},
                 {"checkpoint_entry_id", source.command_entry_id},
                 {"target_program_version_id", std::string(target_version_id)},
                 {"target_input_identity", program_replacement_input_identity(target_input)}}));
}

std::string graph_migration_run_id(const GraphMigrationCapsule& capsule,
                                   std::string_view              target_version_id) {
    return detail::sha256_identity(
        "program-graph-migration-run/v1",
        detail::canonical_json_bytes(
            json{{"capsule_id", capsule.id()},
                 {"target_program_version_id", std::string(target_version_id)}}));
}

bool supports_live_graph_migration(const detail::MaterializedProgram& materialized) {
    const auto& plan = materialized.bundle.typed_orchestration_plan();
    return !materialized.bundle.control_source() && plan.nodes().size() == 1 &&
           plan.root().operation() == ProgramOperationKind::CallCore && plan.root().core() &&
           *plan.root().core() == materialized.root->core_name;
}

bool supports_root_execution_lease(
    const detail::MaterializedProgram& materialized,
    const RunBudget& budget) {
    return supports_live_graph_migration(materialized) &&
           budget.max_child_depth == 0 && budget.max_total_children == 0;
}

std::optional<ProgramExecutionLease> make_execution_lease(
    const detail::MaterializedProgram& materialized,
    const ProgramTransitionPublication& publication) {
    const auto& run = publication.run_record;
    if (!supports_root_execution_lease(materialized, run.remaining_budget()) ||
        run.child_depth() != 0 ||
        !run.invocation().parent_run_id.empty()) {
        return std::nullopt;
    }
    const auto acquired = run.updated_at_ms();
    const auto wall = run.remaining_budget().wall_time_ms;
    if (!wall || wall > static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max() - acquired)) {
        throw_runtime_diagnostic(
            "P_RUN_LEASE", "Program execution lease lifetime exceeds the supported range");
    }
    return ProgramExecutionLease(ProgramExecutionLeaseData{
        run.owner_scope(), run.run_id(), run.continuation().attempt,
        run.program_version_id(), run.bundle_id(), materialized.root->core_name,
        materialized.root->compiled_plan_identity,
        program_root_core_thread_id(run.run_id(),
                                    materialized.root->compiled_plan_identity),
        graph::Checkpoint::generate_id(), acquired,
        acquired + static_cast<std::int64_t>(wall)});
}

ProgramTransitionPublication continuing_running_publication(
    const ProgramRunRecord&     previous,
    const ProgramJournalRecord& previous_journal,
    RunBudget                   remaining,
    std::int64_t                timestamp,
    RunBudget                   inflight = {}) {
    auto                 journal = ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.journal_head(), previous.run_id(), previous.program_version_id(),
        previous.bundle_id(), previous_journal.sequence + 1, previous.continuation(), remaining,
        inflight, previous.exact_checkpoint(), timestamp});
    ProgramRunRecordData data;
    data.owner_scope = previous.owner_scope();
    data.run_id = previous.run_id();
    data.logical_run_id                   = previous.logical_run_id();
    data.program_version_id = previous.program_version_id();
    data.bundle_id = previous.bundle_id();
    data.binding_fingerprint = previous.binding_fingerprint();
    data.invocation = previous.invocation();
    data.child_depth = previous.child_depth();
    data.children = previous.children();
    data.continuation = journal.continuation;
    data.remaining_budget = remaining;
    data.exact_checkpoint = previous.exact_checkpoint();
    data.exact_checkpoint_content_id = previous.exact_checkpoint_content_id();
    data.attempt_resume = previous.attempt_resume();
    data.settled_branches = previous.settled_branches();
    data.pending_input = previous.pending_input();
    data.pending_effect = previous.pending_effect();
    data.terminal_result = previous.terminal_result();
    data.fork_receipt = previous.fork_receipt();
    data.fork_source_run_id = previous.fork_source_run_id();
    data.fork_source_program_version_id = previous.fork_source_program_version_id();
    data.fork_source_checkpoint_id = previous.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = previous.recorded_binding_set_fingerprint();
    data.journal_head = journal.id;
    data.event_sequence = previous.event_sequence();
    data.effect_sequence = previous.effect_sequence();
    data.created_at_ms = previous.created_at_ms();
    data.updated_at_ms = timestamp;
    return ProgramTransitionPublication{
        ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
}

RunBudget lineage_descendant_budget(const ProgramRunRecord& run) {
    RunBudget committed;
    const auto add = [](auto& target, auto value) {
        using Integer = std::decay_t<decltype(target)>;
        if (value > std::numeric_limits<Integer>::max() - target)
            throw_runtime_diagnostic("P_CHILD_BUDGET", "Durable child budget ledger overflowed");
        target = static_cast<Integer>(target + value);
    };
    for (const auto& child : run.children()) {
        const auto& budget = child.invocation.granted_budget;
        add(committed.wall_time_ms, budget.wall_time_ms);
        add(committed.model_tokens, budget.model_tokens);
        add(committed.monetary_microunits, budget.monetary_microunits);
        add(committed.max_program_operations, budget.max_program_operations);
        add(committed.max_core_steps, budget.max_core_steps);
        add(committed.max_dynamic_compiles, budget.max_dynamic_compiles);
        if (budget.max_total_children == std::numeric_limits<std::uint64_t>::max())
            throw_runtime_diagnostic("P_CHILD_BUDGET", "Durable child budget ledger overflowed");
        add(committed.max_total_children, budget.max_total_children + 1);
    }
    return committed;
}

RunBudget debit_budget(RunBudget available, const RunBudget& debit) noexcept {
    available.wall_time_ms -= debit.wall_time_ms;
    available.model_tokens -= debit.model_tokens;
    available.monetary_microunits -= debit.monetary_microunits;
    available.max_concurrency -= debit.max_concurrency;
    available.max_program_operations -= debit.max_program_operations;
    available.max_core_steps -= debit.max_core_steps;
    available.max_dynamic_compiles -= debit.max_dynamic_compiles;
    available.max_child_depth -= debit.max_child_depth;
    available.max_total_children -= debit.max_total_children;
    return available;
}

// #414: what a fork takes out of its source's remainder. Wall time, tokens,
// money, operations, Core steps, dynamic compiles and child budgets are
// amounts: the fork's share leaves the source. max_concurrency is a limit,
// not an amount: the fork carries its own and the source keeps its own.
RunBudget debit_fork_share(RunBudget available, const RunBudget& share) noexcept {
    const auto concurrency    = available.max_concurrency;
    available                 = debit_budget(std::move(available), share);
    available.max_concurrency = concurrency;
    return available;
}

// The lineage envelope of a publication, from what attach_run_lineage read: the lineage the run belongs to, if
// any, and the generation it names when the publication is a retry of the run's last one.
ProgramTransitionPublication attach_run_lineage_from(ProgramTransitionPublication      publication,
                                                     std::string_view                  lineage_id,
                                                     const std::optional<ProgramRunLineage>& current,
                                                     const std::optional<ProgramRunGeneration>& generation) {
    const auto& run = publication.run_record;
    if (!current) {
        auto first = ProgramRunGeneration::create(ProgramRunGenerationData{
            run.owner_scope(), std::string(lineage_id), 1, run.run_id(), run.program_version_id(),
            run.bundle_id(), run.id(), run.journal_head(), std::nullopt, run.created_at_ms(),
            run.child_depth()});
        publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
            run.owner_scope(), std::string(lineage_id), run.run_id(), 1, first.id(), run.id(),
            run.journal_head(), publication.journal_record.remaining_budget,
            publication.journal_record.inflight_reservation, std::nullopt, run.created_at_ms(),
            run.updated_at_ms(), lineage_descendant_budget(run)});
        publication.run_generation = std::move(first);
        return publication;
    }

    // Reconstruct the exact initial/successor envelope for an idempotent retry.
    if (current->active_run_record_id() == run.id() &&
        current->active_journal_head() == run.journal_head()) {
        publication.run_lineage = *current;
        if (generation && generation->initial_run_record_id() == run.id() &&
            generation->initial_journal_head() == run.journal_head()) {
            publication.run_generation = *generation;
        }
        return publication;
    }

    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        current->owner_scope(), current->lineage_id(), current->root_run_id(),
        current->active_generation(), current->active_generation_id(), run.id(),
        run.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, current->id(), current->created_at_ms(),
        run.updated_at_ms(), lineage_descendant_budget(run)});
    return publication;
}

bool is_retry_of_lineage_head(const ProgramRunLineage& current, const ProgramRunRecord& run) {
    return current.active_run_record_id() == run.id() && current.active_journal_head() == run.journal_head();
}

// The lineage envelope of a publication, reading the lineage the awaited way.
asio::awaitable<ProgramTransitionPublication> attach_run_lineage_async(
    const ProgramTransitionStore& transitions, ProgramTransitionPublication publication) {
    if (publication.run_lineage) co_return publication;

    const auto& run = publication.run_record;
    auto current = co_await transitions.load_run_lineage(run.owner_scope(), run.run_id());
    const auto lineage_id = current ? current->lineage_id()
                                    : program_run_lineage_id(run.owner_scope(), run.run_id());
    if (!current) current = co_await transitions.load_lineage(run.owner_scope(), lineage_id);
    std::optional<ProgramRunGeneration> generation;
    if (current && is_retry_of_lineage_head(*current, run))
        generation = co_await transitions.load_generation(run.owner_scope(), lineage_id,
                                                          current->active_generation());
    co_return attach_run_lineage_from(std::move(publication), lineage_id, current, generation);
}

asio::awaitable<ProgramTransitionPublishResult> compare_publish_with_lineage_async(
    ProgramTransitionStore&       transitions,
    std::string_view              owner_scope,
    std::string_view              expected_journal_head,
    ProgramTransitionPublication publication) {
    auto attached = co_await attach_run_lineage_async(transitions, std::move(publication));
    co_return co_await transitions.compare_publish(owner_scope, expected_journal_head, std::move(attached));
}

struct ActiveRunLineage {
    ProgramRunLineage    lineage;
    ProgramRunGeneration generation;
};

// What load_active_run_lineage makes of the lineage and generation it read: the pair, or a diagnostic when the run is
// not the active generation of its lineage.
ActiveRunLineage bind_active_run_lineage(const ProgramRunRecord&          run,
                                         std::optional<ProgramRunLineage> lineage,
                                         std::optional<ProgramRunGeneration> generation) {
    if (!generation) {
        throw_runtime_diagnostic("P_RUN_LINEAGE",
                                 "Program run lineage has no active generation",
                                 json{{"run_id", run.run_id()},
                                      {"lineage_id", lineage->lineage_id()}});
    }
    if (lineage->active_run_record_id() != run.id() ||
        lineage->active_journal_head() != run.journal_head() ||
        !does_program_run_generation_bind(*generation, *lineage, run)) {
        throw_runtime_diagnostic(
            "P_RUN_GENERATION_STALE",
            "Program run is not the active generation of its lineage",
            json{{"run_id", run.run_id()},
                 {"lineage_id", lineage->lineage_id()},
                 {"active_generation", lineage->active_generation()},
                 {"active_run_id", generation->run_id()}});
    }
    return ActiveRunLineage{std::move(*lineage), std::move(*generation)};
}

asio::awaitable<std::optional<ActiveRunLineage>> load_active_run_lineage_async(
    const ProgramTransitionStore& transitions, const ProgramRunRecord& run) {
    auto lineage = co_await transitions.load_run_lineage(run.owner_scope(), run.run_id());
    if (!lineage) co_return std::nullopt;
    auto generation = co_await transitions.load_generation(run.owner_scope(), lineage->lineage_id(),
                                                           lineage->active_generation());
    co_return bind_active_run_lineage(run, std::move(lineage), std::move(generation));
}

bool fork_resume_matches(const ForkCompatibilityReceipt& receipt,
                         const ProgramRunRecord&         target,
                         const ProgramResume&            resume) {
    if (receipt.initial_resume_binding()) {
        return receipt.matches_initial_resume(resume.pending_id, resume.value);
    }
    if (receipt.storage_schema_version() >= ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
        return false;
    }

    // Legacy receipts did not retain the initial fork value. Preserve retries
    // only while the current snapshot still proves that exact consumed value.
    if (const auto pending = target.pending_input()) {
        return pending->state() == ProgramPendingState::Consumed &&
               pending->call_id() == resume.pending_id && pending->consumed_result() &&
               detail::canonical_json_bytes(*pending->consumed_result()) ==
                   detail::canonical_json_bytes(resume.value);
    }
    if (const auto pending = target.pending_effect()) {
        return pending->state() == ProgramPendingState::Consumed &&
               pending->call_id() == resume.pending_id && pending->reconciled_result() &&
               detail::canonical_json_bytes(*pending->reconciled_result()) ==
                   detail::canonical_json_bytes(resume.value);
    }
    return false;
}

RunInvocation bind_runtime_invocation(const ProgramInvocation& projection,
                                      std::string_view         owner_scope,
                                      std::string_view         program_version_id,
                                      std::string_view         run_id) {
    if (projection.canonical_request) {
        auto invocation = *projection.canonical_request;
        invocation.validate();
        if (invocation.owner_scope != owner_scope ||
            invocation.program_version_id != program_version_id || invocation.run_id != run_id ||
            invocation.input != projection.input || invocation.budget != projection.budget ||
            invocation.correlation_id != projection.trace_id ||
            invocation.parent_run_id != projection.parent_run_id) {
            throw_runtime_diagnostic(
                "P_INVOCATION_MISMATCH",
                "Canonical RunInvocation does not match its runtime-only projection");
        }
        return invocation;
    }

    RunInvocation invocation;
    invocation.owner_scope        = std::string(owner_scope);
    invocation.agent_id           = "neograph-program-runtime";
    invocation.program_version_id = std::string(program_version_id);
    invocation.run_id             = std::string(run_id);
    invocation.parent_run_id      = projection.parent_run_id;
    invocation.budget             = projection.budget;
    invocation.input              = projection.input;
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "runtime:" + invocation.run_id;
    invocation.correlation_id =
        projection.trace_id.empty() ? "runtime:" + invocation.run_id : projection.trace_id;
    invocation.validate();
    return invocation;
}

ProgramInvocation runtime_projection(RunInvocation                     invocation,
                                     std::shared_ptr<ProgramEventSink> events = {}) {
    ProgramInvocation projection;
    projection.input             = invocation.input;
    projection.budget            = invocation.budget;
    projection.trace_id          = invocation.correlation_id;
    projection.events            = std::move(events);
    projection.requested_run_id  = invocation.run_id;
    projection.canonical_request = std::move(invocation);
    return projection;
}

std::string core_thread_identity(std::string_view run_id, std::string_view generation_id) {
    return program_root_core_thread_id(run_id, generation_id);
}

std::string core_thread_identity(std::string_view run_id,
                                 std::string_view generation_id,
                                 std::string_view operation_id) {
    std::string identity(run_id);
    identity.push_back('\0');
    identity.append(operation_id);
    identity.push_back('\0');
    identity.append(generation_id);
    return detail::sha256_identity("program-core-thread/v1", identity);
}

// #405: for a run that is a fork, the source whose provider calls the fork
// repeats, as the fork's attempts replay them from the source's journal: the
// source's attempt at the fork is the one before the fork's first.
asio::awaitable<std::optional<detail::RunControl::ForkProviderReplay>> fork_provider_replay_of(
    ProgramTransitionStore& transitions, graph::CheckpointStore& checkpoints,
    std::string owner_scope, ProgramRunRecord record) {
    const auto source_run_id = record.fork_source_run_id();
    const auto source_checkpoint_id = record.fork_source_checkpoint_id();
    if (!source_run_id || !source_checkpoint_id) co_return std::nullopt;
    const auto forked = checkpoints.load_by_id(*source_checkpoint_id);
    if (!forked) co_return std::nullopt;
    const auto source = co_await transitions.load(owner_scope, *source_run_id);
    if (!source || !source->exact_checkpoint()) co_return std::nullopt;
    const auto events = co_await transitions.load_events(owner_scope, record.run_id());
    if (events.empty() || events.front().attempt == 0) co_return std::nullopt;
    const auto generation = source->exact_checkpoint()->core_generation_id;
    co_return detail::RunControl::ForkProviderReplay{
        source->run_id(), source->program_version_id(), generation,
        core_thread_identity(source->run_id(), generation), forked->thread_id,
        events.front().attempt - 1};
}

// #431: a fork on a host whose provider calls are not journaled has no record
// to replay the paused invocation's calls from, so a call the fork runs again
// is sent and charged again. The notice names the calls the paused attempt
// recorded in the paused operation: the runtime cannot tell which of them the
// fork runs again (a call of a node that finished in the paused step is
// replayed from the step's saved writes). It is given only when the paused
// attempt recorded such a call.
std::vector<ProgramNotice> fork_provider_call_notices(bool journaled,
                                                      const ProgramRunRecord& source) {
    const auto terminal = source.terminal_result();
    if (journaled || !terminal) return {};
    std::string operation;
    if (const auto input = source.pending_input())
        operation = input->operation_id();
    else if (const auto effect = source.pending_effect())
        operation = effect->operation_id();
    json          calls = json::array();
    std::uint64_t microunits = 0;
    for (const auto& record : terminal->monetary_records()) {
        if (record.operation_id != operation) continue;
        calls.push_back(json{{"call_id", record.call_id},
                             {"node", record.node},
                             {"model", record.model},
                             {"microunits", record.microunits}});
        microunits += record.microunits;
    }
    if (calls.empty()) return {};
    const auto count = calls.size();
    json       witness{{"source_run_id", source.run_id()},
                       {"source_attempt", terminal->attempt()},
                       {"operation_id", operation},
                       {"calls", std::move(calls)},
                       {"microunits", microunits}};
    return {ProgramNotice{
        "P_FORK_PROVIDER_CALLS_NOT_JOURNALED",
        "This host keeps no durable provider-call journal, so the fork cannot replay provider "
        "calls: a call that the fork runs again is sent and charged again. The paused attempt "
        "recorded " + std::to_string(count) + " provider call(s) in operation " + operation + ".",
        std::move(witness)}};
}

// #398: the run a recorded result was written by, which its provider
// evidence is bound to (owner scope aside).
struct RecordCustody {
    std::string run_id;
    std::string program_version_id;
    std::string bundle_id;
};

// #398: a recorded terminal result (a JavaScript command's or join member's
// result, or a plan branch's settled result) whose failure carries provider
// evidence holds it under the custody of the run that wrote it. A run that
// continues another (a fork, a graph migration) replays such records under
// its own custody, so it re-binds each one: it restores the evidence under
// the source run's custody, which must match exactly, and stores it again
// under its own. A record without provider evidence is unchanged.
void rebind_recorded_failure(json& result, std::string_view owner_scope,
                             const RecordCustody& source, const RecordCustody& target,
                             const std::shared_ptr<sp::NativeArchive>& archive) {
    if (!result.is_object() || !result.contains("failure")) return;
    auto failure_record = detail::owned_json_copy(result["failure"]);
    if (!failure_record.is_object() || !failure_record.contains("witness")) return;
    ProgramFailure failure;
    failure.witness = detail::owned_json_copy(failure_record["witness"]);
    if (!detail::has_provider_outcome(failure)) return;
    if (!failure.witness.contains("provider_outcome_custody") ||
        !failure.witness["provider_outcome_custody"].is_object())
        throw_runtime_diagnostic("P_FORK_CUSTODY",
                                 "A recorded provider failure has no custody to re-bind");
    const auto stored = detail::owned_json_copy(failure.witness["provider_outcome_custody"]);
    if (!stored.contains("operation_id") || !stored["operation_id"].is_string() ||
        !stored.contains("attempt") || !stored["attempt"].is_number_unsigned())
        throw_runtime_diagnostic("P_FORK_CUSTODY",
                                 "A recorded provider failure's custody is malformed");
    const auto operation_id = stored["operation_id"].get<std::string>();
    const auto attempt      = stored["attempt"].get<std::uint64_t>();
    try {
        detail::restore_provider_failure(
            failure,
            detail::provider_failure_custody(owner_scope, source.run_id, source.program_version_id,
                                             source.bundle_id, operation_id, attempt),
            archive);
    } catch (const std::invalid_argument& error) {
        throw_runtime_diagnostic("P_FORK_CUSTODY",
                                 std::string("A recorded provider failure is not the source "
                                             "run's: ") + error.what());
    }
    detail::persist_provider_failure(
        failure,
        detail::provider_failure_custody(owner_scope, target.run_id, target.program_version_id,
                                         target.bundle_id, operation_id, attempt),
        archive);
    failure_record["witness"] = std::move(failure.witness);
    result["failure"]         = std::move(failure_record);
}

// #398: the recorded results of a paused join's settled members (#380), each
// re-bound as above.
json rebind_recorded_members(const json& members, std::string_view owner_scope,
                             const RecordCustody& source, const RecordCustody& target,
                             const std::shared_ptr<sp::NativeArchive>& archive) {
    json rebound = json::object();
    for (const auto& [member, stored] : members.items()) {
        auto record = detail::owned_json_copy(stored);
        rebind_recorded_failure(record, owner_scope, source, target, archive);
        rebound[member] = std::move(record);
    }
    return rebound;
}

// #386: the replay records of a paused plan (settled_branches) that a run
// continuing `source` in another run, `run_id` of `target`, carries. A
// paused call's record names its checkpoint in the source run; the
// continuation gets its own copy, on the call's Core thread in the new run,
// so that answering the call there never writes into the source run.
// A settled branch's provider evidence is re-bound to the new run (#398).
std::optional<json> continued_replay_records(const ProgramRunRecord&         source,
                                             std::string_view                run_id,
                                             const detail::MaterializedProgram& target,
                                             graph::CheckpointStore&         checkpoints,
                                             const std::shared_ptr<sp::NativeArchive>& archive) {
    const auto records = source.settled_branches();
    if (!records) return std::nullopt;
    const RecordCustody source_custody{source.run_id(), source.program_version_id(),
                                       source.bundle_id()};
    const RecordCustody target_custody{std::string(run_id), target.version.id(),
                                       target.bundle.id()};
    json carried = json::object();
    for (const auto& [key, stored] : records->items()) {
        auto record = detail::owned_json_copy(stored);
        if (record.is_object() && record.contains("kind") && record["kind"] == json("settled") &&
            record.contains("result")) {
            auto result = detail::owned_json_copy(record["result"]);
            rebind_recorded_failure(result, source.owner_scope(), source_custody, target_custody,
                                    archive);
            record["result"] = std::move(result);
        }
        if (record.is_object() && record.contains("kind") && record["kind"] == json("paused")) {
            const auto checkpoint_id = record["checkpoint"]["checkpoint_id"].get<std::string>();
            const auto loaded        = checkpoints.load_by_id(checkpoint_id);
            if (!loaded || loaded->id != checkpoint_id)
                throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND",
                                         "A paused call's checkpoint was not found");
            auto copy      = *loaded;
            copy.id        = graph::Checkpoint::generate_id();
            copy.thread_id = core_thread_identity(run_id, target.root->compiled_plan_identity,
                                                  record["operation_id"].get<std::string>());
            copy.parent_id = loaded->id;
            copy.metadata["forked_from"] =
                json{{"thread_id", loaded->thread_id}, {"checkpoint_id", loaded->id}};
            copy.timestamp = std::max<std::int64_t>(now_ms(), loaded->timestamp + 1);
            checkpoints.save(copy);
            // #390: the copy replays what the call's paused step finished.
            graph::copy_resume_pending_writes(checkpoints, *loaded, copy);
            record["checkpoint"] = json{{"core_name", target.root->core_name},
                                        {"core_generation_id", target.root->compiled_plan_identity},
                                        {"core_thread_id", copy.thread_id},
                                        {"checkpoint_id", copy.id},
                                        {"checkpoint_schema_version", copy.schema_version}};
        }
        carried[key] = std::move(record);
    }
    return carried;
}

std::optional<std::string> javascript_call_core_operation_for_thread(
    const JavaScriptCommand& command,
    std::string              operation_id,
    std::string_view         run_id,
    std::string_view         generation_id,
    std::string_view         thread_id,
    std::size_t              depth = 0) {
    if (depth > 32) return std::nullopt;
    if (command.kind() == JavaScriptCommandKind::CallCore) {
        return core_thread_identity(run_id, generation_id, operation_id) == thread_id
                   ? std::optional<std::string>{std::move(operation_id)}
                   : std::nullopt;
    }
    const auto arguments = command.arguments();
    if (command.kind() == JavaScriptCommandKind::Await) {
        return javascript_call_core_operation_for_thread(
            JavaScriptCommand::from_json(arguments.at("command")), operation_id + "/await", run_id,
            generation_id, thread_id, depth + 1);
    }
    if (command.kind() == JavaScriptCommandKind::Join) {
        const auto& members = arguments.at("members");
        for (std::size_t index = 0; index < members.size(); ++index) {
            auto found = javascript_call_core_operation_for_thread(
                JavaScriptCommand::from_json(members.at(index)),
                operation_id + "/member/" + std::to_string(index), run_id, generation_id, thread_id,
                depth + 1);
            if (found) return found;
        }
    }
    return std::nullopt;
}

std::optional<JavaScriptCommand> javascript_command_at_operation(
    const JavaScriptCommand& command,
    std::string              operation_id,
    std::string_view         target_operation_id,
    std::size_t              depth = 0) {
    if (depth > 32) return std::nullopt;
    if (operation_id == target_operation_id) return command;
    const auto arguments = command.arguments();
    if (command.kind() == JavaScriptCommandKind::Await) {
        return javascript_command_at_operation(
            JavaScriptCommand::from_json(arguments.at("command")), operation_id + "/await",
            target_operation_id, depth + 1);
    }
    if (command.kind() == JavaScriptCommandKind::Join) {
        const auto& members = arguments.at("members");
        for (std::size_t index = 0; index < members.size(); ++index) {
            auto found = javascript_command_at_operation(
                JavaScriptCommand::from_json(members.at(index)),
                operation_id + "/member/" + std::to_string(index), target_operation_id, depth + 1);
            if (found) return found;
        }
    }
    return std::nullopt;
}

struct GlobalChildQuotaState {
    explicit GlobalChildQuotaState(ProgramChildQuotaConfig limits_value) : limits(limits_value) {}

    ProgramChildQuotaConfig                        limits;
    detail::PureMutex                              mutex;
    std::uint64_t                                  active_children = 0;
    std::uint64_t                                  pending_spawns  = 0;
    std::unordered_map<std::string, std::uint64_t> active_by_owner;
    std::unordered_map<std::string, std::uint64_t> pending_by_owner;
};

struct ChildQuotaReservation {
    std::shared_ptr<GlobalChildQuotaState> state;
    std::string                            owner_scope;
    bool                                   pending = false;
    bool                                   active  = false;

    bool activate() {
        if (!pending || active) return false;
        std::lock_guard lock(state->mutex);
        const auto&     limits = state->limits;
        const auto      owner_active =
            state->active_by_owner.find(owner_scope) == state->active_by_owner.end()
                     ? 0
                     : state->active_by_owner.at(owner_scope);
        if (limits.max_active_children != 0 && state->active_children >= limits.max_active_children)
            return false;
        if (limits.max_active_children_per_owner != 0 &&
            owner_active >= limits.max_active_children_per_owner)
            return false;
        --state->pending_spawns;
        auto pending_it = state->pending_by_owner.find(owner_scope);
        if (pending_it != state->pending_by_owner.end() && --pending_it->second == 0)
            state->pending_by_owner.erase(pending_it);
        ++state->active_children;
        ++state->active_by_owner[owner_scope];
        pending = false;
        active  = true;
        return true;
    }

    void release() noexcept {
        if (!state || (!pending && !active)) return;
        std::lock_guard lock(state->mutex);
        if (pending) {
            if (state->pending_spawns != 0) --state->pending_spawns;
            auto it = state->pending_by_owner.find(owner_scope);
            if (it != state->pending_by_owner.end() && it->second != 0 && --it->second == 0)
                state->pending_by_owner.erase(it);
            pending = false;
        }
        if (active) {
            if (state->active_children != 0) --state->active_children;
            auto it = state->active_by_owner.find(owner_scope);
            if (it != state->active_by_owner.end() && it->second != 0 && --it->second == 0)
                state->active_by_owner.erase(it);
            active = false;
        }
    }

    ~ChildQuotaReservation() { release(); }
};

// The remaining budget that the journal holds beneath the reservation of a JavaScript command, for an attempt that ends
// before it has begun (see detail::held_command_remainder): the attempt has run nothing, so the reservation is neither
// settled nor returned. Nothing when the journal holds none, and when the store cannot be read: the end of the attempt
// then keeps the grant, as it did, and its publication fails closed.
asio::awaitable<std::optional<RunBudget>> held_remainder_of_unstarted_attempt(const detail::RunControl& control) {
    try {
        co_return co_await detail::held_command_remainder(control);
    } catch (...) {
        co_return std::nullopt;
    }
}

// An attempt whose first event, Started, the observer rejected: no node of it has run. The run ends Failed with the code
// of the observer's error and `remaining`, the budget the resume or the reconciliation left it. The reservation of a
// JavaScript callCore that the journal holds is not part of `remaining` and is not returned, since nothing has measured
// its use.
asio::awaitable<void> complete_attempt_event_sink_rejected(const std::shared_ptr<detail::RunControl>& control,
                                                           RunBudget                                   remaining,
                                                           CoreCheckpointIdentity                      checkpoint,
                                                           std::string                                 message) {
    detail::RunOutcome failed;
    failed.remaining_budget = std::move(remaining);
    failed.checkpoint       = std::move(checkpoint);
    failed.status           = ProgramTerminalStatus::Failed;
    failed.failure = ProgramFailure{"P_EVENT_SINK", std::move(message), "root", "", 0, json::object()};
    co_await control->complete_async(std::move(failed));
}

// The terminal publications below await the transition store; none of them throws. An attempt that has not begun
// (`attempt_unstarted`) ends with the remainder beneath the reservation of a command that its journal holds, not with its
// grant, which includes the reservation.
asio::awaitable<void> complete_attempt_launch_failure(std::shared_ptr<detail::RunControl> control,
                                                      std::optional<std::string>          checkpoint_id,
                                                      std::exception_ptr                  error,
                                                      bool                                attempt_unstarted = false) {
    std::string message = "Program execution coroutine failed";
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exception) {
            message += ": ";
            try {
                detail::validate_utf8(exception.what());
                message += exception.what();
            } catch (const std::invalid_argument&) {
                message += "exception message contained non-UTF-8 bytes";
            }
        } catch (...) {
            message += " with a non-standard exception";
        }
    }

    std::optional<RunBudget> held;
    if (attempt_unstarted) held = co_await held_remainder_of_unstarted_attempt(*control);

    detail::RunOutcome outcome;
    outcome.status           = ProgramTerminalStatus::Failed;
    outcome.remaining_budget = held ? *held : control->granted_budget;

    outcome.usage.program_operations = control->attempt == 1 ? 1 : 0;
    if (control->attempt == 1 && !held) outcome.remaining_budget.max_program_operations = 0;
    outcome.failure =
        ProgramFailure{"P_RUNTIME_CORE_FAILURE", std::move(message), "root", "", 0, json::object()};
    if (checkpoint_id) {
        outcome.checkpoint = CoreCheckpointIdentity{
            control->materialized->root->core_name,
            control->materialized->root->compiled_plan_identity, control->core_thread_id,
            *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
    }
    co_await control->complete_async(std::move(outcome));
}

std::optional<CoreCheckpointIdentity> attempt_checkpoint(
    const std::shared_ptr<detail::RunControl>& control,
    const std::optional<std::string>&          checkpoint_id) {
    if (!checkpoint_id) return std::nullopt;
    return CoreCheckpointIdentity{
        control->materialized->root->core_name, control->materialized->root->compiled_plan_identity,
        control->core_thread_id, *checkpoint_id, graph::CHECKPOINT_SCHEMA_VERSION};
}

asio::awaitable<void> complete_attempt_admission_cancelled(
    std::shared_ptr<detail::RunControl> control,
    std::optional<std::string>          checkpoint_id,
    bool                                attempt_unstarted) {
    const auto cause = control->seal_terminal_cause();
    if (cause == detail::CancellationCause::None) {
        co_await complete_attempt_launch_failure(
            control, checkpoint_id,
            std::make_exception_ptr(
                std::runtime_error("Host admission stopped without a Program cancellation cause")),
            attempt_unstarted);
        co_return;
    }

    std::optional<RunBudget> held;
    if (attempt_unstarted) held = co_await held_remainder_of_unstarted_attempt(*control);
    detail::RunOutcome outcome;
    outcome.remaining_budget = held ? *held : control->granted_budget;
    outcome.checkpoint       = attempt_checkpoint(control, checkpoint_id);
    if (cause == detail::CancellationCause::Timeout) {
        outcome.status = ProgramTerminalStatus::TimedOut;
        outcome.failure =
            ProgramFailure{"P_RUNTIME_TIMEOUT",
                           "Program wall-time budget expired while waiting for host capacity",
                           "root",
                           "",
                           0,
                           json::object()};
    } else if (cause == detail::CancellationCause::EventSink) {
        outcome.status  = ProgramTerminalStatus::Failed;
        outcome.failure = ProgramFailure{
            "P_EVENT_SINK", "Program event sink failed before host admission", "root", "", 0,
            json::object()};
    } else {
        outcome.status = ProgramTerminalStatus::Cancelled;
        outcome.failure =
            ProgramFailure{"P_RUNTIME_CANCELLED",
                           "Program attempt was cancelled while waiting for host capacity",
                           "root",
                           "",
                           0,
                           json::object()};
    }
    co_await control->complete_async(std::move(outcome));
}

asio::awaitable<void> complete_attempt_host_admission_failure(std::shared_ptr<detail::RunControl> control,
                                                              std::optional<std::string>          checkpoint_id,
                                                              HostAdmissionError                  error,
                                                              bool                                attempt_unstarted) {
    std::optional<RunBudget> held;
    if (attempt_unstarted) held = co_await held_remainder_of_unstarted_attempt(*control);
    detail::RunOutcome outcome;
    outcome.status           = ProgramTerminalStatus::Failed;
    outcome.remaining_budget = held ? *held : control->granted_budget;
    outcome.checkpoint       = attempt_checkpoint(control, checkpoint_id);
    outcome.failure          = ProgramFailure{"P_HOST_ADMISSION",
                                     error.what(),
                                     "root",
                                     "",
                                     0,
                                     json{{"failure", std::string(to_string(error.failure()))}}};
    co_await control->complete_async(std::move(outcome));
}

HostAdmissionRequest make_host_admission_request(const std::shared_ptr<detail::RunControl>& control,
                                                 const ProgramHostAdmissionResolver& resolver) {
    const ProgramHostAdmissionContext context{control->owner_scope,
                                              control->program_version_id,
                                              control->run_id,
                                              control->operation_id,
                                              control->attempt,
                                              control->granted_budget,
                                              control->persisted_invocation.child_depth};
    auto                              request = resolver(context);

    const auto now = std::chrono::steady_clock::now();
    if (now >= control->deadline) {
        throw HostAdmissionError(HostAdmissionFailure::QueueTimeout,
                                 "Program deadline elapsed before host admission");
    }
    const auto remaining =
        std::max(std::chrono::milliseconds{1},
                 std::chrono::duration_cast<std::chrono::milliseconds>(control->deadline - now));
    if (request.queue_timeout <= std::chrono::milliseconds::zero() ||
        request.queue_timeout > remaining) {
        request.queue_timeout = remaining;
    }

    // The policy selects capacity and scheduling hints, never a mutable
    // identity.  Per-attempt identities keep accounting and priority
    // inheritance isolated across concurrent Program runs.
    request.owner_scope = control->owner_scope;
    request.operation_id =
        "program/" + control->run_id + "/attempt/" + std::to_string(control->attempt);
    return request;
}

std::shared_ptr<async::HostTimer> arm_host_admission_deadline(
    const std::shared_ptr<detail::RunControl>& control) {
    auto timer = std::make_shared<async::HostTimer>(control->deadline_executor);
    timer->arm_at(control->deadline);
    asio::co_spawn(
        control->deadline_executor,
        [control, timer]() -> asio::awaitable<void> {
            asio::error_code error;
            co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
            if (!error) (void)co_await control->cancel_async(detail::CancellationCause::Timeout);
            co_return;
        },
        asio::detached);
    return timer;
}

void cancel_host_admission_deadline(const std::shared_ptr<detail::RunControl>& control,
                                    std::shared_ptr<async::HostTimer>          timer) noexcept {
    try {
        asio::post(control->deadline_executor, [timer = std::move(timer)] { timer->cancel(); });
    } catch (...) {}
}

asio::awaitable<void> execute_host_admitted_attempt(
    std::shared_ptr<detail::RunControl>      control,
    json                                     input,
    std::optional<std::string>               checkpoint_id,
    std::shared_ptr<HostAdmissionController> host_admission,
    ProgramHostAdmissionResolver             resolver,
    std::shared_ptr<async::HostTimer>        deadline_wait,
    std::optional<json>                      javascript_core_resume_value) {
    const auto stop_deadline = [&] {
        if (deadline_wait) cancel_host_admission_deadline(control, std::move(deadline_wait));
    };
    // A handler cannot suspend: it only notes how the attempt stopped, and the terminal publication follows it.
    enum class Stopped { Cancelled, QueueTimeout, AdmissionFailed, LaunchFailed } stopped = Stopped::Cancelled;
    std::optional<HostAdmissionError> admission_error;
    std::exception_ptr                launch_error;
    // Once the attempt has begun, what stops it is not an admission outcome and the run has done work: the publications
    // below then keep the grant of the attempt, as they always did.
    bool attempt_started = false;
    try {
        if (control->cancel_token->is_cancelled()) {
            stop_deadline();
            co_await complete_attempt_admission_cancelled(control, checkpoint_id, true);
            co_return;
        }

        auto         request = make_host_admission_request(control, resolver);
        auto         lease =
            co_await host_admission->reserve_async(std::move(request), control->cancel_token);
        if (!lease.held()) {
            throw std::logic_error("Host admission granted an empty Program resource lease");
        }
        auto held_lease = std::make_shared<HostResourceLease>(std::move(lease));
        control->add_terminal_cleanup([held_lease] { held_lease->release(); });
        stop_deadline();

        if (control->cancel_token->is_cancelled()) {
            co_await complete_attempt_admission_cancelled(control, checkpoint_id, true);
            co_return;
        }
        attempt_started = true;
        co_await detail::execute_run_attempt(control, std::move(input), checkpoint_id,
                                             std::move(javascript_core_resume_value));
        co_return;
    } catch (const graph::CancelledException&) {
        stopped = Stopped::Cancelled;
    } catch (const HostAdmissionError& error) {
        admission_error = error;
        stopped = error.failure() == HostAdmissionFailure::QueueTimeout ? Stopped::QueueTimeout
                                                                        : Stopped::AdmissionFailed;
    } catch (...) {
        // The host's resolver threw, or the lease was empty: the attempt cannot be launched, which is the one outcome
        // left to an attempt that has not begun. One that has begun fails its launch where the coroutine ends.
        if (attempt_started) throw;
        launch_error = std::current_exception();
        stopped      = Stopped::LaunchFailed;
    }
    stop_deadline();
    if (stopped == Stopped::QueueTimeout) (void)co_await control->cancel_async(detail::CancellationCause::Timeout);
    if (stopped == Stopped::LaunchFailed)
        co_await complete_attempt_launch_failure(control, checkpoint_id, launch_error, !attempt_started);
    else if (stopped == Stopped::AdmissionFailed)
        co_await complete_attempt_host_admission_failure(control, checkpoint_id, *admission_error, !attempt_started);
    else
        co_await complete_attempt_admission_cancelled(control, checkpoint_id, !attempt_started);
}

// The attempt's coroutine failed: close the run, on the attempts executor, awaiting the terminal publication.
void launch_failure_after(const std::shared_ptr<const detail::RuntimeScheduler>& scheduler,
                          const std::shared_ptr<detail::RunControl>&             control,
                          const std::optional<std::string>&                      checkpoint_id,
                          std::exception_ptr                                     error) noexcept {
    try {
        asio::co_spawn(scheduler->attempts, complete_attempt_launch_failure(control, checkpoint_id, error),
                       asio::detached);
    } catch (...) {
    }
}

// `keepalive` is the runtime's state: the attempt may outlive the ProgramRuntime object on an injected executor, and
// the runtime's callbacks that it calls need that state.
void spawn_run_attempt(const std::shared_ptr<const detail::RuntimeScheduler>& scheduler,
                       std::shared_ptr<void>                      keepalive,
                       const std::shared_ptr<detail::RunControl>& control,
                       json                                       input,
                       std::optional<std::string>                 checkpoint_id,
                       std::shared_ptr<HostAdmissionController>   host_admission          = {},
                       ProgramHostAdmissionResolver               host_admission_resolver = {},
                       std::optional<json> javascript_core_resume_value = std::nullopt) noexcept {
    try {
        if (host_admission) {
            auto deadline_wait = arm_host_admission_deadline(control);
            asio::co_spawn(scheduler->attempts,
                           execute_host_admitted_attempt(
                               control, std::move(input), checkpoint_id, std::move(host_admission),
                               std::move(host_admission_resolver), std::move(deadline_wait),
                               std::move(javascript_core_resume_value)),
                           [scheduler, control, checkpoint_id, keepalive](std::exception_ptr error) {
                               if (error) launch_failure_after(scheduler, control, checkpoint_id, error);
                           });
            return;
        }
        asio::co_spawn(scheduler->attempts,
                       detail::execute_run_attempt(control, std::move(input), checkpoint_id,
                                                   std::move(javascript_core_resume_value)),
                       [scheduler, control, checkpoint_id, keepalive](std::exception_ptr error) {
                           if (error) launch_failure_after(scheduler, control, checkpoint_id, error);
                       });
    } catch (...) {
        launch_failure_after(scheduler, control, checkpoint_id, std::current_exception());
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
                         bool                               allow_child_metadata = false) {
    if (!allow_child_metadata && (!invocation.parent_run_id.empty() || invocation.child_depth != 0))
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
    const auto& budget = invocation.budget;
    const bool  has_dynamic_expansion =
        materialized.bundle.program_schema_version() >= PROGRAM_SCHEMA_VERSION_V4 &&
        std::any_of(materialized.bundle.typed_orchestration_plan().nodes().begin(),
                    materialized.bundle.typed_orchestration_plan().nodes().end(),
                    [](const auto& node) {
                        return node.operation() == ProgramOperationKind::ExpandTaskGraph;
                    });
    constexpr auto max_safe_wall_time_ms =
        static_cast<std::uint64_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) / 2;
    // Name the first rule the invocation budget breaks.
    std::string invalid;
    if (budget.wall_time_ms == 0)
        invalid = "wall_time_ms must be positive";
    else if (budget.wall_time_ms > max_safe_wall_time_ms)
        invalid = "wall_time_ms exceeds " + std::to_string(max_safe_wall_time_ms);
    else if (budget.max_concurrency == 0)
        invalid = "max_concurrency must be positive";
    else if (budget.max_program_operations == 0)
        invalid = "max_program_operations must be positive";
    else if (budget.max_core_steps == 0)
        invalid = "max_core_steps must be positive";
    else if (budget.max_core_steps > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        invalid = "max_core_steps exceeds " + std::to_string(std::numeric_limits<int>::max());
    else if (!has_dynamic_expansion && budget.max_dynamic_compiles != 0 &&
             !materialized.bundle.control_source())
        invalid = "max_dynamic_compiles must be 0 unless the Program uses JavaScript control "
                  "or expand_task_graph";
    else if (has_dynamic_expansion && budget.max_dynamic_compiles == 0)
        invalid = "max_dynamic_compiles must be positive for a Program that uses "
                  "expand_task_graph";
    else if (budget.max_child_depth > MAX_SUPPORTED_CHILD_DEPTH)
        invalid = "max_child_depth exceeds " + std::to_string(MAX_SUPPORTED_CHILD_DEPTH);
    else if ((budget.max_child_depth == 0) != (budget.max_total_children == 0))
        invalid = "max_child_depth and max_total_children must be enabled together";
    if (!invalid.empty()) {
        throw_runtime_diagnostic("P_START_BUDGET", "Program invocation budget is invalid: " + invalid,
                                 json{{"rule", invalid}});
    }

    const auto ceiling = materialized.version.policy_snapshot().budget_ceiling();
    for (const auto& requirement : materialized.bundle.declared_budget_requirements()) {
        const auto granted = budget_value(budget, requirement.resource);
        const auto allowed = ceiling_value(ceiling, requirement.resource);
        if (granted < requirement.minimum || granted > requirement.maximum || granted > allowed) {
            throw_runtime_diagnostic("P_START_BUDGET",
                                     "Program invocation budget " + requirement.resource + "=" +
                                         std::to_string(granted) +
                                         " is outside its admitted bounds [" +
                                         std::to_string(requirement.minimum) + ", " +
                                         std::to_string(requirement.maximum) +
                                         "] (policy ceiling " + std::to_string(allowed) + ")",
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

template <class T>
bool contains_all_values(const std::vector<T>& available, const std::vector<T>& requested) {
    return std::all_of(requested.begin(), requested.end(), [&](const auto& value) {
        return std::find(available.begin(), available.end(), value) != available.end();
    });
}

bool replacement_authority_is_attenuated(const detail::MaterializedProgram& source,
                                         const detail::MaterializedProgram& target) {
    const auto source_profile = source.version.admission_profile();
    const auto target_profile = target.version.admission_profile();
    const auto source_policy  = source.version.policy_snapshot();
    const auto target_policy  = target.version.policy_snapshot();
    const auto& source_closure = source.bundle.capability_effect_closure();
    const auto& target_closure = target.bundle.capability_effect_closure();

    return source.version.ownership_scope() == target.version.ownership_scope() &&
           execution_guarantee_rank(target.version.execution_guarantee()) >=
               execution_guarantee_rank(source.version.execution_guarantee()) &&
           execution_guarantee_rank(target_profile.minimum_execution_guarantee()) >=
               execution_guarantee_rank(source_profile.minimum_execution_guarantee()) &&
           execution_guarantee_rank(target_policy.minimum_execution_guarantee()) >=
               execution_guarantee_rank(source_policy.minimum_execution_guarantee()) &&
           source_profile.mode() == target_profile.mode() &&
           source_profile.registry_fingerprint() == target_profile.registry_fingerprint() &&
           target_profile.max_program_schema_version() <=
               source_profile.max_program_schema_version() &&
           contains_all_values(source_profile.allowed_source_kinds(),
                               target_profile.allowed_source_kinds()) &&
           contains_all_values(source_profile.allowed_executables(),
                               target_profile.allowed_executables()) &&
           contains_all_values(source_profile.allowed_effect_modes(),
                               target_profile.allowed_effect_modes()) &&
           source_policy.owner_scope() == target_policy.owner_scope() &&
           source_policy.registry_fingerprint() == target_policy.registry_fingerprint() &&
           contains_all(source_policy.allowed_capabilities(),
                        target_policy.allowed_capabilities()) &&
           contains_all(source_policy.allowed_effects(), target_policy.allowed_effects()) &&
           contains_all(source_policy.allowed_module_digests(),
                        target_policy.allowed_module_digests()) &&
           contains_all(source_closure.capabilities, target_closure.capabilities) &&
           contains_all(source_closure.effects, target_closure.effects) &&
           contains_all_values(source.bundle.executable_registry_identities(),
                               target.bundle.executable_registry_identities()) &&
           contains_all_values(source.version.core_materialization_receipt().capability_bindings,
                               target.version.core_materialization_receipt().capability_bindings);
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
    auto result            = budget;
    result.max_child_depth = 0;
    if (result.max_total_children == std::numeric_limits<std::uint64_t>::max())
        throw_runtime_diagnostic("P_CHILD_BUDGET", "Child budget exceeds total-child range");
    ++result.max_total_children;
    return result;
}

bool budget_sum_fits(const RunBudget& parent, const RunBudget& used, const RunBudget& additional) {
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
           fits(parent.max_total_children, used.max_total_children, additional.max_total_children);
}

RunBudget restore_reserved_resources(const RunBudget& available, const RunBudget& reservation) {
    RunBudget  restored    = available;
    const auto add_checked = [](auto current, auto reserved, std::string_view resource) {
        if (reserved > std::numeric_limits<decltype(current)>::max() - current) {
            throw_runtime_diagnostic(
                "P_JS_COMMAND_BUDGET",
                "JavaScript command reservation overflows the recoverable budget",
                json{{"resource", resource}});
        }
        return static_cast<decltype(current)>(current + reserved);
    };
    restored.wall_time_ms =
        add_checked(restored.wall_time_ms, reservation.wall_time_ms, "wall_time_ms");
    restored.model_tokens =
        add_checked(restored.model_tokens, reservation.model_tokens, "model_tokens");
    restored.monetary_microunits = add_checked(
        restored.monetary_microunits, reservation.monetary_microunits, "monetary_microunits");
    restored.max_program_operations =
        add_checked(restored.max_program_operations, reservation.max_program_operations,
                    "max_program_operations");
    restored.max_core_steps =
        add_checked(restored.max_core_steps, reservation.max_core_steps, "max_core_steps");
    return restored;
}

void validate_child_link(const detail::MaterializedProgram& materialized,
                         const ModuleLinkReceipt&           link,
                         const ProgramVersion&              requested_version,
                         const ProgramInvocation&           invocation,
                         const RunBudget&                   parent_budget,
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
    if (execution_guarantee_rank(requested_version.execution_guarantee()) <
        execution_guarantee_rank(link.minimum_execution_guarantee())) {
        throw_runtime_diagnostic(
            "P_CHILD_GUARANTEE", "Child execution guarantee falls below the immutable link floor",
            json{{"child", std::string(to_string(requested_version.execution_guarantee()))},
                 {"minimum", std::string(to_string(link.minimum_execution_guarantee()))}});
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
    if (parent_budget.max_child_depth == 0)
        throw_runtime_diagnostic("P_CHILD_DEPTH", "Child depth budget is exhausted");
    const auto remaining_depth = parent_budget.max_child_depth - 1;
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
std::string child_run_id_for(std::string_view         parent_run_id,
                             const ModuleLinkReceipt& link,
                             const ProgramInvocation& invocation) {
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
    return std::all_of(plan.nodes().begin(), plan.nodes().end(), [](const ProgramPlanNode& node) {
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

struct JavaScriptPendingCommand {
    std::uint64_t     ordinal = 0;
    JavaScriptCommand command =
        JavaScriptCommand::call_core("runtime:pending", "runtime:pending", json::object());
    std::string                bundle_id;
    std::string                effect_identity;
    std::optional<std::string> core_resume_operation_id;
};

std::optional<JavaScriptPendingCommand> javascript_pending_command(
    const ProgramPendingEffect& pending) {
    if (pending.core_node() != "javascript") return std::nullopt;
    const auto payload = pending.payload();
    if (!payload.is_object() || !payload.contains("bundle_id") ||
        !payload.at("bundle_id").is_string() || !payload.contains("command_ordinal") ||
        !payload.at("command_ordinal").is_number_unsigned() || !payload.contains("command") ||
        !payload.contains("effect_identity") || !payload.at("effect_identity").is_string()) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "JavaScript pending effect does not carry a complete command coordinate");
    }
    const auto bundle_id       = payload.at("bundle_id").get<std::string>();
    const auto ordinal         = payload.at("command_ordinal").get<std::uint64_t>();
    const auto command         = JavaScriptCommand::from_json(payload.at("command"));
    const auto effect_identity = payload.at("effect_identity").get<std::string>();
    if (bundle_id.empty() || ordinal == 0 || !detail::is_sha256_identity(effect_identity)) {
        throw_runtime_diagnostic("P_JS_COMMAND_JOURNAL_MISMATCH",
                                 "JavaScript pending effect carries an invalid command coordinate");
    }
    const auto expected_operation_id = "root.javascript." + std::to_string(ordinal);
    if (pending.operation_id() != expected_operation_id || effect_identity != pending.effect_id() ||
        effect_identity != pending.call_id() ||
        detail::canonical_json_bytes(command.to_json()) !=
            detail::canonical_json_bytes(pending.core_interrupt_value())) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "JavaScript pending effect coordinate disagrees with its command payload");
    }
    const auto expected_effect_identity =
        detail::sha256_identity("program-javascript-command-effect/v1",
                                detail::canonical_json_bytes(json{{"bundle_id", bundle_id},
                                                                  {"command_ordinal", ordinal},
                                                                  {"command", command.to_json()}}));
    if (effect_identity != expected_effect_identity) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "JavaScript pending effect identity does not bind its command coordinate");
    }
    return JavaScriptPendingCommand{ordinal, command, bundle_id, effect_identity, std::nullopt};
}

json javascript_resume_terminal_result(const json& value, const RunBudget& consumed_reservation) {
    return json{{"status", "completed"},
                {"output", detail::owned_json_copy(value)},
                {"execution_trace", json::array()},
                {"usage", json{{"wall_time_ms", consumed_reservation.wall_time_ms},
                               {"model_tokens", consumed_reservation.model_tokens},
                               {"monetary_microunits", consumed_reservation.monetary_microunits},
                               {"program_operations", consumed_reservation.max_program_operations},
                               {"core_steps", consumed_reservation.max_core_steps},
                               {"peak_concurrency", 0U}}}};
}

ProgramJavaScriptCommandJournalEntry javascript_completion_entry(
    const std::vector<ProgramJavaScriptCommandJournalEntry>& entries,
    const JavaScriptPendingCommand&                          pending,
    const json&                                              result) {
    const auto found = std::find_if(entries.rbegin(), entries.rend(), [&](const auto& entry) {
        return entry.command_ordinal() == pending.ordinal;
    });
    if (found == entries.rend() || !found->pending() || found->bundle_id() != pending.bundle_id ||
        detail::canonical_json_bytes(found->command().to_json()) !=
            detail::canonical_json_bytes(pending.command.to_json()) ||
        found->effect_identity() != std::optional<std::string>(pending.effect_identity)) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "JavaScript pending command does not match the durable command journal");
    }
    return ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
        entries.empty() ? 1 : entries.back().sequence() + 1, found->bundle_id(), pending.ordinal,
        pending.command, pending.effect_identity, result});
}

ProgramChildRecord child_record_for(std::string_view         child_run_id,
                                    const ModuleLinkReceipt& link,
                                    const ProgramInvocation& invocation,
                                    ProgramChildState        state) {
    return ProgramChildRecord{
        std::string(child_run_id), link.id(), link.serialize_canonical(),
        ProgramPersistedInvocation{invocation.input, invocation.budget, invocation.trace_id,
                                   invocation.parent_run_id, invocation.child_depth},
        state};
}

std::optional<ProgramChildRecord> find_child(const ProgramRunRecord& record,
                                             std::string_view        child_run_id) {
    const auto children = record.children();
    const auto found    = std::find_if(children.begin(), children.end(), [&](const auto& child) {
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

std::vector<ProgramChildRecord> terminal_children(const std::vector<ProgramChildRecord>& children) {
    auto result = children;
    for (auto& child : result) {
        const bool in_flight = child.state == ProgramChildState::Publishing ||
                               child.state == ProgramChildState::Dispatched;
        if (in_flight) child.state = ProgramChildState::Cancelled;
        // An interrupted/ambiguous child carries a non-cancelled result while
        // it is resumable.  Parent cancellation supersedes that outcome, and
        // the cancelled join record must not retain a mismatched result.
        if (in_flight && child.terminal_result &&
            child.terminal_result->status() != ProgramTerminalStatus::Cancelled)
            child.terminal_result.reset();
    }
    return result;
}

// A parent run's child relation is one durable compare-and-swap chain.  Child
// dispatch, child completion, and parent terminal publication can run on
// different scheduler threads; serialize local writers for the same chain.
// Striping avoids a global bottleneck while harmlessly serializing collisions.
detail::OperationLock& child_relation_publication_mutex(const ProgramTransitionStore& transitions,
                                                        std::string_view owner_scope,
                                                        std::string_view parent_run_id) {
    static std::array<detail::OperationLock, 257> locks;
    const auto                         mix = [](std::size_t seed, std::size_t value) noexcept {
        return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
    };
    auto key = std::hash<const void*>{}(static_cast<const void*>(&transitions));
    key      = mix(key, std::hash<std::string_view>{}(owner_scope));
    key      = mix(key, std::hash<std::string_view>{}(parent_run_id));
    return locks[key % locks.size()];
}

asio::awaitable<std::optional<ProgramRunRecord>> load_active_agent_run(
    std::shared_ptr<ProgramTransitionStore> transitions,
    std::string                             owner,
    std::string                             run) {
    const auto lineage = co_await transitions->load_run_lineage(owner, run);
    if (!lineage) co_return co_await transitions->load(owner, run);
    const auto generation =
        co_await transitions->load_generation(owner, lineage->lineage_id(), lineage->active_generation());
    if (!generation || generation->id() != lineage->active_generation_id())
        throw_runtime_diagnostic("P_RUN_LINEAGE", "Agent has no active generation");
    co_return co_await transitions->load(owner, generation->run_id());
}

struct ChildRecordPublication {
    ProgramTransitionPublishResult  result = ProgramTransitionPublishResult::Conflict;
    std::optional<ProgramRunRecord> record;
};

asio::awaitable<ChildRecordPublication> publish_child_record(std::shared_ptr<detail::RunControl> parent,
                                                             ProgramChildRecord                 child);

asio::awaitable<ChildRecordPublication> publish_child_record_unlocked(
    std::string                             owner_scope,
    std::string                             parent_run_id,
    std::shared_ptr<ProgramTransitionStore> transitions,
    ProgramChildRecord                      child) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto previous = co_await load_active_agent_run(transitions, owner_scope, parent_run_id);
        if (!previous) co_return ChildRecordPublication{};
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
                co_return ChildRecordPublication{ProgramTransitionPublishResult::AlreadyPresent, previous};
            }
            // A duplicate dispatch can race the first caller after it has
            // advanced the durable relation.  Publishing is the pre-dispatch
            // state, so an already-dispatched/terminal child is an idempotent
            // success for this metadata-only operation; callers re-read the
            // child below and reuse its durable run instead of regressing it.
            if (child.state == ProgramChildState::Publishing) {
                co_return ChildRecordPublication{ProgramTransitionPublishResult::AlreadyPresent, previous};
            }
            if (!valid_child_state_transition(existing->state, child.state)) {
                throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                         "Durable child state transition is not monotonic",
                                         json{{"child_run_id", child.child_run_id},
                                              {"from", std::string(to_string(existing->state))},
                                              {"to", std::string(to_string(child.state))}});
            }
            const auto next_state  = child.state;
            const auto next_result = child.terminal_result;
            const auto next_generation = child.terminal_generation;
            child                  = *existing;
            child.state            = next_state;
            child.terminal_result  = next_result;
            child.terminal_generation  = next_generation;
        }
        if (previous->continuation().state != ContinuationState::Running) co_return ChildRecordPublication{};
        const auto previous_journal = co_await transitions->latest(owner_scope, previous->run_id());
        if (!previous_journal || previous_journal->id != previous->journal_head()) co_return ChildRecordPublication{};

        auto children = previous->children();
        if (existing) {
            auto found = std::find_if(children.begin(), children.end(), [&](const auto& value) {
                return value.child_run_id == child.child_run_id;
            });
            if (found != children.end()) *found = child;
        } else {
            children.push_back(child);
        }
        auto                 journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), previous->run_id(), previous->program_version_id(),
            previous->bundle_id(), previous_journal->sequence + 1, previous_journal->continuation,
            previous->remaining_budget(), previous_journal->inflight_reservation,
            previous->exact_checkpoint(), std::max(now_ms(), previous->updated_at_ms())});
        ProgramRunRecordData data;
        data.owner_scope                      = previous->owner_scope();
        data.run_id                           = previous->run_id();
        data.logical_run_id                   = previous->logical_run_id();
        data.program_version_id               = previous->program_version_id();
        data.bundle_id                        = previous->bundle_id();
        data.binding_fingerprint              = previous->binding_fingerprint();
        data.invocation                       = previous->invocation();
        data.child_depth                      = previous->child_depth();
        data.continuation                     = journal.continuation;
        data.remaining_budget                 = previous->remaining_budget();
        data.exact_checkpoint                 = previous->exact_checkpoint();
        data.exact_checkpoint_content_id      = previous->exact_checkpoint_content_id();
        data.attempt_resume                   = previous->attempt_resume();
        data.settled_branches                 = previous->settled_branches();
        data.pending_input                    = previous->pending_input();
        data.pending_effect                   = previous->pending_effect();
        data.terminal_result                  = previous->terminal_result();
        data.fork_receipt                     = previous->fork_receipt();
        data.fork_source_run_id               = previous->fork_source_run_id();
        data.fork_source_program_version_id   = previous->fork_source_program_version_id();
        data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
        data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
        data.children                         = std::move(children);
        data.journal_head                     = journal.id;
        data.event_sequence                   = previous->event_sequence();
        data.effect_sequence                  = previous->effect_sequence();
        data.created_at_ms                    = previous->created_at_ms();
        data.updated_at_ms                    = journal.timestamp_ms;
        auto publication                      = ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
        const auto result = co_await compare_publish_with_lineage_async(
            *transitions, owner_scope, previous->journal_head(), std::move(publication));
        if (result == ProgramTransitionPublishResult::Published ||
            result == ProgramTransitionPublishResult::AlreadyPresent) {
            co_return ChildRecordPublication{
                result, co_await load_active_agent_run(transitions, owner_scope, parent_run_id)};
        }
    }
    co_return ChildRecordPublication{};
}

asio::awaitable<ChildRecordPublication> publish_child_record(
    std::string                             owner_scope,
    std::string                             parent_run_id,
    std::shared_ptr<ProgramTransitionStore> transitions,
    ProgramChildRecord                      child) {
    auto lock = co_await child_relation_publication_mutex(*transitions, owner_scope, parent_run_id).lock_async();
    co_return co_await publish_child_record_unlocked(owner_scope, parent_run_id, transitions, std::move(child));
}

asio::awaitable<ChildRecordPublication> publish_child_record(std::shared_ptr<detail::RunControl> parent,
                                                             ProgramChildRecord                 child) {
    co_return co_await publish_child_record(parent->owner_scope, parent->logical_run_id, parent->transitions,
                                            std::move(child));
}

// Publishes the terminal state of a child into its parent's durable relation. False when it could not (the parent is
// not running, the child is not the active generation, a permanent rejection); it never throws.
asio::awaitable<bool> publish_child_completion(std::shared_ptr<ProgramTransitionStore> transitions,
                                               std::string                             owner_scope,
                                               std::string                             parent_run_id,
                                               std::string                             child_run_id,
                                               ProgramResult                           result) {
    try {
        auto lock =
            co_await child_relation_publication_mutex(*transitions, owner_scope, parent_run_id).lock_async();
        std::uint64_t terminal_generation = 0;
        const auto    child_lineage = co_await transitions->load_run_lineage(owner_scope, child_run_id);
        if (child_lineage) {
            const auto active = co_await transitions->load_generation(
                owner_scope, child_lineage->lineage_id(), child_lineage->active_generation());
            if (!active || active->run_id() != result.run_id()) co_return false;
            if (result.run_id() != child_run_id) terminal_generation = active->generation();
        } else if (result.run_id() != child_run_id)
            co_return false;
        for (;;) {
            const auto record = co_await load_active_agent_run(transitions, owner_scope, parent_run_id);
            if (!record) co_return false;
            const auto existing = find_child(*record, child_run_id);
            if (!existing) co_return false;
            const auto state = child_state_for_result(result.status());
            if (existing->state == state && existing->terminal_result &&
                existing->terminal_result->serialize_canonical() == result.serialize_canonical())
                co_return true;
            const auto publication = co_await publish_child_record_unlocked(
                owner_scope, parent_run_id, transitions,
                ProgramChildRecord{existing->child_run_id, existing->link_id,
                                   existing->link_receipt, existing->invocation, state, result,
                                   terminal_generation});
            if (publication.result == ProgramTransitionPublishResult::Published ||
                publication.result == ProgramTransitionPublishResult::AlreadyPresent)
                co_return true;

            // A remote writer may advance this CAS chain.  Keep retrying while
            // the parent can still accept the child's terminal transition.
            const auto current = co_await load_active_agent_run(transitions, owner_scope, parent_run_id);
            if (!current || current->continuation().state != ContinuationState::Running)
                co_return false;
            // A rejection against an unchanged head is a validation/storage
            // failure, not competing publication. Do not strand completion
            // waiters in an infinite retry loop for a permanent rejection.
            if (current->journal_head() == record->journal_head()) co_return false;
            // Let the other writer's turn run before reading its result.
            co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        }
    } catch (...) {
        co_return false;
    }
}

void bind_child_completion(const std::shared_ptr<detail::RunControl>& child,
                           std::string_view                           owner_scope,
                           std::string_view                           parent_run_id,
                           std::string_view                           child_run_id,
                           std::function<void()>                      after_publication = {}) {
    const auto        transitions = child->transitions;
    const std::string owner(owner_scope);
    const std::string parent(parent_run_id);
    const std::string child_id(child_run_id);
    child->set_completion_callback(
        [transitions, owner, parent, child_id,
         after_publication = std::move(after_publication)](ProgramResult result) -> asio::awaitable<void> {
            const bool published = co_await publish_child_completion(transitions, owner, parent, child_id,
                                                                     std::move(result));
            if (published && after_publication) after_publication();
        });
}

asio::awaitable<bool> is_unstarted_child_dispatch(std::shared_ptr<ProgramTransitionStore> transitions,
                                                  std::string                             owner_scope,
                                                  std::string                             child_run_id) {
    const auto record = co_await transitions->load(owner_scope, child_run_id);
    if (!record || record->continuation().state != ContinuationState::Running ||
        record->continuation().attempt != 1 || record->event_sequence() != 1 ||
        record->effect_sequence() != 0 || record->exact_checkpoint() || record->pending_input() ||
        record->pending_effect() || record->terminal_result())
        co_return false;
    const auto journal = co_await transitions->latest(owner_scope, child_run_id);
    if (!journal || journal->sequence != 1 || journal->previous_id != "") co_return false;
    const auto events = co_await transitions->load_events(owner_scope, child_run_id);
    co_return events.size() == 1 && events.front().sequence == 1 &&
           events.front().kind == ProgramEventKind::Started;
}
ProgramJournalRecord initial_record(
    const detail::RunControl&             control,
    std::optional<CoreCheckpointIdentity> checkpoint = std::nullopt,
    std::optional<std::int64_t>           timestamp  = std::nullopt) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        "", control.run_id, control.program_version_id, control.bundle_id, 1,
        ProgramContinuation{"root", ContinuationState::Running, control.attempt},
        control.granted_budget, RunBudget{}, std::move(checkpoint), timestamp.value_or(now_ms())});
}

ProgramTransitionPublication initial_publication(
    const detail::RunControl&               control,
    const ProgramEvent&                     started,
    std::optional<CoreCheckpointIdentity>   checkpoint            = std::nullopt,
    std::optional<ForkCompatibilityReceipt> fork_receipt          = std::nullopt,
    std::optional<std::string>              recorded_binding_set  = std::nullopt,
    std::optional<ProgramPendingInput>      pending_input         = std::nullopt,
    std::optional<ProgramPendingEffect>     pending_effect        = std::nullopt,
    std::optional<std::int64_t>             timestamp             = std::nullopt,
    std::optional<std::string>              checkpoint_content_id = std::nullopt,
    std::vector<ProgramChildRecord>         inherited_children    = {},
    std::optional<ProgramAttemptResume>     attempt_resume        = std::nullopt,
    std::optional<json>                     settled_branches      = std::nullopt) {
    auto journal = initial_record(control, checkpoint, timestamp);
    ProgramRunRecordData data;
    data.owner_scope         = control.owner_scope;
    data.run_id              = control.run_id;
    data.logical_run_id              = control.logical_run_id;
    data.program_version_id  = control.program_version_id;
    data.bundle_id           = control.bundle_id;
    data.binding_fingerprint = control.binding_fingerprint;
    data.invocation          = control.invocation;
    data.children                    = std::move(inherited_children);
    data.child_depth         = control.persisted_invocation.child_depth;
    data.continuation        = journal.continuation;
    data.remaining_budget    = control.granted_budget;
    data.exact_checkpoint    = std::move(checkpoint);
    data.exact_checkpoint_content_id = std::move(checkpoint_content_id);
    data.attempt_resume      = std::move(attempt_resume);
    data.settled_branches    = std::move(settled_branches);
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

ProgramContextPublication transfer_context_to_run(
    const ProgramContextPublication& source, std::string target_run_id) {
    ContextEpochData epoch_data;
    epoch_data.run_id = std::move(target_run_id);
    epoch_data.sequence = 1;
    epoch_data.feed_id = source.epoch.feed_id();
    epoch_data.raw_from_sequence = source.epoch.raw_from_sequence();
    epoch_data.raw_through_sequence = source.epoch.raw_through_sequence();
    epoch_data.raw_window_digest = source.epoch.raw_window_digest();
    epoch_data.artifact_ids = source.epoch.artifact_ids();
    epoch_data.guarantee_profile = source.epoch.guarantee_profile();
    // The run's first epoch has no predecessor, so it keeps the source's order
    // and nothing it claimed about a prefix.
    epoch_data.ordered_artifact_ids = source.epoch.ordered_artifact_ids();
    epoch_data.assembly_layout = source.epoch.assembly_layout();
    auto epoch = ContextEpoch::create(std::move(epoch_data));

    ContextAssemblyReceiptData receipt_data;
    receipt_data.context_epoch_id = epoch.id();
    receipt_data.normalized_request_digest =
        source.assembly_receipt.normalized_request_digest();
    receipt_data.message_window_digest = source.assembly_receipt.message_window_digest();
    receipt_data.artifact_ids = source.assembly_receipt.artifact_ids();
    receipt_data.required_skill_artifact_ids =
        source.assembly_receipt.required_skill_artifact_ids();
    receipt_data.raw_from_sequence = source.assembly_receipt.raw_from_sequence();
    receipt_data.raw_through_sequence = source.assembly_receipt.raw_through_sequence();
    receipt_data.estimated_input_tokens = source.assembly_receipt.estimated_input_tokens();
    receipt_data.mandatory_input_tokens = source.assembly_receipt.mandatory_input_tokens();
    receipt_data.assembled_artifact_ids = source.assembly_receipt.assembled_artifact_ids();
    receipt_data.artifact_classes = source.assembly_receipt.artifact_classes();
    receipt_data.class_sources = source.assembly_receipt.class_sources();
    receipt_data.class_rule_version = source.assembly_receipt.class_rule_version();
    receipt_data.encoder_family = source.assembly_receipt.encoder_family();
    receipt_data.stable_boundary = source.assembly_receipt.stable_boundary();
    receipt_data.prefix_boundary = source.assembly_receipt.prefix_boundary();
    receipt_data.prefix_digest = source.assembly_receipt.prefix_digest();
    receipt_data.tools_digest = source.assembly_receipt.tools_digest();
    auto artifacts = source.artifacts;
    auto assembly = ContextAssemblyReceipt::create(receipt_data, epoch, artifacts);
    return {std::move(epoch), std::move(artifacts), std::move(assembly)};
}

ProgramJournalRecord resumed_record(const detail::RunControl&   control,
                                    const ProgramJournalRecord& previous,
                                    RunBudget                   remaining_budget,
                                    RunBudget                   inflight_reservation) {
    return ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.id, control.run_id, control.program_version_id, control.bundle_id,
        previous.sequence + 1,
        ProgramContinuation{"root", ContinuationState::Running, control.attempt},
        std::move(remaining_budget), std::move(inflight_reservation), previous.core_checkpoint,
        now_ms()});
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

asio::awaitable<TerminalPublicationResult> publish_terminal_record(const detail::RunControl& control,
                                                                  const ProgramResult&      result,
                                                                  std::exception_ptr&       cause) {
    auto lock = co_await child_relation_publication_mutex(*control.transitions, control.owner_scope,
                                                          control.logical_run_id)
                    .lock_async();
    for (int retry = 0; retry < 3; ++retry) {
        std::optional<ProgramRunPublicationHead> head;
        std::optional<ProgramRunRecord> loaded_previous;
        const ProgramRunRecord* previous = nullptr;
        try {
            head = (co_await control.transitions->load_run_publication_head(control.owner_scope,
                                                                 control.run_id));
            if (head) {
                previous = &head->run_record;
            } else {
                // Preserve absence/error classification for stores whose coherent
                // acquisition fails closed, without re-reading a native snapshot.
                loaded_previous = (co_await control.transitions->load(control.owner_scope, control.run_id));
                if (loaded_previous) previous = &*loaded_previous;
            }
        } catch (const std::exception& error) {
            cause = std::current_exception();
            std::cerr << "terminal load exception run=" << control.run_id
                      << " error=" << error.what() << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        } catch (...) {
            cause = std::current_exception();
            std::cerr << "terminal load unknown exception run=" << control.run_id << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        }
        if (!previous || previous->continuation().state != ContinuationState::Running ||
            previous->continuation().attempt != control.attempt) {
            co_return TerminalPublicationResult::TransitionConflict;
        }

        std::optional<ProgramJournalRecord> loaded_previous_journal;
        const ProgramJournalRecord* previous_journal = head ? &head->journal_record : nullptr;
        try {
            if (!head) {
                loaded_previous_journal = (co_await control.transitions->latest(control.owner_scope,
                                                                      control.run_id));
                if (loaded_previous_journal) previous_journal = &*loaded_previous_journal;
            }
        } catch (const std::exception& error) {
            cause = std::current_exception();
            std::cerr << "terminal latest exception run=" << control.run_id
                      << " error=" << error.what() << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        } catch (...) {
            cause = std::current_exception();
            std::cerr << "terminal latest unknown exception run=" << control.run_id << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        }
        if (!previous_journal || previous_journal->id != previous->journal_head()) {
            std::cerr << "terminal journal mismatch run=" << control.run_id
                      << " record_head=" << previous->journal_head()
                      << " latest=" << (previous_journal ? previous_journal->id : "<none>")
                      << " record_seq=" << previous->event_sequence() << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        }

        try {
            auto events               = control.events_after(previous->event_sequence());
            auto terminal_reservation = RunBudget{};
            if (result.status() == ProgramTerminalStatus::Interrupted ||
                result.status() == ProgramTerminalStatus::AmbiguousEffect) {
                terminal_reservation = previous_journal->inflight_reservation;
                const auto stored_checkpoint =
                    result.checkpoint() && control.checkpoints
                        ? control.checkpoints->load_by_id(result.checkpoint()->checkpoint_id)
                        : std::nullopt;
                const bool synthetic_terminal_checkpoint =
                    stored_checkpoint && stored_checkpoint->metadata.is_object() &&
                    stored_checkpoint->metadata.value("neograph_program", std::string{}) ==
                        "terminal";
                // A JavaScript command returned for reconciliation keeps the
                // resources it reserved until the reconciliation settles them
                // (#402, #403), whichever checkpoint the run carries.
                const bool javascript_reconciliation =
                    result.interrupt() && result.interrupt()->pending_effect &&
                    !result.interrupt()->pending_input &&
                    result.interrupt()->pending_effect->core_node() == "javascript";
                if (result.status() == ProgramTerminalStatus::Interrupted && stored_checkpoint &&
                    !synthetic_terminal_checkpoint && !javascript_reconciliation) {
                    terminal_reservation = RunBudget{};
                }
            }
            auto                 journal = ProgramJournalRecord::create(ProgramJournalRecordData{
                previous->journal_head(), control.run_id, control.program_version_id,
                control.bundle_id, previous_journal->sequence + 1,
                ProgramContinuation{"root", continuation_state(result.status()), control.attempt},
                result.remaining_budget(), terminal_reservation, result.checkpoint(), now_ms()});
            ProgramRunRecordData data;
            data.owner_scope         = control.owner_scope;
            data.run_id              = control.run_id;
            data.logical_run_id      = control.logical_run_id;
            data.program_version_id  = control.program_version_id;
            data.bundle_id           = control.bundle_id;
            data.binding_fingerprint = previous->binding_fingerprint();
            data.invocation          = previous->invocation();
            data.child_depth         = previous->child_depth();
            const bool resumable = result.status() == ProgramTerminalStatus::Interrupted ||
                                   result.status() == ProgramTerminalStatus::AmbiguousEffect;
            data.children            = resumable ? previous->children()
                                                : terminal_children(previous->children());
            data.continuation        = journal.continuation;
            data.remaining_budget    = result.remaining_budget();
            data.exact_checkpoint    = result.checkpoint();
            if (result.checkpoint() == previous->exact_checkpoint()) {
                data.exact_checkpoint_content_id =
                    previous->exact_checkpoint_content_id();
            }
            data.pending_input       = previous->pending_input();
            data.pending_effect      = previous->pending_effect();
            if (const auto interrupt = result.interrupt()) {
                data.pending_input  = interrupt->pending_input;
                data.pending_effect = interrupt->pending_effect;
            }
            // #384, #386: what the paused plan recorded, which the attempt
            // that resumes the run replays.
            if (result.status() == ProgramTerminalStatus::Interrupted && result.checkpoint())
                data.settled_branches = control.settled_branches();
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
            if (const auto interrupt = result.interrupt(); interrupt && interrupt->pending_effect) {
                ++data.effect_sequence;
                effects.emplace_back(data.effect_sequence, *interrupt->pending_effect);
            }
            data.created_at_ms = previous->created_at_ms();
            data.updated_at_ms = journal.timestamp_ms;
            auto publication   = ProgramTransitionPublication{
                ProgramRunRecord::create(std::move(data)), std::move(journal), std::move(events),
                std::move(effects)};
            publication = co_await attach_run_lineage_async(*control.transitions, std::move(publication));
            const auto execution_lease = control.execution_lease();
            ProgramTransitionPublishResult published;
            if (execution_lease)
                published = (co_await control.transitions->compare_publish_execution(
                      control.owner_scope, previous->journal_head(), std::move(publication),
                      execution_lease, std::nullopt));
            else
                published = (co_await control.transitions->compare_publish(
                      control.owner_scope, previous->journal_head(), std::move(publication)));
            if (published == ProgramTransitionPublishResult::Published ||
                published == ProgramTransitionPublishResult::AlreadyPresent) {
                if (execution_lease) control.clear_execution_lease(execution_lease->id());
                co_return TerminalPublicationResult::Published;
            }
        } catch (const std::exception& error) {
            cause = std::current_exception();
            std::cerr << "terminal publication exception run=" << control.run_id
                      << " error=" << error.what() << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        } catch (...) {
            cause = std::current_exception();
            std::cerr << "terminal publication unknown exception run=" << control.run_id << '\n';
            co_return TerminalPublicationResult::JournalFailure;
        }
    }
    co_return TerminalPublicationResult::TransitionConflict;
}

// Initial publication is durable before the mandatory admission hook can run.
// A rejected hook must close that published run rather than strand an
// unregistered Running record that neither callers nor recovery can complete.
asio::awaitable<bool> admit_started(std::shared_ptr<detail::RunControl>   control,
                                    ProgramEvent                          started,
                                    RunBudget                             remaining_budget,
                                    std::optional<CoreCheckpointIdentity> checkpoint = std::nullopt) {
    // A handler cannot suspend: note what blocked the admission, then close the run after it.
    std::optional<ProgramFailure> blocked;
    try {
        control->emit_lifecycle_hook(HookPhase::MessageAdmitted, started);
        co_return true;
    } catch (const std::exception& error) {
        blocked = ProgramFailure{"P_HOOK_BLOCKED", error.what(), "root", "", 0, json::object()};
    } catch (...) {
        blocked = ProgramFailure{"P_HOOK_BLOCKED", "Message admission hook blocked", "root", "", 0,
                                 json::object()};
    }
    detail::RunOutcome outcome;
    outcome.status           = ProgramTerminalStatus::Failed;
    outcome.remaining_budget = std::move(remaining_budget);
    outcome.checkpoint       = std::move(checkpoint);
    outcome.failure          = std::move(blocked);
    co_await control->complete_async(std::move(outcome));
    co_return false;
}

}  // namespace

ProgramExecutorRequired::ProgramExecutorRequired()
    : std::runtime_error("This build has no threads, so ProgramRuntime has none to run Programs on: set "
                         "RuntimeConfig::executor (for a WebAssembly host, async::HostLoop::executor())") {}

ProgramBlockingCallRefused::ProgramBlockingCallRefused(std::string operation, std::string alternative,
                                                       std::string reason)
    : std::runtime_error(operation + " is refused: " + reason +
                         (alternative.empty() ? std::string{} : "; use " + alternative + " instead")),
      operation_(std::move(operation)),
      alternative_(std::move(alternative)) {}

namespace detail {

namespace {

// Whether the calling thread runs `executor`, for the executors the runtime can see into: the executor of an
// io_context or a thread_pool, and a strand of either. Anything else is the host's to say
// (ProgramExecutorIdentity::running_in_this_thread).
bool runs_in_this_thread(const asio::any_io_executor& executor) noexcept {
    try {
        if (const auto* io = executor.target<asio::io_context::executor_type>()) return io->running_in_this_thread();
        if (const auto* pool = executor.target<asio::thread_pool::executor_type>())
            return pool->running_in_this_thread();
        if (const auto* strand = executor.target<asio::strand<asio::io_context::executor_type>>())
            return strand->get_inner_executor().running_in_this_thread();
        if (const auto* strand = executor.target<asio::strand<asio::thread_pool::executor_type>>())
            return strand->get_inner_executor().running_in_this_thread();
    } catch (...) {
    }
    return false;
}
}  // namespace


bool RuntimeScheduler::on_executor_thread() const {
    if (!injected) return false;
    if (running_in_this_thread) return running_in_this_thread();
    return runs_in_this_thread(host_executor);
}

bool RuntimeScheduler::runs_on_attempt_thread() const {
    if (on_executor_thread()) return true;
    // The runtime's own pools: attempts run on one, deadline timers on the other.
    return !injected && (runs_in_this_thread(attempts) || runs_in_this_thread(deadlines));
}

void RuntimeScheduler::require_synchronous_call(std::string_view operation,
                                                std::string_view alternative) const {
    if (!host_pumped) return;
    throw ProgramBlockingCallRefused(
        std::string(operation), std::string(alternative),
        NEOGRAPH_BUILD_THREADS
            ? "the runtime's executor is run by a host's event loop, and a synchronous call would block it"
            : "this build has no threads, so a synchronous call has nothing to wait on");
}

void RuntimeScheduler::require_wait(std::string_view operation, std::string_view alternative) const {
    if (host_pumped)
        throw ProgramBlockingCallRefused(
            std::string(operation), std::string(alternative),
            NEOGRAPH_BUILD_THREADS
                ? "the runtime's executor is run by a host's event loop, and waiting would block it"
                : "this build has no threads, so nothing could finish while it waits");
    if (on_executor_thread())
        throw ProgramBlockingCallRefused(
            std::string(operation), std::string(alternative),
            "it is made on a thread that runs the runtime's executor, which would wait for work only that thread can do");
}

RunControl::RunControl(std::string                                owner,
                       std::string                                run,
                       std::uint64_t                              attempt_value,
                       std::shared_ptr<const MaterializedProgram> pinned,
                       std::string                                binding,
                       ProgramPersistedInvocation                 execution_invocation,
                       RunInvocation                              invocation_value,
                       std::string                                thread,
                       std::uint64_t                              event_sequence,
                       std::shared_ptr<ProgramEventSink>          sink,
                       std::shared_ptr<const RuntimeScheduler>    scheduler_value,
                       std::shared_ptr<graph::CheckpointStore>    checkpoint_store,
                       std::shared_ptr<graph::Store>              store,
                       std::shared_ptr<ProgramTransitionStore>    transition_store,
                       std::optional<ProgramExecutionLease>       execution_lease,
                       std::string                                logical_identity)
    : owner_scope(std::move(owner)),
      run_id(std::move(run)),
      logical_run_id(logical_identity.empty() ? run_id : std::move(logical_identity)),
      program_version_id(pinned->version.id()),
      bundle_id(pinned->bundle.id()),
      binding_fingerprint(std::move(binding)),
      attempt(attempt_value),
      materialized(std::move(pinned)),
      persisted_invocation(std::move(execution_invocation)),
      invocation(std::move(invocation_value)),
      granted_budget(persisted_invocation.granted_budget),
      started_at(std::chrono::steady_clock::now()),
      deadline(started_at + std::chrono::milliseconds(granted_budget.wall_time_ms)),
      scheduler(std::move(scheduler_value)),
      deadline_executor(scheduler->deadlines),
      waiter_strand(asio::make_strand(scheduler->waiters)),
      core_thread_id(std::move(thread)),
      trace_id(invocation.correlation_id),
      checkpoints(std::move(checkpoint_store)),
      state_store(std::move(store)),
      transitions(std::move(transition_store)),
      cancel_token(std::make_shared<graph::CancelToken>()),
      sink_(std::move(sink)),
      next_sequence_(event_sequence + 1),
      execution_lease_(std::move(execution_lease)) {
    if (invocation.owner_scope != owner_scope || invocation.run_id != run_id ||
        invocation.program_version_id != program_version_id ||
        invocation.input != persisted_invocation.input ||
        invocation.correlation_id != persisted_invocation.trace_id ||
        invocation.parent_run_id != persisted_invocation.parent_run_id) {
        throw std::invalid_argument("Program runtime projection does not match RunInvocation");
    }
    if (execution_lease_ &&
        (execution_lease_->owner_scope() != owner_scope ||
         execution_lease_->run_id() != run_id || execution_lease_->attempt() != attempt ||
         execution_lease_->program_version_id() != program_version_id ||
         execution_lease_->bundle_id() != bundle_id ||
         execution_lease_->core_name() != materialized->root->core_name ||
         execution_lease_->core_generation_id() !=
             materialized->root->compiled_plan_identity ||
         execution_lease_->core_thread_id() != core_thread_id)) {
        throw std::invalid_argument("Program execution lease does not bind RunControl");
    }
    if (execution_lease_ && supports_live_graph_migration(*materialized)) {
        renew_graph_safe_point_request_locked();
    }
}

RunControl::RunControl(ProgramRunRecord                        record,
                       std::vector<ProgramEvent>               loaded_events,
                       std::shared_ptr<ProgramTransitionStore> transition_store,
                       std::shared_ptr<sp::NativeArchive> archive,
                       std::shared_ptr<const RuntimeScheduler> scheduler_value)
    : owner_scope(record.owner_scope()),
      run_id(record.run_id()),
      logical_run_id(record.logical_run_id()),
      program_version_id(record.program_version_id()),
      bundle_id(record.bundle_id()),
      binding_fingerprint(record.binding_fingerprint()),
      attempt(record.continuation().attempt),
      materialized(),
      persisted_invocation(ProgramPersistedInvocation{
          record.invocation().input, record.invocation().budget, record.invocation().correlation_id,
          record.invocation().parent_run_id, record.child_depth()}),
      invocation(record.invocation()),
      granted_budget(persisted_invocation.granted_budget),
      started_at(std::chrono::steady_clock::now()),
      deadline(started_at + std::chrono::milliseconds(record.remaining_budget().wall_time_ms)),
      scheduler(std::move(scheduler_value)),
      deadline_executor(scheduler->deadlines),
      waiter_strand(asio::make_strand(scheduler->waiters)),
      core_thread_id(record.exact_checkpoint() ? record.exact_checkpoint()->core_thread_id
                                               : std::string{}),
      trace_id(invocation.correlation_id),
      checkpoints(),
      state_store(),
      transitions(std::move(transition_store)),
      cancel_token(std::make_shared<graph::CancelToken>()),
      result_(record.terminal_result()),
      events_(std::move(loaded_events)),
      latest_checkpoint_(record.exact_checkpoint()),
      next_sequence_(record.event_sequence() + 1),
      terminal_decided_(result_.has_value()),
      completion_claimed_(result_.has_value()) {
    native_history_archive = std::move(archive);
    if (result_) result_ = result_->restore_provider_failure(owner_scope, native_history_archive);
}
void RunControl::set_completion_callback(CompletionCallback callback) noexcept {
    std::shared_ptr<RunControl> next;
    {
        std::lock_guard lock(mutex_);
        next = successor_;
        if (!next) completion_callback_ = std::move(callback);
    }
    if (next) next->set_completion_callback(std::move(callback));
}

void RunControl::add_logical_cleanup(TerminalCleanup cleanup) {
    std::shared_ptr<RunControl> next;
    bool                        run_now = false;
    {
        std::lock_guard lock(mutex_);
        next = successor_;
        if (!next) {
            run_now = terminal_decided_ || result_.has_value();
            if (!run_now) logical_cleanups_.push_back(std::move(cleanup));
        }
    }
    if (next)
        next->add_logical_cleanup(std::move(cleanup));
    else if (run_now && cleanup)
        cleanup();
}
std::shared_ptr<RunControl> RunControl::following_successor() const {
    std::lock_guard lock(mutex_);
    return follow_successor_ ? successor_ : nullptr;
}
asio::awaitable<void> RunControl::retire_to(std::shared_ptr<RunControl> successor,
                                            std::shared_ptr<ReentrantOperationLock> held) {
    if (!successor || successor.get() == this || successor->owner_scope != owner_scope ||
        successor->logical_run_id != logical_run_id)
        throw std::invalid_argument("Invalid Program successor control");
    std::vector<std::shared_ptr<RunControl>> children;
    std::vector<TerminalCleanup>             cleanups;
    CompletionCallback                       callback;
    std::vector<AsyncWaiter>                 waiters;
    {
        std::lock_guard lock(mutex_);
        if (successor_) co_return;
        successor_        = successor;
        follow_successor_ = !invocation.parent_run_id.empty();
        for (const auto& weak : children_)
            if (auto child = weak.lock()) children.push_back(child);
        children_.clear();
        cleanups.swap(logical_cleanups_);
        callback            = std::move(completion_callback_);
        cancellation_cause_ = CancellationCause::ParentTerminal;
        if (follow_successor_) waiters.swap(waiters_);
    }
    for (const auto& child : children)
        co_await successor->attach_child(child, held);
    for (auto& cleanup : cleanups)
        successor->add_logical_cleanup(std::move(cleanup));
    if (callback) successor->set_completion_callback(std::move(callback));
    cancel_token->cancel();
    abort_handoff();
    co_await abort_graph_migration_async();
    cv_.notify_all();
    asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
        for (const auto& waiter : waiters)
            if (auto timer = waiter.timer.lock()) timer->cancel();
    });
}

void RunControl::add_terminal_cleanup(TerminalCleanup cleanup) {
    bool run_now = false;
    {
        std::lock_guard lock(mutex_);
        run_now = completion_claimed_;
        if (!run_now) {
            terminal_cleanups_.push_back(std::move(cleanup));
            return;
        }
    }
    if (cleanup) cleanup();
}
void RunControl::set_child_launch_callback(ChildLaunchCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    child_launch_callback_ = std::move(callback);
}
void RunControl::set_child_recovery_callback(ChildRecoveryCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    child_recovery_callback_ = std::move(callback);
}
asio::awaitable<ChildRecoveryMode> RunControl::can_recover_child(std::string binding,
                                                                 json        input,
                                                                 std::string operation) const {
    ChildRecoveryCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = child_recovery_callback_;
    }
    if (!callback) co_return ChildRecoveryMode::None;
    co_return co_await callback(std::move(binding), std::move(input), std::move(operation));
}
void RunControl::set_existing_child_reconnect_callback(
    ExistingChildReconnectCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    existing_child_reconnect_callback_ = std::move(callback);
}
asio::awaitable<std::shared_ptr<RunControl>> RunControl::reconnect_existing_child(
    std::string binding, json input, std::string operation) const {
    ExistingChildReconnectCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = existing_child_reconnect_callback_;
    }
    if (!callback)
        throw_runtime_diagnostic("P_CHILD_RECOVERY", "Existing child reconnect is not configured");
    co_return co_await callback(std::move(binding), std::move(input), std::move(operation));
}
void RunControl::set_child_binding_validation_callback(
    ChildBindingValidationCallback callback) noexcept {
    std::lock_guard lock(mutex_);
    child_binding_validation_callback_ = std::move(callback);
}

void RunControl::set_hook_runtime(std::shared_ptr<HookRuntime> runtime) noexcept {
    if (!runtime && !hook_runtime_enabled_.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(mutex_);
    hook_runtime_ = std::move(runtime);
    hook_runtime_enabled_.store(static_cast<bool>(hook_runtime_), std::memory_order_release);
}

std::shared_ptr<HookRuntime> RunControl::hook_runtime() const {
    if (!hook_runtime_enabled_.load(std::memory_order_acquire)) return {};
    std::lock_guard lock(mutex_);
    return hook_runtime_;
}

void RunControl::set_core_tool_grant_resolver(ProgramCoreToolGrantResolver resolver) {
    std::lock_guard lock(mutex_);
    core_tool_grant_resolver_ = std::move(resolver);
}

void RunControl::set_core_tool_grant_awaitable_resolver(ProgramCoreToolGrantAwaitableResolver resolver) {
    std::lock_guard lock(mutex_);
    core_tool_grant_awaitable_resolver_ = std::move(resolver);
}

asio::awaitable<std::optional<ProgramCoreToolGrant>> RunControl::resolve_core_tool_grant(
    std::string operation_id) const {
    ProgramCoreToolGrantResolver          resolver;
    ProgramCoreToolGrantAwaitableResolver awaitable_resolver;
    {
        std::lock_guard lock(mutex_);
        resolver           = core_tool_grant_resolver_;
        awaitable_resolver = core_tool_grant_awaitable_resolver_;
    }
    // The context holds views of this frame's strings; they stay alive until the resolver's awaitable completes.
    const ProgramCoreToolGrantContext context{owner_scope, program_version_id, run_id, operation_id, attempt,
                                              binding_fingerprint};
    if (awaitable_resolver) co_return co_await awaitable_resolver(context);
    if (!resolver) co_return std::nullopt;
    co_return resolver(context);
}

void RunControl::set_core_provider_call_resolver(ProgramCoreProviderCallResolver resolver,
                                                 bool required) {
    std::lock_guard lock(mutex_);
    core_provider_call_resolver_ = std::move(resolver);
    require_core_provider_call_broker_ = required;
}

void RunControl::add_notice(ProgramNotice notice) {
    std::lock_guard lock(notices_mutex_);
    notices.push_back(std::move(notice));
}

std::vector<ProgramNotice> RunControl::notices_snapshot() const {
    std::lock_guard lock(notices_mutex_);
    return notices;
}

void RunControl::set_monetary_account(std::shared_ptr<ProgramMonetaryAccount> account) {
    std::lock_guard lock(mutex_);
    monetary_account_ = std::move(account);
}

void RunControl::set_journaled_usage(std::shared_ptr<UsageAccumulator> usage) {
    std::lock_guard lock(mutex_);
    journaled_usage_ = std::move(usage);
}

std::shared_ptr<graph::ProviderCallBroker> RunControl::resolve_core_provider_call_broker(
    std::string_view operation_id) const {
    ProgramCoreProviderCallResolver resolver;
    bool required;
    std::shared_ptr<ProgramMonetaryAccount> monetary;
    std::shared_ptr<UsageAccumulator>       journaled_usage;
    {
        std::lock_guard lock(mutex_);
        resolver = core_provider_call_resolver_;
        required = require_core_provider_call_broker_;
        monetary = monetary_account_;
        journaled_usage = journaled_usage_;
    }
    if (!resolver) {
        if (required)
            throw std::runtime_error("Program Core provider broker resolver is required");
        return {};
    }
    ProgramCoreProviderCallContext context{owner_scope,      program_version_id, run_id,
                                           operation_id,     attempt,            std::move(monetary),
                                           std::move(journaled_usage)};
    if (fork_provider_replay) {
        // #405: the Core threads the operation may run on (the root's, its
        // own and every enclosing operation's), each with the source's thread
        // of the same derivation.
        const auto& fork = *fork_provider_replay;
        ProgramCoreProviderCallReplaySource source;
        source.run_id             = fork.run_id;
        source.program_version_id = fork.program_version_id;
        source.max_attempt        = fork.max_attempt;
        // A call the fork continues from a checkpoint its source recorded
        // (a paused plan branch, #386) runs on the source's own thread.
        const auto add = [&source](std::string target, std::string origin) {
            source.threads.emplace_back(origin, origin);
            source.threads.emplace_back(std::move(target), std::move(origin));
        };
        const auto generation = materialized->root->compiled_plan_identity;
        add(core_thread_identity(run_id, generation), fork.root_thread_id);
        add(core_thread_identity(run_id, generation), fork.forked_thread_id);
        for (std::size_t end = 0; end <= operation_id.size(); ++end) {
            if (end < operation_id.size() && operation_id[end] != '.' && operation_id[end] != '/')
                continue;
            const auto enclosing = operation_id.substr(0, end);
            if (enclosing.empty() || enclosing == "root") continue;
            add(core_thread_identity(run_id, generation, enclosing),
                core_thread_identity(fork.run_id, fork.generation_id, enclosing));
        }
        context.replay_source = std::move(source);
    }
    // #543: what a broker tells the host about a call goes to the run's handle.
    context.diagnostic = [weak = weak_from_this()](ProgramCoreProviderCallDiagnostic diagnostic) {
        if (const auto control = std::const_pointer_cast<RunControl>(weak.lock()))
            control->add_notice(ProgramNotice{std::move(diagnostic.code), std::move(diagnostic.message),
                                              std::move(diagnostic.witness)});
    };
    const auto binding = resolver(context);
    if (!binding || binding->owner_scope != owner_scope ||
        binding->program_version_id != program_version_id ||
        binding->run_id != run_id || binding->operation_id != operation_id ||
        binding->attempt != attempt || !binding->broker) {
        throw std::runtime_error("Program Core provider broker binding is absent or stale");
    }
    return binding->broker;
}

void RunControl::emit_lifecycle_hook(HookPhase phase, const ProgramEvent& event,
                                     std::optional<ProgramTerminalStatus> status,
                                     bool observe_cancellation) const {
    if (!hook_runtime_enabled_.load(std::memory_order_acquire)) return;
    std::shared_ptr<HookRuntime> runtime;
    {
        std::lock_guard lock(mutex_);
        runtime = hook_runtime_;
    }
    if (!runtime) return;

    json data{{"program_version_id", program_version_id},
              {"bundle_id", bundle_id},
              {"attempt", attempt},
              {"program_event_id", event.id},
              {"program_event_sequence", event.sequence}};
    if (status) data["terminal_status"] = std::string(to_string(*status));
    const auto remaining = deadline > std::chrono::steady_clock::now()
        ? deadline - std::chrono::steady_clock::now()
        : std::chrono::steady_clock::duration::zero();
    const auto parent_deadline = std::chrono::system_clock::now() +
        std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
    async::run_sync(runtime->emit_async(
        phase, "program_lifecycle", owner_scope, run_id, std::move(data),
        observe_cancellation ? cancel_token : std::shared_ptr<graph::CancelToken>{},
        parent_deadline, event.sequence));
}

asio::awaitable<void> RunControl::validate_child_binding(std::string binding_name,
                                                         std::string operation_id) const {
    ChildBindingValidationCallback callback;
    {
        std::lock_guard lock(mutex_);
        callback = child_binding_validation_callback_;
    }
    if (!callback)
        throw_runtime_diagnostic("P_CHILD_BINDING",
                                 "Program spawn has no configured child binding resolver",
                                 json{{"binding", std::string(binding_name)},
                                      {"operation_id", std::string(operation_id)}});
    try {
        co_await callback(binding_name, operation_id);
    } catch (const ProgramDiagnosticError& error) {
        auto diagnostic = error.diagnostic();
        if (!diagnostic.witness.is_object()) diagnostic.witness = json::object();
        diagnostic.witness["operation_id"] = std::string(operation_id);
        throw ProgramDiagnosticError(std::move(diagnostic));
    }
}

void RunControl::set_task_graph_expansion_context(
    std::shared_ptr<TaskGraphFragmentStore> store,
    TaskGraphExpansionPolicyResolver        policy_resolver) noexcept {
    std::lock_guard lock(mutex_);
    task_graph_fragments_       = std::move(store);
    task_graph_policy_resolver_ = std::move(policy_resolver);
}

std::shared_ptr<TaskGraphFragmentStore> RunControl::task_graph_fragments() const {
    std::lock_guard lock(mutex_);
    return task_graph_fragments_;
}

std::optional<TaskGraphExpansionPolicy> RunControl::task_graph_policy(
    std::string_view expansion_operation_id) const {
    TaskGraphExpansionPolicyResolver resolver;
    {
        std::lock_guard lock(mutex_);
        resolver = task_graph_policy_resolver_;
    }
    if (!resolver) return std::nullopt;
    return resolver(owner_scope, program_version_id, expansion_operation_id);
}

asio::awaitable<std::shared_ptr<RunControl>> RunControl::launch_child(
    std::string                    binding_name,
    json                           input,
    std::string                    operation_id,
    std::string                    execution_key,
    std::optional<TaskGraphBudget> budget_override,
    bool                           exact_budget) const {
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
        co_return co_await callback(binding_name, std::move(input), operation_id, execution_key,
                                    std::move(budget_override), exact_budget);
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
    for (const auto& child : children)
        (void)child->cancel(cause);
}

// A cancellation that could not reach the store stops the children in memory, here, and leaves their durable
// cancellation to a task of the executor: it needs no thread to wait.
void RunControl::cancel_children_in_background(CancellationCause cause) noexcept {
    try {
        asio::co_spawn(
            scheduler->attempts,
            [keep = shared_from_this(), cause]() -> asio::awaitable<void> {
                co_await keep->cancel_children_async(cause);
            },
            asio::detached);
    } catch (...) {
        cancel_children(cause);
    }
}

asio::awaitable<void> RunControl::cancel_children_async(CancellationCause cause,
                                                        std::shared_ptr<ReentrantOperationLock> held) {
    std::vector<std::shared_ptr<RunControl>> children;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(children_, [](const auto& weak) { return weak.expired(); });
        children.reserve(children_.size());
        for (const auto& weak : children_)
            if (auto child = weak.lock()) children.push_back(std::move(child));
    }
    for (const auto& child : children)
        (void)co_await child->cancel_async(cause, held);
}

asio::awaitable<void> RunControl::attach_child(std::shared_ptr<RunControl> child,
                                               std::shared_ptr<ReentrantOperationLock> held) {
    if (!child || child.get() == this) co_return;
    CancellationCause cause = CancellationCause::ParentTerminal;
    bool              cancel_now;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(children_, [](const auto& weak) { return weak.expired(); });
        cancel_now = terminal_decided_ || cancellation_cause_ != CancellationCause::None ||
                     result_.has_value();
        if (!cancel_now) {
            children_.push_back(child);
            co_return;
        }
        if (cancellation_cause_ != CancellationCause::None) cause = cancellation_cause_;
    }
    (void)co_await child->cancel_async(cause, std::move(held));
}
// The Cancelled state of an interrupted run, as the publication that makes it durable, with the terminal event and
// the result it carries. A terminal hook that blocks turns it into a Failed state instead: cancelling an interrupted run
// is a durable obligation, so a blocked hook must not make it a no-op.
struct InterruptedCancellation {
    ProgramTransitionPublication publication;
    ProgramEvent                 terminal;
    ProgramResult                result;
};

InterruptedCancellation build_interrupted_cancellation(const RunControl&                 control,
                                                       const ProgramRunRecord&           previous,
                                                       const ProgramJournalRecord&       previous_journal,
                                                       std::optional<ProgramPendingInput>  pending_input,
                                                       std::optional<ProgramPendingEffect> pending_effect) {
    const auto timestamp = now_ms();
    auto       journal   = ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.journal_head(), control.run_id, control.program_version_id, control.bundle_id,
        previous_journal.sequence + 1,
        ProgramContinuation{"root", ContinuationState::Cancelled, control.attempt},
        previous.remaining_budget(), RunBudget{}, previous.exact_checkpoint(), timestamp});
    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Cancelled;
    result_data.run_id             = control.run_id;
    result_data.program_version_id = control.program_version_id;
    result_data.bundle_id          = control.bundle_id;
    result_data.operation_id       = control.operation_id;
    result_data.attempt            = control.attempt;
    result_data.remaining_budget   = previous.remaining_budget();
    result_data.checkpoint         = previous.exact_checkpoint();
    auto       cancelled_result    = ProgramResult::create(std::move(result_data));
    const auto checkpoint          = previous.exact_checkpoint();
    auto       terminal            = ProgramEvent::create(
        ProgramEvent{"", previous.event_sequence() + 1, timestamp, control.run_id,
                     control.program_version_id, control.bundle_id, control.operation_id,
                     checkpoint ? checkpoint->core_generation_id : std::string{},
                     checkpoint ? checkpoint->core_thread_id : std::string{}, control.trace_id,
                     control.attempt, ProgramEventKind::Terminal,
                     ProgramTerminalEvent{ProgramTerminalStatus::Cancelled}});
    ProgramRunRecordData data;
    data.owner_scope                    = control.owner_scope;
    data.run_id                         = control.run_id;
    data.logical_run_id                 = control.logical_run_id;
    data.program_version_id             = control.program_version_id;
    data.bundle_id                      = control.bundle_id;
    data.binding_fingerprint            = previous.binding_fingerprint();
    data.invocation                     = previous.invocation();
    data.child_depth                    = previous.child_depth();
    data.children                       = terminal_children(previous.children());
    data.continuation                   = journal.continuation;
    data.remaining_budget               = previous.remaining_budget();
    data.exact_checkpoint               = previous.exact_checkpoint();
    data.exact_checkpoint_content_id    = previous.exact_checkpoint_content_id();
    data.pending_input                  = std::move(pending_input);
    data.pending_effect                 = std::move(pending_effect);
    data.terminal_result                = cancelled_result;
    data.fork_receipt                   = previous.fork_receipt();
    data.fork_source_run_id             = previous.fork_source_run_id();
    data.fork_source_program_version_id = previous.fork_source_program_version_id();
    data.fork_source_checkpoint_id      = previous.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = previous.recorded_binding_set_fingerprint();
    data.journal_head    = journal.id;
    data.event_sequence  = terminal.sequence;
    data.effect_sequence = previous.effect_sequence();
    data.created_at_ms   = previous.created_at_ms();
    data.updated_at_ms   = timestamp;
    const auto block_cancellation = [&](std::string message) {
        journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous.journal_head(), control.run_id, control.program_version_id, control.bundle_id,
            previous_journal.sequence + 1,
            ProgramContinuation{"root", ContinuationState::Failed, control.attempt},
            previous.remaining_budget(), RunBudget{}, previous.exact_checkpoint(), timestamp});
        ProgramResultData blocked_data;
        blocked_data.status             = ProgramTerminalStatus::Failed;
        blocked_data.run_id             = control.run_id;
        blocked_data.program_version_id = control.program_version_id;
        blocked_data.bundle_id          = control.bundle_id;
        blocked_data.operation_id       = control.operation_id;
        blocked_data.attempt            = control.attempt;
        blocked_data.remaining_budget   = previous.remaining_budget();
        blocked_data.checkpoint         = previous.exact_checkpoint();
        blocked_data.failure = ProgramFailure{
            "P_HOOK_BLOCKED", std::move(message), "root", "", 0, json::object()};
        cancelled_result = ProgramResult::create(std::move(blocked_data));
        terminal.id.clear();
        terminal.payload = ProgramTerminalEvent{ProgramTerminalStatus::Failed};
        terminal = ProgramEvent::create(std::move(terminal));
        data.continuation    = journal.continuation;
        data.terminal_result = cancelled_result;
        data.journal_head    = journal.id;
        try {
            control.emit_lifecycle_hook(HookPhase::BeforeTerminalPublication, terminal,
                                        ProgramTerminalStatus::Failed, false);
        } catch (...) {}
    };
    try {
        control.emit_lifecycle_hook(HookPhase::BeforeTerminalPublication, terminal,
                                    ProgramTerminalStatus::Cancelled, false);
    } catch (const std::exception& error) {
        block_cancellation(error.what());
    } catch (...) {
        block_cancellation("Cancellation terminal hook blocked");
    }
    auto event = terminal;
    return InterruptedCancellation{
        ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)), std::move(journal),
                                     {event}, {}},
        std::move(terminal), std::move(cancelled_result)};
}

// The durable half of a cancellation (#557): a run that is interrupted has no attempt to stop, so the cancellation
// is the publication of its Cancelled terminal state. A live run is stopped in memory, which needs no store.
asio::awaitable<bool> RunControl::cancel_async(CancellationCause cause,
                                              std::shared_ptr<ReentrantOperationLock> held,
                                              std::exception_ptr* failure) {
    try {
        if (const auto next = following_successor()) co_return co_await next->cancel_async(cause, held);
        std::shared_ptr<ReentrantOperationLock> coordination_mutex;
        {
            std::lock_guard lock(mutex_);
            coordination_mutex = handoff_coordination_mutex_;
        }
        // The process-wide coordination lock serialises this cancellation with a resume, a replacement and a migration
        // of the same run: a cancellation that arrives while one of them commits waits for it, then sees its outcome.
        // A caller that holds the lock already passes it in (`held`), and so do we to the children we cancel.
        ReentrantOperationLock::Guard coordination_lock;
        if (coordination_mutex && coordination_mutex != held) {
            auto taken        = co_await coordination_mutex->lock_async();
            coordination_lock = std::move(taken);
        }
        const auto held_here = coordination_mutex ? coordination_mutex : held;
        auto previous = co_await transitions->load(owner_scope, run_id);
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
            if (disposition == ProgramPendingDisposition::Duplicate) co_return false;
            if (disposition == ProgramPendingDisposition::Applied) {
                const auto previous_journal = (co_await transitions->latest(owner_scope, run_id));
                if (!previous_journal || previous_journal->id != previous->journal_head()) {
                    co_return false;
                }
                auto built = build_interrupted_cancellation(*this, *previous, *previous_journal,
                                                            std::move(pending_input), std::move(pending_effect));
                auto&       terminal         = built.terminal;
                auto&       cancelled_result = built.result;
                const auto published = (co_await compare_publish_with_lineage_async(
                    *transitions, owner_scope, previous->journal_head(), std::move(built.publication)));
                if (published != ProgramTransitionPublishResult::Published) {
                    co_return false;
                }
                try {
                    if (cancelled_result.status() == ProgramTerminalStatus::Failed) {
                        emit_lifecycle_hook(HookPhase::RunFailed, terminal,
                                            ProgramTerminalStatus::Failed, false);
                    } else {
                        emit_lifecycle_hook(HookPhase::RunCancelled, terminal,
                                            ProgramTerminalStatus::Cancelled, false);
                    }
                } catch (...) {}
                ProgramResult            callback_result = cancelled_result;
                CompletionCallback       callback;
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
                abort_handoff();
                co_await abort_graph_migration_async();
                co_await cancel_children_async(cause, held_here);
                if (callback) {
                    try {
                        co_await callback(callback_result);
                    } catch (...) {}
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
                co_return true;
            }
        }
        {
            std::lock_guard lock(mutex_);
            if (result_ || terminal_decided_ || cancellation_cause_ != CancellationCause::None)
                co_return false;
            cancellation_cause_ = cause;
        }
        cancel_token->cancel();
        abort_handoff();
        co_await abort_graph_migration_async();
        co_await cancel_children_async(cause, held_here);
        co_return true;
    } catch (...) {
        if (failure) *failure = std::current_exception();
    }
    // A store failed: stop the run in memory, which needs no store.
    co_return cancel_without_waiting(cause, false);
}

// The cancellation that waits for nothing: an executing run stops in memory at once, and what needs the store (the
// release of a held graph migration, the cancellation of the children, and, with `durable`, the Cancelled state of an
// interrupted run) is a task of the executor. The result says whether the call accepted the cancellation: true when it
// stopped an executing run, or scheduled the Cancelled state of an interrupted one; false when the run is over or
// cancelled already, when that state is already scheduled, or when the executor cannot take the task.
bool RunControl::cancel_without_waiting(CancellationCause cause, bool durable) noexcept {
    bool applied     = false;
    bool interrupted = false;
    try {
        std::lock_guard lock(mutex_);
        if (!result_ && !terminal_decided_ && cancellation_cause_ == CancellationCause::None) {
            cancellation_cause_ = cause;
            applied             = true;
        } else {
            // A run that paused holds the Interrupted result that its terminal publication left (an AmbiguousEffect run
            // does not: cancel_async leaves that state as it is). The result is the handle's own view, which can be
            // stale: the store decides when the task runs.
            interrupted = result_ && result_->status() == ProgramTerminalStatus::Interrupted;
        }
    } catch (...) {
    }
    cancel_token->cancel();
    abort_handoff();
    abort_graph_migration();
    if (applied) {
        cancel_children_in_background(cause);
        return true;
    }
    // An interrupted run is not executing: its Cancelled state is the store's to record.
    return durable && interrupted && schedule_interrupted_cancellation(cause);
}

// Schedules the Cancelled state of an interrupted run as a task of the executor, and returns whether it did. The claim
// on the run (PendingCancellations) is taken first, so that a second call schedules nothing and a resume that starts
// afterwards is refused until the task has ended.
bool RunControl::schedule_interrupted_cancellation(CancellationCause cause) noexcept {
    try {
        std::shared_ptr<PendingCancellations> pending;
        {
            std::lock_guard lock(mutex_);
            pending = pending_cancellations_;
        }
        PendingCancellations::Claim claim;
        if (pending) {
            claim = pending->begin(owner_scope, run_id);
            if (!claim) return false;
        }
        // The runtime's executor does not run work inline when the host gave it (see Impl), so on a host-pumped executor
        // the task is queued, in a turn of the loop as well: it has taken no lock and read no record when this returns.
        // The claim above is what keeps a resume that is called next from overtaking it.
        asio::co_spawn(
            scheduler->attempts,
            [keep = shared_from_this(), cause, claim = std::move(claim)]() mutable -> asio::awaitable<void> {
                co_await record_scheduled_cancellation(std::move(keep), cause, std::move(claim));
            },
            asio::detached);
        return true;
    } catch (...) {
        return false;
    }
}

// The task that records the Cancelled state. Nothing can receive its error, so a cancellation that records nothing is
// reported on the handle (a notice), and the run stays as it was: interrupted, and cancellable again.
asio::awaitable<void> RunControl::record_scheduled_cancellation(std::shared_ptr<RunControl> self,
                                                                CancellationCause cause,
                                                                PendingCancellations::Claim claim) {
    std::exception_ptr failure;
    bool               recorded = false;
    try {
        recorded = co_await self->cancel_async(cause, {}, &failure);
    } catch (...) {
        failure = std::current_exception();
    }
    claim.release();
    if (!recorded) self->report_unrecorded_cancellation(std::move(failure));
}

void RunControl::report_unrecorded_cancellation(std::exception_ptr failure) noexcept {
    try {
        {
            // Only an interrupted run is left as it was. Another path may have ended it since (a cancel_async of the
            // caller's, for one), and an executing run was stopped in memory whatever the store did.
            std::lock_guard lock(mutex_);
            if (!result_ || result_->status() != ProgramTerminalStatus::Interrupted) return;
        }
        json witness{{"run_id", run_id}, {"reason", failure ? "store_error" : "state_changed"}};
        if (failure) {
            std::string message;
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                message = error.what();
            } catch (...) {
                message = "non-standard exception";
            }
            for (auto& c : message)  // keep the text valid whatever what() held
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) c = '?';
            witness["error"] = std::move(message);
        }
        add_notice(ProgramNotice{
            "P_CANCELLATION_NOT_RECORDED",
            "A scheduled cancellation did not record the Cancelled state of an interrupted run, which is as it was",
            std::move(witness)});
    } catch (...) {
    }
}

// The synchronous cancellation. Its result says whether the call accepted the cancellation, and it does the same work as
// cancel_async. Where it may wait it waits for the store, on the calling thread, so the result is whether the state was
// written. It does not wait where waiting could not end: on a thread that runs the runtime's executor (a node body, a
// deadline) it would block a thread that a suspended operation may need, and on a host-pumped executor nothing could
// finish. There an executing run stops in memory and the Cancelled state of an interrupted run is scheduled as a task
// of the executor (cancel_without_waiting); cancel_async is the call that waits until that state is published.
bool RunControl::cancel(CancellationCause cause) noexcept {
    try {
        if (const auto next = following_successor()) return next->cancel(cause);
        if (scheduler->runs_on_attempt_thread()) return cancel_without_waiting(cause, true);
        return scheduler->run_blocking(cancel_async(cause), "ProgramHandle::cancel", "ProgramHandle::cancel_async",
                                       scheduler->stores_inline);
    } catch (...) {
        if (const auto next = following_successor()) return next->cancel(cause);
        return cancel_without_waiting(cause, true);
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

ProgramEvent RunControl::make_event(std::string_view    event_operation_id,
                                    ProgramEventKind    kind,
                                    ProgramEventPayload payload) {
    return preview_event(next_sequence_, event_operation_id, kind, std::move(payload));
}

ProgramEvent RunControl::preview_event(std::uint64_t       sequence,
                                       ProgramEventKind    kind,
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

ProgramEvent RunControl::stage_event(std::string_view    event_operation_id,
                                     ProgramEventKind    kind,
                                     ProgramEventPayload payload) {
    std::lock_guard lock(mutex_);
    auto            event = make_event(event_operation_id, kind, std::move(payload));
    events_.push_back(event);
    ++next_sequence_;
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

void RunControl::emit(std::string_view    event_operation_id,
                      ProgramEventKind    kind,
                      ProgramEventPayload payload) {
    const auto event = stage_event(event_operation_id, kind, std::move(payload));
    deliver_event(event);
}

ProgramResult RunControl::make_result(RunOutcome outcome) const {
    // Only a resumable result carries an interrupt. Fallback paths (failed
    // publication, retirement, a blocking hook) switch the status after the
    // interrupt was attached; keeping it would make the result invalid (#371).
    if (outcome.status != ProgramTerminalStatus::Interrupted &&
        outcome.status != ProgramTerminalStatus::AmbiguousEffect)
        outcome.interrupt.reset();
    return ProgramResult::create_for_runtime(ProgramResult::ConstructionData{
        outcome.status, run_id, program_version_id, bundle_id, operation_id, attempt,
        std::move(outcome.output), outcome.usage, outcome.remaining_budget,
        std::move(outcome.checkpoint), std::move(outcome.interrupt), std::move(outcome.failure),
        std::move(outcome.execution_trace), std::move(outcome.provider_budget_authority),
        std::move(outcome.monetary_records)},
        owner_scope, native_history_archive);
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
        control.materialized->root->core_name, control.materialized->root->compiled_plan_identity,
        control.core_thread_id, checkpoint.id, checkpoint.schema_version};
}

asio::awaitable<void> RunControl::complete_async(RunOutcome outcome) {
    try {
        std::vector<TerminalCleanup> terminal_cleanups;
        {
            std::lock_guard lock(mutex_);
            if (completion_claimed_) co_return;
            completion_claimed_ = true;
            terminal_decided_   = true;
            terminal_cleanups.swap(terminal_cleanups_);
            terminal_cleanups.insert(terminal_cleanups.end(),
                                     std::make_move_iterator(logical_cleanups_.begin()),
                                     std::make_move_iterator(logical_cleanups_.end()));
            logical_cleanups_.clear();
        }
        abort_handoff();
        co_await abort_graph_migration_async();
        for (auto& cleanup : terminal_cleanups) {
            if (cleanup) cleanup();
        }
        bool retired = false;
        {
            std::lock_guard lock(mutex_);
            retired = static_cast<bool>(successor_);
        }
        if (retired) {
            // Retirement is not a logical agent termination. Its children and
            // logical cleanups belong to the successor; no terminal hook or
            // checkpoint is published for the superseded execution.
            outcome.status = ProgramTerminalStatus::Cancelled;
            if (!outcome.failure || !outcome.failure->provider_outcome) outcome.failure.reset();
            auto                     result = make_result(outcome);
            std::vector<AsyncWaiter> waiters;
            {
                std::lock_guard lock(mutex_);
                result_ = std::move(result);
                sink_.reset();
                waiters.swap(waiters_);
                cv_.notify_all();
            }
            asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
                for (const auto& waiter : waiters)
                    if (auto timer = waiter.timer.lock()) timer->cancel();
            });
            co_return;
        }
        const auto current_cause = cancellation_cause();
        const bool retained_children = current_cause == CancellationCause::None &&
            (outcome.status == ProgramTerminalStatus::Interrupted ||
             outcome.status == ProgramTerminalStatus::AmbiguousEffect);
        if (!retained_children)
            co_await cancel_children_async(current_cause == CancellationCause::None
                                               ? CancellationCause::ParentTerminal : current_cause);
        ensure_terminal_checkpoint(*this, outcome);

        std::vector<ProgramEvent> terminal_events;
        bool                      checkpoint_already_published = false;
        bool                      checkpoint_already_staged    = false;
        if (outcome.checkpoint) {
            std::lock_guard lock(mutex_);
            checkpoint_already_published =
                latest_checkpoint_ && *latest_checkpoint_ == *outcome.checkpoint;
            if (!checkpoint_already_published) {
                const auto found =
                    std::find_if(events_.rbegin(), events_.rend(), [&](const auto& event) {
                        return event.kind == ProgramEventKind::CheckpointPublished &&
                               std::get<ProgramCheckpointEvent>(event.payload).checkpoint ==
                                   *outcome.checkpoint;
                    });
                if (found != events_.rend()) {
                    terminal_events.push_back(*found);
                    checkpoint_already_staged = true;
                }
            }
        }
        if (outcome.checkpoint && !checkpoint_already_published && !checkpoint_already_staged) {
            terminal_events.push_back(stage_event(ProgramEventKind::CheckpointPublished,
                                                  ProgramCheckpointEvent{*outcome.checkpoint}));
        }
        terminal_events.push_back(
            stage_event(ProgramEventKind::Terminal, ProgramTerminalEvent{outcome.status}));

        try {
            // Use the durable Program event sequence so retries reuse one hook
            // outbox identity rather than creating duplicate lifecycle effects.
            emit_lifecycle_hook(HookPhase::BeforeTerminalPublication, terminal_events.back(),
                                outcome.status,
                                outcome.status != ProgramTerminalStatus::Cancelled);
        } catch (const std::exception& error) {
            outcome.status = ProgramTerminalStatus::Failed;
            auto provider_outcome = outcome.failure
                ? std::move(outcome.failure->provider_outcome) : sp::runtime::Result{};
            auto provider_cause = outcome.failure
                ? std::move(outcome.failure->provider_cause) : std::exception_ptr{};
            json causal_witness = json::object();
            if (outcome.failure && outcome.failure->witness.is_object() &&
                outcome.failure->witness.contains("provider_error"))
                causal_witness["provider_error"] = std::move(outcome.failure->witness["provider_error"]);
            outcome.failure = ProgramFailure{"P_HOOK_BLOCKED", error.what(), "root", "", 0,
                                             std::move(causal_witness), std::move(provider_outcome),
                                             std::move(provider_cause)};
            auto failed_terminal = terminal_events.back();
            failed_terminal.id.clear();
            failed_terminal.payload = ProgramTerminalEvent{outcome.status};
            terminal_events.back() = ProgramEvent::create(std::move(failed_terminal));
            {
                std::lock_guard lock(mutex_);
                events_.back() = terminal_events.back();
            }
            // The replacement terminal event has a different status and thus a
            // different hook input. It needs its own deterministic hook pass.
            // If that pass also blocks, the already-failed event remains the
            // fail-closed terminal transition rather than reverting to the
            // original, unauthorized status.
            try {
                emit_lifecycle_hook(HookPhase::BeforeTerminalPublication,
                                    terminal_events.back(), outcome.status, false);
            } catch (...) {}
        }
        if (retained_children && outcome.status != ProgramTerminalStatus::Interrupted &&
            outcome.status != ProgramTerminalStatus::AmbiguousEffect)
            co_await cancel_children_async(CancellationCause::ParentTerminal);
        auto       result       = make_result(outcome);
        std::exception_ptr publication_cause;
        const auto publication = co_await publish_terminal_record(*this, result, publication_cause);
        const bool is_published = publication == TerminalPublicationResult::Published;
        if (!is_published) {
            const auto first_unpublished_sequence = terminal_events.front().sequence;
            {
                std::lock_guard lock(mutex_);
                while (!events_.empty() && events_.back().sequence >= first_unpublished_sequence) {
                    events_.pop_back();
                }
            }

            RunOutcome failed = std::move(outcome);
            failed.status = ProgramTerminalStatus::Failed;
            const bool journal_failed = publication == TerminalPublicationResult::JournalFailure;
            auto primary_failure = result.failure();
            if (primary_failure && primary_failure->provider_outcome) {
                failed.failure = std::move(primary_failure);
                failed.failure->witness["terminal_publication_failure"] =
                    journal_failed ? "journal" : "transition";
            } else {
                failed.failure = ProgramFailure{
                    journal_failed ? "P_JOURNAL_CONFLICT" : "P_TRANSITION_CONFLICT",
                    journal_failed ? "Program journal finalization failed"
                                   : "Atomic Program terminal transition publication failed",
                    "root", "", 0, json::object()};
            }
            if (publication_cause) {
                try {
                    std::rethrow_exception(publication_cause);
                } catch (const std::exception& error) {
                    failed.failure->witness["terminal_publication_cause"] =
                        json{{"message", error.what()}};
                } catch (...) {
                    failed.failure->witness["terminal_publication_cause"] =
                        json{{"kind", "non_standard_exception"}};
                }
                if (failed.failure->provider_outcome) {
                    try {
                        std::rethrow_exception(publication_cause);
                    } catch (...) {
                        try {
                            std::throw_with_nested(ProviderOutcomeError(
                                "Program terminal publication failed",
                                failed.failure->provider_outcome,
                                failed.failure->provider_cause));
                        } catch (...) {
                            failed.failure->provider_cause = std::current_exception();
                        }
                    }
                }
            }
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
            if (result.status() == ProgramTerminalStatus::Failed) {
                try {
                    emit_lifecycle_hook(HookPhase::RunFailed, terminal_events.back(), result.status(), false);
                } catch (...) {}
            } else if (result.status() == ProgramTerminalStatus::Cancelled) {
                try {
                    emit_lifecycle_hook(HookPhase::RunCancelled, terminal_events.back(), result.status(), false);
                } catch (...) {}
            }
        }
        {
            std::lock_guard lock(mutex_);
            sink_.reset();
        }

        ProgramResult            callback_result = result;
        CompletionCallback       callback;
        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            callback = completion_callback_;
        }
        if (callback) {
            try {
                co_await callback(callback_result);
            } catch (...) {}
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
        fail_closed(std::current_exception());
    }
}

void RunControl::fail_closed(std::exception_ptr error) noexcept {
    // An exception escaping terminalization must not take the host process
    // down (#371). Deliver an internal failure to waiters; the durable record
    // keeps whatever was last published.
    try {
        std::string message = "Program terminalization failed";
        try {
            if (error) std::rethrow_exception(error);
        } catch (const std::exception& failure) {
            message += ": ";
            message += failure.what();
        } catch (...) {}
        for (auto& c : message)  // keep the failure text valid whatever what() held
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e)
                c = '?';
        RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = granted_budget;
        failed.failure =
            ProgramFailure{"P_RUNTIME_INTERNAL", std::move(message), "root", "", 0, json::object()};
        auto                     result = make_result(std::move(failed));
        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            if (result_) return;
            result_ = std::move(result);
            sink_.reset();
            waiters.swap(waiters_);
            cv_.notify_all();
        }
        asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
            for (const auto& waiter : waiters)
                if (auto timer = waiter.timer.lock()) timer->cancel();
        });
    } catch (...) {
        std::terminate();  // not even a minimal failure could be delivered
    }
}

ProgramResult RunControl::wait() const {
    std::unique_lock lock(mutex_);
    if (!result_.has_value() && !(follow_successor_ && successor_))
        scheduler->require_wait("ProgramHandle::wait", "ProgramHandle::wait_async");
    cv_.wait(lock, [this] { return result_.has_value() || (follow_successor_ && successor_); });
    if (follow_successor_ && successor_) {
        auto next = successor_;
        lock.unlock();
        return next->wait();
    }
    return *result_;
}
asio::awaitable<ProgramResult> RunControl::wait_async() const {
    return wait_async_with_control(shared_from_this());
}
asio::awaitable<ProgramResult> RunControl::wait_async_with_control(
    std::shared_ptr<const RunControl> control) {
    auto timer = std::make_shared<async::HostTimer>(control->waiter_strand);
    timer->arm_never();
    co_await asio::co_spawn(
        control->waiter_strand,
        [control, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(control->mutex_);
                if (control->result_ || (control->follow_successor_ && control->successor_))
                    co_return;
                control->waiters_.push_back(AsyncWaiter{timer});
            }
            asio::error_code error;
            co_await         timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);
    std::shared_ptr<RunControl>  next;
    std::optional<ProgramResult> result;
    {
        std::lock_guard lock(control->mutex_);
        if (control->follow_successor_) next = control->successor_;
        if (!next) result = control->result_;
    }
    if (next) co_return co_await next->wait_async();
    if (!result) throw std::runtime_error("Program wait ended before terminal publication");
    co_return* result;
}
std::optional<ProgramResult> RunControl::try_result() const {
    std::unique_lock lock(mutex_);
    if (follow_successor_ && successor_) {
        auto next = successor_;
        lock.unlock();
        return next->try_result();
    }
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

void RunControl::set_settled_branches(json branches) {
    std::lock_guard lock(mutex_);
    settled_branches_ = branches.is_object() && !branches.empty()
                            ? std::optional<json>(std::move(branches))
                            : std::nullopt;
}

std::optional<json> RunControl::settled_branches() const {
    std::lock_guard lock(mutex_);
    return settled_branches_;
}

asio::awaitable<std::optional<ExactProgramHandoff>> RunControl::latest_handoff_async() const {
    const auto record  = (co_await transitions->load(owner_scope, run_id));
    const auto journal = (co_await transitions->latest(owner_scope, run_id));
    const auto lineage = (co_await transitions->load_run_lineage(owner_scope, run_id));
    if (!record || !journal || !lineage || record->journal_head() != journal->id ||
        lineage->active_run_record_id() != record->id() ||
        lineage->active_journal_head() != record->journal_head() ||
        lineage->inflight_reservation() != RunBudget{} ||
        record->continuation().state != ContinuationState::Running) {
        co_return std::nullopt;
    }
    const auto generation = (co_await transitions->load_generation(
        owner_scope, lineage->lineage_id(), lineage->active_generation()));
    if (!generation || !does_program_run_generation_bind(*generation, *lineage, *record)) {
        co_return std::nullopt;
    }
    const auto commands = (co_await transitions->load_javascript_commands(owner_scope, run_id));
    if (commands.empty()) co_return std::nullopt;
    const auto& checkpoint = commands.back();
    const auto  terminal   = checkpoint.terminal_result();
    const auto  arguments  = checkpoint.command().arguments();
    if (!checkpoint.completed() ||
        checkpoint.command().kind() != JavaScriptCommandKind::Checkpoint || !terminal ||
        !terminal->is_object() || !terminal->contains("status") ||
        terminal->at("status") != "completed" || !terminal->contains("output") ||
        !arguments.is_object() || !arguments.contains("value") ||
        detail::canonical_json_bytes(terminal->at("output")) !=
            detail::canonical_json_bytes(arguments.at("value"))) {
        co_return std::nullopt;
    }
    co_return ExactProgramHandoff{
        ExactProgramHandoffReference{run_id, record->journal_head(), checkpoint.coordinate_id(),
                                     checkpoint.id()},
        detail::owned_json_copy(arguments.at("value"))};
}

std::optional<ExactProgramHandoff> RunControl::latest_handoff() const {
    return scheduler->run_blocking(latest_handoff_async(), "ProgramHandle::latest_handoff",
                                   "ProgramHandle::latest_handoff_async", scheduler->stores_inline);
}

void RunControl::set_pending_cancellations(std::shared_ptr<PendingCancellations> pending) {
    std::lock_guard lock(mutex_);
    pending_cancellations_ = std::move(pending);
}

void RunControl::set_handoff_coordination_mutex(
    std::shared_ptr<ReentrantOperationLock> coordination_mutex,
    std::function<bool(const RunControl*)> admission) {
    if (!coordination_mutex || !admission) {
        throw std::invalid_argument("Program handoff coordination must be complete");
    }
    std::lock_guard lock(mutex_);
    if (handoff_coordination_mutex_ && handoff_coordination_mutex_ != coordination_mutex) {
        throw std::logic_error("Program handoff coordination mutex is already configured");
    }
    handoff_coordination_mutex_ = std::move(coordination_mutex);
    handoff_admission_          = std::move(admission);
}

std::uint64_t RunControl::arm_handoff(const std::function<bool(const RunControl*)>& admission) {
    if (!admission(this)) {
        throw std::runtime_error(
            "Program handoff requires one process-local execution control");
    }
    std::lock_guard lock(mutex_);
    if (result_ || terminal_decided_ || cancellation_cause_ != CancellationCause::None) {
        throw std::runtime_error("Program cannot arm a handoff after termination or cancellation");
    }
    if (handoff_request_id_ != 0) {
        throw std::logic_error("Program already has an outstanding handoff request");
    }
    if (next_handoff_request_id_ == 0) {
        throw std::overflow_error("Program handoff request identity overflowed");
    }
    handoff_request_id_ = next_handoff_request_id_++;
    held_handoff_.reset();
    return handoff_request_id_;
}

std::uint64_t RunControl::request_handoff() {
    // The lock may be held by an operation that is suspended on a store; this thread must not be the one it needs.
    scheduler->require_synchronous_call("ProgramHandle::next_handoff", "ProgramHandle::next_handoff_async");
    if (scheduler->runs_on_attempt_thread())
        throw ProgramBlockingCallRefused(
            "ProgramHandle::next_handoff", "ProgramHandle::next_handoff_async",
            "it is called on a thread that runs the runtime's executor, and the lock it takes may be held by an "
            "operation that is waiting for that thread");
    std::shared_ptr<ReentrantOperationLock> coordination_mutex;
    std::function<bool(const RunControl*)> admission;
    {
        std::lock_guard lock(mutex_);
        coordination_mutex = handoff_coordination_mutex_;
        admission           = handoff_admission_;
    }
    if (!coordination_mutex || !admission) {
        throw std::logic_error("Program handoff coordination is not configured");
    }
    auto coordination_lock = coordination_mutex->lock_blocking();
    return arm_handoff(admission);
}

asio::awaitable<std::uint64_t> RunControl::request_handoff_async() {
    std::shared_ptr<ReentrantOperationLock> coordination_mutex;
    std::function<bool(const RunControl*)> admission;
    {
        std::lock_guard lock(mutex_);
        coordination_mutex = handoff_coordination_mutex_;
        admission           = handoff_admission_;
    }
    if (!coordination_mutex || !admission) {
        throw std::logic_error("Program handoff coordination is not configured");
    }
    auto coordination_lock = co_await coordination_mutex->lock_async();
    co_return arm_handoff(admission);
}

HeldProgramHandoff RunControl::wait_handoff(std::uint64_t request_id) {
    std::unique_lock lock(mutex_);
    if (!(handoff_request_id_ != request_id || held_handoff_ || result_ ||
          cancellation_cause_ != CancellationCause::None))
        scheduler->require_wait("ProgramHandoff::reference", "ProgramHandoff::wait_async");
    handoff_cv_.wait(lock, [&] {
        return handoff_request_id_ != request_id || held_handoff_ || result_ ||
               cancellation_cause_ != CancellationCause::None;
    });
    if (handoff_request_id_ == request_id && held_handoff_) {
        return HeldProgramHandoff{request_id, *held_handoff_};
    }
    throw std::runtime_error("Program handoff request ended before reaching a checkpoint");
}

asio::awaitable<HeldProgramHandoff> RunControl::wait_handoff_async(
    std::uint64_t request_id) {
    auto       control    = shared_from_this();
    auto       timer      = std::make_shared<async::HostTimer>(waiter_strand);
    timer->arm_never();
    co_await asio::co_spawn(
        waiter_strand,
        [control, request_id, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(control->mutex_);
                if (control->handoff_request_id_ != request_id || control->held_handoff_ ||
                    control->result_ ||
                    control->cancellation_cause_ != CancellationCause::None) {
                    co_return;
                }
                control->handoff_waiters_.push_back(AsyncWaiter{timer});
            }
            asio::error_code error;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);

    std::lock_guard lock(mutex_);
    if (handoff_request_id_ == request_id && held_handoff_) {
        co_return HeldProgramHandoff{request_id, *held_handoff_};
    }
    throw std::runtime_error("Program handoff request ended before reaching a checkpoint");
}

bool RunControl::has_active_handoff_request() const noexcept {
    std::lock_guard lock(mutex_);
    return handoff_request_id_ != 0;
}

bool RunControl::has_held_handoff() const noexcept {
    std::lock_guard lock(mutex_);
    return handoff_request_id_ != 0 && held_handoff_.has_value();
}

asio::awaitable<void> RunControl::reach_latest_handoff_if_requested() {
    {
        std::lock_guard lock(mutex_);
        if (handoff_request_id_ == 0 || held_handoff_ || result_ || terminal_decided_ ||
            cancellation_cause_ != CancellationCause::None) {
            co_return;
        }
    }

    const auto handoff = co_await latest_handoff_async();
    if (!handoff) co_return;
    std::vector<AsyncWaiter> waiters;
    {
        std::lock_guard lock(mutex_);
        if (handoff_request_id_ == 0 || held_handoff_ || result_ || terminal_decided_ ||
            cancellation_cause_ != CancellationCause::None) {
            co_return;
        }
        held_handoff_ = *handoff;
        waiters.swap(handoff_waiters_);
        handoff_cv_.notify_all();
    }
    asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
        for (const auto& waiter : waiters) {
            if (auto waiting = waiter.timer.lock()) waiting->cancel();
        }
    });
}

asio::awaitable<void> RunControl::hold_latest_handoff_if_requested(std::uint64_t ordinal) {
    if (checkpoint_handler) co_await checkpoint_handler(ordinal);
    co_await reach_latest_handoff_if_requested();

    auto control = shared_from_this();
    auto timer = std::make_shared<async::HostTimer>(waiter_strand);
    timer->arm_never();
    // Register and initiate the wait on the same strand used by release.
    // A pool executor alone permits expiry mutation to race async_wait.
    co_await asio::co_spawn(
        waiter_strand,
        [control, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(control->mutex_);
                if (control->handoff_request_id_ == 0 || !control->held_handoff_ ||
                    control->result_ || control->terminal_decided_ ||
                    control->cancellation_cause_ != CancellationCause::None) {
                    co_return;
                }
                control->handoff_release_waiter_ = timer;
            }
            asio::error_code error;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);
    co_return;
}

void RunControl::release_handoff(std::uint64_t request_id) noexcept {
    try {
        std::shared_ptr<async::HostTimer> release_waiter;
        std::vector<AsyncWaiter>            waiters;
        {
            std::lock_guard lock(mutex_);
            if (handoff_request_id_ != request_id) return;
            handoff_request_id_ = 0;
            held_handoff_.reset();
            release_waiter = handoff_release_waiter_.lock();
            handoff_release_waiter_.reset();
            waiters.swap(handoff_waiters_);
            handoff_cv_.notify_all();
        }
        if (release_waiter) {
            asio::dispatch(release_waiter->get_executor(),
                           [release_waiter] {
                               try {
                                   release_waiter->arm_at(
                                       (std::chrono::steady_clock::time_point::min)());
                               } catch (...) {}
                           });
        }
        asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
            for (const auto& waiter : waiters) {
                if (auto waiting = waiter.timer.lock()) waiting->cancel();
            }
        });
    } catch (...) {}
}

void RunControl::abort_handoff() noexcept {
    try {
        std::uint64_t request_id = 0;
        {
            std::lock_guard lock(mutex_);
            request_id = handoff_request_id_;
        }
        if (request_id != 0) release_handoff(request_id);
    } catch (...) {}
}

void RunControl::renew_graph_safe_point_request_locked() {
    if (!materialized || !supports_live_graph_migration(*materialized)) {
        graph_safe_point_request_.reset();
        return;
    }
    graph_safe_point_request_ = std::make_shared<graph::GraphSafePointRequest>(
        graph::GraphGenerationIdentity{materialized->root->core_name,
                                       materialized->root->compiled_plan_identity});
}

std::shared_ptr<graph::GraphSafePointRequest>
RunControl::graph_safe_point_request() const {
    std::lock_guard lock(mutex_);
    return graph_safe_point_request_;
}

std::uint64_t RunControl::arm_graph_migration(const std::function<bool(const RunControl*)>& admission) {
    if (!admission(this)) {
        throw std::runtime_error(
            "Program graph migration requires one process-local execution control");
    }

    std::lock_guard lock(mutex_);
    if (result_ || terminal_decided_ || cancellation_cause_ != CancellationCause::None) {
        throw std::runtime_error(
            "Program cannot arm graph migration after termination or cancellation");
    }
    if (handoff_request_id_ != 0 || graph_migration_request_id_ != 0) {
        throw std::logic_error("Program already has an outstanding migration request");
    }
    if (!graph_safe_point_request_) {
        throw std::runtime_error(
            "Program topology does not support live graph migration");
    }
    if (next_graph_migration_request_id_ == 0) {
        throw std::overflow_error("Program graph migration request identity overflowed");
    }
    if (!graph_safe_point_request_->request()) {
        throw std::runtime_error(
            "Program root Core call can no longer reach a graph migration safe point");
    }
    graph_migration_request_id_ = next_graph_migration_request_id_++;
    finished_graph_migration_request_id_ = 0;
    held_graph_migration_.reset();
    graph_migration_error_.clear();
    return graph_migration_request_id_;
}

std::uint64_t RunControl::request_graph_migration() {
    std::shared_ptr<ReentrantOperationLock> coordination_mutex;
    std::function<bool(const RunControl*)> admission;
    {
        std::lock_guard lock(mutex_);
        coordination_mutex = handoff_coordination_mutex_;
        admission = handoff_admission_;
    }
    if (!coordination_mutex || !admission) {
        throw std::logic_error("Program graph migration coordination is not configured");
    }
    auto coordination_lock = coordination_mutex->lock_blocking();
    return arm_graph_migration(admission);
}

asio::awaitable<std::uint64_t> RunControl::request_graph_migration_async() {
    std::shared_ptr<ReentrantOperationLock> coordination_mutex;
    std::function<bool(const RunControl*)> admission;
    {
        std::lock_guard lock(mutex_);
        coordination_mutex = handoff_coordination_mutex_;
        admission = handoff_admission_;
    }
    if (!coordination_mutex || !admission) {
        throw std::logic_error("Program graph migration coordination is not configured");
    }
    auto coordination_lock = co_await coordination_mutex->lock_async();
    co_return arm_graph_migration(admission);
}

GraphMigrationCapsule RunControl::wait_graph_migration(std::uint64_t request_id) {
    std::unique_lock lock(mutex_);
    if (!(graph_migration_request_id_ != request_id || held_graph_migration_ ||
          finished_graph_migration_request_id_ == request_id || result_ ||
          cancellation_cause_ != CancellationCause::None))
        scheduler->require_wait("ProgramRuntime::migrate_graph", "ProgramRuntime::migrate_graph_async");
    graph_migration_cv_.wait(lock, [&] {
        return graph_migration_request_id_ != request_id || held_graph_migration_ ||
               finished_graph_migration_request_id_ == request_id || result_ ||
               cancellation_cause_ != CancellationCause::None;
    });
    if (graph_migration_request_id_ == request_id && held_graph_migration_) {
        return *held_graph_migration_;
    }
    if (finished_graph_migration_request_id_ == request_id &&
        !graph_migration_error_.empty()) {
        throw std::runtime_error(graph_migration_error_);
    }
    throw std::runtime_error(
        "Program graph migration ended before reaching a durable safe point");
}

void RunControl::notify_graph_migration_locked(std::vector<AsyncWaiter>& async_waiters) {
    graph_migration_cv_.notify_all();
    for (auto& waiter : graph_migration_waiters_) async_waiters.push_back(std::move(waiter));
    graph_migration_waiters_.clear();
}

void RunControl::wake_async_waiters(std::vector<AsyncWaiter> waiters) const {
    if (waiters.empty()) return;
    asio::dispatch(waiter_strand, [waiters = std::move(waiters)] {
        for (const auto& waiter : waiters)
            if (auto timer = waiter.timer.lock()) timer->cancel();
    });
}

asio::awaitable<GraphMigrationCapsule> RunControl::wait_graph_migration_async(std::uint64_t request_id) {
    auto control = shared_from_this();
    auto timer   = std::make_shared<async::HostTimer>(waiter_strand);
    timer->arm_never();
    co_await asio::co_spawn(
        waiter_strand,
        [control, request_id, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(control->mutex_);
                if (control->graph_migration_request_id_ != request_id ||
                    control->held_graph_migration_ ||
                    control->finished_graph_migration_request_id_ == request_id || control->result_ ||
                    control->cancellation_cause_ != CancellationCause::None) {
                    co_return;
                }
                control->graph_migration_waiters_.push_back(AsyncWaiter{timer});
            }
            asio::error_code error;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);

    std::lock_guard lock(mutex_);
    if (graph_migration_request_id_ == request_id && held_graph_migration_) {
        co_return *held_graph_migration_;
    }
    if (finished_graph_migration_request_id_ == request_id && !graph_migration_error_.empty()) {
        throw std::runtime_error(graph_migration_error_);
    }
    throw std::runtime_error("Program graph migration ended before reaching a durable safe point");
}

asio::awaitable<void> RunControl::hold_graph_migration(
    const graph::GraphSafePoint& safe_point,
    RunBudget remaining_budget) {
    std::uint64_t request_id = 0;
    {
        std::lock_guard lock(mutex_);
        if (graph_migration_request_id_ == 0 || !graph_safe_point_request_ ||
            !graph_safe_point_request_->captured()) {
            co_return;
        }
        request_id = graph_migration_request_id_;
    }

    const auto fail_request = [&](std::string message) {
        std::vector<AsyncWaiter> waiters;
        {
            std::lock_guard lock(mutex_);
            if (graph_migration_request_id_ != request_id) return;
            graph_migration_request_id_ = 0;
            finished_graph_migration_request_id_ = request_id;
            held_graph_migration_.reset();
            graph_migration_error_ = std::move(message);
            renew_graph_safe_point_request_locked();
            notify_graph_migration_locked(waiters);
        }
        wake_async_waiters(std::move(waiters));
    };
    const auto fence_request = [&](std::string message) -> asio::awaitable<void> {
        if (const auto local_lease = execution_lease()) {
            try {
                if (!(co_await transitions->load_execution_lease(owner_scope, run_id))) {
                    clear_execution_lease(local_lease->id());
                }
            } catch (...) {}
        }
        {
            std::vector<AsyncWaiter> waiters;
            {
                std::lock_guard lock(mutex_);
                if (graph_migration_request_id_ == request_id) {
                    graph_migration_request_id_ = 0;
                    finished_graph_migration_request_id_ = request_id;
                    held_graph_migration_.reset();
                    graph_migration_error_ = std::move(message);
                    notify_graph_migration_locked(waiters);
                }
            }
            wake_async_waiters(std::move(waiters));
        }
        (void)co_await cancel_async(CancellationCause::ParentTerminal);
        RunOutcome cancelled;
        cancelled.status = ProgramTerminalStatus::Cancelled;
        cancelled.remaining_budget = remaining_budget;
        cancelled.usage.wall_time_ms =
            granted_budget.wall_time_ms - remaining_budget.wall_time_ms;
        cancelled.usage.model_tokens =
            granted_budget.model_tokens - remaining_budget.model_tokens;
        cancelled.usage.monetary_microunits =
            granted_budget.monetary_microunits - remaining_budget.monetary_microunits;
        cancelled.usage.program_operations =
            granted_budget.max_program_operations -
            remaining_budget.max_program_operations;
        cancelled.usage.core_steps =
            granted_budget.max_core_steps - remaining_budget.max_core_steps;
        cancelled.checkpoint = CoreCheckpointIdentity{
            safe_point.generation().core_name,
            safe_point.generation().core_generation_id,
            safe_point.checkpoint().thread_id,
            safe_point.checkpoint().id,
            safe_point.checkpoint().schema_version};
        co_await complete_async(std::move(cancelled));
    };

    bool source_checkpoint_committed = false;
    bool uncertain_publication = false;
    bool checkpoint_event_adopted = false;
    std::optional<ProgramEvent> safe_point_event;
    std::optional<std::string>  failure;  // set by the handlers below, which cannot suspend
    try {
        const auto& checkpoint = safe_point.checkpoint();
        const CoreCheckpointIdentity checkpoint_identity{
            safe_point.generation().core_name,
            safe_point.generation().core_generation_id,
            checkpoint.thread_id,
            checkpoint.id,
            checkpoint.schema_version};
        const auto checkpoint_content_id =
            graph_migration_checkpoint_content_id(checkpoint);
        const auto previous = co_await async_transitions().load(owner_scope, run_id);
        const auto previous_journal = co_await async_transitions().latest(owner_scope, run_id);
        std::optional<ActiveRunLineage> active;
        if (previous) active = co_await load_active_run_lineage_async(async_transitions(), *previous);
        const auto expected_execution_lease = execution_lease();
        if (!previous || !previous_journal || !active ||
            !expected_execution_lease ||
            previous->journal_head() != previous_journal->id ||
            previous->continuation().state != ContinuationState::Running ||
            previous->continuation().attempt != attempt || previous->child_depth() != 0 ||
            !previous->invocation().parent_run_id.empty() || previous->pending_input() ||
            previous->pending_effect() || previous->terminal_result() ||
            !previous->children().empty() || previous->effect_sequence() != 0 ||
            active->lineage.inflight_reservation() != RunBudget{} ||
            active->lineage.committed_descendant_budget() != RunBudget{} ||
            checkpoint_identity.core_name != materialized->root->core_name ||
            checkpoint_identity.core_generation_id !=
                materialized->root->compiled_plan_identity ||
            checkpoint_identity.core_thread_id != core_thread_id) {
            throw std::runtime_error(
                "Program source is not at an admitted graph migration boundary");
        }

        auto events = events_after(previous->event_sequence());
        std::uint64_t event_sequence = previous->event_sequence();
        for (const auto& event : events) {
            if (event.sequence != ++event_sequence) {
                throw std::runtime_error(
                    "Program graph migration events are not contiguous");
            }
        }
        auto checkpoint_event = preview_event(
            event_sequence + 1, ProgramEventKind::CheckpointPublished,
            ProgramCheckpointEvent{checkpoint_identity});
        safe_point_event = checkpoint_event;
        events.push_back(checkpoint_event);

        const auto timestamp = std::max(
            {now_ms(), previous->updated_at_ms(), checkpoint.timestamp});
        auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), run_id, program_version_id, bundle_id,
            previous_journal->sequence + 1, previous->continuation(), remaining_budget,
            RunBudget{}, checkpoint_identity, timestamp});
        ProgramRunRecordData data;
        data.owner_scope = previous->owner_scope();
        data.run_id = previous->run_id();
        data.logical_run_id              = previous->logical_run_id();
        data.program_version_id = previous->program_version_id();
        data.bundle_id = previous->bundle_id();
        data.binding_fingerprint = previous->binding_fingerprint();
        data.invocation = previous->invocation();
        data.child_depth = previous->child_depth();
        data.continuation = previous->continuation();
        data.remaining_budget = remaining_budget;
        data.exact_checkpoint = checkpoint_identity;
        data.exact_checkpoint_content_id = checkpoint_content_id;
        data.pending_input = previous->pending_input();
        data.pending_effect = previous->pending_effect();
        data.terminal_result = previous->terminal_result();
        data.fork_receipt = previous->fork_receipt();
        data.children = previous->children();
        data.journal_head = journal.id;
        data.fork_source_run_id = previous->fork_source_run_id();
        data.fork_source_program_version_id =
            previous->fork_source_program_version_id();
        data.fork_source_checkpoint_id = previous->fork_source_checkpoint_id();
        data.recorded_binding_set_fingerprint =
            previous->recorded_binding_set_fingerprint();
        data.event_sequence = checkpoint_event.sequence;
        data.effect_sequence = previous->effect_sequence();
        data.created_at_ms = previous->created_at_ms();
        data.updated_at_ms = timestamp;
        auto publication = ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal),
            std::move(events), {}};
        const auto safe_point_evidence = ProgramGraphSafePointEvidence(
            materialized->bundle, materialized->version, safe_point);
        publication = co_await attach_run_lineage_async(async_transitions(), std::move(publication));
        const auto candidate_run = publication.run_record;
        const auto candidate_journal = publication.journal_record;
        const auto candidate_lineage = publication.run_lineage;
        if (!candidate_lineage) {
            throw std::runtime_error(
                "Program graph migration source has no durable lineage head");
        }
        const auto candidate_capsule = GraphMigrationCapsule::seal(
            active->generation, *candidate_lineage, materialized->version, safe_point);
        const auto publication_state = [&]() -> asio::awaitable<int> {
            try {
                const auto durable = co_await transitions->load(owner_scope, run_id);
                const auto durable_journal = co_await transitions->latest(owner_scope, run_id);
                const auto durable_lineage = co_await transitions->load_run_lineage(owner_scope, run_id);
                if (durable && durable_journal && durable_lineage && candidate_lineage &&
                    durable->id() == candidate_run.id() &&
                    durable_journal->id == candidate_journal.id &&
                    durable_lineage->id() == candidate_lineage->id()) {
                    co_return 1;
                }
                if (durable && durable_journal && durable_lineage &&
                    durable->id() == previous->id() &&
                    durable_journal->id == previous_journal->id &&
                    durable_lineage->id() == active->lineage.id()) {
                    co_return 0;
                }
            } catch (...) {}
            co_return -1;
        };
        ProgramTransitionPublishResult published = ProgramTransitionPublishResult::Conflict;
        std::exception_ptr             publish_failure;
        try {
            published = co_await async_transitions().compare_publish_graph_safe_point(
                owner_scope, previous->journal_head(), std::move(publication),
                safe_point_evidence, candidate_capsule, *expected_execution_lease);
        } catch (...) {
            publish_failure = std::current_exception();
        }
        if (publish_failure) {
            const auto state = co_await publication_state();
            if (state == 1) {
                published = ProgramTransitionPublishResult::AlreadyPresent;
            } else {
                uncertain_publication = state != 0;
                std::rethrow_exception(publish_failure);
            }
        }
        if (published != ProgramTransitionPublishResult::Published &&
            published != ProgramTransitionPublishResult::AlreadyPresent) {
            const auto state = co_await publication_state();
            if (state == 1) {
                published = ProgramTransitionPublishResult::AlreadyPresent;
            } else {
                uncertain_publication = state != 0;
            }
        }
        if (published != ProgramTransitionPublishResult::Published &&
            published != ProgramTransitionPublishResult::AlreadyPresent) {
            throw std::runtime_error(
                "Program graph migration source checkpoint lost its transition CAS");
        }
        source_checkpoint_committed = true;
        clear_execution_lease(expected_execution_lease->id());
        adopt_published_event(checkpoint_event);
        checkpoint_event_adopted = true;
        {
            std::lock_guard lock(mutex_);
            latest_checkpoint_ = checkpoint_identity;
        }

        const auto durable = co_await async_transitions().load(owner_scope, run_id);
        std::optional<ActiveRunLineage> durable_active;
        if (durable) durable_active = co_await load_active_run_lineage_async(async_transitions(), *durable);
        if (!durable || !durable_active ||
            durable->exact_checkpoint() != std::optional{checkpoint_identity} ||
            durable->exact_checkpoint_content_id() !=
                std::optional{checkpoint_content_id}) {
            throw std::runtime_error(
                "Program graph migration source checkpoint was not durable");
        }
        const auto durable_capsule = co_await async_transitions().load_graph_migration_capsule(
            owner_scope, run_id, durable_active->lineage.id());
        if (!durable_capsule || durable_capsule->id() != candidate_capsule.id()) {
            throw std::runtime_error(
                "Program graph migration source hold was not durable");
        }

        deliver_event(checkpoint_event);

        {
            std::vector<AsyncWaiter> waiters;
            {
                std::lock_guard lock(mutex_);
                if (graph_migration_request_id_ != request_id || result_ || terminal_decided_ ||
                    cancellation_cause_ != CancellationCause::None) {
                    throw std::runtime_error(
                        "Program graph migration source stopped before it could be held");
                }
                held_graph_migration_ = *durable_capsule;
                latest_checkpoint_ = checkpoint_identity;
                notify_graph_migration_locked(waiters);
            }
            wake_async_waiters(std::move(waiters));
        }
    } catch (const std::exception& error) {
        failure = std::string(error.what());
    } catch (...) {
        failure = "Program graph migration source publication failed";
    }
    if (failure) {
        if (source_checkpoint_committed || uncertain_publication) {
            if (!checkpoint_event_adopted && safe_point_event) {
                try {
                    adopt_published_event(*safe_point_event);
                    checkpoint_event_adopted = true;
                    std::lock_guard lock(mutex_);
                    latest_checkpoint_ =
                        std::get<ProgramCheckpointEvent>(safe_point_event->payload).checkpoint;
                } catch (...) {}
            }
            co_await fence_request(*failure);
        } else {
            fail_request(*failure);
        }
        co_return;
    }

    auto control = shared_from_this();
    auto timer = std::make_shared<async::HostTimer>(waiter_strand);
    timer->arm_never();
    // Registration and wait initiation must serialize with the release expiry,
    // including when the migration caller rejects before this coroutine suspends.
    co_await asio::co_spawn(
        waiter_strand,
        [control, request_id, timer]() -> asio::awaitable<void> {
            {
                std::lock_guard lock(control->mutex_);
                if (control->graph_migration_request_id_ != request_id ||
                    !control->held_graph_migration_ || control->result_ ||
                    control->terminal_decided_ ||
                    control->cancellation_cause_ != CancellationCause::None) {
                    co_return;
                }
                control->graph_migration_release_waiter_ = timer;
            }
            asio::error_code error;
            co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
        },
        asio::use_awaitable);
}

bool RunControl::has_active_graph_migration_request() const noexcept {
    std::lock_guard lock(mutex_);
    return graph_migration_request_id_ != 0;
}

bool RunControl::graph_migration_matches(
    std::uint64_t request_id,
    std::string_view capsule_id) const noexcept {
    std::lock_guard lock(mutex_);
    return graph_migration_request_id_ == request_id && held_graph_migration_ &&
           held_graph_migration_->id() == capsule_id && !result_ && !terminal_decided_ &&
           cancellation_cause_ == CancellationCause::None;
}

std::optional<ProgramExecutionLease> RunControl::execution_lease() const {
    std::lock_guard lock(mutex_);
    return execution_lease_;
}

void RunControl::set_execution_lease(ProgramExecutionLease lease) {
    std::lock_guard lock(mutex_);
    execution_lease_ = std::move(lease);
    renew_graph_safe_point_request_locked();
}

void RunControl::clear_execution_lease(std::string_view lease_id) const noexcept {
    std::lock_guard lock(mutex_);
    if (execution_lease_ && execution_lease_->id() == lease_id) {
        execution_lease_.reset();
    }
}

asio::awaitable<void> RunControl::release_graph_migration_async(std::uint64_t request_id) {
    try {
        std::optional<GraphMigrationCapsule> held_capsule;
        {
            std::lock_guard lock(mutex_);
            if (graph_migration_request_id_ == request_id && held_graph_migration_) {
                held_capsule = held_graph_migration_;
            }
        }

        bool durable_release = !held_capsule;
        if (held_capsule) {
            try {
                const auto previous = (co_await transitions->load(owner_scope, run_id));
                const auto previous_journal = (co_await transitions->latest(owner_scope, run_id));
                const auto active = previous
                    ? (co_await load_active_run_lineage_async(*transitions, *previous))
                    : std::nullopt;
                const auto durable_capsule = active
                    ? (co_await transitions->load_graph_migration_capsule(
                          owner_scope, run_id, active->lineage.id()))
                    : std::nullopt;
                if (previous && previous_journal && active && durable_capsule &&
                    durable_capsule->id() == held_capsule->id() &&
                    active->lineage.id() == held_capsule->source_lineage_head_id() &&
                    previous->journal_head() == previous_journal->id) {
                    const auto timestamp = std::max(now_ms(), previous->updated_at_ms());
                    auto remaining = previous->remaining_budget();
                    remaining.wall_time_ms = std::min(
                        remaining.wall_time_ms,
                        remaining_attempt_wall_time(granted_budget.wall_time_ms, started_at,
                                                    std::chrono::steady_clock::now()));
                    auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
                        previous->journal_head(), run_id, previous->program_version_id(),
                        previous->bundle_id(), previous_journal->sequence + 1,
                        previous->continuation(), remaining, RunBudget{},
                        previous->exact_checkpoint(), timestamp});
                    ProgramRunRecordData data;
                    data.owner_scope = previous->owner_scope();
                    data.run_id = previous->run_id();
                    data.logical_run_id      = previous->logical_run_id();
                    data.program_version_id = previous->program_version_id();
                    data.bundle_id = previous->bundle_id();
                    data.binding_fingerprint = previous->binding_fingerprint();
                    data.invocation = previous->invocation();
                    data.child_depth = previous->child_depth();
                    data.continuation = journal.continuation;
                    data.remaining_budget = remaining;
                    data.exact_checkpoint = previous->exact_checkpoint();
                    data.exact_checkpoint_content_id =
                        previous->exact_checkpoint_content_id();
                    data.attempt_resume = previous->attempt_resume();
                    data.settled_branches = previous->settled_branches();
                    data.pending_input = previous->pending_input();
                    data.pending_effect = previous->pending_effect();
                    data.terminal_result = previous->terminal_result();
                    data.fork_receipt = previous->fork_receipt();
                    data.children = previous->children();
                    data.journal_head = journal.id;
                    data.fork_source_run_id = previous->fork_source_run_id();
                    data.fork_source_program_version_id =
                        previous->fork_source_program_version_id();
                    data.fork_source_checkpoint_id = previous->fork_source_checkpoint_id();
                    data.recorded_binding_set_fingerprint =
                        previous->recorded_binding_set_fingerprint();
                    data.event_sequence = previous->event_sequence();
                    data.effect_sequence = previous->effect_sequence();
                    data.created_at_ms = previous->created_at_ms();
                    data.updated_at_ms = timestamp;
                    auto publication = (co_await attach_run_lineage_async(
                        *transitions,
                        ProgramTransitionPublication{
                            ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}}));
                    const auto next_execution_lease =
                        make_execution_lease(*materialized, publication);
                    const auto candidate_run = publication.run_record;
                    const auto candidate_lineage = publication.run_lineage;
                    ProgramTransitionPublishResult published = ProgramTransitionPublishResult::Conflict;
                    std::exception_ptr             publish_failure;
                    try {
                        published = next_execution_lease
                            ? (co_await transitions->compare_publish_execution(
                                  owner_scope, previous->journal_head(),
                                  std::move(publication), std::nullopt,
                                  next_execution_lease))
                            : ProgramTransitionPublishResult::Conflict;
                    } catch (...) {
                        publish_failure = std::current_exception();
                    }
                    if (publish_failure) {
                        // The publication may have been applied before the store failed: it did if the candidate
                        // is what is stored now.
                        const auto stored = (co_await transitions->load(owner_scope, run_id));
                        const auto lineage = (co_await transitions->load_run_lineage(owner_scope, run_id));
                        if (stored && lineage && candidate_lineage &&
                            stored->id() == candidate_run.id() &&
                            lineage->id() == candidate_lineage->id()) {
                            published = ProgramTransitionPublishResult::AlreadyPresent;
                        } else {
                            std::rethrow_exception(publish_failure);
                        }
                    }
                    durable_release =
                        published == ProgramTransitionPublishResult::Published ||
                        published == ProgramTransitionPublishResult::AlreadyPresent;
                    if (durable_release && next_execution_lease) {
                        set_execution_lease(*next_execution_lease);
                    }
                }
            } catch (...) {
                durable_release = false;
            }
        }

        std::shared_ptr<async::HostTimer> release_waiter;
        std::vector<AsyncWaiter>            waiters;
        {
            std::lock_guard lock(mutex_);
            const bool cancelled = cancellation_cause_ != CancellationCause::None ||
                                   result_ || terminal_decided_;
            if (!durable_release && !cancelled) co_return;
            if (graph_migration_request_id_ == request_id) {
                graph_migration_request_id_ = 0;
                held_graph_migration_.reset();
                graph_migration_error_.clear();
                release_waiter = graph_migration_release_waiter_.lock();
                graph_migration_release_waiter_.reset();
                renew_graph_safe_point_request_locked();
            } else if (finished_graph_migration_request_id_ == request_id) {
                finished_graph_migration_request_id_ = 0;
                graph_migration_error_.clear();
            } else {
                co_return;
            }
            notify_graph_migration_locked(waiters);
        }
        wake_async_waiters(std::move(waiters));
        if (release_waiter) {
            asio::dispatch(release_waiter->get_executor(), [release_waiter] {
                try {
                    release_waiter->arm_at(
                        (std::chrono::steady_clock::time_point::min)());
                } catch (...) {}
            });
        }
    } catch (...) {}
}

void RunControl::release_graph_migration(std::uint64_t request_id) noexcept {
    try {
        scheduler->run_blocking(release_graph_migration_async(request_id), "ProgramRuntime::migrate_graph",
                                "ProgramRuntime::migrate_graph_async", scheduler->stores_inline);
    } catch (...) {
        // Where this thread may not wait for the store (a host-pumped executor), the release is a task of the
        // executor: the held source is released in the host's turns, not left parked.
        try {
            asio::co_spawn(
                scheduler->attempts,
                [keep = shared_from_this(), request_id]() -> asio::awaitable<void> {
                    co_await keep->release_graph_migration_async(request_id);
                },
                asio::detached);
        } catch (...) {
        }
    }
}

asio::awaitable<void> RunControl::abort_graph_migration_async() {
    std::uint64_t request_id = 0;
    {
        std::lock_guard lock(mutex_);
        request_id = graph_migration_request_id_;
    }
    if (request_id != 0) co_await release_graph_migration_async(request_id);
}

void RunControl::abort_graph_migration() noexcept {
    try {
        std::uint64_t request_id = 0;
        {
            std::lock_guard lock(mutex_);
            request_id = graph_migration_request_id_;
        }
        if (request_id != 0) release_graph_migration(request_id);
    } catch (...) {}
}

asio::awaitable<ProgramRunRecord> RunControl::snapshot_async() const {
    const auto record = (co_await transitions->load(owner_scope, run_id));
    if (!record) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    if (const auto terminal = record->terminal_result()) {
        if (const auto retained = try_result(); retained && retained->id() == terminal->id())
            co_return record->with_hydrated_terminal(*retained);
        co_return record->with_hydrated_terminal(
            terminal->restore_provider_failure(owner_scope, native_history_archive));
    }
    co_return *record;
}

ProgramRunRecord RunControl::snapshot() const {
    return scheduler->run_blocking(snapshot_async(), "ProgramHandle::snapshot", "ProgramHandle::snapshot_async",
                                   scheduler->stores_inline);
}

asio::awaitable<bool> RunControl::consume_dynamic_compile() {
    for (int retry = 0; retry < 3; ++retry) {
        const auto previous         = co_await async_transitions().load(owner_scope, run_id);
        const auto previous_journal = co_await async_transitions().latest(owner_scope, run_id);
        if (!previous || !previous_journal || previous->journal_head() != previous_journal->id ||
            previous->continuation().state != ContinuationState::Running ||
            previous->continuation().attempt != attempt ||
            previous->remaining_budget().max_dynamic_compiles == 0) {
            co_return false;
        }

        const auto lineage = co_await async_transitions().load_run_lineage(owner_scope, run_id);
        if (lineage && previous->remaining_budget().max_dynamic_compiles <=
                           lineage->committed_descendant_budget().max_dynamic_compiles)
            co_return false;
        auto remaining = previous->remaining_budget();
        --remaining.max_dynamic_compiles;
        auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), run_id, program_version_id, bundle_id,
            previous_journal->sequence + 1, previous->continuation(), remaining,
            previous_journal->inflight_reservation, previous->exact_checkpoint(),
            std::max(now_ms(), previous->updated_at_ms())});

        ProgramRunRecordData data;
        data.owner_scope                      = previous->owner_scope();
        data.run_id                           = previous->run_id();
        data.logical_run_id                   = previous->logical_run_id();
        data.program_version_id               = previous->program_version_id();
        data.bundle_id                        = previous->bundle_id();
        data.binding_fingerprint              = previous->binding_fingerprint();
        data.invocation                       = previous->invocation();
        data.child_depth                      = previous->child_depth();
        data.continuation                     = journal.continuation;
        data.remaining_budget                 = remaining;
        data.exact_checkpoint                 = previous->exact_checkpoint();
        data.exact_checkpoint_content_id      = previous->exact_checkpoint_content_id();
        data.attempt_resume                   = previous->attempt_resume();
        data.settled_branches                 = previous->settled_branches();
        data.pending_input                    = previous->pending_input();
        data.pending_effect                   = previous->pending_effect();
        data.terminal_result                  = previous->terminal_result();
        data.fork_receipt                     = previous->fork_receipt();
        data.children                         = previous->children();
        data.journal_head                     = journal.id;
        data.fork_source_run_id               = previous->fork_source_run_id();
        data.fork_source_program_version_id   = previous->fork_source_program_version_id();
        data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
        data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
        data.event_sequence                   = previous->event_sequence();
        data.effect_sequence                  = previous->effect_sequence();
        data.created_at_ms                    = previous->created_at_ms();
        data.updated_at_ms                    = journal.timestamp_ms;
        auto publication = ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
        const auto published = co_await compare_publish_with_lineage_async(
            async_transitions(), owner_scope, previous->journal_head(), std::move(publication));
        if (published == ProgramTransitionPublishResult::Published ||
            published == ProgramTransitionPublishResult::AlreadyPresent) {
            limit_dynamic_compiles(remaining.max_dynamic_compiles);
            co_return true;
        }
    }
    co_return false;
}

asio::awaitable<ProgramTransitionPublishResult> RunControl::publish_command_reservation(
    RunBudget remaining_budget, RunBudget inflight_reservation) {
    remaining_budget.max_dynamic_compiles = std::min<std::uint64_t>(
        remaining_budget.max_dynamic_compiles, dynamic_compile_ceiling.load());
    const auto head = co_await async_transitions().load_run_publication_head(owner_scope, run_id);
    if (!head) co_return ProgramTransitionPublishResult::Conflict;
    const auto& previous         = head->run_record;
    const auto& previous_journal = head->journal_record;
    if (previous.journal_head() != previous_journal.id ||
        previous.continuation().state != ContinuationState::Running ||
        previous.continuation().attempt != attempt ||
        previous_journal.inflight_reservation != RunBudget{})
        co_return ProgramTransitionPublishResult::Conflict;
    auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
        previous.journal_head(), run_id, program_version_id, bundle_id,
        previous_journal.sequence + 1, previous.continuation(), std::move(remaining_budget),
        std::move(inflight_reservation), previous_journal.core_checkpoint, now_ms()});

    ProgramRunRecordData data;
    data.owner_scope                      = owner_scope;
    data.run_id                           = run_id;
    data.logical_run_id                   = logical_run_id;
    data.program_version_id               = program_version_id;
    data.bundle_id                        = bundle_id;
    data.binding_fingerprint              = previous.binding_fingerprint();
    data.invocation                       = previous.invocation();
    data.child_depth                      = previous.child_depth();
    data.children                         = previous.children();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = journal.remaining_budget;
    data.exact_checkpoint                 = previous.exact_checkpoint();
    data.exact_checkpoint_content_id      = previous.exact_checkpoint_content_id();
    data.attempt_resume                   = previous.attempt_resume();
    data.settled_branches                 = previous.settled_branches();
    data.pending_input                    = previous.pending_input();
    data.pending_effect                   = previous.pending_effect();
    data.fork_receipt                     = previous.fork_receipt();
    data.fork_source_run_id               = previous.fork_source_run_id();
    data.fork_source_program_version_id   = previous.fork_source_program_version_id();
    data.fork_source_checkpoint_id        = previous.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = previous.recorded_binding_set_fingerprint();
    data.journal_head                     = journal.id;
    data.event_sequence                   = previous.event_sequence();
    data.effect_sequence                  = previous.effect_sequence();
    data.created_at_ms                    = previous.created_at_ms();
    data.updated_at_ms                    = journal.timestamp_ms;
    auto publication = co_await attach_run_lineage_async(async_transitions(), ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                                   std::move(journal), {}, {}, std::nullopt, {}});
    const auto owned_lease = execution_lease();
    if (owned_lease)
        co_return co_await async_transitions().compare_publish_execution(
            owner_scope, previous.journal_head(), std::move(publication), owned_lease, owned_lease);
    co_return co_await async_transitions().compare_publish(owner_scope, previous.journal_head(),
                                                           std::move(publication));
}

asio::awaitable<ProgramTransitionPublishResult> RunControl::publish_javascript_command(
    std::uint64_t              command_ordinal,
    JavaScriptCommand          command,
    std::optional<std::string> effect_identity,
    std::optional<json>        terminal_result,
    std::optional<RunBudget>   remaining_budget,
    std::optional<RunBudget>   inflight_reservation,
    std::optional<json>        settled_members,
    std::optional<json>        await_started_at) {
    if (remaining_budget)
        remaining_budget->max_dynamic_compiles = std::min<std::uint64_t>(
            remaining_budget->max_dynamic_compiles, dynamic_compile_ceiling.load());
    if (!command_ordinal) co_return ProgramTransitionPublishResult::Conflict;

    for (int retry = 0; retry < 3; ++retry) {
        const auto head = co_await async_transitions().load_command_publication_head(owner_scope, run_id);
        // The default store implementation reads optimistically. A child join
        // can advance its head between reads; retry that miss before treating
        // a command-result publication as an unresolved external outcome.
        if (!head) continue;
        const auto* previous = head ? &head->run_record : nullptr;
        const auto* previous_journal = head ? &head->journal_record : nullptr;
        if (!previous || !previous_journal || previous->journal_head() != previous_journal->id ||
            previous->continuation().state != ContinuationState::Running ||
            previous->continuation().attempt != attempt) {
            co_return ProgramTransitionPublishResult::Conflict;
        }

        auto found = head->latest_command;
        if (found && found->command_ordinal() > command_ordinal) {
            // Older-coordinate retries still inspect the complete immutable history.
            // Normal append/settlement needs only the newest command, not every row.
            const auto existing = co_await async_transitions().load_javascript_commands(owner_scope, run_id);
            const auto older = std::find_if(existing.rbegin(), existing.rend(), [&](const auto& entry) {
                return entry.command_ordinal() == command_ordinal;
            });
            found = older == existing.rend() ? std::nullopt
                                             : std::optional<ProgramJavaScriptCommandJournalEntry>(*older);
        } else if (found && found->command_ordinal() != command_ordinal) {
            found.reset();
        }
        if (found) {
            if (found->bundle_id() != bundle_id ||
                detail::canonical_json_bytes(found->command().to_json()) !=
                    detail::canonical_json_bytes(command.to_json()) ||
                found->effect_identity() != effect_identity) {
                co_return ProgramTransitionPublishResult::Conflict;
            }
            if (found->completed()) {
                co_return found->terminal_result() == terminal_result
                           ? ProgramTransitionPublishResult::AlreadyPresent
                           : ProgramTransitionPublishResult::Conflict;
            }
            if (!terminal_result) {
                // The pending entries of a coordinate are cumulative: a new
                // one carries the settled members (#380) and the await starts
                // (#391) of the latest one, so the attempt that resumes or
                // recovers the command reads both from it.
                auto members = settled_members ? settled_members : found->settled_members();
                auto starts  = found->await_started_at();
                if (await_started_at) {
                    json merged = starts ? *starts : json::object();
                    for (const auto& [operation_id, started_at] : await_started_at->items()) {
                        // A recorded start never moves.
                        if (merged.contains(operation_id) && merged[operation_id] != started_at)
                            co_return ProgramTransitionPublishResult::Conflict;
                        merged[operation_id] = started_at;
                    }
                    starts = std::move(merged);
                }
                if (members == found->settled_members() && starts == found->await_started_at())
                    co_return ProgramTransitionPublishResult::AlreadyPresent;
                settled_members  = std::move(members);
                await_started_at = std::move(starts);
            }
        }
        // Settled members and await starts belong to an existing pending
        // coordinate.
        if ((settled_members || await_started_at) && (terminal_result || !found))
            co_return ProgramTransitionPublishResult::Conflict;

        const auto sequence = head->latest_command ? head->latest_command->sequence() + 1 : 1;
        ProgramJavaScriptCommandJournalEntryData entry_data{
            sequence, bundle_id, command_ordinal, command, effect_identity, terminal_result,
            settled_members};
        entry_data.await_started_at = await_started_at;
        ProgramJavaScriptCommandJournalEntry entry(std::move(entry_data));
        // A completed command becomes replayable as soon as its terminal
        // journal entry is durable. Publish every event staged by that command
        // and its newest checkpoint in the same CAS so crash recovery cannot
        // observe the result without the state needed to resume after it.
        auto events                    = terminal_result ? events_after(previous->event_sequence())
                                                         : std::vector<ProgramEvent>{};
        auto latest_command_checkpoint = previous->exact_checkpoint();
        for (auto event = events.rbegin(); event != events.rend(); ++event) {
            if (event->kind != ProgramEventKind::CheckpointPublished) continue;
            latest_command_checkpoint = std::get<ProgramCheckpointEvent>(event->payload).checkpoint;
            break;
        }
        std::vector<ProgramEvent> deferred_checkpoint_events;
        for (const auto& event : events) {
            if (event.kind == ProgramEventKind::CheckpointPublished)
                deferred_checkpoint_events.push_back(event);
        }
        auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), run_id, program_version_id, bundle_id,
            previous_journal->sequence + 1, previous->continuation(),
            remaining_budget.value_or(previous->remaining_budget()),
            inflight_reservation.value_or(previous_journal->inflight_reservation),
            latest_command_checkpoint, now_ms()});

        ProgramRunRecordData data;
        data.owner_scope                      = owner_scope;
        data.run_id                           = run_id;
        data.logical_run_id                   = logical_run_id;
        data.program_version_id               = program_version_id;
        data.bundle_id                        = bundle_id;
        data.binding_fingerprint              = previous->binding_fingerprint();
        data.invocation                       = previous->invocation();
        data.child_depth                      = previous->child_depth();
        data.children                         = previous->children();
        data.continuation                     = journal.continuation;
        data.remaining_budget                 = journal.remaining_budget;
        data.exact_checkpoint                 = latest_command_checkpoint;
        if (latest_command_checkpoint == previous->exact_checkpoint()) {
            data.exact_checkpoint_content_id = previous->exact_checkpoint_content_id();
            data.attempt_resume = previous->attempt_resume();
        }
        data.pending_input                    = previous->pending_input();
        data.pending_effect                   = previous->pending_effect();
        data.fork_receipt                     = previous->fork_receipt();
        data.fork_source_run_id               = previous->fork_source_run_id();
        data.fork_source_program_version_id   = previous->fork_source_program_version_id();
        data.fork_source_checkpoint_id        = previous->fork_source_checkpoint_id();
        data.recorded_binding_set_fingerprint = previous->recorded_binding_set_fingerprint();
        data.journal_head                     = journal.id;
        data.event_sequence  = events.empty() ? previous->event_sequence() : events.back().sequence;
        data.effect_sequence = previous->effect_sequence();
        data.created_at_ms   = previous->created_at_ms();
        data.updated_at_ms   = journal.timestamp_ms;

        auto publication = ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                                        std::move(journal),
                                                        std::move(events),
                                                        {},
                                                        std::nullopt,
                                                        {std::move(entry)}};
        publication = co_await attach_run_lineage_async(async_transitions(), std::move(publication));
        const auto owned_lease = execution_lease();
        ProgramTransitionPublishResult published;
        if (owned_lease)
            published = co_await async_transitions().compare_publish_execution(
                owner_scope, previous->journal_head(), std::move(publication), owned_lease, owned_lease);
        else
            published = co_await async_transitions().compare_publish(
                owner_scope, previous->journal_head(), std::move(publication));
        if (published == ProgramTransitionPublishResult::Published ||
            published == ProgramTransitionPublishResult::AlreadyPresent) {
            if (terminal_result && command.kind() == JavaScriptCommandKind::Checkpoint) {
                co_await reach_latest_handoff_if_requested();
            }
            if (terminal_result && latest_command_checkpoint) {
                std::lock_guard lock(mutex_);
                latest_checkpoint_ = latest_command_checkpoint;
            }
            for (const auto& event : deferred_checkpoint_events)
                deliver_event(event);
            co_return published;
        }
        // A concurrent terminal transition cannot be retried as a live
        // command.  Otherwise, reload the head and let the exact coordinate
        // check above classify an already-published append.
    }
    co_return ProgramTransitionPublishResult::Conflict;
}

}  // namespace detail
namespace {

asio::awaitable<ProgramResult> wait_for_control(std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->wait_async();
}

struct ProcessRuntimeState {
    struct ControlEntry {
        std::string                       owner_scope;
        std::string                       run_id;
        std::weak_ptr<detail::RunControl> control;
    };

    std::shared_ptr<detail::ReentrantOperationLock> transition_mutex =
        std::make_shared<detail::ReentrantOperationLock>();
    detail::PureMutex         controls_mutex;
    std::vector<ControlEntry> controls;
    // The cancellations of interrupted runs that a task of an executor has yet to record.
    std::shared_ptr<detail::PendingCancellations> pending_cancellations =
        std::make_shared<detail::PendingCancellations>();
};

std::shared_ptr<ProcessRuntimeState> process_runtime_state(const ProgramTransitionStore& transitions) {
    static std::mutex registry_mutex;
    static std::unordered_map<const ProgramTransitionStore*, std::weak_ptr<ProcessRuntimeState>>
        address_registry;
    static std::unordered_map<std::string, std::weak_ptr<ProcessRuntimeState>> key_registry;
    std::lock_guard lock(registry_mutex);
    std::erase_if(address_registry, [](const auto& entry) { return entry.second.expired(); });
    std::erase_if(key_registry, [](const auto& entry) { return entry.second.expired(); });
    const auto coordination_key = transitions.process_coordination_key();
    auto& weak = coordination_key.empty() ? address_registry[&transitions]
                                          : key_registry[coordination_key];
    if (auto state = weak.lock()) return state;
    auto state = std::make_shared<ProcessRuntimeState>();
    weak       = state;
    return state;
}

bool same_process_coordination_backend(const ProgramTransitionStore& lhs,
                                       const ProgramTransitionStore& rhs) {
    const auto lhs_key = lhs.process_coordination_key();
    const auto rhs_key = rhs.process_coordination_key();
    return !lhs_key.empty() || !rhs_key.empty() ? !lhs_key.empty() && lhs_key == rhs_key
                                                : &lhs == &rhs;
}

}  // namespace

struct ProgramHandoff::Impl {
    Impl(std::shared_ptr<detail::RunControl> source, std::uint64_t request)
        : control(std::move(source)), request_id(request) {}
    Impl(std::shared_ptr<detail::RunControl> source, detail::HeldProgramHandoff held)
        : control(std::move(source)), request_id(held.request_id), handoff(std::move(held.handoff)) {}

    const ExactProgramHandoff& resolved() const {
        {
            std::lock_guard lock(resolve_mutex);
            if (handoff) return *handoff;
        }
        auto reached = control->wait_handoff(request_id).handoff;
        std::lock_guard lock(resolve_mutex);
        if (!handoff) handoff = std::move(reached);
        return *handoff;
    }

    std::shared_ptr<detail::RunControl> control;
    std::uint64_t                       request_id = 0;
    mutable detail::PureMutex           resolve_mutex;
    mutable std::optional<ExactProgramHandoff> handoff;
    std::atomic_bool                    consumed{false};
};

ProgramHandoff::ProgramHandoff(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ProgramHandoff::ProgramHandoff(ProgramHandoff&& other) noexcept : impl_(std::move(other.impl_)) {}

ProgramHandoff& ProgramHandoff::operator=(ProgramHandoff&& other) noexcept {
    if (this == &other) return *this;
    if (impl_ && !impl_->consumed.exchange(true)) {
        impl_->control->release_handoff(impl_->request_id);
    }
    impl_ = std::move(other.impl_);
    return *this;
}

ProgramHandoff::~ProgramHandoff() {
    if (impl_ && !impl_->consumed.exchange(true)) {
        impl_->control->release_handoff(impl_->request_id);
    }
}

const ExactProgramHandoffReference& ProgramHandoff::reference() const {
    if (!*this) throw std::logic_error("Program handoff lease is empty or consumed");
    return impl_->resolved().reference;
}

json ProgramHandoff::value() const {
    if (!*this) throw std::logic_error("Program handoff lease is empty or consumed");
    return detail::owned_json_copy(impl_->resolved().value);
}

asio::awaitable<ExactProgramHandoff> ProgramHandoff::wait_async() const {
    if (!*this) throw std::logic_error("Program handoff lease is empty or consumed");
    return wait_async_with_impl(impl_);
}

asio::awaitable<ExactProgramHandoff> ProgramHandoff::wait_async_with_impl(
    std::shared_ptr<Impl> impl) {
    {
        std::lock_guard lock(impl->resolve_mutex);
        if (impl->handoff) co_return *impl->handoff;
    }
    auto reached = co_await impl->control->wait_handoff_async(impl->request_id);
    {
        std::lock_guard lock(impl->resolve_mutex);
        if (!impl->handoff) impl->handoff = std::move(reached.handoff);
        co_return *impl->handoff;
    }
}

ProgramHandoff::operator bool() const noexcept {
    return impl_ && !impl_->consumed.load();
}

void ProgramHandoff::consume() noexcept {
    if (impl_) impl_->consumed.store(true);
}

ProgramHandle::ProgramHandle(std::shared_ptr<detail::RunControl> control)
    : control_(std::move(control)) {}
ProgramHandle::~ProgramHandle() = default;
const std::string& ProgramHandle::run_id() const noexcept {
    return control_->run_id;
}
const std::string& ProgramHandle::logical_run_id() const noexcept {
    return control_->logical_run_id;
}
const std::string& ProgramHandle::program_version_id() const noexcept {
    return control_->program_version_id;
}
std::uint64_t ProgramHandle::attempt() const noexcept {
    return control_->attempt;
}
namespace {
// The awaitable forms of a handle's calls. Each takes the control by value, so that the awaitable keeps it alive
// whatever happens to the handle that made it.
asio::awaitable<bool> cancel_control(std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->cancel_async(detail::CancellationCause::User);
}
asio::awaitable<std::optional<ExactProgramHandoff>> latest_handoff_control(
    std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->latest_handoff_async();
}
asio::awaitable<ProgramRunRecord> snapshot_control(std::shared_ptr<detail::RunControl> control) {
    co_return co_await control->snapshot_async();
}
}  // namespace

bool ProgramHandle::cancel() noexcept {
    return control_->cancel(detail::CancellationCause::User);
}
asio::awaitable<bool> ProgramHandle::cancel_async() {
    // Not a coroutine itself: an awaitable starts when it is first awaited, and the handle may be gone by then. The
    // coroutine takes its own reference to the control as it is called.
    return cancel_control(control_);
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
std::optional<ExactProgramHandoff> ProgramHandle::latest_handoff() const {
    return control_->latest_handoff();
}
asio::awaitable<std::optional<ExactProgramHandoff>> ProgramHandle::latest_handoff_async() const {
    return latest_handoff_control(control_);
}
ProgramHandoff ProgramHandle::next_handoff() const {
    const auto request_id = control_->request_handoff();
    try {
        return ProgramHandoff(
            std::make_shared<ProgramHandoff::Impl>(control_, request_id));
    } catch (...) {
        control_->release_handoff(request_id);
        throw;
    }
}
asio::awaitable<ProgramHandoff> ProgramHandle::next_handoff_async() const {
    // Called at once, so that the awaitable holds the control (the lambda has no captures to outlive).
    return [](std::shared_ptr<detail::RunControl> control) -> asio::awaitable<ProgramHandoff> {
        const auto request_id = co_await control->request_handoff_async();
        try {
            co_return ProgramHandoff(std::make_shared<ProgramHandoff::Impl>(control, request_id));
        } catch (...) {
            control->release_handoff(request_id);
            throw;
        }
    }(control_);
}
ProgramRunRecord ProgramHandle::snapshot() const {
    return control_->snapshot();
}
asio::awaitable<ProgramRunRecord> ProgramHandle::snapshot_async() const {
    return snapshot_control(control_);
}
std::vector<ProgramNotice> ProgramHandle::notices() const {
    return control_->notices_snapshot();
}
ProgramHandle ProgramHandle::resume(ProgramRuntime& runtime, ProgramResume resume_value) const {
    return runtime.resume(control_->owner_scope, control_->run_id, std::move(resume_value));
}
ProgramHandle ProgramHandle::reconcile(ProgramRuntime&         runtime,
                                       ProgramEffectResolution resolution) const {
    return runtime.reconcile(control_->owner_scope, control_->run_id, std::move(resolution));
}

struct ProgramRuntime::Impl : std::enable_shared_from_this<ProgramRuntime::Impl> {
    explicit Impl(RuntimeConfig runtime_config) : config(std::move(runtime_config)) {
        auto executors = std::make_shared<detail::RuntimeScheduler>();
        if (config.executor) {
            // #557: the host's executor runs everything, and nothing runs inline. The runtime starts no thread.
            executors->injected = true;
            executors->host_pumped = config.executor_identity.host_pumped ||
                                     async::is_host_pumped(config.executor) || !NEOGRAPH_BUILD_THREADS;
            executors->host_executor          = config.executor;
            executors->running_in_this_thread = config.executor_identity.running_in_this_thread;
            if (executors->host_pumped && config.hook_runtime)
                throw ProgramBlockingCallRefused(
                    "RuntimeConfig::hook_runtime", {},
                    "a lifecycle hook blocks its caller until the hook answers, and the runtime's executor is run by a "
                    "host's event loop");
            const asio::any_io_executor never_inline =
                asio::prefer(config.executor, asio::execution::blocking.never);
            executors->attempts  = never_inline;
            executors->deadlines = never_inline;
            executors->waiters   = never_inline;
        } else {
            if (!NEOGRAPH_BUILD_THREADS) throw ProgramExecutorRequired();
            // Today's pools are one implementation of an executor: attempts on `scheduler_threads` threads,
            // deadline timers on one, waiters on the system executor.
            pool.emplace(config.scheduler_threads);
            deadline_pool.emplace(1);
            executors->attempts  = pool->get_executor();
            executors->deadlines = deadline_pool->get_executor();
            executors->waiters   = asio::system_executor{};
        }
        // Every operation of the runtime is a coroutine that awaits the stores (#557); the synchronous API runs the
        // same coroutine to completion on the calling thread. That wait is refused (RuntimeScheduler::run_blocking)
        // where it would hang the thread that has to complete the store call, unless every store completes at once.
        transitions = config.transitions;
        const auto* sync_transitions = dynamic_cast<const SyncProgramTransitionStore*>(config.transitions.get());
        const auto* sync_fragments   = dynamic_cast<const SyncTaskGraphFragmentStore*>(config.task_graph_fragments.get());
        executors->stores_inline =
            (!config.catalog || config.catalog->completes_inline()) &&
            (sync_transitions && sync_transitions->completes_at_once()) &&
            (!config.task_graph_fragments || (sync_fragments && sync_fragments->completes_at_once()));
        // The stack of a JavaScript control source: 0 is the default of JavaScriptCompileLimits.
        const JavaScriptCompileLimits javascript_defaults;
        executors->javascript_stack_bytes = config.javascript_stack_bytes == 0
                                                ? javascript_defaults.max_stack_bytes
                                                : config.javascript_stack_bytes;
        if (executors->javascript_stack_bytes > javascript_defaults.memory_limit_bytes)
            throw std::invalid_argument("RuntimeConfig::javascript_stack_bytes exceeds the JavaScript memory limit");
        scheduler = std::move(executors);
        // #543: a fork may send a call its source journaled and the journal cannot adopt only when the host says so.
        if (config.fork_may_resend_provider_calls && config.core_provider_call_resolver) {
            config.core_provider_call_resolver =
                [inner = std::move(config.core_provider_call_resolver)](
                    const ProgramCoreProviderCallContext& context) {
                    auto allowed = context;
                    if (allowed.replay_source) allowed.replay_source->may_resend = true;
                    return inner(allowed);
                };
        }
        global_child_quota = std::make_shared<GlobalChildQuotaState>(config.child_quota);
        process_state      = process_runtime_state(*transitions);
    }

    // A control for a run read back from the store: the run's events are loaded first, which only a coroutine can await.
    asio::awaitable<std::shared_ptr<detail::RunControl>> make_record_control(ProgramRunRecord record) {
        auto events  = co_await transitions->load_events(record.owner_scope(), record.run_id(), 0);
        auto control = std::make_shared<detail::RunControl>(std::move(record), std::move(events), transitions,
                                                            config.native_history_archive, scheduler);
        // Not registered: this control runs nothing. A cancel() of its handle still takes the claim that keeps a second
        // cancel() and a resume from competing with the scheduled cancellation (see register_control).
        control->set_pending_cancellations(process_state->pending_cancellations);
        co_return control;
    }

    // The synchronous form of an operation: its coroutine body, run to completion on the calling thread.
    template <typename Result>
    Result run_blocking(asio::awaitable<Result> body, std::string_view operation, std::string_view alternative) {
        return scheduler->run_blocking(std::move(body), operation, alternative, scheduler->stores_inline);
    }
    // The same for a body that takes something it must not lose when the call is refused: the body is built only
    // after the refusal has been ruled out.
    template <typename Make>
    auto run_blocking_lazy(Make make, std::string_view operation, std::string_view alternative) {
        scheduler->require_synchronous_call(operation, alternative);
        return run_blocking(make(), operation, alternative);
    }

    // Runs a synchronous body on the runtime's executor and completes on the executor of the awaiting coroutine.
    // The state outlives the body, whatever happens to the ProgramRuntime object in the meantime.
    // (co_spawn builds a default Result when the body throws, and most results have no default: the body hands back
    // an optional.)
    template <typename Result, typename Body>
    asio::awaitable<Result> run_on_executor(Body body) {
        auto produced = co_await asio::co_spawn(
            scheduler->attempts,
            [keep = shared_from_this(), body = std::move(body)]() mutable -> asio::awaitable<std::optional<Result>> {
                co_return std::optional<Result>(body());
            },
            asio::use_awaitable);
        co_return std::move(*produced);
    }
    // The same for a body that is a coroutine.
    template <typename Result, typename Body>
    asio::awaitable<Result> run_coroutine_on_executor(Body body) {
        auto produced = co_await asio::co_spawn(
            scheduler->attempts,
            [keep = shared_from_this(), body = std::move(body)]() mutable -> asio::awaitable<std::optional<Result>> {
                co_return std::optional<Result>(co_await body());
            },
            asio::use_awaitable);
        co_return std::move(*produced);
    }

    // config.transitions, awaited by every coroutine of the runtime.
    std::shared_ptr<ProgramTransitionStore>                transitions;

    // The runtime's pools; empty when the host gave an executor.
    std::optional<asio::thread_pool>                       pool;
    std::optional<asio::thread_pool>                       deadline_pool;
    std::shared_ptr<const detail::RuntimeScheduler>        scheduler;

    asio::awaitable<void> verify_synthesis_grant(ProgramChildSynthesisRecord record,
                                                 bool require_reservation = true) const {
        const auto& d = record.data();
        if (!config.child_synthesis_gateway || !config.child_synthesis_grant_resolver)
            throw_runtime_diagnostic("P_CHILD_SYNTHESIS_RECOVERY_REQUIRED",
                                     "Child synthesis host policy is unavailable");
        const auto grant = config.child_synthesis_grant_resolver(
            d.proposal.data().owner_scope, d.parent.run.run_id(), d.grant.id());
        if (!grant || grant->id() != d.grant.id())
            throw_runtime_diagnostic("P_CHILD_SYNTHESIS_AUTHORITY",
                                     "Stored child grant is not authorized by current host policy");
        if (require_reservation) {
            const auto source = (co_await transitions->load_lineage_head(
                d.proposal.data().owner_scope, d.proposal.data().lineage_id,
                d.reservation.data().source_lineage_head_id));
            const auto reserved = (co_await transitions->load_lineage_head(
                d.proposal.data().owner_scope, d.proposal.data().lineage_id,
                d.reservation.data().reserved_lineage_head_id));
            if (!source || source->id() != d.parent.lineage.id() || !reserved ||
                reserved->active_generation_id() != d.parent.generation.id() ||
                reserved->remaining_budget() != d.reservation.data().remaining_after_reservation)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_RESERVATION",
                                         "Stored synthesis has no matching durable compile debit");
        }
        auto lineage = (co_await transitions->load_run_lineage(d.proposal.data().owner_scope,
                                                            d.parent.run.run_id()));
        if (!lineage || lineage->active_generation_id() != d.parent.generation.id())
            throw_runtime_diagnostic("P_CHILD_SYNTHESIS_GENERATION",
                                     "Child synthesis generation is no longer active");
    }

    asio::awaitable<bool> synthesis_child_is_uncertain(ProgramChildSynthesisRecord record) const {
        using State   = ProgramChildSynthesisState;
        const auto& d = record.data();
        if (d.state != State::Dispatching && d.state != State::Spawned) co_return false;
        const auto child_id = d.artifacts.at("child_run_id").get<std::string>();
        const auto child    = (co_await transitions->load(d.proposal.data().owner_scope, child_id));
        if (!child || child->continuation().state != ContinuationState::Running ||
            child->exact_checkpoint())
            co_return false;
        const auto parent =
            (co_await transitions->load(d.proposal.data().owner_scope, d.parent.run.run_id()));
        const auto relation =
            parent ? find_child(*parent, child_id) : std::optional<ProgramChildRecord>{};
        if (!relation || relation->state != ProgramChildState::Dispatched) co_return false;
        const auto bundle =
            ProgramBundle::parse(detail::canonical_json_bytes(d.artifacts.at("bundle")));
        co_return !bundle.control_source() &&
               !checkpointless_replay_safe(bundle.typed_orchestration_plan());
    }

    asio::awaitable<void> publish_synthesis(ProgramChildSynthesisRecord         next,
                                            std::shared_ptr<detail::RunControl> control = {}) {
        co_await verify_synthesis_grant(next);
        const auto& d = next.data();
        for (int retry = 0; retry < 5; ++retry) {
            auto previous =
                (co_await transitions->load(d.proposal.data().owner_scope, d.parent.run.run_id()));
            auto journal =
                (co_await transitions->latest(d.proposal.data().owner_scope, d.parent.run.run_id()));
            if (!previous || !journal || previous->journal_head() != journal->id) continue;
            if (control && control->cancellation_cause() != detail::CancellationCause::None)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CANCELLED",
                                         "Parent cancelled during synthesis");
            const auto timestamp = std::max(now_ms(), previous->updated_at_ms());
            auto       remaining = previous->remaining_budget();
            const auto elapsed = static_cast<std::uint64_t>(timestamp - previous->updated_at_ms());
            // During ng.spawn/await the command journal owns an in-flight reservation.
            // Preserve that ledger exactly; its running attempt accounts elapsed time.
            if (d.state < ProgramChildSynthesisState::Dispatching) {
                remaining.wall_time_ms -= std::min(remaining.wall_time_ms, elapsed);
                if (!remaining.wall_time_ms)
                    throw_runtime_diagnostic("P_CHILD_SYNTHESIS_BUDGET",
                                             "Parent deadline elapsed during synthesis");
            }
            if (control && std::chrono::steady_clock::now() >= control->deadline)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_BUDGET",
                                         "Parent synthesis deadline elapsed");
            auto publication = (co_await attach_run_lineage_async(
                *transitions,
                continuing_running_publication(*previous, *journal, remaining, timestamp,
                                               journal->inflight_reservation)));
            publication.child_synthesis_records.push_back(next);
            const auto lease = (co_await transitions->load_execution_lease(
                d.proposal.data().owner_scope, d.parent.run.run_id()));
            ProgramTransitionPublishResult result;
            if (lease)
                result = (co_await transitions->compare_publish_execution(
                                      d.proposal.data().owner_scope, previous->journal_head(),
                                      std::move(publication), lease, lease));
            else
                result = (co_await transitions->compare_publish(d.proposal.data().owner_scope,
                                                                      previous->journal_head(),
                                                                      std::move(publication)));
            if (result == ProgramTransitionPublishResult::Published) co_return;
            if (result == ProgramTransitionPublishResult::AlreadyPresent) {
                if (d.state == ProgramChildSynthesisState::Compiling ||
                    d.state == ProgramChildSynthesisState::Validating)
                    throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CONFLICT",
                                             "A durable synthesis claim already has an owner");
                co_return;
            }
        }
        throw_runtime_diagnostic(
            "P_CHILD_SYNTHESIS_CONFLICT",
            "Child synthesis stage lost its parent or record compare-and-swap");
    }

    asio::awaitable<std::optional<ProgramChildSynthesisRecord>> synthesis_binding(
        std::shared_ptr<detail::RunControl> parent, std::string name) const {
        if (!parent->invocation.budget.max_dynamic_compiles || !parent->materialized ||
            !parent->materialized->bundle.control_source())
            co_return std::nullopt;
        for (const auto& record :
             (co_await transitions->load_child_syntheses(parent->owner_scope, parent->run_id))) {
            if (record.data().binding_name != name) continue;
            (co_await verify_synthesis_grant(record));
            using State = ProgramChildSynthesisState;
            if (record.data().state != State::Bound && record.data().state != State::Dispatching &&
                record.data().state != State::Spawned)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_RECOVERY_REQUIRED",
                                         "Child synthesis binding is not ready");
            co_return record;
        }
        co_return std::nullopt;
    }

    asio::awaitable<std::optional<ProgramChildRecord>> inherited_child(
        std::shared_ptr<detail::RunControl> parent, std::string name) const {
        if (parent->inherited_children.empty()) co_return std::nullopt;
        const auto run = (co_await parent->snapshot_async());
        // Every publication on this run (child completion, dispatch, ...) moves the
        // lineage head, so the head read here may legitimately differ from the record
        // read above. What must hold is generation identity: the lineage still selects
        // the generation this control runs as.
        const auto lineage =
            (co_await transitions->load_run_lineage(parent->owner_scope, parent->run_id));
        const auto active =
            lineage ? (co_await transitions->load_generation(parent->owner_scope,
                                                           lineage->lineage_id(),
                                                           lineage->active_generation()))
                    : std::nullopt;
        if (!active || active->id() != lineage->active_generation_id() ||
            active->lineage_id() != lineage->lineage_id() ||
            active->generation() != lineage->active_generation() ||
            active->run_id() != parent->run_id ||
            active->program_version_id() != run.program_version_id() ||
            active->bundle_id() != run.bundle_id() ||
            lineage->root_run_id() != parent->logical_run_id ||
            run.logical_run_id() != parent->logical_run_id)
            throw_runtime_diagnostic("P_CHILD_GENERATION",
                                     "Inherited child requires the active parent generation");
        for (const auto& inherited : parent->inherited_children) {
            const auto link = ModuleLinkReceipt::parse(inherited.link_receipt);
            if (link.child_name() == name) co_return find_child(run, inherited.child_run_id);
        }
        co_return std::nullopt;
    }

    asio::awaitable<std::optional<ProgramRuntimeChildBinding>> resolve_child_binding(
        std::shared_ptr<detail::RunControl> parent, std::string name) const {
        if (const auto child = (co_await inherited_child(parent, name))) {
            auto link    = ModuleLinkReceipt::parse(child->link_receipt);
            auto version = (co_await config.catalog->resolve_version_async(parent->owner_scope,
                                                           link.child_program_version_id()));
            if (!version)
                throw_runtime_diagnostic("P_CHILD_BINDING",
                                         "Inherited child version is unavailable");
            co_return ProgramRuntimeChildBinding{std::move(link), std::move(*version)};
        }
        if (auto record = (co_await synthesis_binding(parent, name))) {
            const auto& a    = record->data().artifacts;
            auto        link = ModuleLinkReceipt::parse(detail::canonical_json_bytes(a.at("link")));
            auto        version = (co_await config.catalog->resolve_version_async(parent->owner_scope,
                                                                  link.child_program_version_id()));
            if (!version)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_RECOVERY_REQUIRED",
                                         "Synthesized child version is unavailable");
            co_return ProgramRuntimeChildBinding{std::move(link), std::move(*version)};
        }
        co_return config.child_binding_resolver
                   ? config.child_binding_resolver(parent->owner_scope, parent->program_version_id,
                                                   name)
                   : std::nullopt;
    }

    asio::awaitable<std::optional<ProgramChildRecord>> recoverable_existing_child(
        std::shared_ptr<detail::RunControl> parent, std::string binding, json input,
        std::string operation) const {
        const auto run = (co_await parent->snapshot_async());
        if (run.owner_scope() != parent->owner_scope ||
            run.logical_run_id() != parent->logical_run_id ||
            run.continuation().state != ContinuationState::Running ||
            parent->cancellation_cause() != detail::CancellationCause::None)
            co_return std::nullopt;
        const auto resolved = (co_await resolve_child_binding(parent, binding));
        if (!resolved) co_return std::nullopt;
        const auto& link = resolved->receipt;
        if (link.owner_scope() != parent->owner_scope || link.child_name() != binding ||
            link.child_program_version_id() != resolved->version.id())
            co_return std::nullopt;

        auto child = (co_await inherited_child(parent, binding));
        if (child) {
            const auto inherited = std::find_if(
                parent->inherited_children.begin(), parent->inherited_children.end(),
                [&](const auto& admitted) { return same_child_metadata(admitted, *child); });
            if (inherited == parent->inherited_children.end()) co_return std::nullopt;
        } else {
            const auto expected_id = detail::sha256_identity(
                "program-dsl-child/v1",
                detail::canonical_json_bytes(
                    json{{"owner_scope", parent->owner_scope},
                         {"parent_run_id", parent->run_id},
                         {"parent_program_version_id", parent->program_version_id},
                         {"link_id", link.id()},
                         {"operation_id", std::string(operation)},
                         {"execution_key", std::string(operation)}}));
            child = find_child(run, expected_id);
        }
        if (!child || child->link_id != link.id() ||
            child->link_receipt != link.serialize_canonical() ||
            child->invocation.parent_run_id != parent->logical_run_id ||
            child->invocation.child_depth != parent->persisted_invocation.child_depth + 1 ||
            detail::canonical_json_bytes(child->invocation.input) !=
                detail::canonical_json_bytes(input))
            co_return std::nullopt;
        const auto persisted = (co_await transitions->load(parent->owner_scope, child->child_run_id));
        if (!persisted || persisted->owner_scope() != parent->owner_scope ||
            persisted->run_id() != child->child_run_id ||
            persisted->program_version_id() != resolved->version.id() ||
            persisted->bundle_id() != resolved->version.bundle_id() ||
            persisted->child_depth() != child->invocation.child_depth)
            co_return std::nullopt;
        ProgramInvocation invocation{
            child->invocation.input, child->invocation.granted_budget,
            child->invocation.trace_id, {}, child->child_run_id,
            child->invocation.parent_run_id, child->invocation.child_depth};
        if (persisted->invocation() != bind_runtime_invocation(
                invocation, parent->owner_scope, resolved->version.id(), child->child_run_id))
            co_return std::nullopt;
        co_return child;
    }

    void configure_child_launcher(const std::shared_ptr<detail::RunControl>& control) {
        // The callbacks below run on an attempt, which can outlive the ProgramRuntime object on an injected
        // executor: each one keeps this state alive while it runs, and finds it gone only after shutdown.
        const std::weak_ptr<Impl> weak_impl = weak_from_this();
        if (!config.child_binding_resolver && !config.child_synthesis_gateway &&
            !config.recover_existing_child_commands) {
            if (config.task_graph_fragments && config.task_graph_policy_resolver)
                control->set_task_graph_expansion_context(config.task_graph_fragments,
                                                          config.task_graph_policy_resolver);
            return;
        }
        const auto weak_control = std::weak_ptr<detail::RunControl>(control);
        if (config.recover_existing_child_commands) {
            control->set_existing_child_reconnect_callback(
                [this, weak_impl, weak_control](std::string binding, json input, std::string operation)
                    -> asio::awaitable<std::shared_ptr<detail::RunControl>> {
                    const auto keep = weak_impl.lock();
                    if (!keep) throw std::runtime_error("ProgramRuntime has been destroyed");
                    const auto parent = weak_control.lock();
                    const auto child = parent
                                           ? (co_await recoverable_existing_child(parent, binding, input,
                                                                         operation))
                                           : std::nullopt;
                    if (!child)
                        throw_runtime_diagnostic("P_CHILD_RECOVERY",
                                                 "Existing child recovery binding changed");
                    // Reconnect cannot create a missing run. Do not fall through
                    // to start_child if publication disappears after validation.
                    // (A handler cannot suspend: the failure is noted, and classified after the try.)
                    std::optional<ProgramHandle> reconnected;
                    std::exception_ptr           reconnect_failure;
                    try {
                        reconnected.emplace(co_await ProgramRuntime(keep, ProgramRuntime::Borrowed{})
                                                .reconnect_core(parent->owner_scope, child->child_run_id));
                    } catch (...) {
                        reconnect_failure = std::current_exception();
                    }
                    if (reconnect_failure) {
                        try {
                            std::rethrow_exception(reconnect_failure);
                        } catch (const ProgramDiagnosticError& error) {
                            if (error.diagnostic().code == "P_RUN_NOT_FOUND")
                                throw_runtime_diagnostic("P_CHILD_RECOVERY",
                                                         "Existing child run disappeared");
                            throw;
                        }
                    }
                    bind_budgeted_child_completion(reconnected->control_, parent->owner_scope,
                                                     parent->logical_run_id, child->child_run_id);
                    co_await parent->attach_child(reconnected->control_);
                    if (const auto result = reconnected->try_result())
                        (void)co_await publish_child_completion(transitions, parent->owner_scope,
                                                                parent->logical_run_id, child->child_run_id,
                                                                *result);
                    co_return reconnected->control_;
                });
        }
        if (config.child_synthesis_gateway || config.recover_existing_child_commands) {
            control->set_child_recovery_callback([this, weak_impl, weak_control](std::string binding,
                                                                                 json        input,
                                                                                 std::string operation)
                                                     -> asio::awaitable<detail::ChildRecoveryMode> {
                const auto keep   = weak_impl.lock();
                const auto parent = weak_control.lock();
                using Mode = detail::ChildRecoveryMode;
                if (!keep || !parent) co_return Mode::None;
                if (config.recover_existing_child_commands &&
                    (co_await recoverable_existing_child(parent, binding, input, operation)))
                    co_return Mode::Existing;
                if (!config.child_synthesis_gateway) co_return Mode::None;
                if (const auto child = (co_await inherited_child(parent, binding)))
                    co_return !config.recover_existing_child_commands &&
                                   detail::canonical_json_bytes(child->invocation.input) ==
                                       detail::canonical_json_bytes(input)
                               ? Mode::Synthesis : Mode::None;
                const auto synthesis = (co_await synthesis_binding(parent, binding));
                if (!synthesis) co_return Mode::None;
                if (synthesis->data().state == ProgramChildSynthesisState::Bound)
                    co_return Mode::Synthesis;
                const auto& artifacts = synthesis->data().artifacts;
                const auto  link =
                    ModuleLinkReceipt::parse(detail::canonical_json_bytes(artifacts.at("link")));
                const auto expected_id = detail::sha256_identity(
                    "program-dsl-child/v1",
                    detail::canonical_json_bytes(
                        json{{"owner_scope", parent->owner_scope},
                             {"parent_run_id", parent->run_id},
                             {"parent_program_version_id", parent->program_version_id},
                             {"link_id", link.id()},
                             {"operation_id", std::string(operation)},
                             {"execution_key", std::string(operation)}}));
                co_return artifacts.at("child_run_id") == expected_id &&
                               detail::canonical_json_bytes(artifacts.at("child_input")) ==
                                   detail::canonical_json_bytes(input)
                           ? Mode::Synthesis : Mode::None;
            });
        }

        control->set_child_binding_validation_callback(
            [this, weak_impl, weak_control](std::string binding_name, std::string operation_id)
                -> asio::awaitable<void> {
                const auto keep = weak_impl.lock();
                if (!keep) throw std::runtime_error("ProgramRuntime has been destroyed");
                const auto parent = weak_control.lock();
                if (!parent) throw std::runtime_error("Parent Program control expired");
                const auto resolved = (co_await resolve_child_binding(parent, binding_name));
                if (!resolved)
                    throw_runtime_diagnostic("P_CHILD_BINDING",
                                             "Verified child binding was not resolved",
                                             json{{"binding", std::string(binding_name)},
                                                  {"operation_id", std::string(operation_id)}});
                if (resolved->receipt.child_name() != binding_name)
                    throw_runtime_diagnostic(
                        "P_CHILD_BINDING",
                        "Verified child binding does not match its immutable link receipt",
                        json{{"binding", std::string(binding_name)},
                             {"receipt_child_name", resolved->receipt.child_name()},
                             {"operation_id", std::string(operation_id)}});
            });
        control->set_child_launch_callback([this, weak_impl, weak_control](
                                               std::string binding_name, json input,
                                               std::string                    operation_id,
                                               std::string                    execution_key,
                                               std::optional<TaskGraphBudget> budget_override,
                                               bool exact_budget) -> asio::awaitable<std::shared_ptr<detail::RunControl>> {
            const auto keep = weak_impl.lock();
            if (!keep) throw std::runtime_error("ProgramRuntime has been destroyed");
            const auto parent = weak_control.lock();
            if (!parent) throw std::runtime_error("Parent Program control expired");
            if (parent->cancellation_cause() != detail::CancellationCause::None)
                throw_runtime_diagnostic("P_RUNTIME_CANCELLED",
                                         "Parent Program is already cancelling",
                                         json{{"parent_run_id", parent->run_id},
                                              {"operation_id", std::string(operation_id)}});

            const auto resolved = (co_await resolve_child_binding(parent, binding_name));
            if (!resolved)
                throw_runtime_diagnostic("P_CHILD_BINDING",
                                         "Verified child binding was not resolved",
                                         json{{"binding", std::string(binding_name)},
                                              {"operation_id", std::string(operation_id)}});
            if (resolved->receipt.child_name() != binding_name)
                throw_runtime_diagnostic(
                    "P_CHILD_BINDING",
                    "Verified child binding does not match its immutable link receipt",
                    json{{"binding", std::string(binding_name)},
                         {"receipt_child_name", resolved->receipt.child_name()},
                         {"operation_id", std::string(operation_id)}});
            if (const auto child = (co_await inherited_child(parent, binding_name))) {
                if (detail::canonical_json_bytes(child->invocation.input) !=
                    detail::canonical_json_bytes(input))
                    throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                             "Inherited child invocation cannot be changed");
                ProgramInvocation invocation{
                    child->invocation.input,      child->invocation.granted_budget,
                    child->invocation.trace_id,   {},
                    child->child_run_id,          child->invocation.parent_run_id,
                    child->invocation.child_depth};
                co_return (co_await ProgramRuntime(keep, ProgramRuntime::Borrowed{})
                               .start_child_core(parent->owner_scope, ProgramHandle(parent), resolved->receipt,
                                                 resolved->version, std::move(invocation)))
                    .control_;
            }
            const auto& limits = resolved->receipt.budget();
            RunBudget   budget{limits.wall_time_ms,           limits.model_tokens,
                             limits.monetary_microunits,    limits.max_concurrency,
                             limits.max_program_operations, limits.max_core_steps,
                             limits.max_dynamic_compiles,   limits.max_child_depth,
                             limits.max_total_children};
            if (budget_override) {
                // Schema-1 fragments read 0 as "the link's whole ceiling". Schema-2 fragments
                // are exact: the child gets what the aggregate counted, and 0 grants nothing.
                const auto apply_requested = [exact_budget](std::uint64_t requested,
                                                            std::uint64_t ceiling) {
                    if (exact_budget) return std::min(requested, ceiling);
                    return requested == 0 ? ceiling : std::min(requested, ceiling);
                };
                budget.wall_time_ms =
                    apply_requested(budget_override->wall_time_ms, budget.wall_time_ms);
                budget.model_tokens =
                    apply_requested(budget_override->model_tokens, budget.model_tokens);
                budget.monetary_microunits = apply_requested(budget_override->monetary_microunits,
                                                             budget.monetary_microunits);
            }
            if (parent->granted_budget.max_child_depth > 0) {
                budget.max_child_depth =
                    std::min(budget.max_child_depth, parent->granted_budget.max_child_depth - 1);
            } else {
                budget.max_child_depth = 0;
            }
            if (parent->granted_budget.max_total_children > 0) {
                budget.max_total_children = std::min(budget.max_total_children,
                                                     parent->granted_budget.max_total_children - 1);
            } else {
                budget.max_total_children = 0;
            }
            // #364: a spawned child runs under its parent's retry ceiling.
            budget.max_node_retries = parent->granted_budget.max_node_retries;
            const auto child_run_id = detail::sha256_identity(
                "program-dsl-child/v1",
                detail::canonical_json_bytes(
                    json{{"owner_scope", parent->owner_scope},
                         {"parent_run_id", parent->run_id},
                         {"parent_program_version_id", parent->program_version_id},
                         {"link_id", resolved->receipt.id()},
                         {"operation_id", std::string(operation_id)},
                         {"execution_key", std::string(execution_key)}}));
            auto synthesis = (co_await synthesis_binding(parent, binding_name));
            if (synthesis) {
                using State = ProgramChildSynthesisState;
                auto data   = synthesis->data();
                if (data.state == State::Bound) {
                    data.previous_id = synthesis->id();
                    ++data.revision;
                    data.state                     = State::Dispatching;
                    data.artifacts["child_run_id"] = child_run_id;
                    data.artifacts["child_input"]  = input;
                    synthesis = ProgramChildSynthesisRecord::create(std::move(data));
                    (co_await publish_synthesis(*synthesis, parent));
                } else if (data.artifacts.at("child_run_id") != child_run_id ||
                           detail::canonical_json_bytes(data.artifacts.at("child_input")) !=
                               detail::canonical_json_bytes(input)) {
                    throw_runtime_diagnostic(
                        "P_CHILD_SYNTHESIS_CONFLICT",
                        "One synthesis grant cannot spawn another child invocation");
                }
            }
            ProgramInvocation invocation{std::move(input),
                                         budget,
                                         parent->trace_id + ":" + std::string(operation_id) + ":" +
                                             std::string(execution_key),
                                         {},
                                         child_run_id,
                                         parent->logical_run_id,
                                         parent->persisted_invocation.child_depth + 1};
            ProgramHandle child =
                co_await ProgramRuntime(keep, ProgramRuntime::Borrowed{})
                    .start_child_core(parent->owner_scope, ProgramHandle(parent), resolved->receipt,
                                      resolved->version, std::move(invocation));
            if (synthesis && synthesis->data().state == ProgramChildSynthesisState::Dispatching) {
                auto data        = synthesis->data();
                data.previous_id = synthesis->id();
                ++data.revision;
                data.state = ProgramChildSynthesisState::Spawned;
                (co_await publish_synthesis(ProgramChildSynthesisRecord::create(std::move(data)), parent));
            }
            co_return child.control_;
        });
        if (config.task_graph_fragments && config.task_graph_policy_resolver)
            control->set_task_graph_expansion_context(config.task_graph_fragments,
                                                      config.task_graph_policy_resolver);
    }

    struct ControlRegistration {
        std::shared_ptr<detail::RunControl> control;
        bool                                inserted = false;
    };

    asio::awaitable<ControlRegistration> register_control(std::shared_ptr<detail::RunControl> control,
                                                          bool process_unique = false) {
        control->native_history_archive = config.native_history_archive;
        if (!control->recorded_replay) {
            control->set_hook_runtime(config.hook_runtime);
            control->set_core_tool_grant_resolver(config.core_tool_grant_resolver);
            control->set_core_tool_grant_awaitable_resolver(config.core_tool_grant_awaitable_resolver);
            control->set_core_provider_call_resolver(
                config.core_provider_call_resolver, config.require_core_provider_call_broker);
        }
        if (control->logical_run_id != control->run_id) {
            const auto run = (co_await control->snapshot_async());
            if (!run.children().empty()) {
                const auto lineage =
                    (co_await transitions->load_run_lineage(control->owner_scope, control->run_id));
                const auto initial = lineage
                                         ? (co_await transitions->load_generation_initial_publication(
                                               control->owner_scope, lineage->lineage_id(),
                                               lineage->active_generation()))
                                         : std::nullopt;
                if (!initial || initial->run_record.run_id() != control->run_id)
                    throw_runtime_diagnostic(
                        "P_CHILD_GENERATION",
                        "Inherited child bindings need their generation publication");
                control->inherited_children = initial->run_record.children();
            }
        }
        if (!control->recorded_replay) configure_child_launcher(control);
        if (config.checkpoint_handler && !control->recorded_replay) {
            control->checkpoint_handler = [weak = std::weak_ptr<detail::RunControl>(control),
                                           handler =
                                               config.checkpoint_handler](std::uint64_t ordinal)
                                              -> asio::awaitable<void> {
                auto live = weak.lock();
                if (!live || live->has_active_handoff_request()) co_return;
                const auto commands =
                    (co_await live->transitions->load_javascript_commands(live->owner_scope, live->run_id));
                if (commands.empty() || commands.back().command_ordinal() != ordinal ||
                    !(co_await live->latest_handoff_async()))
                    co_return;
                ProgramHandle                 handle(live);
                std::optional<ProgramHandoff> lease;
                try {
                    lease.emplace(co_await handle.next_handoff_async());
                } catch (const std::logic_error&) {
                    // A concurrent explicit host request has priority.
                    if (live->has_active_handoff_request()) co_return;
                    throw;
                }
                co_await live->reach_latest_handoff_if_requested();
                handler(std::move(handle), std::move(*lease));
            };
        }
        control->set_pending_cancellations(process_state->pending_cancellations);
        const auto weak_process_state = std::weak_ptr<ProcessRuntimeState>(process_state);
        control->set_handoff_coordination_mutex(
            process_state->transition_mutex,
            [weak_process_state](const detail::RunControl* requester) {
                const auto state = weak_process_state.lock();
                if (!state) return false;
                std::lock_guard process_lock(state->controls_mutex);
                std::erase_if(state->controls, [](const auto& entry) {
                    const auto live = entry.control.lock();
                    return !live || live->try_result().has_value();
                });
                for (const auto& entry : state->controls) {
                    if (entry.owner_scope != requester->owner_scope ||
                        entry.run_id != requester->run_id) {
                        continue;
                    }
                    if (const auto live = entry.control.lock();
                        live && live.get() != requester) {
                        return false;
                    }
                }
                return true;
            });
        std::lock_guard lock(mutex);
        if (stopping) throw std::runtime_error("ProgramRuntime is stopping");
        {
            std::lock_guard process_lock(process_state->controls_mutex);
            std::erase_if(process_state->controls, [](const auto& entry) {
                const auto live = entry.control.lock();
                return !live || live->try_result().has_value();
            });
            if (process_unique) {
                for (auto& entry : process_state->controls) {
                    if (entry.owner_scope == control->owner_scope &&
                        entry.run_id == control->run_id) {
                        if (auto existing = entry.control.lock()) {
                            co_return ControlRegistration{std::move(existing), false};
                        }
                    }
                }
            }
            process_state->controls.push_back(
                {control->owner_scope, control->run_id, control});
        }
        std::erase_if(controls, [](const std::weak_ptr<detail::RunControl>& weak) {
            const auto live = weak.lock();
            return !live || live->try_result().has_value();
        });
        controls.push_back(control);
        co_return ControlRegistration{control, true};
    }

    std::shared_ptr<ChildQuotaReservation> reserve_global_child(std::string_view owner_scope) {
        if (!global_child_quota) return {};
        auto reservation         = std::make_shared<ChildQuotaReservation>();
        reservation->state       = global_child_quota;
        reservation->owner_scope = std::string(owner_scope);
        auto&           state    = *global_child_quota;
        std::lock_guard lock(state.mutex);
        const auto      pending_for_owner =
            state.pending_by_owner.find(reservation->owner_scope) == state.pending_by_owner.end()
                     ? 0
                     : state.pending_by_owner.at(reservation->owner_scope);
        if (state.limits.max_pending_spawn_requests != 0 &&
            state.pending_spawns >= state.limits.max_pending_spawn_requests)
            throw_runtime_diagnostic("P_CHILD_QUOTA",
                                     "Global pending child-spawn quota is exhausted",
                                     json{{"owner_scope", reservation->owner_scope},
                                          {"kind", "pending"},
                                          {"limit", state.limits.max_pending_spawn_requests}});
        if (state.limits.max_pending_spawn_requests_per_owner != 0 &&
            pending_for_owner >= state.limits.max_pending_spawn_requests_per_owner)
            throw_runtime_diagnostic(
                "P_CHILD_QUOTA", "Per-owner pending child-spawn quota is exhausted",
                json{{"owner_scope", reservation->owner_scope},
                     {"kind", "pending-owner"},
                     {"limit", state.limits.max_pending_spawn_requests_per_owner}});
        ++state.pending_spawns;
        ++state.pending_by_owner[reservation->owner_scope];
        reservation->pending = true;
        return reservation;
    }

    std::shared_ptr<ChildQuotaReservation> reserve_active_child(std::string_view owner_scope) {
        auto reservation = reserve_global_child(owner_scope);
        if (reservation && !reservation->activate()) {
            reservation->release();
            throw_runtime_diagnostic("P_CHILD_QUOTA", "Global active child quota is exhausted",
                                     json{{"owner_scope", std::string(owner_scope)},
                                          {"limit", config.child_quota.max_active_children}});
        }
        return reservation;
    }

    void commit_active_child(const std::shared_ptr<detail::RunControl>& control,
                             std::shared_ptr<ChildQuotaReservation>&    reservation) {
        if (!reservation) return;
        const auto held = reservation;
        control->add_logical_cleanup([held] { held->release(); });
        reservation.reset();
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

    std::shared_ptr<detail::RunControl> find_process_control(std::string_view owner_scope,
                                                              std::string_view run_id) {
        std::lock_guard lock(process_state->controls_mutex);
        std::erase_if(process_state->controls, [](const auto& entry) {
            const auto live = entry.control.lock();
            return !live || live->try_result().has_value();
        });
        for (auto& entry : process_state->controls) {
            if (entry.owner_scope == owner_scope && entry.run_id == run_id) {
                if (auto control = entry.control.lock()) return control;
            }
        }
        return {};
    }

    // `held` is the coordination lock when the caller holds it (a replacement or a migration fencing its source).
    asio::awaitable<void> cancel_process_controls(std::string owner_scope, std::string run_id,
                                                  std::shared_ptr<detail::ReentrantOperationLock> held = {}) {
        std::vector<std::shared_ptr<detail::RunControl>> matches;
        {
            std::lock_guard lock(process_state->controls_mutex);
            std::erase_if(process_state->controls, [](const auto& entry) {
                const auto live = entry.control.lock();
                return !live || live->try_result().has_value();
            });
            for (auto& entry : process_state->controls) {
                if (entry.owner_scope == owner_scope && entry.run_id == run_id) {
                    if (auto control = entry.control.lock()) matches.push_back(std::move(control));
                }
            }
        }
        for (auto& control : matches)
            (void)co_await control->cancel_async(detail::CancellationCause::ParentTerminal, held);
    }

    struct ChildReservationState {
        RunBudget                                      used;
        std::unordered_map<std::string, RunBudget>     permanent_by_child;
        std::unordered_map<std::string, std::uint32_t> active_concurrency;
    };
    struct ParentChildBudget {
        RunBudget parent;
        RunBudget child;
    };

    static bool child_execution_active(const ProgramChildRecord& child) noexcept {
        return child.state == ProgramChildState::Publishing ||
               (child.state == ProgramChildState::Dispatched && !child.terminal_result);
    }

    static RunBudget permanent_child_reservation(const RunBudget& budget) {
        auto reservation            = child_reservation(budget);
        reservation.max_concurrency = 0;
        return reservation;
    }

    static void add_budget(RunBudget& target, const RunBudget& value) noexcept {
        target.wall_time_ms += value.wall_time_ms;
        target.model_tokens += value.model_tokens;
        target.monetary_microunits += value.monetary_microunits;
        target.max_concurrency += value.max_concurrency;
        target.max_program_operations += value.max_program_operations;
        target.max_core_steps += value.max_core_steps;
        target.max_dynamic_compiles += value.max_dynamic_compiles;
        target.max_total_children += value.max_total_children;
    }

    static void subtract_budget(RunBudget& target, const RunBudget& value) noexcept {
        const auto subtract = [](auto& current, const auto amount) {
            current = current >= amount ? current - amount : 0;
        };
        subtract(target.wall_time_ms, value.wall_time_ms);
        subtract(target.model_tokens, value.model_tokens);
        subtract(target.monetary_microunits, value.monetary_microunits);
        subtract(target.max_concurrency, value.max_concurrency);
        subtract(target.max_program_operations, value.max_program_operations);
        subtract(target.max_core_steps, value.max_core_steps);
        subtract(target.max_dynamic_compiles, value.max_dynamic_compiles);
        subtract(target.max_total_children, value.max_total_children);
    }

    static bool activate_concurrency_locked(ChildReservationState& state,
                                            std::string_view       child_run_id,
                                            const RunBudget&       parent_budget,
                                            std::uint32_t          amount) noexcept {
        if (amount == 0) return true;
        const auto existing = state.active_concurrency.find(std::string(child_run_id));
        if (existing != state.active_concurrency.end()) return existing->second == amount;
        if (state.used.max_concurrency > parent_budget.max_concurrency ||
            amount > parent_budget.max_concurrency - state.used.max_concurrency)
            return false;
        state.used.max_concurrency += amount;
        state.active_concurrency.emplace(std::string(child_run_id), amount);
        return true;
    }

    static void release_concurrency_locked(ChildReservationState& state,
                                           std::string_view       child_run_id,
                                           std::uint32_t          amount) noexcept {
        const auto found = state.active_concurrency.find(std::string(child_run_id));
        if (found == state.active_concurrency.end()) return;
        const auto released = std::min(found->second, amount);
        found->second -= released;
        if (found->second == 0) state.active_concurrency.erase(found);
        state.used.max_concurrency =
            state.used.max_concurrency >= released ? state.used.max_concurrency - released : 0;
    }

    void bind_budgeted_child_completion(const std::shared_ptr<detail::RunControl>& child,
                                        std::string_view                           owner_scope,
                                        std::string_view                           parent_run_id,
                                        std::string_view                           child_run_id) {
        const auto parent      = std::string(parent_run_id);
        const auto child_id    = child->logical_run_id;
        const auto concurrency = child->granted_budget.max_concurrency;
        // Resource release is independent of the parent join CAS.  A transient
        // publication failure must not strand the parent's active-concurrency
        // lease while the durable child record remains recoverable.
        child->add_logical_cleanup([weak_impl = weak_from_this(), parent, child_id, concurrency] {
            if (const auto live = weak_impl.lock())
                live->release_child_concurrency(parent, child_id, concurrency);
        });
        ::neograph::program::bind_child_completion(child, owner_scope, parent, child_id);
    }

    bool activate_child_concurrency(std::string_view parent_run_id,
                                    std::string_view child_run_id,
                                    const RunBudget& parent_budget,
                                    const RunBudget& child_budget) {
        std::lock_guard lock(mutex);
        const auto      found = child_reservations.find(std::string(parent_run_id));
        if (found == child_reservations.end()) return false;
        return activate_concurrency_locked(found->second, child_run_id, parent_budget,
                                           child_budget.max_concurrency);
    }

    void release_child_concurrency(std::string_view parent_run_id,
                                   std::string_view child_run_id,
                                   std::uint32_t    amount) noexcept {
        std::lock_guard lock(mutex);
        const auto      found = child_reservations.find(std::string(parent_run_id));
        if (found == child_reservations.end()) return;
        release_concurrency_locked(found->second, child_run_id, amount);
        if (found->second.used == RunBudget{} && found->second.permanent_by_child.empty() &&
            found->second.active_concurrency.empty())
            child_reservations.erase(found);
    }
    struct ChildConcurrencyLease {
        Impl*         owner = nullptr;
        std::string   parent_run_id;
        std::string   child_run_id;
        std::uint32_t amount = 0;

        ~ChildConcurrencyLease() {
            if (owner) owner->release_child_concurrency(parent_run_id, child_run_id, amount);
        }

        void commit() noexcept { owner = nullptr; }
    };

    bool reserve_child(std::string_view parent_run_id,
                       std::string_view child_run_id,
                       const RunBudget& parent_budget,
                       const RunBudget& child_budget) {
        const auto      permanent = permanent_child_reservation(child_budget);
        const auto      full      = child_reservation(child_budget);
        std::lock_guard lock(mutex);
        auto&           state    = child_reservations[std::string(parent_run_id)];
        const auto      child_id = std::string(child_run_id);
        if (state.permanent_by_child.find(child_id) != state.permanent_by_child.end() ||
            !budget_sum_fits(parent_budget, state.used, full))
            return false;
        add_budget(state.used, permanent);
        state.permanent_by_child.emplace(child_id, permanent);
        if (activate_concurrency_locked(state, child_id, parent_budget,
                                        child_budget.max_concurrency))
            return true;
        state.permanent_by_child.erase(child_id);
        subtract_budget(state.used, permanent);
        return false;
    }

    void release_child(std::string_view parent_run_id,
                       std::string_view child_run_id,
                       const RunBudget& child_budget) noexcept {
        (void)child_budget;
        std::lock_guard lock(mutex);
        const auto      found = child_reservations.find(std::string(parent_run_id));
        if (found == child_reservations.end()) return;
        const auto child = found->second.permanent_by_child.find(std::string(child_run_id));
        if (child == found->second.permanent_by_child.end()) return;
        subtract_budget(found->second.used, child->second);
        found->second.permanent_by_child.erase(child);
        release_concurrency_locked(found->second, child_run_id,
                                   std::numeric_limits<std::uint32_t>::max());
        if (found->second.used == RunBudget{} && found->second.permanent_by_child.empty() &&
            found->second.active_concurrency.empty())
            child_reservations.erase(found);
    }

    RunBudget reserved_children(std::string_view parent_run_id) {
        std::lock_guard lock(mutex);
        const auto      found = child_reservations.find(std::string(parent_run_id));
        return found == child_reservations.end() ? RunBudget{} : found->second.used;
    }

    asio::awaitable<RunBudget> hydrate_child_reservations(ProgramRunRecord parent,
                                                          ProgramJournalRecord journal) {
        if (journal.id != parent.journal_head())
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Child reservation recovery lost its parent journal");
        const auto children = parent.children();
        auto available = parent.remaining_budget();
        if (!children.empty()) {
            // A pending structured command owns the parent's spendable allocation.
            // This reconstructs the same ceiling used at admission; it does not
            // credit either the durable remainder or the compile ledger.
            available = restore_reserved_resources(available, journal.inflight_reservation);
        }
        std::set<std::string> child_ids;
        std::uint64_t admitted_wall = 0;
        for (const auto& child : children) {
            if (!child_ids.insert(child.child_run_id).second) {
                throw_runtime_diagnostic(
                    "P_CHILD_CONFLICT", "Persisted parent contains duplicate child records",
                    json{{"parent_run_id", parent.run_id()}, {"child_run_id", child.child_run_id}});
            }
            const auto wall = child.invocation.granted_budget.wall_time_ms;
            if (wall > std::numeric_limits<std::uint64_t>::max() - admitted_wall)
                throw_runtime_diagnostic("P_CHILD_BUDGET", "Durable child budget ledger overflowed");
            admitted_wall += wall;
        }

        auto reservation_capacity = available;
        std::unique_lock lock(mutex);
        if (admitted_wall > available.wall_time_ms) {
            const auto found = child_reservations.find(parent.logical_run_id());
            const bool restoring_wall = found == child_reservations.end() ||
                std::any_of(children.begin(), children.end(), [&](const auto& child) {
                    return !found->second.permanent_by_child.contains(child.child_run_id);
                });
            if (restoring_wall) {
                // Replacement transfers a smaller wall-time remainder, not a new grant.
                // Reconstruct already-admitted reservations against the immutable original
                // envelope; fresh admission still uses `available`, and recovery retains each
                // child's original deadline. Never perform history I/O under the runtime lock.
                lock.unlock();
                const auto lineage = (co_await transitions->load_run_lineage(
                    parent.owner_scope(), parent.run_id()));
                const auto original = lineage
                    ? (co_await transitions->load_generation(parent.owner_scope(), lineage->lineage_id(), 1))
                    : std::nullopt;
                const auto initial = original
                    ? (co_await transitions->load_generation_initial_publication(
                          parent.owner_scope(), lineage->lineage_id(), 1))
                    : std::nullopt;
                if (!initial || !initial->run_generation || !initial->run_lineage ||
                    lineage->owner_scope() != parent.owner_scope() ||
                    lineage->root_run_id() != parent.logical_run_id() ||
                    original->generation() != 1 || original->owner_scope() != parent.owner_scope() ||
                    original->lineage_id() != lineage->lineage_id() ||
                    original->id() != initial->run_generation->id() ||
                    !is_valid_program_run_lineage_initial(*initial->run_lineage, *original) ||
                    !does_program_run_generation_bind(*original, *initial->run_lineage,
                                                       initial->run_record) ||
                    initial->run_record.run_id() != lineage->root_run_id() ||
                    initial->run_record.logical_run_id() != parent.logical_run_id())
                    throw_runtime_diagnostic("P_CHILD_BUDGET",
                        "Persisted child reservations have no original wall-time envelope");
                const auto original_wall = initial->run_record.invocation().budget.wall_time_ms;
                if (admitted_wall > original_wall)
                    throw_runtime_diagnostic("P_CHILD_BUDGET",
                        "Persisted child wall reservations exceed the original envelope");
                reservation_capacity.wall_time_ms = original_wall;
                lock.lock();
            }
        }

        auto&           state = child_reservations[parent.logical_run_id()];
        for (const auto& child : children) {
            const auto permanent = permanent_child_reservation(child.invocation.granted_budget);
            const bool in_flight = child_execution_active(child);
            const auto existing  = state.permanent_by_child.find(child.child_run_id);
            if (existing == state.permanent_by_child.end()) {
                auto additional = permanent;
                if (in_flight)
                    additional.max_concurrency = child.invocation.granted_budget.max_concurrency;
                if (!budget_sum_fits(reservation_capacity, state.used, additional)) {
                    throw_runtime_diagnostic(
                        "P_CHILD_BUDGET",
                        "Persisted child reservations exceed the parent remainder",
                        json{{"parent_run_id", parent.run_id()},
                             {"child_run_id", child.child_run_id}});
                }
                add_budget(state.used, permanent);
                state.permanent_by_child.emplace(child.child_run_id, permanent);
            } else if (existing->second != permanent) {
                throw_runtime_diagnostic(
                    "P_CHILD_BUDGET", "Persisted child reservation changed after admission",
                    json{{"parent_run_id", parent.run_id()}, {"child_run_id", child.child_run_id}});
            }

            const auto active = state.active_concurrency.find(child.child_run_id);
            if (in_flight) {
                if (!activate_concurrency_locked(state, child.child_run_id, available,
                                                 child.invocation.granted_budget.max_concurrency)) {
                    throw_runtime_diagnostic(
                        "P_CHILD_BUDGET",
                        "Persisted child concurrency exceeds the parent remainder",
                        json{{"parent_run_id", parent.run_id()},
                             {"child_run_id", child.child_run_id}});
                }
            } else if (active != state.active_concurrency.end()) {
                release_concurrency_locked(state, child.child_run_id, active->second);
            }
        }
        co_return available;
    }
    asio::awaitable<ParentChildBudget> hydrate_parent_child_budget(std::string owner_scope,
                                                                   std::string parent_run_id,
                                                                   std::string child_run_id) {
        const auto child_lineage = (co_await transitions->load_run_lineage(owner_scope, child_run_id));
        if (child_lineage) child_run_id = child_lineage->root_run_id();
        const auto active = (co_await load_active_agent_run(transitions, owner_scope, parent_run_id));
        const auto head = active ? (co_await transitions->load_run_publication_head(
                                       owner_scope, active->run_id()))
                                 : std::nullopt;
        const auto* parent = head ? &head->run_record : nullptr;
        if (!parent) {
            throw_runtime_diagnostic("P_CHILD_PARENT",
                                     "Child recovery requires its durable parent run",
                                     json{{"parent_run_id", std::string(parent_run_id)},
                                          {"child_run_id", std::string(child_run_id)}});
        }
        const auto parent_state = parent->continuation().state;
        if (parent_state != ContinuationState::Running &&
            parent_state != ContinuationState::Interrupted &&
            parent_state != ContinuationState::AmbiguousEffect)
            throw_runtime_diagnostic("P_CHILD_PARENT", "Closed parent cannot restart child work");
        const auto child = find_child(*parent, child_run_id);
        if (!child || child->invocation.parent_run_id != parent_run_id) {
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Durable child relation does not match its parent",
                                     json{{"parent_run_id", std::string(parent_run_id)},
                                          {"child_run_id", std::string(child_run_id)}});
        }
        const auto available = (co_await hydrate_child_reservations(*parent, head->journal_record));
        co_return ParentChildBudget{available, child->invocation.granted_budget};
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
            // An injected executor is not the runtime's to join: its attempts finish there, holding this state.
            if (pool) pool->join();
            if (deadline_pool) deadline_pool->join();
        } catch (...) {
            std::terminate();
        }
    }

    RuntimeConfig                                          config;
    std::shared_ptr<GlobalChildQuotaState>                 global_child_quota;
    std::shared_ptr<ProcessRuntimeState>                   process_state;
    detail::PureMutex                                      mutex;
    // Held by fork, replace and migrate_graph from their first read to their publication, across store I/O.
    detail::OperationLock                                  fork_operation;
    bool                                                   stopping = false;
    std::unordered_map<std::string, ChildReservationState> child_reservations;
    std::vector<std::weak_ptr<detail::RunControl>>         controls;
};

ProgramRuntime::ProgramRuntime(RuntimeConfig config) {
    if (!config.catalog || !config.checkpoints || !config.transitions ||
        config.scheduler_threads == 0) {
        throw std::invalid_argument(
            "ProgramRuntime requires catalog, checkpoints, transitions, and at least one "
            "scheduler thread");
    }
    if (static_cast<bool>(config.host_admission) !=
        static_cast<bool>(config.host_admission_resolver)) {
        throw std::invalid_argument(
            "ProgramRuntime host admission requires both a controller and a request resolver");
    }
    impl_ = std::make_shared<Impl>(std::move(config));
}

ProgramRuntime::ProgramRuntime(std::shared_ptr<Impl> impl, Borrowed) noexcept
    : impl_(std::move(impl)), borrowed_(true) {}

ProgramRuntime ProgramRuntime::borrowed() const noexcept { return ProgramRuntime(impl_, Borrowed{}); }

ProgramRuntime::ProgramRuntime(ProgramRuntime&& other) noexcept
    : impl_(std::move(other.impl_)), borrowed_(other.borrowed_) {}

ProgramRuntime& ProgramRuntime::operator=(ProgramRuntime&& other) noexcept {
    if (this == &other) return *this;
    if (impl_ && !borrowed_) impl_->shutdown();
    impl_     = std::move(other.impl_);
    borrowed_ = other.borrowed_;
    return *this;
}

ProgramRuntime::~ProgramRuntime() {
    if (impl_ && !borrowed_) impl_->shutdown();
}

asio::awaitable<ProgramActiveRun> ProgramRuntime::start_active_core(
    RunInvocation invocation, std::shared_ptr<ProgramEventSink> events) {
    if (!invocation.program_version_id.empty() || !invocation.parent_run_id.empty()) {
        throw std::invalid_argument(
            "Active-version start accepts only a top-level request without a version id");
    }
    const auto binding = (co_await impl_->config.catalog->active_binding_async(invocation.owner_scope));
    if (!binding) {
        throw_runtime_diagnostic("P_ACTIVATION_NOT_FOUND",
                                 "No admitted Program activation exists for this owner");
    }
    const auto& activation = binding->activation;
    // Materialize the version from the observed activation, not a newer active
    // pointer that may have won a concurrent activation CAS.
    const auto resolved = (co_await impl_->config.catalog->resolve_version_async(
        invocation.owner_scope, binding->version.id()));
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    invocation.program_version_id = resolved->id();
    invocation.selected_activation = activation;
    invocation.validate();
    const auto owner_scope = invocation.owner_scope;
    auto handle = (co_await start_resolved_core(
        owner_scope, *resolved,
        runtime_projection(std::move(invocation), std::move(events)), true));
    co_return ProgramActiveRun{activation, std::move(handle)};
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_core(RunInvocation                     invocation,
                                                          std::shared_ptr<ProgramEventSink> events) {
    invocation.validate();
    if (!invocation.parent_run_id.empty()) {
        throw std::invalid_argument(
            "Top-level RunInvocation must not carry parent_run_id; use start_child");
    }

    const auto owner_scope = invocation.owner_scope;
    const auto resolved =
        (co_await impl_->config.catalog->resolve_version_async(owner_scope, invocation.program_version_id));
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }

    co_return (co_await start_resolved_core(owner_scope, *resolved,
                          runtime_projection(std::move(invocation), std::move(events))));
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_core(std::string       owner_scope,
                                                          ProgramVersion    version,
                                                          ProgramInvocation invocation) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (version.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    const auto resolved = (co_await impl_->config.catalog->resolve_version_async(owner_scope, version.id()));
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    co_return (co_await start_resolved_core(owner_scope, *resolved, std::move(invocation)));
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_resolved_core(std::string       owner_scope,
                                                                   ProgramVersion    version,
                                                                   ProgramInvocation invocation,
                                                                   bool              active_selection) {
    const auto selected = invocation.canonical_request
        ? invocation.canonical_request->selected_activation
        : std::nullopt;
    if (selected.has_value() != active_selection ||
        (selected &&
         (selected->owner_scope() != owner_scope ||
          selected->active_version_id() != version.id() ||
          selected->policy_snapshot_hash() != version.policy_snapshot().fingerprint()))) {
        throw_runtime_diagnostic(
            "P_ACTIVATION_MISMATCH",
            "Only an exact host-selected activation can bind a new run");
    }
    auto pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, version));
    validate_invocation(*pinned, invocation);
    const auto run_id =
        invocation.requested_run_id.empty() ? generate_run_id() : invocation.requested_run_id;
    const auto canonical = bind_runtime_invocation(invocation, owner_scope, version.id(), run_id);
    const auto core_thread_id = core_thread_identity(run_id, pinned->root->compiled_plan_identity);
    const auto binding_fingerprint = capability_binding_receipt_root(
        pinned->version.core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{canonical.input, canonical.budget,
                                          canonical.correlation_id, canonical.parent_run_id,
                                          invocation.child_depth};
    const auto transition_time = now_ms();
    std::optional<ProgramExecutionLease> execution_lease;
    if (supports_root_execution_lease(*pinned, canonical.budget) &&
        invocation.child_depth == 0) {
        if (!canonical.budget.wall_time_ms ||
            canonical.budget.wall_time_ms > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max() - transition_time)) {
            throw_runtime_diagnostic(
                "P_RUN_LEASE", "Program execution lease lifetime exceeds the supported range");
        }
        execution_lease.emplace(ProgramExecutionLeaseData{
            std::string(owner_scope), run_id, 1, version.id(), pinned->bundle.id(),
            pinned->root->core_name, pinned->root->compiled_plan_identity, core_thread_id,
            graph::Checkpoint::generate_id(), transition_time,
            transition_time + static_cast<std::int64_t>(canonical.budget.wall_time_ms)});
    }
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, 1, pinned, binding_fingerprint,
        std::move(persisted), canonical, core_thread_id, 0, std::move(invocation.events),
        impl_->scheduler, impl_->config.checkpoints, impl_->config.state_store,
         impl_->transitions, execution_lease);
    control->set_hook_runtime(impl_->config.hook_runtime);
    control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
    control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
    control->set_core_provider_call_resolver(
        impl_->config.core_provider_call_resolver,
        impl_->config.require_core_provider_call_broker);

    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    auto publication = (co_await attach_run_lineage_async(
        *control->transitions,
        initial_publication(*control, started, std::nullopt, std::nullopt,
                            std::nullopt, std::nullopt, std::nullopt,
                            transition_time)));
    ProgramTransitionPublishResult published;
    if (execution_lease)
        published = (co_await control->transitions->compare_publish_execution(
              owner_scope, "", std::move(publication), std::nullopt, execution_lease));
    else
        published = (co_await control->transitions->compare_publish(
              owner_scope, "", std::move(publication)));
    if (published != ProgramTransitionPublishResult::Published) {
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }
    (co_await impl_->register_control(control));
    if (!(co_await admit_started(control, started, invocation.budget))) {
        co_return ProgramHandle(std::move(control));
    }

    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        detail::RunOutcome failed;
        failed.status                                  = ProgramTerminalStatus::Failed;
        failed.remaining_budget                        = invocation.budget;
        failed.usage.program_operations                = 1;
        failed.remaining_budget.max_program_operations = 0;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }

    spawn_run_attempt(impl_->scheduler, impl_, control, std::move(invocation.input), std::nullopt,
                      impl_->config.host_admission, impl_->config.host_admission_resolver);
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_child_core(std::string       owner_scope,
                                                               ProgramHandle     parent,
                                                               ModuleLinkReceipt link,
                                                               ProgramVersion    version,
                                                               ProgramInvocation invocation) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (!parent.control_) throw std::invalid_argument("Parent Program handle is empty");
    if (parent.control_->transitions.get() != impl_->transitions.get())
        throw_runtime_diagnostic("P_CHILD_PARENT", "Parent handle belongs to another runtime");
    if (parent.control_->owner_scope != owner_scope || link.owner_scope() != owner_scope)
        throw_runtime_diagnostic("P_CHILD_OWNER", "Child owner scope is not admitted");

    // Admission and reservation recovery must share one record/journal snapshot;
    // a concurrent child completion may advance both immediately after this read.
    std::optional<ProgramRunPublicationHead> parent_head;
    {
        auto lock = co_await child_relation_publication_mutex(*impl_->transitions, owner_scope,
                                                              parent.control_->logical_run_id)
                        .lock_async();
        parent_head = co_await impl_->transitions->load_run_publication_head(owner_scope, parent.run_id());
    }
    if (!parent_head)
        throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                 "Parent-child admission has no consistent transition head");
    auto parent_record = std::move(parent_head->run_record);
    auto parent_journal = std::move(parent_head->journal_record);
    if (parent_record.continuation().state != ContinuationState::Running)
        throw_runtime_diagnostic("P_CHILD_PARENT", "Child can only attach to a running parent");
    if (parent.control_->cancellation_cause() != detail::CancellationCause::None)
        throw_runtime_diagnostic("P_RUNTIME_CANCELLED", "Parent Program is already cancelling",
                                 json{{"parent_run_id", std::string(parent.run_id())}});
    if (version.ownership_scope() != owner_scope)
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    const auto resolved = (co_await impl_->config.catalog->resolve_version_async(owner_scope, version.id()));
    if (!resolved) throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    auto pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *resolved));

    const auto parent_depth = parent.control_->persisted_invocation.child_depth;
    if (parent_depth == std::numeric_limits<std::uint32_t>::max())
        throw_runtime_diagnostic("P_CHILD_DEPTH", "Child depth exceeds uint32 range");
    const std::string parent_run_id(parent.control_->logical_run_id);
    invocation.parent_run_id    = parent_run_id;
    invocation.child_depth      = parent_depth + 1;
    const auto run_id           = child_run_id_for(parent_run_id, link, invocation);
    invocation.requested_run_id = run_id;
    const auto canonical = bind_runtime_invocation(invocation, owner_scope, version.id(), run_id);
    const auto occupied  = (co_await impl_->transitions->load(owner_scope, run_id));
    if (occupied) {
        if (occupied->program_version_id() != version.id() ||
            occupied->bundle_id() != version.bundle_id() || occupied->invocation() != canonical ||
            occupied->child_depth() != invocation.child_depth) {
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT",
                "Requested child run id is already bound to another Program invocation",
                json{{"child_run_id", run_id}});
        }
    }
    auto existing = find_child(parent_record, run_id);
    if (occupied && !existing) {
        const auto active =
            (co_await load_active_agent_run(impl_->transitions, owner_scope, parent_run_id));
        auto head = active ? (co_await impl_->transitions->load_run_publication_head(
                                 owner_scope, active->run_id()))
                           : std::nullopt;
        const auto* durable_parent = head ? &head->run_record : nullptr;
        if (!durable_parent || durable_parent->continuation().state != ContinuationState::Running)
            throw_runtime_diagnostic(
                "P_CHILD_PARENT", "Durable parent is no longer running for child recovery",
                json{{"parent_run_id", parent_run_id}, {"child_run_id", run_id}});
        existing = find_child(*durable_parent, run_id);
        if (!existing)
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT", "Existing child run has no durable parent admission",
                json{{"parent_run_id", parent_run_id}, {"child_run_id", run_id}});
        parent_record = std::move(head->run_record);
        parent_journal = std::move(head->journal_record);
    }
    const auto hydrated_parent_budget =
        (co_await impl_->hydrate_child_reservations(parent_record, parent_journal));
    if (existing) {
        const auto expected = child_record_for(run_id, link, invocation, existing->state);
        if (!same_child_metadata(*existing, expected)) {
            throw_runtime_diagnostic(
                "P_CHILD_CONFLICT", "Durable child identity is already bound to different metadata",
                json{{"child_run_id", run_id}});
        }
        try {
            const auto persisted_link = ModuleLinkReceipt::parse(existing->link_receipt);
            if (persisted_link.id() != link.id() || persisted_link.owner_scope() != owner_scope ||
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
            throw_runtime_diagnostic("P_CHILD_RECEIPT", "Durable child receipt is malformed",
                                     json{{"child_run_id", run_id}, {"detail", error.what()}});
        }
    }
    RunBudget parent_budget = hydrated_parent_budget;
    if (!existing) {
        if (parent_journal.id != parent_record.journal_head()) {
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Parent-child admission lost the transition CAS");
        }
        parent_budget = restore_reserved_resources(parent_record.remaining_budget(),
                                                    parent_journal.inflight_reservation);
        const auto reserved = impl_->reserved_children(parent_run_id);
        validate_child_link(*pinned, link, version, invocation, parent_budget, reserved);
        validate_invocation(*pinned, invocation, true);
    }

    std::shared_ptr<ChildQuotaReservation> global_quota;
    if (!existing) global_quota = impl_->reserve_active_child(owner_scope);

    bool reservation_active = false;
    if (!existing) {
        if (!impl_->reserve_child(parent_run_id, run_id, parent_budget, invocation.budget)) {
            throw_runtime_diagnostic("P_CHILD_BUDGET",
                                     "Child budget exceeds the parent's unspent allocation");
        }
        reservation_active = true;
        try {
            const auto publishing =
                child_record_for(run_id, link, invocation, ProgramChildState::Publishing);
            const auto attached = (co_await publish_child_record(parent.control_, publishing));
            if (!attached.record) {
                impl_->release_child(parent_run_id, run_id, invocation.budget);
                reservation_active = false;
                throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                         "Parent-child attachment lost its transition CAS");
            }
            if (attached.result == ProgramTransitionPublishResult::AlreadyPresent) {
                impl_->release_child(parent_run_id, run_id, invocation.budget);
                reservation_active = false;
                if (global_quota) {
                    global_quota->release();
                    global_quota.reset();
                }
            } else {
                // The immutable child reservation is now durable. It must remain
                // charged even if the in-memory dispatch is created later.
                reservation_active = false;
            }
        } catch (...) {
            if (reservation_active) impl_->release_child(parent_run_id, run_id, invocation.budget);
            throw;
        }
    }

    bool replay_initial_dispatch = false;
    if (const auto child_record = (co_await impl_->transitions->load(owner_scope, run_id))) {
        const auto durable_parent =
            (co_await load_active_agent_run(impl_->transitions, owner_scope, parent_run_id));
        const auto durable_child  = durable_parent ? find_child(*durable_parent, run_id)
                                                   : std::optional<ProgramChildRecord>{};
        replay_initial_dispatch =
            ((existing && existing->state == ProgramChildState::Publishing) ||
             (durable_child && durable_child->state == ProgramChildState::Publishing)) &&
            (co_await is_unstarted_child_dispatch(impl_->transitions, owner_scope, run_id));
        if (auto live = impl_->find_control(owner_scope, run_id)) {
            if (global_quota) {
                global_quota->release();
                global_quota.reset();
            }
            impl_->bind_budgeted_child_completion(live, owner_scope, parent_run_id, run_id);
            co_await parent.control_->attach_child(live);
            co_return ProgramHandle(std::move(live));
        }
        if (child_record->continuation().state == ContinuationState::Running &&
            !replay_initial_dispatch) {
            if (global_quota) {
                global_quota->release();
                global_quota.reset();
            }
            auto reconnected = (co_await reconnect_core(owner_scope, run_id));
            impl_->bind_budgeted_child_completion(reconnected.control_, owner_scope, parent_run_id,
                                                  run_id);
            co_await parent.control_->attach_child(reconnected.control_);
            if (const auto result = reconnected.try_result())
                (co_await publish_child_completion(impl_->transitions, owner_scope, parent_run_id,
                                         run_id, *result));
            co_return reconnected;
        }
        if (child_record->continuation().state != ContinuationState::Running) {
            if (global_quota) {
                global_quota->release();
                global_quota.reset();
            }
            auto reconnected = (co_await reconnect_core(owner_scope, run_id));
            impl_->bind_budgeted_child_completion(reconnected.control_, owner_scope, parent_run_id,
                                                  run_id);
            co_await parent.control_->attach_child(reconnected.control_);
            if (const auto result = reconnected.try_result())
                (co_await publish_child_completion(impl_->transitions, owner_scope, parent_run_id,
                                         run_id, *result));
            co_return reconnected;
        }
    }

    std::shared_ptr<detail::RunControl> control;
    try {
        const auto core_thread_id =
            core_thread_identity(run_id, pinned->root->compiled_plan_identity);
        const auto binding_fingerprint = capability_binding_receipt_root(
            pinned->version.core_materialization_receipt().capability_bindings);
        ProgramPersistedInvocation persisted{canonical.input, canonical.budget,
                                             canonical.correlation_id, canonical.parent_run_id,
                                             invocation.child_depth};
        control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), run_id, 1, std::move(pinned), binding_fingerprint,
            std::move(persisted), canonical, core_thread_id, 0, std::move(invocation.events),
            impl_->scheduler, impl_->config.checkpoints,
             impl_->config.state_store, impl_->transitions);
        control->set_hook_runtime(impl_->config.hook_runtime);
        control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
        control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
        control->set_core_provider_call_resolver(
            impl_->config.core_provider_call_resolver,
            impl_->config.require_core_provider_call_broker);
        impl_->commit_active_child(control, global_quota);
        impl_->bind_budgeted_child_completion(control, owner_scope, parent_run_id, run_id);
        ProgramEvent started;
        if (replay_initial_dispatch) {
            const auto events = (co_await impl_->transitions->load_events(owner_scope, run_id));
            if (events.size() != 1 || events.front().kind != ProgramEventKind::Started ||
                events.front().sequence != 1) {
                throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                         "Durable child publication is incomplete",
                                         json{{"child_run_id", run_id}});
            }
            started = events.front();
            control->adopt_published_event(started);
        } else {
            started              = control->stage_event(ProgramEventKind::Started,
                                                        ProgramStartedEvent{invocation.budget});
            const auto published = (co_await compare_publish_with_lineage_async(
                *control->transitions, owner_scope, "", initial_publication(*control, started)));
            if (published != ProgramTransitionPublishResult::Published &&
                published != ProgramTransitionPublishResult::AlreadyPresent) {
                auto winner = (co_await impl_->transitions->load(owner_scope, run_id));
                if (!winner) {
                    throw_runtime_diagnostic("P_RUN_CONFLICT",
                                             "Requested child Program run id is unavailable");
                }
                auto reconnected = (co_await reconnect_core(owner_scope, run_id));
                impl_->bind_budgeted_child_completion(reconnected.control_, owner_scope,
                                                      parent_run_id, run_id);
                co_await parent.control_->attach_child(reconnected.control_);
                if (const auto result = reconnected.try_result())
                    (co_await publish_child_completion(impl_->transitions, owner_scope, parent_run_id,
                                             run_id, *result));
                co_return reconnected;
            }
        }

        // The durable relation is already present; this transition only records that
        // dispatch crossed the publication boundary.
        const auto dispatched = (co_await publish_child_record(
            parent.control_,
            child_record_for(run_id, link, invocation, ProgramChildState::Dispatched)));
        const auto dispatched_record = dispatched.record ? find_child(*dispatched.record, run_id)
                                                         : std::optional<ProgramChildRecord>{};
        if (!dispatched_record || dispatched_record->state != ProgramChildState::Dispatched) {
            if (dispatched_record && dispatched_record->state != ProgramChildState::Publishing) {
                auto reconnected = (co_await reconnect_core(owner_scope, run_id));
                impl_->bind_budgeted_child_completion(reconnected.control_, owner_scope,
                                                      parent_run_id, run_id);
                co_await parent.control_->attach_child(reconnected.control_);
                if (const auto result = reconnected.try_result())
                    (co_await publish_child_completion(impl_->transitions, owner_scope, parent_run_id,
                                             run_id, *result));
                co_return reconnected;
            }
            const auto current_parent =
                (co_await load_active_agent_run(impl_->transitions, owner_scope, parent_run_id));
            if (current_parent &&
                current_parent->continuation().state != ContinuationState::Running)
                (void)co_await control->cancel_async(detail::CancellationCause::ParentTerminal);
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Parent-child dispatch lost its transition CAS",
                                     json{{"child_run_id", run_id}});
        }

        co_await parent.control_->attach_child(control);
        (co_await impl_->register_control(control));
        if (!(co_await admit_started(control, started, invocation.budget))) {
            co_return ProgramHandle(std::move(control));
        }
        std::optional<std::string> sink_error;
        try {
            control->deliver_event(started);
        } catch (const std::exception& error) {
            sink_error = error.what();
        }
        if (sink_error) {
            detail::RunOutcome failed;
            failed.status                                  = ProgramTerminalStatus::Failed;
            failed.remaining_budget                        = invocation.budget;
            failed.usage.program_operations                = 1;
            failed.remaining_budget.max_program_operations = 0;
            failed.failure =
                ProgramFailure{"P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
            (co_await control->complete_async(std::move(failed)));
            co_return ProgramHandle(std::move(control));
        }
    } catch (...) {
        throw;
    }
    spawn_run_attempt(impl_->scheduler, impl_, control, std::move(invocation.input), std::nullopt,
                      impl_->config.host_admission, impl_->config.host_admission_resolver);
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::replay_recorded_core(
    std::string source_run_id, RunInvocation invocation,
    RecordedBindingSet recorded, std::shared_ptr<ProgramEventSink> events) {
    invocation.validate();
    if (source_run_id.empty() || !invocation.parent_run_id.empty())
        throw_runtime_diagnostic("P_REPLAY_SOURCE", "Recorded replay requires a top-level source");
    const auto owner = invocation.owner_scope;
    const auto source = (co_await impl_->transitions->load(owner, source_run_id));
    if (!source || !source->terminal_result())
        throw_runtime_diagnostic("P_REPLAY_SOURCE", "Recorded replay source is not terminal");
    const auto terminal = *source->terminal_result();
    if (terminal.status() == ProgramTerminalStatus::Interrupted ||
        terminal.status() == ProgramTerminalStatus::AmbiguousEffect)
        throw_runtime_diagnostic("P_REPLAY_SOURCE",
            "Recorded replay cannot allocate custody from an unresolved source");
    const auto& authority = terminal.provider_budget_authority();
    if (!authority)
        throw_runtime_diagnostic("P_REPLAY_CUSTODY", "Source lacks authenticated provider bank custody");
    if (source->owner_scope() != owner || source->program_version_id() != invocation.program_version_id ||
        source->invocation().input != invocation.input || source->invocation().budget != invocation.budget ||
        source->child_depth() != 0 || !source->invocation().parent_run_id.empty())
        throw_runtime_diagnostic("P_REPLAY_PERMISSION", "Replay differs from its original source permissions");
    const auto version = (co_await detail::CatalogRuntimeAccess::load_admitted_version_async(
        *impl_->config.catalog, owner, source->program_version_id()));
    if (!version)
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Recorded source version was not found");
    recorded.validate_target(*version);
    const auto fingerprint = recorded.fingerprint();
    const auto source_events = (co_await impl_->transitions->load_events(owner, source_run_id));
    auto pinned = (co_await detail::CatalogRuntimeAccess::pin_recorded_async(
        *impl_->config.catalog, *version, std::move(recorded), source_events));
    if (version->execution_guarantee() == ExecutionGuarantee::Unmanaged &&
        !pinned->bundle.control_source())
        throw_runtime_diagnostic("P_REPLAY_GUARANTEE",
            "Unmanaged native execution cannot be admitted as recorded replay");
    auto projection = runtime_projection(invocation, events);
    // Validate immutable permissions, not the already-spent resource remainder.
    validate_invocation(*pinned, projection);
    const auto binding = capability_binding_receipt_root(
        version->core_materialization_receipt().capability_bindings);
    if (binding != source->binding_fingerprint() || pinned->bundle.id() != source->bundle_id())
        throw_runtime_diagnostic("P_REPLAY_BINDING", "Source execution identity differs from its pinned version");
    const auto run_id = invocation.run_id;
    if (const auto existing = (co_await impl_->transitions->load(owner, run_id))) {
        const auto lineage = (co_await impl_->transitions->load_run_lineage(owner, run_id));
        const auto initial = lineage
            ? (co_await impl_->transitions->load_generation_initial_publication(
                owner, lineage->lineage_id(), 1)) : std::nullopt;
        if (existing->invocation() != invocation ||
            existing->recorded_binding_set_fingerprint() != std::optional<std::string>(fingerprint) ||
            !initial || !initial->recorded_replay_source ||
            initial->recorded_replay_source->run_id != source_run_id ||
            initial->recorded_replay_source->run_record_id != source->id())
            throw_runtime_diagnostic("P_RUN_CONFLICT", "Recorded replay retry differs from its durable receipt");
        if (auto active = impl_->find_control(owner, run_id)) co_return ProgramHandle(std::move(active));
        if (auto active = impl_->find_process_control(owner, run_id))
            co_return ProgramHandle(std::move(active));
        if (existing->terminal_result())
            co_return ProgramHandle((co_await impl_->make_record_control(*existing)));
        const auto lease = (co_await impl_->transitions->load_execution_lease(owner, run_id));
        if (!lease || now_ms() < lease->expires_at_ms())
            throw_runtime_diagnostic("P_REPLAY_LEASE_HELD", "Recorded replay dispatch custody is still held");
        ProgramPersistedInvocation persisted{
            invocation.input, existing->remaining_budget(), invocation.correlation_id, "", 0};
        auto control = std::make_shared<detail::RunControl>(
            owner, run_id, existing->continuation().attempt, std::move(pinned), binding,
            std::move(persisted), invocation, lease->core_thread_id(), existing->event_sequence(),
            events, impl_->scheduler, impl_->config.checkpoints,
            impl_->config.state_store, impl_->transitions, lease);
        control->recorded_replay = std::make_shared<detail::RecordedRunReplay>();
        control->recorded_replay->source_run_id = std::string(source_run_id);
        detail::RunOutcome failed;
        failed.status = ProgramTerminalStatus::Failed;
        failed.remaining_budget = existing->remaining_budget();
        // Lost replay execution has uncertain CPU/Core consumption. Fence it;
        // neither the transferred resources nor provider holds are refunded.
        failed.remaining_budget.wall_time_ms = 0;
        failed.remaining_budget.max_core_steps = 0;
        failed.usage.model_tokens = terminal.usage().model_tokens;
        failed.provider_budget_authority = authority;
        failed.failure = ProgramFailure{"P_REPLAY_EXECUTION_LOST",
            "Recorded replay execution was lost after its durable allocation",
            "root", "", 0, json::object()};
        (co_await impl_->register_control(control));
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }
    const auto active_source = (co_await load_active_run_lineage_async(*impl_->transitions, *source));
    if (!active_source || active_source->lineage.committed_descendant_budget() != RunBudget{} ||
        active_source->lineage.inflight_reservation() != RunBudget{})
        throw_runtime_diagnostic("P_REPLAY_CUSTODY", "Source does not have transferable exact replay custody");
    const auto available = active_source->lineage.remaining_budget();
    if (available.wall_time_ms == 0 ||
        (!pinned->bundle.control_source() && available.max_core_steps == 0))
        throw_runtime_diagnostic("P_REPLAY_BUDGET", "Source has no remaining replay work authority");
    auto replay = std::make_shared<detail::RecordedRunReplay>();
    replay->source_run_id = std::string(source_run_id);
    replay->provider_authority = *authority;
    for (const auto& event : source_events) {
        if (event.kind == ProgramEventKind::OperationStarted)
            replay->operations[event.operation_id].push_back(std::get<ProgramOperationEvent>(event.payload));
    }
    if (!pinned->bundle.control_source() && replay->operations.empty())
        throw_runtime_diagnostic("P_REPLAY_COORDINATES", "Source lacks exact captured operation coordinates");
    auto commands = (co_await impl_->transitions->load_javascript_commands(owner, source_run_id));
    for (const auto& command : commands) {
        if (!command.pending()) continue;
        const auto completion = std::find_if(commands.begin(), commands.end(), [&](const auto& candidate) {
            return candidate.command_ordinal() == command.command_ordinal() && candidate.completed();
        });
        if (completion == commands.end())
            throw_runtime_diagnostic("P_REPLAY_EVIDENCE_REQUIRED", "Source command lacks a captured terminal outcome");
    }
    replay->commands = std::move(commands);
    const auto contexts = (co_await impl_->transitions->load_context_publications(owner, source_run_id));
    const auto thread_id = core_thread_identity(run_id, pinned->root->compiled_plan_identity);
    ProgramPersistedInvocation persisted{
        invocation.input, available, invocation.correlation_id, "", 0};
    auto control = std::make_shared<detail::RunControl>(
        owner, run_id, 1, std::move(pinned), binding, std::move(persisted), invocation,
        thread_id, 0, events, impl_->scheduler, impl_->config.checkpoints,
        impl_->config.state_store, impl_->transitions);
    control->recorded_replay = std::move(replay);
    const auto started = control->stage_event(
        ProgramEventKind::Started, ProgramStartedEvent{available});
    auto publication = initial_publication(
        *control, started, std::nullopt, std::nullopt, fingerprint);
    if (!contexts.empty())
        publication.context_publication = transfer_context_to_run(contexts.back(), run_id);
    publication = (co_await attach_run_lineage_async(*impl_->transitions, std::move(publication)));
    publication.recorded_replay_source = ProgramRecordedReplaySource{
        source->run_id(), source->id(), source->journal_head(), active_source->lineage.id()};
    publication.recorded_replay_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        active_source->lineage.owner_scope(), active_source->lineage.lineage_id(),
        active_source->lineage.root_run_id(), active_source->lineage.active_generation(),
        active_source->lineage.active_generation_id(), active_source->lineage.active_run_record_id(),
        active_source->lineage.active_journal_head(), debit_budget(available, available),
        active_source->lineage.inflight_reservation(), active_source->lineage.id(),
        active_source->lineage.created_at_ms(),
        std::max(active_source->lineage.updated_at_ms(), publication.run_record.updated_at_ms()),
        active_source->lineage.committed_descendant_budget()});
    const auto acquired = publication.run_record.updated_at_ms();
    if (available.wall_time_ms > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - acquired))
        throw_runtime_diagnostic("P_REPLAY_BUDGET", "Replay lease lifetime cannot be represented");
    const auto lease = ProgramExecutionLease(ProgramExecutionLeaseData{
        owner, run_id, 1, control->program_version_id, control->bundle_id,
        control->materialized->root->core_name, control->materialized->root->compiled_plan_identity,
        control->core_thread_id, graph::Checkpoint::generate_id(), acquired,
        acquired + static_cast<std::int64_t>(available.wall_time_ms)});
    const auto published = (co_await impl_->transitions->compare_publish_execution(
        owner, "", std::move(publication), std::nullopt, lease));
    if (published != ProgramTransitionPublishResult::Published)
        throw_runtime_diagnostic("P_REPLAY_ALLOCATION_CONFLICT", "Source replay allocation lost its exact CAS");
    control->set_execution_lease(lease);
    (co_await impl_->register_control(control));
    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        detail::RunOutcome failed;
        failed.status = ProgramTerminalStatus::Failed;
        failed.remaining_budget = available;
        failed.usage.model_tokens = terminal.usage().model_tokens;
        failed.provider_budget_authority = authority;
        failed.failure = ProgramFailure{"P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }
    spawn_run_attempt(impl_->scheduler, impl_, control, invocation.input, std::nullopt,
                      impl_->config.host_admission, impl_->config.host_admission_resolver);
    co_return ProgramHandle(std::move(control));
}


asio::awaitable<ProgramHandle> ProgramRuntime::fork_core(ExactProgramCheckpointReference   source,
                                                         RunInvocation                     invocation,
                                                         ProgramResume                     resume_value,
                                                         std::shared_ptr<ProgramEventSink> events) {
    invocation.validate();
    if (!invocation.parent_run_id.empty()) {
        throw std::invalid_argument(
            "Top-level RunInvocation must not carry parent_run_id; use start_child");
    }

    const auto owner_scope = invocation.owner_scope;
    const auto resolved =
        (co_await impl_->config.catalog->resolve_version_async(owner_scope, invocation.program_version_id));
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    co_return (co_await fork_core(owner_scope, std::move(source), *resolved,
                runtime_projection(std::move(invocation), std::move(events)),
                std::move(resume_value)));
}

asio::awaitable<ProgramHandle> ProgramRuntime::fork_core(std::string                     owner_scope,
                                                         ExactProgramCheckpointReference source,
                                                         ProgramVersion                  target,
                                                         ProgramInvocation               invocation,
                                                         ProgramResume                   resume_value) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    auto fork_lock = co_await impl_->fork_operation.lock_async();
    if (!invocation.parent_run_id.empty() || invocation.child_depth != 0) {
        throw std::invalid_argument(
            "Program fork invocation must be top-level; child forks require parent admission");
    }
    if (source.source_run_id.empty() || source.source_checkpoint_id.empty()) {
        throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND", "Program checkpoint was not found");
    }
    const auto run_id =
        invocation.requested_run_id.empty() ? generate_run_id() : invocation.requested_run_id;
    if (run_id == source.source_run_id) {
        throw_runtime_diagnostic("P_FORK_SAME_RUN",
                                 "Fork target run id must differ from the exact source run");
    }
    if (target.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    const auto resolved_target = (co_await impl_->config.catalog->resolve_version_async(owner_scope, target.id()));
    if (!resolved_target) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto target_pinned =
        (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *resolved_target));
    // #395: a fork of a JavaScript main() runs main() again with the source
    // run's input, as a resume of the source does, so that it yields the
    // commands the source's journal recorded.
    if (target_pinned->bundle.control_source()) {
        if (const auto source_run =
                (co_await impl_->transitions->load(owner_scope, source.source_run_id))) {
            invocation.input = source_run->invocation().input;
            if (invocation.canonical_request)
                invocation.canonical_request->input = source_run->invocation().input;
        }
    }
    const auto canonical = bind_runtime_invocation(invocation, owner_scope, target.id(), run_id);
    const auto reconnect_existing = [&]() -> asio::awaitable<std::optional<ProgramHandle>> {
        const auto existing = (co_await impl_->transitions->load(owner_scope, run_id));
        if (!existing) co_return std::nullopt;
        const auto fork = existing->fork_receipt();
        const auto plan = (co_await impl_->transitions->load_migration_plan(owner_scope, run_id));
        const auto source_version = fork ? (co_await impl_->config.catalog->resolve_version_async(
                                               owner_scope, fork->source_program_version_id()))
                                         : std::nullopt;
        const auto expected_plan = source_version
                                       ? std::optional<MigrationPlan>(
                                             (co_await impl_->config.catalog->plan_migration_async(
                                                 owner_scope, source_version->id(), target.id())))
                                       : std::nullopt;
        if (existing->program_version_id() == target.id() &&
            existing->bundle_id() == target.bundle_id() && existing->invocation() == canonical &&
            fork && fork->compatible() && fork->source_run_id() == source.source_run_id &&
            fork->source_checkpoint_id() == source.source_checkpoint_id &&
            fork->target_program_version_id() == target.id() &&
            fork_resume_matches(*fork, *existing, resume_value) && plan && expected_plan &&
            expected_plan->is_compatible() && plan->semantically_matches(*expected_plan)) {
            co_return (co_await reconnect_core(owner_scope, run_id));
        }
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    };
    if (auto existing = co_await reconnect_existing()) co_return std::move(*existing);

    const auto source_record = (co_await impl_->transitions->load(owner_scope, source.source_run_id));
    if (!source_record || !source_record->exact_checkpoint() ||
        source_record->exact_checkpoint()->checkpoint_id != source.source_checkpoint_id) {
        throw_runtime_diagnostic("P_CHECKPOINT_NOT_FOUND", "Program checkpoint was not found");
    }
    const auto source_lineage = (co_await load_active_run_lineage_async(*impl_->transitions, *source_record));
    if (!source_lineage) {
        throw_runtime_diagnostic("P_FORK_LINEAGE",
                                 "Program fork source has no durable lineage head");
    }
    if (source_lineage->lineage.committed_descendant_budget() != RunBudget{}) {
        throw_runtime_diagnostic(
            "P_FORK_DESCENDANTS",
            "Program fork cannot replace a generation with committed descendant budget");
    }
    if (source_record->child_depth() != 0 || !source_record->invocation().parent_run_id.empty())
        throw_runtime_diagnostic("P_FORK_CHILD", "Child Program generations cannot be forked");
    if (source_record->continuation().state != ContinuationState::Interrupted) {
        throw_runtime_diagnostic("P_FORK_STATE",
                                 "Program fork source is not at an admitted durable boundary");
    }
    const auto source_runtime_state = (co_await impl_->transitions->load_effective_runtime_state(
        owner_scope, source_record->run_id()));
    const auto& source_contexts = source_runtime_state.context_publications;
    if (!source_contexts.empty() && !impl_->config.runtime_recovery_handler) {
        throw_runtime_diagnostic(
            "P_RUNTIME_RECOVERY_UNAVAILABLE",
            "Program fork cannot clone runtime context without a recovery boundary");
    }
    const auto source_version =
        (co_await impl_->config.catalog->resolve_version_async(owner_scope, source_record->program_version_id()));
    if (!source_version) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto source_pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *source_version));
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
    const auto migration_plan = MigrationPlan::between(*source_version, source_pinned->bundle,
                                                       *resolved_target, target_pinned->bundle);
    if (!migration_plan.is_compatible()) {
        throw_runtime_diagnostic(
            "P_FORK_MIGRATION_INCOMPATIBLE", "Program fork migration plan is not compatible",
            json{{"migration_plan_id", migration_plan.id()},
                 {"compatibility", std::string(to_string(migration_plan.compatibility()))},
                 {"blockers", migration_plan.blockers()}});
    }

    const auto remaining = source_lineage->lineage.remaining_budget();
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
    if (!budget_leq(invocation.budget, target_pinned->version.policy_snapshot().budget_ceiling())) {
        throw_runtime_diagnostic("P_FORK_BUDGET",
                                 "Fork continuation budget exceeds target authority ceiling");
    }
    const auto& target_ceiling = target_pinned->version.policy_snapshot().budget_ceiling();
    // A fork carries a *remainder*, so the initial admission minimum may
    // already have been consumed by the source run. Its upper bounds and
    // authority ceiling still constrain the target continuation.
    for (const auto& requirement : target_pinned->bundle.declared_budget_requirements()) {
        const auto granted = budget_value(invocation.budget, requirement.resource);
        const auto allowed = ceiling_value(target_ceiling, requirement.resource);
        if (granted > requirement.maximum || granted > allowed) {
            throw_runtime_diagnostic("P_FORK_BUDGET",
                                     "Fork continuation budget exceeds target admitted bounds",
                                     json{{"resource", requirement.resource},
                                          {"maximum", requirement.maximum},
                                          {"policy_ceiling", allowed},
                                          {"granted", granted}});
        }
    }
    // #414: every resume of the source needs wall time and Core steps. A fork
    // that takes all it has left would leave the source unable to run again,
    // so it is refused here, before anything is cloned or debited.
    const auto leaves_the_source_nothing = [](const char* resource, std::uint64_t left,
                                              std::uint64_t taken) {
        if (taken != left) return;
        throw_runtime_diagnostic(
            "P_FORK_SOURCE_BUDGET",
            std::string("Program fork would leave its source run without ") + resource +
                ", so the source could not resume; give the fork a share of the source remainder",
            json{{"resource", resource},
                 {"source_remaining", left},
                 {"fork_budget", taken},
                 {"fork_budget_at_most", left - 1}});
    };
    leaves_the_source_nothing("wall_time_ms", remaining.wall_time_ms, requested.wall_time_ms);
    leaves_the_source_nothing("max_core_steps", remaining.max_core_steps, requested.max_core_steps);
    auto       fork_pending_input  = source_record->pending_input();
    auto       fork_pending_effect = source_record->pending_effect();
    std::optional<std::string> fork_initial_pending_id;
    const auto transition_time     = static_cast<std::uint64_t>(now_ms());
    if (fork_pending_input) {
        const auto update = fork_pending_input->submit(resume_value.pending_id, resume_value.value,
                                                       transition_time);
        if (update.disposition != ProgramPendingDisposition::Applied) {
            throw_runtime_diagnostic(update.diagnostic_code, update.message);
        }
        fork_pending_input = update.value;
        fork_initial_pending_id = fork_pending_input->call_id();
    } else if (fork_pending_effect) {
        const auto update =
            fork_pending_effect->submit(resume_value.pending_id, fork_pending_effect->effect_id(),
                                        resume_value.value, transition_time);
        if (update.disposition != ProgramPendingDisposition::Applied) {
            throw_runtime_diagnostic(update.diagnostic_code, update.message);
        }
        fork_pending_effect = update.value;
        fork_initial_pending_id = fork_pending_effect->call_id();
    } else if (!resume_value.pending_id.empty()) {
        throw_runtime_diagnostic("P_PENDING_WRONG_ID",
                                 "Source run has no matching pending operation");
    }
    receipt =
        receipt.with_initial_resume_binding(std::move(fork_initial_pending_id), resume_value.value);

    // #395: a fork of a JavaScript main() continues the command its source
    // paused in, as a resume of the source does. It carries the source's
    // command journal, rebound to the target bundle, so that the commands
    // that finished replay without running or charging again, and its copy
    // of the paused call's checkpoint lives on that call's Core thread in the
    // fork, where the attempt's exact resume looks for it.
    std::vector<ProgramJavaScriptCommandJournalEntry> fork_commands;
    std::optional<std::string>                        fork_core_operation;
    if (target_pinned->bundle.control_source()) {
        const auto source_commands =
            (co_await impl_->transitions->load_javascript_commands(owner_scope, source.source_run_id));
        if (source_commands.empty() || !source_commands.back().pending())
            throw_runtime_diagnostic("P_FORK_STATE",
                                     "JavaScript Program fork source is not paused in a command");
        const auto& paused_command = source_commands.back();
        fork_core_operation        = javascript_call_core_operation_for_thread(
            paused_command.command(),
            "root.javascript." + std::to_string(paused_command.command_ordinal()),
            source.source_run_id, checkpoint_identity.core_generation_id,
            checkpoint_identity.core_thread_id);
        if (!fork_core_operation)
            throw_runtime_diagnostic(
                "P_FORK_STATE",
                "JavaScript Program fork source checkpoint is not its paused callCore's");
        const auto& target_bundle_id = target_pinned->bundle.id();
        // #398: a recorded failure's provider evidence is re-bound to the fork.
        const RecordCustody source_custody{source_record->run_id(),
                                           source_record->program_version_id(),
                                           source_record->bundle_id()};
        const RecordCustody target_custody{run_id, target.id(), target_bundle_id};
        const auto&         archive = impl_->config.native_history_archive;
        for (const auto& entry : source_commands) {
            auto terminal_result = entry.terminal_result();
            if (terminal_result)
                rebind_recorded_failure(*terminal_result, owner_scope, source_custody,
                                        target_custody, archive);
            auto settled_members = entry.settled_members();
            if (settled_members)
                settled_members = rebind_recorded_members(*settled_members, owner_scope,
                                                           source_custody, target_custody, archive);
            ProgramJavaScriptCommandJournalEntryData data{
                entry.sequence(), target_bundle_id, entry.command_ordinal(), entry.command(),
                entry.effect_identity()
                    ? std::optional<std::string>(detail::javascript_command_effect_identity(
                          target_bundle_id, entry.command_ordinal(), entry.command()))
                    : std::nullopt,
                std::move(terminal_result), std::move(settled_members)};
            data.await_started_at = entry.await_started_at();
            fork_commands.emplace_back(std::move(data));
        }
    }
    const auto target_thread_id =
        core_thread_identity(run_id, target_pinned->root->compiled_plan_identity);
    const auto cloned_thread_id =
        fork_core_operation ? core_thread_identity(run_id,
                                                   target_pinned->root->compiled_plan_identity,
                                                   *fork_core_operation)
                            : target_thread_id;
    auto cloned_checkpoint      = *loaded_checkpoint;
    cloned_checkpoint.id        = graph::Checkpoint::generate_id();
    cloned_checkpoint.thread_id = cloned_thread_id;
    cloned_checkpoint.parent_id = checkpoint_identity.checkpoint_id;
    cloned_checkpoint.metadata["forked_from"] =
        json{{"thread_id", checkpoint_identity.core_thread_id},
             {"checkpoint_id", checkpoint_identity.checkpoint_id}};
    cloned_checkpoint.timestamp =
        std::max<std::int64_t>(now_ms(), loaded_checkpoint->timestamp + 1);
    impl_->config.checkpoints->save(cloned_checkpoint);
    // #390: the fork replays the work the paused step finished, as a resume
    // of the source would, instead of running and charging it again.
    graph::copy_resume_pending_writes(*impl_->config.checkpoints, *loaded_checkpoint,
                                      cloned_checkpoint);
    const auto                   cloned_checkpoint_id = cloned_checkpoint.id;
    const CoreCheckpointIdentity target_checkpoint{
        target_pinned->root->core_name, target_pinned->root->compiled_plan_identity,
        cloned_thread_id, cloned_checkpoint_id, cloned_checkpoint.schema_version};
    const auto binding_fingerprint = capability_binding_receipt_root(
        resolved_target->core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{canonical.input, canonical.budget,
                                         canonical.correlation_id, canonical.parent_run_id,
                                         invocation.child_depth};
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, source_record->continuation().attempt + 1,
        std::move(target_pinned), binding_fingerprint, std::move(persisted), canonical,
        target_thread_id, 0, std::move(invocation.events), impl_->scheduler,
         impl_->config.checkpoints, impl_->config.state_store, impl_->transitions);
    // #405: the fork's first attempt repeats the provider calls the paused
    // invocation made; a journaling broker replays them from the source.
    control->fork_provider_replay = detail::RunControl::ForkProviderReplay{
        source_record->run_id(), source_record->program_version_id(),
        checkpoint_identity.core_generation_id,
        core_thread_identity(source_record->run_id(), checkpoint_identity.core_generation_id),
        checkpoint_identity.core_thread_id, source_record->continuation().attempt};
    // #431: tell the caller when the host cannot replay those calls.
    control->notices = fork_provider_call_notices(impl_->config.core_provider_calls_journaled,
                                                  *source_record);
    control->set_hook_runtime(impl_->config.hook_runtime);
    control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
    control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
    control->set_core_provider_call_resolver(
        impl_->config.core_provider_call_resolver,
        impl_->config.require_core_provider_call_broker);
    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{invocation.budget});
    // The fork's first attempt resumes the cloned checkpoint with the resume
    // value; its recovery must too (#377), for a JavaScript main() as well
    // (#395).
    auto fork_resume = std::optional<ProgramAttemptResume>{
        ProgramAttemptResume{target_checkpoint.checkpoint_id, resume_value.value}};
    // #386: the fork replays what the source's paused plan finished, as a
    // resume of the source would.
    auto fork_records = continued_replay_records(*source_record, run_id, *control->materialized,
                                                 *impl_->config.checkpoints,
                                                 impl_->config.native_history_archive);
    auto publication =
        initial_publication(*control, started, target_checkpoint, receipt, std::nullopt,
                            std::move(fork_pending_input), std::move(fork_pending_effect),
                            std::nullopt, std::nullopt, {}, std::move(fork_resume),
                            std::move(fork_records));
    publication.commands = std::move(fork_commands);
    if (!source_contexts.empty()) {
        publication.context_publication = transfer_context_to_run(source_contexts.back(), run_id);
    }
    publication.migration_plan = migration_plan;
    publication = (co_await attach_run_lineage_async(*control->transitions, std::move(publication)));
    publication.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        source_lineage->lineage.owner_scope(), source_lineage->lineage.lineage_id(),
        source_lineage->lineage.root_run_id(), source_lineage->lineage.active_generation(),
        source_lineage->lineage.active_generation_id(),
        source_lineage->lineage.active_run_record_id(),
        source_lineage->lineage.active_journal_head(),
        debit_fork_share(source_lineage->lineage.remaining_budget(), requested),
        source_lineage->lineage.inflight_reservation(), source_lineage->lineage.id(),
        source_lineage->lineage.created_at_ms(),
        std::max(source_lineage->lineage.updated_at_ms(), publication.run_record.updated_at_ms()),
        source_lineage->lineage.committed_descendant_budget()});
    const auto cloned_context = publication.context_publication;
    const auto execution_lease =
        make_execution_lease(*control->materialized, publication);
    ProgramTransitionPublishResult published;
    if (execution_lease)
        published = (co_await control->transitions->compare_publish_execution(
              owner_scope, "", std::move(publication), std::nullopt,
              execution_lease));
    else
        published = (co_await control->transitions->compare_publish(
              owner_scope, "", std::move(publication)));
    if (published != ProgramTransitionPublishResult::Published) {
        if (auto existing = co_await reconnect_existing()) co_return std::move(*existing);
        throw_runtime_diagnostic("P_RUN_CONFLICT", "Requested Program run id is unavailable");
    }
    std::optional<MigrationPlan> durable_plan;
    try {
        durable_plan = (co_await control->transitions->load_migration_plan(owner_scope, run_id));
    } catch (const std::exception& error) {
        throw_runtime_diagnostic("P_FORK_MIGRATION_PROOF",
                                 "Fork publication migration proof could not be read back",
                                 json{{"run_id", run_id}, {"detail", error.what()}});
    }
    if (!durable_plan || durable_plan->id() != migration_plan.id()) {
        throw_runtime_diagnostic(
            "P_FORK_MIGRATION_PROOF", "Fork publication did not durably retain its migration proof",
            json{{"run_id", run_id},
                 {"expected_plan_id", migration_plan.id()},
                 {"stored_plan_id", durable_plan ? durable_plan->id() : std::string{}}});
    }
    if (execution_lease) control->set_execution_lease(*execution_lease);
    (co_await impl_->register_control(control));
    fork_lock.unlock();
    if (cloned_context) {
        impl_->config.runtime_recovery_handler(ProgramRuntimeRecoveryState{
            std::string(owner_scope), run_id, {*cloned_context}, {}, {}, std::nullopt});
    }
    if (!(co_await admit_started(control, started, invocation.budget, target_checkpoint))) {
        co_return ProgramHandle(std::move(control));
    }
    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = invocation.budget;
        failed.checkpoint       = target_checkpoint;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }
    // #395: a JavaScript main() runs again with the source's input and
    // resumes its paused callCore with the resume value, as resume does.
    if (fork_core_operation) {
        spawn_run_attempt(impl_->scheduler, impl_, control, control->persisted_invocation.input,
                          target_checkpoint.checkpoint_id, impl_->config.host_admission,
                          impl_->config.host_admission_resolver, std::move(resume_value.value));
    } else {
        spawn_run_attempt(impl_->scheduler, impl_, control, std::move(resume_value.value),
                          target_checkpoint.checkpoint_id, impl_->config.host_admission,
                          impl_->config.host_admission_resolver);
    }
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_core(ExactProgramHandoffReference source,
                                                            RunInvocation                invocation,
                                                            std::shared_ptr<ProgramEventSink> events) {
    invocation.validate();
    const auto owner_scope = invocation.owner_scope;
    const auto resolved =
        (co_await impl_->config.catalog->resolve_version_async(owner_scope, invocation.program_version_id));
    if (!resolved) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    co_return (co_await replace_core(owner_scope, std::move(source), *resolved,
                   runtime_projection(std::move(invocation), std::move(events))));
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_core(ProgramHandoff*                source_lease,
                                                            RunInvocation                 invocation,
                                                            std::shared_ptr<ProgramEventSink> events) {
    if (!source_lease) throw std::invalid_argument("Program replacement requires an active handoff lease");
    ProgramHandoff& source = *source_lease;
    if (!source || !source.impl_ || !source.impl_->control) {
        throw std::invalid_argument("Program replacement requires an active handoff lease");
    }
    if (!same_process_coordination_backend(*source.impl_->control->transitions,
                                           *impl_->transitions)) {
        throw std::invalid_argument(
            "Program handoff lease and replacement runtime use different transition stores");
    }
    if (source.impl_->control->owner_scope != invocation.owner_scope ||
        source.impl_->control->run_id != source.reference().source_run_id) {
        throw std::invalid_argument("Program handoff lease does not bind the replacement source");
    }
    const auto source_record_id = (co_await source.impl_->control->snapshot_async()).id();
    std::exception_ptr failure;
    try {
        auto result = (co_await replace_core(source.reference(), std::move(invocation), std::move(events)));
        source.consume();
        if (!source.impl_->control->following_successor())
            (void)co_await source.impl_->control->cancel_async(detail::CancellationCause::ParentTerminal);
        co_return result;
    } catch (...) {
        failure = std::current_exception();
    }
    bool definitely_uncommitted = false;
    try {
        const auto lineage = (co_await impl_->transitions->load_run_lineage(
            source.impl_->control->owner_scope, source.impl_->control->run_id));
        definitely_uncommitted =
            lineage && lineage->active_run_record_id() == source_record_id;
    } catch (...) {}
    if (!definitely_uncommitted) {
        source.consume();
        if (!source.impl_->control->following_successor())
            (void)co_await source.impl_->control->cancel_async(detail::CancellationCause::ParentTerminal);
    }
    std::rethrow_exception(failure);
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_core(std::string                    owner_scope,
                                                            ExactProgramHandoffReference   source,
                                                            ProgramVersion                 target,
                                                            ProgramInvocation              invocation) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    auto transition_lock = co_await impl_->fork_operation.lock_async();
    auto reconnect_lock = co_await impl_->process_state->transition_mutex->lock_async();
    if (source.source_run_id.empty() || source.source_journal_head.empty() ||
        source.command_coordinate_id.empty() || source.command_entry_id.empty()) {
        throw_runtime_diagnostic("P_REPLACEMENT_HANDOFF",
                                 "Program replacement requires an exact handoff reference");
    }
    if (target.ownership_scope() != owner_scope) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    const auto resolved_target = (co_await impl_->config.catalog->resolve_version_async(owner_scope, target.id()));
    if (!resolved_target) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto target_pinned =
        (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *resolved_target));
    if (!target_pinned->bundle.control_source()) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_TARGET",
            "Program replacement target must expose restricted JavaScript control");
    }
    if (!invocation.input.is_object() || !invocation.input.contains("handoff") ||
        !invocation.input.contains("previous_run_id") ||
        !invocation.input.at("previous_run_id").is_string() ||
        invocation.input.at("previous_run_id").get<std::string>() != source.source_run_id) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_INPUT",
            "Replacement input must bind handoff state and the exact predecessor run");
    }

    const auto run_id = invocation.requested_run_id.empty()
                            ? replacement_run_id(source, target.id(), invocation.input)
                            : invocation.requested_run_id;
    if (run_id == source.source_run_id) {
        throw_runtime_diagnostic("P_REPLACEMENT_SAME_RUN",
                                 "Replacement target run must differ from its source");
    }
    std::shared_ptr<detail::RunControl> replacement_control;
    const auto                          cancel_source_control = [&]() -> asio::awaitable<void> {
        auto live = impl_->find_process_control(owner_scope, source.source_run_id);
        if (live && replacement_control)
            co_await live->retire_to(replacement_control, impl_->process_state->transition_mutex);
        else
            co_await impl_->cancel_process_controls(owner_scope, source.source_run_id,
                                                    impl_->process_state->transition_mutex);
    };
    const auto reconnect_existing = [&]() -> asio::awaitable<std::optional<ProgramHandle>> {
        const auto existing = (co_await impl_->transitions->load(owner_scope, run_id));
        if (!existing) co_return std::nullopt;
        const auto lineage = (co_await impl_->transitions->load_run_lineage(owner_scope, run_id));
        const auto generation = lineage ? (co_await impl_->transitions->load_generation(
                                              owner_scope, lineage->lineage_id(),
                                              lineage->active_generation()))
                                        : std::nullopt;
        const auto receipt = generation ? generation->replacement_receipt() : std::nullopt;
        auto retry_projection = invocation;
        retry_projection.budget = existing->invocation().budget;
        retry_projection.parent_run_id = existing->invocation().parent_run_id;
        retry_projection.child_depth   = existing->child_depth();
        if (retry_projection.canonical_request) {
            retry_projection.canonical_request->budget = existing->invocation().budget;
            retry_projection.canonical_request->parent_run_id =
                existing->invocation().parent_run_id;
        }
        const auto retry_canonical =
            bind_runtime_invocation(retry_projection, owner_scope, target.id(), run_id);
        if (lineage && generation && receipt && generation->run_id() == run_id &&
            existing->program_version_id() == target.id() &&
            existing->bundle_id() == target.bundle_id() &&
            existing->invocation() == retry_canonical &&
            receipt->source_run_id() == source.source_run_id &&
            receipt->source_journal_head() == source.source_journal_head &&
            receipt->checkpoint_coordinate_id() == source.command_coordinate_id &&
            receipt->checkpoint_entry_id() == source.command_entry_id &&
            receipt->target_generation() == generation->generation() &&
            receipt->target_run_id() == run_id &&
            receipt->target_program_version_id() == target.id() &&
            receipt->target_bundle_id() == target.bundle_id() &&
            receipt->target_invocation_id() == existing->invocation().canonical_identity() &&
            receipt->target_binding_fingerprint() == existing->binding_fingerprint() &&
            receipt->target_initial_run_record_id() == generation->initial_run_record_id() &&
            receipt->target_initial_journal_head() == generation->initial_journal_head() &&
            receipt->matches_target_input(invocation.input)) {
            auto reconnected = (co_await reconnect_core(owner_scope, run_id, true));
            replacement_control = reconnected.control_;
            co_await cancel_source_control();
            co_return reconnected;
        }
        throw_runtime_diagnostic("P_RUN_CONFLICT",
                                 "Requested replacement run id is unavailable");
    };
    if (auto existing = co_await reconnect_existing()) co_return std::move(*existing);

    const auto source_record =
        (co_await impl_->transitions->load(owner_scope, source.source_run_id));
    if (!source_record || source_record->journal_head() != source.source_journal_head) {
        throw_runtime_diagnostic("P_REPLACEMENT_HANDOFF",
                                 "Program replacement handoff is not the active source head");
    }
    if (source_record->recorded_binding_set_fingerprint()) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_RECORDED_SOURCE",
            "Recorded replay evidence cannot authorize a live-bound replacement");
    }
    const auto source_lineage =
        (co_await load_active_run_lineage_async(*impl_->transitions, *source_record));
    if (!source_lineage || source_record->continuation().state != ContinuationState::Running ||
        source_record->pending_input() || source_record->pending_effect() ||
        source_record->terminal_result() ||
        source_lineage->lineage.inflight_reservation() != RunBudget{}) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_STATE",
            "Program replacement source is not at an admitted quiescent boundary");
    }
    if ((!invocation.parent_run_id.empty() &&
         invocation.parent_run_id != source_record->invocation().parent_run_id) ||
        (invocation.child_depth && invocation.child_depth != source_record->child_depth()))
        throw_runtime_diagnostic("P_REPLACEMENT_PARENT",
                                 "Replacement cannot change its parent or depth");
    invocation.parent_run_id = source_record->invocation().parent_run_id;
    invocation.child_depth   = source_record->child_depth();
    if (invocation.canonical_request)
        invocation.canonical_request->parent_run_id = invocation.parent_run_id;
    std::set<std::string> inherited_names;
    for (const auto& child : source_record->children()) {
        if (child.terminal_result && child.state == ProgramChildState::Dispatched)
            throw_runtime_diagnostic(
                "P_REPLACEMENT_CHILD",
                "An interrupted child requires disposition before replacement");
        const auto link = ModuleLinkReceipt::parse(child.link_receipt);
        if (!inherited_names.insert(link.child_name()).second)
            throw_runtime_diagnostic("P_REPLACEMENT_CHILD_BINDING",
                                     "Retained children require unique binding names");

        const auto policy = resolved_target->policy_snapshot();
        const auto allows = [](const auto& allowed, const auto& required) {
            return std::all_of(required.begin(), required.end(), [&](const auto& item) {
                return std::find(allowed.begin(), allowed.end(), item) != allowed.end();
            });
        };
        if (!allows(policy.allowed_capabilities(), link.granted_capabilities()) ||
            !allows(policy.allowed_effects(), link.granted_effects()) ||
            execution_guarantee_rank(link.minimum_execution_guarantee()) <
                execution_guarantee_rank(resolved_target->execution_guarantee()))
            throw_runtime_diagnostic("P_REPLACEMENT_CHILD_AUTHORITY",
                                     "Target cannot retain the admitted child authority");
    }
    if (!source_record->invocation().parent_run_id.empty() || !source_record->children().empty()) {
        if (const auto live = impl_->find_process_control(owner_scope, source.source_run_id)) {
            if (!live->has_held_handoff() ||
                impl_->find_control(owner_scope, source.source_run_id) != live)
                throw_runtime_diagnostic(
                    "P_REPLACEMENT_FAMILY_HANDOFF",
                    "Replace a live agent tree through its owning runtime and held checkpoint");
        }
    }
    if (const auto live = impl_->find_process_control(owner_scope, source.source_run_id);
        live && live->cancellation_cause() != detail::CancellationCause::None)
        throw_runtime_diagnostic("P_REPLACEMENT_CANCELLED",
                                 "Cancelled source cannot select another generation");
    const auto source_journal =
        (co_await impl_->transitions->latest(owner_scope, source.source_run_id));
    if (!source_journal || source_journal->id != source.source_journal_head ||
        source_journal->inflight_reservation != RunBudget{}) {
        throw_runtime_diagnostic("P_REPLACEMENT_HANDOFF",
                                 "Program replacement source journal is not quiescent");
    }
    const auto commands =
        (co_await impl_->transitions->load_javascript_commands(owner_scope, source.source_run_id));
    const auto source_runtime_state = (co_await impl_->transitions->load_effective_runtime_state(
        owner_scope, source.source_run_id));
    const auto& source_contexts = source_runtime_state.context_publications;
    std::vector<HookOutboxEntry> unresolved_source_hooks =
        source_runtime_state.inherited_hook_outbox_entries;
    for (const auto& hook : source_runtime_state.hook_outbox_entries) {
        const auto state = hook.data().state;
        if (state != HookExecutionState::Succeeded && state != HookExecutionState::Cancelled) {
            unresolved_source_hooks.push_back(hook);
        }
    }
    if ((!source_contexts.empty() || !unresolved_source_hooks.empty()) &&
        !impl_->config.runtime_recovery_handler) {
        throw_runtime_diagnostic(
            "P_RUNTIME_RECOVERY_UNAVAILABLE",
            "Program replacement cannot transfer runtime state without a recovery boundary");
    }
    if (commands.empty()) {
        throw_runtime_diagnostic("P_REPLACEMENT_HANDOFF",
                                 "Program replacement checkpoint was not found");
    }
    const auto& checkpoint = commands.back();
    const auto  terminal   = checkpoint.terminal_result();
    const auto  arguments  = checkpoint.command().arguments();
    if (!checkpoint.completed() ||
        checkpoint.command().kind() != JavaScriptCommandKind::Checkpoint ||
        checkpoint.coordinate_id() != source.command_coordinate_id ||
        checkpoint.id() != source.command_entry_id || !terminal || !terminal->is_object() ||
        !terminal->contains("status") || terminal->at("status") != "completed" ||
        !terminal->contains("output") || !arguments.is_object() || !arguments.contains("value") ||
        detail::canonical_json_bytes(terminal->at("output")) !=
            detail::canonical_json_bytes(arguments.at("value")) ||
        detail::canonical_json_bytes(invocation.input.at("handoff")) !=
            detail::canonical_json_bytes(arguments.at("value"))) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_HANDOFF",
            "Program replacement requires the exact latest completed ng.checkpoint");
    }

    const auto source_version = (co_await impl_->config.catalog->resolve_version_async(
        owner_scope, source_record->program_version_id()));
    if (!source_version) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto source_pinned =
        (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *source_version));
    if (!source_pinned->bundle.control_source() ||
        !replacement_authority_is_attenuated(*source_pinned, *target_pinned)) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_AUTHORITY",
            "Program replacement target authority is not an attenuation of the source");
    }

    if (!source_record->invocation().parent_run_id.empty() &&
        contract_fingerprint(source_pinned->bundle.output_contract()) !=
            contract_fingerprint(target_pinned->bundle.output_contract()))
        throw_runtime_diagnostic("P_REPLACEMENT_CHILD_OUTPUT",
                                 "Child replacement must preserve the parent's result contract");
    const auto requested_budget = source_lineage->lineage.remaining_budget();
    if (invocation.budget != requested_budget) {
        throw_runtime_diagnostic(
            "P_REPLACEMENT_BUDGET",
            "Program replacement must transfer the exact authoritative remaining budget");
    }
    const auto transition_time = std::max(now_ms(), source_record->updated_at_ms());
    const auto remaining = program_replacement_remaining_budget(
        *source_record, source_lineage->lineage, transition_time);
    invocation.budget = remaining;
    if (invocation.canonical_request) invocation.canonical_request->budget = remaining;
    validate_invocation(*target_pinned, invocation, true);
    const auto canonical = bind_runtime_invocation(invocation, owner_scope, target.id(), run_id);

    const auto target_thread_id =
        core_thread_identity(run_id, target_pinned->root->compiled_plan_identity);
    const auto binding_fingerprint = capability_binding_receipt_root(
        resolved_target->core_materialization_receipt().capability_bindings);
    ProgramPersistedInvocation persisted{canonical.input, canonical.budget,
                                         canonical.correlation_id, canonical.parent_run_id,
                                         invocation.child_depth};
    auto                       control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), run_id, 1, std::move(target_pinned), binding_fingerprint,
        std::move(persisted), canonical, target_thread_id, 0, std::move(invocation.events),
        impl_->scheduler, impl_->config.checkpoints, impl_->config.state_store,
        impl_->transitions, std::nullopt, source_record->logical_run_id());
    replacement_control = control;
    control->set_hook_runtime(impl_->config.hook_runtime);
    control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
    control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
    control->set_core_provider_call_resolver(
        impl_->config.core_provider_call_resolver,
        impl_->config.require_core_provider_call_broker);
    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{remaining});
    auto publication = initial_publication(
        *control, started, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        transition_time, std::nullopt, source_record->children());
    if (!source_contexts.empty()) {
        publication.context_publication =
            transfer_context_to_run(source_contexts.back(), run_id);
    }
    auto replacement_receipt = ProgramReplacementReceipt(ProgramReplacementReceiptData{
        std::string(owner_scope),
        source_lineage->lineage.lineage_id(),
        source_lineage->generation.generation(),
        source_lineage->generation.id(),
        source_lineage->lineage.id(),
        source_record->run_id(),
        source_record->id(),
        source_record->journal_head(),
        source_record->program_version_id(),
        source_record->bundle_id(),
        checkpoint.coordinate_id(),
        checkpoint.id(),
        program_replacement_handoff_identity(arguments.at("value")),
        source_lineage->generation.generation() + 1,
        run_id,
        target.id(),
        target.bundle_id(),
        program_replacement_input_identity(invocation.input),
        publication.run_record.invocation().canonical_identity(),
        publication.run_record.binding_fingerprint(),
        publication.run_record.id(),
        publication.run_record.journal_head()});
    std::optional<ProgramRuntimeStateTransferReceipt> transfer_receipt;
    if (!source_contexts.empty() || !unresolved_source_hooks.empty()) {
        std::vector<std::string> context_ids;
        context_ids.reserve(source_contexts.size());
        for (const auto& context : source_contexts) context_ids.push_back(context.epoch.id());
        std::vector<ProgramRuntimeStateTransferHookReference> hook_references;
        hook_references.reserve(unresolved_source_hooks.size());
        for (const auto& hook : unresolved_source_hooks) {
            hook_references.push_back({hook.data().invocation.id(), hook.id(),
                                       hook.data().event.id()});
        }
        transfer_receipt.emplace(ProgramRuntimeStateTransferReceiptData{
            std::string(owner_scope), source_lineage->lineage.lineage_id(),
            source_lineage->generation.id(), source_lineage->lineage.id(),
            source_record->run_id(), source_record->id(), source_record->journal_head(),
            source_lineage->generation.generation() + 1, run_id,
            publication.run_record.id(), publication.run_record.journal_head(),
            std::move(context_ids),
            publication.context_publication
                ? std::optional<std::string>(publication.context_publication->epoch.id())
                : std::nullopt,
            std::move(hook_references)});
    }
    auto successor                 = ProgramRunGeneration::create(ProgramRunGenerationData{
        std::string(owner_scope), source_lineage->lineage.lineage_id(),
        source_lineage->generation.generation() + 1, run_id, target.id(), target.bundle_id(),
        publication.run_record.id(), publication.journal_record.id, source_lineage->generation.id(),
        publication.run_record.created_at_ms(), invocation.child_depth,
        std::move(replacement_receipt), std::nullopt, transfer_receipt});
    publication.run_generation = successor;
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        std::string(owner_scope), source_lineage->lineage.lineage_id(),
        source_lineage->lineage.root_run_id(), successor.generation(), successor.id(),
        publication.run_record.id(), publication.journal_record.id, remaining, RunBudget{},
        source_lineage->lineage.id(), source_lineage->lineage.created_at_ms(),
        std::max(source_lineage->lineage.updated_at_ms(), publication.run_record.updated_at_ms()),
        source_lineage->lineage.committed_descendant_budget()});
    const auto transferred_context = publication.context_publication;
    const auto execution_lease =
        make_execution_lease(*control->materialized, publication);

    ProgramTransitionPublishResult published = ProgramTransitionPublishResult::Conflict;
    std::exception_ptr             publish_failure;
    try {
        if (execution_lease)
            published = (co_await control->transitions->compare_publish_execution(
                  owner_scope, "", std::move(publication), std::nullopt,
                  execution_lease));
        else
            published = (co_await control->transitions->compare_publish(
                  owner_scope, "", std::move(publication)));
    } catch (...) {
        publish_failure = std::current_exception();
    }
    if (publish_failure) {
        bool committed = false;
        try {
            const auto durable = (co_await control->transitions->load_generation(
                owner_scope, successor.lineage_id(), successor.generation()));
            committed = durable && durable->id() == successor.id();
            if (committed) co_await cancel_source_control();
        } catch (...) {}
        if (!committed || (!execution_lease && source_record->invocation().parent_run_id.empty() &&
                           source_record->children().empty()))
            std::rethrow_exception(publish_failure);
        published = ProgramTransitionPublishResult::AlreadyPresent;
    }
    if (published != ProgramTransitionPublishResult::Published &&
        published != ProgramTransitionPublishResult::AlreadyPresent) {
        if (auto existing = co_await reconnect_existing()) co_return std::move(*existing);
        throw_runtime_diagnostic("P_REPLACEMENT_CONFLICT",
                                 "Program lineage already selected another successor");
    }
    if (!source_record->invocation().parent_run_id.empty() &&
        !impl_->find_process_control(owner_scope, source.source_run_id)) {
        const auto budgets = (co_await impl_->hydrate_parent_child_budget(
            owner_scope, invocation.parent_run_id, control->logical_run_id));
        auto quota = impl_->reserve_active_child(owner_scope);
        if (!impl_->activate_child_concurrency(invocation.parent_run_id, control->logical_run_id,
                                               budgets.parent, budgets.child))
            throw_runtime_diagnostic("P_CHILD_CONCURRENCY",
                                     "Replacement child exceeds parent concurrency");
        impl_->commit_active_child(control, quota);
        impl_->bind_budgeted_child_completion(control, owner_scope, invocation.parent_run_id,
                                              control->logical_run_id);
    }
    const bool family =
        !source_record->invocation().parent_run_id.empty() || !source_record->children().empty();
    std::optional<Impl::ControlRegistration> family_registration;
    if (family) {
        if (execution_lease) control->set_execution_lease(*execution_lease);
        family_registration = (co_await impl_->register_control(control, true));
        replacement_control = family_registration->control;
    }
    co_await cancel_source_control();
    reconnect_lock.unlock();
    transition_lock.unlock();
    std::exception_ptr         recovery_failure;
    std::optional<std::string> recovery_error;
    try {
        if (transfer_receipt) {
            ProgramRuntimeRecoveryState recovery{
                std::string(owner_scope),
                run_id,
                transferred_context ? std::vector<ProgramContextPublication>{*transferred_context}
                                    : std::vector<ProgramContextPublication>{},
                {},
                unresolved_source_hooks,
                transfer_receipt};
            impl_->config.runtime_recovery_handler(recovery);
        }
    } catch (const std::exception& error) {
        if (!family) throw;
        recovery_error = error.what();
    }
    if (recovery_error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = remaining;
        failed.failure          = ProgramFailure{
            "P_RUNTIME_RECOVERY_UNAVAILABLE", *recovery_error, "root", "", 0, json::object()};
        (co_await replacement_control->complete_async(std::move(failed)));
        co_return ProgramHandle(replacement_control);
    }
    if (execution_lease) control->set_execution_lease(*execution_lease);
    auto registration =
        family_registration ? *family_registration : (co_await impl_->register_control(control, true));
    if (!registration.inserted) co_return ProgramHandle(std::move(registration.control));
    if (!(co_await admit_started(control, started, remaining))) {
        co_return ProgramHandle(std::move(control));
    }
    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        detail::RunOutcome failed;
        failed.status           = ProgramTerminalStatus::Failed;
        failed.remaining_budget = remaining;
        failed.failure =
            ProgramFailure{"P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }
    spawn_run_attempt(impl_->scheduler, impl_, control, invocation.input, std::nullopt,
                      impl_->config.host_admission, impl_->config.host_admission_resolver);
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_core(std::string        owner_scope,
                                                            ProgramHandoff*    source_lease,
                                                            ProgramVersion     target,
                                                            ProgramInvocation  invocation) {
    if (!source_lease) throw std::invalid_argument("Program replacement requires an active handoff lease");
    ProgramHandoff& source = *source_lease;
    if (!source || !source.impl_ || !source.impl_->control) {
        throw std::invalid_argument("Program replacement requires an active handoff lease");
    }
    if (!same_process_coordination_backend(*source.impl_->control->transitions,
                                           *impl_->transitions)) {
        throw std::invalid_argument(
            "Program handoff lease and replacement runtime use different transition stores");
    }
    if (source.impl_->control->owner_scope != owner_scope ||
        source.impl_->control->run_id != source.reference().source_run_id) {
        throw std::invalid_argument("Program handoff lease does not bind the replacement source");
    }
    const auto source_record_id = (co_await source.impl_->control->snapshot_async()).id();
    std::exception_ptr failure;
    try {
        auto result = (co_await replace_core(owner_scope, source.reference(), target, std::move(invocation)));
        source.consume();
        if (!source.impl_->control->following_successor())
            (void)co_await source.impl_->control->cancel_async(detail::CancellationCause::ParentTerminal);
        co_return result;
    } catch (...) {
        failure = std::current_exception();
    }
    bool definitely_uncommitted = false;
    try {
        const auto lineage = (co_await impl_->transitions->load_run_lineage(
            source.impl_->control->owner_scope, source.impl_->control->run_id));
        definitely_uncommitted =
            lineage && lineage->active_run_record_id() == source_record_id;
    } catch (...) {}
    if (!definitely_uncommitted) {
        source.consume();
        if (!source.impl_->control->following_successor())
            (void)co_await source.impl_->control->cancel_async(detail::CancellationCause::ParentTerminal);
    }
    std::rethrow_exception(failure);
}

ProgramHandle ProgramRuntime::migrate_graph(
    const ProgramHandle& source,
    ProgramGraphMigrationTarget target) {
    impl_->scheduler->require_synchronous_call("ProgramRuntime::migrate_graph", "ProgramRuntime::migrate_graph_async");
    // The call waits for the source to reach a safe point, which only the executor can bring about: made on a thread
    // that runs it, it would wait for ever.
    impl_->scheduler->require_wait("ProgramRuntime::migrate_graph", "ProgramRuntime::migrate_graph_async");
    return async::run_sync(migrate_graph_core(source, std::move(target)));
}

// The body of migrate_graph. It waits for the source to reach a safe point without blocking a thread, so the
// synchronous form drives it on a private io_context and the awaitable form on the runtime's executor.
asio::awaitable<ProgramHandle> ProgramRuntime::migrate_graph_core(
    ProgramHandle source,
    ProgramGraphMigrationTarget target) {
    if (!source.control_ || !source.control_->materialized) {
        throw std::invalid_argument(
            "Program graph migration requires a live source handle");
    }
    if (target.target_program_version_id.empty()) {
        throw std::invalid_argument(
            "Program graph migration requires a target ProgramVersion id");
    }
    if (!same_process_coordination_backend(*source.control_->transitions,
                                           *impl_->transitions)) {
        throw std::invalid_argument(
            "Program graph migration source and target use different transition stores");
    }

    auto fork_lock = co_await impl_->fork_operation.lock_async();
    const auto owner_scope = source.control_->owner_scope;
    const auto source_record_before = (co_await impl_->transitions->load(
        owner_scope, source.control_->run_id));
    if (!source_record_before ||
        source_record_before->recorded_binding_set_fingerprint()) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_SOURCE",
            "Program graph migration source is absent or replay-bound");
    }
    const auto source_version = (co_await impl_->config.catalog->resolve_version_async(
        owner_scope, source_record_before->program_version_id()));
    const auto target_version = (co_await impl_->config.catalog->resolve_version_async(
        owner_scope, target.target_program_version_id));
    if (!source_version || !target_version) {
        throw_runtime_diagnostic(
            "P_VERSION_NOT_FOUND",
            "Program graph migration requires exact owner-scoped admitted versions");
    }
    auto source_pinned = (co_await detail::CatalogRuntimeAccess::pin_async(
        *impl_->config.catalog, *source_version));
    auto target_pinned = (co_await detail::CatalogRuntimeAccess::pin_async(
        *impl_->config.catalog, *target_version));
    const auto migration_plan = target.semantic_adapter
        ? target.semantic_adapter->migration_plan(*source_version, source_pinned->bundle,
                                                  *target_version, target_pinned->bundle)
        : (co_await impl_->config.catalog->plan_migration_async(
              owner_scope, source_version->id(), target_version->id()));
    if (!supports_live_graph_migration(*source_pinned) ||
        !supports_live_graph_migration(*target_pinned) ||
        (!target.semantic_adapter &&
         (source_pinned->root->core_name != target_pinned->root->core_name ||
          source_pinned->root->compiled_plan_identity !=
              target_pinned->root->compiled_plan_identity)) ||
        !migration_plan.is_compatible()) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_INCOMPATIBLE",
            "Program graph migration target is not an admitted compatible root Core");
    }
    if (!target.requested_run_id.empty() &&
        target.requested_run_id == source.control_->run_id) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_SAME_RUN",
            "Program graph migration target run must differ from its source");
    }

    if (const auto lineage = (co_await impl_->transitions->load_run_lineage(
            owner_scope, source.control_->run_id));
        lineage && lineage->active_run_record_id() != source_record_before->id()) {
        const auto generation = (co_await impl_->transitions->load_generation(
            owner_scope, lineage->lineage_id(), lineage->active_generation()));
        const auto receipt = generation
            ? generation->graph_migration_receipt()
            : std::nullopt;
        const auto expected_target_run_id = receipt
            ? (target.requested_run_id.empty()
                   ? graph_migration_run_id(receipt->capsule(), target_version->id())
                   : target.requested_run_id)
            : std::string{};
        if (generation && receipt && generation->id() == lineage->active_generation_id() &&
            receipt->capsule().source_run_id() == source.control_->run_id &&
            receipt->source_version().id() == source_version->id() &&
            receipt->target_version().id() == target_version->id() &&
            generation->run_id() == receipt->target_run_id() &&
            receipt->target_run_id() == expected_target_run_id) {
            // Only the fork lock is held here; the reconnect takes the transition lock itself.
            co_return (co_await reconnect_core(owner_scope, source.control_->run_id));
        }
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_STALE",
            "Program graph migration source already selected another successor");
    }

    const auto request_id = co_await source.control_->request_graph_migration_async();
    struct SourceReleaseGuard {
        std::shared_ptr<detail::RunControl> control;
        std::uint64_t request_id = 0;
        bool consumed = false;
        ~SourceReleaseGuard() {
            if (!consumed && control) control->release_graph_migration(request_id);
        }
    } source_guard{source.control_, request_id};
    const auto capsule = co_await source.control_->wait_graph_migration_async(request_id);

    auto reconnect_lock = co_await impl_->process_state->transition_mutex->lock_async();
    if (!source.control_->graph_migration_matches(request_id, capsule.id())) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_SOURCE",
            "Program graph migration source hold was cancelled before publication");
    }
    const auto source_record = (co_await impl_->transitions->load(
        owner_scope, capsule.source_run_id()));
    const auto active = source_record
        ? (co_await load_active_run_lineage_async(*impl_->transitions, *source_record))
        : std::nullopt;
    if (!source_record || !active ||
        source_record->id() != active->lineage.active_run_record_id() ||
        active->generation.id() != capsule.source_generation_id() ||
        active->lineage.id() != capsule.source_lineage_head_id() ||
        source_record->exact_checkpoint() !=
            std::optional{capsule.core_checkpoint()} ||
        source_record->exact_checkpoint_content_id() !=
            std::optional{graph_migration_checkpoint_content_id(
                capsule.checkpoint())}) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_STALE",
            "Program graph migration capsule is no longer the active source head");
    }
    const auto source_runtime_state = (co_await impl_->transitions->load_effective_runtime_state(
        owner_scope, source_record->run_id()));
    const auto& source_contexts = source_runtime_state.context_publications;
    std::vector<HookOutboxEntry> unresolved_source_hooks =
        source_runtime_state.inherited_hook_outbox_entries;
    for (const auto& hook : source_runtime_state.hook_outbox_entries) {
        const auto state = hook.data().state;
        if (state != HookExecutionState::Succeeded && state != HookExecutionState::Cancelled) {
            unresolved_source_hooks.push_back(hook);
        }
    }
    if ((!source_contexts.empty() || !unresolved_source_hooks.empty()) &&
        !impl_->config.runtime_recovery_handler) {
        throw_runtime_diagnostic(
            "P_RUNTIME_RECOVERY_UNAVAILABLE",
            "Program graph migration cannot transfer runtime state without a recovery boundary");
    }

    const auto target_run_id = target.requested_run_id.empty()
        ? graph_migration_run_id(capsule, target_version->id())
        : target.requested_run_id;
    if (target_run_id == source_record->run_id()) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_SAME_RUN",
            "Program graph migration target run must differ from its source");
    }
    if ((co_await impl_->transitions->load(owner_scope, target_run_id))) {
        throw_runtime_diagnostic(
            "P_RUN_CONFLICT",
            "Requested Program graph migration target run id is unavailable");
    }
    if (capsule.checkpoint().timestamp ==
        std::numeric_limits<std::int64_t>::max()) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_CHECKPOINT",
            "Program graph migration checkpoint timestamp is exhausted");
    }

    const auto snapshot_time = std::max(
        {now_ms(), source_record->updated_at_ms(),
         capsule.checkpoint().timestamp + 1});
    if (program_replacement_remaining_budget(
            *source_record, active->lineage, snapshot_time).wall_time_ms == 0) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_EXPIRED",
            "Program graph migration exhausted its wall-time budget before target preparation");
    }
    auto target_snapshot = target.semantic_adapter
        ? target.semantic_adapter->project(capsule)
        : capsule.checkpoint();
    target_snapshot.id = graph::Checkpoint::generate_id();
    target_snapshot.thread_id = program_root_core_thread_id(
        target_run_id, target_pinned->root->compiled_plan_identity);
    target_snapshot.parent_id = capsule.core_checkpoint().checkpoint_id;
    target_snapshot.metadata["migrated_from"] = capsule.id();
    if (target.semantic_adapter) {
        target_snapshot.metadata["semantic_migration_adapter_id"] =
            target.semantic_adapter->id();
    }
    target_snapshot.timestamp = snapshot_time;
    impl_->config.checkpoints->save(target_snapshot);
    const auto durable_target_checkpoint =
        impl_->config.checkpoints->load_by_id(target_snapshot.id);
    const auto target_checkpoint_content_id =
        graph_migration_checkpoint_content_id(target_snapshot);
    if (!durable_target_checkpoint ||
        graph_migration_checkpoint_content_id(*durable_target_checkpoint) !=
            target_checkpoint_content_id) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_CHECKPOINT",
            "Program graph migration target checkpoint did not survive exact read-back");
    }
    const auto transition_time = std::max(
        {now_ms(), source_record->updated_at_ms(), target_snapshot.timestamp});
    const auto remaining = program_replacement_remaining_budget(
        *source_record, active->lineage, transition_time);
    if (remaining.wall_time_ms == 0) {
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_EXPIRED",
            "Program graph migration exhausted its wall-time budget during target preparation");
    }
    auto target_invocation = source_record->invocation();
    target_invocation.program_version_id = target_version->id();
    target_invocation.selected_activation.reset();
    target_invocation.run_id = target_run_id;
    target_invocation.budget = remaining;
    target_invocation.validate();
    const CoreCheckpointIdentity target_checkpoint{
        target_pinned->root->core_name,
        target_pinned->root->compiled_plan_identity,
        target_snapshot.thread_id,
        target_snapshot.id,
        target_snapshot.schema_version};

    ProgramPersistedInvocation persisted{
        target_invocation.input, remaining, target_invocation.correlation_id,
        target_invocation.parent_run_id, 0};
    const auto binding_fingerprint = capability_binding_receipt_root(
        target_version->core_materialization_receipt().capability_bindings);
    auto control = std::make_shared<detail::RunControl>(
        owner_scope, target_run_id, source_record->continuation().attempt, std::move(target_pinned),
        binding_fingerprint, std::move(persisted), target_invocation, target_snapshot.thread_id, 0,
        std::move(target.events), impl_->scheduler, impl_->config.checkpoints,
        impl_->config.state_store, impl_->transitions, std::nullopt,
        source_record->logical_run_id());
    control->set_hook_runtime(impl_->config.hook_runtime);
    control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
    control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
    control->set_core_provider_call_resolver(
        impl_->config.core_provider_call_resolver,
        impl_->config.require_core_provider_call_broker);
    auto started = control->preview_event(
        1, ProgramEventKind::Started, ProgramStartedEvent{remaining});
    started.id.clear();
    started.timestamp_ms = transition_time;
    started = ProgramEvent::create(std::move(started));
    control->adopt_published_event(started);
    // #386: the same replay records as any continuation of the source.
    auto migrated_records = continued_replay_records(*source_record, target_run_id,
                                                     *control->materialized,
                                                     *impl_->config.checkpoints,
                                                     impl_->config.native_history_archive);
    auto publication = initial_publication(
        *control, started, target_checkpoint, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, transition_time,
        target_checkpoint_content_id, {}, std::nullopt, std::move(migrated_records));
    if (!source_contexts.empty()) {
        publication.context_publication =
            transfer_context_to_run(source_contexts.back(), target_run_id);
    }
    auto receipt = ProgramGraphMigrationReceipt(
        ProgramGraphMigrationReceiptData{
            capsule, source_pinned->bundle, source_pinned->version,
            control->materialized->bundle, control->materialized->version,
            migration_plan.id(), active->generation.generation() + 1,
            target_run_id, target_version->id(), target_version->bundle_id(),
            publication.run_record.invocation().canonical_identity(),
             publication.run_record.binding_fingerprint(),
             publication.run_record.id(), publication.run_record.journal_head(),
             target_checkpoint, target_snapshot, target.semantic_adapter});
    std::optional<ProgramRuntimeStateTransferReceipt> transfer_receipt;
    if (!source_contexts.empty() || !unresolved_source_hooks.empty()) {
        std::vector<std::string> context_ids;
        context_ids.reserve(source_contexts.size());
        for (const auto& context : source_contexts) context_ids.push_back(context.epoch.id());
        std::vector<ProgramRuntimeStateTransferHookReference> hook_references;
        hook_references.reserve(unresolved_source_hooks.size());
        for (const auto& hook : unresolved_source_hooks) {
            hook_references.push_back({hook.data().invocation.id(), hook.id(),
                                       hook.data().event.id()});
        }
        transfer_receipt.emplace(ProgramRuntimeStateTransferReceiptData{
            owner_scope, active->lineage.lineage_id(), active->generation.id(),
            active->lineage.id(), source_record->run_id(), source_record->id(),
            source_record->journal_head(), active->generation.generation() + 1,
            target_run_id, publication.run_record.id(), publication.run_record.journal_head(),
            std::move(context_ids),
            publication.context_publication
                ? std::optional<std::string>(publication.context_publication->epoch.id())
                : std::nullopt,
            std::move(hook_references)});
    }
    auto successor = ProgramRunGeneration::create(ProgramRunGenerationData{
        owner_scope, active->lineage.lineage_id(),
        active->generation.generation() + 1, target_run_id,
        target_version->id(), target_version->bundle_id(),
        publication.run_record.id(), publication.run_record.journal_head(),
        active->generation.id(), publication.run_record.created_at_ms(), 0,
        std::nullopt, receipt, transfer_receipt});
    publication.run_generation = successor;
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        owner_scope, active->lineage.lineage_id(), active->lineage.root_run_id(),
        successor.generation(), successor.id(), publication.run_record.id(),
        publication.run_record.journal_head(), remaining, RunBudget{},
        active->lineage.id(), active->lineage.created_at_ms(),
        publication.run_record.updated_at_ms(), RunBudget{}});
    publication.migration_plan = migration_plan;
    const auto transferred_context = publication.context_publication;
    const auto target_execution_lease =
        make_execution_lease(*control->materialized, publication);
    if (!target_execution_lease) {
        throw_runtime_diagnostic(
            "P_RUN_LEASE", "Program graph migration target cannot acquire execution ownership");
    }

    const auto fence_source = [&]() -> asio::awaitable<void> {
        source_guard.consumed = true;
        co_await impl_->cancel_process_controls(owner_scope, capsule.source_run_id(),
                                                impl_->process_state->transition_mutex);
    };
    ProgramTransitionPublishResult published = ProgramTransitionPublishResult::Conflict;
    std::exception_ptr             publish_failure;
    try {
        published = (co_await impl_->transitions->compare_publish_execution(
            owner_scope, "", std::move(publication), std::nullopt,
            target_execution_lease));
    } catch (...) {
        publish_failure = std::current_exception();
    }
    if (publish_failure) {
        bool committed = false;
        bool definitely_uncommitted = false;
        try {
            const auto durable = (co_await impl_->transitions->load_generation(
                owner_scope, successor.lineage_id(), successor.generation()));
            committed = durable && durable->id() == successor.id();
            const auto lineage = (co_await impl_->transitions->load_lineage(
                owner_scope, successor.lineage_id()));
            definitely_uncommitted = lineage &&
                lineage->id() == capsule.source_lineage_head_id();
        } catch (...) {}
        if (committed || !definitely_uncommitted) co_await fence_source();
        if (!committed) std::rethrow_exception(publish_failure);
        published = ProgramTransitionPublishResult::AlreadyPresent;
    }
    if (published != ProgramTransitionPublishResult::Published &&
        published != ProgramTransitionPublishResult::AlreadyPresent) {
        bool definitely_uncommitted = false;
        try {
            const auto lineage = (co_await impl_->transitions->load_lineage(
                owner_scope, successor.lineage_id()));
            definitely_uncommitted = lineage &&
                lineage->id() == capsule.source_lineage_head_id();
        } catch (...) {}
        if (!definitely_uncommitted) co_await fence_source();
        throw_runtime_diagnostic(
            "P_GRAPH_MIGRATION_CONFLICT",
            "Program lineage already selected another graph migration successor");
    }

    co_await fence_source();
    reconnect_lock.unlock();
    fork_lock.unlock();
    if (transfer_receipt) {
        ProgramRuntimeRecoveryState recovery{
            owner_scope, target_run_id,
            transferred_context
                ? std::vector<ProgramContextPublication>{*transferred_context}
                : std::vector<ProgramContextPublication>{},
            {}, unresolved_source_hooks, transfer_receipt};
        impl_->config.runtime_recovery_handler(recovery);
    }
    control->set_execution_lease(*target_execution_lease);
    auto registration = (co_await impl_->register_control(control, true));
    if (!registration.inserted) {
        co_return ProgramHandle(std::move(registration.control));
    }
    const auto admitted = (co_await admit_started(control, started, remaining, target_checkpoint));
    if (!admitted) co_return ProgramHandle(std::move(control));
    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        detail::RunOutcome failed;
        failed.status = ProgramTerminalStatus::Failed;
        failed.remaining_budget = remaining;
        failed.checkpoint = target_checkpoint;
        failed.failure = ProgramFailure{
            "P_EVENT_SINK", *sink_error, "root", "", 0, json::object()};
        (co_await control->complete_async(std::move(failed)));
        co_return ProgramHandle(std::move(control));
    }
    spawn_run_attempt(impl_->scheduler, impl_, control, target_invocation.input,
                      target_checkpoint.checkpoint_id,
                      impl_->config.host_admission,
                      impl_->config.host_admission_resolver);
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<std::vector<ProgramHandle>> ProgramRuntime::recover_children_core(
    std::string owner_scope, std::string parent_run_id) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (parent_run_id.empty())
        throw std::invalid_argument("Program child recovery parent_run_id must not be empty");

    const auto parent_record =
        (co_await load_active_agent_run(impl_->transitions, owner_scope, parent_run_id));
    if (!parent_record)
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program parent run was not found");
    auto parent = (co_await reconnect_core(owner_scope, parent_run_id));

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
            throw_runtime_diagnostic("P_CHILD_CONFLICT",
                                     "Durable child receipt does not preserve parent lineage",
                                     json{{"child_run_id", persisted.child_run_id}});
        }

        if (persisted.state == ProgramChildState::Completed ||
            persisted.state == ProgramChildState::Cancelled ||
            persisted.state == ProgramChildState::Failed) {
            auto child = (co_await reconnect_core(owner_scope, persisted.child_run_id));
            co_await parent.control_->attach_child(child.control_);
            if (const auto result = child.try_result())
                (co_await publish_child_completion(impl_->transitions, owner_scope, parent_run_id,
                                         persisted.child_run_id, *result));
            recovered.push_back(std::move(child));
            continue;
        }

        const auto version =
            (co_await impl_->config.catalog->resolve_version_async(owner_scope, link.child_program_version_id()));
        if (!version)
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND",
                                     "Program version required for child recovery was not found",
                                     json{{"child_run_id", persisted.child_run_id}});
        ProgramInvocation invocation{
            persisted.invocation.input,      persisted.invocation.granted_budget,
            persisted.invocation.trace_id,   {},
            persisted.child_run_id,          persisted.invocation.parent_run_id,
            persisted.invocation.child_depth};
        auto child = (co_await start_child_core(owner_scope, parent, link, *version, std::move(invocation)));
        recovered.push_back(std::move(child));
    }
    co_return recovered;
}
asio::awaitable<ProgramSynthesisReservation> ProgramRuntime::reserve_synthesis_core(
    std::string                owner_scope,
    ProgramHandle              source,
    ProgramHandoff*            handoff_ptr,
    ProgramSynthesisProposal   proposal,
    std::string                expected_lineage_head) {
    ProgramHandoff& handoff = *handoff_ptr;
    if (!source.control_ || !handoff.impl_ || handoff.impl_->consumed.load() ||
        handoff.impl_->control != source.control_ || source.control_->owner_scope != owner_scope ||
        !same_process_coordination_backend(*source.control_->transitions,
                                           *impl_->transitions))
        throw_runtime_diagnostic("P_SYNTHESIS_GENERATION",
                                 "Synthesis requires this runtime's held checkpoint");
    (void)handoff.value();
    if (impl_->find_control(owner_scope, source.run_id()) != source.control_)
        throw_runtime_diagnostic("P_SYNTHESIS_GENERATION", "Synthesis requires the owning runtime");
    auto lock = co_await child_relation_publication_mutex(*impl_->transitions, owner_scope,
                                                          source.logical_run_id())
                    .lock_async();
    const auto      run     = (co_await source.snapshot_async());
    const auto      journal = (co_await impl_->transitions->latest(owner_scope, source.run_id()));
    const auto      active  = (co_await load_active_run_lineage_async(*impl_->transitions, run));
    if (!journal || journal->id != run.journal_head() || !active ||
        active->lineage.id() != expected_lineage_head ||
        proposal.data().owner_scope != owner_scope ||
        proposal.data().parent_run_id != source.run_id() ||
        proposal.data().lineage_id != active->lineage.lineage_id() ||
        source.control_->cancellation_cause() != detail::CancellationCause::None)
        throw_runtime_diagnostic("P_SYNTHESIS_GENERATION", "Synthesis source head is not active");
    auto remaining = run.remaining_budget();
    if (remaining.max_dynamic_compiles <=
        active->lineage.committed_descendant_budget().max_dynamic_compiles)
        throw_runtime_diagnostic("P_SYNTHESIS_BUDGET", "No unallocated compilation budget remains");
    --remaining.max_dynamic_compiles;
    const auto timestamp = std::max(now_ms(), run.updated_at_ms());
    remaining.wall_time_ms -= std::min(remaining.wall_time_ms,
                                       static_cast<std::uint64_t>(timestamp - run.updated_at_ms()));
    if (!remaining.wall_time_ms)
        throw_runtime_diagnostic("P_SYNTHESIS_BUDGET", "Synthesis deadline has expired");
    auto publication =
        (co_await attach_run_lineage_async(*impl_->transitions,
                           continuing_running_publication(run, *journal, remaining, timestamp,
                                                          journal->inflight_reservation)));
    auto reservation = ProgramSynthesisReservation::create(
        {proposal.id(), proposal.data().lineage_id, active->lineage.id(),
         publication.run_lineage->id(), run.remaining_budget(), remaining});
    const auto reserved_journal_head = publication.run_record.journal_head();
    const auto lease =
        (co_await impl_->transitions->load_execution_lease(owner_scope, source.run_id()));
    ProgramTransitionPublishResult published;
    try {
        if (lease)
            published = (co_await impl_->transitions->compare_publish_execution(
                              owner_scope, run.journal_head(), std::move(publication), lease, lease));
        else
            published = (co_await impl_->transitions->compare_publish(
                              owner_scope, run.journal_head(), std::move(publication)));
    } catch (...) {
        // A lost acknowledgement cannot restore compile authority in this live attempt.
        source.control_->limit_dynamic_compiles(remaining.max_dynamic_compiles);
        throw;
    }
    if (published == ProgramTransitionPublishResult::Conflict)
        throw_runtime_diagnostic("P_SYNTHESIS_GENERATION", "Synthesis reservation lost its CAS");
    source.control_->limit_dynamic_compiles(remaining.max_dynamic_compiles);
    {
        std::lock_guard resolve_lock(handoff.impl_->resolve_mutex);
        handoff.impl_->handoff->reference.source_journal_head = reserved_journal_head;
    }
    co_return reservation;
}

asio::awaitable<ProgramChildSynthesisRecord> ProgramRuntime::prepare_child_synthesis_core(
    std::string                  owner_scope,
    ProgramHandle                parent,
    const ProgramHandoff*        handoff_ptr,
    ProgramSynthesisProposal     proposal,
    ProgramChildSynthesisGrant   grant,
    std::string                  binding_name) {
    const ProgramHandoff& handoff = *handoff_ptr;
    if (!handoff.impl_ || handoff.impl_->consumed.load() ||
        handoff.impl_->control != parent.control_ || parent.control_->owner_scope != owner_scope ||
        !same_process_coordination_backend(*parent.control_->transitions,
                                           *impl_->transitions))
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_GENERATION",
                                 "Child synthesis requires this runtime's held parent checkpoint");
    (void)handoff.value();
    auto synthesis_lock =
        co_await child_relation_publication_mutex(*impl_->transitions, owner_scope, parent.run_id())
            .lock_async();
    if (!impl_->config.child_synthesis_gateway || !impl_->config.child_synthesis_grant_resolver)
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_AUTHORITY",
                                 "Child synthesis host policy is unavailable");
    for (const auto& existing :
         (co_await impl_->transitions->load_child_syntheses(owner_scope, parent.run_id()))) {
        if (existing.data().proposal.id() != proposal.id()) continue;
        if (existing.data().grant.id() != grant.id() ||
            existing.data().binding_name != binding_name)
            throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CONFLICT",
                                     "Synthesis retry changed its grant or binding");
        (co_await impl_->verify_synthesis_grant(existing));
        parent.control_->limit_dynamic_compiles(
            existing.data().reservation.data().remaining_after_reservation.max_dynamic_compiles);
        co_return co_await impl_->config.child_synthesis_gateway->continue_child_async(
            existing, [&](const ProgramChildSynthesisRecord& next) -> asio::awaitable<void> {
                co_await impl_->publish_synthesis(next, parent.control_);
            });
    }
    for (const auto& child : (co_await parent.snapshot_async()).children()) {
        if (ModuleLinkReceipt::parse(child.link_receipt).child_name() == binding_name)
            throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CONFLICT",
                                     "Synthesis name already belongs to an attached child");
    }
    if (impl_->config.child_binding_resolver &&
        impl_->config.child_binding_resolver(owner_scope, parent.program_version_id(),
                                             binding_name))
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CONFLICT",
                                 "Synthesis binding collides with a static child");
    auto run     = (co_await parent.snapshot_async());
    auto journal = (co_await impl_->transitions->latest(owner_scope, parent.run_id()));
    auto active  = (co_await load_active_run_lineage_async(*impl_->transitions, run));
    auto version = (co_await impl_->config.catalog->resolve_version_async(owner_scope, parent.program_version_id()));
    if (!journal || journal->id != run.journal_head() || !active || !version ||
        parent.control_->cancellation_cause() != detail::CancellationCause::None)
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_GENERATION",
                                 "Parent synthesis checkpoint is no longer active");
    ProgramChildSynthesisParent context{*version, run, active->lineage, active->generation};
    auto authorization = authorize_program_child_synthesis(proposal, grant, context);
    auto remaining     = run.remaining_budget();
    --remaining.max_dynamic_compiles;
    const auto timestamp = std::max(now_ms(), run.updated_at_ms());
    const auto elapsed   = static_cast<std::uint64_t>(timestamp - run.updated_at_ms());
    remaining.wall_time_ms -= std::min(remaining.wall_time_ms, elapsed);
    auto publication =
        (co_await attach_run_lineage_async(*impl_->transitions,
                           continuing_running_publication(run, *journal, remaining, timestamp,
                                                          journal->inflight_reservation)));
    auto reservation = ProgramSynthesisReservation::create(
        {proposal.id(), proposal.data().lineage_id, active->lineage.id(),
         publication.run_lineage->id(), run.remaining_budget(), remaining});
    validate_program_child_synthesis_reservation(authorization, reservation);
    auto record = ProgramChildSynthesisRecord::create(
        {proposal, grant, std::move(context), reservation, std::move(binding_name)});
    (co_await impl_->verify_synthesis_grant(record, false));
    publication.child_synthesis_records.push_back(record);
    const auto lease =
        (co_await impl_->transitions->load_execution_lease(owner_scope, parent.run_id()));
    ProgramTransitionPublishResult result = ProgramTransitionPublishResult::Conflict;
    std::exception_ptr             publish_failure;
    try {
        if (lease)
            result = (co_await impl_->transitions->compare_publish_execution(
                             owner_scope, run.journal_head(), std::move(publication), lease, lease));
        else
            result = (co_await impl_->transitions->compare_publish(owner_scope, run.journal_head(),
                                                                    std::move(publication)));
    } catch (...) {
        publish_failure = std::current_exception();
    }
    if (publish_failure) {
        const auto failure = publish_failure;
        // A backend may lose the acknowledgement after committing. Read before returning control.
        try {
            for (const auto& stored :
                 (co_await impl_->transitions->load_child_syntheses(owner_scope, parent.run_id())))
                if (stored.data().proposal.id() == proposal.id())
                    parent.control_->limit_dynamic_compiles(
                        stored.data()
                            .reservation.data()
                            .remaining_after_reservation.max_dynamic_compiles);
        } catch (...) {
            // Ambiguous persistence cannot authorize another compile in this live attempt.
            parent.control_->limit_dynamic_compiles(remaining.max_dynamic_compiles);
        }
        std::rethrow_exception(failure);
    }
    if (result == ProgramTransitionPublishResult::Conflict)
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_CONFLICT",
                                 "Synthesis reservation lost its parent compare-and-swap");
    parent.control_->limit_dynamic_compiles(remaining.max_dynamic_compiles);
    co_return co_await impl_->config.child_synthesis_gateway->continue_child_async(
            record, [&](const ProgramChildSynthesisRecord& next) -> asio::awaitable<void> {
                co_await impl_->publish_synthesis(next, parent.control_);
            });
}

asio::awaitable<ProgramChildSynthesisRecord> ProgramRuntime::recover_child_synthesis_core(
    std::string owner_scope, std::string parent_run_id, std::string proposal_id) {
    // Live recovery requires the explicit held-checkpoint API above.
    if (impl_->find_process_control(owner_scope, parent_run_id) ||
        impl_->find_control(owner_scope, parent_run_id))
        throw_runtime_diagnostic("P_CHILD_SYNTHESIS_GENERATION",
                                 "Live synthesis recovery requires the held parent checkpoint");
    for (const auto& record :
         (co_await impl_->transitions->load_child_syntheses(owner_scope, parent_run_id))) {
        if (record.data().proposal.id() != proposal_id) continue;
        (co_await impl_->verify_synthesis_grant(record));
        if ((co_await impl_->synthesis_child_is_uncertain(record))) {
            auto data        = record.data();
            data.previous_id = record.id();
            ++data.revision;
            data.state      = ProgramChildSynthesisState::ReconciliationRequired;
            data.error_code = "P_CHILD_SYNTHESIS_CHILD_UNCERTAIN";
            auto uncertain  = ProgramChildSynthesisRecord::create(std::move(data));
            (co_await impl_->publish_synthesis(uncertain));
            co_return uncertain;
        }

        co_return co_await impl_->config.child_synthesis_gateway->continue_child_async(
            record, [&](const ProgramChildSynthesisRecord& next) -> asio::awaitable<void> {
                co_await impl_->publish_synthesis(next);
            });
    }
    throw_runtime_diagnostic("P_CHILD_SYNTHESIS_NOT_FOUND",
                             "Child synthesis request was not found");
}

asio::awaitable<ProgramHandle> ProgramRuntime::reconnect_core(std::string owner_scope, std::string run_id,
                                                              bool transition_lock_held) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program reconnect run_id must not be empty");
    detail::ReentrantOperationLock::Guard reconnect_lock;
    if (!transition_lock_held) {
        auto held      = co_await impl_->process_state->transition_mutex->lock_async();
        reconnect_lock = std::move(held);
    }
    std::string resolved_run_id(run_id);
    bool        successor_generation = false;
    std::optional<ProgramGraphMigrationReceipt> graph_migration_receipt;
    const auto  requested_record = (co_await impl_->transitions->load(owner_scope, run_id));
    if (!requested_record) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    if (const auto lineage =
            (co_await impl_->transitions->load_run_lineage(owner_scope, requested_record->run_id()))) {
        const auto active = (co_await impl_->transitions->load_generation(
            owner_scope, lineage->lineage_id(), lineage->active_generation()));
        if (!active || active->id() != lineage->active_generation_id()) {
            throw_runtime_diagnostic("P_RUN_LINEAGE",
                                     "Program run lineage has no valid active generation");
        }
        successor_generation = active->replacement_receipt().has_value() ||
                               active->graph_migration_receipt().has_value();
        graph_migration_receipt = active->graph_migration_receipt();
        resolved_run_id = active->run_id();
    }
    run_id = resolved_run_id;
    auto record = (co_await impl_->transitions->load(owner_scope, run_id));
    if (!record) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    std::optional<ProgramRunPublicationHead> recovery_head;
    std::optional<std::int64_t> synthesis_deadline;
    if (record->invocation().budget.max_dynamic_compiles && !record->terminal_result()) {
        for (const auto& synthesis :
             (co_await impl_->transitions->load_child_syntheses(owner_scope, run_id))) {
            (co_await impl_->verify_synthesis_grant(synthesis));
            bool uncertain = false;
            if (!impl_->find_control(owner_scope, run_id) &&
                !impl_->find_process_control(owner_scope, run_id))
                uncertain = co_await impl_->synthesis_child_is_uncertain(synthesis);
            if (uncertain)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_RECOVERY_REQUIRED",
                                         "Child Core dispatch has no durable outcome; reconcile "
                                         "synthesis before parent replay");

            const auto& source = synthesis.data().parent.run;
            const auto  wall   = source.remaining_budget().wall_time_ms;
            if (wall > static_cast<std::uint64_t>(INT64_MAX - source.updated_at_ms()))
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_BUDGET",
                                         "Stored synthesis deadline is out of range");
            const auto deadline = source.updated_at_ms() + static_cast<std::int64_t>(wall);
            synthesis_deadline =
                synthesis_deadline ? std::min(*synthesis_deadline, deadline) : deadline;

            using State = ProgramChildSynthesisState;
            if (synthesis.data().state != State::Bound &&
                synthesis.data().state != State::Dispatching &&
                synthesis.data().state != State::Spawned)
                throw_runtime_diagnostic(
                    "P_CHILD_SYNTHESIS_RECOVERY_REQUIRED",
                    "Recover pending child synthesis before reconnecting its parent");
        }
    }
    try {
        // Recovery must inspect the complete causal histories, not merely the
        // reduced heads retained by a transition snapshot.
        auto runtime_state =
            (co_await impl_->transitions->load_effective_runtime_state(owner_scope, run_id));
        auto& contexts = runtime_state.context_publications;
        auto& hooks = runtime_state.hook_outbox_entries;

        std::vector<ProgramContextPublication> context_prefix;
        context_prefix.reserve(contexts.size());
        for (const auto& context : contexts) {
            validate_program_context_publication(context, *record);
            if (!is_valid_program_context_history_append(context_prefix, context)) {
                throw std::invalid_argument("Program context recovery history is not contiguous");
            }
            context_prefix.push_back(context);
        }
        if (!is_valid_program_hook_history_append({}, hooks, *record)) {
            throw std::invalid_argument("Program hook recovery heads are invalid");
        }

        if (!contexts.empty() || !hooks.empty() ||
            !runtime_state.inherited_hook_outbox_entries.empty()) {
            if (!impl_->config.runtime_recovery_handler) {
                throw_runtime_diagnostic(
                    "P_RUNTIME_RECOVERY_UNAVAILABLE",
                    "Program recovery has durable context or hook state but no recovery boundary",
                    json{{"run_id", std::string(run_id)},
                         {"context_publications", contexts.size()},
                         {"hook_outbox_entries", hooks.size()},
                         {"inherited_hook_outbox_entries",
                          runtime_state.inherited_hook_outbox_entries.size()}});
            }
            impl_->config.runtime_recovery_handler(ProgramRuntimeRecoveryState{
                std::string(owner_scope), std::string(run_id), std::move(contexts),
                std::move(hooks), std::move(runtime_state.inherited_hook_outbox_entries),
                std::move(runtime_state.transfer_receipt)});
        }
    } catch (const ProgramDiagnosticError&) {
        throw;
    } catch (const std::exception& error) {
        throw_runtime_diagnostic("P_DURABLE_HISTORY",
                                 "Program recovery cannot verify context or hook history",
                                 json{{"run_id", std::string(run_id)}, {"detail", error.what()}});
    }
    auto existing_control = impl_->find_control(owner_scope, run_id);
    if (!existing_control) {
        auto process_control = impl_->find_process_control(owner_scope, run_id);
        if (process_control &&
            (successor_generation || process_control->recorded_replay ||
             process_control->has_active_handoff_request() ||
             process_control->has_active_graph_migration_request())) {
            existing_control = std::move(process_control);
        }
    }
    if (existing_control) {
        auto control = std::move(existing_control);
        co_return ProgramHandle(std::move(control));
    }
    if (record->has_children()) {
        recovery_head =
            (co_await impl_->transitions->load_run_publication_head(owner_scope, run_id));
        if (!recovery_head)
            throw_runtime_diagnostic("P_RUN_RECOVERY_CONFLICT",
                                     "Child recovery has no consistent parent transition head");
        record = std::move(recovery_head->run_record);
    }
    if (record->recorded_binding_set_fingerprint() && !record->terminal_result())
        throw_runtime_diagnostic("P_REPLAY_EVIDENCE_REQUIRED",
            "Recorded replay recovery requires its exact owned evidence binding");

    if (graph_migration_receipt) {
        std::optional<MigrationPlan> stored_plan;
        std::optional<ProgramTransitionPublication> initial_publication;
        try {
            stored_plan = (co_await impl_->transitions->load_migration_plan(owner_scope, run_id));
            initial_publication =
                (co_await impl_->transitions->load_generation_initial_publication(
                    owner_scope, graph_migration_receipt->capsule().lineage_id(),
                    graph_migration_receipt->target_generation()));
        } catch (const std::exception& error) {
            throw_runtime_diagnostic(
                "P_GRAPH_MIGRATION_PROOF",
                "Graph migration recovery proof is unreadable",
                json{{"run_id", std::string(run_id)}, {"detail", error.what()}});
        }
        const auto source_version = (co_await impl_->config.catalog->resolve_version_async(
            owner_scope, graph_migration_receipt->source_version().id()));
        const auto target_version = (co_await impl_->config.catalog->resolve_version_async(
            owner_scope, graph_migration_receipt->target_version().id()));
        std::shared_ptr<const detail::MaterializedProgram> source_materialized;
        std::shared_ptr<const detail::MaterializedProgram> target_materialized;
        if (source_version)
            source_materialized =
                co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *source_version);
        if (target_version)
            target_materialized =
                co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *target_version);
        const auto* initial = initial_publication
            ? &initial_publication->run_record
            : nullptr;
        if (!stored_plan || !initial || !source_version || !target_version || !source_materialized ||
            !target_materialized ||
            stored_plan->id() != graph_migration_receipt->migration_plan_id() ||
            source_version->serialize_canonical() !=
                graph_migration_receipt->source_version().serialize_canonical() ||
            target_version->serialize_canonical() !=
                graph_migration_receipt->target_version().serialize_canonical() ||
            source_materialized->bundle.serialize_canonical() !=
                graph_migration_receipt->source_bundle().serialize_canonical() ||
            target_materialized->bundle.serialize_canonical() !=
                graph_migration_receipt->target_bundle().serialize_canonical() ||
            graph_migration_receipt->target_run_id() != initial->run_id() ||
            graph_migration_receipt->target_program_version_id() !=
                initial->program_version_id() ||
            graph_migration_receipt->target_bundle_id() != initial->bundle_id() ||
            graph_migration_receipt->target_invocation_id() !=
                initial->invocation().canonical_identity() ||
            graph_migration_receipt->target_binding_fingerprint() !=
                initial->binding_fingerprint() ||
            graph_migration_receipt->target_initial_run_record_id() != initial->id() ||
            graph_migration_receipt->target_initial_journal_head() !=
                initial->journal_head() ||
            !initial->exact_checkpoint() ||
            graph_migration_receipt->target_core_checkpoint() !=
                *initial->exact_checkpoint() ||
            !initial->exact_checkpoint_content_id() ||
            *initial->exact_checkpoint_content_id() !=
                graph_migration_checkpoint_content_id(
                    graph_migration_receipt->target_checkpoint()) ||
            record->run_id() != initial->run_id() ||
            record->program_version_id() != initial->program_version_id() ||
            record->bundle_id() != initial->bundle_id() ||
            record->binding_fingerprint() != initial->binding_fingerprint() ||
            record->invocation() != initial->invocation()) {
            throw_runtime_diagnostic(
                "P_GRAPH_MIGRATION_PROOF",
                "Graph migration recovery does not match its exact admitted artifacts");
        }
        std::optional<MigrationPlan> expected_plan_value;
        if (graph_migration_receipt->semantic_adapter())
            expected_plan_value = graph_migration_receipt->semantic_adapter()->migration_plan(
                *source_version, source_materialized->bundle, *target_version, target_materialized->bundle);
        else
            expected_plan_value = co_await impl_->config.catalog->plan_migration_async(
                owner_scope, source_version->id(), target_version->id());
        const auto& expected_plan = *expected_plan_value;
        if (!expected_plan.is_compatible() ||
            stored_plan->id() != expected_plan.id()) {
            throw_runtime_diagnostic(
                "P_GRAPH_MIGRATION_PROOF",
                "Graph migration recovery plan does not match the admitted versions");
        }
    }
    if (const auto fork = record->fork_receipt()) {
        // A fork is recoverable only when the exact migration proof survived
        // with the target snapshot.  Recompute the content-addressed proof as
        // well, so a store cannot silently substitute another compatible plan
        // with the same source/target version pair.
        std::optional<MigrationPlan> stored_plan;
        try {
            stored_plan = (co_await impl_->transitions->load_migration_plan(owner_scope, run_id));
        } catch (const std::exception& error) {
            throw_runtime_diagnostic(
                "P_FORK_MIGRATION_PROOF", "Fork recovery migration proof is unreadable",
                json{{"run_id", std::string(run_id)}, {"detail", error.what()}});
        }
        const auto source_version =
            (co_await impl_->config.catalog->resolve_version_async(owner_scope, fork->source_program_version_id()));
        const auto target_version =
            (co_await impl_->config.catalog->resolve_version_async(owner_scope, record->program_version_id()));
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
            throw_runtime_diagnostic("P_FORK_MIGRATION_PROOF",
                                     "Fork recovery requires a durable compatible migration proof",
                                     json{{"run_id", std::string(run_id)},
                                          {"source_version_id", fork->source_program_version_id()},
                                          {"target_version_id", record->program_version_id()}});
        }
        const auto expected_plan = (co_await impl_->config.catalog->plan_migration_async(
            owner_scope, source_version->id(), target_version->id()));
        if (!expected_plan.is_compatible() ||
            !stored_plan->semantically_matches(expected_plan)) {
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
        const auto active_lineage =
            (co_await load_active_run_lineage_async(*impl_->transitions, *record));
        if (!active_lineage) {
            throw_runtime_diagnostic(
                "P_RUN_LINEAGE", "Running Program recovery has no active generation");
        }
        const auto migration_capsule =
            (co_await impl_->transitions->load_graph_migration_capsule(
                owner_scope, record->run_id(), active_lineage->lineage.id()));
        if (migration_capsule) {
            const auto checkpoint = record->exact_checkpoint();
            if (migration_capsule->owner_scope() != owner_scope ||
                migration_capsule->source_run_id() != record->run_id() ||
                migration_capsule->source_program_version_id() !=
                    record->program_version_id() ||
                migration_capsule->source_bundle_id() != record->bundle_id() ||
                migration_capsule->source_generation() !=
                    active_lineage->generation.generation() ||
                migration_capsule->source_generation_id() !=
                    active_lineage->generation.id() ||
                migration_capsule->source_lineage_head_id() !=
                    active_lineage->lineage.id() ||
                !checkpoint || *checkpoint != migration_capsule->core_checkpoint() ||
                !record->exact_checkpoint_content_id() ||
                *record->exact_checkpoint_content_id() !=
                    graph_migration_checkpoint_content_id(
                        migration_capsule->checkpoint())) {
                throw_runtime_diagnostic(
                    "P_GRAPH_MIGRATION_HOLD",
                    "Durable graph migration source hold is corrupt");
            }

            const auto timestamp = std::max(now_ms(), record->updated_at_ms());
            const auto elapsed = static_cast<std::uint64_t>(
                timestamp - record->updated_at_ms());
            if (elapsed < active_lineage->lineage.remaining_budget().wall_time_ms) {
                throw_runtime_diagnostic(
                    "P_GRAPH_MIGRATION_HELD",
                    "Program source is held by a durable graph migration",
                    json{{"run_id", record->run_id()},
                         {"capsule_id", migration_capsule->id()}});
            }

            const auto remaining = program_replacement_remaining_budget(
                *record, active_lineage->lineage, timestamp);
            const auto previous_journal =
                (co_await impl_->transitions->latest(owner_scope, record->run_id()));
            if (!previous_journal || previous_journal->id != record->journal_head()) {
                throw_runtime_diagnostic(
                    "P_GRAPH_MIGRATION_HOLD",
                    "Expired graph migration hold lost its transition head");
            }
            auto journal = ProgramJournalRecord::create(ProgramJournalRecordData{
                record->journal_head(), record->run_id(), record->program_version_id(),
                record->bundle_id(), previous_journal->sequence + 1,
                ProgramContinuation{"root", ContinuationState::TimedOut,
                                    record->continuation().attempt},
                remaining, RunBudget{}, checkpoint, timestamp});
            ProgramResultData result_data;
            result_data.status = ProgramTerminalStatus::TimedOut;
            result_data.run_id = record->run_id();
            result_data.program_version_id = record->program_version_id();
            result_data.bundle_id = record->bundle_id();
            result_data.operation_id = "root";
            result_data.attempt = record->continuation().attempt;
            result_data.remaining_budget = remaining;
            result_data.checkpoint = checkpoint;
            result_data.failure = ProgramFailure{
                "P_GRAPH_MIGRATION_EXPIRED",
                "Graph migration source hold exhausted its wall-time budget",
                "root", checkpoint->core_name, 0,
                json{{"capsule_id", migration_capsule->id()}}};
            auto terminal_result = ProgramResult::create(std::move(result_data));
            auto terminal_event = ProgramEvent::create(ProgramEvent{
                "", record->event_sequence() + 1, timestamp, record->run_id(),
                record->program_version_id(), record->bundle_id(), "root",
                checkpoint->core_generation_id, checkpoint->core_thread_id,
                record->invocation().correlation_id, record->continuation().attempt,
                ProgramEventKind::Terminal,
                ProgramTerminalEvent{ProgramTerminalStatus::TimedOut}});
            ProgramRunRecordData data;
            data.owner_scope = record->owner_scope();
            data.run_id = record->run_id();
            data.logical_run_id              = record->logical_run_id();
            data.program_version_id = record->program_version_id();
            data.bundle_id = record->bundle_id();
            data.binding_fingerprint = record->binding_fingerprint();
            data.invocation = record->invocation();
            data.child_depth = record->child_depth();
            data.continuation = journal.continuation;
            data.remaining_budget = remaining;
            data.exact_checkpoint = checkpoint;
            data.exact_checkpoint_content_id = record->exact_checkpoint_content_id();
            data.pending_input = record->pending_input();
            data.pending_effect = record->pending_effect();
            data.terminal_result = terminal_result;
            data.fork_receipt = record->fork_receipt();
            data.children = record->children();
            data.journal_head = journal.id;
            data.fork_source_run_id = record->fork_source_run_id();
            data.fork_source_program_version_id =
                record->fork_source_program_version_id();
            data.fork_source_checkpoint_id = record->fork_source_checkpoint_id();
            data.recorded_binding_set_fingerprint =
                record->recorded_binding_set_fingerprint();
            data.event_sequence = terminal_event.sequence;
            data.effect_sequence = record->effect_sequence();
            data.created_at_ms = record->created_at_ms();
            data.updated_at_ms = timestamp;
            auto publication = ProgramTransitionPublication{
                ProgramRunRecord::create(std::move(data)), std::move(journal),
                {terminal_event}, {}};
            const auto published = (co_await compare_publish_with_lineage_async(
                *impl_->transitions, owner_scope, record->journal_head(),
                std::move(publication)));
            if (published == ProgramTransitionPublishResult::Conflict) {
                const auto winner = (co_await impl_->transitions->load_run_lineage(
                    owner_scope, record->run_id()));
                if (!winner || winner->id() == active_lineage->lineage.id()) {
                    throw_runtime_diagnostic(
                        "P_GRAPH_MIGRATION_HOLD",
                        "Expired graph migration hold could not be fenced");
                }
            }
            // The lock stays held (by this call, or by the caller that passed it in): the lock is not reentrant for a
            // coroutine, so the retry does not take it again.
            co_return (co_await reconnect_core(owner_scope, run_id, true));
        }
        const auto version =
            (co_await impl_->config.catalog->resolve_version_async(owner_scope, record->program_version_id()));
        if (!version) {
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND",
                                     "Program version required for recovery was not found");
        }
        auto        pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *version));
        const auto& recovery_budget = record->remaining_budget();
        if (record->child_depth() > MAX_SUPPORTED_CHILD_DEPTH ||
            recovery_budget.max_child_depth > MAX_SUPPORTED_CHILD_DEPTH ||
            (recovery_budget.max_child_depth == 0) != (recovery_budget.max_total_children == 0) ||
            !budget_leq(recovery_budget, pinned->version.policy_snapshot().budget_ceiling())) {
            throw_runtime_diagnostic(
                "P_RUN_RECOVERY_BUDGET",
                "Running Program recovery exceeds its admitted child budget boundary");
        }
        const auto binding_fingerprint = capability_binding_receipt_root(
            version->core_materialization_receipt().capability_bindings);
        if (pinned->bundle.id() != record->bundle_id() ||
            binding_fingerprint != record->binding_fingerprint()) {
            throw_runtime_diagnostic("P_RUN_RECOVERY_IDENTITY",
                                     "Running Program recovery identity is incompatible");
        }

        if (supports_root_execution_lease(*pinned, record->remaining_budget()) &&
            record->child_depth() == 0 &&
            record->invocation().parent_run_id.empty()) {
            const auto execution_lease =
                (co_await impl_->transitions->load_execution_lease(owner_scope, run_id));
            if (!execution_lease) {
                throw_runtime_diagnostic(
                    "P_RUN_LEASE_MISSING",
                    "Migration-capable Program recovery has no durable execution lease");
            }
            const auto expected_thread_id = record->exact_checkpoint()
                ? record->exact_checkpoint()->core_thread_id
                : core_thread_identity(run_id, pinned->root->compiled_plan_identity);
            const auto valid_lease =
                execution_lease->owner_scope() == owner_scope &&
                execution_lease->run_id() == run_id &&
                execution_lease->attempt() == record->continuation().attempt &&
                execution_lease->program_version_id() == record->program_version_id() &&
                execution_lease->bundle_id() == record->bundle_id() &&
                execution_lease->core_name() == pinned->root->core_name &&
                execution_lease->core_generation_id() ==
                    pinned->root->compiled_plan_identity &&
                execution_lease->core_thread_id() == expected_thread_id &&
                execution_lease->acquired_at_ms() == record->updated_at_ms() &&
                execution_lease->expires_at_ms() >=
                    execution_lease->acquired_at_ms() &&
                static_cast<std::uint64_t>(execution_lease->expires_at_ms() -
                                           execution_lease->acquired_at_ms()) ==
                    record->remaining_budget().wall_time_ms;
            if (!valid_lease) {
                throw_runtime_diagnostic(
                    "P_RUN_LEASE_CORRUPT",
                    "Migration-capable Program recovery has an invalid execution lease");
            }

            const auto timestamp = std::max(now_ms(), record->updated_at_ms());
            if (timestamp < execution_lease->expires_at_ms()) {
                // #421: say why the run is held and what ends it. Only a
                // define()-only Program without child budgets holds this lease.
                throw_runtime_diagnostic(
                    "P_RUN_LEASE_HELD",
                    "Program execution is held by an execution lease for the run's wall time: "
                    "reconnect() cannot continue the run until the lease expires, and then ends "
                    "it TimedOut. A Program with main(), or one started with child budgets "
                    "(max_child_depth and max_total_children above zero), holds no lease and "
                    "continues from its last checkpoint",
                    json{{"run_id", record->run_id()},
                         {"lease_id", execution_lease->id()},
                         {"expires_at_ms", execution_lease->expires_at_ms()},
                         {"retry_after_ms", execution_lease->expires_at_ms() - timestamp}});
            }

            ProgramPersistedInvocation expired_invocation{
                record->invocation().input, record->remaining_budget(),
                record->invocation().correlation_id,
                record->invocation().parent_run_id, record->child_depth()};
            auto expired_control = std::make_shared<detail::RunControl>(
                std::string(owner_scope), std::string(run_id),
                record->continuation().attempt, pinned, record->binding_fingerprint(),
                std::move(expired_invocation), record->invocation(), expected_thread_id,
                record->event_sequence(), std::shared_ptr<ProgramEventSink>{},
                impl_->scheduler, impl_->config.checkpoints,
                impl_->config.state_store, impl_->transitions, execution_lease);
            expired_control->set_hook_runtime(impl_->config.hook_runtime);
            expired_control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
            expired_control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
            expired_control->set_core_provider_call_resolver(
                impl_->config.core_provider_call_resolver,
                impl_->config.require_core_provider_call_broker);
            detail::RunOutcome timed_out;
            timed_out.status = ProgramTerminalStatus::TimedOut;
            timed_out.remaining_budget = program_replacement_remaining_budget(
                *record, active_lineage->lineage, timestamp);
            timed_out.checkpoint = record->exact_checkpoint();
            timed_out.failure = ProgramFailure{
                "P_RUN_LEASE_EXPIRED",
                "Program execution lease exhausted its wall-time budget", "root",
                pinned->root->core_name, 0,
                json{{"lease_id", execution_lease->id()}}};
            if (impl_->config.core_provider_call_resolver) {
                // The stopped attempt's journaled calls were never settled into
                // the run's budget. Binding the root operation charges them,
                // once, to this terminal result: their money (#372) and their
                // tokens (#381).
                const auto monetary = std::make_shared<ProgramMonetaryAccount>(
                    record->remaining_budget().monetary_microunits,
                    record->continuation().attempt);
                const auto tokens = std::make_shared<UsageAccumulator>();
                expired_control->set_monetary_account(monetary);
                expired_control->set_journaled_usage(tokens);
                (void)expired_control->resolve_core_provider_call_broker(
                    pinned->bundle.typed_orchestration_plan().root_id());
                const auto charged = monetary->used();
                timed_out.usage.monetary_microunits = charged;
                timed_out.remaining_budget.monetary_microunits =
                    charged >= timed_out.remaining_budget.monetary_microunits
                        ? 0 : timed_out.remaining_budget.monetary_microunits - charged;
                timed_out.monetary_records = monetary->records();
                const auto charged_tokens = tokens->total_tokens_wide();
                timed_out.usage.model_tokens = charged_tokens;
                timed_out.remaining_budget.model_tokens =
                    charged_tokens >= timed_out.remaining_budget.model_tokens
                        ? 0 : timed_out.remaining_budget.model_tokens - charged_tokens;
            }
            (co_await expired_control->complete_async(std::move(timed_out)));
            const auto result = expired_control->try_result();
            if (result && result->status() == ProgramTerminalStatus::TimedOut) {
                co_return ProgramHandle(std::move(expired_control));
            }
            throw_runtime_diagnostic(
                "P_RUN_RECOVERY_CONFLICT",
                "Expired Program execution lease lost its terminal fencing CAS");
        }

        const auto                 checkpoint = record->exact_checkpoint();
        std::string                recovered_thread_id;
        std::optional<std::string> resume_checkpoint_id;
        bool                       recover_plan_without_checkpoint = false;
        if (checkpoint) {
            if (pinned->root->compiled_plan_identity != checkpoint->core_generation_id ||
                pinned->root->core_name != checkpoint->core_name) {
                throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                         "Running Program recovery identity is incompatible");
            }
            const auto stored_checkpoint =
                impl_->config.checkpoints->load_by_id(checkpoint->checkpoint_id);
            if (!stored_checkpoint || stored_checkpoint->id != checkpoint->checkpoint_id ||
                stored_checkpoint->thread_id != checkpoint->core_thread_id ||
                stored_checkpoint->schema_version != checkpoint->checkpoint_schema_version ||
                stored_checkpoint->schema_version != graph::CHECKPOINT_SCHEMA_VERSION ||
                (record->exact_checkpoint_content_id() &&
                 graph_migration_checkpoint_content_id(*stored_checkpoint) !=
                     *record->exact_checkpoint_content_id())) {
                throw_runtime_diagnostic(
                    "P_CHECKPOINT_INCOMPATIBLE",
                    "Running Program recovery checkpoint is absent or invalid");
            }
            recovered_thread_id  = checkpoint->core_thread_id;
            resume_checkpoint_id = checkpoint->checkpoint_id;
        } else {
            // JavaScript control sessions are replayed from their durable
            // yielded-command journal.  They intentionally do not require a
            // Core checkpoint: every prior command result is fed back into a
            // fresh generator, and a pending head is surfaced as an external
            // effect instead of being redispatched.
            // #404: a typed plan that is not replay-safe stopped before its
            // first Core checkpoint. Like a JavaScript callCore in that state,
            // the operation it may have started is reconciled, not dispatched
            // again.
            recover_plan_without_checkpoint =
                !pinned->bundle.control_source() &&
                !checkpointless_replay_safe(pinned->bundle.typed_orchestration_plan());
            recovered_thread_id =
                core_thread_identity(run_id, pinned->root->compiled_plan_identity);
        }
        if (recovery_head)
            (co_await impl_->hydrate_child_reservations(*record, recovery_head->journal_record));
        std::shared_ptr<ChildQuotaReservation> global_quota;
        if (!record->invocation().parent_run_id.empty()) {
            const auto parent_child = (co_await impl_->hydrate_parent_child_budget(
                owner_scope, record->invocation().parent_run_id, record->logical_run_id()));
            global_quota = impl_->reserve_active_child(owner_scope);
            if (!impl_->activate_child_concurrency(record->invocation().parent_run_id,
                                                   record->logical_run_id(), parent_child.parent,
                                                   parent_child.child)) {
                throw_runtime_diagnostic(
                    "P_CHILD_CONCURRENCY",
                    "Recovered child exceeds the parent's active concurrency budget",
                    json{{"parent_run_id", record->invocation().parent_run_id},
                         {"child_run_id", std::string(run_id)}});
            }
        }

        std::optional<ProgramJournalRecord> loaded_journal;
        if (!recovery_head) loaded_journal = co_await impl_->transitions->latest(owner_scope, run_id);
        const auto* recovery_journal = recovery_head
            ? &recovery_head->journal_record : (loaded_journal ? &*loaded_journal : nullptr);
        if (!recovery_journal || recovery_journal->id != record->journal_head())
            throw_runtime_diagnostic("P_RUN_RECOVERY_CONFLICT",
                                     "Running Program recovery lost the transition CAS");
        ProgramPersistedInvocation invocation{
            record->invocation().input,
            restore_reserved_resources(record->remaining_budget(),
                                       recovery_journal->inflight_reservation),
            record->invocation().correlation_id, record->invocation().parent_run_id,
            record->child_depth()};
        if (!record->invocation().parent_run_id.empty() && record->continuation().attempt == 1) {
            const auto granted = record->invocation().budget.wall_time_ms;
            if (granted > static_cast<std::uint64_t>(INT64_MAX - record->created_at_ms()))
                throw_runtime_diagnostic("P_CHILD_BUDGET", "Child deadline is out of range");
            const auto deadline = record->created_at_ms() + static_cast<std::int64_t>(granted);
            const auto now      = now_ms();
            if (now >= deadline)
                throw_runtime_diagnostic("P_CHILD_BUDGET",
                                         "Child deadline expired before recovery");
            invocation.granted_budget.wall_time_ms = std::min(
                invocation.granted_budget.wall_time_ms, static_cast<std::uint64_t>(deadline - now));
        }
        if (synthesis_deadline) {
            const auto now = now_ms();
            if (now >= *synthesis_deadline)
                throw_runtime_diagnostic("P_CHILD_SYNTHESIS_BUDGET",
                                         "Parent synthesis deadline expired before recovery");
            invocation.granted_budget.wall_time_ms =
                std::min(invocation.granted_budget.wall_time_ms,
                         static_cast<std::uint64_t>(*synthesis_deadline - now));
        }
        auto control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), std::string(run_id), record->continuation().attempt,
            std::move(pinned), record->binding_fingerprint(), std::move(invocation),
            record->invocation(), std::move(recovered_thread_id), record->event_sequence(),
            std::shared_ptr<ProgramEventSink>{}, impl_->scheduler,
            impl_->config.checkpoints, impl_->config.state_store, impl_->transitions,
            std::nullopt, record->logical_run_id());
        control->recover_plan_without_checkpoint = recover_plan_without_checkpoint;
        control->fork_provider_replay =
            (co_await fork_provider_replay_of(*impl_->transitions, *impl_->config.checkpoints, owner_scope, *record));
        impl_->commit_active_child(control, global_quota);
        if (!record->invocation().parent_run_id.empty())
            impl_->bind_budgeted_child_completion(
                control, owner_scope, record->invocation().parent_run_id, record->logical_run_id());
        if (!record->invocation().parent_run_id.empty()) {
            if (auto parent = impl_->find_control(owner_scope, record->invocation().parent_run_id))
                co_await parent->attach_child(control, impl_->process_state->transition_mutex);
        }
        auto registration = (co_await impl_->register_control(control, successor_generation));
        if (!registration.inserted) co_return ProgramHandle(std::move(registration.control));
        // A resumed attempt resumes its checkpoint with the value it was
        // resumed with, never with the run input (#377).
        const auto& attempt_resume = record->attempt_resume();
        const bool  resumed_attempt = attempt_resume && resume_checkpoint_id &&
                                     attempt_resume->checkpoint_id == *resume_checkpoint_id;
        const bool  control_source = control->materialized->bundle.control_source().has_value();
        spawn_run_attempt(impl_->scheduler, impl_, control,
                          resumed_attempt && !control_source ? attempt_resume->value
                                                             : record->invocation().input,
                          std::move(resume_checkpoint_id), impl_->config.host_admission,
                          impl_->config.host_admission_resolver,
                          resumed_attempt && control_source
                              ? std::optional<json>{attempt_resume->value}
                              : std::nullopt);
        co_return ProgramHandle(std::move(control));
    }
    const bool requires_result =
        state != ContinuationState::Interrupted && state != ContinuationState::AmbiguousEffect;
    if (requires_result && !record->terminal_result()) {
        throw_runtime_diagnostic("P_RUN_INVALID", "Stored terminal Program run is incomplete");
    }
    auto control = (co_await impl_->make_record_control(*record));
    control->set_hook_runtime(impl_->config.hook_runtime);
    control->set_core_tool_grant_resolver(impl_->config.core_tool_grant_resolver);
    control->set_core_tool_grant_awaitable_resolver(impl_->config.core_tool_grant_awaitable_resolver);
    control->set_core_provider_call_resolver(
        impl_->config.core_provider_call_resolver,
        impl_->config.require_core_provider_call_broker);
    if (!record->invocation().parent_run_id.empty())
        impl_->bind_budgeted_child_completion(
            control, owner_scope, record->invocation().parent_run_id, record->logical_run_id());
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::resume_core(std::string   owner_scope,
                                                           std::string   run_id,
                                                           ProgramResume resume_value) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program resume run_id must not be empty");
    // A cancellation that ProgramHandle::cancel accepted and a task has yet to record: the resume would compete with it
    // for the same record, and the call was accepted first.
    if (impl_->process_state->pending_cancellations->pending(owner_scope, run_id))
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program run has a cancellation that is not recorded yet");

    const auto previous = (co_await impl_->transitions->load(owner_scope, run_id));
    if (!previous) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    const auto active_lineage =
        (co_await load_active_run_lineage_async(*impl_->transitions, *previous));
    const auto available_budget = active_lineage ? active_lineage->lineage.remaining_budget()
                                                 : previous->remaining_budget();
    if (previous->child_depth() > MAX_SUPPORTED_CHILD_DEPTH ||
        available_budget.max_child_depth > MAX_SUPPORTED_CHILD_DEPTH) {
        throw_runtime_diagnostic("P_RESUME_BUDGET",
                                 "Program resume exceeds the supported child depth boundary");
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
        const bool duplicate_input = pending_input &&
                                     pending_input->state() == ProgramPendingState::Consumed &&
                                     pending_input->call_id() == resume_value.pending_id &&
                                     pending_input->consumed_result() &&
                                     *pending_input->consumed_result() == resume_value.value;
        const bool duplicate_effect = pending_effect &&
                                      pending_effect->state() == ProgramPendingState::Consumed &&
                                      pending_effect->call_id() == resume_value.pending_id &&
                                      pending_effect->reconciled_result() &&
                                      *pending_effect->reconciled_result() == resume_value.value;
        if (duplicate_input || duplicate_effect) co_return (co_await reconnect_core(owner_scope, run_id));
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
        co_return (co_await reconnect_core(owner_scope, run_id));
    }
    if (pending_disposition == ProgramPendingDisposition::Expired) {
        const auto previous_journal = (co_await impl_->transitions->latest(owner_scope, run_id));
        if (!previous_journal || previous_journal->id != previous->journal_head()) {
            throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
        }
        const auto        checkpoint = *previous->exact_checkpoint();
        auto              journal    = ProgramJournalRecord::create(ProgramJournalRecordData{
            previous->journal_head(), std::string(run_id), previous->program_version_id(),
            previous->bundle_id(), previous_journal->sequence + 1,
            ProgramContinuation{"root", ContinuationState::Failed,
                                previous->continuation().attempt},
            available_budget, RunBudget{}, checkpoint, now_ms()});
        ProgramResultData result_data;
        result_data.status             = ProgramTerminalStatus::Failed;
        result_data.run_id             = std::string(run_id);
        result_data.program_version_id = previous->program_version_id();
        result_data.bundle_id          = previous->bundle_id();
        result_data.operation_id       = "root";
        result_data.attempt            = previous->continuation().attempt;
        result_data.remaining_budget   = available_budget;
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
        expired_data.logical_run_id                 = previous->logical_run_id();
        expired_data.program_version_id             = previous->program_version_id();
        expired_data.bundle_id                      = previous->bundle_id();
        expired_data.binding_fingerprint            = previous->binding_fingerprint();
        expired_data.invocation                     = previous->invocation();
        expired_data.child_depth                    = previous->child_depth();
        expired_data.children                       = terminal_children(previous->children());
        expired_data.continuation                   = journal.continuation;
        expired_data.remaining_budget               = available_budget;
        expired_data.exact_checkpoint               = checkpoint;
        if (previous->exact_checkpoint() == checkpoint) {
            expired_data.exact_checkpoint_content_id =
                previous->exact_checkpoint_content_id();
        }
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
        std::vector<ProgramEvent> expired_events;
        expired_events.push_back(terminal_event);
        auto expired_publication = ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(expired_data)), std::move(journal),
            std::move(expired_events), {}};
        const auto published         = (co_await compare_publish_with_lineage_async(
            *impl_->transitions, owner_scope, previous->journal_head(), std::move(expired_publication)));
        if (published != ProgramTransitionPublishResult::Published &&
            published != ProgramTransitionPublishResult::AlreadyPresent) {
            throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
        }
        if (!previous->invocation().parent_run_id.empty())
            (co_await publish_child_completion(impl_->transitions, owner_scope,
                                     previous->invocation().parent_run_id, run_id, terminal_result));
        throw_runtime_diagnostic(pending_code, pending_message);
    }
    if (pending_disposition != ProgramPendingDisposition::Applied) {
        throw_runtime_diagnostic(pending_code, pending_message);
    }

    // External JavaScript commands consume the submitted result into their
    // exact durable coordinate. A safely interrupted CallCore instead keeps
    // its Pending coordinate and resumes Core from the exact checkpoint; the
    // command is completed only after that resumed dispatch actually returns.
    const auto checkpoint = *previous->exact_checkpoint();
    const auto javascript_commands =
        (co_await impl_->transitions->load_javascript_commands(owner_scope, run_id));
    std::optional<JavaScriptPendingCommand> javascript_pending;
    if (const auto previous_effect = previous->pending_effect()) {
        javascript_pending = javascript_pending_command(*previous_effect);
        if (javascript_pending && javascript_pending->bundle_id != previous->bundle_id()) {
            throw_runtime_diagnostic(
                "P_JS_COMMAND_JOURNAL_MISMATCH",
                "JavaScript pending effect bundle does not match the durable run bundle");
        }
    }
    if (!javascript_pending && previous->pending_input() && !javascript_commands.empty() &&
        javascript_commands.back().pending()) {
        const auto& pending_command = javascript_commands.back();
        if (pending_command.bundle_id() != previous->bundle_id() ||
            !pending_command.effect_identity()) {
            throw_runtime_diagnostic(
                "P_JS_COMMAND_JOURNAL_MISMATCH",
                "Interrupted CallCore does not match its durable pending coordinate");
        }
        const auto operation_id =
            "root.javascript." + std::to_string(pending_command.command_ordinal());
        const auto resume_operation = javascript_call_core_operation_for_thread(
            pending_command.command(), operation_id, run_id, checkpoint.core_generation_id,
            checkpoint.core_thread_id);
        if (resume_operation) {
            const auto target = javascript_command_at_operation(pending_command.command(),
                                                                operation_id, *resume_operation);
            if (!target || target->kind() != JavaScriptCommandKind::CallCore ||
                target->arguments().at("name").get<std::string>() != checkpoint.core_name) {
                throw_runtime_diagnostic(
                    "P_CHECKPOINT_INCOMPATIBLE",
                    "Interrupted JavaScript CallCore target does not match its checkpoint");
            }
            javascript_pending = JavaScriptPendingCommand{
                pending_command.command_ordinal(), pending_command.command(),
                pending_command.bundle_id(), *pending_command.effect_identity(), resume_operation};
        }
        if (!resume_operation) {
            throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                     "Interrupted JavaScript command has no exact CallCore "
                                     "coordinate for its checkpoint");
        }
    }

    const auto version =
        (co_await impl_->config.catalog->resolve_version_async(owner_scope, previous->program_version_id()));
    if (!version) {
        throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
    }
    auto pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *version));

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
    const auto stored_resume_checkpoint =
        impl_->config.checkpoints->load_by_id(checkpoint.checkpoint_id);

    const auto previous_journal = (co_await impl_->transitions->latest(owner_scope, run_id));
    if (!previous_journal || previous_journal->id != previous->journal_head()) {
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
    }
    std::shared_ptr<ChildQuotaReservation> global_quota;
    Impl::ChildConcurrencyLease            child_concurrency;
    if (!previous->invocation().parent_run_id.empty()) {
        const auto parent_run_id = previous->invocation().parent_run_id;
        const auto parent_child =
            (co_await impl_->hydrate_parent_child_budget(owner_scope, parent_run_id, run_id));
        global_quota = impl_->reserve_active_child(owner_scope);
        if (!impl_->activate_child_concurrency(parent_run_id, run_id, parent_child.parent,
                                               parent_child.child)) {
            throw_runtime_diagnostic(
                "P_CHILD_CONCURRENCY",
                "Resumed child exceeds the parent's active concurrency budget",
                json{{"parent_run_id", parent_run_id}, {"child_run_id", std::string(run_id)}});
        }
        child_concurrency.owner         = impl_.get();
        child_concurrency.parent_run_id = parent_run_id;
        child_concurrency.child_run_id  = std::string(run_id);
        child_concurrency.amount        = parent_child.child.max_concurrency;
    }

    const bool synthetic_terminal_checkpoint =
        stored_resume_checkpoint && stored_resume_checkpoint->metadata.is_object() &&
        stored_resume_checkpoint->metadata.value("neograph_program", std::string{}) == "terminal";
    const bool reconcile_javascript_command =
        javascript_pending &&
        (previous->pending_effect().has_value() || !javascript_pending->core_resume_operation_id ||
         synthetic_terminal_checkpoint);
    // The resumed attempt needs the withheld grant to replay the generator and
    // return a reconciled result. The durable command settlement below still
    // consumes the full reservation, so replay cannot dispatch later work.
    const auto attempt_budget      = restore_reserved_resources(available_budget,
                                                                 previous_journal->inflight_reservation);
    auto       resumed_remaining   = available_budget;
    auto       resumed_reservation = previous_journal->inflight_reservation;
    if (reconcile_javascript_command) {
        resumed_reservation = RunBudget{};
    } else if (javascript_pending && javascript_pending->core_resume_operation_id &&
               resumed_reservation == RunBudget{}) {
        auto reservation    = detail::reserve_command_resources(attempt_budget);
        resumed_remaining   = std::move(reservation.remaining);
        resumed_reservation = std::move(reservation.reservation);
    }
    ProgramPersistedInvocation attempt_invocation{
        previous->invocation().input, attempt_budget, previous->invocation().correlation_id,
        previous->invocation().parent_run_id, previous->child_depth()};
    auto control = std::make_shared<detail::RunControl>(
        std::string(owner_scope), std::string(run_id), previous->continuation().attempt + 1,
        std::move(pinned), previous->binding_fingerprint(), std::move(attempt_invocation),
        previous->invocation(), checkpoint.core_thread_id, previous->event_sequence(),
        std::move(resume_value.events), impl_->scheduler,
        impl_->config.checkpoints, impl_->config.state_store, impl_->transitions,
        std::nullopt, previous->logical_run_id());
    control->fork_provider_replay =
        (co_await fork_provider_replay_of(*impl_->transitions, *impl_->config.checkpoints, owner_scope, *previous));
    impl_->commit_active_child(control, global_quota);
    if (!previous->invocation().parent_run_id.empty())
        impl_->bind_budgeted_child_completion(control, owner_scope,
                                              previous->invocation().parent_run_id, run_id);
    if (!previous->invocation().parent_run_id.empty()) {
        if (auto parent = impl_->find_control(owner_scope, previous->invocation().parent_run_id))
            co_await parent->attach_child(control);
    }
    auto journal =
        resumed_record(*control, *previous_journal, resumed_remaining, resumed_reservation);
    const auto started =
        control->stage_event(ProgramEventKind::Started, ProgramStartedEvent{attempt_budget});

    ProgramRunRecordData data;
    data.owner_scope                      = std::string(owner_scope);
    data.run_id                           = std::string(run_id);
    data.logical_run_id                   = previous->logical_run_id();
    data.program_version_id               = previous->program_version_id();
    data.bundle_id                        = previous->bundle_id();
    data.binding_fingerprint              = previous->binding_fingerprint();
    data.invocation                       = previous->invocation();
    data.child_depth                      = previous->child_depth();
    data.children                         = previous->children();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = resumed_remaining;
    data.exact_checkpoint                 = checkpoint;
    if (previous->exact_checkpoint() == checkpoint) {
        data.exact_checkpoint_content_id = previous->exact_checkpoint_content_id();
    }
    data.pending_input                    = std::move(pending_input);
    data.pending_effect                   = std::move(pending_effect);
    // What the paused plan recorded, replayed by this attempt and by its
    // recovery (#384, #386).
    data.settled_branches                 = previous->settled_branches();
    // The value this attempt resumes its checkpoint with, durable before the
    // attempt starts, so that recovery continues the same attempt (#377).
    if (javascript_commands.empty() ||
        (javascript_pending && javascript_pending->core_resume_operation_id &&
         !reconcile_javascript_command)) {
        data.attempt_resume = ProgramAttemptResume{checkpoint.checkpoint_id, resume_value.value};
    }
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

    std::vector<ProgramJavaScriptCommandJournalEntry> command_entries;
    if (reconcile_javascript_command) {
        const auto entries =
            (co_await impl_->transitions->load_javascript_commands(owner_scope, run_id));
        command_entries.push_back(javascript_completion_entry(
            entries, *javascript_pending,
            javascript_resume_terminal_result(resume_value.value,
                                              previous_journal->inflight_reservation)));
    }
    std::vector<ProgramEvent> started_events;
    started_events.push_back(started);
    auto unattached_publication = ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                                               std::move(journal),
                                                               std::move(started_events),
                                                               {},
                                                               std::nullopt,
                                                               std::move(command_entries)};
    auto publication = (co_await attach_run_lineage_async(*impl_->transitions, std::move(unattached_publication)));
    const auto execution_lease =
        make_execution_lease(*control->materialized, publication);
    ProgramTransitionPublishResult published;
    if (execution_lease)
        published = (co_await impl_->transitions->compare_publish_execution(
              owner_scope, previous->journal_head(), std::move(publication),
              std::nullopt, execution_lease));
    else
        published = (co_await impl_->transitions->compare_publish(
              owner_scope, previous->journal_head(), std::move(publication)));
    if (published != ProgramTransitionPublishResult::Published) {
        const auto                winner = (co_await impl_->transitions->load(owner_scope, run_id));
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
            co_return (co_await reconnect_core(owner_scope, run_id));
        }
        throw_runtime_diagnostic("P_RESUME_CONFLICT", "Program resume lost the transition CAS");
    }
    child_concurrency.commit();
    if (execution_lease) control->set_execution_lease(*execution_lease);

    (co_await impl_->register_control(control));
    std::optional<std::string> sink_error;
    try {
        control->deliver_event(started);
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        // The observer rejected the resume before its attempt started: no node of the resumed call has run.
        co_await complete_attempt_event_sink_rejected(control, resumed_remaining, checkpoint, *sink_error);
        co_return ProgramHandle(std::move(control));
    }

    auto attempt_input =
        !javascript_commands.empty() ? previous->invocation().input : std::move(resume_value.value);
    auto core_resume_value = javascript_pending && javascript_pending->core_resume_operation_id &&
                                     !reconcile_javascript_command
                                 ? std::optional<json>{std::move(resume_value.value)}
                                 : std::nullopt;
    spawn_run_attempt(impl_->scheduler, impl_, control, std::move(attempt_input), checkpoint.checkpoint_id,
                      impl_->config.host_admission, impl_->config.host_admission_resolver,
                      std::move(core_resume_value));
    co_return ProgramHandle(std::move(control));
}

asio::awaitable<ProgramHandle> ProgramRuntime::reconcile_core(std::string             owner_scope,
                                                              std::string             run_id,
                                                              ProgramEffectResolution resolution) {
    if (owner_scope.empty()) throw std::invalid_argument("Program owner scope must not be empty");
    if (run_id.empty()) throw std::invalid_argument("Program reconcile run_id must not be empty");
    const auto previous = (co_await impl_->transitions->load(owner_scope, run_id));
    if (!previous) {
        throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Program run was not found");
    }
    const auto active_lineage =
        (co_await load_active_run_lineage_async(*impl_->transitions, *previous));
    const auto available_budget = active_lineage ? active_lineage->lineage.remaining_budget()
                                                 : previous->remaining_budget();
    auto pending = previous->pending_effect();
    if (!pending || !previous->exact_checkpoint()) {
        throw_runtime_diagnostic("P_EFFECT_NOT_AMBIGUOUS",
                                 "Program run has no pending effect to reconcile");
    }
    auto javascript_pending = javascript_pending_command(*pending);
    if (javascript_pending && javascript_pending->bundle_id != previous->bundle_id()) {
        throw_runtime_diagnostic(
            "P_JS_COMMAND_JOURNAL_MISMATCH",
            "JavaScript pending effect bundle does not match the durable run bundle");
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
        co_return (co_await reconnect_core(owner_scope, run_id));
    }
    if (!update.state_changed()) {
        throw_runtime_diagnostic(update.diagnostic_code, update.message);
    }

    const auto checkpoint = *previous->exact_checkpoint();
    const auto javascript_commands =
        (co_await impl_->transitions->load_javascript_commands(owner_scope, run_id));
    if (!javascript_pending && !javascript_commands.empty() &&
        javascript_commands.back().pending()) {
        const auto& pending_command = javascript_commands.back();
        if (pending_command.bundle_id() != previous->bundle_id() ||
            !pending_command.effect_identity()) {
            throw_runtime_diagnostic(
                "P_JS_COMMAND_JOURNAL_MISMATCH",
                "Interrupted CallCore does not match its durable pending coordinate");
        }
        const auto operation_id =
            "root.javascript." + std::to_string(pending_command.command_ordinal());
        const auto resume_operation = javascript_call_core_operation_for_thread(
            pending_command.command(), operation_id, run_id, checkpoint.core_generation_id,
            checkpoint.core_thread_id);
        if (!resume_operation) {
            throw_runtime_diagnostic("P_CHECKPOINT_INCOMPATIBLE",
                                     "Interrupted JavaScript command has no exact CallCore "
                                     "coordinate for its checkpoint");
        }
        const auto target = javascript_command_at_operation(pending_command.command(), operation_id,
                                                            *resume_operation);
        if (!target || target->kind() != JavaScriptCommandKind::CallCore ||
            target->arguments().at("name").get<std::string>() != checkpoint.core_name) {
            throw_runtime_diagnostic(
                "P_CHECKPOINT_INCOMPATIBLE",
                "Interrupted JavaScript CallCore target does not match its checkpoint");
        }
        javascript_pending = JavaScriptPendingCommand{
            pending_command.command_ordinal(), pending_command.command(),
            pending_command.bundle_id(), *pending_command.effect_identity(), resume_operation};
    }
    const bool completed = update.value.state() == ProgramPendingState::Consumed &&
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
            (co_await impl_->config.catalog->resolve_version_async(owner_scope, previous->program_version_id()));
        if (!version) {
            throw_runtime_diagnostic("P_VERSION_NOT_FOUND", "Program version was not found");
        }
        pinned = (co_await detail::CatalogRuntimeAccess::pin_async(*impl_->config.catalog, *version));
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

    const auto previous_journal = (co_await impl_->transitions->latest(owner_scope, run_id));
    if (!previous_journal || previous_journal->id != previous->journal_head()) {
        throw_runtime_diagnostic("P_RECONCILE_CONFLICT",
                                 "Program reconciliation lost the transition CAS");
    }
    std::shared_ptr<ChildQuotaReservation> global_quota;
    Impl::ChildConcurrencyLease            child_concurrency;
    if (completed && !previous->invocation().parent_run_id.empty()) {
        const auto parent_run_id = previous->invocation().parent_run_id;
        const auto parent_child =
            (co_await impl_->hydrate_parent_child_budget(owner_scope, parent_run_id, run_id));
        global_quota = impl_->reserve_active_child(owner_scope);
        if (!impl_->activate_child_concurrency(parent_run_id, run_id, parent_child.parent,
                                               parent_child.child)) {
            throw_runtime_diagnostic(
                "P_CHILD_CONCURRENCY",
                "Reconciled child exceeds the parent's active concurrency budget",
                json{{"parent_run_id", parent_run_id}, {"child_run_id", std::string(run_id)}});
        }
        child_concurrency.owner         = impl_.get();
        child_concurrency.parent_run_id = parent_run_id;
        child_concurrency.child_run_id  = std::string(run_id);
        child_concurrency.amount        = parent_child.child.max_concurrency;
    }
    const auto next_attempt =
        completed ? previous->continuation().attempt + 1 : previous->continuation().attempt;
    const auto next_state = completed   ? ContinuationState::Running
                            : ambiguous ? ContinuationState::AmbiguousEffect
                                        : ContinuationState::Failed;
    // #404: a plan operation that stopped before its first checkpoint, reconciled
    // as completed, replays the result it was given; the plan runs on.
    const bool reconcile_plan_operation = completed && pending->core_node() == "plan";
    const bool resume_javascript_core =
        completed && javascript_pending && javascript_pending->core_resume_operation_id;
    const bool reconcile_javascript_command =
        completed && javascript_pending && !javascript_pending->core_resume_operation_id;
    // #403: a reconciled JavaScript command keeps the provider currency it
    // reserved (tokens and money): what it spent is unknown. Its wall time and
    // Core steps bound how long the run still executes, not what it may spend,
    // and the command no longer runs, so they return to the run and a
    // Completed reconcile continues main().
    RunBudget released_execution;
    if (reconcile_javascript_command) {
        released_execution.wall_time_ms   = previous_journal->inflight_reservation.wall_time_ms;
        released_execution.max_core_steps = previous_journal->inflight_reservation.max_core_steps;
    }
    RunBudget spent_reservation          = previous_journal->inflight_reservation;
    spent_reservation.wall_time_ms       = 0;
    spent_reservation.max_core_steps     = 0;
    const auto attempt_budget =
        completed && !reconcile_javascript_command
            ? restore_reserved_resources(available_budget,
                                         previous_journal->inflight_reservation)
            : restore_reserved_resources(available_budget, released_execution);
    const auto resumed_remaining =
        resume_javascript_core ? available_budget : attempt_budget;
    const auto resumed_reservation = (ambiguous || resume_javascript_core)
                                         ? previous_journal->inflight_reservation
                                         : RunBudget{};
    auto       journal             = ProgramJournalRecord::create(ProgramJournalRecordData{
        previous->journal_head(), std::string(run_id), previous->program_version_id(),
        previous->bundle_id(), previous_journal->sequence + 1,
        ProgramContinuation{"root", next_state, next_attempt}, resumed_remaining,
        resumed_reservation, checkpoint, now_ms()});

    std::shared_ptr<detail::RunControl> live_control;
    std::optional<ProgramResult>        terminal;
    std::vector<ProgramEvent>           outbox;
    if (completed) {
        ProgramPersistedInvocation invocation{
            previous->invocation().input, attempt_budget, previous->invocation().correlation_id,
            previous->invocation().parent_run_id, previous->child_depth()};
        live_control = std::make_shared<detail::RunControl>(
            std::string(owner_scope), std::string(run_id), next_attempt, std::move(pinned),
            previous->binding_fingerprint(), std::move(invocation), previous->invocation(),
            checkpoint.core_thread_id, previous->event_sequence(), std::move(resolution.events),
            impl_->scheduler, impl_->config.checkpoints,
            impl_->config.state_store, impl_->transitions, std::nullopt,
            previous->logical_run_id());
        live_control->fork_provider_replay =
            (co_await fork_provider_replay_of(*impl_->transitions, *impl_->config.checkpoints, owner_scope, *previous));
        impl_->commit_active_child(live_control, global_quota);
        if (!previous->invocation().parent_run_id.empty())
            impl_->bind_budgeted_child_completion(live_control, owner_scope,
                                                  previous->invocation().parent_run_id, run_id);
        if (!previous->invocation().parent_run_id.empty()) {
            if (auto parent =
                    impl_->find_control(owner_scope, previous->invocation().parent_run_id))
                co_await parent->attach_child(live_control);
        }
        outbox.push_back(live_control->stage_event(ProgramEventKind::Started,
                                                   ProgramStartedEvent{attempt_budget}));
    } else {
        ProgramResultData result_data;
        result_data.status =
            ambiguous ? ProgramTerminalStatus::AmbiguousEffect : ProgramTerminalStatus::Failed;
        result_data.run_id             = std::string(run_id);
        result_data.program_version_id = previous->program_version_id();
        result_data.bundle_id          = previous->bundle_id();
        result_data.operation_id       = "root";
        result_data.attempt            = next_attempt;
        result_data.remaining_budget   = available_budget;
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
    data.logical_run_id                   = previous->logical_run_id();
    data.program_version_id               = previous->program_version_id();
    data.bundle_id                        = previous->bundle_id();
    data.binding_fingerprint              = previous->binding_fingerprint();
    data.invocation                       = previous->invocation();
    data.child_depth                      = previous->child_depth();
    data.children                         = previous->children();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = resumed_remaining;
    data.exact_checkpoint                 = checkpoint;
    if (previous->exact_checkpoint() == checkpoint) {
        data.exact_checkpoint_content_id = previous->exact_checkpoint_content_id();
    }
    data.pending_input                    = previous->pending_input();
    data.pending_effect                   = update.value;
    if (completed && !reconcile_plan_operation &&
        (javascript_commands.empty() || resume_javascript_core))
        data.attempt_resume = ProgramAttemptResume{checkpoint.checkpoint_id, *resolution.result};
    // The attempt that continues a paused plan replays what it recorded
    // (#384, #386); an effect left ambiguous keeps the records for the
    // reconciliation that completes it.
    if (completed || ambiguous) data.settled_branches = previous->settled_branches();
    if (reconcile_plan_operation) {
        json settled = data.settled_branches ? *data.settled_branches : json::object();
        settled[pending->operation_id()] =
            json{{"kind", "settled"},
                 {"returned", false},
                 {"result", javascript_resume_terminal_result(*resolution.result, RunBudget{})}};
        data.settled_branches = std::move(settled);
    }
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

    std::vector<ProgramJavaScriptCommandJournalEntry> command_entries;
    if (reconcile_javascript_command) {
        const auto entries =
            (co_await impl_->transitions->load_javascript_commands(owner_scope, run_id));
        command_entries.push_back(javascript_completion_entry(
            entries, *javascript_pending,
            javascript_resume_terminal_result(*resolution.result, spent_reservation)));
    }
    auto publication = (co_await attach_run_lineage_async(
        *impl_->transitions,
        ProgramTransitionPublication{ProgramRunRecord::create(std::move(data)),
                                     std::move(journal),
                                     outbox,
                                     {},
                                     std::nullopt,
                                     std::move(command_entries)}));
    const auto execution_lease = live_control
        ? make_execution_lease(*live_control->materialized, publication)
        : std::nullopt;
    ProgramTransitionPublishResult published;
    if (execution_lease)
        published = (co_await impl_->transitions->compare_publish_execution(
              owner_scope, previous->journal_head(), std::move(publication),
              std::nullopt, execution_lease));
    else
        published = (co_await impl_->transitions->compare_publish(
              owner_scope, previous->journal_head(), std::move(publication)));
    if (published != ProgramTransitionPublishResult::Published) {
        const auto winner = (co_await impl_->transitions->load(owner_scope, run_id));
        if (winner && winner->pending_effect()) {
            if (resolution.resolution == ProgramEffectReconciliation::Unknown &&
                !resolution.result &&
                winner->pending_effect()->call_id() == resolution.pending_id &&
                winner->pending_effect()->state() == ProgramPendingState::Ambiguous) {
                co_return (co_await reconnect_core(owner_scope, run_id));
            }
            const auto duplicate = winner->pending_effect()->reconcile(
                resolution.pending_id, pending->effect_id(), resolution.resolution,
                resolution.result, transition_time);
            if (duplicate.disposition == ProgramPendingDisposition::Duplicate) {
                co_return (co_await reconnect_core(owner_scope, run_id));
            }
        }
        throw_runtime_diagnostic("P_RECONCILE_CONFLICT",
                                 "Program reconciliation lost the transition CAS");
    }
    child_concurrency.commit();
    if (execution_lease) live_control->set_execution_lease(*execution_lease);
    if (!completed) {
        // Reconciliation replaces an already-terminal Interrupted control with
        // a newer durable terminal state.  Do not let reconnect reuse that
        // stale in-memory control while another thread still owns it.
        const auto reconciled = (co_await impl_->transitions->load(owner_scope, run_id));
        if (!reconciled)
            throw_runtime_diagnostic("P_RUN_NOT_FOUND", "Reconciled Program run was not found");
        auto control = (co_await impl_->make_record_control(*reconciled));
        if (!reconciled->invocation().parent_run_id.empty())
            impl_->bind_budgeted_child_completion(control, owner_scope,
                                                  reconciled->invocation().parent_run_id, run_id);
        auto reconnected = ProgramHandle(std::move(control));
        if (const auto result = reconnected.try_result()) {
            if (!previous->invocation().parent_run_id.empty())
                (co_await publish_child_completion(impl_->transitions, owner_scope,
                                         previous->invocation().parent_run_id, run_id, *result));
        }
        co_return reconnected;
    }

    (co_await impl_->register_control(live_control));
    std::optional<std::string> sink_error;
    try {
        live_control->deliver_event(outbox.front());
    } catch (const std::exception& error) {
        sink_error = error.what();
    }
    if (sink_error) {
        // The observer rejected the reconciliation before its attempt started: no node of the call has run.
        co_await complete_attempt_event_sink_rejected(live_control, resumed_remaining, checkpoint, *sink_error);
        co_return ProgramHandle(std::move(live_control));
    }
    auto attempt_input = !javascript_commands.empty() || reconcile_plan_operation
                             ? previous->invocation().input
                             : *resolution.result;
    auto core_resume_value = javascript_pending && javascript_pending->core_resume_operation_id
                                 ? std::optional<json>{*resolution.result}
                                 : std::nullopt;
    // #404: the plan runs again from its root, without a Core resume.
    spawn_run_attempt(impl_->scheduler, impl_, live_control, std::move(attempt_input),
                      reconcile_plan_operation ? std::optional<std::string>{}
                                               : std::optional<std::string>{checkpoint.checkpoint_id},
                      impl_->config.host_admission, impl_->config.host_admission_resolver,
                      std::move(core_resume_value));
    co_return ProgramHandle(std::move(live_control));
}

// ---- The awaitable and the synchronous forms of the operations above (#557) ----
//
// Each operation is one coroutine body, `<name>_core`. The awaitable form runs it on the runtime's executor and completes
// on the executor of the coroutine that awaits it: it awaits the stores and never blocks a thread, so it is the way to use
// the runtime where the executor is pumped by a host's event loop. The synchronous form, for a thread that may wait, runs
// the same body to completion on the calling thread (RuntimeScheduler::run_blocking): it is refused where waiting would
// hang the thread, before the body starts.

asio::awaitable<ProgramResult> ProgramRuntime::run_async(std::string       owner_scope,
                                                         ProgramVersion    version,
                                                         ProgramInvocation invocation) {
    auto handle = co_await start_async(std::move(owner_scope), std::move(version), std::move(invocation));
    co_return co_await handle.wait_async();
}

asio::awaitable<ProgramResult> ProgramRuntime::resume_async(std::string   owner_scope,
                                                            std::string   run_id,
                                                            ProgramResume resume_value) {
    auto handle = co_await resume_handle_async(std::move(owner_scope), std::move(run_id), std::move(resume_value));
    co_return co_await handle.wait_async();
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_async(RunInvocation invocation,
                                                           std::shared_ptr<ProgramEventSink> events) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), invocation = std::move(invocation), events = std::move(events)]() mutable {
            return self.start_core(std::move(invocation), std::move(events));
        });
}

asio::awaitable<ProgramActiveRun> ProgramRuntime::start_active_async(RunInvocation invocation,
                                                                     std::shared_ptr<ProgramEventSink> events) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramActiveRun>(
        [self = borrowed(), invocation = std::move(invocation), events = std::move(events)]() mutable {
            return self.start_active_core(std::move(invocation), std::move(events));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_async(std::string       owner_scope,
                                                           ProgramVersion    version,
                                                           ProgramInvocation invocation) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), version = std::move(version),
         invocation = std::move(invocation)]() mutable {
            return self.start_core(std::move(owner_scope), std::move(version), std::move(invocation));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::replay_recorded_async(std::string source_run_id,
                                                                     RunInvocation invocation,
                                                                     RecordedBindingSet recorded,
                                                                     std::shared_ptr<ProgramEventSink> events) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), source_run_id = std::move(source_run_id), invocation = std::move(invocation),
         recorded = std::move(recorded), events = std::move(events)]() mutable {
            return self.replay_recorded_core(std::move(source_run_id), std::move(invocation), std::move(recorded),
                                             std::move(events));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::start_child_async(std::string       owner_scope,
                                                                 ProgramHandle     parent,
                                                                 ModuleLinkReceipt link,
                                                                 ProgramVersion    version,
                                                                 ProgramInvocation invocation) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), parent = std::move(parent),
         link = std::move(link), version = std::move(version), invocation = std::move(invocation)]() mutable {
            return self.start_child_core(std::move(owner_scope), std::move(parent), std::move(link),
                                         std::move(version), std::move(invocation));
        });
}

asio::awaitable<ProgramSynthesisReservation> ProgramRuntime::reserve_synthesis_async(
    std::string owner_scope, ProgramHandle source, ProgramHandoff& handoff,
    ProgramSynthesisProposal proposal, std::string expected_lineage_head) {
    // This view and the state it shares outlive the wait, whatever happens to the ProgramRuntime object meanwhile.
    auto self = borrowed();
    auto impl = self.impl_;
    // Reaching the checkpoint is awaited here: the body below must not wait for it on the executor's thread.
    if (handoff) (void)co_await handoff.wait_async();
    co_return co_await impl->run_coroutine_on_executor<ProgramSynthesisReservation>(
        [self = std::move(self), owner_scope = std::move(owner_scope), source = std::move(source),
         handoff = &handoff, proposal = std::move(proposal),
         expected_lineage_head = std::move(expected_lineage_head)]() mutable {
            return self.reserve_synthesis_core(std::move(owner_scope), std::move(source), handoff,
                                               std::move(proposal), std::move(expected_lineage_head));
        });
}

asio::awaitable<ProgramChildSynthesisRecord> ProgramRuntime::prepare_child_synthesis_async(
    std::string owner_scope, ProgramHandle parent, const ProgramHandoff& handoff,
    ProgramSynthesisProposal proposal, ProgramChildSynthesisGrant grant, std::string binding_name) {
    auto self = borrowed();
    auto impl = self.impl_;
    if (handoff) (void)co_await handoff.wait_async();
    co_return co_await impl->run_coroutine_on_executor<ProgramChildSynthesisRecord>(
        [self = std::move(self), owner_scope = std::move(owner_scope), parent = std::move(parent),
         handoff = &handoff, proposal = std::move(proposal), grant = std::move(grant),
         binding_name = std::move(binding_name)]() mutable {
            return self.prepare_child_synthesis_core(std::move(owner_scope), std::move(parent), handoff,
                                                     std::move(proposal), std::move(grant),
                                                     std::move(binding_name));
        });
}

asio::awaitable<ProgramChildSynthesisRecord> ProgramRuntime::recover_child_synthesis_async(
    std::string owner_scope, std::string parent_run_id, std::string proposal_id) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramChildSynthesisRecord>(
        [self = borrowed(), owner_scope = std::move(owner_scope), parent_run_id = std::move(parent_run_id),
         proposal_id = std::move(proposal_id)]() mutable {
            return self.recover_child_synthesis_core(std::move(owner_scope), std::move(parent_run_id),
                                                     std::move(proposal_id));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::reconnect_async(std::string owner_scope, std::string run_id) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), run_id = std::move(run_id)]() mutable {
            return self.reconnect_core(std::move(owner_scope), std::move(run_id));
        });
}

asio::awaitable<std::vector<ProgramHandle>> ProgramRuntime::recover_children_async(std::string owner_scope,
                                                                                    std::string parent_run_id) {
    co_return co_await impl_->run_coroutine_on_executor<std::vector<ProgramHandle>>(
        [self = borrowed(), owner_scope = std::move(owner_scope),
         parent_run_id = std::move(parent_run_id)]() mutable {
            return self.recover_children_core(std::move(owner_scope), std::move(parent_run_id));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::resume_handle_async(std::string   owner_scope,
                                                                   std::string   run_id,
                                                                   ProgramResume resume_value) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), run_id = std::move(run_id),
         resume_value = std::move(resume_value)]() mutable {
            return self.resume_core(std::move(owner_scope), std::move(run_id), std::move(resume_value));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::reconcile_async(std::string             owner_scope,
                                                               std::string             run_id,
                                                               ProgramEffectResolution resolution) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), run_id = std::move(run_id),
         resolution = std::move(resolution)]() mutable {
            return self.reconcile_core(std::move(owner_scope), std::move(run_id), std::move(resolution));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::fork_async(ExactProgramCheckpointReference source,
                                                          RunInvocation                   invocation,
                                                          ProgramResume                   resume_value,
                                                          std::shared_ptr<ProgramEventSink> events) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), source = std::move(source), invocation = std::move(invocation),
         resume_value = std::move(resume_value), events = std::move(events)]() mutable {
            return self.fork_core(std::move(source), std::move(invocation), std::move(resume_value),
                                  std::move(events));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::fork_async(std::string                     owner_scope,
                                                          ExactProgramCheckpointReference source,
                                                          ProgramVersion                  target,
                                                          ProgramInvocation               invocation,
                                                          ProgramResume                   resume_value) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), source = std::move(source),
         target = std::move(target), invocation = std::move(invocation),
         resume_value = std::move(resume_value)]() mutable {
            return self.fork_core(std::move(owner_scope), std::move(source), std::move(target),
                                  std::move(invocation), std::move(resume_value));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_async(ExactProgramHandoffReference source,
                                                             RunInvocation                invocation,
                                                             std::shared_ptr<ProgramEventSink> events) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), source = std::move(source), invocation = std::move(invocation),
         events = std::move(events)]() mutable {
            return self.replace_core(std::move(source), std::move(invocation), std::move(events));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_async(ProgramHandoff source,
                                                             RunInvocation  invocation,
                                                             std::shared_ptr<ProgramEventSink> events) {
    // This view and the state it shares outlive the wait, whatever happens to the ProgramRuntime object meanwhile.
    auto self = borrowed();
    auto impl = self.impl_;
    // The handoff is resolved here, so that the body below never waits for the source to reach its checkpoint.
    if (source) (void)co_await source.wait_async();
    co_return co_await impl->run_coroutine_on_executor<ProgramHandle>(
        [self = std::move(self), lease = &source, invocation = std::move(invocation), events = std::move(events)]() mutable {
            return self.replace_core(lease, std::move(invocation), std::move(events));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_async(std::string                  owner_scope,
                                                             ExactProgramHandoffReference source,
                                                             ProgramVersion               target,
                                                             ProgramInvocation            invocation) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), owner_scope = std::move(owner_scope), source = std::move(source),
         target = std::move(target), invocation = std::move(invocation)]() mutable {
            return self.replace_core(std::move(owner_scope), std::move(source), std::move(target),
                                     std::move(invocation));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::replace_async(std::string      owner_scope,
                                                             ProgramHandoff   source,
                                                             ProgramVersion   target,
                                                             ProgramInvocation invocation) {
    auto self = borrowed();
    auto impl = self.impl_;
    if (source) (void)co_await source.wait_async();
    co_return co_await impl->run_coroutine_on_executor<ProgramHandle>(
        [self = std::move(self), owner_scope = std::move(owner_scope), lease = &source, target = std::move(target),
         invocation = std::move(invocation)]() mutable {
            return self.replace_core(std::move(owner_scope), lease, std::move(target), std::move(invocation));
        });
}

asio::awaitable<ProgramHandle> ProgramRuntime::migrate_graph_async(ProgramHandle              source,
                                                                   ProgramGraphMigrationTarget target) {
    co_return co_await impl_->run_coroutine_on_executor<ProgramHandle>(
        [self = borrowed(), source = std::move(source), target = std::move(target)]() mutable {
            return self.migrate_graph_core(std::move(source), std::move(target));
        });
}

// ---- The synchronous forms ----

ProgramActiveRun ProgramRuntime::start_active(RunInvocation invocation, std::shared_ptr<ProgramEventSink> events) {
    return impl_->run_blocking(start_active_core(std::move(invocation), std::move(events)),
                               "ProgramRuntime::start_active", "ProgramRuntime::start_active_async");
}

ProgramHandle ProgramRuntime::start(RunInvocation invocation) {
    return start(std::move(invocation), {});
}

ProgramHandle ProgramRuntime::start(RunInvocation invocation, std::shared_ptr<ProgramEventSink> events) {
    return impl_->run_blocking(start_core(std::move(invocation), std::move(events)), "ProgramRuntime::start",
                               "ProgramRuntime::start_async");
}

ProgramHandle ProgramRuntime::start(std::string_view      owner_scope,
                                    const ProgramVersion& version,
                                    ProgramInvocation     invocation) {
    return impl_->run_blocking(start_core(std::string(owner_scope), version, std::move(invocation)),
                               "ProgramRuntime::start", "ProgramRuntime::start_async");
}

ProgramHandle ProgramRuntime::start_child(std::string_view         owner_scope,
                                          const ProgramHandle&     parent,
                                          const ModuleLinkReceipt& link,
                                          const ProgramVersion&    version,
                                          ProgramInvocation        invocation) {
    return impl_->run_blocking(
        start_child_core(std::string(owner_scope), parent, link, version, std::move(invocation)),
        "ProgramRuntime::start_child", "ProgramRuntime::start_child_async");
}

ProgramHandle ProgramRuntime::replay_recorded(std::string_view source_run_id, RunInvocation invocation,
                                              RecordedBindingSet recorded,
                                              std::shared_ptr<ProgramEventSink> events) {
    return impl_->run_blocking(replay_recorded_core(std::string(source_run_id), std::move(invocation),
                                                    std::move(recorded), std::move(events)),
                               "ProgramRuntime::replay_recorded", "ProgramRuntime::replay_recorded_async");
}

ProgramHandle ProgramRuntime::fork(ExactProgramCheckpointReference   source,
                                   RunInvocation                     invocation,
                                   ProgramResume                     resume_value,
                                   std::shared_ptr<ProgramEventSink> events) {
    return impl_->run_blocking(
        fork_core(std::move(source), std::move(invocation), std::move(resume_value), std::move(events)),
        "ProgramRuntime::fork", "ProgramRuntime::fork_async");
}

ProgramHandle ProgramRuntime::fork(std::string_view                owner_scope,
                                   ExactProgramCheckpointReference source,
                                   const ProgramVersion&           target,
                                   ProgramInvocation               invocation,
                                   ProgramResume                   resume_value) {
    return impl_->run_blocking(fork_core(std::string(owner_scope), std::move(source), target,
                                         std::move(invocation), std::move(resume_value)),
                               "ProgramRuntime::fork", "ProgramRuntime::fork_async");
}

ProgramHandle ProgramRuntime::replace(ExactProgramHandoffReference source,
                                      RunInvocation                invocation,
                                      std::shared_ptr<ProgramEventSink> events) {
    return impl_->run_blocking(replace_core(std::move(source), std::move(invocation), std::move(events)),
                               "ProgramRuntime::replace", "ProgramRuntime::replace_async");
}

ProgramHandle ProgramRuntime::replace(ProgramHandoff&&             source,
                                      RunInvocation                 invocation,
                                      std::shared_ptr<ProgramEventSink> events) {
    // The lease is the caller's: a refused or failed call leaves it as it was.
    return impl_->run_blocking_lazy(
        [&] { return replace_core(&source, std::move(invocation), std::move(events)); },
        "ProgramRuntime::replace", "ProgramRuntime::replace_async");
}

ProgramHandle ProgramRuntime::replace(std::string_view              owner_scope,
                                      ExactProgramHandoffReference   source,
                                      const ProgramVersion&          target,
                                      ProgramInvocation              invocation) {
    return impl_->run_blocking(replace_core(std::string(owner_scope), std::move(source), target,
                                            std::move(invocation)),
                               "ProgramRuntime::replace", "ProgramRuntime::replace_async");
}

ProgramHandle ProgramRuntime::replace(std::string_view      owner_scope,
                                      ProgramHandoff&&       source,
                                      const ProgramVersion&  target,
                                      ProgramInvocation      invocation) {
    return impl_->run_blocking_lazy(
        [&] { return replace_core(std::string(owner_scope), &source, target, std::move(invocation)); },
        "ProgramRuntime::replace", "ProgramRuntime::replace_async");
}

std::vector<ProgramHandle> ProgramRuntime::recover_children(std::string_view owner_scope,
                                                            std::string_view parent_run_id) {
    return impl_->run_blocking(recover_children_core(std::string(owner_scope), std::string(parent_run_id)),
                               "ProgramRuntime::recover_children", "ProgramRuntime::recover_children_async");
}

ProgramSynthesisReservation ProgramRuntime::reserve_synthesis(
    std::string_view                owner_scope,
    const ProgramHandle&            source,
    ProgramHandoff&                 handoff,
    const ProgramSynthesisProposal& proposal,
    std::string_view                expected_lineage_head) {
    return impl_->run_blocking(reserve_synthesis_core(std::string(owner_scope), source, &handoff, proposal,
                                                      std::string(expected_lineage_head)),
                               "ProgramRuntime::reserve_synthesis", "ProgramRuntime::reserve_synthesis_async");
}

ProgramChildSynthesisRecord ProgramRuntime::prepare_child_synthesis(
    std::string_view                  owner_scope,
    const ProgramHandle&              parent,
    const ProgramHandoff&             handoff,
    const ProgramSynthesisProposal&   proposal,
    const ProgramChildSynthesisGrant& grant,
    std::string                       binding_name) {
    return impl_->run_blocking(prepare_child_synthesis_core(std::string(owner_scope), parent, &handoff, proposal,
                                                            grant, std::move(binding_name)),
                               "ProgramRuntime::prepare_child_synthesis",
                               "ProgramRuntime::prepare_child_synthesis_async");
}

ProgramChildSynthesisRecord ProgramRuntime::recover_child_synthesis(std::string_view owner_scope,
                                                                    std::string_view parent_run_id,
                                                                    std::string_view proposal_id) {
    return impl_->run_blocking(recover_child_synthesis_core(std::string(owner_scope), std::string(parent_run_id),
                                                            std::string(proposal_id)),
                               "ProgramRuntime::recover_child_synthesis",
                               "ProgramRuntime::recover_child_synthesis_async");
}

ProgramHandle ProgramRuntime::reconnect(std::string_view owner_scope, std::string_view run_id) {
    return impl_->run_blocking(reconnect_core(std::string(owner_scope), std::string(run_id)),
                               "ProgramRuntime::reconnect", "ProgramRuntime::reconnect_async");
}

ProgramHandle ProgramRuntime::resume(std::string_view owner_scope,
                                     std::string_view run_id,
                                     ProgramResume    resume_value) {
    return impl_->run_blocking(resume_core(std::string(owner_scope), std::string(run_id), std::move(resume_value)),
                               "ProgramRuntime::resume", "ProgramRuntime::resume_handle_async");
}

ProgramHandle ProgramRuntime::reconcile(std::string_view        owner_scope,
                                        std::string_view        run_id,
                                        ProgramEffectResolution resolution) {
    return impl_->run_blocking(
        reconcile_core(std::string(owner_scope), std::string(run_id), std::move(resolution)),
        "ProgramRuntime::reconcile", "ProgramRuntime::reconcile_async");
}

ProgramResult ProgramRuntime::run(std::string_view      owner_scope,
                                  const ProgramVersion& version,
                                  ProgramInvocation     invocation) {
    impl_->scheduler->require_synchronous_call("ProgramRuntime::run", "ProgramRuntime::run_async");
    // Refused before the Program is started, not after: the wait that follows would be refused on a thread that runs
    // the executor, and the Program would be left running.
    impl_->scheduler->require_wait("ProgramRuntime::run", "ProgramRuntime::run_async");
    return start(owner_scope, version, std::move(invocation)).wait();
}

}  // namespace neograph::program
