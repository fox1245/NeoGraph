#include <neograph/program/transition_store.h>
#include <neograph/program/replay.h>

#include "canonical_json.h"
#include "transition_validator.h"
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>

#include <sqlite3.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
#include <neograph/program/postgres_transition_store.h>
#include <libpq-fe.h>
#endif
#include "fixtures/program_store_backends.h"
#include <gtest/gtest.h>
#include <tuple>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <atomic>
#if NEOGRAPH_BUILD_THREADS
#include <barrier>
#endif
#include <chrono>
#include <cstdlib>
#include <filesystem>
#if NEOGRAPH_BUILD_THREADS
#include <future>
#endif
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "fixtures/store_handle.h"

using neograph::program::test_support::OwnedTransitionStore;
namespace {
using namespace neograph::program;
using neograph::ContextArtifact;
using neograph::ContextArtifactData;
using neograph::ContextArtifactKind;
using neograph::ContextAssemblyReceipt;
using neograph::ContextAssemblyReceiptData;
using neograph::ContextEpoch;
using neograph::ContextEpochData;
using neograph::ContextPlacement;
using neograph::ContextPrefixClaim;
using neograph::RuntimeGuaranteeProfile;
std::string digest(char c) {
    return "sha256:" + std::string(64, c);
}
using neograph::json;
RunBudget budget() {
    return {1000, 100, 100, 1, 1, 10, 0, 0, 0};
}
CoreCheckpointIdentity checkpoint() {
    return {"main", digest('4'),
            program_root_core_thread_id("run-1", digest('4')),
            "checkpoint-1", 1};
}
ProgramJournalRecord start_journal() {
    return ProgramJournalRecord::create({{},
                                         "run-1",
                                         digest('1'),
                                         digest('2'),
                                         1,
                                         {"root", ContinuationState::Running, 1},
                                         budget(),
                                         {},
                                         std::nullopt,
                                         10});
}
ProgramEvent event(std::uint64_t       sequence,
                   ProgramEventKind    kind,
                   ProgramEventPayload payload,
                   std::int64_t        time    = 10,
                   std::uint64_t       attempt = 1) {
    ProgramEvent value;
    value.sequence           = sequence;
    value.timestamp_ms       = time;
    value.run_id             = "run-1";
    value.program_version_id = digest('1');
    value.bundle_id          = digest('2');
    value.operation_id       = "root";
    value.core_generation_id = digest('4');
    value.core_run_id = program_root_core_thread_id(value.run_id, digest('4'));
    value.trace_id           = "trace-1";
    value.kind               = kind;
    value.payload            = std::move(payload);
    value.attempt            = attempt;
    return ProgramEvent::create(std::move(value));
}
ProgramRunRecord start_run(const ProgramJournalRecord& journal) {
    ProgramRunRecordData data;
    data.owner_scope         = "owner-a";
    data.run_id              = journal.run_id;
    data.program_version_id  = journal.program_version_id;
    data.bundle_id           = journal.bundle_id;
    data.binding_fingerprint = digest('3');
    RunInvocation invocation;
    invocation.owner_scope        = data.owner_scope;
    invocation.agent_id           = "agent-a";
    invocation.program_version_id = data.program_version_id;
    invocation.run_id             = data.run_id;
    invocation.budget             = budget();
    invocation.input              = {{"input", 1}};
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "idem-1";
    invocation.correlation_id     = "trace-1";
    data.invocation               = std::move(invocation);
    data.continuation             = journal.continuation;
    data.remaining_budget         = journal.remaining_budget;
    data.journal_head             = journal.id;
    data.event_sequence           = 1;
    data.created_at_ms            = 10;
    data.updated_at_ms            = 10;
    return ProgramRunRecord::create(std::move(data));
}
ProgramRunRecordData copy_run_record_data(const ProgramRunRecord& run) {
    ProgramRunRecordData data;
    data.owner_scope                      = run.owner_scope();
    data.run_id                           = run.run_id();
    data.program_version_id               = run.program_version_id();
    data.bundle_id                        = run.bundle_id();
    data.binding_fingerprint              = run.binding_fingerprint();
    data.invocation                       = run.invocation();
    data.child_depth                      = run.child_depth();
    data.continuation                     = run.continuation();
    data.remaining_budget                 = run.remaining_budget();
    data.exact_checkpoint                 = run.exact_checkpoint();
    data.exact_checkpoint_content_id      = run.exact_checkpoint_content_id();
    data.pending_input                    = run.pending_input();
    data.pending_effect                   = run.pending_effect();
    data.terminal_result                  = run.terminal_result();
    data.fork_receipt                     = run.fork_receipt();
    data.children                         = run.children();
    data.journal_head                     = run.journal_head();
    data.fork_source_run_id               = run.fork_source_run_id();
    data.fork_source_program_version_id   = run.fork_source_program_version_id();
    data.fork_source_checkpoint_id        = run.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = run.recorded_binding_set_fingerprint();
    data.event_sequence                   = run.event_sequence();
    data.effect_sequence                  = run.effect_sequence();
    data.created_at_ms                    = run.created_at_ms();
    data.updated_at_ms                    = run.updated_at_ms();
    return data;
}
ProgramTransitionPublication start_publication() {
    auto journal = start_journal();
    return {start_run(journal),
            journal,
            {event(1, ProgramEventKind::Started, ProgramStartedEvent{budget()})},
            {}};
}

ProgramContextPublication context_publication(
    std::uint64_t sequence,
    std::optional<std::string> predecessor = std::nullopt,
    std::vector<ContextArtifact> artifacts = {}, std::string run_id = "run-1") {
    std::vector<std::string> artifact_ids;
    artifact_ids.reserve(artifacts.size());
    for (const auto& artifact : artifacts) artifact_ids.push_back(artifact.id());
    ContextEpochData epoch_data{std::move(run_id), sequence, std::move(predecessor), {}, 0, 0,
                                 digest('9'), std::move(artifact_ids),
                                 RuntimeGuaranteeProfile::Recorded};
    auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData receipt_data{epoch.id(), digest('a'), digest('b'),
                                            epoch.artifact_ids(), {}, 0, 0, 1, 0};
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, artifacts);
    return {std::move(epoch), std::move(artifacts), std::move(receipt)};
}

// An epoch over a feed that records the order of its artifacts, with the receipt of its assembly.
ProgramContextPublication ordered_context_publication(
    std::uint64_t sequence, std::optional<std::string> predecessor, std::uint64_t through,
    std::vector<ContextArtifact> artifacts, std::optional<ContextPrefixClaim> claim,
    std::string run_id = "run-1", neograph::AssemblyLayout layout = neograph::AssemblyLayout::Legacy,
    std::optional<neograph::ContextBreakReason> reason = std::nullopt) {
    ContextEpochData epoch_data;
    epoch_data.run_id = std::move(run_id);
    epoch_data.sequence = sequence;
    epoch_data.predecessor_id = std::move(predecessor);
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = through;
    epoch_data.raw_window_digest = digest('9');
    for (const auto& artifact : artifacts) epoch_data.artifact_ids.push_back(artifact.id());
    epoch_data.ordered_artifact_ids = epoch_data.artifact_ids;
    epoch_data.prefix_claim = claim;
    epoch_data.break_reason = reason;
    epoch_data.assembly_layout = layout;
    auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData receipt_data;
    receipt_data.context_epoch_id = epoch.id();
    if (reason) receipt_data.break_reason = reason;
    receipt_data.normalized_request_digest = digest('a');
    receipt_data.message_window_digest = digest('b');
    receipt_data.artifact_ids = epoch.artifact_ids();
    receipt_data.raw_from_sequence = 1;
    receipt_data.raw_through_sequence = through;
    receipt_data.estimated_input_tokens = 1;
    receipt_data.assembled_artifact_ids = *epoch.ordered_artifact_ids();
    receipt_data.artifact_classes.emplace();
    receipt_data.class_sources.emplace();
    receipt_data.class_rule_version = neograph::STABILITY_DERIVATION_RULE_VERSION;
    for (const auto& artifact : artifacts) {
        receipt_data.artifact_classes->emplace(artifact.id(), artifact.stability_class());
        receipt_data.class_sources->emplace(artifact.id(), artifact.declared_stability_class()
                                                               ? neograph::ClassSource::Declared
                                                               : neograph::ClassSource::Derived);
        if (artifact.kind() == ContextArtifactKind::RequiredSkill) {
            receipt_data.required_skill_artifact_ids.push_back(artifact.id());
        }
    }
    receipt_data.encoder_family = "openai.chat";
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, artifacts);
    return {std::move(epoch), std::move(artifacts), std::move(receipt)};
}

ContextArtifact context_skill(std::string text) {
    return ContextArtifact::create(ContextArtifactData{ContextArtifactKind::RequiredSkill,
                                                        "skill", digest('c'), {}, 0, 0, "text/plain",
                                                        ContextPlacement::BeforeHistory, 0, true,
                                                        json(std::move(text))});
}

ContextArtifact context_note(std::string text) {
    return ContextArtifact::create(ContextArtifactData{ContextArtifactKind::DerivedContext,
                                                        "note", digest('d'), {}, 0, 0, "text/plain",
                                                        ContextPlacement::AfterHistory, 0, false,
                                                        json(std::move(text))});
}

ProgramContextPublication transferred_context_publication(
    const ProgramContextPublication& source, std::string run_id) {
    ContextEpochData epoch_data{std::move(run_id), 1, std::nullopt, source.epoch.feed_id(),
                                source.epoch.raw_from_sequence(),
                                source.epoch.raw_through_sequence(),
                                source.epoch.raw_window_digest(), source.epoch.artifact_ids(),
                                source.epoch.guarantee_profile()};
    epoch_data.ordered_artifact_ids = source.epoch.ordered_artifact_ids();
    epoch_data.assembly_layout = source.epoch.assembly_layout();
    auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData receipt_data{
        epoch.id(), source.assembly_receipt.normalized_request_digest(),
        source.assembly_receipt.message_window_digest(),
        source.assembly_receipt.artifact_ids(),
        source.assembly_receipt.required_skill_artifact_ids(),
        source.assembly_receipt.raw_from_sequence(),
        source.assembly_receipt.raw_through_sequence(),
        source.assembly_receipt.estimated_input_tokens(),
        source.assembly_receipt.mandatory_input_tokens()};
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
    auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, artifacts);
    return {std::move(epoch), std::move(artifacts), std::move(receipt)};
}

neograph::HookOutboxEntry pending_hook_outbox_entry() {
    const auto runtime = neograph::RuntimeEvent::create(
        {{}, 1, neograph::HookPhase::BeforeTerminalPublication, "program_terminal",
         "owner-a", "run-1", json::object()});
    const auto invocation = neograph::HookInvocation::create(
        {{}, digest('f'), runtime.id(), "audit",
         neograph::HookPhase::BeforeTerminalPublication,
         neograph::HookDelivery::BlockingMandatory, neograph::HookFailureMode::FailClosed,
         neograph::HookIdempotency::NonIdempotent, neograph::ToolEffectClass::ExternalWrite,
         {}, {}, json::object()});
    return neograph::HookOutboxEntry::create(
        {invocation, runtime, neograph::HookExecutionState::Pending, 0, 2,
         std::chrono::system_clock::time_point(std::chrono::milliseconds(1000000))});
}

neograph::HookOutboxEntry hook_outbox_entry_with_state(
    const neograph::HookOutboxEntry& previous, neograph::HookExecutionState state) {
    auto data = previous.data();
    data.state = state;
    data.attempt_count = 1;
    data.fencing_token = 1;
    data.lease_expires_at = {};
    if (state == neograph::HookExecutionState::Succeeded) {
        data.receipt = neograph::HookExecutionReceipt::create(
            {data.invocation.id(), 1, state, {"effect", true, true, {}}, {}});
    } else {
        data.receipt.reset();
    }
    return neograph::HookOutboxEntry::create(std::move(data));
}

ContextArtifact hook_output_artifact() {
    return ContextArtifact::create(ContextArtifactData{ContextArtifactKind::HookOutput,
                                                        "hook", digest('c'), {}, 0, 0,
                                                        "application/json",
                                                        ContextPlacement::BeforeLatestUser,
                                                        0, false, json{{"hook", true}}});
}

ProgramExecutionLease execution_lease(
    const ProgramTransitionPublication& publication,
    std::string holder_id = digest('6')) {
    const auto& run = publication.run_record;
    const auto& started = publication.events.front();
    return ProgramExecutionLease(ProgramExecutionLeaseData{
        run.owner_scope(), run.run_id(), run.continuation().attempt,
        run.program_version_id(), run.bundle_id(), "main",
        started.core_generation_id, started.core_run_id, std::move(holder_id),
        run.updated_at_ms(),
        run.updated_at_ms() + static_cast<std::int64_t>(
                                  run.remaining_budget().wall_time_ms)});
}

ProgramTransitionPublication start_publication_for(std::string run_id,
                                                    std::string version_id,
                                                    std::string bundle_id,
                                                    RunBudget   granted,
                                                    std::int64_t time,
                                                    json input = json{{"input", 1}}) {
    auto journal = ProgramJournalRecord::create({{},
                                                 run_id,
                                                 version_id,
                                                 bundle_id,
                                                 1,
                                                 {"root", ContinuationState::Running, 1},
                                                 granted,
                                                 {},
                                                 std::nullopt,
                                                 time});
    RunInvocation invocation;
    invocation.owner_scope        = "owner-a";
    invocation.agent_id           = "agent-a";
    invocation.program_version_id = version_id;
    invocation.run_id             = run_id;
    invocation.budget             = granted;
    invocation.input              = std::move(input);
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "idem-" + run_id;
    invocation.correlation_id     = "trace-" + run_id;

    ProgramRunRecordData data;
    data.owner_scope         = invocation.owner_scope;
    data.run_id              = run_id;
    data.program_version_id  = version_id;
    data.bundle_id           = bundle_id;
    data.binding_fingerprint = digest('3');
    data.invocation          = std::move(invocation);
    data.continuation        = journal.continuation;
    data.remaining_budget    = granted;
    data.journal_head        = journal.id;
    data.event_sequence      = 1;
    data.created_at_ms       = time;
    data.updated_at_ms       = time;
    auto run                 = ProgramRunRecord::create(std::move(data));

    ProgramEvent started;
    started.sequence           = 1;
    started.timestamp_ms       = time;
    started.run_id             = run_id;
    started.program_version_id = version_id;
    started.bundle_id          = bundle_id;
    started.operation_id       = "root";
    started.core_generation_id = digest('4');
    started.core_run_id        = "thread-" + run_id;
    started.trace_id           = "trace-" + run_id;
    started.kind               = ProgramEventKind::Started;
    started.payload            = ProgramStartedEvent{granted};
    started.attempt            = 1;
    return {std::move(run), std::move(journal), {ProgramEvent::create(std::move(started))}, {}};
}

void attach_initial_lineage(ProgramTransitionPublication& publication) {
    const auto& run        = publication.run_record;
    const auto  lineage_id = program_run_lineage_id(run.owner_scope(), run.run_id());
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        run.owner_scope(), lineage_id, 1, run.run_id(), run.program_version_id(), run.bundle_id(),
        run.id(), run.journal_head(), std::nullopt, run.created_at_ms()});
    auto lineage = ProgramRunLineage::create(ProgramRunLineageData{
        run.owner_scope(), lineage_id, run.run_id(), 1, generation.id(), run.id(),
        run.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, std::nullopt, run.created_at_ms(),
        run.updated_at_ms()});
    publication.run_generation = std::move(generation);
    publication.run_lineage    = std::move(lineage);
}

void attach_same_generation_lineage(ProgramTransitionPublication& publication,
                                    const ProgramRunLineage&      previous) {
    const auto& run = publication.run_record;
    publication.run_generation.reset();
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        previous.active_generation(), previous.active_generation_id(), run.id(), run.journal_head(),
        publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        run.updated_at_ms()});
}

void attach_successor_lineage(ProgramTransitionPublication& publication,
                               const ProgramRunLineage&      previous) {
    const auto& run = publication.run_record;
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        run.owner_scope(), previous.lineage_id(), previous.active_generation() + 1, run.run_id(),
        run.program_version_id(), run.bundle_id(), run.id(), run.journal_head(),
        previous.active_generation_id(), run.created_at_ms()});
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        generation.generation(), generation.id(), run.id(), run.journal_head(),
        publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        run.updated_at_ms()});
    publication.run_generation = std::move(generation);
}

ProgramJavaScriptCommandJournalEntry replacement_checkpoint_entry(
    std::uint64_t sequence, bool completed, const json& handoff,
    std::string bundle_id = digest('2')) {
    std::optional<json> result;
    if (completed) {
        result = json{{"status", "completed"},
                      {"output", handoff},
                      {"failure", nullptr},
                      {"execution_trace", json::array()},
                      {"usage", json{{"wall_time_ms", 0},
                                     {"model_tokens", 0},
                                     {"monetary_microunits", 0},
                                     {"program_operations", 0},
                                     {"core_steps", 0},
                                     {"peak_concurrency", 0}}}};
    }
    return ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
        sequence, std::move(bundle_id), 1,
        JavaScriptCommand::checkpoint("replacement:checkpoint", handoff), digest('8'),
        std::move(result)});
}

void attach_replacement_successor(
    ProgramTransitionPublication&                 publication,
    const ProgramRunLineage&                      previous,
    const ProgramRunGeneration&                   predecessor,
    const ProgramRunRecord&                       source,
    const ProgramJavaScriptCommandJournalEntry& checkpoint,
    const std::vector<ProgramContextPublication>& source_contexts = {},
    const std::vector<neograph::HookOutboxEntry>& source_hooks = {}) {
    const auto handoff = checkpoint.command().arguments().at("value");
    auto receipt = ProgramReplacementReceipt(ProgramReplacementReceiptData{
        source.owner_scope(),
        previous.lineage_id(),
        predecessor.generation(),
        predecessor.id(),
        previous.id(),
        source.run_id(),
        source.id(),
        source.journal_head(),
        source.program_version_id(),
        source.bundle_id(),
        checkpoint.coordinate_id(),
        checkpoint.id(),
        program_replacement_handoff_identity(handoff),
        predecessor.generation() + 1,
        publication.run_record.run_id(),
        publication.run_record.program_version_id(),
        publication.run_record.bundle_id(),
        program_replacement_input_identity(publication.run_record.invocation().input),
        publication.run_record.invocation().canonical_identity(),
        publication.run_record.binding_fingerprint(),
        publication.run_record.id(),
        publication.run_record.journal_head()});
    std::optional<ProgramRuntimeStateTransferReceipt> transfer;
    const auto prior_transfer = predecessor.runtime_state_transfer_receipt();
    if (!source_contexts.empty() || !source_hooks.empty() ||
        (prior_transfer && !prior_transfer->hook_references().empty())) {
        std::vector<std::string> context_ids;
        for (const auto& context : source_contexts) context_ids.push_back(context.epoch.id());
        std::vector<ProgramRuntimeStateTransferHookReference> hooks;
        if (prior_transfer) {
            hooks = prior_transfer->hook_references();
        }
        for (const auto& hook : source_hooks) {
            const auto state = hook.data().state;
            if (state == neograph::HookExecutionState::Succeeded ||
                state == neograph::HookExecutionState::Cancelled) continue;
            hooks.push_back({hook.data().invocation.id(), hook.id(), hook.data().event.id()});
        }
        transfer.emplace(ProgramRuntimeStateTransferReceiptData{
            source.owner_scope(), previous.lineage_id(), predecessor.id(), previous.id(),
            source.run_id(), source.id(), source.journal_head(), predecessor.generation() + 1,
            publication.run_record.run_id(), publication.run_record.id(),
            publication.run_record.journal_head(), std::move(context_ids),
            publication.context_publication
                ? std::optional<std::string>(publication.context_publication->epoch.id())
                : std::nullopt,
            std::move(hooks)});
    }
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        publication.run_record.owner_scope(), previous.lineage_id(),
        predecessor.generation() + 1, publication.run_record.run_id(),
        publication.run_record.program_version_id(), publication.run_record.bundle_id(),
        publication.run_record.id(), publication.run_record.journal_head(), predecessor.id(),
        publication.run_record.created_at_ms(), publication.run_record.child_depth(),
        std::move(receipt), std::nullopt, std::move(transfer)});
    publication.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        generation.generation(), generation.id(), publication.run_record.id(),
        publication.run_record.journal_head(), publication.journal_record.remaining_budget,
        publication.journal_record.inflight_reservation, previous.id(), previous.created_at_ms(),
        publication.run_record.updated_at_ms(), previous.committed_descendant_budget()});
    publication.run_generation = std::move(generation);
}

// A fork takes its share of the source's amounts. max_concurrency is a limit:
// the fork carries its own and the source keeps its own (#414), unless the
// test asks for the debit that used to be made.
void attach_fork_split(ProgramTransitionPublication& publication,
                       const ProgramRunLineage&      source,
                       bool                          debit_concurrency = false) {
    attach_initial_lineage(publication);
    const auto debit = [debit_concurrency](RunBudget remaining, const RunBudget& allocated) {
        remaining.wall_time_ms -= allocated.wall_time_ms;
        remaining.model_tokens -= allocated.model_tokens;
        remaining.monetary_microunits -= allocated.monetary_microunits;
        if (debit_concurrency) remaining.max_concurrency -= allocated.max_concurrency;
        remaining.max_program_operations -= allocated.max_program_operations;
        remaining.max_core_steps -= allocated.max_core_steps;
        remaining.max_dynamic_compiles -= allocated.max_dynamic_compiles;
        remaining.max_child_depth -= allocated.max_child_depth;
        remaining.max_total_children -= allocated.max_total_children;
        return remaining;
    };
    publication.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        source.owner_scope(), source.lineage_id(), source.root_run_id(),
        source.active_generation(), source.active_generation_id(), source.active_run_record_id(),
        source.active_journal_head(),
        debit(source.remaining_budget(), publication.journal_record.remaining_budget),
        source.inflight_reservation(), source.id(), source.created_at_ms(),
        publication.run_record.updated_at_ms(), source.committed_descendant_budget()});
}

ProgramJavaScriptCommandJournalEntry javascript_command_entry(std::uint64_t sequence,
                                                              bool          completed,
                                                              std::uint64_t ordinal = 1) {
    return ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
        sequence, digest('2'), ordinal,
        JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}), digest('8'),
        completed ? std::optional<json>{json{{"status", "completed"}}} : std::nullopt});
}

ProgramTransitionPublication javascript_command_publication(
    const ProgramTransitionPublication&  previous,
    ProgramJavaScriptCommandJournalEntry command,
    std::int64_t                         time,
    std::optional<RunBudget>             remaining   = std::nullopt,
    RunBudget                            reservation = {}) {
    const auto           old_run = previous.run_record;
    auto                 journal = ProgramJournalRecord::create(
        {previous.journal_record.id, old_run.run_id(), old_run.program_version_id(),
                         old_run.bundle_id(), previous.journal_record.sequence + 1, old_run.continuation(),
                         remaining.value_or(old_run.remaining_budget()), reservation, old_run.exact_checkpoint(),
                         time});
    ProgramRunRecordData data;
    data.owner_scope                      = old_run.owner_scope();
    data.run_id                           = old_run.run_id();
    data.program_version_id               = old_run.program_version_id();
    data.bundle_id                        = old_run.bundle_id();
    data.binding_fingerprint              = old_run.binding_fingerprint();
    data.invocation                       = old_run.invocation();
    data.child_depth                      = old_run.child_depth();
    data.children                         = old_run.children();
    data.continuation                     = journal.continuation;
    data.remaining_budget                 = journal.remaining_budget;
    data.exact_checkpoint                 = old_run.exact_checkpoint();
    data.exact_checkpoint_content_id      = old_run.exact_checkpoint_content_id();
    data.pending_input                    = old_run.pending_input();
    data.pending_effect                   = old_run.pending_effect();
    data.terminal_result                  = old_run.terminal_result();
    data.fork_receipt                     = old_run.fork_receipt();
    data.fork_source_run_id               = old_run.fork_source_run_id();
    data.fork_source_program_version_id   = old_run.fork_source_program_version_id();
    data.fork_source_checkpoint_id        = old_run.fork_source_checkpoint_id();
    data.recorded_binding_set_fingerprint = old_run.recorded_binding_set_fingerprint();
    data.journal_head                     = journal.id;
    data.event_sequence                   = old_run.event_sequence();
    data.effect_sequence                  = old_run.effect_sequence();
    data.created_at_ms                    = old_run.created_at_ms();
    data.updated_at_ms                    = time;
    return {ProgramRunRecord::create(std::move(data)),
            std::move(journal),
            {},
            {},
            std::nullopt,
             {std::move(command)}};
}

struct ReplacementBoundary {
    ProgramTransitionPublication                 publication;
    ProgramJavaScriptCommandJournalEntry completed_checkpoint;
};

ReplacementBoundary publish_replacement_boundary(BlockingProgramTransitionStore& store,
                                                   bool recorded_source = false,
                                                   std::optional<ProgramContextPublication> context = {},
                                                   std::vector<neograph::HookOutboxEntry> hooks = {}) {
    auto source = start_publication();
    if (recorded_source) {
        auto source_data = copy_run_record_data(source.run_record);
        source_data.recorded_binding_set_fingerprint = digest('f');
        source.run_record = ProgramRunRecord::create(std::move(source_data));
    }
    source.context_publication = std::move(context);
    source.hook_outbox_entries = std::move(hooks);
    attach_initial_lineage(source);
    if (store.compare_publish("owner-a", {}, source) !=
        ProgramTransitionPublishResult::Published) {
        throw std::runtime_error("could not publish replacement source");
    }
    const json handoff{{"cursor", 7}, {"state", "ready"}};
    auto pending = javascript_command_publication(
        source, replacement_checkpoint_entry(1, false, handoff), 20);
    attach_same_generation_lineage(pending, *source.run_lineage);
    if (store.compare_publish("owner-a", source.journal_record.id, pending) !=
        ProgramTransitionPublishResult::Published) {
        throw std::runtime_error("could not publish replacement checkpoint coordinate");
    }
    auto completed_entry = replacement_checkpoint_entry(2, true, handoff);
    auto completed = javascript_command_publication(pending, completed_entry, 30);
    attach_same_generation_lineage(completed, *pending.run_lineage);
    if (store.compare_publish("owner-a", pending.journal_record.id, completed) !=
        ProgramTransitionPublishResult::Published) {
        throw std::runtime_error("could not publish replacement checkpoint result");
    }
    return {std::move(completed), std::move(completed_entry)};
}

void exercise_javascript_command_history(BlockingProgramTransitionStore& store) {
    EXPECT_FALSE(store.load_command_publication_head("owner-a", "run-1"));
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto empty_head = store.load_command_publication_head("owner-a", "run-1");
    ASSERT_TRUE(empty_head);
    EXPECT_EQ(empty_head->run_record.id(), start.run_record.id());
    EXPECT_EQ(empty_head->journal_record.id, start.journal_record.id);
    EXPECT_FALSE(empty_head->latest_command);
    EXPECT_FALSE(store.load_command_publication_head("owner-b", "run-1"));
    const auto pending =
        javascript_command_publication(start, javascript_command_entry(1, false), 20);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);
    const auto held_pending = store.load_command_publication_head("owner-a", "run-1");
    ASSERT_TRUE(held_pending && held_pending->latest_command);
    EXPECT_TRUE(held_pending->latest_command->pending());
    EXPECT_EQ(held_pending->latest_command->sequence(), 1U);
    const auto out_of_order =
        javascript_command_publication(pending, javascript_command_entry(2, false, 2), 25);
    EXPECT_EQ(store.compare_publish("owner-a", pending.journal_record.id, out_of_order),
              ProgramTransitionPublishResult::Conflict);
    const auto completed =
        javascript_command_publication(pending, javascript_command_entry(2, true), 30);
    ASSERT_EQ(store.compare_publish("owner-a", pending.journal_record.id, completed),
              ProgramTransitionPublishResult::Published);
    const auto duplicate =
        javascript_command_publication(completed, javascript_command_entry(3, true), 40);
    EXPECT_EQ(store.compare_publish("owner-a", completed.journal_record.id, duplicate),
              ProgramTransitionPublishResult::Conflict);
    const auto second_pending =
        javascript_command_publication(completed, javascript_command_entry(3, false, 2), 50);
    ASSERT_EQ(store.compare_publish("owner-a", completed.journal_record.id, second_pending),
              ProgramTransitionPublishResult::Published);
    const auto second_completed =
        javascript_command_publication(second_pending, javascript_command_entry(4, true, 2), 60);
    ASSERT_EQ(store.compare_publish("owner-a", second_pending.journal_record.id, second_completed),
              ProgramTransitionPublishResult::Published);

    const auto commands = store.load_javascript_commands("owner-a", "run-1", 0);
    ASSERT_EQ(commands.size(), 4U);
    EXPECT_TRUE(commands[0].pending());
    EXPECT_TRUE(commands[1].completed());
    EXPECT_TRUE(commands[2].pending());
    EXPECT_TRUE(commands[3].completed());
    EXPECT_EQ(commands[0].coordinate_id(), commands[1].coordinate_id());
    EXPECT_EQ(commands[2].coordinate_id(), commands[3].coordinate_id());
    const auto head = store.load_command_publication_head("owner-a", "run-1");
    ASSERT_TRUE(head && head->latest_command);
    EXPECT_EQ(head->run_record.id(), second_completed.run_record.id());
    EXPECT_EQ(head->journal_record.id, second_completed.journal_record.id);
    EXPECT_EQ(head->latest_command->id(), commands.back().id());
    EXPECT_TRUE(held_pending->latest_command->pending());
    EXPECT_EQ(held_pending->run_record.id(), pending.run_record.id());
    EXPECT_FALSE(empty_head->latest_command);
}

