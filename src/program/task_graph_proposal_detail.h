// Internal parsing helpers shared by TaskGraphProposal::parse and every parser of
// planner-authored task data (step plans, patches). Not part of the public API.
#pragma once

#include <neograph/program/source.h>
#include <neograph/program/task_graph_proposal.h>

#include "canonical_json.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neograph::program::detail::task_graph {

inline std::string escape_pointer_segment(std::string_view segment) {
    std::string result;
    result.reserve(segment.size());
    for (const char value : segment) {
        if (value == '~')
            result += "~0";
        else if (value == '/')
            result += "~1";
        else
            result.push_back(value);
    }
    return result;
}

inline std::string child_pointer(std::string_view parent, std::string_view child) {
    std::string result(parent);
    result.push_back('/');
    result += escape_pointer_segment(child);
    return result;
}

inline std::string index_pointer(std::string_view parent, std::size_t index) {
    return std::string(parent) + "/" + std::to_string(index);
}

inline bool is_ancestor_pointer(std::string_view ancestor, std::string_view pointer) {
    if (ancestor == pointer) return true;
    if (ancestor.empty()) return !pointer.empty() && pointer.front() == '/';
    return pointer.size() > ancestor.size() && pointer.starts_with(ancestor) &&
           pointer[ancestor.size()] == '/';
}

inline SourceCoordinate map_source_coordinate(std::string_view                     source_id,
                                       const std::vector<SourceMapEntry>& mappings,
                                       std::string_view                     generated_pointer) {
    const SourceMapEntry* best = nullptr;
    for (const auto& entry : mappings) {
        if (!is_ancestor_pointer(entry.generated_pointer, generated_pointer)) continue;
        if (!best || entry.generated_pointer.size() > best->generated_pointer.size()) best = &entry;
    }
    if (!best) return SourceCoordinate{std::string(source_id), std::string(generated_pointer), std::nullopt};
    SourceCoordinate result = best->authored;
    if (best->generated_pointer == generated_pointer) return result;
    result.span.reset();
    result.json_pointer += generated_pointer.substr(best->generated_pointer.size());
    return result;
}

inline std::optional<std::uint64_t> unsigned_integer(const json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer()) return std::nullopt;
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(signed_value);
}

inline std::string diagnostic_message(std::size_t count) {
    return "Task graph proposal rejected with " + std::to_string(count) + " diagnostic(s)";
}

struct PendingDiagnostic {
    Diagnostic  diagnostic;
    std::string generated_pointer;
    std::string canonical_witness;
};

class DiagnosticAccumulator {
public:
    DiagnosticAccumulator(std::string source_id, std::vector<SourceMapEntry> source_map)
        : source_id_(std::move(source_id)), mappings_(std::move(source_map)) {
        std::sort(mappings_.begin(), mappings_.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.generated_pointer, lhs.authored.source_id,
                            lhs.authored.json_pointer) <
                   std::tie(rhs.generated_pointer, rhs.authored.source_id,
                            rhs.authored.json_pointer);
        });
    }

    void add(std::string generated_pointer, std::string code, std::string message,
             json witness = json::object()) {
        PendingDiagnostic pending;
        pending.generated_pointer    = std::move(generated_pointer);
        pending.diagnostic.phase     = CompilePhase::Normalize;
        pending.diagnostic.code      = std::move(code);
        pending.diagnostic.severity  = DiagnosticSeverity::Error;
        pending.diagnostic.primary   = map(pending.generated_pointer);
        pending.diagnostic.message   = std::move(message);
        pending.diagnostic.witness   = detail::owned_json_copy(witness);
        pending.canonical_witness    = detail::canonical_json_bytes(pending.diagnostic.witness);
        diagnostics_.push_back(std::move(pending));
    }

    bool has_errors() const noexcept { return !diagnostics_.empty(); }

    [[noreturn]] void throw_error() {
        sort();
        std::vector<Diagnostic> diagnostics;
        diagnostics.reserve(diagnostics_.size());
        for (auto& pending : diagnostics_)
            diagnostics.push_back(std::move(pending.diagnostic));
        throw TaskGraphProposalError(std::move(diagnostics));
    }

private:
    void sort() {
        std::sort(diagnostics_.begin(), diagnostics_.end(), [](const auto& lhs, const auto& rhs) {
            return std::tuple{static_cast<int>(lhs.diagnostic.phase), lhs.generated_pointer,
                              lhs.diagnostic.code, lhs.canonical_witness} <
                   std::tuple{static_cast<int>(rhs.diagnostic.phase), rhs.generated_pointer,
                              rhs.diagnostic.code, rhs.canonical_witness};
        });
    }

    SourceCoordinate map(std::string_view generated_pointer) const {
        return map_source_coordinate(source_id_, mappings_, generated_pointer);
    }

    std::string                 source_id_;
    std::vector<SourceMapEntry> mappings_;
    std::vector<PendingDiagnostic> diagnostics_;
};

