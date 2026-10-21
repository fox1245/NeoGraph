#include <neograph/program/transition_store.h>

#include "command_publication_head.h"

#include "canonical_json.h"
#include "node_retry_budget.h"
#include "transition_validator.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace neograph::program {
namespace {
constexpr std::string_view EFFECT_FORMAT      = "neograph-program-effect-outbox-entry",
                           PUBLICATION_FORMAT = "neograph-program-transition-publication";
using detail::transition::Borrowed;
using detail::transition::budget_is_empty;
using detail::transition::fork_allocation_fits;
void validate_context_publication(const ProgramContextPublication& context,
                                  const ProgramRunRecord&          run) {
    if (context.epoch.run_id() != run.run_id()) {
        throw std::invalid_argument("Program context epoch does not bind the run");
    }
    for (const auto& artifact : context.artifacts) {
        if (artifact.kind() == ContextArtifactKind::HookOutput) {
            throw std::invalid_argument("Program context publication cannot persist HookOutput");
        }
    }
    validate_context_assembly_receipt(context.assembly_receipt, context.epoch,
                                      context.artifacts);
}

bool valid_context_history_append(
    const std::vector<ProgramContextPublication>& old_context,
    const std::optional<ProgramContextPublication>& next_context) {
    if (!next_context) return true;
    const auto& epoch = next_context->epoch;
    if (old_context.empty()) return epoch.sequence() == 1 && !epoch.predecessor_id();
    const auto& previous = old_context.back().epoch;
    return epoch.sequence() == previous.sequence() + 1 && epoch.predecessor_id() &&
           *epoch.predecessor_id() == previous.id();
}

bool hook_is_terminal(HookExecutionState state) noexcept {
    // ReconciliationRequired is an unresolved external-effect obligation, not
    // a final state: it may advance only through an explicit known receipt.
    return state == HookExecutionState::Succeeded || state == HookExecutionState::TimedOut ||
           state == HookExecutionState::Cancelled;
}

bool valid_hook_head_update(const HookOutboxEntry& previous,
                             const HookOutboxEntry& next) noexcept {
    try {
        const auto& old_data = previous.data();
        const auto& new_data = next.data();
        if (old_data.invocation.id() != new_data.invocation.id() ||
            old_data.invocation.serialize_canonical() != new_data.invocation.serialize_canonical() ||
            old_data.event.serialize_canonical() != new_data.event.serialize_canonical() ||
            old_data.max_attempts != new_data.max_attempts || old_data.deadline != new_data.deadline ||
            hook_is_terminal(old_data.state) || new_data.attempt_count < old_data.attempt_count ||
            new_data.fencing_token < old_data.fencing_token) return false;
        if (previous.id() == next.id()) return false;
        const auto transition = [&](HookExecutionState from, HookExecutionState to) {
            if (from == HookExecutionState::Triggered)
                return to == HookExecutionState::Pending || to == HookExecutionState::TimedOut ||
                       to == HookExecutionState::Cancelled;
            if (from == HookExecutionState::Pending)
                return to == HookExecutionState::Dispatched || to == HookExecutionState::TimedOut ||
                       to == HookExecutionState::Cancelled;
            if (from == HookExecutionState::Dispatched)
                return to == HookExecutionState::Succeeded || to == HookExecutionState::Failed ||
                       to == HookExecutionState::TimedOut ||
                       to == HookExecutionState::ReconciliationRequired ||
                       to == HookExecutionState::Cancelled;
            if (from == HookExecutionState::Failed)
                return to == HookExecutionState::Dispatched || to == HookExecutionState::TimedOut ||
                       to == HookExecutionState::Cancelled;
            return from == HookExecutionState::ReconciliationRequired &&
                   (to == HookExecutionState::Succeeded || to == HookExecutionState::Failed);
        };
        if (!transition(old_data.state, new_data.state)) return false;

        // A dispatch creates exactly one fenced attempt. Settlement and timeout
        // preserve that attempt; retry is the only later fence advance.
        if (new_data.state == HookExecutionState::Dispatched) {
            if (new_data.attempt_count != old_data.attempt_count + 1 ||
                new_data.fencing_token != old_data.fencing_token + 1) return false;
        } else if (new_data.attempt_count != old_data.attempt_count ||
                   new_data.fencing_token != old_data.fencing_token) {
            return false;
        }
        if (old_data.state == HookExecutionState::ReconciliationRequired &&
            (!new_data.receipt || !new_data.receipt->data().external_effect.outcome_known))
            return false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool valid_hook_history_append(const std::vector<HookOutboxEntry>& old_entries,
                               const std::vector<HookOutboxEntry>& next_entries,
                               const ProgramRunRecord& run) {
    std::map<std::string, HookOutboxEntry, std::less<>> heads;
    for (const auto& entry : old_entries) {
        validate_program_hook_outbox_entry(entry, run);
        const auto [_, inserted] = heads.emplace(entry.data().invocation.id(), entry);
        if (!inserted) return false;
    }
    std::set<std::string, std::less<>> seen;
    for (const auto& entry : next_entries) {
        validate_program_hook_outbox_entry(entry, run);
        if (!seen.emplace(entry.data().invocation.id()).second) return false;
        const auto old = heads.find(entry.data().invocation.id());
        if (old != heads.end() && !valid_hook_head_update(old->second, entry)) return false;
    }
    return true;
}

struct ProgramTransitionHistory final {
    ProgramTransitionHistory(std::shared_ptr<const ProgramTransitionHistory> previous_history,
                             std::vector<ProgramEvent>                     appended_events,
                             std::vector<ProgramEffectOutboxEntry>         appended_effects)
        : previous(std::move(previous_history)), events(std::move(appended_events)),
          effects(std::move(appended_effects)),
          event_count((previous ? previous->event_count : 0) + events.size()),
          effect_count((previous ? previous->effect_count : 0) + effects.size()) {}

    std::shared_ptr<const ProgramTransitionHistory> previous;
    std::vector<ProgramEvent>                       events;
    std::vector<ProgramEffectOutboxEntry>           effects;
    std::size_t                                      event_count;
    std::size_t                                      effect_count;
};

using ProgramTransitionHistoryPtr = std::shared_ptr<const ProgramTransitionHistory>;

constexpr std::size_t TRANSITION_HISTORY_CHUNK_CAPACITY = 32;

template <typename Entry>
std::vector<Entry> append_entries(const std::vector<Entry>& existing,
                                  std::vector<Entry>        additions) {
    if (existing.empty()) return additions;

    std::vector<Entry> entries;
    entries.reserve(existing.size() + additions.size());
    entries.insert(entries.end(), existing.begin(), existing.end());
    entries.insert(entries.end(), std::make_move_iterator(additions.begin()),
                   std::make_move_iterator(additions.end()));
    return entries;
}

ProgramTransitionHistoryPtr append_history(
    ProgramTransitionHistoryPtr                    previous,
    std::vector<ProgramEvent>                      events,
    std::vector<ProgramEffectOutboxEntry>          effects) {
    if (events.empty() && effects.empty()) return previous;
    if (!previous ||
        previous->events.size() + events.size() > TRANSITION_HISTORY_CHUNK_CAPACITY ||
        previous->effects.size() + effects.size() > TRANSITION_HISTORY_CHUNK_CAPACITY) {
        return std::make_shared<const ProgramTransitionHistory>(
            std::move(previous), std::move(events), std::move(effects));
    }
    return std::make_shared<const ProgramTransitionHistory>(
        previous->previous, append_entries(previous->events, std::move(events)),
        append_entries(previous->effects, std::move(effects)));
}

ProgramTransitionHistoryPtr begin_history(
    const std::vector<ProgramEvent>&               events,
    const std::vector<ProgramEffectOutboxEntry>&   effects,
    std::vector<ProgramEvent>                      appended_events,
    std::vector<ProgramEffectOutboxEntry>          appended_effects) {
    return std::make_shared<const ProgramTransitionHistory>(
        nullptr, append_entries(events, std::move(appended_events)),
        append_entries(effects, std::move(appended_effects)));
}

template <typename Entry, typename Sequence>
std::vector<Entry> entries_after(const std::vector<Entry>& entries,
                                 std::uint64_t             after_sequence,
                                 Sequence&&                sequence) {
    if (entries.empty() || after_sequence >= sequence(entries.back())) return {};

    std::vector<Entry> result;
    if (!after_sequence) result.reserve(entries.size());
    for (const auto& entry : entries) {
        if (sequence(entry) > after_sequence) result.push_back(entry);
    }
    return result;
}

std::vector<ProgramEvent> event_entries_after(const ProgramTransitionHistoryPtr& history,
                                              std::uint64_t after_sequence) {
    if (!history || !history->event_count) return {};

    std::vector<ProgramEvent> result;
    if (!after_sequence) result.reserve(history->event_count);
    for (auto current = history; current; current = current->previous) {
        for (auto event = current->events.rbegin(); event != current->events.rend(); ++event) {
            if (event->sequence > after_sequence) result.push_back(*event);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<ProgramEffectOutboxEntry>
effect_entries_after(const ProgramTransitionHistoryPtr& history,
                     std::uint64_t                      after_sequence) {
    if (!history || !history->effect_count) return {};

    std::vector<ProgramEffectOutboxEntry> result;
    if (!after_sequence) result.reserve(history->effect_count);
    for (auto current = history; current; current = current->previous) {
        for (auto effect = current->effects.rbegin(); effect != current->effects.rend(); ++effect) {
            if (effect->sequence() > after_sequence) result.push_back(*effect);
        }
    }
    std::reverse(result.begin(), result.end());
    return result;
}

bool effect_history_contains(const std::vector<ProgramEffectOutboxEntry>& effects,
                             const ProgramTransitionHistoryPtr&            history,
                             std::string_view                              id) {
    if (std::any_of(effects.begin(), effects.end(), [&](const auto& existing) {
            return existing.effect().effect_id() == id;
        })) {
        return true;
    }
    for (auto current = history; current; current = current->previous) {
        if (std::any_of(current->effects.begin(), current->effects.end(),
                        [&](const auto& existing) {
                            return existing.effect().effect_id() == id;
                        })) {
            return true;
        }
    }
    return false;
}

template <typename Integer>
bool checked_add(Integer& target, Integer value) noexcept {
    if (value > std::numeric_limits<Integer>::max() - target) return false;
    target = static_cast<Integer>(target + value);
    return true;
}

std::optional<RunBudget> committed_descendant_budget(const ProgramRunRecord& run) noexcept {
    RunBudget committed;
    for (const auto& child : run.children()) {
        const auto& budget = child.invocation.granted_budget;
        if (!checked_add(committed.wall_time_ms, budget.wall_time_ms) ||
            !checked_add(committed.model_tokens, budget.model_tokens) ||
            !checked_add(committed.monetary_microunits, budget.monetary_microunits) ||
            !checked_add(committed.max_program_operations, budget.max_program_operations) ||
            !checked_add(committed.max_core_steps, budget.max_core_steps) ||
            !checked_add(committed.max_dynamic_compiles, budget.max_dynamic_compiles) ||
            budget.max_total_children == std::numeric_limits<std::uint64_t>::max() ||
            !checked_add(committed.max_total_children, budget.max_total_children + 1)) {
            return std::nullopt;
        }
    }
    return committed;
}

bool fork_resume_binds_initial_target(const ForkCompatibilityReceipt& receipt,
                                      const ProgramRunRecord&         target) {
    if (receipt.storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
        return true;
    }
    if (!receipt.initial_resume_binding()) return false;
    if (const auto pending = target.pending_input()) {
        const auto result = pending->consumed_result();
        return pending->state() == ProgramPendingState::Consumed && result &&
               receipt.matches_initial_resume(pending->call_id(), *result);
    }
    if (const auto pending = target.pending_effect()) {
        const auto result = pending->reconciled_result();
        return pending->state() == ProgramPendingState::Consumed &&
               pending->reconciliation() == ProgramEffectReconciliation::Completed && result &&
               receipt.matches_initial_resume(pending->call_id(), *result);
    }
    return !receipt.initial_resume_binding()->target_pending_id;
}

std::string rs(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_string())
        throw std::invalid_argument("Program transition field '" + k + "' must be a string");
    return v[k].get<std::string>();
}
std::uint64_t ru(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k) || !v[k].is_number_unsigned())
        throw std::invalid_argument("Program transition field '" + k + "' must be unsigned");
    return v[k].get<std::uint64_t>();
}
std::uint32_t r32(const json& v, std::string_view key) {
    auto n = ru(v, key);
    if (n > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("Program transition integer exceeds uint32 range");
    return static_cast<std::uint32_t>(n);
}
json rv(const json& v, std::string_view key) {
    std::string k(key);
    if (!v.contains(k))
        throw std::invalid_argument("Program transition requires field '" + k + "'");
    return v[k];
}
json nested(std::string bytes) {
    return detail::parse_json_strict(bytes);
}
std::string key(std::string_view owner, std::string_view run) {
    std::string out;
    out.reserve(owner.size() + run.size() + 1);
    out.append(owner);
    out.push_back('\0');
    out.append(run);
    return out;
}
std::string graph_migration_capsule_key(std::string_view owner,
                                        std::string_view run,
                                        std::string_view lineage_head) {
    auto out = key(owner, run);
    out.push_back('\0');
    out.append(lineage_head);
    return out;
}
void validate_pub(const ProgramTransitionPublication& publication, std::string_view owner) {
    // Child serializers validate their immutable identity and canonical body while
    // the publication compositor checks these cross-record invariants.
    const auto& run     = publication.run_record;
    const auto& journal = publication.journal_record;
    if (run.owner_scope() != owner || run.run_id() != journal.run_id ||
        run.program_version_id() != journal.program_version_id ||
        run.bundle_id() != journal.bundle_id || run.journal_head() != journal.id ||
        run.continuation() != journal.continuation ||
        run.remaining_budget() != journal.remaining_budget ||
        run.exact_checkpoint() != journal.core_checkpoint) {
        throw std::invalid_argument("Program snapshot and journal do not match");
    }
    if (run.logical_run_id() != run.run_id() &&
        (!publication.run_lineage ||
         (publication.run_generation && publication.run_generation->generation() == 1)))
        throw std::invalid_argument("Logical agent identity requires the exact lineage");
    if (publication.context_publication) {
        validate_context_publication(*publication.context_publication, run);
    }
    if (!valid_hook_history_append({}, publication.hook_outbox_entries, run)) {
        throw std::invalid_argument("Program hook outbox publication is invalid");
    }

    std::uint64_t previous_sequence = 0;
    for (const auto& event : publication.events) {
        if (event.run_id != run.run_id() || event.program_version_id != run.program_version_id() ||
            event.bundle_id != run.bundle_id() ||
            event.sequence <= previous_sequence) {
            throw std::invalid_argument("Program event does not bind snapshot");
        }
        previous_sequence = event.sequence;
    }
    if (!publication.events.empty() && publication.events.back().sequence != run.event_sequence()) {
        throw std::invalid_argument("Program event sequence mismatch");
    }
    if (!publication.events.empty()) {
        const auto& latest = publication.events.back();
        const bool snapshot_operation = latest.kind == ProgramEventKind::Started ||
                                        latest.kind == ProgramEventKind::Terminal;
        if (latest.attempt != run.continuation().attempt ||
            (snapshot_operation && latest.operation_id != run.continuation().operation_id)) {
            throw std::invalid_argument("Latest Program event does not bind snapshot");
        }
        if (latest.kind == ProgramEventKind::CheckpointPublished) {
            const auto& checkpoint = std::get<ProgramCheckpointEvent>(latest.payload).checkpoint;
            if (!run.exact_checkpoint() || checkpoint != *run.exact_checkpoint()) {
                throw std::invalid_argument("Program checkpoint event disagrees with run snapshot");
            }
        }
    }

    previous_sequence = 0;
    for (const auto& effect : publication.effects) {
        if (effect.sequence() <= previous_sequence ||
            effect.effect().operation_id().empty()) {
            throw std::invalid_argument("Program effect does not bind snapshot");
        }
        previous_sequence = effect.sequence();
    }
    std::set<std::string, std::less<>> effect_ids;
    for (const auto& effect : publication.effects) {
        if (!effect_ids.emplace(effect.effect().effect_id()).second) {
            throw std::invalid_argument("Program effect outbox contains a duplicate effect id");
        }
    }
    if (!publication.effects.empty() &&
        publication.effects.back().sequence() != run.effect_sequence()) {
        throw std::invalid_argument("Program effect sequence mismatch");
    }
    previous_sequence = 0;
    for (const auto& command : publication.commands) {
        if (command.bundle_id() != run.bundle_id() ||
            command.sequence() <= previous_sequence) {
            throw std::invalid_argument(
                "JavaScript command journal entry does not bind snapshot");
        }
        previous_sequence = command.sequence();
    }
    if (run.terminal_result() && !publication.events.empty() &&
        publication.events.back().kind == ProgramEventKind::Terminal &&
        std::get<ProgramTerminalEvent>(publication.events.back().payload).status !=
            run.terminal_result()->status()) {
        throw std::invalid_argument("Terminal event/result mismatch");
    }
    if (publication.migration_plan) {
        const auto& plan = *publication.migration_plan;
        const auto fork = run.fork_receipt();
        const auto graph_migration = publication.run_generation
                                         ? publication.run_generation->graph_migration_receipt()
                                         : std::nullopt;
        const bool binds_fork = fork &&
            plan.source_version_id() == fork->source_program_version_id();
        const bool binds_graph_migration = graph_migration &&
            plan.id() == graph_migration->migration_plan_id() &&
            plan.source_version_id() ==
                graph_migration->capsule().source_program_version_id();
        const bool inherits_graph_migration = !fork && !graph_migration &&
            publication.run_lineage && !publication.run_generation;
        if (plan.owner_scope() != owner ||
            plan.target_version_id() != run.program_version_id() ||
            !plan.is_compatible() ||
            (!binds_fork && !binds_graph_migration && !inherits_graph_migration)) {
            throw std::invalid_argument(
                "Program migration publication requires a compatible transition receipt");
        }
    } else if (publication.run_generation &&
               publication.run_generation->graph_migration_receipt()) {
        throw std::invalid_argument(
            "Program Graph migration publication requires its admitted plan");
    }
    if (publication.run_generation && !publication.run_lineage) {
        throw std::invalid_argument("Program generation publication requires a lineage head");
    }
    if (publication.run_lineage) {
        const auto& lineage = *publication.run_lineage;
        const auto  committed = committed_descendant_budget(run);
        if (lineage.owner_scope() != owner ||
            lineage.active_run_record_id() != run.id() ||
            lineage.active_journal_head() != journal.id ||
            lineage.remaining_budget() != journal.remaining_budget ||
            lineage.inflight_reservation() != journal.inflight_reservation || !committed ||
            lineage.committed_descendant_budget() != *committed) {
            throw std::invalid_argument("Program lineage head does not bind the transition");
        }
        if (publication.run_generation) {
            const auto& generation = *publication.run_generation;
            if (generation.generation() > 1 && !generation.replacement_receipt() &&
                !generation.graph_migration_receipt()) {
                throw std::invalid_argument(
                    "Program successor generation requires an admitted transition receipt");
            }
            if (generation.owner_scope() != owner ||
                generation.lineage_id() != lineage.lineage_id() ||
                generation.run_id() != run.run_id() ||
                generation.program_version_id() != run.program_version_id() ||
                generation.bundle_id() != run.bundle_id() ||
                generation.initial_run_record_id() != run.id() ||
                generation.initial_journal_head() != journal.id ||
                generation.id() != lineage.active_generation_id() ||
                generation.child_depth() != run.child_depth()) {
                throw std::invalid_argument("Program generation does not bind the transition");
            }
        }
    }

    if (publication.fork_source_lineage) {
        const auto fork = run.fork_receipt();
        if (!fork || !fork_resume_binds_initial_target(*fork, run) || !publication.run_lineage ||
            !publication.run_generation || publication.run_generation->generation() != 1 ||
            !budget_is_empty(journal.inflight_reservation) ||
            publication.fork_source_lineage->owner_scope() != owner ||
            publication.fork_source_lineage->lineage_id() ==
                publication.run_lineage->lineage_id()) {
            throw std::invalid_argument("Program fork source debit does not bind the target");
        }
    }
    if (publication.recorded_replay_source.has_value() !=
        publication.recorded_replay_source_lineage.has_value())
        throw std::invalid_argument("Recorded replay requires its exact source debit receipt");
    if (publication.recorded_replay_source) {
        const auto& source = *publication.recorded_replay_source;
        detail::validate_token(source.run_id, "Recorded replay source run");
        if (!detail::is_sha256_identity(source.run_record_id) ||
            !detail::is_sha256_identity(source.journal_head) ||
            !detail::is_sha256_identity(source.lineage_head_id) ||
            publication.fork_source_lineage || run.fork_receipt() ||
            !run.recorded_binding_set_fingerprint() || !publication.run_lineage ||
            !publication.run_generation || publication.run_generation->generation() != 1 ||
            journal.sequence != 1 || !journal.previous_id.empty() ||
            !budget_is_empty(journal.inflight_reservation) ||
            publication.recorded_replay_source_lineage->owner_scope() != owner ||
            publication.recorded_replay_source_lineage->lineage_id() ==
                publication.run_lineage->lineage_id())
            throw std::invalid_argument("Recorded replay source debit does not bind its initial target");
    }
}

constexpr std::size_t MAX_CANONICAL_PUBLICATION_BYTES = 16u * 1024u * 1024u;

void append_publication_bytes(std::string& out, std::string_view bytes) {
    if (bytes.size() > MAX_CANONICAL_PUBLICATION_BYTES - out.size()) {
        throw std::invalid_argument("Program JSON exceeds the 16 MiB materialized limit");
    }
    out.append(bytes);
}

std::string publication_bytes(const ProgramTransitionPublication& publication) {
    std::string bytes;
    bytes.reserve(4096);

    append_publication_bytes(bytes, "{");
    if (!publication.child_synthesis_records.empty()) {
        append_publication_bytes(bytes, "\"child_synthesis_records\":[");
        for (std::size_t i = 0; i < publication.child_synthesis_records.size(); ++i) {
            if (i) append_publication_bytes(bytes, ",");
            append_publication_bytes(bytes,
                                     publication.child_synthesis_records[i].serialize_canonical());
        }
        append_publication_bytes(bytes, "],");
    }
    append_publication_bytes(bytes, "\"commands\":[");
    for (std::size_t index = 0; index < publication.commands.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.commands[index].serialize_canonical());
    }

    append_publication_bytes(bytes, "],\"context_publication\":");
    if (publication.context_publication) {
        const auto& context = *publication.context_publication;
        append_publication_bytes(bytes, "{\"artifacts\":[");
        for (std::size_t index = 0; index < context.artifacts.size(); ++index) {
            if (index != 0) append_publication_bytes(bytes, ",");
            append_publication_bytes(bytes, context.artifacts[index].serialize_canonical());
        }
        append_publication_bytes(bytes, "],\"assembly_receipt\":");
        append_publication_bytes(bytes, context.assembly_receipt.serialize_canonical());
        append_publication_bytes(bytes, ",\"epoch\":");
        append_publication_bytes(bytes, context.epoch.serialize_canonical());
        append_publication_bytes(bytes, "}");
    } else {
        append_publication_bytes(bytes, "null");
    }

    append_publication_bytes(bytes, ",\"effects\":[");
    for (std::size_t index = 0; index < publication.effects.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.effects[index].serialize_canonical());
    }

    append_publication_bytes(bytes,
                             "],\"events\":[");
    for (std::size_t index = 0; index < publication.events.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.events[index].serialize_canonical());
    }

    append_publication_bytes(bytes, "]");
    if (publication.fork_source_lineage) {
        append_publication_bytes(bytes, ",\"fork_source_lineage\":");
        append_publication_bytes(bytes,
                                 publication.fork_source_lineage->serialize_canonical());
    }
    append_publication_bytes(bytes, ",\"format\":\"neograph-program-transition-publication\"");
    append_publication_bytes(bytes, ",\"hook_outbox_entries\":[");
    for (std::size_t index = 0; index < publication.hook_outbox_entries.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes,
                                 publication.hook_outbox_entries[index].serialize_canonical());
    }
    append_publication_bytes(bytes, "],\"journal_record\":");
    append_publication_bytes(bytes, publication.journal_record.serialize_canonical());
    append_publication_bytes(bytes, ",\"migration_plan\":");
    if (publication.migration_plan) {
        append_publication_bytes(bytes, publication.migration_plan->serialize_canonical());
    } else {
        append_publication_bytes(bytes, "null");
    }
    if (publication.recorded_replay_source) {
        const auto& source = *publication.recorded_replay_source;
        append_publication_bytes(bytes, ",\"recorded_replay_source\":");
        append_publication_bytes(bytes, detail::canonical_json_bytes(json{
            {"run_id", source.run_id}, {"run_record_id", source.run_record_id},
            {"journal_head", source.journal_head}, {"lineage_head_id", source.lineage_head_id}}));
        append_publication_bytes(bytes, ",\"recorded_replay_source_lineage\":");
        append_publication_bytes(bytes,
                                 publication.recorded_replay_source_lineage->serialize_canonical());
    }
    if (publication.run_lineage) {
        append_publication_bytes(bytes, ",\"run_generation\":");
        if (publication.run_generation) {
            append_publication_bytes(bytes, publication.run_generation->serialize_canonical());
        } else {
            append_publication_bytes(bytes, "null");
        }
        append_publication_bytes(bytes, ",\"run_lineage\":");
        append_publication_bytes(bytes, publication.run_lineage->serialize_canonical());
    }
    append_publication_bytes(bytes, ",\"run_record\":");
    append_publication_bytes(bytes, publication.run_record.serialize_canonical());
    append_publication_bytes(bytes, publication.recorded_replay_source
        ? ",\"storage_schema_version\":7}"
        : publication.child_synthesis_records.empty()
            ? ",\"storage_schema_version\":5}" : ",\"storage_schema_version\":6}");
    return bytes;
}
}  // namespace
bool is_valid_program_recorded_replay_allocation(
    const ProgramTransitionPublication& publication,
    const ProgramRunLineage& previous,
    const ProgramRunRecord& source) {
    if (!publication.recorded_replay_source || !publication.recorded_replay_source_lineage)
        return false;
    const auto& receipt = *publication.recorded_replay_source;
    const auto& target = publication.run_record;
    const auto terminal = source.terminal_result();
    return receipt.run_id == source.run_id() && receipt.run_record_id == source.id() &&
        receipt.journal_head == source.journal_head() && receipt.lineage_head_id == previous.id() &&
        previous.active_run_record_id() == source.id() &&
        previous.active_journal_head() == source.journal_head() &&
        source.owner_scope() == target.owner_scope() && source.run_id() != target.run_id() &&
        source.program_version_id() == target.program_version_id() &&
        source.bundle_id() == target.bundle_id() &&
        source.binding_fingerprint() == target.binding_fingerprint() &&
        source.invocation().input == target.invocation().input &&
        source.invocation().budget == target.invocation().budget &&
        source.child_depth() == 0 && target.child_depth() == 0 &&
        source.invocation().parent_run_id.empty() && target.invocation().parent_run_id.empty() &&
        previous.committed_descendant_budget() == RunBudget{} &&
        previous.inflight_reservation() == RunBudget{} &&
        terminal && terminal->provider_budget_authority().has_value() &&
        terminal->status() != ProgramTerminalStatus::Interrupted &&
        terminal->status() != ProgramTerminalStatus::AmbiguousEffect &&
        target.recorded_binding_set_fingerprint().has_value() &&
        is_valid_program_run_lineage_transition(
            previous, *publication.recorded_replay_source_lineage) &&
        fork_allocation_fits(previous, *publication.recorded_replay_source_lineage,
                             publication.journal_record.remaining_budget);
}
bool does_program_child_generation_result_bind(const ProgramRunRecord&     parent,
                                               const ProgramChildRecord&   child,
                                               const ProgramRunGeneration& generation,
                                               const ProgramRunLineage&    lineage,
                                               const ProgramRunRecord&     result_run) noexcept {
    return child.terminal_result && result_run.terminal_result() &&
           parent.owner_scope() == result_run.owner_scope() &&
           child.invocation.parent_run_id == parent.logical_run_id() &&
           lineage.root_run_id() == child.child_run_id &&
           child.terminal_generation == generation.generation() && child.terminal_generation >= 2 &&
           generation.replacement_receipt().has_value() &&
           does_program_run_generation_bind(generation, lineage, result_run) &&
           result_run.invocation().parent_run_id == parent.logical_run_id() &&
           result_run.child_depth() == child.invocation.child_depth &&
           child.terminal_result->id() == result_run.terminal_result()->id();
}