void exercise_command_head_concurrency(BlockingProgramTransitionStore& writer,
                                       BlockingProgramTransitionStore& reader) {
    auto publication = start_publication();
    ASSERT_EQ(writer.compare_publish("owner-a", {}, publication), ProgramTransitionPublishResult::Published);
    // One read of the head: its record and journal belong to one committed snapshot, and so does its newest command.
    const auto read_head = [&] {
        const auto head = reader.load_command_publication_head("owner-a", "run-1");
        if (!head || head->run_record.journal_head() != head->journal_record.id ||
            (head->latest_command ? head->latest_command->sequence() : 0) + 1 !=
                head->journal_record.sequence) {
            throw std::runtime_error("command publication read mixed two committed snapshots");
        }
    };
    const auto next_publication = [&](std::uint64_t sequence) {
        return javascript_command_publication(
            publication, javascript_command_entry(sequence, sequence % 2 == 0, (sequence + 1) / 2),
            20 + static_cast<std::int64_t>(sequence));
    };
#if NEOGRAPH_BUILD_THREADS
    std::atomic<bool> done{false};
    std::promise<void> started;
    auto observing = std::async(std::launch::async, [&] {
        started.set_value();
        std::size_t reads = 0;
        do {
            read_head();
            ++reads;
        } while (!done.load());
        return reads;
    });
    started.get_future().wait();
    // Always release the reader, including when a backend throws on a write.
    try {
        for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
            auto next = next_publication(sequence);
            if (writer.compare_publish("owner-a", publication.journal_record.id, next) !=
                ProgramTransitionPublishResult::Published) {
                throw std::runtime_error("command publication writer failed");
            }
            publication = std::move(next);
        }
    } catch (...) {
        done.store(true);
        observing.wait();
        throw;
    }
    done.store(true);
    EXPECT_GT(observing.get(), 0U);
#else
    // A build without threads has no reader running beside the writer: the reader takes its turn after every write,
    // which is the interleaving one thread has.
    read_head();
    std::size_t reads = 1;
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
        auto next = next_publication(sequence);
        if (writer.compare_publish("owner-a", publication.journal_record.id, next) !=
            ProgramTransitionPublishResult::Published) {
            throw std::runtime_error("command publication writer failed");
        }
        publication = std::move(next);
        read_head();
        ++reads;
    }
    EXPECT_GT(reads, 0U);
#endif
}

// An existing user wrapper need not implement the new virtual read.
class LegacyCommandHeadStore final : public SyncProgramTransitionStore {
public:
    InMemoryProgramTransitionStore backend;
    mutable std::function<void()> after_command_read;
    std::optional<ProgramRunRecord> load_sync(std::string_view owner, std::string_view run) const override {
        return backend.load_sync(owner, run);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner, std::string_view run) const override {
        return backend.latest_sync(owner, run);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner, std::string_view run, std::uint64_t after) const override {
        return backend.load_events_sync(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner, std::string_view run, std::uint64_t after) const override {
        return backend.load_effects_sync(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        auto values = backend.load_javascript_commands_sync(owner, run, after);
        if (auto callback = std::exchange(after_command_read, {})) callback();
        return values;
    }
    ProgramTransitionPublishResult compare_publish_sync(std::string_view owner, std::string_view previous,
                                                  ProgramTransitionPublication publication) override {
        return backend.compare_publish_sync(owner, previous, std::move(publication));
    }
};
void exercise_javascript_command_reservation_settlement(BlockingProgramTransitionStore& store) {
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    auto available                   = budget();
    available.wall_time_ms           = 800;
    available.max_program_operations = 0;
    RunBudget reservation;
    reservation.wall_time_ms = 200;
    const auto pending = javascript_command_publication(start, javascript_command_entry(1, false),
                                                        20, available, reservation);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);

    const auto terminal = json{{"status", "completed"},
                               {"usage", json{{"wall_time_ms", 50U},
                                              {"model_tokens", 0U},
                                              {"monetary_microunits", 0U},
                                              {"program_operations", 1U},
                                              {"core_steps", 0U},
                                              {"peak_concurrency", 0U}}}};
    const auto completed_entry =
        ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
            2, digest('2'), 1,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}), digest('8'),
            terminal});

    auto over_refund                   = budget();
    over_refund.wall_time_ms           = 951;
    over_refund.max_program_operations = 0;
    const auto invalid =
        javascript_command_publication(pending, completed_entry, 30, over_refund, RunBudget{});
    EXPECT_EQ(store.compare_publish("owner-a", pending.journal_record.id, invalid),
              ProgramTransitionPublishResult::Conflict);

    auto settled                   = budget();
    settled.wall_time_ms           = 950;
    settled.max_program_operations = 0;
    const auto completed =
        javascript_command_publication(pending, completed_entry, 30, settled, RunBudget{});
    ASSERT_EQ(store.compare_publish("owner-a", pending.journal_record.id, completed),
              ProgramTransitionPublishResult::Published);
    const auto latest = store.latest("owner-a", "run-1");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->remaining_budget, settled);
    EXPECT_EQ(latest->inflight_reservation, RunBudget{});
}

ProgramTransitionPublication terminal_publication(const ProgramTransitionPublication& start,
                                                  ProgramTerminalStatus               status,
                                                  ContinuationState                   state,
                                                  std::int64_t                        time = 20,
                                                  bool provider_custody = false) {
    const auto        cp             = checkpoint();
    const auto        event_sequence = start.run_record.event_sequence() + 1;
    auto              journal        = ProgramJournalRecord::create({start.journal_record.id,
                                                                     "run-1",
                                                                     digest('1'),
                                                                     digest('2'),
                                                                     start.journal_record.sequence + 1,
                                                                     {"root", state, 1},
                                                                     budget(),
                                                                     {},
                                                                     cp,
                                                                     time});
    ProgramResultData outcome;
    outcome.status             = status;
    outcome.run_id             = "run-1";
    outcome.program_version_id = digest('1');
    outcome.bundle_id          = digest('2');
    outcome.attempt            = 1;
    outcome.output             = {{"ok", true}};
    outcome.remaining_budget   = budget();
    outcome.checkpoint         = cp;
    if (provider_custody)
        outcome.provider_budget_authority = neograph::UsageAccumulator::AuthoritySnapshot{};
    if (status == ProgramTerminalStatus::Failed) {
        outcome.failure = ProgramFailure{"P_TEST", "failed", "root", "main", 1, json::object()};
    }
    auto                 result = ProgramResult::create(std::move(outcome));
    ProgramRunRecordData data;
    data.owner_scope         = "owner-a";
    data.run_id              = "run-1";
    data.program_version_id  = digest('1');
    data.bundle_id           = digest('2');
    data.binding_fingerprint = digest('3');
    data.invocation          = start.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.terminal_result     = result;
    data.journal_head        = journal.id;
    data.event_sequence      = event_sequence;
    data.created_at_ms       = 10;
    data.updated_at_ms       = time;
    return {ProgramRunRecord::create(std::move(data)),
            journal,
            {event(event_sequence, ProgramEventKind::Terminal, ProgramTerminalEvent{status}, time)},
            {}};
}

ProgramTransitionPublication child_metadata_publication(
    const ProgramTransitionPublication& start,
    ProgramChildState                   state = ProgramChildState::Publishing,
    std::int64_t                        time  = 20,
    std::string                         trace = "child-trace") {
    const auto child = ProgramChildRecord{
        "child-run-1", digest('7'), "link-receipt",
        ProgramPersistedInvocation{{{"child", true}}, budget(), std::move(trace), "run-1", 1},
        state};
    auto journal             = ProgramJournalRecord::create({start.journal_record.id,
                                                             "run-1",
                                                             digest('1'),
                                                             digest('2'),
                                                             start.journal_record.sequence + 1,
                                                             {"root", ContinuationState::Running, 1},
                                                             budget(),
                                                             {},
                                                             std::nullopt,
                                                             time});
    auto data                = ProgramRunRecordData{};
    data.owner_scope         = "owner-a";
    data.run_id              = "run-1";
    data.program_version_id  = digest('1');
    data.bundle_id           = digest('2');
    data.binding_fingerprint = digest('3');
    data.invocation          = start.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.children            = {child};
    data.journal_head        = journal.id;
    data.event_sequence      = start.run_record.event_sequence();
    data.effect_sequence     = start.run_record.effect_sequence();
    data.created_at_ms       = start.run_record.created_at_ms();
    data.updated_at_ms       = time;
    return {ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
}

ProgramPendingEffect pending_effect(std::string effect_id = "effect-one") {
    ProgramPendingEffectData data;
    data.operation_id         = "root";
    data.call_id              = "effect-call";
    data.effect_id            = std::move(effect_id);
    data.result_schema        = {{"type", "object"}, {"additionalProperties", true}};
    data.payload              = {{"kind", "publish"}};
    data.expires_at_unix_ms   = 5000;
    data.effect_mode          = EffectMode::Brokered;
    data.idempotency          = ProgramEffectIdempotency::NonIdempotent;
    data.core_node            = "main";
    data.core_interrupt_value = {{"value", "effect"}};
    return ProgramPendingEffect(std::move(data));
}

ProgramTransitionPublication interrupted_effect_publication(
    const ProgramTransitionPublication& start) {
    auto              pending = pending_effect();
    const auto        cp      = checkpoint();
    auto              journal = ProgramJournalRecord::create({start.journal_record.id,
                                                              "run-1",
                                                              digest('1'),
                                                              digest('2'),
                                                              2,
                                                              {"root", ContinuationState::Interrupted, 1},
                                                              budget(),
                                                              {},
                                                              cp,
                                                              20});
    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = "run-1";
    result_data.program_version_id = digest('1');
    result_data.bundle_id          = digest('2');
    result_data.attempt            = 1;
    result_data.output             = json::object();
    result_data.remaining_budget   = budget();
    result_data.checkpoint         = cp;
    result_data.interrupt =
        ProgramInterrupt{"main", pending.core_interrupt_value(), std::nullopt, pending};

    ProgramRunRecordData data;
    data.owner_scope         = "owner-a";
    data.run_id              = "run-1";
    data.program_version_id  = digest('1');
    data.bundle_id           = digest('2');
    data.binding_fingerprint = digest('3');
    data.invocation          = start.run_record.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.pending_effect      = pending;
    data.terminal_result     = ProgramResult::create(std::move(result_data));
    data.journal_head        = journal.id;
    data.event_sequence      = 2;
    data.effect_sequence     = 1;
    data.created_at_ms       = start.run_record.created_at_ms();
    data.updated_at_ms       = 20;
    return {ProgramRunRecord::create(std::move(data)),
            journal,
            {event(2, ProgramEventKind::Terminal,
                   ProgramTerminalEvent{ProgramTerminalStatus::Interrupted})},
            {{1, std::move(pending)}}};
}
ProgramTransitionPublication resumed_effect_publication(
    const ProgramTransitionPublication& interrupted) {
    const auto old_run = interrupted.run_record;
    const auto pending = *old_run.pending_effect();
    const auto consumed =
        pending.submit(pending.call_id(), pending.effect_id(), json{{"result", "recorded"}}, 30)
            .value;
    const auto           cp      = checkpoint();
    auto                 journal = ProgramJournalRecord::create({interrupted.journal_record.id,
                                                                 "run-1",
                                                                 digest('1'),
                                                                 digest('2'),
                                                                 3,
                                                                 {"root", ContinuationState::Running, 2},
                                                                 budget(),
                                                                 {},
                                                                 cp,
                                                                 30});
    ProgramRunRecordData data;
    data.owner_scope         = old_run.owner_scope();
    data.run_id              = old_run.run_id();
    data.program_version_id  = old_run.program_version_id();
    data.bundle_id           = old_run.bundle_id();
    data.binding_fingerprint = old_run.binding_fingerprint();
    data.invocation          = old_run.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.pending_effect      = consumed;
    data.journal_head        = journal.id;
    data.event_sequence      = 3;
    data.effect_sequence     = 1;
    data.created_at_ms       = old_run.created_at_ms();
    data.updated_at_ms       = 30;
    return {ProgramRunRecord::create(std::move(data)),
            std::move(journal),
            {event(3, ProgramEventKind::Started, ProgramStartedEvent{budget()}, 30, 2)},
            {}};
}
ForkCompatibilityReceipt legacy_fork_receipt() {
    json stored{{"format", "neograph-program-fork-compatibility"},
                {"id", "sha256:8d44faba019e258d1f64257069c3acded03428562409999265cc12ae1e067245"},
                {"owner_scope", "owner-a"},
                {"source_checkpoint_id", "checkpoint-1"},
                {"source_program_version_id", digest('1')},
                {"source_run_id", "run-1"},
                {"status", "compatible"},
                {"storage_schema_version", 1},
                {"target_program_version_id", digest('1')},
                {"witnesses", json::array()}};
    return ForkCompatibilityReceipt::parse(stored.dump());
}

void attach_fork_receipt(ProgramTransitionPublication& publication,
                         std::optional<std::string>    pending_id   = std::string("effect-call"),
                         json                          resume_value = json{{"result", "recorded"}},
                         bool                          retain_target_pending = true,
                         bool                          legacy_receipt        = false) {
    const auto           run = publication.run_record;
    ProgramRunRecordData data;
    data.owner_scope                      = run.owner_scope();
    data.run_id                           = run.run_id();
    data.program_version_id               = run.program_version_id();
    data.bundle_id                        = run.bundle_id();
    data.binding_fingerprint              = run.binding_fingerprint();
    data.invocation                       = run.invocation();
    data.continuation                     = run.continuation();
    data.remaining_budget                 = run.remaining_budget();
    data.exact_checkpoint                 = run.exact_checkpoint();
    data.exact_checkpoint_content_id      = run.exact_checkpoint_content_id();
    data.pending_input                    = run.pending_input();
    data.pending_effect                   = run.pending_effect();
    if (retain_target_pending) {
        const auto pending = pending_effect();
        const auto applied = pending.submit(pending.call_id(), pending.effect_id(),
                                            json{{"result", "recorded"}}, 30);
        if (applied.disposition != ProgramPendingDisposition::Applied) {
            throw std::logic_error("Test fork pending effect could not be consumed");
        }
        data.pending_effect = applied.value;
    }
    data.terminal_result                  = run.terminal_result();
    auto receipt                          = legacy_receipt
                                                ? legacy_fork_receipt()
                                                : ForkCompatibilityReceipt(
                             ForkCompatibilityReceiptData{"owner-a",
                                                          "run-1",
                                                          digest('1'),
                                                          "checkpoint-1",
                                                          digest('1'),
                                                          ForkCompatibilityStatus::Compatible,
                                                                                   {}})
                             .with_initial_resume_binding(std::move(pending_id), resume_value);
    data.fork_receipt                     = std::move(receipt);
    data.children                         = run.children();
    data.journal_head                     = run.journal_head();
    data.recorded_binding_set_fingerprint = run.recorded_binding_set_fingerprint();
    data.event_sequence                   = run.event_sequence();
    data.effect_sequence                  = run.effect_sequence();
    data.created_at_ms                    = run.created_at_ms();
    data.updated_at_ms                    = run.updated_at_ms();
    publication.run_record                = ProgramRunRecord::create(std::move(data));
}
json publication_reference_body(const ProgramTransitionPublication& publication) {
    json events = json::array();
    for (const auto& event : publication.events) {
        events.push_back(detail::parse_json_strict(event.serialize_canonical()));
    }

    json effects = json::array();
    for (const auto& effect : publication.effects) {
        effects.push_back(detail::parse_json_strict(effect.serialize_canonical()));
    }

    return {{"format", "neograph-program-transition-publication"},
             {"storage_schema_version", 5},
            {"run_record", detail::parse_json_strict(publication.run_record.serialize_canonical())},
            {"journal_record",
             detail::parse_json_strict(publication.journal_record.serialize_canonical())},
            {"events", std::move(events)},
            {"effects", std::move(effects)},
              {"commands", json::array()},
              {"context_publication", nullptr},
              {"hook_outbox_entries", json::array()},
            {"migration_plan",
             publication.migration_plan
                 ? detail::parse_json_strict(publication.migration_plan->serialize_canonical())
                 : json(nullptr)}};
}
class CommandReadUnsupportedStore final : public SyncProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load_sync(std::string_view, std::string_view) const override {
        return std::nullopt;
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view, std::string_view) const override {
        return std::nullopt;
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view,
                                          std::string_view,
                                          std::uint64_t) const override {
        return {};
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view,
                                                       std::string_view,
                                                       std::uint64_t) const override {
        return {};
    }
    ProgramTransitionPublishResult compare_publish_sync(std::string_view,
                                                   std::string_view,
                                                   ProgramTransitionPublication) override {
        return ProgramTransitionPublishResult::Conflict;
    }
};

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
class TestSqliteDatabase final {
public:
    explicit TestSqliteDatabase(const std::string& path) {
        if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "SQLite unavailable";
            if (db_) sqlite3_close_v2(db_);
            db_ = nullptr;
            throw std::runtime_error("test SQLite open: " + error);
        }
    }

    TestSqliteDatabase(const TestSqliteDatabase&)            = delete;
    TestSqliteDatabase& operator=(const TestSqliteDatabase&) = delete;

    ~TestSqliteDatabase() {
        if (db_) sqlite3_close_v2(db_);
    }

    void execute(std::string_view sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.data(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error("test SQLite execute: " + message);
        }
    }

    std::int64_t scalar(std::string_view sql) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement,
                               nullptr) != SQLITE_OK)
            throw std::runtime_error("test SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
        const int result = sqlite3_step(statement);
        if (result != SQLITE_ROW) {
            const std::string error = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            throw std::runtime_error("test SQLite scalar: " + error);
        }
        const auto value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return value;
    }

    void insert_legacy(std::string_view owner_scope,
                       std::string_view run_id,
                       std::string_view canonical_bytes,
                       std::string_view last_publication_bytes) {
        sqlite3_stmt*              statement = nullptr;
        constexpr std::string_view sql =
            "INSERT INTO program_transition_runs"
            "(owner_scope, run_id, canonical_bytes, last_publication_bytes) "
            "VALUES(?1, ?2, ?3, ?4)";
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &statement,
                               nullptr) != SQLITE_OK)
            throw std::runtime_error("test SQLite prepare: " + std::string(sqlite3_errmsg(db_)));
        const auto bind = [&](int index, std::string_view value, bool blob) {
            const int result =
                blob ? sqlite3_bind_blob(statement, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT)
                     : sqlite3_bind_text(statement, index, value.data(),
                                         static_cast<int>(value.size()), SQLITE_TRANSIENT);
            if (result != SQLITE_OK) throw std::runtime_error("test SQLite bind");
        };
        try {
            bind(1, owner_scope, false);
            bind(2, run_id, false);
            bind(3, canonical_bytes, true);
            bind(4, last_publication_bytes, true);
            if (sqlite3_step(statement) != SQLITE_DONE)
                throw std::runtime_error("test SQLite insert: " + std::string(sqlite3_errmsg(db_)));
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        sqlite3_finalize(statement);
    }

private:
    sqlite3* db_ = nullptr;
};
#endif

void exercise_initial_reservation_is_rejected(BlockingProgramTransitionStore& store) {
    auto reservation                     = RunBudget{};
    reservation.wall_time_ms             = 1;
    auto                         journal = ProgramJournalRecord::create({{},
                                                                         "run-1",
                                                                         digest('1'),
                                                                         digest('2'),
                                                                         1,
                                                                         {"root", ContinuationState::Running, 1},
                                                                         budget(),
                                                                         reservation,
                                                                         std::nullopt,
                                                                         10});
    ProgramTransitionPublication publication{
        start_run(journal),
        journal,
        {event(1, ProgramEventKind::Started, ProgramStartedEvent{budget()})},
        {}};
    EXPECT_EQ(store.compare_publish("owner-a", {}, std::move(publication)),
              ProgramTransitionPublishResult::Conflict);
}
void exercise_commandless_running_checkpoint_requires_host_evidence(
    BlockingProgramTransitionStore& store) {
    auto initial = start_publication();
    attach_initial_lineage(initial);
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
              ProgramTransitionPublishResult::Published);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);

    const auto installed_checkpoint = checkpoint();
    auto journal = ProgramJournalRecord::create({
        initial.journal_record.id, "run-1", digest('1'), digest('2'), 2,
        {"root", ContinuationState::Running, 1}, budget(), {},
        installed_checkpoint, 20});
    auto data = copy_run_record_data(initial.run_record);
    data.exact_checkpoint = installed_checkpoint;
    data.exact_checkpoint_content_id = digest('7');
    data.journal_head = journal.id;
    data.event_sequence = 2;
    data.updated_at_ms = 20;
    ProgramTransitionPublication update{
        ProgramRunRecord::create(std::move(data)), std::move(journal),
        {event(2, ProgramEventKind::CheckpointPublished,
               ProgramCheckpointEvent{installed_checkpoint}, 20)}, {}};
    attach_same_generation_lineage(update, *lineage);

    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id,
                                    std::move(update)),
              ProgramTransitionPublishResult::Conflict);
}
void exercise_cross_thread_call_core_settlement(BlockingProgramTransitionStore& store,
                                                bool                    exact_command_coordinate) {
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    auto available                   = budget();
    available.wall_time_ms           = 800;
    available.max_program_operations = 0;
    RunBudget reservation;
    reservation.wall_time_ms = 200;
    const auto pending = javascript_command_publication(start, javascript_command_entry(1, false),
                                                        20, available, reservation);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);

    auto unrelated_checkpoint = checkpoint();
    if (exact_command_coordinate) {
        std::string identity = "run-1";
        identity.push_back('\0');
        identity.append("root.javascript.1");
        identity.push_back('\0');
        identity.append(digest('4'));
        unrelated_checkpoint.core_thread_id =
            detail::sha256_identity("program-core-thread/v1", identity);
    } else {
        unrelated_checkpoint.core_thread_id = "sibling-command-thread";
    }
    unrelated_checkpoint.checkpoint_id = "checkpoint-2";
    auto settled                       = available;
    settled.wall_time_ms               = 950;
    const ProgramUsage usage{50, 0, 0, 0, 0, 0};
    auto               journal = ProgramJournalRecord::create({pending.journal_record.id,
                                                               "run-1",
                                                               digest('1'),
                                                               digest('2'),
                                                               3,
                                                               {"root", ContinuationState::Interrupted, 1},
                                                               settled,
                                                               {},
                                                               unrelated_checkpoint,
                                                               30});

    ProgramPendingInputData pending_data;
    pending_data.operation_id         = "root.javascript.1";
    pending_data.call_id              = "call-1";
    pending_data.kind                 = ProgramPendingInputKind::Input;
    pending_data.result_schema        = json::object();
    pending_data.payload              = json::object();
    pending_data.core_node            = "main";
    pending_data.core_interrupt_value = json::object();
    auto pending_input                = ProgramPendingInput(std::move(pending_data));

    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = "run-1";
    result_data.program_version_id = digest('1');
    result_data.bundle_id          = digest('2');
    result_data.attempt            = 1;
    result_data.usage              = usage;
    result_data.remaining_budget   = settled;
    result_data.checkpoint         = unrelated_checkpoint;
    result_data.interrupt = ProgramInterrupt{"main", json::object(), pending_input, std::nullopt};
    auto result           = ProgramResult::create(std::move(result_data));

    const auto           old_run = pending.run_record;
    ProgramRunRecordData data;
    data.owner_scope         = old_run.owner_scope();
    data.run_id              = old_run.run_id();
    data.program_version_id  = old_run.program_version_id();
    data.bundle_id           = old_run.bundle_id();
    data.binding_fingerprint = old_run.binding_fingerprint();
    data.invocation          = old_run.invocation();
    data.child_depth         = old_run.child_depth();
    data.children            = old_run.children();
    data.continuation        = journal.continuation;
    data.remaining_budget    = settled;
    data.exact_checkpoint    = unrelated_checkpoint;
    data.pending_input       = pending_input;
    data.terminal_result     = result;
    data.journal_head        = journal.id;
    data.event_sequence      = 3;
    data.effect_sequence     = old_run.effect_sequence();
    data.created_at_ms       = old_run.created_at_ms();
    data.updated_at_ms       = 30;
    ProgramTransitionPublication invalid{
        ProgramRunRecord::create(std::move(data)),
        std::move(journal),
        {event(2, ProgramEventKind::CheckpointPublished,
               ProgramCheckpointEvent{unrelated_checkpoint}, 30),
         event(3, ProgramEventKind::Terminal,
               ProgramTerminalEvent{ProgramTerminalStatus::Interrupted}, 30)},
        {}};
    EXPECT_EQ(store.compare_publish("owner-a", pending.journal_record.id, std::move(invalid)),
              exact_command_coordinate ? ProgramTransitionPublishResult::Published
                                       : ProgramTransitionPublishResult::Conflict);
}

void exercise_execution_lease_fences_ordinary_publication(
    BlockingProgramTransitionStore& store) {
    auto start = start_publication();
    attach_initial_lineage(start);
    const auto lease = execution_lease(start);
    ASSERT_EQ(store.compare_publish_execution(
                  "owner-a", "", start, std::nullopt, lease),
              ProgramTransitionPublishResult::Published);
    const auto durable_lease = store.load_execution_lease("owner-a", "run-1");
    ASSERT_TRUE(durable_lease);
    EXPECT_EQ(durable_lease->id(), lease.id());

    auto terminal = terminal_publication(
        start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
    attach_same_generation_lineage(terminal, *start.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.load("owner-a", "run-1")->id(), start.run_record.id());

    const auto wrong_lease = execution_lease(start, digest('7'));
    EXPECT_EQ(store.compare_publish_execution(
                  "owner-a", start.journal_record.id, terminal, wrong_lease,
                  std::nullopt),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.compare_publish_execution(
                  "owner-a", start.journal_record.id, terminal, lease,
                  std::nullopt),
              ProgramTransitionPublishResult::Published);
    EXPECT_FALSE(store.load_execution_lease("owner-a", "run-1"));
}
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
void reset_postgres_transition_schema(const char* url) {
    auto* connection = PQconnectdb(url);
    if (!connection || PQstatus(connection) != CONNECTION_OK) {
        const auto message = connection ? std::string(PQerrorMessage(connection))
                                        : std::string("null PostgreSQL connection");
        if (connection) PQfinish(connection);
        throw std::runtime_error("PostgreSQL transition test reset failed: " + message);
    }
    auto* database = PQexec(connection, "SELECT current_database()");
    const bool database_query_succeeded = database &&
        PQresultStatus(database) == PGRES_TUPLES_OK && PQntuples(database) == 1 &&
        PQnfields(database) == 1;
    if (!database_query_succeeded ||
        std::string(PQgetvalue(database, 0, 0)) != "neograph_test") {
        const auto actual = database_query_succeeded
            ? std::string(PQgetvalue(database, 0, 0))
            : std::string("unknown");
        if (database) PQclear(database);
        PQfinish(connection);
        throw std::runtime_error(
            "PostgreSQL transition tests require the disposable neograph_test database; got " +
            actual);
    }
    PQclear(database);
    const char* sql    = R"SQL(
DROP TABLE IF EXISTS neograph_program_execution_leases_v1;
DROP TABLE IF EXISTS neograph_program_graph_migration_capsules_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_runs_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_history_v1;
DROP TABLE IF EXISTS neograph_program_run_generation_publications_v1;
DROP TABLE IF EXISTS neograph_program_run_generations_v1;
DROP TABLE IF EXISTS neograph_program_run_lineage_heads_v1;
DROP TABLE IF EXISTS neograph_program_transition_synthesis_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_hook_outbox_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_context_log_v1;
DROP TABLE IF EXISTS neograph_program_transition_javascript_command_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_effect_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_event_log_v2;
DROP TABLE IF EXISTS neograph_program_transition_run_heads_v2;
)SQL";
    auto* result = PQexec(connection, sql);
    const auto status = result ? PQresultStatus(result) : PGRES_FATAL_ERROR;
    if (status != PGRES_COMMAND_OK) {
        const auto message = result ? std::string(PQresultErrorMessage(result))
                                    : std::string(PQerrorMessage(connection));
        if (result) PQclear(result);
        PQfinish(connection);
        throw std::runtime_error("PostgreSQL transition test reset failed: " + message);
    }
    PQclear(result);
    PQfinish(connection);
}
#endif
}  // namespace

// The contract every ProgramTransitionStore meets, run on each backend in
// tests/fixtures/program_store_backends.h. A clause the in-memory stores cannot
// meet is skipped for them with its reason; PostgreSQL cases skip without
// NEOGRAPH_TEST_POSTGRES_URL.
namespace {
using neograph::program::test_support::StoreBackend;
using neograph::program::test_support::StoreCase;
using neograph::program::test_support::StoreDeployment;
using neograph::program::test_support::WriteStage;

class ProgramTransitionStoreConformance : public ::testing::TestWithParam<StoreCase> {
protected:
    void SetUp() override {
        if (const auto reason =
                neograph::program::test_support::backend_unavailable_reason(GetParam().backend))
            GTEST_SKIP() << *reason;
        deployment_.emplace(GetParam().backend, GetParam().form);
        store_ = deployment_->blocking_transition_store();
    }
    void TearDown() override {
        store_.reset();
        deployment_.reset();
    }

    StoreDeployment& deployment() { return *deployment_; }
    // The store the case writes through. A second `deployment().transition_store()` is a
    // second writer on a database backend, and the same store in memory.
    // Called in the form of the case: through a synchronous store's own operations, or through its awaitable ones.
    BlockingProgramTransitionStore& primary_store() { return *store_; }
    std::shared_ptr<BlockingProgramTransitionStore> second_store() {
        return deployment_->blocking_transition_store();
    }
    // Closes the primary store and opens another over the same data, as after a process restart.
    void restart_store() {
        store_.reset();
        store_ = deployment_->blocking_transition_store();
    }

private:
    std::optional<StoreDeployment>                  deployment_;
    std::shared_ptr<BlockingProgramTransitionStore> store_;
};

constexpr const char* NOT_DURABLE =
    "the in-memory store keeps typed values in the process: durability, restart and "
    "stored-byte tamper detection need a durable backend (declared capability, #556)";

INSTANTIATE_TEST_SUITE_P(
    Backends, ProgramTransitionStoreConformance,
    ::testing::ValuesIn(neograph::program::test_support::store_cases()),
    [](const ::testing::TestParamInfo<StoreCase>& info) {
        return neograph::program::test_support::case_name(info.param);
    });
}  // namespace

