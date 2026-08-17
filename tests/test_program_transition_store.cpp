#include <neograph/program/transition_store.h>
#include <neograph/program/replay.h>

#include "canonical_json.h"
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>

#include <sqlite3.h>
#endif
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <string_view>
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

ProgramContextPublication transferred_context_publication(
    const ProgramContextPublication& source, std::string run_id) {
    ContextEpochData epoch_data{std::move(run_id), 1, std::nullopt, source.epoch.feed_id(),
                                source.epoch.raw_from_sequence(),
                                source.epoch.raw_through_sequence(),
                                source.epoch.raw_window_digest(), source.epoch.artifact_ids(),
                                source.epoch.guarantee_profile()};
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

void attach_fork_split(ProgramTransitionPublication& publication,
                       const ProgramRunLineage&      source) {
    attach_initial_lineage(publication);
    const auto debit = [](RunBudget remaining, const RunBudget& allocated) {
        remaining.wall_time_ms -= allocated.wall_time_ms;
        remaining.model_tokens -= allocated.model_tokens;
        remaining.monetary_microunits -= allocated.monetary_microunits;
        remaining.max_concurrency -= allocated.max_concurrency;
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

ReplacementBoundary publish_replacement_boundary(ProgramTransitionStore& store,
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

void exercise_javascript_command_history(ProgramTransitionStore& store) {
    const auto start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    const auto pending =
        javascript_command_publication(start, javascript_command_entry(1, false), 20);
    ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, pending),
              ProgramTransitionPublishResult::Published);
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
}
void exercise_javascript_command_reservation_settlement(ProgramTransitionStore& store) {
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
                                                  std::int64_t                        time = 20) {
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
class CommandReadUnsupportedStore final : public ProgramTransitionStore {
public:
    std::optional<ProgramRunRecord> load(std::string_view, std::string_view) const override {
        return std::nullopt;
    }
    std::optional<ProgramJournalRecord> latest(std::string_view, std::string_view) const override {
        return std::nullopt;
    }
    std::vector<ProgramEvent> load_events(std::string_view,
                                          std::string_view,
                                          std::uint64_t) const override {
        return {};
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view,
                                                       std::string_view,
                                                       std::uint64_t) const override {
        return {};
    }
    ProgramTransitionPublishResult compare_publish(std::string_view,
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

void exercise_initial_reservation_is_rejected(ProgramTransitionStore& store) {
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
    ProgramTransitionStore& store) {
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
void exercise_cross_thread_call_core_settlement(ProgramTransitionStore& store,
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
    ProgramTransitionStore& store) {
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
}  // namespace

TEST(ProgramTransitionStoreTest, MissingJavaScriptCommandReadSupportFailsClosed) {
    CommandReadUnsupportedStore store;
    EXPECT_THROW((void)store.load_javascript_commands("owner-a", "run-1"), std::runtime_error);
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

TEST(ProgramTransitionStoreTest, InMemoryContextPublicationIsAtomicAppendOnlyAndOwnerScoped) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ContextPublicationFaultLeavesNoPartialSnapshotAndRetries) {
    InMemoryProgramTransitionStore store;
    auto publication = start_publication();
    publication.context_publication = context_publication(1);
    store.fail_next_publication_for_testing(ProgramTransitionFaultPoint::AfterContextSnapshot);
    EXPECT_THROW((void)store.compare_publish("owner-a", {}, publication), std::runtime_error);
    EXPECT_FALSE(store.load("owner-a", "run-1"));
    EXPECT_TRUE(store.load_context_publications("owner-a", "run-1").empty());
    EXPECT_EQ(store.compare_publish("owner-a", {}, publication),
              ProgramTransitionPublishResult::Published);
    EXPECT_EQ(store.load_context_publications("owner-a", "run-1").size(), 1U);
}

TEST(ProgramTransitionStoreTest, InMemoryJavaScriptCommandHistoryIsAppendOnly) {
    InMemoryProgramTransitionStore store;
    exercise_javascript_command_history(store);
}
TEST(ProgramTransitionStoreTest, InMemoryCommandSettlementRefundIsUsageBounded) {
    InMemoryProgramTransitionStore store;
    exercise_javascript_command_reservation_settlement(store);
}
TEST(ProgramTransitionStoreTest, InMemoryCommandlessRunningCheckpointRequiresHostEvidence) {
    InMemoryProgramTransitionStore store;
    exercise_commandless_running_checkpoint_requires_host_evidence(store);
}
TEST(ProgramTransitionStoreTest, InMemoryExecutionLeaseFencesOrdinaryPublication) {
    InMemoryProgramTransitionStore store;
    exercise_execution_lease_fences_ordinary_publication(store);
}

TEST(ProgramTransitionStoreTest, InMemoryBindsEveryReservationToItsExactCoordinate) {
    InMemoryProgramTransitionStore initial_store;
    exercise_initial_reservation_is_rejected(initial_store);
    InMemoryProgramTransitionStore unrelated_store;
    exercise_cross_thread_call_core_settlement(unrelated_store, false);
    InMemoryProgramTransitionStore exact_store;
    exercise_cross_thread_call_core_settlement(exact_store, true);
}

TEST(ProgramTransitionStoreTest, FirstPublishRetryAndOwnerIsolation) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, MigrationPublicationIsDurableAndInheritedAcrossReplay) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkAtomicallyClonesContextButNotSourceHookOwnership) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkPublicationRequiresDurableMigrationProof) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkPublicationRejectsMismatchedInitialResumeBinding) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkPublicationCannotDropSourcePendingEffect) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkPublicationRejectsPendingExpiredAtDurableTargetTime) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, FreshForkPublicationRejectsLegacyReceiptDowngrade) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ForkPublicationRequiresAtomicSourceDebit) {
    InMemoryProgramTransitionStore store;
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

    InMemoryProgramTransitionStore running_store;
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

TEST(ProgramTransitionStoreTest, NonForkMigrationPlanCannotPublishForkLineage) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, EffectOutboxMustBindExactAwaitingPendingEffect) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ConflictingCasAndInvalidPublicationRollBack) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ExactTerminalRetryIsAlreadyPresent) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, InMemoryHistoryRetainsOrderedEventAndEffectHistory) {
    InMemoryProgramTransitionStore store;
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
        InMemoryProgramTransitionStore store;
        auto                           start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        auto terminal = terminal_publication(start, ProgramTerminalStatus::Completed,
                                             ContinuationState::Completed);
        store.fail_next_publication_for_testing(fault);
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
        InMemoryProgramTransitionStore store;
        const auto                     start = start_publication();
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto interrupted = interrupted_effect_publication(start);
        store.fail_next_publication_for_testing(fault);
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

TEST(ProgramTransitionStoreTest, ConcurrentCasHasOneWinner) {
    InMemoryProgramTransitionStore store;
    auto                           start = start_publication();
    ASSERT_EQ(store.compare_publish("owner-a", {}, start),
              ProgramTransitionPublishResult::Published);
    auto completed = terminal_publication(start, ProgramTerminalStatus::Completed,
                                          ContinuationState::Completed, 20);
    auto failed =
        terminal_publication(start, ProgramTerminalStatus::Failed, ContinuationState::Failed, 21);
    std::barrier ready(3);
    auto         publish = [&](ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return store.compare_publish("owner-a", start.journal_record.id, std::move(value));
    };
    auto a = std::async(std::launch::async, [&] { return publish(completed); });
    auto b = std::async(std::launch::async, [&] { return publish(failed); });
    ready.arrive_and_wait();
    auto ar = a.get();
    auto br = b.get();
    EXPECT_NE(ar, br);
    EXPECT_TRUE((ar == ProgramTransitionPublishResult::Published &&
                 br == ProgramTransitionPublishResult::Conflict) ||
                (br == ProgramTransitionPublishResult::Published &&
                 ar == ProgramTransitionPublishResult::Conflict));
}

TEST(ProgramTransitionStoreTest, LineageAwarePublicationRoundTripsAndLoadsGeneration) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, LineageAwareRunCannotBypassHeadAccounting) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, LineageGenerationRejectsReceiptlessSuccessors) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ExactCheckpointReplacementPublishesOneSuccessor) {
    InMemoryProgramTransitionStore store;
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

    std::barrier ready(3);
    auto publish = [&](ProgramTransitionPublication value) {
        ready.arrive_and_wait();
        return store.compare_publish("owner-a", {}, std::move(value));
    };
    auto first_result = std::async(std::launch::async, [&] { return publish(first); });
    auto second_result = std::async(std::launch::async, [&] { return publish(second); });
    ready.arrive_and_wait();
    const auto a = first_result.get();
    const auto b = second_result.get();
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

TEST(ProgramTransitionStoreTest, ReplacementAtomicallyTransfersContextAndUnresolvedHookOwnership) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ReplacementRejectsTransferredContextAssemblyProvenanceDrift) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ReplacementCarriesInheritedHookAcrossSuccessorChain) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ReplacementCannotOmitRequiredRuntimeStateTransfer) {
    InMemoryProgramTransitionStore store;
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
        InMemoryProgramTransitionStore store;
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

        store.fail_next_publication_for_testing(fault);
        EXPECT_THROW((void)store.compare_publish("owner-a", {}, successor), std::runtime_error);
        EXPECT_FALSE(store.load("owner-a", "run-2"));
        EXPECT_EQ(store.load_lineage("owner-a", lineage->lineage_id())->id(), lineage->id());
        EXPECT_EQ(store.load_context_publications("owner-a", "run-1").front().epoch.id(),
                  source_context.epoch.id());
        EXPECT_EQ(store.load_hook_outbox_entries("owner-a", "run-1").front().id(),
                  source_hook.id());
    }
}