inline void add_unknown_fields(DiagnosticAccumulator&                  diagnostics,
                        const json&                             value,
                        std::string_view                        pointer,
                        std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return;
    for (auto field = value.begin(); field != value.end(); ++field) {
        const auto known = std::find(allowed.begin(), allowed.end(), std::string_view(field.key()));
        if (known == allowed.end()) {
            diagnostics.add(child_pointer(pointer, field.key()), "P_PROPOSAL_UNKNOWN_FIELD",
                            "Task graph proposal contains an undeclared field",
                            json{{"field", field.key()}});
        }
    }
}

inline std::optional<std::string> required_token(DiagnosticAccumulator& diagnostics,
                                          const json&             object,
                                          std::string_view        pointer,
                                          std::string_view        field) {
    const auto field_pointer = child_pointer(pointer, field);
    if (!object.contains(std::string(field))) {
        diagnostics.add(field_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Task graph proposal field is required",
                        json{{"field", field}});
        return std::nullopt;
    }
    const auto& value = object.at(std::string(field));
    if (!value.is_string()) {
        diagnostics.add(field_pointer, "P_PROPOSAL_TYPE", "Task graph proposal field must be a string",
                        json{{"field", field}, {"expected", "string"}});
        return std::nullopt;
    }
    const auto result = value.get<std::string>();
    try {
        detail::validate_token(result, field);
    } catch (const std::exception& error) {
        diagnostics.add(field_pointer, "P_PROPOSAL_TOKEN", error.what(), json{{"field", field}});
        return std::nullopt;
    }
    return result;
}

inline std::optional<std::string> required_nonempty_pointer(DiagnosticAccumulator& diagnostics,
                                                      const json&             object,
                                                      std::string_view        pointer,
                                                      std::string_view        field) {
    const auto value = required_token(diagnostics, object, pointer, field);
    if (!value) return std::nullopt;
    try {
        detail::validate_json_pointer(*value);
        if (value->empty()) throw std::invalid_argument("JSON Pointer must not be empty");
    } catch (const std::exception& error) {
        diagnostics.add(child_pointer(pointer, field), "P_PROPOSAL_JSON_POINTER", error.what(),
                        json{{"field", field}});
        return std::nullopt;
    }
    return value;
}

inline std::optional<std::uint64_t> optional_unsigned(DiagnosticAccumulator& diagnostics,
                                                const json&             object,
                                                std::string_view        pointer,
                                                std::string_view        field) {
    if (!object.contains(std::string(field))) return 0;
    const auto value = unsigned_integer(object.at(std::string(field)));
    if (!value) {
        diagnostics.add(child_pointer(pointer, field), "P_PROPOSAL_TYPE",
                        "Task graph proposal budget must be an unsigned integer",
                        json{{"field", field}, {"expected", "unsigned integer"}});
    }
    return value;
}

inline std::optional<TaskGraphBudget> parse_budget(DiagnosticAccumulator& diagnostics,
                                            const json&             value,
                                            std::string_view        pointer) {
    if (!value.is_object()) {
        diagnostics.add(std::string(pointer), "P_PROPOSAL_TYPE", "Task budget must be an object",
                        json{{"expected", "object"}});
        return std::nullopt;
    }
    add_unknown_fields(diagnostics, value, pointer,
                       {"wall_time_ms", "model_tokens", "monetary_microunits", "output_bytes"});
    const auto wall = optional_unsigned(diagnostics, value, pointer, "wall_time_ms");
    const auto tokens = optional_unsigned(diagnostics, value, pointer, "model_tokens");
    const auto monetary = optional_unsigned(diagnostics, value, pointer, "monetary_microunits");
    const auto output = optional_unsigned(diagnostics, value, pointer, "output_bytes");
    if (!wall || !tokens || !monetary || !output) return std::nullopt;
    return TaskGraphBudget{*wall, *tokens, *monetary, *output};
}

inline std::optional<TaskGraphInputBinding> parse_binding(DiagnosticAccumulator& diagnostics,
                                                    const json&             value,
                                                    std::string_view        pointer) {
    if (!value.is_object()) {
        diagnostics.add(std::string(pointer), "P_PROPOSAL_TYPE", "Input binding must be an object",
                        json{{"expected", "object"}});
        return std::nullopt;
    }
    add_unknown_fields(diagnostics, value, pointer, {"from", "to"});

    const auto from_pointer = child_pointer(pointer, "from");
    const auto to_pointer = child_pointer(pointer, "to");
    bool       valid = true;
    if (!value.contains("from")) {
        diagnostics.add(from_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Input binding field is required",
                        json{{"field", "from"}});
        valid = false;
    } else if (!value["from"].is_object()) {
        diagnostics.add(from_pointer, "P_PROPOSAL_TYPE", "Input binding source must be an object",
                        json{{"expected", "object"}});
        valid = false;
    }
    if (!value.contains("to")) {
        diagnostics.add(to_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Input binding field is required",
                        json{{"field", "to"}});
        valid = false;
    } else if (!value["to"].is_object()) {
        diagnostics.add(to_pointer, "P_PROPOSAL_TYPE", "Input binding target must be an object",
                        json{{"expected", "object"}});
        valid = false;
    }
    if (!valid) return std::nullopt;

    const auto& from = value["from"];
    const auto& to = value["to"];
    add_unknown_fields(diagnostics, from, from_pointer, {"task", "artifact", "field"});
    add_unknown_fields(diagnostics, to, to_pointer, {"field"});
    const auto task = required_token(diagnostics, from, from_pointer, "task");
    const auto artifact = required_token(diagnostics, from, from_pointer, "artifact");
    const auto source_field = required_nonempty_pointer(diagnostics, from, from_pointer, "field");
    const auto target_field = required_nonempty_pointer(diagnostics, to, to_pointer, "field");
    if (!task || !artifact || !source_field || !target_field) return std::nullopt;
    return TaskGraphInputBinding{{*task, *artifact, *source_field}, {*target_field}};
}