TEST(ProgramTransitionStoreTest, MissingJavaScriptCommandReadSupportFailsClosed) {
    CommandReadUnsupportedStore    unsupported;
    BlockingProgramTransitionStore store(unsupported);
    EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::runtime_error);
    // The awaitable form refuses too, when it is awaited.
    EXPECT_THROW(
        {
            BlockingProgramTransitionStore awaiting(unsupported, [] {
                BlockingStorePolicy policy;
                policy.await_synchronous_stores = true;
                return policy;
            }());
            (void)awaiting.load_javascript_commands("owner-a", "run-1");
        },
        std::runtime_error);
}

TEST(ProgramTransitionStoreTest, CanonicalValuesRejectTamper) {
    auto publication  = terminal_publication(start_publication(), ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed);
    auto result_bytes = publication.run_record.terminal_result()->serialize_canonical();
    result_bytes.replace(result_bytes.find("completed"), 9, "cancelled");
    EXPECT_THROW((void)ProgramResult::parse(result_bytes), std::invalid_argument);
    auto event_bytes = publication.events.front().serialize_canonical();
    event_bytes.replace(event_bytes.find("run-1"), 5, "run-2");
    EXPECT_THROW((void)ProgramEvent::parse(event_bytes), std::invalid_argument);
    auto run_bytes = publication.run_record.serialize_canonical();
    run_bytes.replace(run_bytes.find("owner-a"), 7, "owner-b");
    EXPECT_THROW((void)ProgramRunRecord::parse(run_bytes), std::invalid_argument);
}

TEST(ProgramTransitionStoreTest, AttemptResumeValueRoundTripsAndLeavesOtherRecordIdsUnchanged) {
    // #377: only a record that carries the value of a resumed attempt uses
    // storage schema 5; every other record keeps its schema and its id.
    const auto start      = start_publication();
    const auto plain      = start.run_record.serialize_canonical();
    const auto has_schema = [](const std::string& bytes, int version) {
        return bytes.find("\"storage_schema_version\":" + std::to_string(version)) !=
               std::string::npos;
    };
    EXPECT_TRUE(has_schema(plain, 3));
    EXPECT_EQ(plain.find("attempt_resume"), std::string::npos);
    EXPECT_EQ(ProgramRunRecord::parse(plain).id(), start.run_record.id());
    EXPECT_FALSE(ProgramRunRecord::parse(plain).attempt_resume());

    auto data             = copy_run_record_data(start.run_record);
    data.exact_checkpoint = checkpoint();
    const auto without    = ProgramRunRecord::create(data);
    data.attempt_resume =
        ProgramAttemptResume{checkpoint().checkpoint_id, json{{"approved", true}}};
    const auto resumed = ProgramRunRecord::create(data);
    const auto bytes   = resumed.serialize_canonical();
    EXPECT_TRUE(has_schema(without.serialize_canonical(), 3));
    EXPECT_TRUE(has_schema(bytes, 5));
    EXPECT_NE(resumed.id(), without.id());
    const auto parsed = ProgramRunRecord::parse(bytes);
    EXPECT_EQ(parsed.id(), resumed.id());
    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    ASSERT_TRUE(parsed.attempt_resume());
    EXPECT_EQ(*parsed.attempt_resume(), *data.attempt_resume);

    // A legacy record cannot carry the value.
    auto legacy = bytes;
    legacy.replace(legacy.find("\"storage_schema_version\":5"), 26,
                   "\"storage_schema_version\":4");
    EXPECT_THROW((void)ProgramRunRecord::parse(legacy), std::invalid_argument);

    // The value binds the running attempt's exact checkpoint.
    auto other_checkpoint = data;
    other_checkpoint.attempt_resume->checkpoint_id = "checkpoint-2";
    EXPECT_THROW((void)ProgramRunRecord::create(other_checkpoint), std::invalid_argument);
    auto no_checkpoint             = data;
    no_checkpoint.exact_checkpoint = std::nullopt;
    EXPECT_THROW((void)ProgramRunRecord::create(no_checkpoint), std::invalid_argument);
}

TEST(ProgramTransitionStoreTest, SettledBranchesRoundTripAndLeaveOtherRecordIdsUnchanged) {
    // #384: only a record that carries the branches a paused plan-level
    // parallel recorded uses storage schema 6; every other record keeps its
    // schema and its id.
    const auto has_schema = [](const std::string& bytes, int version) {
        return bytes.find("\"storage_schema_version\":" + std::to_string(version)) !=
               std::string::npos;
    };
    auto data             = copy_run_record_data(start_publication().run_record);
    data.exact_checkpoint = checkpoint();
    data.attempt_resume   = ProgramAttemptResume{checkpoint().checkpoint_id, json{{"ok", true}}};
    const auto without    = ProgramRunRecord::create(data);
    EXPECT_TRUE(has_schema(without.serialize_canonical(), 5));
    EXPECT_FALSE(without.settled_branches());

    const json branches{{"root.0", json{{"kind", "settled"}, {"returned", false},
                                        {"result", json{{"status", "completed"}}}}}};
    data.settled_branches = branches;
    const auto settled    = ProgramRunRecord::create(data);
    const auto bytes      = settled.serialize_canonical();
    EXPECT_TRUE(has_schema(bytes, 6));
    EXPECT_NE(settled.id(), without.id());
    const auto parsed = ProgramRunRecord::parse(bytes);
    EXPECT_EQ(parsed.id(), settled.id());
    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    ASSERT_TRUE(parsed.settled_branches());
    EXPECT_EQ(*parsed.settled_branches(), branches);

    // A legacy record cannot carry them.
    auto legacy = bytes;
    legacy.replace(legacy.find("\"storage_schema_version\":6"), 26,
                   "\"storage_schema_version\":5");
    EXPECT_THROW((void)ProgramRunRecord::parse(legacy), std::invalid_argument);

    // They belong to an exact checkpoint, and are never empty.
    auto empty             = data;
    empty.settled_branches = json::object();
    EXPECT_THROW((void)ProgramRunRecord::create(empty), std::invalid_argument);
    auto no_checkpoint             = data;
    no_checkpoint.attempt_resume   = std::nullopt;
    no_checkpoint.exact_checkpoint = std::nullopt;
    EXPECT_THROW((void)ProgramRunRecord::create(no_checkpoint), std::invalid_argument);
}

TEST(ProgramTransitionStoreTest, PublicationCanonicalWireMatchesReferenceTree) {
    const auto start       = start_publication();
    const auto publication = interrupted_effect_publication(start);

    EXPECT_EQ(publication.serialize_canonical(),
              detail::canonical_json_bytes(publication_reference_body(publication)));
}

TEST(ProgramTransitionStoreTest, ContextPublicationV4RoundTripsAndReadsLegacyPublications) {
    auto publication = start_publication();
    publication.context_publication = context_publication(1);
    const auto canonical = publication.serialize_canonical();
    const auto parsed = ProgramTransitionPublication::parse(canonical);
    ASSERT_TRUE(parsed.context_publication);
    EXPECT_EQ(parsed.context_publication->epoch.id(), publication.context_publication->epoch.id());
    EXPECT_EQ(parsed.context_publication->assembly_receipt.id(),
              publication.context_publication->assembly_receipt.id());

    const auto legacy_bytes = [](const ProgramTransitionPublication& value,
                                 std::uint32_t schema_version) {
        auto bytes = value.serialize_canonical();
        bytes.replace(bytes.find(",\"context_publication\":null"),
                       std::string(",\"context_publication\":null").size(), "");
        bytes.replace(bytes.find(",\"hook_outbox_entries\":[]"),
                       std::string(",\"hook_outbox_entries\":[]").size(), "");
        bytes.replace(bytes.find("\"storage_schema_version\":5"),
                       std::string("\"storage_schema_version\":5").size(),
                      "\"storage_schema_version\":" + std::to_string(schema_version));
        return bytes;
    };
    const auto parsed_legacy = ProgramTransitionPublication::parse(
        legacy_bytes(start_publication(), 1));
    EXPECT_FALSE(parsed_legacy.context_publication);
    EXPECT_EQ(detail::parse_json_strict(parsed_legacy.serialize_canonical())
                  .at("storage_schema_version"),
              5);

    auto v2 = start_publication();
    attach_initial_lineage(v2);
    EXPECT_FALSE(ProgramTransitionPublication::parse(legacy_bytes(v2, 2)).context_publication);

    auto source = start_publication();
    attach_initial_lineage(source);
    auto interrupted = interrupted_effect_publication(source);
    attach_same_generation_lineage(interrupted, *source.run_lineage);
    auto v3 = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(v3);
    v3.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(v3, *interrupted.run_lineage);
    EXPECT_FALSE(ProgramTransitionPublication::parse(legacy_bytes(v3, 3)).context_publication);
}

TEST_P(ProgramTransitionStoreConformance, ContextPublicationIsAtomicAppendOnlyAndOwnerScoped) {
    auto& store = primary_store();
    auto initial = start_publication();
    initial.context_publication = context_publication(1);
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
              ProgramTransitionPublishResult::Published);
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
              ProgramTransitionPublishResult::AlreadyPresent);

    auto next = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    next.context_publication = context_publication(2, initial.context_publication->epoch.id());
    ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, next),
              ProgramTransitionPublishResult::Published);
    const auto history = store.load_context_publications("owner-a", "run-1");
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1", 1).size(), 1U);
    EXPECT_TRUE(store.load_context_publications("owner-b", "run-1").empty());

    auto out_of_order = javascript_command_publication(next, javascript_command_entry(2, true), 30);
    out_of_order.context_publication = context_publication(4, next.context_publication->epoch.id());
    EXPECT_EQ(store.compare_publish("owner-a", next.journal_record.id, out_of_order),
              ProgramTransitionPublishResult::Conflict);

    auto hook = start_publication_for("run-2", digest('1'), digest('2'), budget(), 10);
    hook.context_publication =
        context_publication(1, std::nullopt, {hook_output_artifact()}, "run-2");
    EXPECT_EQ(store.compare_publish("owner-a", {}, hook), ProgramTransitionPublishResult::Conflict);
}

TEST_P(ProgramTransitionStoreConformance, ContextPublicationVerifiesAClaimToExtendItsPredecessor) {
    auto& store = primary_store();
    const auto skill = context_skill("Verify before editing.");
    auto initial = start_publication();
    initial.context_publication = ordered_context_publication(
        1, std::nullopt, 3, {skill, context_note("reading 1")}, std::nullopt);
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial), ProgramTransitionPublishResult::Published);

    auto next = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    next.context_publication = ordered_context_publication(
        2, initial.context_publication->epoch.id(), 5, {skill, context_note("reading 2")},
        ContextPrefixClaim::Extends);
    ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, next),
              ProgramTransitionPublishResult::Published);

    // A Stable artifact changed, or the window did not grow: the claim does not hold, so nothing is written.
    const auto head = next.context_publication->epoch.id();
    auto changed = javascript_command_publication(next, javascript_command_entry(2, true), 30);
    changed.context_publication = ordered_context_publication(
        3, head, 7, {context_skill("Verify twice."), context_note("reading 3")}, ContextPrefixClaim::Extends);
    EXPECT_EQ(store.compare_publish("owner-a", next.journal_record.id, changed),
              ProgramTransitionPublishResult::Conflict);
    auto stalled = javascript_command_publication(next, javascript_command_entry(2, true), 30);
    stalled.context_publication = ordered_context_publication(
        3, head, 5, {skill, context_note("reading 3")}, ContextPrefixClaim::Extends);
    EXPECT_EQ(store.compare_publish("owner-a", next.journal_record.id, stalled),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").size(), 2U);

    // The same change is a publishable break when the epoch says so, or claims nothing.
    auto unclaimed = javascript_command_publication(next, javascript_command_entry(2, true), 30);
    unclaimed.context_publication = ordered_context_publication(
        3, head, 7, {context_skill("Verify twice."), context_note("reading 3")}, std::nullopt);
    ASSERT_EQ(store.compare_publish("owner-a", next.journal_record.id, unclaimed),
              ProgramTransitionPublishResult::Published);
    const auto history = store.load_context_publications("owner-a", "run-1");
    ASSERT_EQ(history.size(), 3U);
    ASSERT_TRUE(history[1].epoch.prefix_claim());
    EXPECT_EQ(*history[1].epoch.prefix_claim(), ContextPrefixClaim::Extends);
    EXPECT_EQ(history[1].epoch.ordered_artifact_ids(), next.context_publication->epoch.ordered_artifact_ids());
    ASSERT_TRUE(history[1].assembly_receipt.assembled_artifact_ids());
    EXPECT_EQ(history[1].assembly_receipt.encoder_family(), std::optional<std::string>("openai.chat"));
}

TEST_P(ProgramTransitionStoreConformance, ContextPublicationRequiresALayoutChangeToBeDeclared) {
    auto& store = primary_store();
    const auto skill = context_skill("Verify before editing.");
    auto initial = start_publication();
    initial.context_publication = ordered_context_publication(1, std::nullopt, 3, {skill}, std::nullopt);
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial), ProgramTransitionPublishResult::Published);
    const auto head = initial.context_publication->epoch.id();
    const auto stable_first = neograph::AssemblyLayout::StableFirst;

    auto undeclared = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    undeclared.context_publication = ordered_context_publication(2, head, 5, {skill}, std::nullopt, "run-1", stable_first);
    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id, undeclared),
              ProgramTransitionPublishResult::Conflict);
    auto extending = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    extending.context_publication = ordered_context_publication(
        2, head, 5, {skill}, ContextPrefixClaim::Extends, "run-1", stable_first);
    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id, extending),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").size(), 1U);

    auto declared = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    declared.context_publication = ordered_context_publication(
        2, head, 5, {skill}, ContextPrefixClaim::Breaks, "run-1", stable_first,
        neograph::ContextBreakReason::LayoutChange);
    ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, declared),
              ProgramTransitionPublishResult::Published);
    const auto history = store.load_context_publications("owner-a", "run-1");
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history[1].epoch.assembly_layout(), stable_first);
    EXPECT_EQ(history[1].assembly_receipt.break_reason(), std::optional<neograph::ContextBreakReason>(
                                                              neograph::ContextBreakReason::LayoutChange));
}

TEST_P(ProgramTransitionStoreConformance, ForkedContextKeepsTheOrderAndTheReceiptPrefixFields) {
    auto& store = primary_store();
    const auto source_context = ordered_context_publication(
        1, std::nullopt, 3, {context_skill("Verify before editing."), context_note("reading 1")},
        std::nullopt, "run-1", neograph::AssemblyLayout::StableFirst);
    auto source_start = start_publication();
    source_start.context_publication = source_context;
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start), ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto base = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(base);
    base.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    // A copy that lost the order, or the prefix fields of the receipt, is not the source's context.
    const auto copy = [&](bool keep_order, bool keep_prefix_fields, bool keep_layout = true, bool keep_rule = true) {
        ContextEpochData data{"run-2", 1, std::nullopt, source_context.epoch.feed_id(),
                              source_context.epoch.raw_from_sequence(),
                              source_context.epoch.raw_through_sequence(),
                              source_context.epoch.raw_window_digest(), source_context.epoch.artifact_ids(),
                              source_context.epoch.guarantee_profile()};
        if (keep_order) data.ordered_artifact_ids = source_context.epoch.ordered_artifact_ids();
        if (keep_order && keep_layout) data.assembly_layout = source_context.epoch.assembly_layout();
        auto epoch = ContextEpoch::create(std::move(data));
        const auto& source_receipt = source_context.assembly_receipt;
        ContextAssemblyReceiptData receipt_data{
            epoch.id(), source_receipt.normalized_request_digest(), source_receipt.message_window_digest(),
            source_receipt.artifact_ids(), source_receipt.required_skill_artifact_ids(),
            source_receipt.raw_from_sequence(), source_receipt.raw_through_sequence(),
            source_receipt.estimated_input_tokens(), source_receipt.mandatory_input_tokens()};
        if (keep_prefix_fields) {
            receipt_data.assembled_artifact_ids = source_receipt.assembled_artifact_ids();
            receipt_data.artifact_classes = source_receipt.artifact_classes();
            receipt_data.class_sources = source_receipt.class_sources();
            receipt_data.class_rule_version = source_receipt.class_rule_version();
            // Another rule version is a valid receipt of its own, and it is not the source's.
            if (!keep_rule) receipt_data.class_rule_version = *source_receipt.class_rule_version() + 1;
            receipt_data.encoder_family = source_receipt.encoder_family();
        }
        auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, source_context.artifacts);
        return ProgramContextPublication{std::move(epoch), source_context.artifacts, std::move(receipt)};
    };
    for (const auto& [keep_order, keep_prefix_fields, keep_layout, keep_rule] :
         std::vector<std::tuple<bool, bool, bool, bool>>{{false, true, true, true}, {true, false, true, true},
                                                         {false, false, true, true}, {true, true, false, true},
                                                         {true, true, true, false}}) {
        auto dropped = base;
        dropped.context_publication = copy(keep_order, keep_prefix_fields, keep_layout, keep_rule);
        attach_fork_split(dropped, *source.run_lineage);
        EXPECT_EQ(store.compare_publish("owner-a", {}, dropped), ProgramTransitionPublishResult::Conflict)
            << keep_order << keep_prefix_fields << keep_layout << keep_rule;
    }

    // The first epoch of the target has no predecessor, so it claims nothing about a prefix.
    {
        auto claiming = copy(true, true);
        ContextEpochData data{"run-2", 1, std::nullopt, claiming.epoch.feed_id(),
                              claiming.epoch.raw_from_sequence(), claiming.epoch.raw_through_sequence(),
                              claiming.epoch.raw_window_digest(), claiming.epoch.artifact_ids(),
                              claiming.epoch.guarantee_profile()};
        data.ordered_artifact_ids = claiming.epoch.ordered_artifact_ids();
        data.prefix_claim = ContextPrefixClaim::Breaks;
        data.break_reason = neograph::ContextBreakReason::Explicit;
        auto epoch = ContextEpoch::create(std::move(data));
        const auto& copied = claiming.assembly_receipt;
        ContextAssemblyReceiptData receipt_data{
            epoch.id(), copied.normalized_request_digest(), copied.message_window_digest(),
            copied.artifact_ids(), copied.required_skill_artifact_ids(), copied.raw_from_sequence(),
            copied.raw_through_sequence(), copied.estimated_input_tokens(), copied.mandatory_input_tokens()};
        receipt_data.assembled_artifact_ids = copied.assembled_artifact_ids();
        receipt_data.artifact_classes = copied.artifact_classes();
        receipt_data.class_sources = copied.class_sources();
        receipt_data.class_rule_version = copied.class_rule_version();
        receipt_data.encoder_family = copied.encoder_family();
        receipt_data.break_reason = neograph::ContextBreakReason::Explicit;
        auto receipt = ContextAssemblyReceipt::create(std::move(receipt_data), epoch, claiming.artifacts);
        auto target = base;
        target.context_publication =
            ProgramContextPublication{std::move(epoch), claiming.artifacts, std::move(receipt)};
        attach_fork_split(target, *source.run_lineage);
        EXPECT_EQ(store.compare_publish("owner-a", {}, target), ProgramTransitionPublishResult::Conflict);
    }

    auto kept = base;
    kept.context_publication = transferred_context_publication(source_context, "run-2");
    attach_fork_split(kept, *source.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", {}, kept), ProgramTransitionPublishResult::Published);
    const auto contexts = store.load_context_publications("owner-a", "run-2");
    ASSERT_EQ(contexts.size(), 1U);
    EXPECT_EQ(contexts.front().epoch.ordered_artifact_ids(), source_context.epoch.ordered_artifact_ids());
    EXPECT_EQ(contexts.front().epoch.assembly_layout(), neograph::AssemblyLayout::StableFirst);
    EXPECT_EQ(contexts.front().assembly_receipt.artifact_classes(), source_context.assembly_receipt.artifact_classes());
}

TEST(ProgramTransitionStoreTest, ContextPublicationFaultLeavesNoPartialSnapshotAndRetries) {
    OwnedTransitionStore<InMemoryProgramTransitionStore> store;
    auto publication = start_publication();
    publication.context_publication = context_publication(1);
    store.store().fail_next_publication_for_testing(ProgramTransitionFaultPoint::AfterContextSnapshot);
    EXPECT_THROW((void)store.compare_publish("owner-a", {}, publication), std::runtime_error);
    EXPECT_FALSE(store.load("owner-a", "run-1"));
    EXPECT_TRUE(store.load_context_publications("owner-a", "run-1").empty());
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").size(), 1U);
}

// #391: when each await of a command started, in a pending entry of its
// coordinate. Other entries keep schema 1 and their ids.
ProgramJavaScriptCommandJournalEntry await_start_entry(std::uint64_t       sequence,
                                                       std::optional<json> terminal,
                                                       std::optional<json> starts) {
    auto data = ProgramJavaScriptCommandJournalEntryData{
        sequence, digest('2'), 1,
        JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}), digest('8'),
        std::move(terminal)};
    data.await_started_at = std::move(starts);
    return ProgramJavaScriptCommandJournalEntry(std::move(data));
}

std::uint64_t stored_schema(const ProgramJavaScriptCommandJournalEntry& entry) {
    return detail::parse_json_strict(entry.serialize_canonical())
        .at("storage_schema_version")
        .get<std::uint64_t>();
}

void exercise_await_start_history(BlockingProgramTransitionStore& store) {
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    // A head never carries await starts.
    const auto head_with_starts = javascript_command_publication(
        start, await_start_entry(1, std::nullopt, json{{"root.javascript.1", 7}}), 20);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, head_with_starts),
              ProgramTransitionPublishResult::Conflict);
    const auto head =
        javascript_command_publication(start, javascript_command_entry(1, false), 20);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, head),
              ProgramTransitionPublishResult::Published);
    const auto started = javascript_command_publication(
        head, await_start_entry(2, std::nullopt, json{{"root.javascript.1", 1'700'000'000'000ULL}}),
        25);
    ASSERT_EQ(store.compare_publish("owner-a", head.journal_record.id, started),
              ProgramTransitionPublishResult::Published);
    const auto completed =
        javascript_command_publication(started, javascript_command_entry(3, true), 30);
    ASSERT_EQ(store.compare_publish("owner-a", started.journal_record.id, completed),
              ProgramTransitionPublishResult::Published);
    const auto commands = store.load_javascript_commands("owner-a", "run-1", 0);
    ASSERT_EQ(commands.size(), 3U);
    ASSERT_TRUE(commands[1].await_started_at());
    EXPECT_EQ(*commands[1].await_started_at(), (json{{"root.javascript.1", 1'700'000'000'000ULL}}));
    EXPECT_FALSE(commands[0].await_started_at());
    EXPECT_EQ(commands[0].coordinate_id(), commands[1].coordinate_id());
}

TEST(ProgramTransitionStoreTest, CommandJournalAwaitStartsUseSchemaThreeAndRoundTrip) {
    const auto plain = javascript_command_entry(1, false);
    EXPECT_EQ(stored_schema(plain), 1U) << "an entry without await starts keeps its bytes and id";
    EXPECT_EQ(ProgramJavaScriptCommandJournalEntry::parse(plain.serialize_canonical()).id(),
              plain.id());

    const auto starts  = json{{"root.javascript.1", 1'700'000'000'000ULL},
                              {"root.javascript.1/member/0", 1'700'000'000'250ULL}};
    const auto started = await_start_entry(2, std::nullopt, starts);
    EXPECT_EQ(stored_schema(started), 3U);
    const auto parsed = ProgramJavaScriptCommandJournalEntry::parse(started.serialize_canonical());
    EXPECT_EQ(parsed.id(), started.id());
    ASSERT_TRUE(parsed.await_started_at());
    EXPECT_EQ(*parsed.await_started_at(), starts);
    EXPECT_EQ(parsed.coordinate_id(), plain.coordinate_id()) << "the coordinate is unchanged";

    EXPECT_THROW((void)await_start_entry(4, json{{"status", "completed"}}, starts),
                 std::invalid_argument)
        << "a terminal result carries no await starts";
    EXPECT_THROW((void)await_start_entry(4, std::nullopt, json::object()), std::invalid_argument);
    EXPECT_THROW((void)await_start_entry(4, std::nullopt, json::array({1})), std::invalid_argument);
    EXPECT_THROW((void)await_start_entry(4, std::nullopt, json{{"root.javascript.1", -1}}),
                 std::invalid_argument);
    EXPECT_THROW((void)await_start_entry(4, std::nullopt, json{{"root.javascript.1", "now"}}),
                 std::invalid_argument);

    // The version and the field agree.
    auto tampered = detail::parse_json_strict(started.serialize_canonical());
    tampered["storage_schema_version"] = 2;
    EXPECT_THROW((void)ProgramJavaScriptCommandJournalEntry::parse(
                     detail::canonical_json_bytes(tampered)),
                 std::invalid_argument);
    const auto full    = detail::parse_json_strict(started.serialize_canonical());
    auto       missing = json::object();
    for (const auto& [key, value] : full.items())
        if (key != "await_started_at") missing[key] = value;
    EXPECT_THROW((void)ProgramJavaScriptCommandJournalEntry::parse(
                     detail::canonical_json_bytes(missing)),
                 std::invalid_argument);
}

TEST_P(ProgramTransitionStoreConformance, AcceptsAnAwaitStartBeforeTheCommandResult) {
    exercise_await_start_history(primary_store());
}

TEST_P(ProgramTransitionStoreConformance, JavaScriptCommandHistoryIsAppendOnly) {
    exercise_javascript_command_history(primary_store());
}
TEST(ProgramTransitionStoreTest, CommandPublicationHeadSupportsLegacyWrappers) {
    LegacyCommandHeadStore             legacy;
    BlockingProgramTransitionStore     store(legacy);
    exercise_javascript_command_history(store);
}
TEST(ProgramTransitionStoreTest, CommandPublicationHeadRejectsTornLegacyReads) {
    LegacyCommandHeadStore         legacy;
    BlockingProgramTransitionStore store(legacy);
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start), ProgramTransitionPublishResult::Published);
    const auto pending = javascript_command_publication(start, javascript_command_entry(1, false), 20);
    legacy.after_command_read = [&] {
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
                  ProgramTransitionPublishResult::Published);
    };
    EXPECT_FALSE(store.load_command_publication_head("owner-a", "run-1"));
    const auto fresh = store.load_command_publication_head("owner-a", "run-1");
    ASSERT_TRUE(fresh && fresh->latest_command);
    EXPECT_EQ(fresh->run_record.id(), pending.run_record.id());
}
// A second connection reads the head while the first writes: every read is one committed snapshot.
TEST_P(ProgramTransitionStoreConformance, CommandPublicationHeadIsCoherentDuringWrites) {
    const auto reader = second_store();
    exercise_command_head_concurrency(primary_store(), *reader);
}
TEST_P(ProgramTransitionStoreConformance, CommandSettlementRefundIsUsageBounded) {
    exercise_javascript_command_reservation_settlement(primary_store());
}
TEST_P(ProgramTransitionStoreConformance, CommandlessRunningCheckpointRequiresHostEvidence) {
    exercise_commandless_running_checkpoint_requires_host_evidence(primary_store());
}
TEST_P(ProgramTransitionStoreConformance, ExecutionLeaseFencesOrdinaryPublication) {
    exercise_execution_lease_fences_ordinary_publication(primary_store());
}

TEST_P(ProgramTransitionStoreConformance, InitialPublicationCannotHoldAReservation) {
    exercise_initial_reservation_is_rejected(primary_store());
}
TEST_P(ProgramTransitionStoreConformance, ReservationIsRefusedForASiblingCommandCoordinate) {
    exercise_cross_thread_call_core_settlement(primary_store(), false);
}
TEST_P(ProgramTransitionStoreConformance, ReservationIsBoundToItsExactCommandCoordinate) {
    exercise_cross_thread_call_core_settlement(primary_store(), true);
}

