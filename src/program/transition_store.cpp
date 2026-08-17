#include <neograph/program/transition_store.h>

#include "canonical_json.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
namespace neograph::program {
namespace {
constexpr std::string_view EFFECT_FORMAT      = "neograph-program-effect-outbox-entry",
                           PUBLICATION_FORMAT = "neograph-program-transition-publication";
bool is_final(ContinuationState state) noexcept {
    return state != ContinuationState::Running && state != ContinuationState::Interrupted &&
           state != ContinuationState::AmbiguousEffect;
}

bool same_child_metadata(const ProgramChildRecord& lhs, const ProgramChildRecord& rhs) {
    return lhs.child_run_id == rhs.child_run_id && lhs.link_id == rhs.link_id &&
           lhs.link_receipt == rhs.link_receipt && lhs.invocation == rhs.invocation;
}

bool valid_child_state_transition(ProgramChildState previous, ProgramChildState next) noexcept {
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

bool valid_children_transition(const ProgramRunRecord& previous,
                               const ProgramRunRecord& next) {
    const auto previous_children = previous.children();
    const auto next_children     = next.children();
    for (const auto& child : previous_children) {
        const auto found = std::find_if(next_children.begin(), next_children.end(),
                                        [&](const auto& value) {
                                            return value.child_run_id == child.child_run_id;
                                        });
        if (found == next_children.end() || !same_child_metadata(child, *found) ||
            !valid_child_state_transition(child.state, found->state))
            return false;
    }
    return true;
}

bool valid_effect_outbox_binding(const ProgramRunRecord& run,
                                 const std::vector<ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;

    // A Program has one durable pending operation at a time.  An outbox entry
    // is therefore valid only when it is the exact immutable effect that left
    // the run interrupted; a caller cannot smuggle a second effect through a
    // transition publication.
    const auto pending = run.pending_effect();
    if (!pending || pending->state() != ProgramPendingState::Awaiting || effects.size() != 1 ||
        effects.front().effect() != *pending) {
        return false;
    }
    std::set<std::string, std::less<>> ids;
    for (const auto& effect : effects) {
        if (!ids.emplace(effect.effect().effect_id()).second) return false;
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

bool effect_ids_are_unique(const std::vector<ProgramEffectOutboxEntry>& old_effects,
                           const ProgramTransitionHistoryPtr&            old_history,
                           const std::vector<ProgramEffectOutboxEntry>&  new_effects) {
    for (auto next = new_effects.begin(); next != new_effects.end(); ++next) {
        if (std::any_of(new_effects.begin(), next, [&](const auto& existing) {
                return existing.effect().effect_id() == next->effect().effect_id();
            }) ||
            effect_history_contains(old_effects, old_history, next->effect().effect_id())) {
            return false;
        }
    }
    return true;
}

bool same_command_coordinate(const ProgramJavaScriptCommandJournalEntry& lhs,
                             const ProgramJavaScriptCommandJournalEntry& rhs) {
    return lhs.bundle_id() == rhs.bundle_id() &&
           lhs.command_ordinal() == rhs.command_ordinal() &&
           lhs.coordinate_id() == rhs.coordinate_id() &&
           detail::canonical_json_bytes(lhs.command().to_json()) ==
               detail::canonical_json_bytes(rhs.command().to_json()) &&
           lhs.effect_identity() == rhs.effect_identity();
}

bool valid_command_history_append(
    const ProgramRunRecord&                                  run,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& new_commands) {
    if (new_commands.empty()) return true;

    auto prior = old_commands;
    std::uint64_t expected_sequence = 0;
    std::uint64_t highest_ordinal = 0;
    for (const auto& existing : prior) {
        if (existing.bundle_id() != run.bundle_id() || existing.sequence() != expected_sequence + 1)
            return false;
        ++expected_sequence;
        highest_ordinal = std::max(highest_ordinal, existing.command_ordinal());
    }
    for (const auto& entry : new_commands) {
        if (entry.bundle_id() != run.bundle_id() ||
            entry.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;

        const auto found = std::find_if(
            prior.rbegin(), prior.rend(), [&](const auto& previous) {
                return previous.command_ordinal() == entry.command_ordinal();
            });
        if (found == prior.rend()) {
            if (entry.command_ordinal() != highest_ordinal + 1 || !entry.pending() ||
                (!prior.empty() && prior.back().pending()))
                return false;
            highest_ordinal = entry.command_ordinal();
            prior.push_back(entry);
            continue;
        }

        // A command coordinate can only move once, from a durable pending
        // head to one authoritative terminal result.  Conflicting command
        // payloads or a second completion are rejected fail-closed.
        if (!found->pending() || !entry.completed() ||
            !same_command_coordinate(*found, entry)) {
            return false;
        }
        prior.push_back(entry);
    }
    return true;
}
bool budget_is_empty(const RunBudget& budget) noexcept {
    return budget == RunBudget{};
}

bool budget_increased(const RunBudget& next, const RunBudget& previous) noexcept {
    return next.wall_time_ms > previous.wall_time_ms || next.model_tokens > previous.model_tokens ||
           next.monetary_microunits > previous.monetary_microunits ||
           next.max_concurrency > previous.max_concurrency ||
           next.max_program_operations > previous.max_program_operations ||
           next.max_core_steps > previous.max_core_steps ||
           next.max_dynamic_compiles > previous.max_dynamic_compiles ||
           next.max_child_depth > previous.max_child_depth ||
           next.max_total_children > previous.max_total_children;
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

bool fork_resume_binds_source_target(const ForkCompatibilityReceipt& receipt,
                                     const ProgramRunRecord&         target,
                                     const ProgramRunRecord&         source) noexcept {
    if (receipt.storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
        return true;
    }
    try {
        if (const auto source_pending = source.pending_input()) {
            const auto target_pending = target.pending_input();
            const auto result = target_pending ? target_pending->consumed_result() : std::nullopt;
            if (!target_pending || !result || target.pending_effect() ||
                !receipt.matches_initial_resume(target_pending->call_id(), *result)) {
                return false;
            }
            const auto applied =
                source_pending->submit(target_pending->call_id(), *result,
                                       static_cast<std::uint64_t>(target.created_at_ms()));
            return applied.disposition == ProgramPendingDisposition::Applied &&
                   applied.value == *target_pending;
        }
        if (const auto source_pending = source.pending_effect()) {
            const auto target_pending = target.pending_effect();
            const auto result = target_pending ? target_pending->reconciled_result() : std::nullopt;
            if (!target_pending || !result || target.pending_input() ||
                !receipt.matches_initial_resume(target_pending->call_id(), *result)) {
                return false;
            }
            const auto applied =
                source_pending->submit(target_pending->call_id(), target_pending->effect_id(),
                                       *result, static_cast<std::uint64_t>(target.created_at_ms()));
            return applied.disposition == ProgramPendingDisposition::Applied &&
                   applied.value == *target_pending;
        }
        return !target.pending_input() && !target.pending_effect() &&
               receipt.initial_resume_binding() &&
               !receipt.initial_resume_binding()->target_pending_id;
    } catch (const std::exception&) {
        return false;
    }
}

bool fork_binds_predecessor(const ProgramRunRecord&     target,
                            const ProgramRunGeneration& predecessor,
                            const ProgramRunLineage&    lineage,
                            const ProgramRunRecord&     source) noexcept {
    const auto receipt = target.fork_receipt();
    if (!receipt) return false;
    const auto checkpoint = source.exact_checkpoint();
    return checkpoint && target.run_id() != source.run_id() && source.child_depth() == 0 &&
           target.created_at_ms() >= source.updated_at_ms() &&
           source.invocation().parent_run_id.empty() &&
           source.continuation().state == ContinuationState::Interrupted &&
           lineage.committed_descendant_budget() == RunBudget{} && receipt->compatible() &&
           receipt->owner_scope() == target.owner_scope() &&
           receipt->source_run_id() == predecessor.run_id() &&
           receipt->source_program_version_id() == predecessor.program_version_id() &&
           receipt->source_checkpoint_id() == checkpoint->checkpoint_id &&
           receipt->target_program_version_id() == target.program_version_id() &&
           fork_resume_binds_source_target(*receipt, target, source) &&
           source.id() == lineage.active_run_record_id() &&
           source.journal_head() == lineage.active_journal_head();
}

bool fork_allocation_fits(const ProgramRunLineage& previous,
                          const ProgramRunLineage& debited,
                          const RunBudget&         target) noexcept {
    const auto fits = [](auto before, auto after, auto allocated) {
        return after <= before && allocated == before - after;
    };
    return debited.active_generation() == previous.active_generation() &&
           debited.active_generation_id() == previous.active_generation_id() &&
           debited.active_run_record_id() == previous.active_run_record_id() &&
           debited.active_journal_head() == previous.active_journal_head() &&
           debited.inflight_reservation() == previous.inflight_reservation() &&
           debited.committed_descendant_budget() == previous.committed_descendant_budget() &&
           fits(previous.remaining_budget().wall_time_ms,
                debited.remaining_budget().wall_time_ms, target.wall_time_ms) &&
           fits(previous.remaining_budget().model_tokens,
                debited.remaining_budget().model_tokens, target.model_tokens) &&
           fits(previous.remaining_budget().monetary_microunits,
                debited.remaining_budget().monetary_microunits, target.monetary_microunits) &&
           fits(previous.remaining_budget().max_concurrency,
                debited.remaining_budget().max_concurrency, target.max_concurrency) &&
           fits(previous.remaining_budget().max_program_operations,
                debited.remaining_budget().max_program_operations,
                target.max_program_operations) &&
           fits(previous.remaining_budget().max_core_steps,
                debited.remaining_budget().max_core_steps, target.max_core_steps) &&
           fits(previous.remaining_budget().max_dynamic_compiles,
                debited.remaining_budget().max_dynamic_compiles,
                target.max_dynamic_compiles) &&
           fits(previous.remaining_budget().max_child_depth,
                debited.remaining_budget().max_child_depth, target.max_child_depth) &&
           fits(previous.remaining_budget().max_total_children,
                debited.remaining_budget().max_total_children, target.max_total_children);
}

std::optional<ProgramUsage> command_terminal_usage(
    const ProgramJavaScriptCommandJournalEntry& entry) {
    const auto terminal_result = entry.terminal_result();
    if (!entry.completed() || !terminal_result || !terminal_result->contains("usage") ||
        !terminal_result->at("usage").is_object()) {
        return std::nullopt;
    }
    const auto& usage = terminal_result->at("usage");
    const auto  read  = [&](std::string_view key) -> std::optional<std::uint64_t> {
        const std::string owned(key);
        if (!usage.contains(owned) || !usage.at(owned).is_number_unsigned()) return std::nullopt;
        return usage.at(owned).get<std::uint64_t>();
    };
    const auto wall       = read("wall_time_ms");
    const auto model      = read("model_tokens");
    const auto money      = read("monetary_microunits");
    const auto operations = read("program_operations");
    const auto steps      = read("core_steps");
    const auto peak       = read("peak_concurrency");
    if (!wall || !model || !money || !operations || !steps || !peak ||
        *peak > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    // Program operations are charged before the resource reservation is staged.
    return ProgramUsage{*wall, *model, *money, 0, *steps, static_cast<std::uint32_t>(*peak)};
}

bool checkpoint_event_matches(const std::vector<ProgramEvent>&             events,
                              const std::optional<CoreCheckpointIdentity>& checkpoint) {
    if (!checkpoint) return false;
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint == *checkpoint;
    });
}

bool javascript_call_core_checkpoint_matches(const ProgramRunRecord&                     run,
                                             const ProgramJavaScriptCommandJournalEntry& pending,
                                             const CoreCheckpointIdentity& checkpoint) {
    const auto thread_for = [&](std::string_view operation_id) {
        std::string identity(run.run_id());
        identity.push_back('\0');
        identity.append(operation_id);
        identity.push_back('\0');
        identity.append(checkpoint.core_generation_id);
        return detail::sha256_identity("program-core-thread/v1", identity);
    };
    std::function<bool(const JavaScriptCommand&, std::string, std::size_t)> matches;
    matches = [&](const JavaScriptCommand& command, std::string operation_id, std::size_t depth) {
        if (depth > 32) return false;
        if (command.kind() == JavaScriptCommandKind::CallCore) {
            const auto arguments = command.arguments();
            return arguments.contains("name") && arguments.at("name").is_string() &&
                   arguments.at("name").get<std::string>() == checkpoint.core_name &&
                   thread_for(operation_id) == checkpoint.core_thread_id;
        }
        const auto arguments = command.arguments();
        if (command.kind() == JavaScriptCommandKind::Await) {
            return matches(JavaScriptCommand::from_json(arguments.at("command")),
                           operation_id + "/await", depth + 1);
        }
        if (command.kind() == JavaScriptCommandKind::Join) {
            const auto& members = arguments.at("members");
            for (std::size_t index = 0; index < members.size(); ++index)
                if (matches(JavaScriptCommand::from_json(members.at(index)),
                            operation_id + "/member/" + std::to_string(index), depth + 1))
                    return true;
        }
        return false;
    };
    return matches(pending.command(),
                   "root.javascript." + std::to_string(pending.command_ordinal()), 0);
}

bool valid_command_reservation_transition(
    const ProgramJournalRecord&                              previous_journal,
    const ProgramJournalRecord&                              next_journal,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& old_commands,
    const ProgramTransitionPublication&                      publication) {
    const bool increased =
        budget_increased(next_journal.remaining_budget, previous_journal.remaining_budget);
    const bool ordinary  = is_valid_program_journal_transition(previous_journal, next_journal);
    const auto completed = std::find_if(publication.commands.begin(), publication.commands.end(),
                                        [](const auto& entry) { return entry.completed(); });
    const bool checkpoint_changed =
        previous_journal.core_checkpoint != next_journal.core_checkpoint;
    if (completed != publication.commands.end()) {
        if (publication.commands.size() != 1 || !budget_is_empty(next_journal.inflight_reservation))
            return false;
        if (checkpoint_changed &&
            (!checkpoint_event_matches(publication.events, next_journal.core_checkpoint) ||
             !next_journal.core_checkpoint ||
             !javascript_call_core_checkpoint_matches(publication.run_record, *completed,
                                                      *next_journal.core_checkpoint))) {
            return false;
        }
        if (budget_is_empty(previous_journal.inflight_reservation)) return ordinary && !increased;
        const auto usage = command_terminal_usage(*completed);
        return usage && is_valid_program_journal_reservation_settlement(previous_journal,
                                                                        next_journal, *usage);
    }
    if (!increased) return ordinary;

    // A safely interrupted CallCore keeps its command coordinate Pending but
    // may release verified unused capacity only when the exact newer
    // checkpoint and measured terminal usage are committed in this CAS.
    if (!publication.commands.empty() || old_commands.empty() || !old_commands.back().pending() ||
        publication.run_record.continuation().state != ContinuationState::Interrupted ||
        !publication.run_record.terminal_result() ||
        publication.run_record.terminal_result()->status() != ProgramTerminalStatus::Interrupted ||
        !checkpoint_event_matches(publication.events, publication.run_record.exact_checkpoint()) ||
        !publication.run_record.exact_checkpoint() ||
        !javascript_call_core_checkpoint_matches(publication.run_record, old_commands.back(),
                                                 *publication.run_record.exact_checkpoint()) ||
        (previous_journal.core_checkpoint &&
         previous_journal.core_checkpoint == next_journal.core_checkpoint)) {
        return false;
    }
    auto usage               = publication.run_record.terminal_result()->usage();
    usage.program_operations = 0;
    return is_valid_program_journal_reservation_settlement(previous_journal, next_journal, usage);
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

    append_publication_bytes(bytes, "{\"commands\":[");
    for (std::size_t index = 0; index < publication.commands.size(); ++index) {
        if (index != 0) append_publication_bytes(bytes, ",");
        append_publication_bytes(bytes, publication.commands[index].serialize_canonical());
    }

    append_publication_bytes(bytes, "],\"effects\":[");
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
    append_publication_bytes(bytes,
                             ",\"format\":\"neograph-program-transition-publication\","
                             "\"journal_record\":");
    append_publication_bytes(bytes, publication.journal_record.serialize_canonical());
    append_publication_bytes(bytes, ",\"migration_plan\":");
    if (publication.migration_plan) {
        append_publication_bytes(bytes, publication.migration_plan->serialize_canonical());
    } else {
        append_publication_bytes(bytes, "null");
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
    append_publication_bytes(bytes, publication.fork_source_lineage
                                        ? ",\"storage_schema_version\":3}"
                                    : publication.run_lineage
                                        ? ",\"storage_schema_version\":2}"
                                        : ",\"storage_schema_version\":1}");
    return bytes;
}
}  // namespace
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
    if (schema_version >= 2) {
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
    }
    ProgramTransitionPublication out{std::move(run), std::move(journal), std::move(events),
                                       std::move(effects), std::move(migration_plan),
                                       std::move(commands), std::move(run_generation),
                                       std::move(run_lineage), std::move(fork_source_lineage)};
    validate_pub(out, out.run_record.owner_scope());
    return out;
}
std::vector<ProgramJavaScriptCommandJournalEntry> ProgramTransitionStore::load_javascript_commands(
    std::string_view, std::string_view, std::uint64_t) const {
    throw std::runtime_error(
        "ProgramTransitionStore does not support durable JavaScript command-history reads");
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
    std::optional<ProgramTransitionFaultPoint>                        fault;
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
ProgramTransitionPublishResult InMemoryProgramTransitionStore::compare_publish(
    std::string_view owner, std::string_view expected, ProgramTransitionPublication publication) {
    std::string publication_bytes;
    try {
        if (publication.run_record.owner_scope() != owner) {
            return ProgramTransitionPublishResult::Conflict;
        }
        publication_bytes = publication.serialize_canonical();
        if (!valid_effect_outbox_binding(publication.run_record, publication.effects)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        if (!expected.empty() && !detail::is_sha256_identity(expected)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    } catch (const std::invalid_argument&) {
        return ProgramTransitionPublishResult::Conflict;
    }

    auto            storage_key = key(owner, publication.run_record.run_id());
    std::lock_guard lock(impl_->mutex);
    auto            current = impl_->runs.find(storage_key);
    if (current != impl_->runs.end() && current->second->bytes == publication_bytes) {
        return expected == publication.journal_record.previous_id
                   ? ProgramTransitionPublishResult::AlreadyPresent
                   : ProgramTransitionPublishResult::Conflict;
    }
    if (current == impl_->runs.end() && publication.fork_source_lineage) {
        const auto fork = publication.run_record.fork_receipt();
        if (!fork ||
            fork->storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    if (current != impl_->runs.end() && publication.migration_plan) {
        const auto& old_plan = current->second->migration_plan;
        if (!old_plan || old_plan->id() != publication.migration_plan->id()) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    const auto lineage_key = publication.run_lineage
                                 ? key(owner, publication.run_lineage->lineage_id())
                                 : std::string{};
    auto current_lineage = publication.run_lineage ? impl_->lineages.find(lineage_key)
                                                    : impl_->lineages.end();
    const bool adopting_legacy_lineage =
        current != impl_->runs.end() && !current->second->lineage_id && publication.run_lineage;
    if (current != impl_->runs.end() && current->second->lineage_id) {
        if (!publication.run_lineage ||
            *current->second->lineage_id != publication.run_lineage->lineage_id()) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }
    if (publication.run_lineage) {
        if (current_lineage == impl_->lineages.end()) {
            if (!publication.run_generation ||
                !is_valid_program_run_lineage_initial(*publication.run_lineage,
                                                      *publication.run_generation)) {
                return ProgramTransitionPublishResult::Conflict;
            }
        } else if (adopting_legacy_lineage) {
            return ProgramTransitionPublishResult::Conflict;
        } else if (!is_valid_program_run_lineage_transition(
                       current_lineage->second->head, *publication.run_lineage,
                       publication.run_generation)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        if (current_lineage != impl_->lineages.end() && !publication.run_generation) {
            const auto active = current_lineage->second->generations.find(
                publication.run_lineage->active_generation());
            if (active == current_lineage->second->generations.end() ||
                !does_program_run_generation_bind(active->second, *publication.run_lineage,
                                                  publication.run_record)) {
                return ProgramTransitionPublishResult::Conflict;
            }
        }
        if (current_lineage != impl_->lineages.end() && publication.run_generation) {
            const auto active = current_lineage->second->generations.find(
                current_lineage->second->head.active_generation());
            if (active == current_lineage->second->generations.end() ||
                active->second.child_depth() != publication.run_generation->child_depth()) {
                return ProgramTransitionPublishResult::Conflict;
            }
            const auto source = impl_->runs.find(key(owner, active->second.run_id()));
            if (source == impl_->runs.end()) {
                return ProgramTransitionPublishResult::Conflict;
            }
            const auto graph_migration =
                publication.run_generation->graph_migration_receipt();
            const bool valid_successor = graph_migration
                ? source->second->commands.empty() && publication.commands.empty() &&
                      publication.effects.empty() && publication.events.size() == 1 &&
                      does_program_graph_migration_started_event_bind(
                          publication.events.front(), publication.run_record) &&
                      publication.run_record.event_sequence() == 1 &&
                      publication.migration_plan &&
                      is_valid_program_graph_migration_transition(
                          active->second, current_lineage->second->head,
                          source->second->run, *publication.migration_plan,
                          *publication.run_generation, *publication.run_lineage,
                          publication.run_record)
                : !source->second->commands.empty() &&
                      is_valid_program_replacement_transition(
                          active->second, current_lineage->second->head,
                          source->second->run, source->second->commands.back(),
                          *publication.run_generation, *publication.run_lineage,
                          publication.run_record);
            if (!valid_successor) return ProgramTransitionPublishResult::Conflict;
        }
        if (publication.run_generation && current_lineage != impl_->lineages.end() &&
            current_lineage->second->generations.contains(
                publication.run_generation->generation())) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    std::string                                fork_source_key;
    std::shared_ptr<const Impl::StoredLineage> fork_source_current;
    if (publication.fork_source_lineage) {
        fork_source_key = key(owner, publication.fork_source_lineage->lineage_id());
        const auto found = impl_->lineages.find(fork_source_key);
        if (found == impl_->lineages.end() ||
            !is_valid_program_run_lineage_transition(
                found->second->head, *publication.fork_source_lineage) ||
            !fork_allocation_fits(found->second->head, *publication.fork_source_lineage,
                                  publication.journal_record.remaining_budget)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        const auto active =
            found->second->generations.find(found->second->head.active_generation());
        if (active == found->second->generations.end())
            return ProgramTransitionPublishResult::Conflict;
        const auto source = impl_->runs.find(key(owner, active->second.run_id()));
        if (source == impl_->runs.end() ||
            !fork_binds_predecessor(publication.run_record, active->second,
                                    found->second->head, source->second->run)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        fork_source_current = found->second;
    } else if (current == impl_->runs.end() && publication.run_record.fork_receipt()) {
        return ProgramTransitionPublishResult::Conflict;
    }

    const auto new_terminal = publication.run_record.terminal_result();
    const bool publishes_terminal_event =
        !publication.events.empty() && publication.events.back().kind == ProgramEventKind::Terminal;

    if (current == impl_->runs.end()) {
        if (!expected.empty() || !publication.journal_record.previous_id.empty() ||
            publication.journal_record.sequence != 1 ||
            !budget_is_empty(publication.journal_record.inflight_reservation) ||
            publication.events.empty() ||
            publication.events.front().kind != ProgramEventKind::Started ||
            publication.run_record.event_sequence() != publication.events.size() ||
            publication.run_record.effect_sequence() != publication.effects.size() ||
            publication.events.front().sequence != 1 ||
            (!publication.effects.empty() && publication.effects.front().sequence() != 1) ||
            (new_terminal && !publishes_terminal_event) ||
            (publication.run_record.fork_receipt() && !publication.migration_plan)) {
            return ProgramTransitionPublishResult::Conflict;
        }
    } else {
        const auto& old          = *current->second;
        const auto  old_terminal = old.run.terminal_result();
        const auto  old_fork     = old.run.fork_receipt();
        const auto  new_fork     = publication.run_record.fork_receipt();
        const bool  same_fork    = old_fork.has_value() == new_fork.has_value() &&
                               (!old_fork || old_fork->id() == new_fork->id());
        if ((old_terminal && is_final(old.run.continuation().state) && !new_terminal) ||
            (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
             !publishes_terminal_event)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        // A publication may update durable run metadata (such as a child
        // relationship) without advancing the event or effect streams.
        if (old.journal.id != expected || publication.journal_record.previous_id != expected ||
            publication.run_record.created_at_ms() != old.run.created_at_ms() ||
            publication.run_record.updated_at_ms() < old.run.updated_at_ms() ||
            publication.run_record.binding_fingerprint() != old.run.binding_fingerprint() ||
            !same_fork ||
            publication.run_record.recorded_binding_set_fingerprint() !=
                old.run.recorded_binding_set_fingerprint() ||
            publication.run_record.invocation() != old.run.invocation() ||
            (publication.run_record.exact_checkpoint() == old.run.exact_checkpoint() &&
             publication.run_record.exact_checkpoint_content_id() !=
                 old.run.exact_checkpoint_content_id()) ||
            !valid_children_transition(old.run, publication.run_record) ||
            publication.run_record.event_sequence() !=
                old.run.event_sequence() + publication.events.size() ||
            publication.run_record.effect_sequence() !=
                old.run.effect_sequence() + publication.effects.size() ||
            (!publication.events.empty() &&
             publication.events.front().sequence != old.run.event_sequence() + 1) ||
            (!publication.effects.empty() &&
             publication.effects.front().sequence() != old.run.effect_sequence() + 1) ||
            !effect_ids_are_unique(old.effects, old.history, publication.effects)) {
            return ProgramTransitionPublishResult::Conflict;
        }
        const bool command_history_valid =
            valid_command_history_append(old.run, old.commands, publication.commands);
        const bool reservation_valid = valid_command_reservation_transition(
            old.journal, publication.journal_record, old.commands, publication);
        if (!command_history_valid || !reservation_valid) {
            return ProgramTransitionPublishResult::Conflict;
        }
    }

    if (current == impl_->runs.end() &&
        !valid_command_history_append(publication.run_record, {}, publication.commands)) {
        return ProgramTransitionPublishResult::Conflict;
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
    maybe_fail(ProgramTransitionFaultPoint::AfterLineageSnapshot);

    std::vector<ProgramEvent>            events;
    std::vector<ProgramEffectOutboxEntry> effects;
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    ProgramTransitionHistoryPtr           history;
    if (current == impl_->runs.end()) {
        events  = std::move(appended_events);
        effects = std::move(appended_effects);
        commands = std::move(appended_commands);
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
    }

    auto migration_plan = current == impl_->runs.end()
                              ? std::move(publication.migration_plan)
                              : current->second->migration_plan;
    auto candidate = std::make_shared<const Impl::Stored>(
        Impl::Stored{std::move(staged_run), std::move(staged_journal), std::move(events),
                      std::move(effects), std::move(commands), std::move(history),
                      std::move(migration_plan),
                      publication.run_lineage
                          ? std::optional<std::string>(publication.run_lineage->lineage_id())
                          : std::nullopt,
                      std::move(publication_bytes)});
    maybe_fail(ProgramTransitionFaultPoint::BeforeCommit);
    if (staged_lineage || staged_fork_source) {
        auto staged_runs     = impl_->runs;
        auto staged_lineages = impl_->lineages;
        staged_runs.insert_or_assign(storage_key, std::move(candidate));
        if (staged_lineage)
            staged_lineages.insert_or_assign(lineage_key, std::move(staged_lineage));
        if (staged_fork_source)
            staged_lineages.insert_or_assign(fork_source_key, std::move(staged_fork_source));
        impl_->runs.swap(staged_runs);
        impl_->lineages.swap(staged_lineages);
    } else if (current == impl_->runs.end()) {
        impl_->runs.emplace(std::move(storage_key), std::move(candidate));
    } else {
        current->second = std::move(candidate);
    }
    return ProgramTransitionPublishResult::Published;
}
}  // namespace neograph::program