struct ProgramEffectOutboxEntry::Impl {
    Impl(std::uint64_t s, ProgramPendingEffect e) : sequence(s), effect(std::move(e)) {
        auto b = json{{"format", std::string(EFFECT_FORMAT)},
                      {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                      {"sequence", sequence},
                      {"effect", nested(effect.serialize_canonical())}};
        id     = detail::sha256_identity("program-effect-outbox-entry/v1",
                                         detail::canonical_json_bytes(b));
    }
    std::uint64_t        sequence;
    ProgramPendingEffect effect;
    std::string          id;
};
ProgramEffectOutboxEntry::ProgramEffectOutboxEntry(std::uint64_t s, ProgramPendingEffect e)
    : ProgramEffectOutboxEntry(std::make_shared<const Impl>(s, std::move(e))) {
    if (!s) throw std::invalid_argument("Program effect outbox sequence must be positive");
}
ProgramEffectOutboxEntry::ProgramEffectOutboxEntry(std::shared_ptr<const Impl> p)
    : impl_(std::move(p)) {}
std::uint64_t ProgramEffectOutboxEntry::sequence() const noexcept {
    return impl_->sequence;
}
const ProgramPendingEffect& ProgramEffectOutboxEntry::effect() const noexcept {
    return impl_->effect;
}
const std::string& ProgramEffectOutboxEntry::id() const noexcept {
    return impl_->id;
}
std::string ProgramEffectOutboxEntry::serialize_canonical() const {
    if (!impl_->sequence) throw std::invalid_argument("Program effect sequence must be positive");
    auto b = json{{"format", std::string(EFFECT_FORMAT)},
                  {"storage_schema_version", STORAGE_SCHEMA_VERSION},
                  {"sequence", impl_->sequence},
                  {"effect", nested(impl_->effect.serialize_canonical())}};
    if (detail::sha256_identity("program-effect-outbox-entry/v1",
                                detail::canonical_json_bytes(b)) != impl_->id)
        throw std::invalid_argument("Program effect outbox id mismatch");
    b["id"] = impl_->id;
    return detail::canonical_json_bytes(b);
}
ProgramEffectOutboxEntry ProgramEffectOutboxEntry::parse(std::string_view bytes) {
    auto v = detail::parse_json_strict(bytes);
    if (!v.is_object() || rs(v, "format") != EFFECT_FORMAT)
        throw std::invalid_argument("Stored Program effect outbox has unknown format");
    detail::reject_unknown_fields(v, "Stored Program effect outbox",
                                  {"format", "storage_schema_version", "id", "sequence", "effect"});
    if (r32(v, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored Program effect schema unsupported");
    auto out = ProgramEffectOutboxEntry(
        ru(v, "sequence"),
        ProgramPendingEffect::parse(detail::canonical_json_bytes(rv(v, "effect"))));
    if (out.id() != rs(v, "id")) throw std::invalid_argument("Stored Program effect id mismatch");
    return out;
}
std::string ProgramTransitionPublication::serialize_canonical() const {
    validate_pub(*this, run_record.owner_scope());
    return publication_bytes(*this);
}
ProgramTransitionPublication ProgramTransitionPublication::parse(std::string_view bytes) {
    auto v = detail::parse_json_strict(bytes);
    if (!v.is_object() || rs(v, "format") != PUBLICATION_FORMAT)
        throw std::invalid_argument("Stored Program publication has unknown format");
    const auto schema_version = r32(v, "storage_schema_version");
    if (schema_version == 1) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan"});
    } else if (schema_version == 2) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
              "effects", "commands", "migration_plan", "run_generation", "run_lineage"});
    } else if (schema_version == 3) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan", "run_generation", "run_lineage",
              "fork_source_lineage"});
    } else if (schema_version == 4) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan", "run_generation", "run_lineage",
              "fork_source_lineage", "context_publication"});
    } else if (schema_version == 5) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan", "run_generation", "run_lineage",
             "fork_source_lineage", "context_publication", "hook_outbox_entries"});
    } else if (schema_version == 6) {
        detail::reject_unknown_fields(
            v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan", "run_generation", "run_lineage",
             "fork_source_lineage", "context_publication", "hook_outbox_entries",
             "child_synthesis_records"});
    } else if (schema_version == 7) {
        detail::reject_unknown_fields(v, "Stored Program publication",
            {"format", "storage_schema_version", "run_record", "journal_record", "events",
             "effects", "commands", "migration_plan", "run_generation", "run_lineage",
             "fork_source_lineage", "context_publication", "hook_outbox_entries",
             "child_synthesis_records", "recorded_replay_source", "recorded_replay_source_lineage"});
    } else {
        throw std::invalid_argument("Stored Program publication schema unsupported");
    }
    auto run = ProgramRunRecord::parse(detail::canonical_json_bytes(rv(v, "run_record")));
    auto journal =
        ProgramJournalRecord::parse(detail::canonical_json_bytes(rv(v, "journal_record")));
    std::vector<ProgramEvent> events;
    auto                      ev = rv(v, "events");
    if (!ev.is_array()) throw std::invalid_argument("Program events must be array");
    for (const auto& e : ev)
        events.push_back(ProgramEvent::parse(detail::canonical_json_bytes(e)));
    std::vector<ProgramEffectOutboxEntry> effects;
    auto                                  fx = rv(v, "effects");
    if (!fx.is_array()) throw std::invalid_argument("Program effects must be array");
    for (const auto& e : fx)
        effects.push_back(ProgramEffectOutboxEntry::parse(detail::canonical_json_bytes(e)));
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    if (v.contains("commands")) {
        const auto& encoded_commands = rv(v, "commands");
        if (!encoded_commands.is_array())
            throw std::invalid_argument("Program command journal must be array");
        for (const auto& entry : encoded_commands) {
            commands.push_back(ProgramJavaScriptCommandJournalEntry::parse(
                detail::canonical_json_bytes(entry)));
        }
    }
    std::optional<MigrationPlan> migration_plan;
    if (v.contains("migration_plan")) {
        const auto& plan = rv(v, "migration_plan");
        if (!plan.is_null()) migration_plan = MigrationPlan::parse(detail::canonical_json_bytes(plan));
    }
    std::optional<ProgramRunGeneration> run_generation;
    std::optional<ProgramRunLineage>    run_lineage;
    if (schema_version == 2 || schema_version == 3 ||
        (schema_version >= 4 && v.contains("run_lineage"))) {
        const auto& encoded_generation = rv(v, "run_generation");
        if (!encoded_generation.is_null()) {
            run_generation = ProgramRunGeneration::parse(
                detail::canonical_json_bytes(encoded_generation));
        }
        const auto& encoded_lineage = rv(v, "run_lineage");
        if (encoded_lineage.is_null())
            throw std::invalid_argument("Stored Program publication requires a lineage head");
        run_lineage = ProgramRunLineage::parse(detail::canonical_json_bytes(encoded_lineage));
    }
    std::optional<ProgramRunLineage> fork_source_lineage;
    if (schema_version == 3) {
        const auto& encoded_source = rv(v, "fork_source_lineage");
        if (encoded_source.is_null())
            throw std::invalid_argument("Stored Program fork publication requires a source debit");
        fork_source_lineage =
            ProgramRunLineage::parse(detail::canonical_json_bytes(encoded_source));
    } else if (schema_version >= 4 && v.contains("fork_source_lineage")) {
        const auto& encoded_source = rv(v, "fork_source_lineage");
        if (encoded_source.is_null()) {
            throw std::invalid_argument("Stored Program fork source debit must be non-null");
        }
        fork_source_lineage =
            ProgramRunLineage::parse(detail::canonical_json_bytes(encoded_source));
    }
    std::optional<ProgramContextPublication> context_publication;
    if (schema_version >= 4) {
        const auto& encoded_context = rv(v, "context_publication");
        if (!encoded_context.is_null()) {
            if (!encoded_context.is_object()) {
                throw std::invalid_argument("Program context publication must be object or null");
            }
            detail::reject_unknown_fields(encoded_context, "Stored Program context publication",
                                          {"epoch", "artifacts", "assembly_receipt"});
            const auto epoch = ContextEpoch::parse(
                detail::canonical_json_bytes(rv(encoded_context, "epoch")));
            const auto& encoded_artifacts = rv(encoded_context, "artifacts");
            if (!encoded_artifacts.is_array()) {
                throw std::invalid_argument("Program context artifacts must be array");
            }
            std::vector<ContextArtifact> artifacts;
            artifacts.reserve(encoded_artifacts.size());
            for (const auto& artifact : encoded_artifacts) {
                artifacts.push_back(ContextArtifact::parse(detail::canonical_json_bytes(artifact)));
            }
            auto receipt = ContextAssemblyReceipt::parse(
                detail::canonical_json_bytes(rv(encoded_context, "assembly_receipt")), epoch,
                artifacts);
            context_publication = ProgramContextPublication{
                epoch, std::move(artifacts), std::move(receipt)};
        }
    }
    std::vector<HookOutboxEntry> hook_outbox_entries;
    if (schema_version >= 5) {
        const auto& encoded_hooks = rv(v, "hook_outbox_entries");
        if (!encoded_hooks.is_array()) throw std::invalid_argument("Program hook outbox must be array");
        for (const auto& entry : encoded_hooks)
            hook_outbox_entries.push_back(HookOutboxEntry::parse(detail::canonical_json_bytes(entry)));
    }
    ProgramTransitionPublication out{std::move(run), std::move(journal), std::move(events),
                                        std::move(effects), std::move(migration_plan),
                                        std::move(commands), std::move(run_generation),
                                        std::move(run_lineage), std::move(fork_source_lineage),
                                         std::move(context_publication), std::move(hook_outbox_entries)};
    if (schema_version == 6 || v.contains("child_synthesis_records")) {
        const auto& entries = rv(v, "child_synthesis_records");
        if (!entries.is_array() || entries.size() != 1)
            throw std::invalid_argument("Program synthesis publication requires one record");
        for (const auto& entry : entries)
            out.child_synthesis_records.push_back(
                ProgramChildSynthesisRecord::parse(detail::canonical_json_bytes(entry)));
    }
    if (schema_version == 7) {
        const auto& source = rv(v, "recorded_replay_source");
        if (!source.is_object())
            throw std::invalid_argument("Recorded replay source receipt must be an object");
        detail::reject_unknown_fields(source, "Recorded replay source receipt",
                                      {"run_id", "run_record_id", "journal_head", "lineage_head_id"});
        out.recorded_replay_source = ProgramRecordedReplaySource{
            rs(source, "run_id"), rs(source, "run_record_id"),
            rs(source, "journal_head"), rs(source, "lineage_head_id")};
        out.recorded_replay_source_lineage = ProgramRunLineage::parse(
            detail::canonical_json_bytes(rv(v, "recorded_replay_source_lineage")));
    }
    validate_pub(out, out.run_record.owner_scope());
    return out;
}
std::vector<ProgramJavaScriptCommandJournalEntry> ProgramTransitionStore::load_javascript_commands(
    std::string_view, std::string_view, std::uint64_t) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support durable JavaScript command-history reads");
}