TEST_P(ProgramTransitionStoreConformance, FirstPublishRetryAndOwnerIsolation) {
    auto& store = primary_store();
    auto                           publication = start_publication();
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::AlreadyPresent);
    ASSERT_TRUE(store.load("owner-a", "run-1"));
    EXPECT_FALSE(store.load("owner-b", "run-1"));
    EXPECT_FALSE(store.latest("owner-b", "run-1"));
    EXPECT_TRUE(store.load_events("owner-b", "run-1").empty());
}
// The awaitable operations, called from a coroutine on the host's executor, give the answers the blocking form gives.
// (The Awaited form of every case in this suite already calls them; this one calls them from a coroutine.)
TEST_P(ProgramTransitionStoreConformance, AnAwaitingCoroutineSeesTheAnswersOfTheBlockingForm) {
    if (GetParam().form == neograph::program::test_support::StoreForm::Awaited)
        GTEST_SKIP() << "every call of the Awaited form is an awaitable call";
    const auto raw   = primary_store().store();
    const auto start = start_publication();
    const auto terminal =
        terminal_publication(start, ProgramTerminalStatus::Completed, ContinuationState::Completed);

    struct Observed {
        ProgramTransitionPublishResult first   = ProgramTransitionPublishResult::Conflict;
        ProgramTransitionPublishResult retry   = ProgramTransitionPublishResult::Conflict;
        ProgramTransitionPublishResult stale   = ProgramTransitionPublishResult::Published;
        ProgramTransitionPublishResult last    = ProgramTransitionPublishResult::Conflict;
        std::optional<ProgramRunRecord>     loaded;
        std::optional<ProgramRunRecord>     foreign;
        std::optional<ProgramJournalRecord> journal;
        std::size_t                         events = 0;
    } seen;
    std::exception_ptr failure;
    asio::io_context   io;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            seen.first   = co_await raw->compare_publish("owner-a", {}, start);
            seen.retry   = co_await raw->compare_publish("owner-a", {}, start);
            seen.stale   = co_await raw->compare_publish("owner-a", "not-the-head", terminal);
            seen.last    = co_await raw->compare_publish("owner-a", start.journal_record.id, terminal);
            seen.loaded  = co_await raw->load("owner-a", "run-1");
            seen.foreign = co_await raw->load("owner-b", "run-1");
            seen.journal = co_await raw->latest("owner-a", "run-1");
            seen.events  = (co_await raw->load_events("owner-a", "run-1")).size();
        },
        [&](std::exception_ptr error) { failure = error; });
    io.run();
    if (failure) std::rethrow_exception(failure);

    EXPECT_EQ(seen.first, ProgramTransitionPublishResult::Published);
    EXPECT_EQ(seen.retry, ProgramTransitionPublishResult::AlreadyPresent);
    EXPECT_EQ(seen.stale, ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(seen.last, ProgramTransitionPublishResult::Published);
    EXPECT_FALSE(seen.foreign);
    ASSERT_TRUE(seen.loaded);
    ASSERT_TRUE(seen.journal);
    // What the coroutine read is what a blocking read gives afterwards.
    const auto blocking = primary_store().load("owner-a", "run-1");
    ASSERT_TRUE(blocking);
    EXPECT_EQ(seen.loaded->id(), blocking->id());
    EXPECT_EQ(seen.journal->id, primary_store().latest("owner-a", "run-1")->id);
    EXPECT_EQ(seen.events, primary_store().load_events("owner-a", "run-1").size());
}

TEST_P(ProgramTransitionStoreConformance, MigrationPublicationIsDurableAndInheritedAcrossReplay) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto publication =
        start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(publication, *source.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    const auto stored = store.load_migration_plan("owner-a", "run-2");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->id(), publication.migration_plan->id());

    const auto bytes    = publication.serialize_canonical();
    const auto reparsed = ProgramTransitionPublication::parse(bytes);
    ASSERT_TRUE(reparsed.migration_plan.has_value());
    EXPECT_EQ(reparsed.migration_plan->id(), publication.migration_plan->id());
}

TEST_P(ProgramTransitionStoreConformance, ForkAtomicallyClonesContextButNotSourceHookOwnership) {
    auto& store = primary_store();
    const auto source_context = context_publication(1);
    auto source_start = start_publication();
    source_start.context_publication = source_context;
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto omitted = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(omitted);
    omitted.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(omitted, *source.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", {}, omitted),
              ProgramTransitionPublishResult::Conflict);

    auto target = omitted;
    target.context_publication = transferred_context_publication(source_context, "run-2");
    attach_fork_split(target, *source.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Published);
    const auto contexts = store.load_context_publications("owner-a", "run-2");
    ASSERT_EQ(contexts.size(), 1U);
    EXPECT_EQ(contexts.front().epoch.run_id(), "run-2");
    EXPECT_TRUE(store.load_hook_outbox_entries("owner-a", "run-2").empty());
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationRequiresDurableMigrationProof) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);
    auto publication =
        start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(publication);
    attach_fork_split(publication, *source.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2").has_value());
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationRejectsMismatchedInitialResumeBinding) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(target, std::string("wrong-pending"), json{{"result", "recorded"}});
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationCannotDropSourcePendingEffect) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(target, std::nullopt, json(), false);
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationRejectsPendingExpiredAtDurableTargetTime) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 6000);
    attach_fork_receipt(target);
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
}

TEST_P(ProgramTransitionStoreConformance, FreshForkPublicationRejectsLegacyReceiptDowngrade) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(target, std::string("effect-call"), json{{"result", "recorded"}}, true,
                        true);
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
}

// #414: a fork's max_concurrency is its own limit. A store accepts a fork that
// leaves the source's concurrency as it was, and refuses the debit that took it.
template <typename Store>
void exercise_fork_leaves_concurrency_alone(Store& store) {
    auto source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);
    ASSERT_GT(source.run_lineage->remaining_budget().max_concurrency, 0U);

    auto debited = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(debited);
    debited.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(debited, *source.run_lineage, /*debit_concurrency=*/true);
    EXPECT_EQ(store.compare_publish("owner-a", {}, debited),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));

    auto limit = start_publication_for("run-3", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(limit);
    limit.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(limit, *source.run_lineage);
    ASSERT_EQ(limit.fork_source_lineage->remaining_budget().max_concurrency,
              source.run_lineage->remaining_budget().max_concurrency);
    EXPECT_EQ(store.compare_publish("owner-a", {}, limit),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.load_run_lineage("owner-a", "run-1")->remaining_budget().max_concurrency,
              source.run_lineage->remaining_budget().max_concurrency);
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationLeavesTheSourcesConcurrencyAlone) {
    exercise_fork_leaves_concurrency_alone(primary_store());
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationRequiresAtomicSourceDebit) {
    auto& store = primary_store();
    auto source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(target);
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);
    target.fork_source_lineage.reset();
    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_run_lineage("owner-a", "run-1")->id(), source.run_lineage->id());

    attach_fork_split(target, *source.run_lineage);
    target.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        source.run_lineage->owner_scope(), source.run_lineage->lineage_id(),
        source.run_lineage->root_run_id(), source.run_lineage->active_generation(),
        source.run_lineage->active_generation_id(),
        source.run_lineage->active_run_record_id(), source.run_lineage->active_journal_head(),
        source.run_lineage->remaining_budget(), source.run_lineage->inflight_reservation(),
        source.run_lineage->id(), source.run_lineage->created_at_ms(), 30,
        source.run_lineage->committed_descendant_budget()});
    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_run_lineage("owner-a", "run-1")->id(), source.run_lineage->id());

    auto partial_budget         = budget();
    partial_budget.wall_time_ms = 500;
    auto over_debited =
        start_publication_for("run-3", digest('1'), digest('2'), partial_budget, 30);
    attach_fork_receipt(over_debited);
    over_debited.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(over_debited, *source.run_lineage);
    over_debited.fork_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        source.run_lineage->owner_scope(), source.run_lineage->lineage_id(),
        source.run_lineage->root_run_id(), source.run_lineage->active_generation(),
        source.run_lineage->active_generation_id(),
        source.run_lineage->active_run_record_id(), source.run_lineage->active_journal_head(),
        RunBudget{}, source.run_lineage->inflight_reservation(), source.run_lineage->id(),
        source.run_lineage->created_at_ms(), 30,
        source.run_lineage->committed_descendant_budget()});
    EXPECT_EQ(store.compare_publish("owner-a", {}, over_debited),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-3"));
    EXPECT_EQ(store.load_run_lineage("owner-a", "run-1")->id(), source.run_lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, ForkPublicationRequiresAnInterruptedSource) {
    auto& running_store = primary_store();
    auto running_source = start_publication();
    attach_initial_lineage(running_source);
    ASSERT_EQ(running_store.compare_publish("owner-a", {}, running_source),
              ProgramTransitionPublishResult::Published);
    auto running_fork =
        start_publication_for("run-4", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(running_fork);
    running_fork.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(running_fork, *running_source.run_lineage);
    EXPECT_EQ(running_store.compare_publish("owner-a", {}, running_fork),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(running_store.load("owner-a", "run-4"));
}

TEST_P(ProgramTransitionStoreConformance, NonForkMigrationPlanCannotPublishForkLineage) {
    auto& store = primary_store();
    auto                           source_start = start_publication();
    attach_initial_lineage(source_start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
              ProgramTransitionPublishResult::Published);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
              ProgramTransitionPublishResult::Published);
    auto publication =
        start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(
        MigrationPlanData{digest('1'),
                          digest('1'),
                          "owner-a",
                          MigrationCompatibility::Blocked,
                          {"authority_profile"},
                          {MigrationDiagnostic{MigrationDimension::Authority, "authority_profile",
                                               "authority changed", json{{"profile", "source"}},
                           json{{"profile", "target"}}}},
                          {}});
    attach_fork_split(publication, *source.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", {}, std::move(publication)),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2").has_value());
}

TEST_P(ProgramTransitionStoreConformance, EffectOutboxMustBindExactAwaitingPendingEffect) {
    auto& store = primary_store();
    const auto                     start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto interrupted    = interrupted_effect_publication(start);
    interrupted.effects = {ProgramEffectOutboxEntry(1, pending_effect("different-effect"))};
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, start.journal_record.id);
    EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());
}

TEST_P(ProgramTransitionStoreConformance, ConflictingCasAndInvalidPublicationRollBack) {
    auto& store = primary_store();
    auto                           start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal =
        terminal_publication(start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
    EXPECT_EQ(store.compare_publish("owner-a", digest('9'), terminal),
              ProgramTransitionPublishResult::Conflict);

    auto bad_event                    = terminal;
    bad_event.events.front().sequence = 7;
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, bad_event),
              ProgramTransitionPublishResult::Conflict);
    auto latest = store.latest("owner-a", "run-1");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, start.journal_record.id);
    EXPECT_FALSE(store.load("owner-a", "run-1")->terminal_result());
}

TEST_P(ProgramTransitionStoreConformance, ExactTerminalRetryIsAlreadyPresent) {
    auto& store = primary_store();
    auto                           start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal =
        terminal_publication(start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::AlreadyPresent);
}

TEST_P(ProgramTransitionStoreConformance, HistoryRetainsOrderedEventAndEffectHistory) {
    auto& store = primary_store();
    const auto                     start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    const auto interrupted = interrupted_effect_publication(start);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Published);

    const auto resumed = resumed_effect_publication(interrupted);
    ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
              ProgramTransitionPublishResult::Published);

    const auto events = store.load_events("owner-a", "run-1");
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].sequence, 1U);
    EXPECT_EQ(events[1].sequence, 2U);
    EXPECT_EQ(events[2].sequence, 3U);

    const auto after_first = store.load_events("owner-a", "run-1", 1);
    ASSERT_EQ(after_first.size(), 2U);
    EXPECT_EQ(after_first.front().sequence, 2U);
    EXPECT_EQ(after_first.back().sequence, 3U);

    const auto effects = store.load_effects("owner-a", "run-1");
    ASSERT_EQ(effects.size(), 1U);
    EXPECT_EQ(effects.front().sequence(), 1U);
    EXPECT_TRUE(store.load_effects("owner-a", "run-1", 1).empty());
}

TEST(ProgramTransitionStoreTest, FaultAtEachPublishBoundaryPreservesCommittedState) {
    const std::array faults{
        ProgramTransitionFaultPoint::AfterRunSnapshot,
        ProgramTransitionFaultPoint::AfterJournalSnapshot,
        ProgramTransitionFaultPoint::AfterEventSnapshot,
        ProgramTransitionFaultPoint::AfterEffectSnapshot,
        ProgramTransitionFaultPoint::AfterLineageSnapshot,
        ProgramTransitionFaultPoint::BeforeCommit,
    };

    for (const auto fault : faults) {
        OwnedTransitionStore<InMemoryProgramTransitionStore> store;
        auto                           start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed);
        store.store().fail_next_publication_for_testing(fault);
        EXPECT_THROW(store.compare_publish("owner-a", start.journal_record.id, std::move(terminal)),
                     std::runtime_error);

        const auto latest = store.latest("owner-a", "run-1");
        ASSERT_TRUE(latest);
        EXPECT_EQ(latest->id, start.journal_record.id);

        EXPECT_FALSE(store.load("owner-a", "run-1")->terminal_result());
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1u);
        EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());

        EXPECT_EQ(
            store.compare_publish("owner-a", start.journal_record.id,
                                  terminal_publication(start, ProgramTerminalStatus::Completed,
                                                       ContinuationState::Completed)),
            ProgramTransitionPublishResult::Published);
    }
}
TEST(ProgramTransitionStoreTest, EffectPublicationFaultsPreserveSourceAndOutbox) {
    const std::array faults{
        ProgramTransitionFaultPoint::AfterRunSnapshot,
        ProgramTransitionFaultPoint::AfterJournalSnapshot,
        ProgramTransitionFaultPoint::AfterEventSnapshot,
        ProgramTransitionFaultPoint::AfterEffectSnapshot,
        ProgramTransitionFaultPoint::AfterLineageSnapshot,
        ProgramTransitionFaultPoint::BeforeCommit,
    };
    for (const auto fault : faults) {
        OwnedTransitionStore<InMemoryProgramTransitionStore> store;
        const auto                     start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto interrupted = interrupted_effect_publication(start);
        store.store().fail_next_publication_for_testing(fault);
        EXPECT_THROW(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                     std::runtime_error);

        const auto source = store.load("owner-a", "run-1");
        ASSERT_TRUE(source.has_value());
        EXPECT_EQ(source->serialize_canonical(), start.run_record.serialize_canonical());
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, start.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
        EXPECT_TRUE(store.load_effects("owner-a", "run-1").empty());

        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
}

// Two writers race for the same head: one publishes and the other is refused.
TEST_P(ProgramTransitionStoreConformance, ConcurrentCasHasOneWinner) {
    auto& store = primary_store();
    const auto other = second_store();
    auto                           start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto completed = terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed, 20);
    auto failed =
        terminal_publication(start, ProgramTerminalStatus::Failed, ContinuationState::Failed, 21);
#if NEOGRAPH_BUILD_THREADS
    std::barrier ready(3);
    auto         publish = [&](BlockingProgramTransitionStore& writer, ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return writer.compare_publish("owner-a", start.journal_record.id, std::move(value));
    };
    auto a = std::async(std::launch::async, [&] { return publish(store, completed); });
    auto b = std::async(std::launch::async, [&] { return publish(*other, failed); });
    ready.arrive_and_wait();
    auto ar = a.get();
    auto br = b.get();
#else
    // A build without threads has one thread: the two writers take turns, and the second one finds the head moved.
    auto ar = store.compare_publish("owner-a", start.journal_record.id, completed);
    auto br = other->compare_publish("owner-a", start.journal_record.id, failed);
#endif
    EXPECT_NE(ar, br);
    EXPECT_TRUE((ar == ProgramTransitionPublishResult::Published &&
                 br == ProgramTransitionPublishResult::Conflict) ||
                (br == ProgramTransitionPublishResult::Published &&
                 ar == ProgramTransitionPublishResult::Conflict));
    const auto run = store.load("owner-a", "run-1");
    ASSERT_TRUE(run && run->terminal_result());
    EXPECT_EQ(run->terminal_result()->status(),
              ar == ProgramTransitionPublishResult::Published ? ProgramTerminalStatus::Completed
                                                              : ProgramTerminalStatus::Failed);
}

TEST_P(ProgramTransitionStoreConformance, LineageAwarePublicationRoundTripsAndLoadsGeneration) {
    auto& store = primary_store();
    auto publication = start_publication();
    attach_initial_lineage(publication);

    const auto parsed = ProgramTransitionPublication::parse(publication.serialize_canonical());
    ASSERT_TRUE(parsed.run_generation.has_value());
    ASSERT_TRUE(parsed.run_lineage.has_value());
    EXPECT_EQ(parsed.run_generation->id(), publication.run_generation->id());
    EXPECT_EQ(parsed.run_lineage->id(), publication.run_lineage->id());

    ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    const auto lineage = store.load_lineage("owner-a", publication.run_lineage->lineage_id());
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->id(), publication.run_lineage->id());
    EXPECT_EQ(store.load_run_lineage("owner-a", publication.run_record.run_id())->id(),
              lineage->id());
    EXPECT_FALSE(store.load_run_lineage("owner-b", publication.run_record.run_id()));
    const auto generation = store.load_generation("owner-a", lineage->lineage_id(), 1);
    ASSERT_TRUE(generation.has_value());
    EXPECT_EQ(generation->id(), publication.run_generation->id());
    EXPECT_FALSE(store.load_lineage("owner-b", lineage->lineage_id()).has_value());
}

TEST_P(ProgramTransitionStoreConformance, LineageAwareRunCannotBypassHeadAccounting) {
    auto& store = primary_store();
    auto start = start_publication();
    attach_initial_lineage(start);
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Conflict);

    attach_same_generation_lineage(terminal, *start.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.load_lineage("owner-a", start.run_lineage->lineage_id())->id(),
              terminal.run_lineage->id());
    EXPECT_EQ(store.load_lineage_head("owner-a", start.run_lineage->lineage_id(),
                                      start.run_lineage->id())
                  ->id(),
              start.run_lineage->id());
    EXPECT_EQ(store.load_lineage_head("owner-a", start.run_lineage->lineage_id(),
                                      terminal.run_lineage->id())
                  ->id(),
              terminal.run_lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, LineageGenerationRejectsReceiptlessSuccessors) {
    auto& store = primary_store();
    auto source = start_publication();
    attach_initial_lineage(source);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source),
              ProgramTransitionPublishResult::Published);

    auto first = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
    auto second = start_publication_for("run-3", digest('9'), digest('a'), budget(), 20);
    attach_successor_lineage(first, *source.run_lineage);
    attach_successor_lineage(second, *source.run_lineage);

    EXPECT_EQ(store.compare_publish("owner-a", {}, first),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.compare_publish("owner-a", {}, second),
              ProgramTransitionPublishResult::Conflict);

    const auto lineage = store.load_lineage("owner-a", source.run_lineage->lineage_id());
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->id(), source.run_lineage->id());
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_EQ(lineage->active_generation_id(), source.run_generation->id());
    ASSERT_TRUE(store.load_generation("owner-a", lineage->lineage_id(), 1).has_value());
    EXPECT_FALSE(store.load_generation("owner-a", lineage->lineage_id(), 2).has_value());
    EXPECT_FALSE(store.load("owner-a", "run-2").has_value());
    EXPECT_FALSE(store.load("owner-a", "run-3").has_value());
}

// Two writers race to replace the same generation: exactly one successor is published.
TEST_P(ProgramTransitionStoreConformance, ExactCheckpointReplacementPublishesOneSuccessor) {
    auto& store = primary_store();
    const auto other = second_store();
    const auto boundary = publish_replacement_boundary(store);
    const auto lineage = store.load_run_lineage("owner-a", boundary.publication.run_record.run_id());
    ASSERT_TRUE(lineage);
    const auto predecessor =
        store.load_generation("owner-a", lineage->lineage_id(), lineage->active_generation());
    ASSERT_TRUE(predecessor);

    const auto input = json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                            {"previous_run_id", "run-1"}};
    const auto successor_budget = program_replacement_remaining_budget(
        boundary.publication.run_record, *lineage, 40);
    auto first =
        start_publication_for("run-2", digest('7'), digest('8'), successor_budget, 40, input);
    auto second =
        start_publication_for("run-3", digest('9'), digest('a'), successor_budget, 40, input);
    attach_replacement_successor(first, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);
    attach_replacement_successor(second, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);

#if NEOGRAPH_BUILD_THREADS
    std::barrier ready(3);
    auto publish = [&](BlockingProgramTransitionStore& writer, ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return writer.compare_publish("owner-a", {}, std::move(value));
    };
    auto first_result = std::async(std::launch::async, [&] { return publish(store, first); });
    auto second_result = std::async(std::launch::async, [&] { return publish(*other, second); });
    ready.arrive_and_wait();
    const auto a = first_result.get();
    const auto b = second_result.get();
#else
    // A build without threads has one thread: the two writers take turns, and the second one finds the lineage moved.
    const auto a = store.compare_publish("owner-a", {}, first);
    const auto b = other->compare_publish("owner-a", {}, second);
#endif
    EXPECT_TRUE((a == ProgramTransitionPublishResult::Published &&
                 b == ProgramTransitionPublishResult::Conflict) ||
                (b == ProgramTransitionPublishResult::Published &&
                 a == ProgramTransitionPublishResult::Conflict));

    const auto& winner = a == ProgramTransitionPublishResult::Published ? first : second;
    const auto& loser  = a == ProgramTransitionPublishResult::Published ? second : first;
    const auto current = store.load_lineage("owner-a", lineage->lineage_id());
    ASSERT_TRUE(current);
    EXPECT_EQ(current->active_generation(), 2U);
    EXPECT_EQ(current->active_generation_id(), winner.run_generation->id());
    EXPECT_EQ(current->remaining_budget(), successor_budget);
    EXPECT_TRUE(store.load("owner-a", winner.run_record.run_id()));
    EXPECT_FALSE(store.load("owner-a", loser.run_record.run_id()));
    const auto generation = store.load_generation("owner-a", lineage->lineage_id(), 2);
    ASSERT_TRUE(generation);
    ASSERT_TRUE(generation->replacement_receipt());
    EXPECT_EQ(generation->replacement_receipt()->checkpoint_entry_id(),
              boundary.completed_checkpoint.id());
    const auto parsed = ProgramRunGeneration::parse(generation->serialize_canonical());
    ASSERT_TRUE(parsed.replacement_receipt());
    EXPECT_EQ(parsed.id(), generation->id());
    EXPECT_EQ(parsed.replacement_receipt()->id(), generation->replacement_receipt()->id());
    EXPECT_EQ(store.compare_publish("owner-a", {}, winner),
              ProgramTransitionPublishResult::AlreadyPresent);
}

TEST_P(ProgramTransitionStoreConformance, ReplacementAtomicallyTransfersContextAndUnresolvedHookOwnership) {
    auto& store = primary_store();
    const auto source_context = context_publication(1);
    const auto source_hook = pending_hook_outbox_entry();
    const auto boundary = publish_replacement_boundary(
        store, false, source_context, {source_hook});
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation(
        "owner-a", lineage->lineage_id(), lineage->active_generation());
    ASSERT_TRUE(predecessor);

    const auto successor_budget = program_replacement_remaining_budget(
        boundary.publication.run_record, *lineage, 40);
    auto successor = start_publication_for(
        "run-2", digest('7'), digest('8'), successor_budget, 40,
        json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
             {"previous_run_id", "run-1"}});
    successor.context_publication =
        transferred_context_publication(source_context, "run-2");
    attach_replacement_successor(
        successor, *lineage, *predecessor, boundary.publication.run_record,
        boundary.completed_checkpoint, {source_context}, {source_hook});

    ASSERT_EQ(store.compare_publish("owner-a", {}, successor),
              ProgramTransitionPublishResult::Published);
    const auto effective = store.load_effective_runtime_state("owner-a", "run-2");
    ASSERT_TRUE(effective.transfer_receipt);
    ASSERT_EQ(effective.context_publications.size(), 1U);
    EXPECT_EQ(effective.context_publications.front().epoch.run_id(), "run-2");
    ASSERT_EQ(effective.inherited_hook_outbox_entries.size(), 1U);
    EXPECT_EQ(effective.inherited_hook_outbox_entries.front().id(), source_hook.id());
    EXPECT_EQ(effective.inherited_hook_outbox_entries.front().data().event.run_id(), "run-1");
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").front().epoch.id(),
              source_context.epoch.id());
    EXPECT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").front().id(),
              source_hook.id());
}

TEST_P(ProgramTransitionStoreConformance, ReplacementRejectsTransferredContextAssemblyProvenanceDrift) {
    auto& store = primary_store();
    const auto source_context = context_publication(1);
    const auto boundary = publish_replacement_boundary(store, false, source_context);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation(
        "owner-a", lineage->lineage_id(), lineage->active_generation());
    ASSERT_TRUE(predecessor);

    auto successor = start_publication_for(
        "run-2", digest('7'), digest('8'),
        program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40),
        40, json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                 {"previous_run_id", "run-1"}});
    auto transferred = transferred_context_publication(source_context, "run-2");
    ContextAssemblyReceiptData altered_receipt{
        transferred.epoch.id(), digest('c'),
        transferred.assembly_receipt.message_window_digest(),
        transferred.assembly_receipt.artifact_ids(),
        transferred.assembly_receipt.required_skill_artifact_ids(),
        transferred.assembly_receipt.raw_from_sequence(),
        transferred.assembly_receipt.raw_through_sequence(),
        transferred.assembly_receipt.estimated_input_tokens(),
        transferred.assembly_receipt.mandatory_input_tokens()};
    transferred.assembly_receipt = ContextAssemblyReceipt::create(
        std::move(altered_receipt), transferred.epoch, transferred.artifacts);
    successor.context_publication = std::move(transferred);
    attach_replacement_successor(
        successor, *lineage, *predecessor, boundary.publication.run_record,
        boundary.completed_checkpoint, {source_context});

    EXPECT_EQ(store.compare_publish("owner-a", {}, successor),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
}

TEST_P(ProgramTransitionStoreConformance, ReplacementCarriesInheritedHookAcrossSuccessorChain) {
    auto& store = primary_store();
    const auto source_context = context_publication(1);
    const auto source_hook = pending_hook_outbox_entry();
    const auto first_boundary = publish_replacement_boundary(
        store, false, source_context, {source_hook});
    const auto first_lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(first_lineage);
    const auto first_generation = store.load_generation(
        "owner-a", first_lineage->lineage_id(), first_lineage->active_generation());
    ASSERT_TRUE(first_generation);

    auto second = start_publication_for(
        "run-2", digest('7'), digest('8'),
        program_replacement_remaining_budget(
            first_boundary.publication.run_record, *first_lineage, 40),
        40, json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                 {"previous_run_id", "run-1"}});
    second.context_publication = transferred_context_publication(source_context, "run-2");
    attach_replacement_successor(
        second, *first_lineage, *first_generation,
        first_boundary.publication.run_record, first_boundary.completed_checkpoint,
        {source_context}, {source_hook});
    ASSERT_EQ(store.compare_publish("owner-a", {}, second),
              ProgramTransitionPublishResult::Published);

    const json second_handoff{{"cursor", 9}, {"state", "ready"}};
    auto second_pending = javascript_command_publication(
        second, replacement_checkpoint_entry(1, false, second_handoff, digest('8')), 50);
    attach_same_generation_lineage(second_pending, *second.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", second.journal_record.id, second_pending),
              ProgramTransitionPublishResult::Published);
    auto second_checkpoint = replacement_checkpoint_entry(2, true, second_handoff, digest('8'));
    auto second_boundary = javascript_command_publication(
        second_pending, second_checkpoint, 60);
    attach_same_generation_lineage(second_boundary, *second_pending.run_lineage);
    ASSERT_EQ(store.compare_publish(
                  "owner-a", second_pending.journal_record.id, second_boundary),
              ProgramTransitionPublishResult::Published);

    const auto second_lineage = store.load_run_lineage("owner-a", "run-2");
    ASSERT_TRUE(second_lineage);
    const auto second_generation = store.load_generation(
        "owner-a", second_lineage->lineage_id(), second_lineage->active_generation());
    ASSERT_TRUE(second_generation);
    auto third = start_publication_for(
        "run-3", digest('9'), digest('a'),
        program_replacement_remaining_budget(second_boundary.run_record, *second_lineage, 70),
        70, json{{"handoff", second_handoff}, {"previous_run_id", "run-2"}});
    third.context_publication = transferred_context_publication(
        *second.context_publication, "run-3");
    attach_replacement_successor(
        third, *second_lineage, *second_generation, second_boundary.run_record,
        second_checkpoint, {*second.context_publication});
    ASSERT_EQ(store.compare_publish("owner-a", {}, third),
              ProgramTransitionPublishResult::Published);

    const auto effective = store.load_effective_runtime_state("owner-a", "run-3");
    ASSERT_EQ(effective.inherited_hook_outbox_entries.size(), 1U);
    EXPECT_EQ(effective.inherited_hook_outbox_entries.front().id(), source_hook.id());
    EXPECT_EQ(effective.inherited_hook_outbox_entries.front().data().event.run_id(), "run-1");
}

TEST_P(ProgramTransitionStoreConformance, ReplacementCannotOmitRequiredRuntimeStateTransfer) {
    auto& store = primary_store();
    const auto source_context = context_publication(1);
    const auto source_hook = pending_hook_outbox_entry();
    const auto boundary = publish_replacement_boundary(
        store, false, source_context, {source_hook});
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation(
        "owner-a", lineage->lineage_id(), lineage->active_generation());
    ASSERT_TRUE(predecessor);
    auto successor = start_publication_for(
        "run-2", digest('7'), digest('8'),
        program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40),
        40, json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                 {"previous_run_id", "run-1"}});
    attach_replacement_successor(successor, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);
    EXPECT_EQ(store.compare_publish("owner-a", {}, successor),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
}

TEST(ProgramTransitionStoreTest, RuntimeStateTransferFaultLeavesSourceAsOnlyOwner) {
    for (const auto fault : {ProgramTransitionFaultPoint::AfterContextSnapshot,
                             ProgramTransitionFaultPoint::AfterLineageSnapshot,
                             ProgramTransitionFaultPoint::BeforeCommit}) {
        OwnedTransitionStore<InMemoryProgramTransitionStore> store;
        const auto source_context = context_publication(1);
        const auto source_hook = pending_hook_outbox_entry();
        const auto boundary = publish_replacement_boundary(
            store, false, source_context, {source_hook});
        const auto lineage = store.load_run_lineage("owner-a", "run-1");
        ASSERT_TRUE(lineage);
        const auto predecessor = store.load_generation(
            "owner-a", lineage->lineage_id(), lineage->active_generation());
        ASSERT_TRUE(predecessor);
        auto successor = start_publication_for(
            "run-2", digest('7'), digest('8'),
            program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40),
            40, json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                     {"previous_run_id", "run-1"}});
        successor.context_publication =
            transferred_context_publication(source_context, "run-2");
        attach_replacement_successor(
            successor, *lineage, *predecessor, boundary.publication.run_record,
            boundary.completed_checkpoint, {source_context}, {source_hook});

        store.store().fail_next_publication_for_testing(fault);
        EXPECT_THROW((void)store.compare_publish("owner-a", {}, successor), std::runtime_error);
        EXPECT_FALSE(store.load("owner-a", "run-2"));
        EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
        EXPECT_EQ(store.load_context_publications("owner-a", "run-1").front().epoch.id(),
                  source_context.epoch.id());
        EXPECT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").front().id(),
                  source_hook.id());
    }
}

