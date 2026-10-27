#pragma once

// A run that waits on a child which paused on an interrupt (#658).
//
// A parent whose JavaScript command or typed plan awaits a child ends Interrupted when the child does.
// The question belongs to the child, whose run is the one to resume; the parent is matched to the
// child(ren) it awaits by the identity of the awaited spawn operation and child run id, never by
// comparing interrupt payloads.

#include <neograph/program/command_journal.h>
#include <neograph/program/result.h>
#include <neograph/program/run_record.h>

#include "canonical_json.h"
#include "transition_validator.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program::detail {

inline void collect_awaited_spawn_operations(
    const JavaScriptCommand&         command,
    const std::string&               operation_id,
    const std::optional<json>&       settled_members,
    bool                             under_await,
    std::vector<std::string>&        out,
    std::size_t                      depth = 0) {
    if (depth > 32) return;
    if (command.kind() == JavaScriptCommandKind::Spawn) {
        if (under_await) out.push_back(operation_id);
        return;
    }
    const auto arguments = command.arguments();
    if (command.kind() == JavaScriptCommandKind::Await) {
        if (arguments.contains("command")) {
            collect_awaited_spawn_operations(
                JavaScriptCommand::from_json(arguments.at("command")),
                operation_id + "/await", settled_members, true, out, depth + 1);
        }
        return;
    }
    if (command.kind() == JavaScriptCommandKind::Join) {
        if (!arguments.contains("members") || !arguments.at("members").is_array()) return;
        const auto& members = arguments.at("members");
        for (std::size_t index = 0; index < members.size(); ++index) {
            const auto member_op = operation_id + "/member/" + std::to_string(index);
            if (settled_members && settled_members->is_object() &&
                settled_members->contains(member_op))
                continue;
            collect_awaited_spawn_operations(
                JavaScriptCommand::from_json(members.at(index)),
                member_op, settled_members, under_await, out, depth + 1);
        }
    }
}

inline bool child_matches_spawn_operation(const ProgramRunRecord&   run,
                                          const ProgramChildRecord& child,
                                          std::string_view          operation_id) {
    const auto expected_run_id = sha256_identity(
        "program-dsl-child/v1",
        canonical_json_bytes(json{{"owner_scope", run.owner_scope()},
                                  {"parent_run_id", run.run_id()},
                                  {"parent_program_version_id", run.program_version_id()},
                                  {"link_id", child.link_id},
                                  {"operation_id", std::string(operation_id)},
                                  {"execution_key", std::string(operation_id)}}));
    if (child.child_run_id == expected_run_id) return true;
    const auto expected_trace = run.invocation().correlation_id + ":" +
                                std::string(operation_id) + ":" + std::string(operation_id);
    if (child.invocation.trace_id == expected_trace) return true;
    const auto trace_prefix = run.invocation().correlation_id + ":" +
                              std::string(operation_id) + ":";
    const auto trace_suffix = ":" + std::string(operation_id) + ":0";
    return child.invocation.trace_id.starts_with(trace_prefix) &&
           child.invocation.trace_id.ends_with(trace_suffix);
}

/// The children of `run` that its paused await waits on, matched by command/child identity.
/// Empty for a run paused on a question of its own.
inline std::vector<ProgramChildRecord> children_paused_on(
    const ProgramRunRecord&                                  run,
    const std::vector<ProgramJavaScriptCommandJournalEntry>& commands       = {},
    const std::optional<ProgramJournalRecord>&               latest_journal = std::nullopt) {
    std::vector<ProgramChildRecord> paused;
    if (run.continuation().state != ContinuationState::Interrupted) return paused;
    if (!commands.empty()) {
        const auto& latest_command = commands.back();
        if (!latest_command.pending()) return paused;
        const bool reservation_released =
            latest_journal && latest_journal->inflight_reservation == RunBudget{};
        if (reservation_released && run.exact_checkpoint() &&
            transition::javascript_call_core_checkpoint_matches(
                run.run_id(), latest_command, *run.exact_checkpoint())) {
            return paused;
        }
        std::vector<std::string> awaited_spawns;
        collect_awaited_spawn_operations(
            latest_command.command(),
            "root.javascript." + std::to_string(latest_command.command_ordinal()),
            latest_command.settled_members(), false, awaited_spawns);
        for (const auto& child : run.children()) {
            for (const auto& op : awaited_spawns) {
                if (child_matches_spawn_operation(run, child, op)) {
                    paused.push_back(child);
                    break;
                }
            }
        }
        return paused;
    }

    // Typed orchestration plan (no JavaScript command journal): an entered await of a spawned
    // child records the child_run_id it waits on in settled_branches.
    if (const auto settled = run.settled_branches(); settled && settled->is_object()) {
        for (const auto& [operation_id, entry] : settled->items()) {
            (void)operation_id;
            if (!entry.is_object() || entry.value("kind", std::string{}) != "entered" ||
                !entry.contains("child_run_id") || !entry.at("child_run_id").is_string()) {
                continue;
            }
            const auto awaited_id = entry.at("child_run_id").get<std::string>();
            for (const auto& child : run.children()) {
                if (child.child_run_id == awaited_id) {
                    paused.push_back(child);
                    break;
                }
            }
        }
    }
    return paused;
}

}  // namespace neograph::program::detail