std::optional<ProgramRunPublicationHead> ProgramTransitionStore::load_run_publication_head(
    std::string_view owner, std::string_view run_id) const {
    for (int retry = 0; retry < 3; ++retry) {
        auto run = load(owner, run_id);
        if (!run) return std::nullopt;
        auto journal = latest(owner, run_id);
        const auto after = load(owner, run_id);
        if (!after) return std::nullopt;
        if (after->id() != run->id()) continue;
        if (!journal) return std::nullopt;
        detail::validate_run_publication_head(*run, *journal, owner, run_id);
        return ProgramRunPublicationHead{std::move(*run), std::move(*journal)};
    }
    return std::nullopt;
}

std::optional<ProgramCommandPublicationHead> ProgramTransitionStore::load_command_publication_head(
    std::string_view owner, std::string_view run_id) const {
    const auto run = load(owner, run_id);
    if (!run) return std::nullopt;
    const auto journal = latest(owner, run_id);
    if (!journal || run->journal_head() != journal->id) return std::nullopt;
    const auto commands = load_javascript_commands(owner, run_id);
    const auto after = load(owner, run_id);
    if (!after || after->id() != run->id()) return std::nullopt;
    ProgramCommandPublicationHead head{
        *run, *journal, commands.empty() ? std::nullopt
                                        : std::optional<ProgramJavaScriptCommandJournalEntry>(commands.back())};
    detail::validate_command_publication_head(head, owner, run_id);
    return head;
}