TEST_P(ProgramTransitionStoreConformance, HistoricalReplacementInspectionRevalidatesAtoBtoC) {
    auto& store = primary_store();
    const auto first_boundary = publish_replacement_boundary(store);
    const auto first_lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(first_lineage);
    const auto first_generation = store.load_generation(
        "owner-a", first_lineage->lineage_id(), 1);
    ASSERT_TRUE(first_generation);

    const json second_handoff{{"cursor", 9}, {"state", "ready"}};
    const auto second_budget = program_replacement_remaining_budget(
        first_boundary.publication.run_record, *first_lineage, 40);
    auto second = start_publication_for(
        "run-2", digest('7'), digest('8'), second_budget, 40,
        json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
             {"previous_run_id", "run-1"}});
    attach_replacement_successor(second, *first_lineage, *first_generation,
                                 first_boundary.publication.run_record,
                                 first_boundary.completed_checkpoint);
    ASSERT_EQ(store.compare_publish("owner-a", {}, second),
              ProgramTransitionPublishResult::Published);

    auto second_pending = javascript_command_publication(
        second, replacement_checkpoint_entry(1, false, second_handoff, digest('8')), 50);
    attach_same_generation_lineage(second_pending, *second.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", second.journal_record.id, second_pending),
              ProgramTransitionPublishResult::Published);
    auto second_checkpoint = replacement_checkpoint_entry(2, true, second_handoff, digest('8'));
    auto second_boundary = javascript_command_publication(
        second_pending, second_checkpoint, 60);
    attach_same_generation_lineage(second_boundary, *second_pending.run_lineage);
    ASSERT_EQ(store.compare_publish("owner-a", second_pending.journal_record.id,
                                    second_boundary),
              ProgramTransitionPublishResult::Published);

    const auto second_lineage = store.load_run_lineage("owner-a", "run-2");
    ASSERT_TRUE(second_lineage);
    const auto second_generation = store.load_generation(
        "owner-a", second_lineage->lineage_id(), 2);
    ASSERT_TRUE(second_generation);
    const auto third_budget = program_replacement_remaining_budget(
        second_boundary.run_record, *second_lineage, 70);
    auto third = start_publication_for(
        "run-3", digest('9'), digest('a'), third_budget, 70,
        json{{"handoff", second_handoff}, {"previous_run_id", "run-2"}});
    attach_replacement_successor(third, *second_lineage, *second_generation,
                                 second_boundary.run_record, second_checkpoint);
    ASSERT_EQ(store.compare_publish("owner-a", {}, third),
              ProgramTransitionPublishResult::Published);

    const auto chain = inspect_program_replacement_chain(store, "owner-a", "run-1");
    EXPECT_EQ(chain.anchor().active_generation(), 3U);
    ASSERT_EQ(chain.generations().size(), 3U);
    ASSERT_EQ(chain.replacements().size(), 2U);
    EXPECT_EQ(chain.replacements()[0].source_run().run_id(), "run-1");
    EXPECT_EQ(chain.replacements()[0].target_generation().run_id(), "run-2");
    EXPECT_EQ(chain.replacements()[1].source_run().run_id(), "run-2");
    EXPECT_EQ(chain.replacements()[1].target_generation().run_id(), "run-3");
    EXPECT_EQ(chain.replacements()[0].target_initial_publication().run_record.id(),
              second.run_record.id());
    EXPECT_NE(chain.replacements()[0].target_initial_publication().run_record.id(),
              second_boundary.run_record.id());

    const auto from_middle = inspect_program_replacement_chain(store, "owner-a", "run-2");
    const auto from_active = inspect_program_replacement_chain(store, "owner-a", "run-3");
    EXPECT_EQ(from_middle.anchor().id(), chain.anchor().id());
    EXPECT_EQ(from_active.anchor().id(), chain.anchor().id());
    EXPECT_THROW((void)inspect_program_replacement_chain(store, "owner-b", "run-2"),
                 std::invalid_argument);
}

TEST_P(ProgramTransitionStoreConformance, ReplacementReceiptRejectsWrongHandoffWithoutMutation) {
    auto& store = primary_store();
    const auto boundary = publish_replacement_boundary(store);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
    ASSERT_TRUE(predecessor);

    const auto successor_budget = program_replacement_remaining_budget(
        boundary.publication.run_record, *lineage, 40);
    auto target = start_publication_for(
        "run-2", digest('7'), digest('8'), successor_budget, 40,
        json{{"handoff", json{{"cursor", 8}, {"state", "wrong"}}},
             {"previous_run_id", "run-1"}});
    attach_replacement_successor(target, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);
    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, ReplacementReceiptRejectsRetargetedInitialSnapshot) {
    auto& store = primary_store();
    const auto boundary = publish_replacement_boundary(store);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
    ASSERT_TRUE(predecessor);
    const auto successor_budget = program_replacement_remaining_budget(
        boundary.publication.run_record, *lineage, 40);
    auto target = start_publication_for(
        "run-2", digest('7'), digest('8'), successor_budget, 40,
        json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
             {"previous_run_id", "run-1"}});
    attach_replacement_successor(target, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);

    auto retargeted_data = copy_run_record_data(target.run_record);
    retargeted_data.binding_fingerprint = digest('b');
    target.run_record = ProgramRunRecord::create(std::move(retargeted_data));
    auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        target.run_record.owner_scope(), lineage->lineage_id(), 2, target.run_record.run_id(),
        target.run_record.program_version_id(), target.run_record.bundle_id(),
        target.run_record.id(), target.run_record.journal_head(), predecessor->id(),
        target.run_record.created_at_ms(), target.run_record.child_depth(),
        target.run_generation->replacement_receipt()});
    target.run_generation = generation;
    target.run_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        lineage->owner_scope(), lineage->lineage_id(), lineage->root_run_id(), 2,
        generation.id(), target.run_record.id(), target.run_record.journal_head(),
        successor_budget, RunBudget{}, lineage->id(), lineage->created_at_ms(),
        target.run_record.updated_at_ms(), lineage->committed_descendant_budget()});

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, RecordedReplaySourceCannotPublishLiveReplacement) {
    auto& store = primary_store();
    const auto boundary = publish_replacement_boundary(store, true);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
    ASSERT_TRUE(predecessor);
    const auto successor_budget = program_replacement_remaining_budget(
        boundary.publication.run_record, *lineage, 40);
    auto target = start_publication_for(
        "run-2", digest('7'), digest('8'), successor_budget, 40,
        json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
             {"previous_run_id", "run-1"}});
    attach_replacement_successor(target, *lineage, *predecessor,
                                 boundary.publication.run_record,
                                 boundary.completed_checkpoint);

    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2"));
    EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, LineageSuccessorCannotReplenishBudget) {
    auto& store = primary_store();
    auto source = start_publication();
    attach_initial_lineage(source);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source),
              ProgramTransitionPublishResult::Published);

    auto replenished_budget = budget();
    ++replenished_budget.max_dynamic_compiles;
    auto target = start_publication_for("run-2", digest('7'), digest('8'), replenished_budget, 20);
    attach_successor_lineage(target, *source.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2").has_value());
    EXPECT_EQ(store.load_lineage("owner-a", source.run_lineage->lineage_id())->id(),
              source.run_lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, LineageCannotMoveRunsWithoutSuccessorGeneration) {
    auto& store = primary_store();
    auto source = start_publication();
    attach_initial_lineage(source);
    ASSERT_EQ(store.compare_publish("owner-a", {}, source),
              ProgramTransitionPublishResult::Published);

    auto target = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
    attach_same_generation_lineage(target, *source.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", {}, target),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-2").has_value());
    EXPECT_EQ(store.load_lineage("owner-a", source.run_lineage->lineage_id())->id(),
              source.run_lineage->id());
}

TEST_P(ProgramTransitionStoreConformance, LegacyRunCanAdoptLineageWithoutRenewingBudget) {
    auto& store = primary_store();
    const auto legacy = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, legacy),
              ProgramTransitionPublishResult::Published);

    auto terminal = terminal_publication(legacy, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed);
    attach_initial_lineage(terminal);
    ASSERT_EQ(store.compare_publish("owner-a", legacy.journal_record.id, terminal),
              ProgramTransitionPublishResult::Published);
    const auto lineage = store.load_lineage("owner-a", terminal.run_lineage->lineage_id());
    ASSERT_TRUE(lineage.has_value());
    EXPECT_EQ(lineage->remaining_budget(), terminal.journal_record.remaining_budget);
    EXPECT_EQ(store.load_generation("owner-a", lineage->lineage_id(), 1)->initial_run_record_id(),
              terminal.run_record.id());

    auto bypass = terminal_publication(legacy, ProgramTerminalStatus::Failed,
                                       ContinuationState::Failed, 21);
    EXPECT_EQ(store.compare_publish("owner-a", legacy.journal_record.id, bypass),
              ProgramTransitionPublishResult::Conflict);
}

TEST_P(ProgramTransitionStoreConformance, TerminalResultCannotPublishWithoutTerminalOutbox) {
    auto& store = primary_store();
    auto                           start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto terminal =
        terminal_publication(start, ProgramTerminalStatus::Completed, ContinuationState::Completed);
    terminal.events.clear();
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Conflict);
    auto visible = store.load("owner-a", "run-1");

    ASSERT_TRUE(visible);
    EXPECT_FALSE(visible->terminal_result());
    EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
}

TEST_P(ProgramTransitionStoreConformance, ChildMetadataPublicationIsIdempotentAndConflictDetecting) {
    auto& store = primary_store();
    const auto                     start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);

    const auto publishing = child_metadata_publication(start);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, publishing),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, publishing),
              ProgramTransitionPublishResult::AlreadyPresent);
    ASSERT_TRUE(store.load("owner-a", "run-1").has_value());
    ASSERT_EQ(store.load("owner-a", "run-1")->children().size(), 1U);

    const auto conflicting = child_metadata_publication(publishing, ProgramChildState::Publishing,
                                                        21, "different-child-trace");
    EXPECT_EQ(store.compare_publish("owner-a", publishing.journal_record.id, conflicting),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, publishing.journal_record.id);
}
TEST(ProgramTransitionStoreTest, TerminalChildStatesRequireMatchingDurableResults) {
    const auto start = start_publication();
    EXPECT_THROW((void)child_metadata_publication(start, ProgramChildState::Completed),
                 std::invalid_argument);
    EXPECT_THROW((void)child_metadata_publication(start, ProgramChildState::Failed),
                 std::invalid_argument);
}
TEST(ProgramTransitionStoreTest, TerminalChildResultSurvivesRecordRoundTrip) {
    const auto start      = start_publication();
    auto       publishing = child_metadata_publication(start, ProgramChildState::Dispatched);
    auto       children   = publishing.run_record.children();
    ASSERT_EQ(children.size(), 1U);

    const auto        child_id = children.front().child_run_id;
    ProgramResultData child_result_data;
    child_result_data.status             = ProgramTerminalStatus::Completed;
    child_result_data.run_id             = child_id;
    child_result_data.program_version_id = digest('1');
    child_result_data.bundle_id          = digest('2');
    child_result_data.operation_id       = "root";
    child_result_data.attempt            = 1;
    child_result_data.output             = json{{"child", true}};
    child_result_data.remaining_budget   = budget();
    child_result_data.checkpoint         = checkpoint();
    children.front().state               = ProgramChildState::Completed;
    children.front().terminal_result     = ProgramResult::create(std::move(child_result_data));

    const auto&          run = publishing.run_record;
    ProgramRunRecordData data;
    data.owner_scope         = run.owner_scope();
    data.run_id              = run.run_id();
    data.program_version_id  = run.program_version_id();
    data.bundle_id           = run.bundle_id();
    data.binding_fingerprint = run.binding_fingerprint();
    data.invocation          = run.invocation();
    data.continuation        = run.continuation();
    data.remaining_budget    = run.remaining_budget();
    data.children            = std::move(children);
    data.journal_head        = run.journal_head();
    data.event_sequence      = run.event_sequence();
    data.effect_sequence     = run.effect_sequence();
    data.created_at_ms       = run.created_at_ms();
    data.updated_at_ms       = run.updated_at_ms();
    const auto parsed =
        ProgramRunRecord::parse(ProgramRunRecord::create(std::move(data)).serialize_canonical());
    ASSERT_TRUE(parsed.children().front().terminal_result.has_value());
    EXPECT_EQ(parsed.children().front().state, ProgramChildState::Completed);
    EXPECT_EQ(parsed.children().front().terminal_result->status(),
              ProgramTerminalStatus::Completed);
    EXPECT_EQ(parsed.children().front().terminal_result->run_id(), child_id);
}
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE

TEST(ProgramTransitionStoreTest, SQLiteRejectsReceiptlessSuccessorAcrossReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-lineage-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    auto source = start_publication();
    attach_initial_lineage(source);
    auto target = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
    attach_successor_lineage(target, *source.run_lineage);
    const auto lineage_id = source.run_lineage->lineage_id();
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, source),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", {}, target),
                  ProgramTransitionPublishResult::Conflict);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        OwnedTransitionStore<SQLiteProgramTransitionStore> same_backend(path);
        EXPECT_FALSE(store.process_coordination_key().empty());
        EXPECT_EQ(store.process_coordination_key(), same_backend.process_coordination_key());
        const auto lineage = store.load_lineage("owner-a", lineage_id);
        ASSERT_TRUE(lineage.has_value());
        EXPECT_EQ(lineage->id(), source.run_lineage->id());
        EXPECT_EQ(lineage->active_generation(), 1U);
        ASSERT_TRUE(store.load_generation("owner-a", lineage_id, 1).has_value());
        EXPECT_FALSE(store.load_generation("owner-a", lineage_id, 2).has_value());
        EXPECT_EQ(store.load_generation("owner-a", lineage_id, 1)->id(),
                  source.run_generation->id());
        EXPECT_EQ(store.load_lineage_head("owner-a", lineage_id, source.run_lineage->id())->id(),
                  source.run_lineage->id());
        EXPECT_EQ(store.load_run_lineage("owner-a", source.run_record.run_id())->id(),
                  source.run_lineage->id());
        EXPECT_FALSE(store.load_run_lineage("owner-a", target.run_record.run_id()));
        EXPECT_FALSE(store.load("owner-a", target.run_record.run_id()));
        EXPECT_FALSE(store.load_lineage("owner-b", lineage_id).has_value());
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteReplacementReceiptSurvivesReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-replacement-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    std::string lineage_id;
    std::string generation_id;
    std::string receipt_id;
    RunBudget   successor_budget;
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto boundary = publish_replacement_boundary(store);
        const auto lineage = store.load_run_lineage("owner-a", "run-1");
        ASSERT_TRUE(lineage);
        const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
        ASSERT_TRUE(predecessor);
        successor_budget = program_replacement_remaining_budget(
            boundary.publication.run_record, *lineage, 40);
        auto target = start_publication_for(
            "run-2", digest('7'), digest('8'), successor_budget, 40,
            json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                 {"previous_run_id", "run-1"}});
        attach_replacement_successor(target, *lineage, *predecessor,
                                     boundary.publication.run_record,
                                     boundary.completed_checkpoint);
        ASSERT_EQ(store.compare_publish("owner-a", {}, target),
                  ProgramTransitionPublishResult::Published);
        lineage_id   = lineage->lineage_id();
        generation_id = target.run_generation->id();
        receipt_id    = target.run_generation->replacement_receipt()->id();
    }
    {
        TestSqliteDatabase database(path);
        database.execute(
            "DELETE FROM program_run_generation_publications_v1 WHERE generation = 2");
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto lineage = store.load_lineage("owner-a", lineage_id);
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 2U);
        EXPECT_EQ(lineage->active_generation_id(), generation_id);
        const auto generation = store.load_generation("owner-a", lineage_id, 2);
        ASSERT_TRUE(generation);
        ASSERT_TRUE(generation->replacement_receipt());
        EXPECT_EQ(generation->replacement_receipt()->id(), receipt_id);
        EXPECT_EQ(store.load_run_lineage("owner-a", "run-1")->id(), lineage->id());
        EXPECT_EQ(store.load_run_lineage("owner-a", "run-2")->id(), lineage->id());
        EXPECT_EQ(store.load("owner-a", "run-2")->remaining_budget(), successor_budget);
        ASSERT_TRUE(store.load_generation_initial_publication("owner-a", lineage_id, 1));
        ASSERT_TRUE(store.load_generation_initial_publication("owner-a", lineage_id, 2));
        const auto chain = inspect_program_replacement_chain(store, "owner-a", "run-2");
        ASSERT_EQ(chain.replacements().size(), 1U);
        EXPECT_EQ(chain.replacements().front().target_generation().id(), generation_id);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteConnectionsPublishExactlyOneReplacementSuccessor) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-replacement-cas-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> first_store(path);
        OwnedTransitionStore<SQLiteProgramTransitionStore> second_store(path);
        const auto boundary = publish_replacement_boundary(first_store);
        const auto lineage = first_store.load_run_lineage("owner-a", "run-1");
        ASSERT_TRUE(lineage);
        const auto predecessor =
            first_store.load_generation("owner-a", lineage->lineage_id(), 1);
        ASSERT_TRUE(predecessor);
        const auto successor_budget = program_replacement_remaining_budget(
            boundary.publication.run_record, *lineage, 40);
        const auto input = json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                                {"previous_run_id", "run-1"}};
        auto first =
            start_publication_for("run-2", digest('7'), digest('8'), successor_budget, 40, input);
        auto second =
            start_publication_for("run-3", digest('9'), digest('a'), successor_budget, 40, input);
        attach_replacement_successor(first, *lineage, *predecessor,
                                     boundary.publication.run_record,
                                     boundary.completed_checkpoint);
        attach_replacement_successor(second, *lineage, *predecessor,
                                     boundary.publication.run_record,
                                     boundary.completed_checkpoint);

        std::barrier ready(3);
        auto first_result = std::async(std::launch::async, [&] {
            ready.arrive_and_wait();
            return first_store.compare_publish("owner-a", {}, first);
        });
        auto second_result = std::async(std::launch::async, [&] {
            ready.arrive_and_wait();
            return second_store.compare_publish("owner-a", {}, second);
        });
        ready.arrive_and_wait();
        const auto a = first_result.get();
        const auto b = second_result.get();
        ASSERT_TRUE((a == ProgramTransitionPublishResult::Published &&
                     b == ProgramTransitionPublishResult::Conflict) ||
                    (b == ProgramTransitionPublishResult::Published &&
                     a == ProgramTransitionPublishResult::Conflict));

        const auto& winner = a == ProgramTransitionPublishResult::Published ? first : second;
        const auto& loser  = a == ProgramTransitionPublishResult::Published ? second : first;
        const auto head = first_store.load_lineage("owner-a", lineage->lineage_id());
        ASSERT_TRUE(head);
        EXPECT_EQ(head->active_generation_id(), winner.run_generation->id());
        EXPECT_TRUE(first_store.load("owner-a", winner.run_record.run_id()));
        EXPECT_FALSE(first_store.load("owner-a", loser.run_record.run_id()));
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteConnectionsRejectReceiptlessSuccessors) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-lineage-cas-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> first_store(path);
        OwnedTransitionStore<SQLiteProgramTransitionStore> second_store(path);
        auto source = start_publication();
        attach_initial_lineage(source);
        ASSERT_EQ(first_store.compare_publish("owner-a", {}, source),
                  ProgramTransitionPublishResult::Published);

        auto first = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
        auto second = start_publication_for("run-3", digest('9'), digest('a'), budget(), 20);
        attach_successor_lineage(first, *source.run_lineage);
        attach_successor_lineage(second, *source.run_lineage);
        EXPECT_EQ(first_store.compare_publish("owner-a", {}, first),
                  ProgramTransitionPublishResult::Conflict);
        EXPECT_EQ(second_store.compare_publish("owner-a", {}, second),
                  ProgramTransitionPublishResult::Conflict);

        const auto lineage = first_store.load_lineage("owner-a", source.run_lineage->lineage_id());
        ASSERT_TRUE(lineage.has_value());
        EXPECT_EQ(lineage->id(), source.run_lineage->id());
        EXPECT_EQ(lineage->active_generation(), 1U);
        EXPECT_FALSE(first_store.load_generation("owner-a", lineage->lineage_id(), 2));
        EXPECT_FALSE(first_store.load("owner-a", first.run_record.run_id()));
        EXPECT_FALSE(second_store.load("owner-a", second.run_record.run_id()));
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteReopensAtomicPublicationAndOwnerIsolation) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);

        const auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                   ContinuationState::Completed);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
                  ProgramTransitionPublishResult::AlreadyPresent);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 2U);
        EXPECT_TRUE(store.load("owner-b", "run-1") == std::nullopt);
    }

    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_TRUE(run->terminal_result().has_value());
        EXPECT_EQ(run->terminal_result()->status(), ProgramTerminalStatus::Completed);
        ASSERT_TRUE(store.latest("owner-a", "run-1").has_value());
        EXPECT_EQ(store.load_events("owner-a", "run-1", 1).size(), 1U);
    }

    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteJavaScriptCommandHistorySurvivesReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-javascript-command-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        exercise_javascript_command_history(store);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto commands = store.load_javascript_commands("owner-a", "run-1", 0);
        ASSERT_EQ(commands.size(), 4U);
        EXPECT_TRUE(commands[0].pending());
        EXPECT_TRUE(commands[1].completed());
        EXPECT_TRUE(commands[2].pending());
        EXPECT_TRUE(commands[3].completed());
        EXPECT_EQ(commands[0].coordinate_id(), commands[1].coordinate_id());
        EXPECT_EQ(commands[2].coordinate_id(), commands[3].coordinate_id());
    }
    std::filesystem::remove(path);
}
TEST(ProgramTransitionStoreTest, SQLiteCommandPublicationHeadReadsOnlyTailAndRejectsTailCorruption) {
    const auto path = (std::filesystem::temp_directory_path() / "neograph-command-head-corruption.db").string();
    std::filesystem::remove(path);
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        exercise_javascript_command_history(store);
        TestSqliteDatabase db(path);
        db.execute("UPDATE program_transition_javascript_command_log_v2 SET canonical_bytes = 'broken' WHERE sequence = 1");
        const auto head = store.load_command_publication_head("owner-a", "run-1");
        ASSERT_TRUE(head && head->latest_command);
        EXPECT_EQ(head->latest_command->sequence(), 4U);
        EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::exception);
        db.execute("UPDATE program_transition_javascript_command_log_v2 SET sequence = 5 WHERE sequence = 4");
        EXPECT_THROW((void)store.load_command_publication_head("owner-a", "run-1"), std::invalid_argument);
        const ProgramTransitionPublication current{head->run_record, head->journal_record, {}, {}};
        EXPECT_THROW((void)store.compare_publish(
                         "owner-a", head->journal_record.id,
                         javascript_command_publication(current, javascript_command_entry(5, false, 3), 70)),
                     std::invalid_argument);
        db.execute("UPDATE program_transition_javascript_command_log_v2 SET sequence = 4, coordinate_id = 'broken' WHERE sequence = 5");
        EXPECT_THROW((void)store.load_command_publication_head("owner-a", "run-1"), std::invalid_argument);
    }
    std::filesystem::remove(path);
}
TEST(ProgramTransitionStoreTest, SQLiteContextPublicationIsAtomicAcrossReopenAndCorruption) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-context-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    auto initial = start_publication();
    initial.context_publication = context_publication(1);
    auto next = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    next.context_publication = context_publication(2, initial.context_publication->epoch.id());
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", {}, initial),
                  ProgramTransitionPublishResult::AlreadyPresent);
        ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, next),
                  ProgramTransitionPublishResult::Published);
        const auto history = store.load_context_publications("owner-a", "run-1");
        ASSERT_EQ(history.size(), 2U);
        EXPECT_EQ(history[1].epoch.predecessor_id(), initial.context_publication->epoch.id());
        EXPECT_EQ(store.load_context_publications("owner-a", "run-1", 1).size(), 1U);
        EXPECT_TRUE(store.load_context_publications("owner-b", "run-1").empty());

        auto invalid = javascript_command_publication(next, javascript_command_entry(2, true), 30);
        invalid.context_publication = context_publication(4, next.context_publication->epoch.id());
        EXPECT_EQ(store.compare_publish("owner-a", next.journal_record.id, invalid),
                  ProgramTransitionPublishResult::Conflict);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_context_log_v1"), 2);
        database.execute("UPDATE program_transition_context_log_v1 SET canonical_bytes = x'7B7D' "
                         "WHERE owner_scope = 'owner-a' AND run_id = 'run-1' AND sequence = 2");
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        EXPECT_THROW((void)store.load_context_publications("owner-a", "run-1"),
                     std::invalid_argument);
        auto retry = javascript_command_publication(next, javascript_command_entry(2, true), 30);
        retry.context_publication = context_publication(3, next.context_publication->epoch.id());
        EXPECT_THROW((void)store.compare_publish("owner-a", next.journal_record.id, retry),
                     std::invalid_argument);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_context_log_v1"), 2);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_javascript_command_log_v2"),
                  1);
    }
    std::filesystem::remove(path);
}


