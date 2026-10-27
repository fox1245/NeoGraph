/**
 * @file transition_validator.h
 * @brief Internal. The Program transition state machine, written once for every
 *        ProgramTransitionStore backend.
 *
 * A publication is accepted when every check below passes against the state the
 * backend has stored. The checks are pure: they read the stored state through
 * TransitionReads and decide, and they never write. Each backend keeps its own
 * locking, transaction and writes:
 *
 *   1. prepare_transition()   checks that need no stored state, and serializes;
 *   2. the backend takes its lock or opens its transaction and loads the run
 *      head and the execution lease, because it needs both again to write;
 *   3. validate_transition()  decides Proceed, AlreadyPresent or Conflict;
 *   4. on Proceed the backend writes the publication in the same transaction.
 *
 * TransitionReads is the read set of the state machine. Every read a check makes
 * is a method of it, named by what it is for, so a read that a database must lock
 * (Lock::Update) is a different call from one that need not be. Header only: the
 * SQLite and PostgreSQL stores are separate libraries from neograph_program.
 */
#pragma once

#include <neograph/program/child_synthesis.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/journal.h>
#include <neograph/program/lineage.h>
#include <neograph/program/replacement.h>
#include <neograph/program/transition_store.h>

#include "canonical_json.h"
#include "node_retry_budget.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace neograph::program::detail::transition {

// ---------------------------------------------------------------------------
// Predicates over values. None of them reads stored state.
// ---------------------------------------------------------------------------

inline bool is_final(ContinuationState state) noexcept {
    return state != ContinuationState::Running && state != ContinuationState::Interrupted &&
           state != ContinuationState::AmbiguousEffect;
}

inline bool same_child_metadata(const ProgramChildRecord& lhs, const ProgramChildRecord& rhs) {
    return lhs.child_run_id == rhs.child_run_id && lhs.link_id == rhs.link_id &&
           lhs.link_receipt == rhs.link_receipt && lhs.invocation == rhs.invocation;
}

inline bool valid_child_state_transition(ProgramChildState previous, ProgramChildState next) noexcept {
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

inline bool valid_children_transition(const ProgramRunRecord& previous,
                                      const ProgramRunRecord& next) {
    const auto& previous_children = previous.children();
    const auto& next_children     = next.children();
    std::unordered_map<std::string_view, const ProgramChildRecord*> next_by_id;
    next_by_id.reserve(next_children.size());
    for (const auto& child : next_children) {
        next_by_id.emplace(child.child_run_id, &child);
    }
    for (const auto& child : previous_children) {
        const auto found = next_by_id.find(child.child_run_id);
        if (found == next_by_id.end() || !same_child_metadata(child, *found->second) ||
            !valid_child_state_transition(child.state, found->second->state))
            return false;
    }
    return true;
}

inline bool same_fork(const ProgramRunRecord& old_run, const ProgramRunRecord& new_run) {
    const auto old_receipt = old_run.fork_receipt();
    const auto new_receipt = new_run.fork_receipt();
    return old_receipt.has_value() == new_receipt.has_value() &&
           (!old_receipt || old_receipt->id() == new_receipt->id());
}

inline bool has_blocking_hook_obligation(const std::vector<HookOutboxEntry>& entries) noexcept {
    return std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        const auto state = entry.data().state;
        const bool pending = state != HookExecutionState::Succeeded &&
            state != HookExecutionState::Cancelled;
        return pending && entry.data().invocation.data().delivery == HookDelivery::BlockingMandatory;
    });
}

inline bool valid_effect_outbox_binding(const ProgramRunRecord& run,
                                        const std::vector<ProgramEffectOutboxEntry>& effects) {
    if (effects.empty()) return true;
    const auto pending = run.pending_effect();
    return pending && (pending->state() == ProgramPendingState::Awaiting ||
                       (pending->state() == ProgramPendingState::Ambiguous &&
                        run.continuation().state == ContinuationState::AmbiguousEffect)) &&
           effects.size() == 1 &&
           effects.front().effect() == *pending;
}

inline bool same_command_coordinate(const ProgramJavaScriptCommandJournalEntry& lhs,
                                    const ProgramJavaScriptCommandJournalEntry& rhs) {
    return lhs.bundle_id() == rhs.bundle_id() &&
           lhs.command_ordinal() == rhs.command_ordinal() &&
           lhs.coordinate_id() == rhs.coordinate_id() &&
           detail::canonical_json_bytes(lhs.command().to_json()) ==
               detail::canonical_json_bytes(rhs.command().to_json()) &&
           lhs.effect_identity() == rhs.effect_identity();
}

inline bool valid_command_history_append(
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
                entry.settled_members() || entry.await_started_at() ||
                (!prior.empty() && prior.back().pending()))
                return false;
            highest_ordinal = entry.command_ordinal();
            prior.push_back(entry);
            continue;
        }

        // A command coordinate can only move once, from a durable pending
        // head to one authoritative terminal result. Before that, a paused
        // join may record its settled members, and an await its start (#391),
        // as further pending entries of the same coordinate. Conflicting
        // command payloads or a second
        // completion are rejected fail-closed.
        if (!found->pending() || !same_command_coordinate(*found, entry) ||
            (!entry.completed() && !entry.settled_members() && !entry.await_started_at())) {
            return false;
        }
        prior.push_back(entry);
    }
    return true;
}