TEST(ProgramTransitionStoreTest, HistoricalReplacementInspectionRevalidatesAtoBtoC) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ReplacementReceiptRejectsWrongHandoffWithoutMutation) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ReplacementReceiptRejectsRetargetedInitialSnapshot) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, RecordedReplaySourceCannotPublishLiveReplacement) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, LineageSuccessorCannotReplenishBudget) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, LineageCannotMoveRunsWithoutSuccessorGeneration) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, LegacyRunCanAdoptLineageWithoutRenewingBudget) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, TerminalResultCannotPublishWithoutTerminalOutbox) {
    InMemoryProgramTransitionStore store;
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

TEST(ProgramTransitionStoreTest, ChildMetadataPublicationIsIdempotentAndConflictDetecting) {
    InMemoryProgramTransitionStore store;
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
TEST(ProgramTransitionStoreTest, SQLiteCommandlessRunningCheckpointRequiresHostEvidence) {
    SQLiteProgramTransitionStore store(":memory:");
    exercise_commandless_running_checkpoint_requires_host_evidence(store);
}
TEST(ProgramTransitionStoreTest, SQLiteExecutionLeaseFencesOrdinaryPublication) {
    SQLiteProgramTransitionStore store(":memory:");
    exercise_execution_lease_fences_ordinary_publication(store);
}

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
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, source),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", {}, target),
                  ProgramTransitionPublishResult::Conflict);
    }
    {
        SQLiteProgramTransitionStore store(path);
        SQLiteProgramTransitionStore same_backend(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore first_store(path);
        SQLiteProgramTransitionStore second_store(path);
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
        SQLiteProgramTransitionStore first_store(path);
        SQLiteProgramTransitionStore second_store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
        exercise_javascript_command_history(store);
    }
    {
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
TEST(ProgramTransitionStoreTest, SQLiteCommandSettlementRefundIsUsageBounded) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-command-reservation-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {
        SQLiteProgramTransitionStore store(path);
        exercise_javascript_command_reservation_settlement(store);
    }
    std::filesystem::remove(path);
}

TEST(ProgramTransitionStoreTest, SQLiteBindsEveryReservationToItsExactCoordinate) {
    static std::atomic<unsigned> sequence{0};
    const auto                   path =
        (std::filesystem::temp_directory_path() / ("neograph-program-reservation-integrity-" +
                                                   std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);
    {
        SQLiteProgramTransitionStore rejected_store(path);
        exercise_initial_reservation_is_rejected(rejected_store);
        exercise_cross_thread_call_core_settlement(rejected_store, false);
    }
    std::filesystem::remove(path);
    {
        SQLiteProgramTransitionStore exact_store(path);
        exercise_cross_thread_call_core_settlement(exact_store, true);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, interrupted),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", interrupted.journal_record.id, resumed),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
    {
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
        ASSERT_EQ(store.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        ASSERT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::Published);
        EXPECT_EQ(store.compare_publish("owner-a", start.journal_record.id, child),
                  ProgramTransitionPublishResult::AlreadyPresent);
    }
    {
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
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
        SQLiteProgramTransitionStore store(path);
        const auto                   run = store.load("owner-a", "run-1");
        ASSERT_TRUE(run.has_value());
        EXPECT_EQ(run->continuation().state, ContinuationState::Running);
        EXPECT_EQ(store.latest("owner-a", "run-1")->id, resumed.journal_record.id);
        EXPECT_EQ(store.load_events("owner-a", "run-1", 2).size(), 1U);
        EXPECT_EQ(store.load_effects("owner-a", "run-1").size(), 1U);
    }
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
        SQLiteProgramTransitionStore first(path);
        SQLiteProgramTransitionStore second(path);
        const auto                   start = start_publication();
        ASSERT_EQ(first.compare_publish("owner-a", {}, start),
                  ProgramTransitionPublishResult::Published);
        const auto   completed = terminal_publication(start, ProgramTerminalStatus::Completed,
                                                      ContinuationState::Completed, 20);
        const auto   failed    = terminal_publication(start, ProgramTerminalStatus::Failed,
                                                      ContinuationState::Failed, 21);
        std::barrier ready(3);
        auto         publish = [&](SQLiteProgramTransitionStore& store,
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

TEST(ProgramTransitionStoreTest, HookOutboxHeadsAreAtomicAndReconnectable) {
    InMemoryProgramTransitionStore store;
    auto publication = start_publication();
    publication.hook_outbox_entries.push_back(pending_hook_outbox_entry());
    store.fail_next_publication_for_testing(ProgramTransitionFaultPoint::AfterHookSnapshot);
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

    InMemoryProgramTransitionStore store;
    auto initial = start_publication();
    initial.hook_outbox_entries = {pending};
    ASSERT_EQ(store.compare_publish("owner-a", {}, initial), ProgramTransitionPublishResult::Published);
    auto skipped = javascript_command_publication(initial, javascript_command_entry(1, false), 20);
    skipped.hook_outbox_entries = {succeeded};
    EXPECT_EQ(store.compare_publish("owner-a", initial.journal_record.id, skipped),
              ProgramTransitionPublishResult::Conflict);
}

TEST(ProgramTransitionStoreTest, TimedOutMandatoryHookBlocksReplacement) {
    InMemoryProgramTransitionStore store;
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