TEST(ProgramTransitionStoreTest, SQLiteReopensDurableMigrationProof) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-migration-proof-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    auto source_start = start_publication();
    attach_initial_lineage(source_start);
    auto source = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    auto publication =
        start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(publication);
    publication.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(publication, *source.run_lineage);
    auto dropped = start_publication_for("run-3", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(dropped, std::nullopt, json(), false);
    dropped.migration_plan = publication.migration_plan;
    attach_fork_split(dropped, *source.run_lineage);
    auto downgraded = start_publication_for("run-4", digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(downgraded, std::string("effect-call"), json{{"result", "recorded"}}, true,
                        true);
    downgraded.migration_plan = publication.migration_plan;
    attach_fork_split(downgraded, *source.run_lineage);
    const auto expected_id     = publication.migration_plan->id();
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, source_start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", source_start.journal_record.id, source),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", {}, dropped),
                  ProgramTransitionPublishResult::Conflict);
        EXPECT_EQ(store.compare_publish("owner-a", {}, downgraded),
                  ProgramTransitionPublishResult::Conflict);
        ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
                  ProgramTransitionPublishResult::Published);
        ASSERT_TRUE(store.load_migration_plan("owner-a", "run-2").has_value());
        EXPECT_EQ(store.load_migration_plan("owner-a", "run-2")->id(), expected_id);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-2");
        ASSERT_TRUE(run.has_value());
        ASSERT_TRUE(run->fork_receipt().has_value());
        const auto proof = store.load_migration_plan("owner-a", "run-2");
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->id(), expected_id);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteEffectOutboxSurvivesResumePublication) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-effect-history-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start       = start_publication();
    const auto interrupted = interrupted_effect_publication(start);
    const auto resumed     = resumed_effect_publication(interrupted);
    EXPECT_NO_THROW((void)resumed.serialize_canonical());
    EXPECT_TRUE(
        is_valid_program_journal_transition(interrupted.journal_record, resumed.journal_record));
    auto aggregate_events = interrupted.events;
    aggregate_events.insert(aggregate_events.end(), resumed.events.begin(), resumed.events.end());
    auto aggregate_effects = interrupted.effects;
    aggregate_effects.insert(aggregate_effects.end(), resumed.effects.begin(),
                             resumed.effects.end());
    const ProgramTransitionPublication aggregate{resumed.run_record, resumed.journal_record,
                                                 std::move(aggregate_events),
                                                 std::move(aggregate_effects), std::nullopt};
    EXPECT_NO_THROW((void)aggregate.serialize_canonical());
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
        ASSERT_TRUE(store.load("owner-a", "run-1").has_value());
        EXPECT_EQ(store.load("owner-a", "run-1")->continuation().state, ContinuationState::Running);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteChildMetadataPublicationSurvivesReopen) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-child-transition-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto child = child_metadata_publication(start);
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_EQ(run->children().size(), 1U);
        EXPECT_EQ(run->children().front().child_run_id, "child-run-1");
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, child.journal_record.id);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteStoresTransitionDeltasAndReopens) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-delta-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start = start_publication();
    const auto child = child_metadata_publication(start);
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_run_heads_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_event_log_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_effect_log_v2"), 0);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM sqlite_master "
                                  "WHERE type = 'table' AND name = 'program_transition_runs'"),
                  0);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_FALSE(run->terminal_result().has_value());
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, child.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 1U);
        EXPECT_TRUE(store.load_events("owner-a", "run-1", 1).empty());
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteMigratesLegacySnapshotToDeltaLog) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-legacy-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    const auto start         = start_publication();
    const auto interrupted   = interrupted_effect_publication(start);
    const auto resumed       = resumed_effect_publication(interrupted);
    auto       legacy_events = start.events;
    legacy_events.insert(legacy_events.end(), interrupted.events.begin(), interrupted.events.end());
    const ProgramTransitionPublication legacy{interrupted.run_record, interrupted.journal_record,
                                              std::move(legacy_events), interrupted.effects,
                                              std::nullopt};
    json                               legacy_retry_body = json::object();
    for (const auto& [key, value] : publication_reference_body(interrupted).items()) {
        if (key != "commands") legacy_retry_body[key] = value;
    }
    const auto legacy_retry_bytes = detail::canonical_json_bytes(legacy_retry_body);
    {
        TestSqliteDatabase database(path);
        database.execute(
            "CREATE TABLE program_transition_runs ("
            "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
            "canonical_bytes BLOB NOT NULL, last_publication_bytes BLOB NOT NULL, "
            "PRIMARY KEY(owner_scope, run_id))");
        database.insert_legacy("owner-a", "run-1", legacy.serialize_canonical(),
                               legacy_retry_bytes);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->continuation().state, ContinuationState::Interrupted);
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, interrupted.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 2U);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::AlreadyPresent);
        EXPECT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
    }
    {
        TestSqliteDatabase database(path);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_run_heads_v2"), 1);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_event_log_v2"), 3);
        EXPECT_EQ(database.scalar("SELECT COUNT(*) FROM program_transition_effect_log_v2"), 1);
    }
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->continuation().state, ContinuationState::Running);
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, resumed.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1", 2).size(), 1U);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    std::filesystem::remove(path);
    {
        ProgramTransitionPublication bad_legacy = legacy;
        bad_legacy.commands = {javascript_command_entry(1, false, 1),
                               javascript_command_entry(2, true, 2)};
        TestSqliteDatabase database(path);
        database.execute(
            "CREATE TABLE program_transition_runs ("
            "owner_scope TEXT NOT NULL, run_id TEXT NOT NULL, "
            "canonical_bytes BLOB NOT NULL, last_publication_bytes BLOB NOT NULL, "
            "PRIMARY KEY(owner_scope, run_id))");
        database.insert_legacy("owner-a", "run-1", bad_legacy.serialize_canonical(),
                               legacy_retry_bytes);
    }
    EXPECT_THROW((void)SQLiteProgramTransitionStore(path), std::invalid_argument);
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteConcurrentCasAcrossConnectionsHasOneWinner) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-transition-cas-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {
        OwnedTransitionStore<SQLiteProgramTransitionStore> first(path);
        OwnedTransitionStore<SQLiteProgramTransitionStore> second(path);
        const auto                   start = start_publication();
        ASSERT_EQ(first.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto   completed = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                      ContinuationState::Completed, 20);
        const auto   failed    = terminal_publication(start, ProgramTerminalStatus::Failed,
                                                      ContinuationState::Failed, 21);
        std::barrier ready(3);
        auto         publish = [&](BlockingProgramTransitionStore& store,
                           ProgramTransitionPublication  publication) {
            ready.arrive_and_wait();
            return store.compare_publish("owner-a", start.journal_record.id,
                                                 std::move(publication));
        };
        auto a = std::async(std::launch::async, [&] { return publish(first, completed); });
        auto b = std::async(std::launch::async, [&] { return publish(second, failed); });
        ready.arrive_and_wait();
        const auto a_result = a.get();
        const auto b_result = b.get();
        EXPECT_TRUE((a_result == ProgramTransitionPublishResult::Published &&
                     b_result == ProgramTransitionPublishResult::Conflict) ||
                    (b_result == ProgramTransitionPublishResult::Published &&
                     a_result == ProgramTransitionPublishResult::Conflict));

        const auto run = first.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        ASSERT_TRUE(run->terminal_result().has_value());
        EXPECT_TRUE(run->terminal_result()->status() == ProgramTerminalStatus::Completed ||
                    run->terminal_result()->status() == ProgramTerminalStatus::Failed);
    }
    std::filesystem::remove(path);
}
#endif

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_POSTGRES
TEST(ProgramTransitionStoreTest, PostgreSQLReopensAtomicLineageAndOwnerIsolation) {
    const auto* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url || !*url) {
        GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set; skipping PostgreSQL transition test.";
    }
    reset_postgres_transition_schema(url);
    static std::atomic<unsigned> sequence{0};
    const auto suffix = std::to_string(sequence.fetch_add(1));
    const auto run_id = "pg-transition-run-" + suffix;
    auto publication = start_publication_for(run_id, digest('d'), digest('e'), budget(), 100);
    attach_initial_lineage(publication);
    const auto lineage_id = publication.run_lineage->lineage_id();
    const auto generation_id = publication.run_generation->id();

    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> same_backend(url);
        EXPECT_FALSE(store.process_coordination_key().empty());
        EXPECT_EQ(store.process_coordination_key(), same_backend.process_coordination_key());
        const auto run = store.load("owner-a", run_id);
        ASSERT_TRUE(run);
        EXPECT_EQ(run->id(), publication.run_record.id());
        EXPECT_FALSE(store.load("owner-b", run_id));
        ASSERT_EQ(store.load_events("owner-a", run_id).size(), 1U);
        const auto lineage = store.load_lineage("owner-a", lineage_id);
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->id(), publication.run_lineage->id());
        EXPECT_EQ(store.load_run_lineage("owner-a", run_id)->id(), lineage->id());
        const auto generation = store.load_generation("owner-a", lineage_id, 1);
        ASSERT_TRUE(generation);
        EXPECT_EQ(generation->id(), generation_id);
        const auto initial = store.load_generation_initial_publication(
            "owner-a", lineage_id, 1);
        ASSERT_TRUE(initial);
        EXPECT_EQ(initial->run_record.id(), publication.run_record.id());
    }
}
TEST(ProgramTransitionStoreTest, PostgreSQLCommandPublicationHeadReadsOnlyTailAndRejectsTailCorruption) {
    const auto* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url || !*url) GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set";
    reset_postgres_transition_schema(url);
    OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
    exercise_javascript_command_history(store);
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection(PQconnectdb(url), PQfinish);
    ASSERT_EQ(PQstatus(connection.get()), CONNECTION_OK);
    const auto execute = [&](const char* sql) {
        std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexec(connection.get(), sql), PQclear);
        ASSERT_EQ(PQresultStatus(result.get()), PGRES_COMMAND_OK);
    };
    execute("UPDATE neograph_program_transition_javascript_command_log_v2 SET canonical_bytes = 'broken' WHERE sequence = 1");
    const auto head = store.load_command_publication_head("owner-a", "run-1");
    ASSERT_TRUE(head && head->latest_command);
    EXPECT_EQ(head->latest_command->sequence(), 4U);
    EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::exception);
    execute("UPDATE neograph_program_transition_javascript_command_log_v2 SET coordinate_id = 'broken' WHERE sequence = 4");
    EXPECT_THROW((void)store.load_command_publication_head("owner-a", "run-1"), std::invalid_argument);
}

TEST(ProgramTransitionStoreTest, PostgreSQLConcurrentInitialCasHasOneWinner) {
    const auto* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url || !*url) {
        GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set; skipping PostgreSQL transition test.";
    }
    reset_postgres_transition_schema(url);
    static std::atomic<unsigned> sequence{1000};
    const auto run_id = "pg-transition-cas-" + std::to_string(sequence.fetch_add(1));
    auto first = start_publication_for(run_id, digest('f'), digest('1'), budget(), 200);
    auto second = start_publication_for(run_id, digest('2'), digest('3'), budget(), 201);
    attach_initial_lineage(first);
    attach_initial_lineage(second);
    OwnedTransitionStore<PostgreSQLProgramTransitionStore> first_store(url);
    OwnedTransitionStore<PostgreSQLProgramTransitionStore> second_store(url);
    std::barrier ready(3);
    auto publish = [&](BlockingProgramTransitionStore& store,
                       ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return store.compare_publish("owner-a", {}, std::move(value));
    };
    auto a = std::async(std::launch::async, [&] { return publish(first_store, first); });
    auto b = std::async(std::launch::async, [&] { return publish(second_store, second); });
    ready.arrive_and_wait();
    const auto a_result = a.get();
    const auto b_result = b.get();
    EXPECT_TRUE((a_result == ProgramTransitionPublishResult::Published &&
                 b_result == ProgramTransitionPublishResult::Conflict) ||
                (b_result == ProgramTransitionPublishResult::Published &&
                 a_result == ProgramTransitionPublishResult::Conflict));
    const auto winner = first_store.load("owner-a", run_id);
    ASSERT_TRUE(winner);
    EXPECT_TRUE(winner->id() == first.run_record.id() || winner->id() == second.run_record.id());
}


TEST(ProgramTransitionStoreTest, PostgreSQLReplacementEffectsHooksAndChildrenSurviveReopen) {
    const auto* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url || !*url) {
        GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set; skipping PostgreSQL transition test.";
    }

    reset_postgres_transition_schema(url);
    std::string lineage_id;
    std::string successor_generation_id;
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        const auto boundary = publish_replacement_boundary(store);
        const auto lineage = store.load_run_lineage("owner-a", "run-1");
        ASSERT_TRUE(lineage);
        const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
        ASSERT_TRUE(predecessor);
        const auto successor_budget = program_replacement_remaining_budget(
            boundary.publication.run_record, *lineage, 40);
        auto target = start_publication_for(
            "run-2", digest('7'), digest('8'), successor_budget, 40,
            json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                 {"previous_run_id", "run-1"}});
        attach_replacement_successor(target, *lineage, *predecessor,
                                     boundary.publication.run_record,
                                     boundary.completed_checkpoint);
        ASSERT_EQ(store.compare_publish("owner-a", {}, target),
                  ProgramTransitionPublishResult::Published);
        lineage_id = lineage->lineage_id();
        successor_generation_id = target.run_generation->id();
    }
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        const auto lineage = store.load_lineage("owner-a", lineage_id);
        ASSERT_TRUE(lineage);
        EXPECT_EQ(lineage->active_generation(), 2U);
        EXPECT_EQ(lineage->active_generation_id(), successor_generation_id);
        ASSERT_TRUE(store.load_generation_initial_publication("owner-a", lineage_id, 1));
        ASSERT_TRUE(store.load_generation_initial_publication("owner-a", lineage_id, 2));
        const auto chain = inspect_program_replacement_chain(store, "owner-a", "run-2");
        ASSERT_EQ(chain.replacements().size(), 1U);
        EXPECT_EQ(chain.replacements().front().target_generation().id(),
                  successor_generation_id);
    }

    reset_postgres_transition_schema(url);
    {
        const auto start = start_publication();
        const auto interrupted = interrupted_effect_publication(start);
        const auto resumed = resumed_effect_publication(interrupted);
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
        ASSERT_TRUE(store.load("owner-a", "run-1"));
        EXPECT_EQ(store.load("owner-a", "run-1")->continuation().state,
                  ContinuationState::Running);
    }

    reset_postgres_transition_schema(url);
    {
        auto initial = start_publication();
        initial.context_publication = context_publication(1);
        initial.hook_outbox_entries = {pending_hook_outbox_entry()};
        attach_initial_lineage(initial);
        auto next = javascript_command_publication(
            initial, javascript_command_entry(1, false), 20);
        next.context_publication =
            context_publication(2, initial.context_publication->epoch.id());
        attach_same_generation_lineage(next, *initial.run_lineage);
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, next),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.load_context_publications("owner-a", "run-1").size(), 2U);
        ASSERT_EQ(store.load_context_publications("owner-a", "run-1", 1).size(), 1U);
        ASSERT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").size(), 1U);
    }
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.load_context_publications("owner-a", "run-1").size(), 2U);
        ASSERT_EQ(store.load_context_publications("owner-a", "run-1", 1).size(), 1U);
        ASSERT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").size(), 1U);
    }

    reset_postgres_transition_schema(url);
    {
        const auto initial = start_publication();
        const auto child = child_metadata_publication(initial);
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        ASSERT_EQ(store.compare_publish("owner-a", {}, initial),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", initial.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
    }
    {
        OwnedTransitionStore<PostgreSQLProgramTransitionStore> store(url);
        const auto run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run);
        ASSERT_EQ(run->children().size(), 1U);
        EXPECT_EQ(run->children().front().child_run_id, "child-run-1");
    }
}
#endif

TEST(ProgramTransitionStoreTest, HookOutboxHeadsAreAtomicAndReconnectable) {
    OwnedTransitionStore<InMemoryProgramTransitionStore> store;
    auto publication = start_publication();
    publication.hook_outbox_entries.push_back(pending_hook_outbox_entry());
    store.store().fail_next_publication_for_testing(ProgramTransitionFaultPoint::AfterHookSnapshot);
    EXPECT_THROW(store.compare_publish("owner-a", {}, publication), std::runtime_error);
    EXPECT_TRUE(store.load_hook_outbox_entries("owner-a", "run-1").empty());
    ASSERT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    const auto hooks = store.load_hook_outbox_entries("owner-a", "run-1");
    ASSERT_EQ(hooks.size(), 1U);
    EXPECT_EQ(hooks.front().data().state, neograph::HookExecutionState::Pending);
}

TEST(ProgramTransitionStoreTest, RejectsSkippedAndContradictoryHookTransitions) {
    const auto pending = pending_hook_outbox_entry();
    const auto succeeded = hook_outbox_entry_with_state(pending, neograph::HookExecutionState::Succeeded);
    const auto run = start_publication().run_record;
    EXPECT_FALSE(is_valid_program_hook_history_append({pending}, {succeeded}, run));
}

TEST_P(ProgramTransitionStoreConformance, StoreRefusesASkippedHookTransition) {
    auto& store = primary_store();
    const auto pending = pending_hook_outbox_entry();
    const auto succeeded = hook_outbox_entry_with_state(pending, neograph::HookExecutionState::Succeeded);
    auto initial = start_publication();
    initial.hook_outbox_entries = {pending};
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial), ProgramTransitionPublishResult::Published);
    auto skipped = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    skipped.hook_outbox_entries = {succeeded};
    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id, skipped),
              ProgramTransitionPublishResult::Conflict);
}

TEST_P(ProgramTransitionStoreConformance, TimedOutMandatoryHookBlocksReplacement) {
    auto& store = primary_store();
    const auto boundary = publish_replacement_boundary(store);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
    ASSERT_TRUE(predecessor);

    auto timed_source = javascript_command_publication(
        boundary.publication, javascript_command_entry(3, false, 2), 35);
    timed_source.commands.clear();
    attach_same_generation_lineage(timed_source, *lineage);
    const auto pending = pending_hook_outbox_entry();
    timed_source.hook_outbox_entries = {
        hook_outbox_entry_with_state(pending, neograph::HookExecutionState::TimedOut)};
    ASSERT_EQ(store.compare_publish("owner-a", boundary.publication.journal_record.id, timed_source),
              ProgramTransitionPublishResult::Published);

    const auto current = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(current);
    const auto input = json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}},
                            {"previous_run_id", "run-1"}};
    auto successor = start_publication_for(
        "run-2", digest('7'), digest('8'),
        program_replacement_remaining_budget(timed_source.run_record, *current, 40), 40, input);
    attach_replacement_successor(successor, *current, *predecessor, timed_source.run_record,
                                 boundary.completed_checkpoint);
    EXPECT_EQ(store.compare_publish("owner-a", {}, successor),
              ProgramTransitionPublishResult::Conflict);
}

namespace {
// A second interruption of a resumed run, publishing one pending effect.
ProgramTransitionPublication second_interruption_publication(
    const ProgramTransitionPublication& resumed, std::string effect_id) {
    const auto old_run = resumed.run_record;
    const auto pending = pending_effect(std::move(effect_id));
    const auto cp      = checkpoint();
    auto       journal = ProgramJournalRecord::create({resumed.journal_record.id,
                                                       "run-1",
                                                       digest('1'),
                                                       digest('2'),
                                                       resumed.journal_record.sequence + 1,
                                                       {"root", ContinuationState::Interrupted, 2},
                                                       budget(),
                                                       {},
                                                       cp,
                                                       40});
    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = "run-1";
    result_data.program_version_id = digest('1');
    result_data.bundle_id          = digest('2');
    result_data.attempt            = 2;
    result_data.output             = json::object();
    result_data.remaining_budget   = budget();
    result_data.checkpoint         = cp;
    result_data.interrupt = ProgramInterrupt{"main", pending.core_interrupt_value(), std::nullopt, pending};

    ProgramRunRecordData data;
    data.owner_scope         = old_run.owner_scope();
    data.run_id              = old_run.run_id();
    data.program_version_id  = old_run.program_version_id();
    data.bundle_id           = old_run.bundle_id();
    data.binding_fingerprint = old_run.binding_fingerprint();
    data.invocation          = old_run.invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.exact_checkpoint    = cp;
    data.pending_effect      = pending;
    data.terminal_result     = ProgramResult::create(std::move(result_data));
    data.journal_head        = journal.id;
    data.event_sequence      = old_run.event_sequence() + 1;
    data.effect_sequence     = old_run.effect_sequence() + 1;
    data.created_at_ms       = old_run.created_at_ms();
    data.updated_at_ms       = 40;
    return {ProgramRunRecord::create(std::move(data)),
            journal,
            {event(old_run.event_sequence() + 1, ProgramEventKind::Terminal,
                   ProgramTerminalEvent{ProgramTerminalStatus::Interrupted}, 40, 2)},
            {{old_run.effect_sequence() + 1, pending}}};
}
}  // namespace

// ---------------------------------------------------------------------------
// Contract clauses that need the backend's help: fault injection, a second
// connection, a restart or a corrupted record.
// ---------------------------------------------------------------------------

namespace {
// Fails the `stage` write of `publication`, checks that the committed state did not move, and
// publishes the same publication once the fault is gone.
template <typename Unchanged>
void expect_fault_leaves_state_then_retries(StoreDeployment&                    deployment,
                                            BlockingProgramTransitionStore&             store,
                                            WriteStage                          stage,
                                            std::string_view                    expected_head,
                                            const ProgramTransitionPublication& publication,
                                            Unchanged&&                         unchanged) {
    SCOPED_TRACE(neograph::program::test_support::write_stage_name(stage));
    deployment.arm_fault(stage);
    EXPECT_THROW((void)store.compare_publish("owner-a", expected_head, publication),
                 std::runtime_error);
    deployment.disarm_faults();
    unchanged();
    EXPECT_EQ(store.compare_publish("owner-a", expected_head, publication),
              ProgramTransitionPublishResult::Published);
}
}  // namespace

// Unit atomicity: the writes of one publication commit together. Whichever of them fails, the
// committed state stays exactly as it was, and the same publication then publishes.
TEST_P(ProgramTransitionStoreConformance, FailedWriteLeavesNoPartialPublication) {
    for (const auto stage : {WriteStage::Run, WriteStage::Events}) {
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store = dep.blocking_transition_store();
        const auto      start = start_publication();
        ASSERT_EQ(store->compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                   ContinuationState::Completed);
        expect_fault_leaves_state_then_retries(
            dep, *store, stage, start.journal_record.id, terminal, [&] {
                EXPECT_EQ(store->latest("owner-a", "run-1")->id, start.journal_record.id);
                EXPECT_FALSE(store->load("owner-a", "run-1")->terminal_result());
                EXPECT_EQ(store->load_events("owner-a", "run-1").size(), 1U);
            });
        EXPECT_EQ(store->load_events("owner-a", "run-1").size(), 2U);
    }
    {
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store = dep.blocking_transition_store();
        const auto      start = start_publication();
        ASSERT_EQ(store->compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto interrupted = interrupted_effect_publication(start);
        expect_fault_leaves_state_then_retries(
            dep, *store, WriteStage::Effects, start.journal_record.id, interrupted, [&] {
                EXPECT_EQ(store->latest("owner-a", "run-1")->id, start.journal_record.id);
                EXPECT_EQ(store->load_events("owner-a", "run-1").size(), 1U);
                EXPECT_TRUE(store->load_effects("owner-a", "run-1").empty());
            });
        EXPECT_EQ(store->load_effects("owner-a", "run-1").size(), 1U);
    }
    {
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store   = dep.blocking_transition_store();
        auto            initial = start_publication();
        initial.context_publication = context_publication(1);
        expect_fault_leaves_state_then_retries(
            dep, *store, WriteStage::Context, {}, initial, [&] {
                EXPECT_FALSE(store->load("owner-a", "run-1"));
                EXPECT_TRUE(store->load_events("owner-a", "run-1").empty());
                EXPECT_TRUE(store->load_context_publications("owner-a", "run-1").empty());
            });
        EXPECT_EQ(store->load_context_publications("owner-a", "run-1").size(), 1U);
    }
    {
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store   = dep.blocking_transition_store();
        auto            initial = start_publication();
        initial.hook_outbox_entries = {pending_hook_outbox_entry()};
        expect_fault_leaves_state_then_retries(
            dep, *store, WriteStage::Hooks, {}, initial, [&] {
                EXPECT_FALSE(store->load("owner-a", "run-1"));
                EXPECT_TRUE(store->load_events("owner-a", "run-1").empty());
                EXPECT_TRUE(store->load_hook_outbox_entries("owner-a", "run-1").empty());
            });
        EXPECT_EQ(store->load_hook_outbox_entries("owner-a", "run-1").size(), 1U);
    }
    {
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store   = dep.blocking_transition_store();
        auto            initial = start_publication();
        attach_initial_lineage(initial);
        expect_fault_leaves_state_then_retries(
            dep, *store, WriteStage::Lineage, {}, initial, [&] {
                EXPECT_FALSE(store->load("owner-a", "run-1"));
                EXPECT_TRUE(store->load_events("owner-a", "run-1").empty());
                EXPECT_FALSE(store->load_lineage("owner-a", initial.run_lineage->lineage_id()));
                EXPECT_FALSE(store->load_run_lineage("owner-a", "run-1"));
                EXPECT_FALSE(store->load_generation("owner-a", initial.run_lineage->lineage_id(), 1));
            });
        EXPECT_TRUE(store->load_generation("owner-a", initial.run_lineage->lineage_id(), 1));
    }
}

// A replacement moves the source's context and hook ownership in the same unit as the new
// generation: a failed write leaves the source as the only owner.
TEST_P(ProgramTransitionStoreConformance, FailedWriteLeavesTheSourceAsTheOnlyOwnerAcrossAReplacement) {
    for (const auto stage : {WriteStage::Run, WriteStage::Context, WriteStage::Lineage}) {
        SCOPED_TRACE(neograph::program::test_support::write_stage_name(stage));
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store_owner = dep.blocking_transition_store();
        auto&           store       = *store_owner;
        const auto source_context = context_publication(1);
        const auto source_hook    = pending_hook_outbox_entry();
        const auto boundary = publish_replacement_boundary(store, false, source_context, {source_hook});
        const auto lineage  = store.load_run_lineage("owner-a", "run-1");
        ASSERT_TRUE(lineage);
        const auto predecessor =
            store.load_generation("owner-a", lineage->lineage_id(), lineage->active_generation());
        ASSERT_TRUE(predecessor);
        auto successor = start_publication_for(
            "run-2", digest('7'), digest('8'),
            program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40), 40,
            json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}}, {"previous_run_id", "run-1"}});
        successor.context_publication = transferred_context_publication(source_context, "run-2");
        attach_replacement_successor(successor, *lineage, *predecessor,
                                     boundary.publication.run_record, boundary.completed_checkpoint,
                                     {source_context}, {source_hook});

        dep.arm_fault(stage);
        EXPECT_THROW((void)store.compare_publish("owner-a", {}, successor), std::runtime_error);
        dep.disarm_faults();
        EXPECT_FALSE(store.load("owner-a", "run-2"));
        EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
        EXPECT_FALSE(store.load_generation("owner-a", lineage->lineage_id(), 2));
        EXPECT_EQ(store.load_context_publications("owner-a", "run-1").front().epoch.id(),
                  source_context.epoch.id());
        EXPECT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").front().id(), source_hook.id());
        EXPECT_EQ(store.compare_publish("owner-a", {}, successor),
                  ProgramTransitionPublishResult::Published);
    }
}

// Own writes: a publication that returned Published is visible to every later read, including
// reads from another connection to the same data.
TEST_P(ProgramTransitionStoreConformance, CommittedPublicationIsVisibleToAnotherConnection) {
    auto&      writer = primary_store();
    const auto reader = second_store();
    const auto start  = start_publication();
    ASSERT_EQ(writer.compare_publish("owner-a", {}, start), ProgramTransitionPublishResult::Published);
    {
        const auto head = reader->load_run_publication_head("owner-a", "run-1");
        ASSERT_TRUE(head);
        EXPECT_EQ(head->run_record.id(), start.run_record.id());
        EXPECT_EQ(head->journal_record.id, start.journal_record.id);
    }
    const auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                               ContinuationState::Completed);
    ASSERT_EQ(writer.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Published);
    const auto head = reader->load_run_publication_head("owner-a", "run-1");
    ASSERT_TRUE(head);
    EXPECT_EQ(head->run_record.id(), terminal.run_record.id());
    EXPECT_EQ(head->journal_record.id, terminal.journal_record.id);
    EXPECT_EQ(reader->latest("owner-a", "run-1")->id, terminal.journal_record.id);
    EXPECT_EQ(reader->load_events("owner-a", "run-1").size(), 2U);
    EXPECT_EQ(reader->load_events("owner-a", "run-1", 1).size(), 1U);
    // The reader's own retry of the writer's publication is an exact retry, not a conflict.
    EXPECT_EQ(reader->compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::AlreadyPresent);
}

// Owner isolation on the write path: a publication cannot be published under, or read from,
// another owner.
TEST_P(ProgramTransitionStoreConformance, PublicationForAnotherOwnerIsRefused) {
    auto&      store = primary_store();
    const auto start = start_publication();
    EXPECT_EQ(store.compare_publish("owner-b", {}, start), ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.compare_publish("", {}, start), ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-a", "run-1"));
    ASSERT_EQ(store.compare_publish("owner-a", {}, start), ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.compare_publish("owner-b", {}, start), ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(store.load("owner-b", "run-1"));
    EXPECT_FALSE(store.latest("owner-b", "run-1"));
    EXPECT_FALSE(store.load_run_publication_head("owner-b", "run-1"));
    EXPECT_TRUE(store.load_events("owner-b", "run-1").empty());
    EXPECT_TRUE(store.load_javascript_commands("owner-b", "run-1").empty());
    EXPECT_FALSE(store.load("", "run-1"));
    EXPECT_FALSE(store.load("owner-a", ""));
}

// Duplicate refusal: an effect id is published once per run.
TEST_P(ProgramTransitionStoreConformance, ReusedEffectIdIsRefused) {
    auto&      store       = primary_store();
    const auto start       = start_publication();
    const auto interrupted = interrupted_effect_publication(start);
    const auto resumed     = resumed_effect_publication(interrupted);
    ASSERT_EQ(store.compare_publish("owner-a", {}, start), ProgramTransitionPublishResult::Published);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Published);
    ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
              ProgramTransitionPublishResult::Published);

    const auto reused = second_interruption_publication(resumed, "effect-one");
    EXPECT_EQ(store.compare_publish("owner-a", resumed.journal_record.id, reused),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, resumed.journal_record.id);
    EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);

    const auto fresh = second_interruption_publication(resumed, "effect-two");
    ASSERT_EQ(store.compare_publish("owner-a", resumed.journal_record.id, fresh),
              ProgramTransitionPublishResult::Published);
    const auto effects = store.load_effects("owner-a", "run-1");
    ASSERT_EQ(effects.size(), 2U);
    EXPECT_EQ(effects[0].sequence(), 1U);
    EXPECT_EQ(effects[1].sequence(), 2U);
    EXPECT_EQ(effects[1].effect().effect_id(), "effect-two");
}

// Durability and restart: after the store is closed and opened again, every committed
// publication is still there and a retry whose acknowledgement was lost is still recognized.
TEST_P(ProgramTransitionStoreConformance, PublishedStateSurvivesRestart) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    auto initial = start_publication();
    initial.context_publication = context_publication(1);
    initial.hook_outbox_entries = {pending_hook_outbox_entry()};
    attach_initial_lineage(initial);
    auto interrupted = interrupted_effect_publication(initial);
    attach_same_generation_lineage(interrupted, *initial.run_lineage);
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, initial),
              ProgramTransitionPublishResult::Published);
    ASSERT_EQ(primary_store().compare_publish("owner-a", initial.journal_record.id, interrupted),
              ProgramTransitionPublishResult::Published);

    restart_store();
    auto& store = primary_store();
    ASSERT_TRUE(store.load("owner-a", "run-1"));
    EXPECT_EQ(store.load("owner-a", "run-1")->id(), interrupted.run_record.id());
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, interrupted.journal_record.id);
    EXPECT_EQ(store.load_events("owner-a", "run-1").size(), 2U);
    EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").size(), 1U);
    EXPECT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").size(), 1U);
    const auto lineage = store.load_run_lineage("owner-a", "run-1");
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->id(), interrupted.run_lineage->id());
    EXPECT_EQ(store.load_generation("owner-a", lineage->lineage_id(), 1)->id(),
              initial.run_generation->id());
    EXPECT_FALSE(store.load("owner-b", "run-1"));

    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id, interrupted),
              ProgramTransitionPublishResult::AlreadyPresent);
    const auto resumed = resumed_effect_publication(interrupted);
    auto       next    = resumed;
    attach_same_generation_lineage(next, *interrupted.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, next),
              ProgramTransitionPublishResult::Published);
}

// An execution lease is part of the durable state: it still fences ordinary publication after
// a restart, and the holder can still release it.
TEST_P(ProgramTransitionStoreConformance, ExecutionLeaseSurvivesRestart) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    auto start = start_publication();
    attach_initial_lineage(start);
    const auto lease = execution_lease(start);
    ASSERT_EQ(primary_store().compare_publish_execution("owner-a", "", start, std::nullopt, lease),
              ProgramTransitionPublishResult::Published);
    restart_store();
    auto& store = primary_store();
    ASSERT_TRUE(store.load_execution_lease("owner-a", "run-1"));
    EXPECT_EQ(store.load_execution_lease("owner-a", "run-1")->id(), lease.id());
    EXPECT_FALSE(store.load_execution_lease("owner-b", "run-1"));

    auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed);
    attach_same_generation_lineage(terminal, *start.run_lineage);
    EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, terminal),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.compare_publish_execution("owner-a", start.journal_record.id, terminal, lease,
                                              std::nullopt),
              ProgramTransitionPublishResult::Published);
    EXPECT_FALSE(store.load_execution_lease("owner-a", "run-1"));
}

// Fail closed and tamper detection: a stored record whose bytes are not canonical is never
// read as a valid state, and never extended.
TEST_P(ProgramTransitionStoreConformance, TamperedStoredRunRecordFailsClosed) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    const auto start = start_publication();
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    deployment().corrupt_run_record("owner-a", "run-1");
    restart_store();
    auto& store = primary_store();
    EXPECT_THROW((void)store.load("owner-a", "run-1"), std::exception);
    EXPECT_THROW((void)store.load_run_publication_head("owner-a", "run-1"), std::exception);
    const auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                               ContinuationState::Completed);
    EXPECT_THROW((void)store.compare_publish("owner-a", start.journal_record.id, terminal),
                 std::exception);
    EXPECT_FALSE(store.load("owner-b", "run-1"));
}