// Command-log append invariants and tail validation:
// - Store write paths: InMemoryProgramTransitionStore::compare_publish_impl
//   (src/program/transition_store.cpp) writes Stored::commands;
//   SQLiteProgramTransitionStore::compare_publish_impl and migrate_legacy_schema
//   (src/program/sqlite_transition_store.cpp) call append_javascript_commands (the sole
//   INSERT INTO program_transition_javascript_command_log_v2; normalize_legacy_snapshot
//   validates publication.commands with valid_command_history_append before
//   migrate_legacy_schema appends them); and
//   PostgreSQLProgramTransitionStore::compare_publish_impl
//   (src/program/postgres_transition_store.cpp) calls append_javascript_commands (the sole
//   INSERT INTO neograph_program_transition_javascript_command_log_v2). WebAssembly
//   JsTransitionStore::compare_publish and compare_publish_execution (wasm/js_stores.cpp)
//   run prepare_transition and forward to the host store (wasm/stores.mjs and
//   wasm/memory_stores.cpp delegate to InMemoryProgramTransitionStore), and
//   HostPumpedTransitionStore (tests/fixtures/host_pumped_stores.h) delegates to its inner
//   ProgramTransitionStore. Every store write is gated by validate_transition (or
//   normalize_legacy_snapshot's valid_command_history_append check), which runs
//   valid_initial_publication (calling valid_command_history_append) when !current and
//   valid_increment when current.
// - Callers building ProgramTransitionPublication::commands:
//   RunControl::publish_javascript_command (src/program/runtime.cpp, called from
//   execute_run_attempt in src/program/run_attempt.cpp), ProgramRuntime::resume_core, and
//   ProgramRuntime::reconcile_core (src/program/runtime.cpp) append one entry through
//   compare_publish / compare_publish_execution (valid_increment). ProgramRuntime::fork_core
//   (src/program/runtime.cpp) copies the source log rebound to target_bundle_id onto a new
//   run_id (expected_journal_head empty, checked by valid_initial_publication and full-log
//   valid_command_history_append). ProgramRuntime::replace_core, migrate_graph_core,
//   RunControl::hold_graph_migration, and ProgramRuntime::reconnect_core
//   (src/program/runtime.cpp) leave publication.commands empty.
// - Bundle immutability for a run_id: prepare_transition (via
//   ProgramTransitionPublication::serialize_canonical in src/program/transition_store.cpp)
//   requires run_record.bundle_id() == journal_record.bundle_id, and every journal step in
//   valid_command_reservation_transition (is_valid_program_journal_transition,
//   is_valid_program_journal_reservation_settlement, and
//   is_valid_program_journal_reservation_release in src/program/journal.cpp, and
//   is_valid_program_graph_safe_point_transition in src/program/graph_migration.cpp) requires
//   next_journal.bundle_id == previous_journal.bundle_id. Replacement
//   (is_valid_program_replacement_transition in src/program/replacement.cpp), graph migration
//   (is_valid_program_graph_migration_transition in src/program/graph_migration.cpp), and
//   fork (fork_binds_predecessor in src/program/transition_validator.h) require
//   target.run_id() != source.run_id() and are admitted only on initial publication, while
//   legacy lineage adoption (lineage_binds in src/program/transition_validator.h) is an
//   increment on the same run_id and keeps bundle_id() unchanged.
// - Because every stored entry passed this check when appended, ordinals in sequence order
//   never decrease and no coordinate older than the last entry's ordinal is pending. Moreover,
//   each of the (command_ordinal - 1) completed earlier ordinals occupies at least 2 entries
//   (initial pending plus completion) and any non-initial entry for the current ordinal
//   (completed, settled_members, or await_started_at) has at least 1 prior entry of the same
//   ordinal, so sequence - command_ordinal >= command_ordinal - (requires_prior_same_ordinal ? 0 : 1).
//   The publish path now parses only the last stored entry (via TransitionReads::latest_command);
//   a stored log that is corrupt in earlier entries is still detected when the whole log is
//   read (load_javascript_commands, recovery/replay via load_javascript_commands, and
//   TransitionReads::command_log).
inline bool valid_command_history_append_after(
    const ProgramRunRecord&                                     run,
    const std::optional<ProgramJavaScriptCommandJournalEntry>& last_command,
    const std::vector<ProgramJavaScriptCommandJournalEntry>&    new_commands) {
    if (new_commands.empty()) return true;
    if (last_command) {
        const bool requires_prior_same_ordinal =
            !last_command->pending() || last_command->settled_members() ||
            last_command->await_started_at();
        if (last_command->bundle_id() != run.bundle_id() ||
            last_command->command_ordinal() > last_command->sequence() ||
            last_command->sequence() - last_command->command_ordinal() <
                last_command->command_ordinal() - (requires_prior_same_ordinal ? 0U : 1U) ||
            (last_command->command_ordinal() == 1 && !requires_prior_same_ordinal &&
             last_command->sequence() != 1)) {
            return false;
        }
    }

    auto          current_last      = last_command;
    std::uint64_t expected_sequence = current_last ? current_last->sequence() : 0;
    std::uint64_t highest_ordinal   = current_last ? current_last->command_ordinal() : 0;
    for (const auto& entry : new_commands) {
        if (entry.bundle_id() != run.bundle_id() ||
            expected_sequence == std::numeric_limits<std::uint64_t>::max() ||
            entry.sequence() != expected_sequence + 1) {
            return false;
        }
        ++expected_sequence;

        if (!current_last || entry.command_ordinal() != current_last->command_ordinal()) {
            if (entry.command_ordinal() != highest_ordinal + 1 || !entry.pending() ||
                entry.settled_members() || entry.await_started_at() ||
                (current_last && current_last->pending())) {
                return false;
            }
            highest_ordinal = entry.command_ordinal();
            current_last    = entry;
            continue;
        }

        if (!current_last->pending() || !same_command_coordinate(*current_last, entry) ||
            (!entry.completed() && !entry.settled_members() && !entry.await_started_at())) {
            return false;
        }
        current_last = entry;
    }
    return true;
}