std::vector<ProgramContextPublication> ProgramTransitionStore::load_context_publications(
    std::string_view, std::string_view, std::uint64_t) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support durable context-publication reads");
}

std::vector<ProgramChildSynthesisRecord> ProgramTransitionStore::load_child_syntheses(
    std::string_view, std::string_view) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support durable child synthesis reads");
}

std::vector<HookOutboxEntry> ProgramTransitionStore::load_hook_outbox_entries(
    std::string_view, std::string_view) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support durable hook-outbox reads");
}

void validate_program_context_publication(const ProgramContextPublication& context,
                                          const ProgramRunRecord& run) {
    validate_context_publication(context, run);
}

bool is_valid_program_context_history_append(
    const std::vector<ProgramContextPublication>& old_context,
    const std::optional<ProgramContextPublication>& next_context) {
    return valid_context_history_append(old_context, next_context);
}

void validate_program_hook_outbox_entry(const HookOutboxEntry& entry,
                                        const ProgramRunRecord& run) {
    if (entry.data().event.owner_scope() != run.owner_scope() ||
        entry.data().event.run_id() != run.run_id()) {
        throw std::invalid_argument("Program hook outbox entry does not bind the run");
    }
    // Reparse validates the immutable id, embedded invocation, event, and receipt.
    if (HookOutboxEntry::parse(entry.serialize_canonical()).id() != entry.id()) {
        throw std::invalid_argument("Program hook outbox entry identity mismatch");
    }
}