// Fail closed and tamper detection, key binding: a stored row whose identity column disagrees
// with the record in its bytes is never returned as that record.
TEST_P(ProgramTransitionStoreConformance, StoredGenerationRowMustAgreeWithItsRecord) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    auto start = start_publication();
    attach_initial_lineage(start);
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto lineage_id = start.run_lineage->lineage_id();
    ASSERT_TRUE(primary_store().load_generation("owner-a", lineage_id, 1));
    deployment().corrupt_generation_identity("owner-a", lineage_id);
    restart_store();
    EXPECT_THROW((void)primary_store().load_generation("owner-a", lineage_id, 1),
                 std::invalid_argument);
}

// Key binding for the execution lease: a stored lease whose id column disagrees with the lease
// in its bytes is never returned.
TEST_P(ProgramTransitionStoreConformance, StoredLeaseRowMustAgreeWithItsRecord) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    auto start = start_publication();
    attach_initial_lineage(start);
    const auto lease = execution_lease(start);
    ASSERT_EQ(primary_store().compare_publish_execution("owner-a", "", start, std::nullopt, lease),
              ProgramTransitionPublishResult::Published);
    ASSERT_TRUE(primary_store().load_execution_lease("owner-a", "run-1"));
    deployment().corrupt_lease_identity("owner-a", "run-1");
    restart_store();
    EXPECT_THROW((void)primary_store().load_execution_lease("owner-a", "run-1"),
                 std::invalid_argument);
}

// Key binding for the lineage: a stored head or history record that is not the lineage it is
// stored under is never returned.
TEST_P(ProgramTransitionStoreConformance, StoredLineageRecordsMustAgreeWithTheirKey) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    auto first = start_publication();
    attach_initial_lineage(first);
    auto second = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
    attach_initial_lineage(second);
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, first),
              ProgramTransitionPublishResult::Published);
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, second),
              ProgramTransitionPublishResult::Published);
    const auto first_lineage  = first.run_lineage->lineage_id();
    const auto second_lineage = second.run_lineage->lineage_id();
    ASSERT_TRUE(primary_store().load_lineage("owner-a", first_lineage));
    ASSERT_TRUE(primary_store().load_lineage_head("owner-a", first_lineage, first.run_lineage->id()));
    deployment().replace_lineage_records("owner-a", second_lineage, first_lineage);
    restart_store();
    EXPECT_THROW((void)primary_store().load_lineage("owner-a", first_lineage),
                 std::invalid_argument);
    EXPECT_THROW((void)primary_store().load_lineage_head("owner-a", first_lineage,
                                                         first.run_lineage->id()),
                 std::invalid_argument);
    EXPECT_TRUE(primary_store().load_lineage("owner-a", second_lineage));
}

// Fail closed: a stored command row whose coordinate column disagrees with the entry in its
// bytes is corruption, in a read and in the check that decides a publication. Every store
// raises the same typed error; none reads it as a refusal.
TEST_P(ProgramTransitionStoreConformance, StoredCommandRowMustAgreeWithItsCoordinate) {
    if (!deployment().durable()) GTEST_SKIP() << NOT_DURABLE;
    const auto start = start_publication();
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto pending =
        javascript_command_publication(start, javascript_command_entry(1, false), 20);
    ASSERT_EQ(primary_store().compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);
    deployment().corrupt_command_coordinate("owner-a", "run-1");
    restart_store();
    auto& store = primary_store();
    EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::invalid_argument);
    const auto completed =
        javascript_command_publication(pending, javascript_command_entry(2, true), 30);
    EXPECT_THROW((void)store.compare_publish("owner-a", pending.journal_record.id, completed),
                 std::invalid_argument);
    EXPECT_EQ(store.latest("owner-a", "run-1")->id, pending.journal_record.id);
}

// An expected head that cannot be a journal head matches no stored head, so the publication is
// refused without reading the state it would be compared with. A stored record that cannot be
// read does not turn that refusal into an error.
TEST_P(ProgramTransitionStoreConformance, MalformedExpectedHeadIsRefusedBeforeStateIsRead) {
    const auto start = start_publication();
    EXPECT_EQ(primary_store().compare_publish("owner-a", "not-a-journal-head", start),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_FALSE(primary_store().load("owner-a", "run-1"));
    ASSERT_EQ(primary_store().compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                               ContinuationState::Completed);
    EXPECT_EQ(primary_store().compare_publish("owner-a", "not-a-journal-head", terminal),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(primary_store().latest("owner-a", "run-1")->id, start.journal_record.id);
    if (!deployment().durable()) return;
    deployment().corrupt_run_record("owner-a", "run-1");
    restart_store();
    EXPECT_EQ(primary_store().compare_publish("owner-a", "not-a-journal-head", terminal),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_THROW((void)primary_store().compare_publish("owner-a", start.journal_record.id, terminal),
                 std::exception);
}

// A transition store that implements no JavaScript command journal read refuses to answer
// instead of reporting an empty journal.
TEST(ProgramTransitionStoreTest, UnimplementedDurableReadsFailClosed) {
    CommandReadUnsupportedStore    unsupported;
    BlockingProgramTransitionStore store(unsupported);
    EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::runtime_error);
    EXPECT_THROW((void)store.load_context_publications("owner-a", "run-1"), std::runtime_error);
    EXPECT_THROW((void)store.load_child_syntheses("owner-a", "run-1"), std::runtime_error);
    EXPECT_THROW((void)store.load_hook_outbox_entries("owner-a", "run-1"), std::runtime_error);
}

namespace {
// The parent's publication that records its child's result. `terminal_generation` is nonzero
// only for a result that belongs to a replacement generation of the child.
ProgramTransitionPublication child_result_publication(const ProgramTransitionPublication& previous,
                                                      std::uint64_t terminal_generation,
                                                      std::int64_t  time = 30) {
    auto children = previous.run_record.children();
    ProgramResultData child_result;
    child_result.status             = ProgramTerminalStatus::Completed;
    // A replacement generation's result is that run's, not the child's own.
    child_result.run_id = terminal_generation ? "child-run-1-replacement"
                                              : children.front().child_run_id;
    child_result.program_version_id = digest('1');
    child_result.bundle_id          = digest('2');
    child_result.operation_id       = "root";
    child_result.attempt            = 1;
    child_result.output             = json{{"child", true}};
    child_result.remaining_budget   = budget();
    child_result.checkpoint         = checkpoint();
    children.front().state               = ProgramChildState::Completed;
    children.front().terminal_result     = ProgramResult::create(std::move(child_result));
    children.front().terminal_generation = terminal_generation;

    auto journal = ProgramJournalRecord::create({previous.journal_record.id,
                                                 "run-1",
                                                 digest('1'),
                                                 digest('2'),
                                                 previous.journal_record.sequence + 1,
                                                 {"root", ContinuationState::Running, 1},
                                                 budget(),
                                                 {},
                                                 std::nullopt,
                                                 time});
    auto data            = copy_run_record_data(previous.run_record);
    data.children        = std::move(children);
    data.continuation    = journal.continuation;
    data.remaining_budget = journal.remaining_budget;
    data.journal_head    = journal.id;
    data.updated_at_ms   = time;
    return {ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
}

// A run that finished with its provider-budget custody, the exact record a recorded replay
// starts from. It has a lineage, which the replay debits.
ProgramTransitionPublication published_finished_source(BlockingProgramTransitionStore& store) {
    auto start = start_publication();
    attach_initial_lineage(start);
    if (store.compare_publish("owner-a", {}, start) != ProgramTransitionPublishResult::Published)
        throw std::runtime_error("could not publish the replay source");
    auto finished = terminal_publication(start, ProgramTerminalStatus::Completed,
                                         ContinuationState::Completed, 20, true);
    attach_same_generation_lineage(finished, *start.run_lineage);
    if (store.compare_publish("owner-a", start.journal_record.id, finished) !=
        ProgramTransitionPublishResult::Published)
        throw std::runtime_error("could not publish the finished replay source");
    return finished;
}

// The first publication of a recorded replay of `source`: a new run and lineage that take the
// source lineage's whole remainder, recorded as one debit of the source in the same unit.
ProgramTransitionPublication recorded_replay_publication(const ProgramTransitionPublication& source,
                                                         std::string run_id = "run-2") {
    auto target = start_publication_for(std::move(run_id), digest('1'), digest('2'), budget(), 30);
    auto data   = copy_run_record_data(target.run_record);
    data.recorded_binding_set_fingerprint = digest('f');
    target.run_record                     = ProgramRunRecord::create(std::move(data));
    attach_initial_lineage(target);
    const auto& previous                  = *source.run_lineage;
    target.recorded_replay_source = ProgramRecordedReplaySource{
        source.run_record.run_id(), source.run_record.id(), source.run_record.journal_head(),
        previous.id()};
    target.recorded_replay_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
        previous.owner_scope(), previous.lineage_id(), previous.root_run_id(),
        previous.active_generation(), previous.active_generation_id(),
        previous.active_run_record_id(), previous.active_journal_head(), RunBudget{},
        previous.inflight_reservation(), previous.id(), previous.created_at_ms(), 30,
        previous.committed_descendant_budget()});
    return target;
}
}  // namespace

// ---------------------------------------------------------------------------
// Golden verdicts. Each row publishes into an empty store and names what the last
// call returned or threw. The rows pin the validator's decisions, so a change to how
// a store reaches one has to change a row, and every backend has to agree on it.
// ---------------------------------------------------------------------------

namespace {
using Verdict = std::string;

Verdict verdict_of(ProgramTransitionPublishResult result) {
    switch (result) {
    case ProgramTransitionPublishResult::Published: return "Published";
    case ProgramTransitionPublishResult::AlreadyPresent: return "AlreadyPresent";
    case ProgramTransitionPublishResult::Conflict: return "Conflict";
    }
    return "Unknown";
}

template <typename Call>
Verdict verdict_of_call(Call&& call) {
    try {
        return verdict_of(call());
    } catch (const std::invalid_argument&) {
        return "throws invalid_argument";
    } catch (const std::runtime_error&) {
        return "throws runtime_error";
    } catch (...) {
        return "throws unexpected";
    }
}

struct GoldenRow {
    const char* name;
    Verdict     expected;
    std::function<Verdict(BlockingProgramTransitionStore&)> run;
};

ProgramTransitionPublication published(BlockingProgramTransitionStore&       store,
                                       ProgramTransitionPublication publication,
                                       std::string_view             expected_head = {}) {
    if (store.compare_publish("owner-a", expected_head, publication) !=
        ProgramTransitionPublishResult::Published)
        throw std::runtime_error("golden setup publication was not published");
    return publication;
}

ProgramTransitionPublication published_start(BlockingProgramTransitionStore& store, bool lineage = false) {
    auto start = start_publication();
    if (lineage) attach_initial_lineage(start);
    return published(store, std::move(start));
}

ProgramTransitionPublication published_interrupted_source(BlockingProgramTransitionStore& store) {
    auto source_start = published_start(store, true);
    auto source       = interrupted_effect_publication(source_start);
    attach_same_generation_lineage(source, *source_start.run_lineage);
    return published(store, std::move(source), source_start.journal_record.id);
}

ProgramTransitionPublication fork_of(const ProgramTransitionPublication& source,
                                     std::string                         run_id) {
    auto target = start_publication_for(std::move(run_id), digest('1'), digest('2'), budget(), 30);
    attach_fork_receipt(target);
    target.migration_plan = MigrationPlan::create(MigrationPlanData{
        digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
    attach_fork_split(target, *source.run_lineage);
    return target;
}

const std::vector<GoldenRow>& golden_rows() {
    static const std::vector<GoldenRow> rows{
        {"first publication", "Published",
         [](auto& store) {
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, start_publication()); });
         }},
        {"exact retry of the first publication", "AlreadyPresent",
         [](auto& store) {
             published_start(store);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, start_publication()); });
         }},
        {"first publication naming an expected head", "Conflict",
         [](auto& store) {
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", digest('9'), start_publication()); });
         }},
        {"first publication under another owner", "Conflict",
         [](auto& store) {
             return verdict_of_call([&] { return store.compare_publish("owner-b", {}, start_publication()); });
         }},
        {"increment against the head", "Published",
         [](auto& store) {
             const auto start = published_start(store);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", start.journal_record.id,
                     terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed));
             });
         }},
        {"exact retry of an increment", "AlreadyPresent",
         [](auto& store) {
             const auto start    = published_start(store);
             const auto terminal = published(
                 store, terminal_publication(start, ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish("owner-a", start.journal_record.id, terminal);
             });
         }},
        {"retry of an increment naming another head", "Conflict",
         [](auto& store) {
             const auto start    = published_start(store);
             const auto terminal = published(
                 store, terminal_publication(start, ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed),
                 start.journal_record.id);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", digest('9'), terminal); });
         }},
        {"increment against an unknown head", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", digest('9'),
                     terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed));
             });
         }},
        {"increment against a head that has moved", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             published(store,
                       terminal_publication(start, ProgramTerminalStatus::Completed,
                                            ContinuationState::Completed),
                       start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", start.journal_record.id,
                     terminal_publication(start, ProgramTerminalStatus::Failed,
                                          ContinuationState::Failed, 21));
             });
         }},
        {"event sequence out of order", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             auto       bad   = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                     ContinuationState::Completed);
             bad.events.front().sequence = 7;
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, bad); });
         }},
        {"terminal result without a terminal event", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             auto       bad   = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                     ContinuationState::Completed);
             bad.events.clear();
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, bad); });
         }},
        {"effect outbox naming another effect", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             auto       bad   = interrupted_effect_publication(start);
             bad.effects      = {ProgramEffectOutboxEntry(1, pending_effect("different-effect"))};
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, bad); });
         }},
        {"reused effect id", "Conflict",
         [](auto& store) {
             const auto start       = published_start(store);
             const auto interrupted = published(store, interrupted_effect_publication(start),
                                                 start.journal_record.id);
             const auto resumed     = published(store, resumed_effect_publication(interrupted),
                                                interrupted.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish("owner-a", resumed.journal_record.id,
                                              second_interruption_publication(resumed, "effect-one"));
             });
         }},
        {"command published before its predecessor", "Conflict",
         [](auto& store) {
             const auto start   = published_start(store);
             const auto pending = published(
                 store, javascript_command_publication(start, javascript_command_entry(1, false), 20),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", pending.journal_record.id,
                     javascript_command_publication(pending, javascript_command_entry(2, false, 2), 25));
             });
         }},
        {"command completed twice", "Conflict",
         [](auto& store) {
             const auto start   = published_start(store);
             const auto pending = published(
                 store, javascript_command_publication(start, javascript_command_entry(1, false), 20),
                 start.journal_record.id);
             const auto completed = published(
                 store, javascript_command_publication(pending, javascript_command_entry(2, true), 30),
                 pending.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", completed.journal_record.id,
                     javascript_command_publication(completed, javascript_command_entry(3, true), 40));
             });
         }},
        {"command completed with a conflicting payload", "Conflict",
         [](auto& store) {
             const auto start   = published_start(store);
             const auto pending = published(
                 store, javascript_command_publication(start, javascript_command_entry(1, false), 20),
                 start.journal_record.id);
             const auto conflicting = ProgramJavaScriptCommandJournalEntry(
                 ProgramJavaScriptCommandJournalEntryData{
                     2, digest('2'), 1,
                     JavaScriptCommand::call_core("journal:store", "main", json{{"input", 2}}),
                     digest('8'), json{{"status", "completed"}}});
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", pending.journal_record.id,
                     javascript_command_publication(pending, conflicting, 30));
             });
         }},
        {"command with a sequence gap", "Conflict",
         [](auto& store) {
             const auto start   = published_start(store);
             const auto pending = published(
                 store, javascript_command_publication(start, javascript_command_entry(1, false), 20),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", pending.journal_record.id,
                     javascript_command_publication(pending, javascript_command_entry(3, true), 30));
             });
         }},
        {"command with a stale ordinal", "Conflict",
         [](auto& store) {
             const auto start   = published_start(store);
             const auto pending = published(
                 store, javascript_command_publication(start, javascript_command_entry(1, false), 20),
                 start.journal_record.id);
             const auto completed = published(
                 store, javascript_command_publication(pending, javascript_command_entry(2, true), 30),
                 pending.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", completed.journal_record.id,
                     javascript_command_publication(completed, javascript_command_entry(3, false, 1), 40));
             });
         }},
        {"ordinary publication under a live lease", "Conflict",
         [](auto& store) {
             auto start = start_publication();
             attach_initial_lineage(start);
             store.compare_publish_execution("owner-a", "", start, std::nullopt, execution_lease(start));
             auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                  ContinuationState::Completed);
             attach_same_generation_lineage(terminal, *start.run_lineage);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, terminal); });
         }},
        {"release with a lease that is not live", "Conflict",
         [](auto& store) {
             auto start = start_publication();
             attach_initial_lineage(start);
             store.compare_publish_execution("owner-a", "", start, std::nullopt, execution_lease(start));
             auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                  ContinuationState::Completed);
             attach_same_generation_lineage(terminal, *start.run_lineage);
             return verdict_of_call([&] {
                 return store.compare_publish_execution("owner-a", start.journal_record.id, terminal,
                                                        execution_lease(start, digest('7')),
                                                        std::nullopt);
             });
         }},
        {"release with the live lease", "Published",
         [](auto& store) {
             auto start = start_publication();
             attach_initial_lineage(start);
             const auto lease = execution_lease(start);
             store.compare_publish_execution("owner-a", "", start, std::nullopt, lease);
             auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                  ContinuationState::Completed);
             attach_same_generation_lineage(terminal, *start.run_lineage);
             return verdict_of_call([&] {
                 return store.compare_publish_execution("owner-a", start.journal_record.id, terminal,
                                                        lease, std::nullopt);
             });
         }},
        {"exact retry of a release", "AlreadyPresent",
         [](auto& store) {
             auto start = start_publication();
             attach_initial_lineage(start);
             const auto lease = execution_lease(start);
             store.compare_publish_execution("owner-a", "", start, std::nullopt, lease);
             auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                  ContinuationState::Completed);
             attach_same_generation_lineage(terminal, *start.run_lineage);
             store.compare_publish_execution("owner-a", start.journal_record.id, terminal, lease,
                                             std::nullopt);
             return verdict_of_call([&] {
                 return store.compare_publish_execution("owner-a", start.journal_record.id, terminal,
                                                        lease, std::nullopt);
             });
         }},
        {"increment that drops the run's lineage", "Conflict",
         [](auto& store) {
             const auto start = published_start(store, true);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", start.journal_record.id,
                     terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed));
             });
         }},
        {"increment that keeps the run's lineage", "Published",
         [](auto& store) {
             const auto start    = published_start(store, true);
             auto       terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                        ContinuationState::Completed);
             attach_same_generation_lineage(terminal, *start.run_lineage);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, terminal); });
         }},
        {"legacy run adopting a lineage", "Published",
         [](auto& store) {
             const auto legacy   = published_start(store);
             auto       terminal = terminal_publication(legacy, ProgramTerminalStatus::Completed,
                                                        ContinuationState::Completed);
             attach_initial_lineage(terminal);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", legacy.journal_record.id, terminal); });
         }},
        {"successor generation without a receipt", "Conflict",
         [](auto& store) {
             const auto source = published_start(store, true);
             auto       target = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
             attach_successor_lineage(target, *source.run_lineage);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"successor lineage that raises the budget", "Conflict",
         [](auto& store) {
             const auto source   = published_start(store, true);
             auto       raised   = budget();
             ++raised.max_dynamic_compiles;
             auto target = start_publication_for("run-2", digest('7'), digest('8'), raised, 20);
             attach_successor_lineage(target, *source.run_lineage);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"another run taking over the lineage head", "Conflict",
         [](auto& store) {
             const auto source = published_start(store, true);
             auto       target = start_publication_for("run-2", digest('7'), digest('8'), budget(), 20);
             attach_same_generation_lineage(target, *source.run_lineage);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"context epoch that skips a sequence", "Conflict",
         [](auto& store) {
             auto initial                = start_publication();
             initial.context_publication = context_publication(1);
             published(store, initial);
             auto next = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
             next.context_publication =
                 context_publication(3, initial.context_publication->epoch.id());
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", initial.journal_record.id, next); });
         }},
        {"context epoch that carries hook output", "Conflict",
         [](auto& store) {
             auto hook = start_publication_for("run-2", digest('1'), digest('2'), budget(), 10);
             hook.context_publication =
                 context_publication(1, std::nullopt, {hook_output_artifact()}, "run-2");
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, hook); });
         }},
        {"hook transition that skips a state", "Conflict",
         [](auto& store) {
             const auto pending   = pending_hook_outbox_entry();
             const auto succeeded =
                 hook_outbox_entry_with_state(pending, neograph::HookExecutionState::Succeeded);
             auto initial                = start_publication();
             initial.hook_outbox_entries = {pending};
             published(store, initial);
             auto skipped = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
             skipped.hook_outbox_entries = {succeeded};
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", initial.journal_record.id, skipped); });
         }},
        {"fork with its source debit", "Published",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", {}, fork_of(source, "run-2")); });
         }},
        {"fork without its source debit", "Conflict",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             auto       target = fork_of(source, "run-2");
             target.fork_source_lineage.reset();
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"fork without a migration proof", "Conflict",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             auto       target = fork_of(source, "run-2");
             target.migration_plan.reset();
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"fork that debits the source's concurrency limit", "Conflict",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             auto target = start_publication_for("run-2", digest('1'), digest('2'), budget(), 30);
             attach_fork_receipt(target);
             target.migration_plan = MigrationPlan::create(MigrationPlanData{
                 digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {}, {}, {}});
             attach_fork_split(target, *source.run_lineage, /*debit_concurrency=*/true);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"fork from a source that is still running", "Conflict",
         [](auto& store) {
             const auto source = published_start(store, true);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", {}, fork_of(source, "run-2")); });
         }},
        {"child metadata published once and retried", "AlreadyPresent",
         [](auto& store) {
             const auto start = published_start(store);
             const auto child = published(store, child_metadata_publication(start), start.journal_record.id);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", start.journal_record.id, child); });
         }},
        {"child metadata that changes after publication", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             const auto child = published(store, child_metadata_publication(start), start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", child.journal_record.id,
                     child_metadata_publication(child, ProgramChildState::Publishing, 21,
                                                "different-child-trace"));
             });
         }},
        {"child removed from the record", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             const auto child = published(store, child_metadata_publication(start), start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", child.journal_record.id,
                     terminal_publication(child, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed, 30));
             });
         }},
        {"child with an illegal state transition", "Conflict",
         [](auto& store) {
             const auto start      = published_start(store);
             const auto dispatched = published(
                 store, child_metadata_publication(start, ProgramChildState::Dispatched),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish(
                     "owner-a", dispatched.journal_record.id,
                     child_metadata_publication(dispatched, ProgramChildState::Publishing, 30));
             });
         }},
        {"replacement at the exact checkpoint", "Published",
         [](auto& store) {
             const auto boundary = publish_replacement_boundary(store);
             const auto lineage  = store.load_run_lineage("owner-a", "run-1");
             const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
             auto target = start_publication_for(
                 "run-2", digest('7'), digest('8'),
                 program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40), 40,
                 json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}}, {"previous_run_id", "run-1"}});
             attach_replacement_successor(target, *lineage, *predecessor,
                                          boundary.publication.run_record, boundary.completed_checkpoint);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"replacement carrying another handoff", "Conflict",
         [](auto& store) {
             const auto boundary = publish_replacement_boundary(store);
             const auto lineage  = store.load_run_lineage("owner-a", "run-1");
             const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
             auto target = start_publication_for(
                 "run-2", digest('7'), digest('8'),
                 program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40), 40,
                 json{{"handoff", json{{"cursor", 8}, {"state", "wrong"}}}, {"previous_run_id", "run-1"}});
             attach_replacement_successor(target, *lineage, *predecessor,
                                          boundary.publication.run_record, boundary.completed_checkpoint);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"replacement from a recorded replay source", "Conflict",
         [](auto& store) {
             const auto boundary = publish_replacement_boundary(store, true);
             const auto lineage  = store.load_run_lineage("owner-a", "run-1");
             const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
             auto target = start_publication_for(
                 "run-2", digest('7'), digest('8'),
                 program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40), 40,
                 json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}}, {"previous_run_id", "run-1"}});
             attach_replacement_successor(target, *lineage, *predecessor,
                                          boundary.publication.run_record, boundary.completed_checkpoint);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"replacement that drops the source's context", "Conflict",
         [](auto& store) {
             const auto source_context = context_publication(1);
             const auto source_hook    = pending_hook_outbox_entry();
             const auto boundary =
                 publish_replacement_boundary(store, false, source_context, {source_hook});
             const auto lineage  = store.load_run_lineage("owner-a", "run-1");
             const auto predecessor = store.load_generation("owner-a", lineage->lineage_id(), 1);
             auto target = start_publication_for(
                 "run-2", digest('7'), digest('8'),
                 program_replacement_remaining_budget(boundary.publication.run_record, *lineage, 40), 40,
                 json{{"handoff", json{{"cursor", 7}, {"state", "ready"}}}, {"previous_run_id", "run-1"}});
             attach_replacement_successor(target, *lineage, *predecessor,
                                          boundary.publication.run_record, boundary.completed_checkpoint);
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, target); });
         }},
        {"fork from a source with an unresolved mandatory hook", "Conflict",
         [](auto& store) {
             auto source_start                = start_publication();
             source_start.hook_outbox_entries = {pending_hook_outbox_entry()};
             attach_initial_lineage(source_start);
             published(store, source_start);
             auto source = interrupted_effect_publication(source_start);
             attach_same_generation_lineage(source, *source_start.run_lineage);
             published(store, source, source_start.journal_record.id);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", {}, fork_of(source, "run-2")); });
         }},
        {"fork follow-up that repeats its migration proof", "Published",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             const auto fork   = published(store, fork_of(source, "run-2"));
             auto next = javascript_command_publication(fork, javascript_command_entry(1, false), 40);
             attach_same_generation_lineage(next, *fork.run_lineage);
             next.migration_plan = fork.migration_plan;
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", fork.journal_record.id, next); });
         }},
        {"fork follow-up that carries another migration proof", "Conflict",
         [](auto& store) {
             const auto source = published_interrupted_source(store);
             const auto fork   = published(store, fork_of(source, "run-2"));
             auto next = javascript_command_publication(fork, javascript_command_entry(1, false), 40);
             attach_same_generation_lineage(next, *fork.run_lineage);
             next.migration_plan = MigrationPlan::create(MigrationPlanData{
                 digest('1'), digest('1'), "owner-a", MigrationCompatibility::ForkCompatible, {},
                 {MigrationDiagnostic{MigrationDimension::Budget, "budget_unchanged",
                                      "the target keeps the source budget", json::object(),
                                      json::object()}},
                 {}});
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", fork.journal_record.id, next); });
         }},
        {"child result without a replacement generation", "Published",
         [](auto& store) {
             const auto start = published_start(store);
             const auto child = published(
                 store, child_metadata_publication(start, ProgramChildState::Dispatched),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish("owner-a", child.journal_record.id,
                                              child_result_publication(child, 0));
             });
         }},
        {"child result naming a generation that is not stored", "Conflict",
         [](auto& store) {
             const auto start = published_start(store);
             const auto child = published(
                 store, child_metadata_publication(start, ProgramChildState::Dispatched),
                 start.journal_record.id);
             return verdict_of_call([&] {
                 return store.compare_publish("owner-a", child.journal_record.id,
                                              child_result_publication(child, 2));
             });
         }},
        {"recorded replay taking the source's whole remainder", "Published",
         [](auto& store) {
             const auto source = published_finished_source(store);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", {}, recorded_replay_publication(source)); });
         }},
        {"exact retry of a recorded replay", "AlreadyPresent",
         [](auto& store) {
             const auto source = published_finished_source(store);
             const auto replay = published(store, recorded_replay_publication(source));
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, replay); });
         }},
        {"recorded replay naming a stale source lineage head", "Conflict",
         [](auto& store) {
             const auto source = published_finished_source(store);
             auto       replay = recorded_replay_publication(source);
             replay.recorded_replay_source->lineage_head_id = digest('a');
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, replay); });
         }},
        {"recorded replay that debits less than it takes", "Conflict",
         [](auto& store) {
             const auto source = published_finished_source(store);
             auto       replay = recorded_replay_publication(source);
             const auto& debited = *replay.recorded_replay_source_lineage;
             replay.recorded_replay_source_lineage = ProgramRunLineage::create(ProgramRunLineageData{
                 debited.owner_scope(), debited.lineage_id(), debited.root_run_id(),
                 debited.active_generation(), debited.active_generation_id(),
                 debited.active_run_record_id(), debited.active_journal_head(),
                 source.run_lineage->remaining_budget(), debited.inflight_reservation(),
                 debited.predecessor_head_id(), debited.created_at_ms(), debited.updated_at_ms(),
                 debited.committed_descendant_budget()});
             return verdict_of_call([&] { return store.compare_publish("owner-a", {}, replay); });
         }},
        {"recorded replay of a run that has not finished", "Conflict",
         [](auto& store) {
             const auto source = published_start(store, true);
             return verdict_of_call(
                 [&] { return store.compare_publish("owner-a", {}, recorded_replay_publication(source)); });
         }},
    };
    return rows;
}
}  // namespace

TEST_P(ProgramTransitionStoreConformance, GoldenVerdictsAreTheSameOnEveryBackend) {
    for (const auto& row : golden_rows()) {
        SCOPED_TRACE(row.name);
        StoreDeployment dep(GetParam().backend, GetParam().form);
        const auto      store = dep.blocking_transition_store();
        EXPECT_EQ(row.run(*store), row.expected) << row.name;
    }
}