struct ParsedTask {
    std::size_t                      index = 0;
    TaskGraphTask                    task;
    std::vector<std::size_t>         binding_source_indices;
    std::vector<std::size_t>         dependency_source_indices;
    bool                             id_valid = false;
    bool                             template_valid = false;
    const TaskGraphTemplateContract* template_contract = nullptr;
};

inline ParsedTask parse_task(DiagnosticAccumulator& diagnostics, const json& value, std::size_t index) {
    const auto pointer = index_pointer("/tasks", index);
    ParsedTask parsed;
    parsed.index = index;
    if (!value.is_object()) {
        diagnostics.add(pointer, "P_PROPOSAL_TYPE", "Task graph task must be an object",
                        json{{"expected", "object"}});
        return parsed;
    }
    add_unknown_fields(diagnostics, value, pointer,
                       {"id", "template", "input_bindings", "depends_on", "budget"});

    if (const auto id = required_token(diagnostics, value, pointer, "id")) {
        parsed.task.id = *id;
        parsed.id_valid = true;
    }
    if (const auto template_id = required_token(diagnostics, value, pointer, "template")) {
        parsed.task.template_id = *template_id;
        parsed.template_valid = true;
    }

    const auto bindings_pointer = child_pointer(pointer, "input_bindings");
    if (!value.contains("input_bindings")) {
        diagnostics.add(bindings_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Task field is required",
                        json{{"field", "input_bindings"}});
    } else if (!value["input_bindings"].is_array()) {
        diagnostics.add(bindings_pointer, "P_PROPOSAL_TYPE", "Task input_bindings must be an array",
                        json{{"expected", "array"}});
    } else {
        std::size_t binding_index = 0;
        for (const auto& binding : value["input_bindings"]) {
            if (const auto parsed_binding = parse_binding(
                    diagnostics, binding, index_pointer(bindings_pointer, binding_index))) {
                parsed.task.input_bindings.push_back(*parsed_binding);
                parsed.binding_source_indices.push_back(binding_index);
            }
            ++binding_index;
        }
    }

    const auto dependencies_pointer = child_pointer(pointer, "depends_on");
    if (!value.contains("depends_on")) {
        diagnostics.add(dependencies_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Task field is required",
                        json{{"field", "depends_on"}});
    } else if (!value["depends_on"].is_array()) {
        diagnostics.add(dependencies_pointer, "P_PROPOSAL_TYPE", "Task depends_on must be an array",
                        json{{"expected", "array"}});
    } else {
        std::set<std::string> seen;
        std::size_t           dependency_index = 0;
        for (const auto& dependency : value["depends_on"]) {
            const auto item_pointer = index_pointer(dependencies_pointer, dependency_index);
            if (!dependency.is_string()) {
                diagnostics.add(item_pointer, "P_PROPOSAL_TYPE", "Task dependency must be a string",
                                json{{"expected", "string"}});
            } else {
                const auto id = dependency.get<std::string>();
                try {
                    detail::validate_token(id, "depends_on item");
                    if (!seen.insert(id).second) {
                        diagnostics.add(item_pointer, "P_PROPOSAL_DUPLICATE_DEPENDENCY",
                                        "Task dependency must occur at most once",
                                        json{{"task", id}});
                    } else {
                        parsed.task.depends_on.push_back(id);
                        parsed.dependency_source_indices.push_back(dependency_index);
                    }
                } catch (const std::exception& error) {
                    diagnostics.add(item_pointer, "P_PROPOSAL_TOKEN", error.what(),
                                    json{{"field", "depends_on"}});
                }
            }
            ++dependency_index;
        }
    }

    const auto budget_pointer = child_pointer(pointer, "budget");
    if (!value.contains("budget")) {
        diagnostics.add(budget_pointer, "P_PROPOSAL_REQUIRED_FIELD", "Task field is required",
                        json{{"field", "budget"}});
    } else if (const auto budget = parse_budget(diagnostics, value["budget"], budget_pointer)) {
        parsed.task.budget = *budget;
    }
    return parsed;
}

}  // namespace neograph::program::detail::task_graph