bool is_valid_program_hook_history_append(const std::vector<HookOutboxEntry>& old_entries,
                                          const std::vector<HookOutboxEntry>& next_entries,
                                          const ProgramRunRecord& run) {
    return valid_hook_history_append(old_entries, next_entries, run);
}

bool is_valid_program_runtime_context_clone(
    const std::vector<ProgramContextPublication>& source_contexts,
    const ProgramRunRecord& target_run,
    const std::optional<ProgramContextPublication>& target_context) noexcept {
    try {
        if (source_contexts.empty()) return !target_context;
        if (!target_context) return false;
        const auto& source = source_contexts.back();
        const auto& target = *target_context;
        if (target.epoch.run_id() != target_run.run_id() || target.epoch.sequence() != 1 ||
            target.epoch.predecessor_id() || target.epoch.feed_id() != source.epoch.feed_id() ||
            target.epoch.raw_from_sequence() != source.epoch.raw_from_sequence() ||
            target.epoch.raw_through_sequence() != source.epoch.raw_through_sequence() ||
            target.epoch.raw_window_digest() != source.epoch.raw_window_digest() ||
            target.epoch.artifact_ids() != source.epoch.artifact_ids() ||
            target.epoch.guarantee_profile() != source.epoch.guarantee_profile() ||
            target.assembly_receipt.normalized_request_digest() !=
                source.assembly_receipt.normalized_request_digest() ||
            target.assembly_receipt.message_window_digest() !=
                source.assembly_receipt.message_window_digest() ||
            target.assembly_receipt.artifact_ids() !=
                source.assembly_receipt.artifact_ids() ||
            target.assembly_receipt.required_skill_artifact_ids() !=
                source.assembly_receipt.required_skill_artifact_ids() ||
            target.assembly_receipt.raw_from_sequence() !=
                source.assembly_receipt.raw_from_sequence() ||
            target.assembly_receipt.raw_through_sequence() !=
                source.assembly_receipt.raw_through_sequence() ||
            target.assembly_receipt.estimated_input_tokens() !=
                source.assembly_receipt.estimated_input_tokens() ||
            target.assembly_receipt.mandatory_input_tokens() !=
                source.assembly_receipt.mandatory_input_tokens() ||
            target.artifacts.size() != source.artifacts.size()) {
            return false;
        }
        for (std::size_t index = 0; index < source.artifacts.size(); ++index) {
            if (source.artifacts[index].serialize_canonical() !=
                target.artifacts[index].serialize_canonical()) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool is_valid_program_runtime_state_transfer(
    const std::optional<ProgramRuntimeStateTransferReceipt>& receipt,
    const ProgramRunGeneration& source_generation,
    const ProgramRunLineage& source_lineage,
    const ProgramRunRecord& source_run,
    const std::vector<ProgramContextPublication>& source_contexts,
    const std::vector<HookOutboxEntry>& source_hooks,
    const ProgramRunGeneration& target_generation,
    const ProgramRunRecord& target_run,
    const std::optional<ProgramContextPublication>& target_context) noexcept {
    try {
        std::vector<const HookOutboxEntry*> unresolved;
        for (const auto& hook : source_hooks) {
            const auto state = hook.data().state;
            if (state != HookExecutionState::Succeeded && state != HookExecutionState::Cancelled) {
                unresolved.push_back(&hook);
            }
        }
        const auto prior_transfer = source_generation.runtime_state_transfer_receipt();
        if (!receipt) {
            return source_contexts.empty() && unresolved.empty() && !target_context &&
                   (!prior_transfer || prior_transfer->hook_references().empty());
        }
        const auto& value = *receipt;
        const auto target_receipt = target_generation.runtime_state_transfer_receipt();
        if (!target_receipt || target_receipt->id() != value.id() ||
            value.owner_scope() != source_generation.owner_scope() ||
            value.owner_scope() != target_generation.owner_scope() ||
            value.lineage_id() != source_generation.lineage_id() ||
            value.lineage_id() != target_generation.lineage_id() ||
            value.source_generation_id() != source_generation.id() ||
            value.source_lineage_head_id() != source_lineage.id() ||
            value.source_run_id() != source_run.run_id() ||
            value.source_run_record_id() != source_run.id() ||
            value.source_journal_head() != source_run.journal_head() ||
            value.target_generation() != target_generation.generation() ||
            value.target_run_id() != target_run.run_id() ||
            value.target_initial_run_record_id() != target_run.id() ||
            value.target_initial_journal_head() != target_run.journal_head() ||
            target_generation.generation() != source_generation.generation() + 1 ||
            !target_generation.predecessor_generation_id() ||
            *target_generation.predecessor_generation_id() != source_generation.id() ||
            source_lineage.active_generation_id() != source_generation.id() ||
            source_lineage.active_run_record_id() != source_run.id() ||
            source_lineage.active_journal_head() != source_run.journal_head() ||
            value.source_context_epoch_ids().size() != source_contexts.size()) {
            return false;
        }
        for (std::size_t index = 0; index < source_contexts.size(); ++index) {
            if (value.source_context_epoch_ids()[index] != source_contexts[index].epoch.id()) {
                return false;
            }
        }
        std::vector<ProgramRuntimeStateTransferHookReference> expected_hooks;
        if (prior_transfer) {
            expected_hooks = prior_transfer->hook_references();
        }
        for (const auto* hook : unresolved) {
            const auto duplicate = std::find_if(
                expected_hooks.begin(), expected_hooks.end(), [&](const auto& reference) {
                    return reference.invocation_id == hook->data().invocation.id();
                });
            if (duplicate != expected_hooks.end()) return false;
            expected_hooks.push_back(
                {hook->data().invocation.id(), hook->id(), hook->data().event.id()});
        }
        if (value.hook_references() != expected_hooks) {
            return false;
        }
        for (const auto* hook : unresolved) {
            if (hook->data().event.owner_scope() != source_run.owner_scope() ||
                hook->data().event.run_id() != source_run.run_id()) {
                return false;
            }
        }
        if (source_contexts.empty()) {
            return !value.target_context_epoch_id() && !target_context;
        }
        return value.target_context_epoch_id() && target_context &&
               *value.target_context_epoch_id() == target_context->epoch.id() &&
               is_valid_program_runtime_context_clone(
                   source_contexts, target_run, target_context);
    } catch (...) {
        return false;
    }
}

ProgramEffectiveRuntimeState ProgramTransitionStore::load_effective_runtime_state(
    std::string_view owner_scope, std::string_view run_id) const {
    ProgramEffectiveRuntimeState result;
    result.context_publications = load_context_publications(owner_scope, run_id);
    result.hook_outbox_entries = load_hook_outbox_entries(owner_scope, run_id);
    const auto target_run = load(owner_scope, run_id);
    const auto lineage = target_run ? load_run_lineage(owner_scope, run_id) : std::nullopt;
    std::optional<ProgramRunGeneration> target_generation;
    if (lineage) {
        for (auto generation = lineage->active_generation(); generation > 0; --generation) {
            auto candidate = load_generation(owner_scope, lineage->lineage_id(), generation);
            if (candidate && candidate->run_id() == run_id) {
                target_generation = std::move(candidate);
                break;
            }
        }
    }
    if (!target_run || !lineage || !target_generation) {
        return result;
    }
    result.transfer_receipt = target_generation->runtime_state_transfer_receipt();
    if (!result.transfer_receipt) return result;
    const auto& receipt = *result.transfer_receipt;
    if (target_generation->generation() <= 1) {
        throw std::runtime_error("Runtime-state transfer target generation is invalid");
    }
    const auto source_generation = load_generation(
        owner_scope, lineage->lineage_id(), target_generation->generation() - 1);
    const auto source_lineage = load_lineage_head(
        owner_scope, lineage->lineage_id(), receipt.source_lineage_head_id());
    const auto source_run = load(owner_scope, receipt.source_run_id());
    const auto target_initial = load_generation_initial_publication(
        owner_scope, lineage->lineage_id(), target_generation->generation());
    if (!source_generation || !source_lineage || !source_run || !target_initial) {
        throw std::runtime_error("Runtime-state transfer source evidence is unavailable");
    }
    auto source_state = load_effective_runtime_state(owner_scope, receipt.source_run_id());
    const auto& source_contexts = source_state.context_publications;
    const auto& source_hooks = source_state.hook_outbox_entries;
    const auto target_context = target_initial->context_publication;
    if (!is_valid_program_runtime_state_transfer(
            result.transfer_receipt, *source_generation, *source_lineage, *source_run,
            source_contexts, source_hooks, *target_generation, target_initial->run_record,
            target_context)) {
        throw std::runtime_error("Runtime-state transfer evidence is invalid");
    }
    std::vector<HookOutboxEntry> source_hook_heads = source_state.inherited_hook_outbox_entries;
    source_hook_heads.insert(source_hook_heads.end(), source_hooks.begin(), source_hooks.end());
    for (const auto& reference : receipt.hook_references()) {
        const auto found = std::find_if(
            source_hook_heads.begin(), source_hook_heads.end(), [&](const auto& hook) {
                return hook.data().invocation.id() == reference.invocation_id &&
                       hook.id() == reference.head_id &&
                       hook.data().event.id() == reference.event_id;
            });
        if (found == source_hook_heads.end()) {
            throw std::runtime_error("Transferred hook head is unavailable");
        }
        result.inherited_hook_outbox_entries.push_back(*found);
    }
    return result;
}

std::optional<ProgramTransitionPublication>
ProgramTransitionStore::load_generation_initial_publication(
    std::string_view, std::string_view, std::uint64_t) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support generation publication-history reads");
}

struct InMemoryProgramTransitionStore::Impl {
    struct Stored {
        ProgramRunRecord                         run;
        ProgramJournalRecord                     journal;
        std::vector<ProgramEvent>                events;
        std::vector<ProgramEffectOutboxEntry>    effects;
        std::vector<ProgramJavaScriptCommandJournalEntry> commands;
        std::vector<ProgramContextPublication>             context_publications;
        std::vector<HookOutboxEntry>                        hook_outbox_entries;
        std::vector<ProgramChildSynthesisRecord>            child_syntheses;
        ProgramTransitionHistoryPtr              history;
        std::optional<MigrationPlan>             migration_plan;
        std::optional<std::string>               lineage_id;
        std::string                              bytes;
    };
    struct StoredLineage {
        ProgramRunLineage                         head;
        std::map<std::uint64_t, ProgramRunGeneration> generations;
        std::map<std::uint64_t, std::string>          initial_publications;
        std::map<std::string, ProgramRunLineage, std::less<>> heads;
    };
    mutable std::mutex                                                mutex;
    std::map<std::string, std::shared_ptr<const Stored>, std::less<>> runs;
    std::map<std::string, std::shared_ptr<const StoredLineage>, std::less<>> lineages;
    std::map<std::string, GraphMigrationCapsule, std::less<>> graph_migration_capsules;
    std::map<std::string, ProgramExecutionLease, std::less<>> execution_leases;
    std::optional<ProgramTransitionFaultPoint>                        fault;
    class Reads;
};

// The stored state as the transition validator reads it. The caller holds `mutex`, so what
// a read points into stays as it is until the publication is staged.
class InMemoryProgramTransitionStore::Impl::Reads final
    : public detail::transition::TransitionReads {
public:
    Reads(const Impl& impl, std::string_view owner_scope) : impl_(impl), owner_scope_(owner_scope) {}

    std::optional<GraphMigrationCapsule> graph_migration_capsule(
        std::string_view run_id, std::string_view lineage_head_id) override {
        const auto found = impl_.graph_migration_capsules.find(
            graph_migration_capsule_key(owner_scope_, run_id, lineage_head_id));
        if (found == impl_.graph_migration_capsules.end()) return std::nullopt;
        return found->second;
    }
    bool effect_id_published(std::string_view run_id, std::string_view effect_id) override {
        const auto* run = stored(run_id);
        return run && effect_history_contains(run->effects, run->history, effect_id);
    }
    Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>> command_log(
        std::string_view run_id) override {
        return history_of(stored(run_id), &Stored::commands);
    }
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(
        std::string_view run_id) override {
        const auto* run = stored(run_id);
        if (!run || run->commands.empty()) return std::nullopt;
        return run->commands.back();
    }
    std::optional<ProgramRunRecord> run_record(std::string_view run_id, Lock) override {
        const auto* run = stored(run_id);
        if (!run) return std::nullopt;
        return run->run;
    }
    std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id, Lock) override {
        const auto* lineage = stored_lineage(lineage_id);
        if (!lineage) return std::nullopt;
        return lineage->head;
    }
    std::optional<ProgramRunGeneration> generation(std::string_view lineage_id,
                                                   std::uint64_t    ordinal) override {
        const auto* lineage = stored_lineage(lineage_id);
        if (!lineage) return std::nullopt;
        const auto found = lineage->generations.find(ordinal);
        if (found == lineage->generations.end()) return std::nullopt;
        return found->second;
    }
    std::optional<std::string> run_lineage_id(std::string_view run_id) override {
        const auto* run = stored(run_id);
        return run ? run->lineage_id : std::nullopt;
    }
    Borrowed<std::vector<ProgramContextPublication>> context_history(
        std::string_view run_id) override {
        return history_of(stored(run_id), &Stored::context_publications);
    }
    Borrowed<std::vector<ProgramChildSynthesisRecord>> synthesis_heads(
        std::string_view run_id) override {
        return history_of(stored(run_id), &Stored::child_syntheses);
    }
    Borrowed<std::vector<HookOutboxEntry>> hook_heads(std::string_view run_id,
                                                      const ProgramRunRecord&) override {
        return history_of(stored(run_id), &Stored::hook_outbox_entries);
    }

private:
    // One of the run's stored histories, or an empty one for a run that is not stored.
    template <typename Entry>
    static Borrowed<std::vector<Entry>> history_of(const Stored* run,
                                                   std::vector<Entry> Stored::*history) {
        if (!run) return Borrowed<std::vector<Entry>>(std::vector<Entry>{});
        return Borrowed<std::vector<Entry>>(run->*history);
    }
    const Stored* stored(std::string_view run_id) const {
        const auto found = impl_.runs.find(key(owner_scope_, run_id));
        return found == impl_.runs.end() ? nullptr : found->second.get();
    }
    const StoredLineage* stored_lineage(std::string_view lineage_id) const {
        const auto found = impl_.lineages.find(key(owner_scope_, lineage_id));
        return found == impl_.lineages.end() ? nullptr : found->second.get();
    }

    const Impl&      impl_;
    std::string_view owner_scope_;
};
InMemoryProgramTransitionStore::InMemoryProgramTransitionStore()
    : impl_(std::make_unique<Impl>()) {}
InMemoryProgramTransitionStore::~InMemoryProgramTransitionStore() = default;
InMemoryProgramTransitionStore::InMemoryProgramTransitionStore(
    InMemoryProgramTransitionStore&&) noexcept = default;
InMemoryProgramTransitionStore& InMemoryProgramTransitionStore::operator=(
    InMemoryProgramTransitionStore&&) noexcept = default;

void InMemoryProgramTransitionStore::fail_next_publication_for_testing(
    ProgramTransitionFaultPoint point) {
    std::lock_guard lock(impl_->mutex);
    impl_->fault = point;
}

std::optional<ProgramRunPublicationHead>
InMemoryProgramTransitionStore::load_run_publication_head(std::string_view owner,
                                                         std::string_view run_id) const {
    if (owner.empty() || run_id.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->runs.find(key(owner, run_id));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    detail::validate_run_publication_head(snapshot->run, snapshot->journal, owner, run_id);
    return ProgramRunPublicationHead{snapshot->run, snapshot->journal};
}

std::optional<ProgramCommandPublicationHead>
InMemoryProgramTransitionStore::load_command_publication_head(std::string_view owner,
                                                             std::string_view run_id) const {
    if (owner.empty() || run_id.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->runs.find(key(owner, run_id));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    ProgramCommandPublicationHead head{
        snapshot->run, snapshot->journal,
        snapshot->commands.empty() ? std::nullopt
                                   : std::optional<ProgramJavaScriptCommandJournalEntry>(snapshot->commands.back())};
    detail::validate_command_publication_head(head, owner, run_id);
    return head;
}
std::optional<ProgramRunRecord> InMemoryProgramTransitionStore::load(std::string_view o,
                                                                     std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->run;
}
std::optional<ProgramJournalRecord> InMemoryProgramTransitionStore::latest(
    std::string_view o, std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->journal;
}
std::vector<ProgramEvent> InMemoryProgramTransitionStore::load_events(std::string_view o,
                                                                      std::string_view r,
                                                                      std::uint64_t    a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    if (snapshot->history) return event_entries_after(snapshot->history, a);
    return entries_after(snapshot->events, a,
                         [](const ProgramEvent& event) { return event.sequence; });
}
std::vector<ProgramEffectOutboxEntry> InMemoryProgramTransitionStore::load_effects(
    std::string_view o, std::string_view r, std::uint64_t a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    if (snapshot->history) return effect_entries_after(snapshot->history, a);
    return entries_after(snapshot->effects, a,
                         [](const ProgramEffectOutboxEntry& effect) {
                         return effect.sequence();
                         });
}
std::vector<ProgramJavaScriptCommandJournalEntry>
InMemoryProgramTransitionStore::load_javascript_commands(std::string_view o,
                                                         std::string_view r,
                                                         std::uint64_t    a) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    return entries_after(snapshot->commands, a,
                          [](const ProgramJavaScriptCommandJournalEntry& command) {
                              return command.sequence();
                          });
}
std::vector<ProgramContextPublication> InMemoryProgramTransitionStore::load_context_publications(
    std::string_view o, std::string_view r, std::uint64_t after_sequence) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return {};
        snapshot = found->second;
    }
    std::vector<ProgramContextPublication> result;
    for (const auto& context : snapshot->context_publications) {
        if (context.epoch.sequence() > after_sequence) result.push_back(context);
    }
    return result;
}
std::vector<ProgramChildSynthesisRecord> InMemoryProgramTransitionStore::load_child_syntheses(
    std::string_view owner_scope, std::string_view run_id) const {
    std::lock_guard lock(impl_->mutex);
    auto            found = impl_->runs.find(key(owner_scope, run_id));
    if (found == impl_->runs.end()) return {};
    return found->second->child_syntheses;
}