// Error kinds. A refusal is a Conflict, never an exception; a failed write is a
// runtime_error; and nothing that is thrown leaves a partial publication behind.
TEST_P(ProgramTransitionStoreConformance, ErrorsAreConflictsOrRuntimeErrors) {
    StoreDeployment dep(GetParam().backend, GetParam().form);
    const auto      store = dep.blocking_transition_store();
    const auto      start = start_publication();
    dep.arm_fault(WriteStage::Run);
    EXPECT_EQ(verdict_of_call([&] { return store->compare_publish("owner-a", {}, start); }),
              "throws runtime_error");
    dep.disarm_faults();
    EXPECT_FALSE(store->load("owner-a", "run-1"));
    EXPECT_EQ(verdict_of_call([&] { return store->compare_publish("owner-a", {}, start); }),
              "Published");

    auto mismatched = start;
    mismatched.events.front().sequence = 2;
    EXPECT_EQ(verdict_of_call([&] { return store->compare_publish("owner-a", {}, mismatched); }),
              "Conflict");
    EXPECT_EQ(verdict_of_call([&] { return store->compare_publish("owner-a", "not-a-head", start); }),
              "Conflict");
}

namespace {
class CountingTransitionReads final : public neograph::program::detail::transition::TransitionReads {
public:
    std::size_t                                               command_log_reads    = 0;
    std::size_t                                               latest_command_reads = 0;
    std::vector<ProgramJavaScriptCommandJournalEntry>         commands;
    std::vector<ProgramContextPublication>                    contexts;
    std::vector<ProgramChildSynthesisRecord>                  syntheses;
    std::vector<neograph::HookOutboxEntry>                    hooks;

    std::optional<GraphMigrationCapsule> graph_migration_capsule(std::string_view,
                                                                 std::string_view) override {
        return std::nullopt;
    }
    bool effect_id_published(std::string_view, std::string_view) override {
        return false;
    }
    neograph::program::detail::transition::Borrowed<
        std::vector<ProgramJavaScriptCommandJournalEntry>>
    command_log(std::string_view) override {
        ++command_log_reads;
        return neograph::program::detail::transition::Borrowed<
            std::vector<ProgramJavaScriptCommandJournalEntry>>(commands);
    }
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(std::string_view) override {
        ++latest_command_reads;
        if (commands.empty()) return std::nullopt;
        return commands.back();
    }
    std::optional<ProgramRunRecord> run_record(std::string_view, Lock) override {
        return std::nullopt;
    }
    std::optional<ProgramRunLineage> lineage_head(std::string_view, Lock) override {
        return std::nullopt;
    }
    std::optional<ProgramRunGeneration> generation(std::string_view, std::uint64_t) override {
        return std::nullopt;
    }
    std::optional<std::string> run_lineage_id(std::string_view) override {
        return std::nullopt;
    }
    neograph::program::detail::transition::Borrowed<std::vector<ProgramContextPublication>>
    context_history(std::string_view) override {
        return neograph::program::detail::transition::Borrowed<
            std::vector<ProgramContextPublication>>(contexts);
    }
    neograph::program::detail::transition::Borrowed<std::vector<ProgramChildSynthesisRecord>>
    synthesis_heads(std::string_view) override {
        return neograph::program::detail::transition::Borrowed<
            std::vector<ProgramChildSynthesisRecord>>(syntheses);
    }
    neograph::program::detail::transition::Borrowed<std::vector<neograph::HookOutboxEntry>>
    hook_heads(std::string_view, const ProgramRunRecord&) override {
        return neograph::program::detail::transition::Borrowed<
            std::vector<neograph::HookOutboxEntry>>(hooks);
    }
};
}  // namespace

TEST(ProgramTransitionStoreTest, ValidIncrementReadsCommandLogOnlyWhenNeeded) {
    namespace transition = neograph::program::detail::transition;

    CountingTransitionReads reads;
    const auto              start = start_publication();
    const auto check_increment    = [&](const ProgramTransitionPublication& old_pub,
                                        const ProgramTransitionPublication& next_pub) {
        const transition::CurrentHead old_head{
            old_pub.run_record, old_pub.journal_record, old_pub.migration_plan, {}};
        return transition::valid_increment(reads, old_head, next_pub, old_pub.journal_record.id,
                                           nullptr, nullptr, nullptr);
    };

    // A publication without commands and without a budget increase does not read the command log.
    const auto child_pub = child_metadata_publication(start, ProgramChildState::Publishing, 20);
    EXPECT_TRUE(check_increment(start, child_pub));
    EXPECT_EQ(reads.command_log_reads, 0U);
    EXPECT_EQ(reads.latest_command_reads, 0U);

    const auto dispatched_pub =
        child_metadata_publication(child_pub, ProgramChildState::Dispatched, 25);
    EXPECT_TRUE(check_increment(child_pub, dispatched_pub));
    EXPECT_EQ(reads.command_log_reads, 0U);
    EXPECT_EQ(reads.latest_command_reads, 0U);

    const auto terminal_pub =
        terminal_publication(start, ProgramTerminalStatus::Completed, ContinuationState::Completed, 30);
    EXPECT_TRUE(check_increment(start, terminal_pub));
    EXPECT_EQ(reads.command_log_reads, 0U);
    EXPECT_EQ(reads.latest_command_reads, 0U);

    // A publication that carries a command reads latest_command once and command_log zero times.
    const auto pending_entry = javascript_command_entry(1, false, 1);
    const auto pending_pub   = javascript_command_publication(start, pending_entry, 20);
    EXPECT_TRUE(check_increment(start, pending_pub));
    EXPECT_EQ(reads.latest_command_reads, 1U);
    EXPECT_EQ(reads.command_log_reads, 0U);

    reads.commands.push_back(pending_entry);

    // A publication without commands that raises the remaining budget reads latest_command
    // and is rejected because no command completed to settle an in-flight reservation.
    auto raised_budget = start.journal_record.remaining_budget;
    ++raised_budget.wall_time_ms;
    auto raised_journal = ProgramJournalRecord::create({pending_pub.journal_record.id,
                                                        "run-1",
                                                        digest('1'),
                                                        digest('2'),
                                                        pending_pub.journal_record.sequence + 1,
                                                        {"root", ContinuationState::Running, 1},
                                                        raised_budget,
                                                        {},
                                                        std::nullopt,
                                                        25});
    auto raised_data             = copy_run_record_data(pending_pub.run_record);
    raised_data.remaining_budget = raised_budget;
    raised_data.journal_head     = raised_journal.id;
    raised_data.updated_at_ms    = 25;
    const ProgramTransitionPublication raised_pub{
        ProgramRunRecord::create(std::move(raised_data)), std::move(raised_journal), {}, {}};
    EXPECT_FALSE(check_increment(pending_pub, raised_pub));
    EXPECT_EQ(reads.latest_command_reads, 2U);
    EXPECT_EQ(reads.command_log_reads, 0U);

    // Reject cases on the command log and child transitions remain rejected.
    const auto conflicting_payload_entry = ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData{
            2, digest('2'), 1,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 2}}),
            digest('8'), json{{"status", "completed"}}});
    const auto conflicting_payload_pub =
        javascript_command_publication(pending_pub, conflicting_payload_entry, 30);
    EXPECT_FALSE(check_increment(pending_pub, conflicting_payload_pub));

    const auto new_ordinal_while_pending_pub =
        javascript_command_publication(pending_pub, javascript_command_entry(2, false, 2), 30);
    EXPECT_FALSE(check_increment(pending_pub, new_ordinal_while_pending_pub));

    const auto sequence_gap_pub =
        javascript_command_publication(pending_pub, javascript_command_entry(3, true, 1), 30);
    EXPECT_FALSE(check_increment(pending_pub, sequence_gap_pub));

    const auto completed_entry = javascript_command_entry(2, true, 1);
    const auto completed_pub   = javascript_command_publication(pending_pub, completed_entry, 30);
    EXPECT_TRUE(check_increment(pending_pub, completed_pub));
    reads.commands.push_back(completed_entry);

    const auto second_completion_pub =
        javascript_command_publication(completed_pub, javascript_command_entry(3, true, 1), 40);
    EXPECT_FALSE(check_increment(completed_pub, second_completion_pub));

    const auto stale_ordinal_pub =
        javascript_command_publication(completed_pub, javascript_command_entry(3, false, 1), 40);
    EXPECT_FALSE(check_increment(completed_pub, stale_ordinal_pub));
    EXPECT_EQ(reads.command_log_reads, 0U);

    // Reservation settlement (with command completion) and interrupted CallCore reservation
    // release (without commands, raising remaining_budget) both validate from latest_command.
    auto available                   = budget();
    available.wall_time_ms           = 800;
    available.max_program_operations = 0;
    RunBudget reservation;
    reservation.wall_time_ms = 200;
    const auto reserved_pending_entry = javascript_command_entry(3, false, 2);
    const auto reserved_pending_pub   = javascript_command_publication(
        completed_pub, reserved_pending_entry, 45, available, reservation);
    EXPECT_TRUE(check_increment(completed_pub, reserved_pending_pub));
    reads.commands.push_back(reserved_pending_entry);

    auto call_core_checkpoint = checkpoint();
    {
        std::string identity = "run-1";
        identity.push_back('\0');
        identity.append("root.javascript.2");
        identity.push_back('\0');
        identity.append(digest('4'));
        call_core_checkpoint.core_thread_id =
            detail::sha256_identity("program-core-thread/v1", identity);
    }
    call_core_checkpoint.checkpoint_id = "checkpoint-2";
    auto settled                       = available;
    settled.wall_time_ms               = 950;
    const ProgramUsage usage{50, 0, 0, 0, 0, 0};
    auto               release_journal = ProgramJournalRecord::create(
        {reserved_pending_pub.journal_record.id,
         "run-1",
         digest('1'),
         digest('2'),
         reserved_pending_pub.journal_record.sequence + 1,
         {"root", ContinuationState::Interrupted, 1},
         settled,
         {},
         call_core_checkpoint,
         50});
    ProgramPendingInputData pending_data;
    pending_data.operation_id         = "root.javascript.2";
    pending_data.call_id              = "call-1";
    pending_data.kind                 = ProgramPendingInputKind::Input;
    pending_data.result_schema        = json::object();
    pending_data.payload              = json::object();
    pending_data.core_node            = "main";
    pending_data.core_interrupt_value = json::object();
    auto              pending_input   = ProgramPendingInput(std::move(pending_data));
    ProgramResultData result_data;
    result_data.status             = ProgramTerminalStatus::Interrupted;
    result_data.run_id             = "run-1";
    result_data.program_version_id = digest('1');
    result_data.bundle_id          = digest('2');
    result_data.attempt            = 1;
    result_data.usage              = usage;
    result_data.remaining_budget   = settled;
    result_data.checkpoint         = call_core_checkpoint;
    result_data.interrupt =
        ProgramInterrupt{"main", json::object(), pending_input, std::nullopt};
    auto release_data             = copy_run_record_data(reserved_pending_pub.run_record);
    release_data.continuation     = release_journal.continuation;
    release_data.remaining_budget = settled;
    release_data.exact_checkpoint = call_core_checkpoint;
    release_data.pending_input    = pending_input;
    release_data.terminal_result  = ProgramResult::create(std::move(result_data));
    release_data.journal_head     = release_journal.id;
    release_data.event_sequence   = reserved_pending_pub.run_record.event_sequence() + 2;
    release_data.updated_at_ms    = 50;
    const ProgramTransitionPublication release_pub{
        ProgramRunRecord::create(std::move(release_data)),
        std::move(release_journal),
        {event(reserved_pending_pub.run_record.event_sequence() + 1,
               ProgramEventKind::CheckpointPublished,
               ProgramCheckpointEvent{call_core_checkpoint}, 50),
         event(reserved_pending_pub.run_record.event_sequence() + 2, ProgramEventKind::Terminal,
               ProgramTerminalEvent{ProgramTerminalStatus::Interrupted}, 50)},
        {}};
    const auto reads_before_release = reads.latest_command_reads;
    EXPECT_TRUE(check_increment(reserved_pending_pub, release_pub));
    EXPECT_EQ(reads.latest_command_reads, reads_before_release + 1U);
    EXPECT_EQ(reads.command_log_reads, 0U);

    const auto settled_command_entry = ProgramJavaScriptCommandJournalEntry(
        ProgramJavaScriptCommandJournalEntryData{
            4, digest('2'), 2,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}),
            digest('8'),
            json{{"status", "completed"},
                 {"usage", json{{"wall_time_ms", 50U},
                                {"model_tokens", 0U},
                                {"monetary_microunits", 0U},
                                {"program_operations", 1U},
                                {"core_steps", 0U},
                                {"peak_concurrency", 0U}}}}});
    auto over_refund         = settled;
    over_refund.wall_time_ms = 951;
    EXPECT_FALSE(check_increment(
        reserved_pending_pub,
        javascript_command_publication(reserved_pending_pub, settled_command_entry, 50,
                                       over_refund, RunBudget{})));
    const auto reads_before_settle = reads.latest_command_reads;
    EXPECT_TRUE(check_increment(
        reserved_pending_pub,
        javascript_command_publication(reserved_pending_pub, settled_command_entry, 50, settled,
                                       RunBudget{})));
    EXPECT_EQ(reads.latest_command_reads, reads_before_settle + 1U);
    EXPECT_EQ(reads.command_log_reads, 0U);
}

TEST(ProgramTransitionStoreTest, ValidCommandHistoryAppendAfterEquivalence) {
    namespace transition = neograph::program::detail::transition;

    const auto start = start_publication();
    const auto& run  = start.run_record;

    const auto expect_same = [&](
                                 const std::vector<ProgramJavaScriptCommandJournalEntry>& log,
                                 const std::vector<ProgramJavaScriptCommandJournalEntry>& appended,
                                 bool expected) {
        const auto last =
            log.empty() ? std::nullopt
                        : std::optional<ProgramJavaScriptCommandJournalEntry>(log.back());
        const bool full = transition::valid_command_history_append(run, log, appended);
        const bool tail = transition::valid_command_history_append_after(run, last, appended);
        EXPECT_EQ(full, expected);
        EXPECT_EQ(tail, full);
    };

    const auto append_accepted =
        [&](std::vector<ProgramJavaScriptCommandJournalEntry>        log,
            const std::vector<ProgramJavaScriptCommandJournalEntry>& appended) {
            const auto last =
                log.empty() ? std::nullopt
                            : std::optional<ProgramJavaScriptCommandJournalEntry>(log.back());
            EXPECT_TRUE(transition::valid_command_history_append(run, log, appended));
            EXPECT_TRUE(transition::valid_command_history_append_after(run, last, appended));
            log.insert(log.end(), appended.begin(), appended.end());
            return log;
        };

    const auto join_cmd = JavaScriptCommand::join(
        "journal:join", "all",
        {JavaScriptCommand::call_core("journal:m0", "main", json{{"input", 1}}),
         JavaScriptCommand::call_core("journal:m1", "main", json{{"input", 2}})});
    const auto await_cmd = JavaScriptCommand::await(
        "journal:await",
        JavaScriptCommand::call_core("journal:inner", "main", json{{"input", 1}}), 500);

    // 1. Empty log.
    const std::vector<ProgramJavaScriptCommandJournalEntry> empty_log;
    expect_same(empty_log, {}, true);
    expect_same(empty_log, {javascript_command_entry(1, false, 1)}, true);
    expect_same(empty_log, {javascript_command_entry(1, true, 1)}, false);
    expect_same(empty_log, {javascript_command_entry(1, false, 2)}, false);
    expect_same(empty_log, {javascript_command_entry(2, false, 1)}, false);

    // 2. Pending last entry, and 3. Completed last entry (built only via accepted appends).
    const auto pending_log   = append_accepted(empty_log, {javascript_command_entry(1, false, 1)});
    const auto completed_log = append_accepted(pending_log, {javascript_command_entry(2, true, 1)});
    const auto two_ordinals_pending_log =
        append_accepted(completed_log, {javascript_command_entry(3, false, 2)});
    const auto two_ordinals_completed_log =
        append_accepted(two_ordinals_pending_log, {javascript_command_entry(4, true, 2)});
    const auto three_ordinals_pending_log =
        append_accepted(two_ordinals_completed_log, {javascript_command_entry(5, false, 3)});

    // 4. New ordinal = highest + 1.
    expect_same(completed_log, {javascript_command_entry(3, false, 2)}, true);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, false, 3)}, true);
    expect_same(three_ordinals_pending_log, {javascript_command_entry(6, true, 3)}, true);
    expect_same(pending_log, {javascript_command_entry(2, false, 2)}, false);
    expect_same(three_ordinals_pending_log, {javascript_command_entry(6, false, 4)}, false);

    // 5. New ordinal skipping one.
    expect_same(empty_log, {javascript_command_entry(1, false, 2)}, false);
    expect_same(completed_log, {javascript_command_entry(3, false, 3)}, false);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, false, 4)}, false);

    // 6. Repeated old ordinal whose coordinate is completed.
    expect_same(completed_log, {javascript_command_entry(3, false, 1)}, false);
    expect_same(completed_log, {javascript_command_entry(3, true, 1)}, false);
    expect_same(two_ordinals_pending_log, {javascript_command_entry(4, false, 1)}, false);
    expect_same(two_ordinals_pending_log, {javascript_command_entry(4, true, 1)}, false);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, false, 1)}, false);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, true, 1)}, false);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, false, 2)}, false);
    expect_same(three_ordinals_pending_log, {javascript_command_entry(6, true, 1)}, false);
    expect_same(three_ordinals_pending_log, {javascript_command_entry(6, true, 2)}, false);

    // 7. Settled members on a pending join.
    const ProgramJavaScriptCommandJournalEntry join_pending(
        ProgramJavaScriptCommandJournalEntryData{3, digest('2'), 2, join_cmd, digest('8'),
                                                 std::nullopt});
    const ProgramJavaScriptCommandJournalEntry join_settled_initial(
        ProgramJavaScriptCommandJournalEntryData{
            3, digest('2'), 2, join_cmd, digest('8'), std::nullopt,
            json{{"root.javascript.2/member/0", json{{"status", "completed"}}}}});
    expect_same(completed_log, {join_settled_initial}, false);
    const auto join_pending_log = append_accepted(completed_log, {join_pending});
    const ProgramJavaScriptCommandJournalEntry join_settled(
        ProgramJavaScriptCommandJournalEntryData{
            4, digest('2'), 2, join_cmd, digest('8'), std::nullopt,
            json{{"root.javascript.2/member/0", json{{"status", "completed"}}}}});
    expect_same(join_pending_log, {join_settled}, true);
    const auto join_settled_log = append_accepted(join_pending_log, {join_settled});
    const ProgramJavaScriptCommandJournalEntry join_completed(
        ProgramJavaScriptCommandJournalEntryData{
            5, digest('2'), 2, join_cmd, digest('8'), json{{"status", "completed"}}});
    expect_same(join_settled_log, {join_completed}, true);

    // 8. An await start.
    const ProgramJavaScriptCommandJournalEntry await_pending(
        ProgramJavaScriptCommandJournalEntryData{3, digest('2'), 2, await_cmd, digest('8'),
                                                 std::nullopt});
    const ProgramJavaScriptCommandJournalEntry await_start_initial(
        ProgramJavaScriptCommandJournalEntryData{
            3, digest('2'), 2, await_cmd, digest('8'), std::nullopt, std::nullopt,
            json{{"root.javascript.2/await", 1000}}});
    expect_same(completed_log, {await_start_initial}, false);
    const auto await_pending_log = append_accepted(completed_log, {await_pending});
    const ProgramJavaScriptCommandJournalEntry await_started(
        ProgramJavaScriptCommandJournalEntryData{
            4, digest('2'), 2, await_cmd, digest('8'), std::nullopt, std::nullopt,
            json{{"root.javascript.2/await", 1000}}});
    expect_same(await_pending_log, {await_started}, true);
    const auto await_started_log = append_accepted(await_pending_log, {await_started});
    const ProgramJavaScriptCommandJournalEntry await_completed(
        ProgramJavaScriptCommandJournalEntryData{
            5, digest('2'), 2, await_cmd, digest('8'), json{{"status", "completed"}}});
    expect_same(await_started_log, {await_completed}, true);
    const ProgramJavaScriptCommandJournalEntry await_ord1_pending(
        ProgramJavaScriptCommandJournalEntryData{1, digest('2'), 1, await_cmd, digest('8'),
                                                 std::nullopt});
    const ProgramJavaScriptCommandJournalEntry await_ord1_started(
        ProgramJavaScriptCommandJournalEntryData{
            2, digest('2'), 1, await_cmd, digest('8'), std::nullopt, std::nullopt,
            json{{"root.javascript.1/await", 1000}}});
    const ProgramJavaScriptCommandJournalEntry await_ord1_completed(
        ProgramJavaScriptCommandJournalEntryData{
            3, digest('2'), 1, await_cmd, digest('8'), json{{"status", "completed"}}});
    const auto await_ord1_pending_log = append_accepted(empty_log, {await_ord1_pending});
    const auto await_ord1_started_log =
        append_accepted(await_ord1_pending_log, {await_ord1_started});
    expect_same(await_ord1_started_log, {await_ord1_completed}, true);

    // 9. A second completion.
    expect_same(completed_log, {javascript_command_entry(3, true, 1)}, false);
    expect_same(two_ordinals_completed_log, {javascript_command_entry(5, true, 2)}, false);

    // 10. A conflicting payload for the same ordinal.
    const ProgramJavaScriptCommandJournalEntry conflicting_payload(
        ProgramJavaScriptCommandJournalEntryData{
            2, digest('2'), 1,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 99}}),
            digest('8'), json{{"status", "completed"}}});
    expect_same(pending_log, {conflicting_payload}, false);

    // 11. A sequence gap in the new entries.
    expect_same(empty_log, {javascript_command_entry(2, false, 1)}, false);
    expect_same(pending_log, {javascript_command_entry(3, true, 1)}, false);
    expect_same(completed_log,
                {javascript_command_entry(3, false, 2), javascript_command_entry(5, true, 2)},
                false);

    // 12. A wrong bundle id.
    const ProgramJavaScriptCommandJournalEntry wrong_bundle_initial(
        ProgramJavaScriptCommandJournalEntryData{
            1, digest('9'), 1,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}),
            digest('8'), std::nullopt});
    const ProgramJavaScriptCommandJournalEntry wrong_bundle_complete(
        ProgramJavaScriptCommandJournalEntryData{
            2, digest('9'), 1,
            JavaScriptCommand::call_core("journal:store", "main", json{{"input", 1}}),
            digest('8'), json{{"status", "completed"}}});
    expect_same(empty_log, {wrong_bundle_initial}, false);
    expect_same(pending_log, {wrong_bundle_complete}, false);

    // 13. Two (and three) new entries in one publication.
    expect_same(empty_log,
                {javascript_command_entry(1, false, 1), javascript_command_entry(2, true, 1)},
                true);
    expect_same(pending_log,
                {javascript_command_entry(2, true, 1), javascript_command_entry(3, false, 2)},
                true);
    expect_same(completed_log,
                {javascript_command_entry(3, false, 2), javascript_command_entry(4, true, 2)},
                true);
    expect_same(completed_log, {join_pending, join_settled, join_completed}, true);
    expect_same(completed_log, {await_pending, await_started, await_completed}, true);
    expect_same(empty_log,
                {javascript_command_entry(1, false, 1), javascript_command_entry(2, true, 1),
                 javascript_command_entry(3, true, 1)},
                false);
    expect_same(completed_log,
                {javascript_command_entry(3, false, 2), javascript_command_entry(4, false, 3)},
                false);
    expect_same(completed_log,
                {javascript_command_entry(3, false, 2), javascript_command_entry(4, true, 1)},
                false);

    // Call-counting TransitionReads verification on a publication with commands, plus
    // disagreement / nullopt checks on latest_command and valid_command_reservation_transition.
    EXPECT_FALSE(transition::valid_command_history_append_after(
        run, std::nullopt, {javascript_command_entry(3, false, 2)}));
    EXPECT_FALSE(transition::valid_command_history_append_after(
        run, pending_log.front(), {javascript_command_entry(3, false, 2)}));
    const auto ordinary_child_pub = child_metadata_publication(start);
    EXPECT_TRUE(transition::valid_command_reservation_transition(
        start.run_record, start.journal_record, ordinary_child_pub.journal_record, std::nullopt,
        ordinary_child_pub, nullptr, nullptr, nullptr));

    CountingTransitionReads reads;
    reads.commands = completed_log;
    const auto second_Completed_pub = javascript_command_publication(
        javascript_command_publication(start, pending_log.front(), 20), completed_log.back(), 30);
    const auto third_pending_pub = javascript_command_publication(
        second_Completed_pub, javascript_command_entry(3, false, 2), 40);
    const transition::CurrentHead head{second_Completed_pub.run_record,
                                       second_Completed_pub.journal_record,
                                       second_Completed_pub.migration_plan,
                                       {}};
    EXPECT_TRUE(transition::valid_increment(reads, head, third_pending_pub,
                                            second_Completed_pub.journal_record.id, nullptr,
                                            nullptr, nullptr));
    EXPECT_EQ(reads.latest_command_reads, 1U);
    EXPECT_EQ(reads.command_log_reads, 0U);
}

TEST(ProgramTransitionStoreTest, ChildIndexPreservesValidationAndRejectsDuplicateChildRunIds) {
    namespace transition = neograph::program::detail::transition;

    const auto start = start_publication();
    const auto make_child = [](std::string id, ProgramChildState state) {
        return ProgramChildRecord{
            std::move(id), digest('7'), "link-receipt",
            ProgramPersistedInvocation{{{"child", true}}, budget(), "child-trace", "run-1", 1},
            state};
    };

    // ProgramRunRecord::create rejects duplicate child_run_id entries.
    {
        auto duplicate_data     = copy_run_record_data(start.run_record);
        duplicate_data.children = {make_child("child-run-1", ProgramChildState::Publishing),
                                   make_child("child-run-1", ProgramChildState::Dispatched)};
        EXPECT_THROW((void)ProgramRunRecord::create(std::move(duplicate_data)),
                     std::invalid_argument);
    }

    // Multiple children validate state transitions, removal rejection, and result binding via the index.
    const auto with_children = [&](const ProgramTransitionPublication& previous,
                                   std::vector<ProgramChildRecord>     children,
                                   std::int64_t                        time) {
        auto journal = ProgramJournalRecord::create({previous.journal_record.id,
                                                     "run-1",
                                                     digest('1'),
                                                     digest('2'),
                                                     previous.journal_record.sequence + 1,
                                                     {"root", ContinuationState::Running, 1},
                                                     budget(),
                                                     {},
                                                     std::nullopt,
                                                     time});
        auto data             = copy_run_record_data(previous.run_record);
        data.children         = std::move(children);
        data.continuation     = journal.continuation;
        data.remaining_budget = journal.remaining_budget;
        data.journal_head     = journal.id;
        data.updated_at_ms    = time;
        return ProgramTransitionPublication{
            ProgramRunRecord::create(std::move(data)), std::move(journal), {}, {}};
    };

    CountingTransitionReads reads;
    const auto check_increment = [&](const ProgramTransitionPublication& old_pub,
                                     const ProgramTransitionPublication& next_pub) {
        const transition::CurrentHead old_head{
            old_pub.run_record, old_pub.journal_record, old_pub.migration_plan, {}};
        return transition::valid_increment(reads, old_head, next_pub, old_pub.journal_record.id,
                                           nullptr, nullptr, nullptr);
    };

    const auto pub_two = with_children(
        start,
        {make_child("child-run-1", ProgramChildState::Publishing),
         make_child("child-run-2", ProgramChildState::Dispatched)},
        20);
    EXPECT_TRUE(check_increment(start, pub_two));
    const transition::CurrentHead start_head{
        start.run_record, start.journal_record, start.migration_plan, {}};
    EXPECT_TRUE(transition::child_results_bind(reads, "owner-a", &start_head, pub_two));

    // Removing a child is rejected.
    const auto removed_second = with_children(
        pub_two, {make_child("child-run-1", ProgramChildState::Dispatched)}, 30);
    EXPECT_FALSE(check_increment(pub_two, removed_second));

    // Illegal state transition (Dispatched -> Publishing) on the second child is rejected.
    const auto illegal_second = with_children(
        pub_two,
        {make_child("child-run-1", ProgramChildState::Dispatched),
         make_child("child-run-2", ProgramChildState::Publishing)},
        30);
    EXPECT_FALSE(check_increment(pub_two, illegal_second));

    // Completing the second child while advancing the first to Dispatched is accepted.
    auto completed_child_2 = make_child("child-run-2", ProgramChildState::Completed);
    ProgramResultData child_2_result;
    child_2_result.status             = ProgramTerminalStatus::Completed;
    child_2_result.run_id             = "child-run-2";
    child_2_result.program_version_id = digest('1');
    child_2_result.bundle_id          = digest('2');
    child_2_result.operation_id       = "root";
    child_2_result.attempt            = 1;
    child_2_result.output             = json{{"child", 2}};
    child_2_result.remaining_budget   = budget();
    child_2_result.checkpoint         = checkpoint();
    completed_child_2.terminal_result = ProgramResult::create(std::move(child_2_result));

    const auto progressed = with_children(
        pub_two,
        {make_child("child-run-1", ProgramChildState::Dispatched), std::move(completed_child_2)},
        30);
    EXPECT_TRUE(check_increment(pub_two, progressed));
    const transition::CurrentHead pub_two_head{
        pub_two.run_record, pub_two.journal_record, pub_two.migration_plan, {}};
    EXPECT_TRUE(transition::child_results_bind(reads, "owner-a", &pub_two_head, progressed));
    EXPECT_EQ(reads.command_log_reads, 0U);
}