inline bool budget_is_empty(const RunBudget& budget) noexcept {
    return budget == RunBudget{};
}

inline bool budget_increased(const RunBudget& next, const RunBudget& previous) noexcept {
    return next.wall_time_ms > previous.wall_time_ms || next.model_tokens > previous.model_tokens ||
           next.monetary_microunits > previous.monetary_microunits ||
           next.max_concurrency > previous.max_concurrency ||
           next.max_program_operations > previous.max_program_operations ||
           next.max_core_steps > previous.max_core_steps ||
           next.max_dynamic_compiles > previous.max_dynamic_compiles ||
           next.max_child_depth > previous.max_child_depth ||
           next.max_total_children > previous.max_total_children ||
           !detail::node_retries_within(next.max_node_retries, previous.max_node_retries);
}

inline bool fork_resume_binds_source_target(const ForkCompatibilityReceipt& receipt,
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

inline bool fork_binds_predecessor(const ProgramRunRecord&     target,
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

// `concurrency_is_a_limit` is a fork's allocation (#414): its max_concurrency
// is its own limit, up to the source's, and the source keeps its own. A
// recorded replay hands the whole remainder over, concurrency included.
inline bool fork_allocation_fits(const ProgramRunLineage& previous,
                                 const ProgramRunLineage& debited,
                                 const RunBudget&         target,
                                 bool                     concurrency_is_a_limit = false) noexcept {
    const auto fits = [](auto before, auto after, auto allocated) {
        return after <= before && allocated == before - after;
    };
    const auto concurrency_fits = [&] {
        const auto before = previous.remaining_budget().max_concurrency;
        const auto after  = debited.remaining_budget().max_concurrency;
        return concurrency_is_a_limit ? after == before && target.max_concurrency <= before
                                      : fits(before, after, target.max_concurrency);
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
           concurrency_fits() &&
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
                debited.remaining_budget().max_total_children, target.max_total_children) &&
           // A child's retry ceiling is its own; the parent's is never debited.
           debited.remaining_budget().max_node_retries ==
               previous.remaining_budget().max_node_retries;
}

inline std::optional<ProgramUsage> command_terminal_usage(
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

inline bool checkpoint_event_matches(const std::vector<ProgramEvent>&             events,
                                     const std::optional<CoreCheckpointIdentity>& checkpoint) {
    if (!checkpoint) return false;
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.kind == ProgramEventKind::CheckpointPublished &&
               std::get<ProgramCheckpointEvent>(event.payload).checkpoint == *checkpoint;
    });
}

/// Whether `command` (the command at `ordinal`) is a Core call, or holds one, whose Core thread is the one of
/// `checkpoint`.
inline bool command_holds_core_call_at(std::string_view run_id, const JavaScriptCommand& command,
                                       std::uint64_t ordinal, const CoreCheckpointIdentity& checkpoint) {
    const auto thread_for = [&](std::string_view operation_id) {
        std::string identity(run_id);
        identity.push_back('\0');
        identity.append(operation_id);
        identity.push_back('\0');
        identity.append(checkpoint.core_generation_id);
        return detail::sha256_identity("program-core-thread/v1", identity);
    };
    std::function<bool(const JavaScriptCommand&, std::string, std::size_t)> matches;
    matches = [&](const JavaScriptCommand& node, std::string operation_id, std::size_t depth) {
        if (depth > 32) return false;
        if (node.kind() == JavaScriptCommandKind::CallCore) {
            const auto arguments = node.arguments();
            return arguments.contains("name") && arguments.at("name").is_string() &&
                   arguments.at("name").get<std::string>() == checkpoint.core_name &&
                   thread_for(operation_id) == checkpoint.core_thread_id;
        }
        const auto arguments = node.arguments();
        if (node.kind() == JavaScriptCommandKind::Await) {
            return matches(JavaScriptCommand::from_json(arguments.at("command")),
                           operation_id + "/await", depth + 1);
        }
        if (node.kind() == JavaScriptCommandKind::Join) {
            const auto& members = arguments.at("members");
            for (std::size_t index = 0; index < members.size(); ++index)
                if (matches(JavaScriptCommand::from_json(members.at(index)),
                            operation_id + "/member/" + std::to_string(index), depth + 1))
                    return true;
        }
        return false;
    };
    return matches(command, "root.javascript." + std::to_string(ordinal), 0);
}

inline bool javascript_call_core_checkpoint_matches(std::string_view                            run_id,
                                                    const ProgramJavaScriptCommandJournalEntry& pending,
                                                    const CoreCheckpointIdentity& checkpoint) {
    return command_holds_core_call_at(run_id, pending.command(), pending.command_ordinal(), checkpoint);
}

inline bool javascript_call_core_checkpoint_matches(const ProgramRunRecord&                     run,
                                                    const ProgramJavaScriptCommandJournalEntry& pending,
                                                    const CoreCheckpointIdentity& checkpoint) {
    return javascript_call_core_checkpoint_matches(run.run_id(), pending, checkpoint);
}

/**
 * The one rule for whether an Interrupted outcome releases the resources that the run's pending
 * JavaScript command reserved (#658). The attempt, the terminal publisher and the transition
 * validator all ask it, each with the same facts about the same checkpoint value, the one that the
 * Interrupted record carries as `exact_checkpoint`:
 *   - `checkpoint` is that checkpoint, `previous_checkpoint` the one of the journal head it follows;
 *   - `checkpoint_event_published`: a CheckpointPublished event for `checkpoint` is part of the outcome;
 *   - `command_holds_core_call_here`: whether the pending command is a Core call (or holds one) whose
 *     Core thread is the checkpoint's; empty when no JavaScript command is pending.
 * The run released its reservation only if it stopped in a Core call at a checkpoint that this outcome
 * wrote. A command that waits on a child's run (an await of a spawn) writes none, so it keeps it.
 */
inline bool interrupt_releases_command_reservation(
    const std::optional<CoreCheckpointIdentity>& previous_checkpoint,
    const std::optional<CoreCheckpointIdentity>& checkpoint,
    bool                                         checkpoint_event_published,
    std::optional<bool>                          command_holds_core_call_here) {
    if (!checkpoint || !checkpoint_event_published) return false;
    if (previous_checkpoint && *previous_checkpoint == *checkpoint) return false;
    return !command_holds_core_call_here || *command_holds_core_call_here;
}

inline bool valid_command_reservation_transition(
    const ProgramRunRecord&                                     previous_run,
    const ProgramJournalRecord&                                 previous_journal,
    const ProgramJournalRecord&                                 next_journal,
    const std::optional<ProgramJavaScriptCommandJournalEntry>& last_command,
    const ProgramTransitionPublication&                         publication,
    const ProgramGraphSafePointEvidence*                        safe_point_evidence,
    const GraphMigrationCapsule*                                safe_point_capsule,
    const ProgramExecutionLease*                                execution_lease) {
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
    // Non-increasing budgets without a completed command return here before
    // inspecting last_command; when increased is true, valid_increment always
    // queries reads.latest_command(old_run.run_id()).
    if (!increased) {
        if (safe_point_evidence && safe_point_capsule && execution_lease) {
            return is_valid_program_graph_safe_point_transition(
                previous_run, previous_journal, publication, *safe_point_evidence,
                *safe_point_capsule, *execution_lease);
        }
        return ordinary;
    }

    // A safely interrupted CallCore keeps its command coordinate Pending but
    // may release verified unused capacity only when the exact newer
    // checkpoint and measured terminal usage are committed in this CAS.
    if (!publication.commands.empty() || !last_command || !last_command->pending() ||
        publication.run_record.continuation().state != ContinuationState::Interrupted ||
        !publication.run_record.terminal_result() ||
        publication.run_record.terminal_result()->status() != ProgramTerminalStatus::Interrupted ||
        !publication.run_record.exact_checkpoint() ||
        !interrupt_releases_command_reservation(
            previous_journal.core_checkpoint, publication.run_record.exact_checkpoint(),
            checkpoint_event_matches(publication.events, publication.run_record.exact_checkpoint()),
            javascript_call_core_checkpoint_matches(publication.run_record, *last_command,
                                                    *publication.run_record.exact_checkpoint()))) {
        return false;
    }
    auto usage               = publication.run_record.terminal_result()->usage();
    usage.program_operations = 0;
    return is_valid_program_journal_reservation_release(previous_journal, next_journal, usage);
}

inline bool valid_initial_publication(const ProgramTransitionPublication& publication) {
    const auto& journal = publication.journal_record;
    const auto  terminal = publication.run_record.terminal_result();
    const bool  terminal_event = !publication.events.empty() &&
                                 publication.events.back().kind == ProgramEventKind::Terminal;
    return journal.previous_id.empty() && journal.sequence == 1 &&
           budget_is_empty(journal.inflight_reservation) && !publication.events.empty() &&
           publication.events.front().kind == ProgramEventKind::Started &&
           publication.run_record.event_sequence() == publication.events.size() &&
           publication.run_record.effect_sequence() == publication.effects.size() &&
           publication.events.front().sequence == 1 &&
           (publication.effects.empty() || publication.effects.front().sequence() == 1) &&
           valid_command_history_append(publication.run_record, {}, publication.commands) &&
           (!terminal || terminal_event) &&
           (!publication.run_record.fork_receipt() || publication.migration_plan.has_value());
}

// ---------------------------------------------------------------------------
// The read set.
// ---------------------------------------------------------------------------

/** A value the reader either points at, in storage it owns, or hands over. */
template <typename T>
class Borrowed final {
public:
    explicit Borrowed(const T& referenced) noexcept : value_(&referenced) {}
    explicit Borrowed(T&& owned) : value_(std::move(owned)) {}
    const T& get() const noexcept {
        return value_.index() == 0 ? *std::get<0>(value_) : std::get<1>(value_);
    }

private:
    std::variant<const T*, T> value_;
};

/**
 * What the state machine reads from stored state, other than the run head and the
 * execution lease the backend loads before it validates. Every method answers for
 * one owner scope. A run or lineage that is not stored reads as absent.
 */
class TransitionReads {
public:
    /** Update: a database holds the row for the rest of its transaction. */
    enum class Lock : std::uint8_t { None, Update };

    virtual ~TransitionReads() = default;

    /** The graph safe point stored for a run at one lineage head. */
    virtual std::optional<GraphMigrationCapsule> graph_migration_capsule(
        std::string_view run_id, std::string_view lineage_head_id) = 0;
    /** Whether the run ever published an effect with this id. */
    virtual bool effect_id_published(std::string_view run_id, std::string_view effect_id) = 0;
    /**
     * The run's command log in sequence order. A stored log that fails its integrity
     * check is corrupt: the reader throws std::invalid_argument, as it does for every
     * other stored record that contradicts itself.
     */
    virtual Borrowed<std::vector<ProgramJavaScriptCommandJournalEntry>> command_log(
        std::string_view run_id) = 0;
    virtual std::optional<ProgramJavaScriptCommandJournalEntry> latest_command(
        std::string_view run_id) = 0;
    virtual std::optional<ProgramRunRecord> run_record(std::string_view run_id, Lock lock) = 0;
    virtual std::optional<ProgramRunLineage> lineage_head(std::string_view lineage_id,
                                                          Lock             lock) = 0;
    virtual std::optional<ProgramRunGeneration> generation(std::string_view lineage_id,
                                                            std::uint64_t    ordinal) = 0;
    /** The lineage a run belongs to, once a publication of the run named one. */
    virtual std::optional<std::string> run_lineage_id(std::string_view run_id) = 0;
    virtual Borrowed<std::vector<ProgramContextPublication>> context_history(
        std::string_view run_id) = 0;
    /**
     * Whether a history read from storage is a valid epoch chain. A backend that reads
     * a history back from bytes checks it here; one that only ever stores a history it
     * validated as it was appended has nothing to check.
     */
    virtual bool context_history_is_intact(const std::vector<ProgramContextPublication>&) {
        return true;
    }
    virtual Borrowed<std::vector<ProgramChildSynthesisRecord>> synthesis_heads(
        std::string_view run_id) = 0;
    /** The current hook head of every invocation of the run; `run` is the run they bind. */
    virtual Borrowed<std::vector<HookOutboxEntry>> hook_heads(std::string_view       run_id,
                                                              const ProgramRunRecord& run) = 0;
};

// ---------------------------------------------------------------------------
// The state machine.
// ---------------------------------------------------------------------------

/** The run head a backend loaded under its lock; every member outlives validate_transition. */
struct CurrentHead {
    const ProgramRunRecord&             run_record;
    const ProgramJournalRecord&         journal_record;
    const std::optional<MigrationPlan>& migration_plan;
    std::string_view                    last_publication_bytes;
};

struct TransitionRequest {
    std::string_view                     owner_scope;
    std::string_view                     expected_journal_head;
    const ProgramTransitionPublication&  publication;
    /// The publication's canonical bytes, from prepare_transition().
    std::string_view                     publication_bytes;
    const ProgramGraphSafePointEvidence* safe_point_evidence = nullptr;
    const GraphMigrationCapsule*         safe_point_capsule  = nullptr;
    const ProgramExecutionLease*         expected_lease      = nullptr;
    const ProgramExecutionLease*         next_lease          = nullptr;
};

enum class TransitionVerdict : std::uint8_t {
    /// Every check passed; write the publication.
    Proceed,
    /// The same publication is already the run's head.
    AlreadyPresent,
    Conflict,
};

/** What a backend needs to know, besides the verdict, to write the publication. */
struct TransitionPlan {
    /// The run is already recorded as belonging to its lineage.
    bool run_lineage_recorded = false;
    /// The publication's lineage already has a head; the head is updated, not created.
    bool lineage_head_exists = false;
};

/**
 * The checks that need no stored state. Returns the publication's canonical bytes, or
 * nothing when the request is refused before any state is read. An expected head that
 * cannot be a journal head is one of those: it can match no stored head, so it is
 * refused without reading the possibly unreadable state it would be compared with.
 */
inline std::optional<std::string> prepare_transition(
    std::string_view                     owner_scope,
    std::string_view                     expected_journal_head,
    const ProgramTransitionPublication&  publication,
    const ProgramGraphSafePointEvidence* safe_point_evidence,
    const GraphMigrationCapsule*         safe_point_capsule,
    const ProgramExecutionLease*         expected_lease,
    const ProgramExecutionLease*         next_lease) {
    try {
        if (!expected_journal_head.empty() && !is_sha256_identity(expected_journal_head))
            return std::nullopt;
        if (publication.run_record.owner_scope() != owner_scope ||
            static_cast<bool>(safe_point_evidence) != static_cast<bool>(safe_point_capsule) ||
            (safe_point_evidence && (!expected_lease || next_lease)))
            return std::nullopt;
        if (!valid_effect_outbox_binding(publication.run_record, publication.effects))
            return std::nullopt;
        return publication.serialize_canonical();
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
}

/** An effect id is published once per run, and not twice by one publication. */
inline bool effect_ids_are_new(TransitionReads&                              reads,
                               std::string_view                              run_id,
                               const std::vector<ProgramEffectOutboxEntry>& new_effects) {
    std::set<std::string, std::less<>> ids;
    for (const auto& effect : new_effects) {
        const auto& effect_id = effect.effect().effect_id();
        if (!ids.emplace(effect_id).second) return false;
        if (reads.effect_id_published(run_id, effect_id)) return false;
    }
    return true;
}

/** A publication after the run's head: the run, its journal, its histories and its reservations. */
inline bool valid_increment(TransitionReads&                     reads,
                            const CurrentHead&                   old_head,
                            const ProgramTransitionPublication&  next_publication,
                            std::string_view                     expected_journal_head,
                            const ProgramGraphSafePointEvidence* safe_point_evidence,
                            const GraphMigrationCapsule*         safe_point_capsule,
                            const ProgramExecutionLease*         execution_lease) {
    const auto& old_run      = old_head.run_record;
    const auto& next_run     = next_publication.run_record;
    const auto& old_journal  = old_head.journal_record;
    const auto& next_journal = next_publication.journal_record;
    const auto  old_terminal = old_run.terminal_result();
    const auto  new_terminal = next_run.terminal_result();
    const bool  terminal_event = !next_publication.events.empty() &&
                                 next_publication.events.back().kind == ProgramEventKind::Terminal;

    if ((old_terminal && is_final(old_run.continuation().state) && !new_terminal) ||
        (new_terminal && (!old_terminal || old_terminal->id() != new_terminal->id()) &&
         !terminal_event))
        return false;
    if (old_journal.id != expected_journal_head ||
        next_journal.previous_id != expected_journal_head ||
        next_run.created_at_ms() != old_run.created_at_ms() ||
        next_run.updated_at_ms() < old_run.updated_at_ms() ||
        next_run.binding_fingerprint() != old_run.binding_fingerprint() ||
        !same_fork(old_run, next_run) ||
        next_run.recorded_binding_set_fingerprint() != old_run.recorded_binding_set_fingerprint() ||
        next_run.invocation() != old_run.invocation() ||
        next_run.logical_run_id() != old_run.logical_run_id() ||
        (next_run.exact_checkpoint() == old_run.exact_checkpoint() &&
         next_run.exact_checkpoint_content_id() != old_run.exact_checkpoint_content_id()) ||
        !valid_children_transition(old_run, next_run) ||
        next_run.event_sequence() != old_run.event_sequence() + next_publication.events.size() ||
        next_run.effect_sequence() != old_run.effect_sequence() + next_publication.effects.size())
        return false;
    if (!next_publication.events.empty() &&
        next_publication.events.front().sequence != old_run.event_sequence() + 1)
        return false;
    if (!next_publication.effects.empty() &&
        next_publication.effects.front().sequence() != old_run.effect_sequence() + 1)
        return false;
    if (!effect_ids_are_new(reads, old_run.run_id(), next_publication.effects)) return false;
    // Read the last command only when the publication appends commands or raises the remaining
    // budget: valid_command_history_append_after returns at once when commands is empty, and
    // valid_command_reservation_transition reads last_command only when the budget increased.
    const auto last_command =
        (!next_publication.commands.empty() ||
         budget_increased(next_journal.remaining_budget, old_journal.remaining_budget))
            ? reads.latest_command(old_run.run_id())
            : std::nullopt;
    if (!valid_command_history_append_after(old_run, last_command, next_publication.commands) ||
        !valid_command_reservation_transition(old_run, old_journal, next_journal,
                                              last_command, next_publication,
                                              safe_point_evidence, safe_point_capsule,
                                              execution_lease))
        return false;
    if (next_publication.migration_plan &&
        (!old_head.migration_plan ||
         next_publication.migration_plan->id() != old_head.migration_plan->id()))
        return false;
    return true;
}

/** A child's terminal result is bound to the child's own durable generation. */
inline bool child_results_bind(TransitionReads&                    reads,
                               std::string_view                    owner_scope,
                               const CurrentHead*                  current,
                               const ProgramTransitionPublication& publication) {
    // Every old record per id: a result is unchanged when any of them matches it, as a full scan would find.
    std::unordered_multimap<std::string_view, const ProgramChildRecord*> old_by_id;
    if (current) {
        const auto& old_children = current->run_record.children();
        old_by_id.reserve(old_children.size());
        for (const auto& old : old_children) {
            old_by_id.emplace(old.child_run_id, &old);
        }
    }
    for (const auto& child : publication.run_record.children()) {
        if (!child.terminal_generation ||
            (publication.run_generation && publication.run_generation->replacement_receipt()))
            continue;
        bool unchanged = false;
        for (auto [it, end] = old_by_id.equal_range(child.child_run_id); it != end; ++it) {
            const auto& old = *it->second;
            if (old.terminal_generation == child.terminal_generation && old.terminal_result &&
                child.terminal_result && old.terminal_result->id() == child.terminal_result->id())
                unchanged = true;
        }
        if (unchanged) continue;
        const auto lineage_id = program_run_lineage_id(owner_scope, child.child_run_id);
        const auto lineage    = reads.lineage_head(lineage_id, TransitionReads::Lock::None);
        const auto generation = reads.generation(lineage_id, child.terminal_generation);
        const auto run        = generation ? reads.run_record(generation->run_id(),
                                                              TransitionReads::Lock::None)
                                           : std::nullopt;
        if (!lineage || !generation || !run ||
            !does_program_child_generation_result_bind(publication.run_record, child, *generation,
                                                       *lineage, *run))
            return false;
    }
    return true;
}

/** The run's lineage and generation: the head moves only to an admitted successor. */
inline bool lineage_binds(TransitionReads&                    reads,
                          const CurrentHead*                  current,
                          const ProgramTransitionPublication& publication,
                          const ProgramExecutionLease*        next_lease,
                          TransitionPlan&                     plan) {
    const auto& run_id         = publication.run_record.run_id();
    const auto  run_lineage_id = reads.run_lineage_id(run_id);
    const bool  adopting_legacy_lineage = current && !run_lineage_id && publication.run_lineage;
    if (current && run_lineage_id) {
        if (!publication.run_lineage || *run_lineage_id != publication.run_lineage->lineage_id())
            return false;
    } else if (run_lineage_id) {
        return false;
    }
    plan.run_lineage_recorded = run_lineage_id.has_value();

    if (!publication.run_lineage) return true;
    const auto current_lineage =
        reads.lineage_head(publication.run_lineage->lineage_id(), TransitionReads::Lock::Update);
    plan.lineage_head_exists = current_lineage.has_value();
    if (!current_lineage) {
        if (!publication.run_generation ||
            !is_valid_program_run_lineage_initial(*publication.run_lineage,
                                                  *publication.run_generation))
            return false;
    } else if (adopting_legacy_lineage) {
        return false;
    } else if (!is_valid_program_run_lineage_transition(*current_lineage, *publication.run_lineage,
                                                        publication.run_generation)) {
        return false;
    }
    if (current_lineage && !publication.run_generation) {
        const auto active = reads.generation(publication.run_lineage->lineage_id(),
                                             publication.run_lineage->active_generation());
        if (!active ||
            !does_program_run_generation_bind(*active, *publication.run_lineage,
                                              publication.run_record))
            return false;
    }
    if (current_lineage && publication.run_generation) {
        const auto active = reads.generation(publication.run_lineage->lineage_id(),
                                             current_lineage->active_generation());
        if (!active || active->child_depth() != publication.run_generation->child_depth())
            return false;
        const auto source = reads.run_record(active->run_id(), TransitionReads::Lock::Update);
        if (!source) return false;
        const auto graph_migration = publication.run_generation->graph_migration_receipt();
        const auto source_contexts = reads.context_history(active->run_id());
        const auto source_hooks    = reads.hook_heads(active->run_id(), *source);
        bool       valid_successor = is_valid_program_runtime_state_transfer(
            publication.run_generation->runtime_state_transfer_receipt(), *active, *current_lineage,
            *source, source_contexts.get(), source_hooks.get(), *publication.run_generation,
            publication.run_record, publication.context_publication);
        if (valid_successor && graph_migration) {
            const auto command = reads.latest_command(active->run_id());
            const auto durable_capsule =
                reads.graph_migration_capsule(active->run_id(), current_lineage->id());
            valid_successor = durable_capsule && next_lease && !command &&
                              publication.commands.empty() && publication.effects.empty() &&
                              publication.events.size() == 1 &&
                              does_program_graph_migration_started_event_bind(
                                  publication.events.front(), publication.run_record) &&
                              publication.run_record.event_sequence() == 1 &&
                              publication.migration_plan &&
                              is_valid_program_graph_migration_transition(
                                  *active, *current_lineage, *source, *durable_capsule,
                                  *publication.migration_plan, *publication.run_generation,
                                  *publication.run_lineage, publication.run_record);
        } else if (valid_successor) {
            const auto checkpoint = reads.latest_command(active->run_id());
            valid_successor = checkpoint &&
                              is_valid_program_replacement_transition(
                                  *active, *current_lineage, *source, *checkpoint,
                                  *publication.run_generation, *publication.run_lineage,
                                  publication.run_record);
        }
        if (!valid_successor) return false;
    }
    if (publication.run_generation && current_lineage &&
        reads.generation(publication.run_lineage->lineage_id(),
                         publication.run_generation->generation()))
        return false;
    return true;
}

/** A fork debits its source lineage in the same publication and binds the source's state. */
inline bool fork_source_binds(TransitionReads&                    reads,
                              const CurrentHead*                  current,
                              const ProgramTransitionPublication& publication) {
    if (!publication.fork_source_lineage)
        return current || !publication.run_record.fork_receipt();
    const auto current_fork_source = reads.lineage_head(
        publication.fork_source_lineage->lineage_id(), TransitionReads::Lock::Update);
    if (!current_fork_source ||
        !is_valid_program_run_lineage_transition(*current_fork_source,
                                                 *publication.fork_source_lineage) ||
        !fork_allocation_fits(*current_fork_source, *publication.fork_source_lineage,
                              publication.journal_record.remaining_budget, true))
        return false;
    const auto active = reads.generation(current_fork_source->lineage_id(),
                                         current_fork_source->active_generation());
    const auto source = active ? reads.run_record(active->run_id(), TransitionReads::Lock::Update)
                               : std::nullopt;
    std::optional<Borrowed<std::vector<ProgramContextPublication>>> source_contexts;
    if (active) source_contexts.emplace(reads.context_history(active->run_id()));
    const std::vector<ProgramContextPublication> no_contexts;
    return active && source &&
           !has_blocking_hook_obligation(reads.hook_heads(active->run_id(), *source).get()) &&
           is_valid_program_runtime_context_clone(source_contexts ? source_contexts->get()
                                                                   : no_contexts,
                                                  publication.run_record,
                                                  publication.context_publication) &&
           fork_binds_predecessor(publication.run_record, *active, *current_fork_source, *source);
}

/** A recorded replay takes its retained work authority from the exact source debit. */
inline bool replay_source_binds(TransitionReads&                    reads,
                                const CurrentHead*                  current,
                                const ProgramTransitionPublication& publication) {
    if (!publication.recorded_replay_source) return true;
    const auto& receipt = *publication.recorded_replay_source;
    const auto  lineage = reads.lineage_head(
        publication.recorded_replay_source_lineage->lineage_id(), TransitionReads::Lock::Update);
    const auto source   = reads.run_record(receipt.run_id, TransitionReads::Lock::Update);
    const auto contexts = reads.context_history(receipt.run_id);
    return !current && lineage && source &&
           !has_blocking_hook_obligation(reads.hook_heads(receipt.run_id, *source).get()) &&
           is_valid_program_runtime_context_clone(contexts.get(), publication.run_record,
                                                  publication.context_publication) &&
           is_valid_program_recorded_replay_allocation(publication, *lineage, *source);
}

/**
 * Decides whether `request` may be written against the stored state. `current` is the
 * run's head and `current_lease` its execution lease, both loaded by the caller (null
 * when the run has none). The checks run in this order, and the first to fail is a
 * Conflict:
 *   the live lease, the head compare-and-swap, the child results, the context, synthesis
 *   and hook histories, then the lineage, the fork source and the replay source.
 */
inline TransitionVerdict validate_transition(const TransitionRequest&   request,
                                             const CurrentHead*         current,
                                             const ProgramExecutionLease* current_lease,
                                             TransitionReads&           reads,
                                             TransitionPlan&            plan) {
    const auto& publication = request.publication;
    const auto& run_id      = publication.run_record.run_id();
    if (request.safe_point_evidence && !current) return TransitionVerdict::Conflict;

    if (current && current->last_publication_bytes == request.publication_bytes) {
        if (request.safe_point_capsule) {
            const auto stored = reads.graph_migration_capsule(
                run_id, request.safe_point_capsule->source_lineage_head_id());
            if (!stored || stored->id() != request.safe_point_capsule->id())
                return TransitionVerdict::Conflict;
        }
        const bool lease_result_matches =
            request.next_lease
                ? current_lease && current_lease->id() == request.next_lease->id()
                : !request.expected_lease || !current_lease;
        if (!lease_result_matches) return TransitionVerdict::Conflict;
        return request.expected_journal_head == publication.journal_record.previous_id
                   ? TransitionVerdict::AlreadyPresent
                   : TransitionVerdict::Conflict;
    }
    if (request.expected_lease) {
        if (!current_lease || current_lease->id() != request.expected_lease->id() || !current ||
            request.expected_lease->owner_scope() != request.owner_scope ||
            request.expected_lease->run_id() != run_id ||
            request.expected_lease->attempt() != current->run_record.continuation().attempt ||
            request.expected_lease->program_version_id() !=
                current->run_record.program_version_id() ||
            request.expected_lease->bundle_id() != current->run_record.bundle_id())
            return TransitionVerdict::Conflict;
    } else if (current_lease) {
        return TransitionVerdict::Conflict;
    }
    if (request.next_lease &&
        !does_program_execution_lease_bind(*request.next_lease, publication, request.expected_lease))
        return TransitionVerdict::Conflict;
    if (!current && publication.fork_source_lineage) {
        const auto fork = publication.run_record.fork_receipt();
        if (!fork ||
            fork->storage_schema_version() < ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION)
            return TransitionVerdict::Conflict;
    }

    if (!current) {
        if (!request.expected_journal_head.empty() || !valid_initial_publication(publication))
            return TransitionVerdict::Conflict;
    } else if (!valid_increment(reads, *current, publication, request.expected_journal_head,
                                request.safe_point_evidence, request.safe_point_capsule,
                                request.expected_lease)) {
        return TransitionVerdict::Conflict;
    }

    if (!child_results_bind(reads, request.owner_scope, current, publication))
        return TransitionVerdict::Conflict;

    const auto context_history = reads.context_history(run_id);
    if (!reads.context_history_is_intact(context_history.get()) ||
        !is_valid_program_context_history_append(context_history.get(),
                                                 publication.context_publication))
        return TransitionVerdict::Conflict;
    if (!publication.child_synthesis_records.empty() &&
        !is_valid_program_child_synthesis_publication(
            reads.synthesis_heads(run_id).get(), current ? &current->run_record : nullptr,
            publication))
        return TransitionVerdict::Conflict;
    const auto hook_heads =
        reads.hook_heads(run_id, current ? current->run_record : publication.run_record);
    if (!is_valid_program_hook_history_append(hook_heads.get(), publication.hook_outbox_entries,
                                              publication.run_record))
        return TransitionVerdict::Conflict;

    if (!lineage_binds(reads, current, publication, request.next_lease, plan) ||
        !fork_source_binds(reads, current, publication) ||
        !replay_source_binds(reads, current, publication))
        return TransitionVerdict::Conflict;
    return TransitionVerdict::Proceed;
}

}  // namespace neograph::program::detail::transition