std::vector<HookOutboxEntry> InMemoryProgramTransitionStore::load_hook_outbox_entries(
    std::string_view o, std::string_view r) const {
    std::shared_ptr<const Impl::Stored> snapshot;
    { std::lock_guard lock(impl_->mutex); const auto found = impl_->runs.find(key(o, r));
      if (found == impl_->runs.end()) return {}; snapshot = found->second; }
    return snapshot->hook_outbox_entries;
}
std::optional<MigrationPlan> InMemoryProgramTransitionStore::load_migration_plan(
    std::string_view o, std::string_view r) const {
    if (o.empty() || r.empty()) return std::nullopt;
    std::shared_ptr<const Impl::Stored> snapshot;
    {
        std::lock_guard lock(impl_->mutex);
        const auto      found = impl_->runs.find(key(o, r));
        if (found == impl_->runs.end()) return std::nullopt;
        snapshot = found->second;
    }
    return snapshot->migration_plan;
}
std::optional<ProgramRunLineage> ProgramTransitionStore::load_run_lineage(
    std::string_view, std::string_view) const {
    return std::nullopt;
}
std::optional<ProgramRunLineage> InMemoryProgramTransitionStore::load_lineage(
    std::string_view owner, std::string_view lineage_id) const {
    if (owner.empty() || lineage_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->lineages.find(key(owner, lineage_id));
    if (found == impl_->lineages.end()) return std::nullopt;
    return found->second->head;
}
std::optional<ProgramRunLineage> InMemoryProgramTransitionStore::load_run_lineage(
    std::string_view owner, std::string_view run_id) const {
    if (owner.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto run = impl_->runs.find(key(owner, run_id));
    if (run == impl_->runs.end() || !run->second->lineage_id) return std::nullopt;
    const auto lineage = impl_->lineages.find(key(owner, *run->second->lineage_id));
    if (lineage == impl_->lineages.end())
        throw std::invalid_argument("Stored Program run lineage association is corrupt");
    return lineage->second->head;
}
std::optional<ProgramRunLineage> InMemoryProgramTransitionStore::load_lineage_head(
    std::string_view owner, std::string_view lineage_id, std::string_view head_id) const {
    if (owner.empty() || lineage_id.empty() || head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto lineage = impl_->lineages.find(key(owner, lineage_id));
    if (lineage == impl_->lineages.end()) return std::nullopt;
    const auto found = lineage->second->heads.find(head_id);
    if (found == lineage->second->heads.end()) return std::nullopt;
    return found->second;
}
std::optional<ProgramRunGeneration> InMemoryProgramTransitionStore::load_generation(
    std::string_view owner, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto lineage = impl_->lineages.find(key(owner, lineage_id));
    if (lineage == impl_->lineages.end()) return std::nullopt;
    const auto found = lineage->second->generations.find(generation);
    if (found == lineage->second->generations.end()) return std::nullopt;
    return found->second;
}
std::optional<ProgramTransitionPublication>
InMemoryProgramTransitionStore::load_generation_initial_publication(
    std::string_view owner, std::string_view lineage_id, std::uint64_t generation) const {
    if (owner.empty() || lineage_id.empty() || generation == 0) return std::nullopt;
    std::string bytes;
    {
        std::lock_guard lock(impl_->mutex);
        const auto lineage = impl_->lineages.find(key(owner, lineage_id));
        if (lineage == impl_->lineages.end()) return std::nullopt;
        const auto found = lineage->second->initial_publications.find(generation);
        if (found == lineage->second->initial_publications.end()) return std::nullopt;
        bytes = found->second;
    }
    return ProgramTransitionPublication::parse(bytes);
}
std::optional<GraphMigrationCapsule>
InMemoryProgramTransitionStore::load_graph_migration_capsule(
    std::string_view owner, std::string_view run_id,
    std::string_view source_lineage_head_id) const {
    if (owner.empty() || run_id.empty() || source_lineage_head_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->graph_migration_capsules.find(
        graph_migration_capsule_key(owner, run_id, source_lineage_head_id));
    return found == impl_->graph_migration_capsules.end()
        ? std::nullopt : std::optional<GraphMigrationCapsule>{found->second};
}
std::optional<ProgramExecutionLease>
InMemoryProgramTransitionStore::load_execution_lease(
    std::string_view owner, std::string_view run_id) const {
    if (owner.empty() || run_id.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->execution_leases.find(key(owner, run_id));
    return found == impl_->execution_leases.end()
        ? std::nullopt : std::optional<ProgramExecutionLease>{found->second};
}
ProgramTransitionPublishResult InMemoryProgramTransitionStore::compare_publish(
    std::string_view owner, std::string_view expected, ProgramTransitionPublication publication) {
    return compare_publish_impl(owner, expected, std::move(publication), nullptr, nullptr,
                                nullptr, nullptr);
}
ProgramTransitionPublishResult
InMemoryProgramTransitionStore::compare_publish_execution(
    std::string_view owner, std::string_view expected,
    ProgramTransitionPublication publication,
    std::optional<ProgramExecutionLease> expected_lease,
    std::optional<ProgramExecutionLease> next_lease) {
    return compare_publish_impl(owner, expected, std::move(publication), nullptr, nullptr,
                                expected_lease ? &*expected_lease : nullptr,
                                next_lease ? &*next_lease : nullptr);
}
ProgramTransitionPublishResult
InMemoryProgramTransitionStore::compare_publish_graph_safe_point(
    std::string_view owner,
    std::string_view expected,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence& evidence,
    const GraphMigrationCapsule& capsule,
    const ProgramExecutionLease& execution_lease) {
    return compare_publish_impl(owner, expected, std::move(publication), &evidence, &capsule,
                                &execution_lease, nullptr);
}
ProgramTransitionPublishResult InMemoryProgramTransitionStore::compare_publish_impl(
    std::string_view owner,
    std::string_view expected,
    ProgramTransitionPublication publication,
    const ProgramGraphSafePointEvidence* safe_point_evidence,
    const GraphMigrationCapsule* safe_point_capsule,
    const ProgramExecutionLease* expected_lease,
    const ProgramExecutionLease* next_lease) {
    auto prepared = detail::transition::prepare_transition(
        owner, expected, publication, safe_point_evidence, safe_point_capsule, expected_lease,
        next_lease);
    if (!prepared) return ProgramTransitionPublishResult::Conflict;
    std::string publication_bytes = std::move(*prepared);

    auto            storage_key = key(owner, publication.run_record.run_id());
    std::lock_guard lock(impl_->mutex);
    auto            current = impl_->runs.find(storage_key);
    auto current_lease = impl_->execution_leases.find(storage_key);

    std::optional<detail::transition::CurrentHead> head;
    if (current != impl_->runs.end()) {
        const auto& stored = *current->second;
        head.emplace(detail::transition::CurrentHead{stored.run, stored.journal,
                                                     stored.migration_plan, stored.bytes});
    }
    Impl::Reads                        reads(*impl_, owner);
    detail::transition::TransitionPlan plan;
    switch (detail::transition::validate_transition(
        {owner, expected, publication, publication_bytes, safe_point_evidence, safe_point_capsule,
         expected_lease, next_lease},
        head ? &*head : nullptr,
        current_lease == impl_->execution_leases.end() ? nullptr : &current_lease->second, reads,
        plan)) {
    case detail::transition::TransitionVerdict::Conflict:
        return ProgramTransitionPublishResult::Conflict;
    case detail::transition::TransitionVerdict::AlreadyPresent:
        return ProgramTransitionPublishResult::AlreadyPresent;
    case detail::transition::TransitionVerdict::Proceed:
        break;
    }

    // The checks passed, so every lineage the publication names is stored (or is created by it).
    const auto lineage_key = publication.run_lineage
                                 ? key(owner, publication.run_lineage->lineage_id())
                                 : std::string{};
    auto current_lineage = publication.run_lineage ? impl_->lineages.find(lineage_key)
                                                    : impl_->lineages.end();
    std::string                                fork_source_key;
    std::shared_ptr<const Impl::StoredLineage> fork_source_current;
    if (publication.fork_source_lineage) {
        fork_source_key = key(owner, publication.fork_source_lineage->lineage_id());
        fork_source_current = impl_->lineages.find(fork_source_key)->second;
    }
    std::string replay_source_key;
    std::shared_ptr<const Impl::StoredLineage> replay_source_current;
    if (publication.recorded_replay_source) {
        replay_source_key = key(owner, publication.recorded_replay_source_lineage->lineage_id());
        replay_source_current = impl_->lineages.find(replay_source_key)->second;
    }

    const auto synthesis_heads = current == impl_->runs.end()
                                     ? std::vector<ProgramChildSynthesisRecord>{}
                                     : current->second->child_syntheses;
    auto next_syntheses = synthesis_heads;
    for (const auto& entry : publication.child_synthesis_records) {
        auto found =
            std::find_if(next_syntheses.begin(), next_syntheses.end(), [&](const auto& old) {
                return old.data().proposal.id() == entry.data().proposal.id();
            });
        if (found == next_syntheses.end())
            next_syntheses.push_back(entry);
        else
            *found = entry;
    }

    const auto maybe_fail = [&](ProgramTransitionFaultPoint point) {
        if (impl_->fault && *impl_->fault == point) {
            impl_->fault.reset();
            throw std::runtime_error("injected in-memory Program transition failure");
        }
    };
    auto staged_run = std::move(publication.run_record);
    maybe_fail(ProgramTransitionFaultPoint::AfterRunSnapshot);
    auto staged_journal = std::move(publication.journal_record);
    maybe_fail(ProgramTransitionFaultPoint::AfterJournalSnapshot);

    auto appended_events = std::move(publication.events);
    maybe_fail(ProgramTransitionFaultPoint::AfterEventSnapshot);
    auto appended_effects = std::move(publication.effects);
    maybe_fail(ProgramTransitionFaultPoint::AfterEffectSnapshot);
    auto appended_commands = std::move(publication.commands);
    auto appended_context = std::move(publication.context_publication);
    auto appended_hooks = std::move(publication.hook_outbox_entries);
    maybe_fail(ProgramTransitionFaultPoint::AfterContextSnapshot);
    maybe_fail(ProgramTransitionFaultPoint::AfterHookSnapshot);

    std::shared_ptr<const Impl::StoredLineage> staged_lineage;
    if (publication.run_lineage) {
        std::map<std::uint64_t, ProgramRunGeneration> generations;
        std::map<std::uint64_t, std::string>          initial_publications;
        std::map<std::string, ProgramRunLineage, std::less<>> heads;
        if (current_lineage != impl_->lineages.end()) {
            generations = current_lineage->second->generations;
            initial_publications = current_lineage->second->initial_publications;
        }
        if (current_lineage != impl_->lineages.end()) heads = current_lineage->second->heads;
        if (publication.run_generation) {
            generations.emplace(publication.run_generation->generation(),
                                *publication.run_generation);
            initial_publications.emplace(publication.run_generation->generation(),
                                         publication_bytes);
        }
        heads.emplace(publication.run_lineage->id(), *publication.run_lineage);
        staged_lineage = std::make_shared<const Impl::StoredLineage>(Impl::StoredLineage{
            *publication.run_lineage, std::move(generations),
            std::move(initial_publications), std::move(heads)});
    }
    std::shared_ptr<const Impl::StoredLineage> staged_fork_source;
    if (publication.fork_source_lineage) {
        auto heads = fork_source_current->heads;
        heads.emplace(publication.fork_source_lineage->id(),
                      *publication.fork_source_lineage);
        staged_fork_source = std::make_shared<const Impl::StoredLineage>(Impl::StoredLineage{
            *publication.fork_source_lineage, fork_source_current->generations,
            fork_source_current->initial_publications,
            std::move(heads)});
    }
    std::shared_ptr<const Impl::StoredLineage> staged_replay_source;
    if (publication.recorded_replay_source_lineage) {
        auto heads = replay_source_current->heads;
        heads.emplace(publication.recorded_replay_source_lineage->id(),
                      *publication.recorded_replay_source_lineage);
        staged_replay_source = std::make_shared<const Impl::StoredLineage>(Impl::StoredLineage{
            *publication.recorded_replay_source_lineage, replay_source_current->generations,
            replay_source_current->initial_publications, std::move(heads)});
    }
    maybe_fail(ProgramTransitionFaultPoint::AfterLineageSnapshot);

    std::vector<ProgramEvent>            events;
    std::vector<ProgramEffectOutboxEntry> effects;
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    std::vector<ProgramContextPublication>             context_publications;
    std::vector<HookOutboxEntry>                        hook_outbox_entries;
    ProgramTransitionHistoryPtr           history;
    if (current == impl_->runs.end()) {
        events  = std::move(appended_events);
        effects = std::move(appended_effects);
        commands = std::move(appended_commands);
        if (appended_context) context_publications.push_back(std::move(*appended_context));
        hook_outbox_entries = std::move(appended_hooks);
    } else {
        const auto& old = *current->second;
        if (old.history) {
            history =
                append_history(old.history, std::move(appended_events), std::move(appended_effects));
        } else if (appended_events.empty() && appended_effects.empty()) {
            events  = old.events;
            effects = old.effects;
        } else {
            history = begin_history(old.events, old.effects, std::move(appended_events),
                                    std::move(appended_effects));
        }
        commands = append_entries(old.commands, std::move(appended_commands));
        context_publications = old.context_publications;
        if (appended_context) context_publications.push_back(std::move(*appended_context));
        hook_outbox_entries = old.hook_outbox_entries;
        for (auto& entry : appended_hooks) {
            auto existing = std::find_if(hook_outbox_entries.begin(), hook_outbox_entries.end(),
                [&](const auto& value) { return value.data().invocation.id() == entry.data().invocation.id(); });
            if (existing == hook_outbox_entries.end()) hook_outbox_entries.push_back(std::move(entry));
            else *existing = std::move(entry);
        }
    }

    auto migration_plan = current == impl_->runs.end()
                              ? std::move(publication.migration_plan)
                              : current->second->migration_plan;
    auto candidate      = std::make_shared<const Impl::Stored>(Impl::Stored{
        std::move(staged_run), std::move(staged_journal), std::move(events), std::move(effects),
        std::move(commands), std::move(context_publications), std::move(hook_outbox_entries),
        std::move(next_syntheses), std::move(history), std::move(migration_plan),
        publication.run_lineage ? std::optional<std::string>(publication.run_lineage->lineage_id())
                                     : std::nullopt,
        std::move(publication_bytes)});
    if (staged_lineage || staged_fork_source || staged_replay_source || safe_point_capsule ||
        expected_lease || next_lease) {
        // Allocate only absent index nodes. Existing nodes already own their
        // storage; their immutable handles can be swapped without allocating.
        decltype(impl_->runs) inserted_runs;
        if (current == impl_->runs.end())
            inserted_runs.emplace(storage_key, std::move(candidate));
        decltype(impl_->lineages) inserted_lineages;
        if (staged_lineage && current_lineage == impl_->lineages.end())
            inserted_lineages.emplace(lineage_key, std::move(staged_lineage));
        const auto fork_source_position = staged_fork_source
            ? impl_->lineages.find(fork_source_key) : impl_->lineages.end();
        const auto replay_source_position = staged_replay_source
            ? impl_->lineages.find(replay_source_key) : impl_->lineages.end();

        decltype(impl_->graph_migration_capsules) inserted_capsules;
        auto capsule_position = impl_->graph_migration_capsules.end();
        std::optional<GraphMigrationCapsule> staged_capsule;
        if (safe_point_capsule) {
            staged_capsule = *safe_point_capsule;
            auto capsule_key = graph_migration_capsule_key(
                owner, safe_point_capsule->source_run_id(),
                safe_point_capsule->source_lineage_head_id());
            capsule_position = impl_->graph_migration_capsules.find(capsule_key);
            if (capsule_position == impl_->graph_migration_capsules.end())
                inserted_capsules.emplace(std::move(capsule_key), std::move(*staged_capsule));
        }
        decltype(impl_->execution_leases) inserted_leases;
        std::optional<ProgramExecutionLease> staged_lease;
        if (next_lease) {
            staged_lease = *next_lease;
            if (current_lease == impl_->execution_leases.end())
                inserted_leases.emplace(storage_key, std::move(*staged_lease));
        }

        static_assert(noexcept(candidate.swap(candidate)) &&
                      noexcept(staged_lineage.swap(staged_lineage)));
        static_assert(std::is_nothrow_swappable_v<GraphMigrationCapsule> &&
                      std::is_nothrow_swappable_v<ProgramExecutionLease>);
        const auto commit_new_nodes = []<class Map>(Map& heads, Map& insertions) noexcept {
            static_assert(std::allocator_traits<typename Map::allocator_type>::is_always_equal::value);
            static_assert(std::is_nothrow_invocable_v<typename Map::key_compare,
                                                    const typename Map::key_type&,
                                                    const typename Map::key_type&>);
            heads.merge(insertions);
        };

        maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
        // Every potentially throwing operation is complete. The existing mutex
        // keeps the multi-index commit indivisible to readers and competing writers;
        // node transfer allocates nothing and immutable-handle swaps cannot throw.
        // Retain replaced handles until all indexes agree, as the old map swaps did.
        if (current != impl_->runs.end()) current->second.swap(candidate);
        commit_new_nodes(impl_->runs, inserted_runs);
        if (staged_lineage) current_lineage->second.swap(staged_lineage);
        commit_new_nodes(impl_->lineages, inserted_lineages);
        if (staged_fork_source) fork_source_position->second.swap(staged_fork_source);
        if (staged_replay_source) replay_source_position->second.swap(staged_replay_source);
        if (capsule_position != impl_->graph_migration_capsules.end())
            std::swap(capsule_position->second, *staged_capsule);
        commit_new_nodes(impl_->graph_migration_capsules, inserted_capsules);
        if (next_lease && current_lease != impl_->execution_leases.end())
            std::swap(current_lease->second, *staged_lease);
        commit_new_nodes(impl_->execution_leases, inserted_leases);
        if (expected_lease && !next_lease) {
            const auto retired_lease = impl_->execution_leases.extract(current_lease);
        }
    } else if (current == impl_->runs.end()) {
        maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
        impl_->runs.emplace(std::move(storage_key), std::move(candidate));
    } else {
        maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
        current->second = std::move(candidate);
    }
    return ProgramTransitionPublishResult::Published;
}
}  // namespace neograph::program
