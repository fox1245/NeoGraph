#include <neograph/program/task_graph_proposal.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

std::string escape_pointer_segment(std::string_view segment) {
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

std::string child_pointer(std::string_view parent, std::string_view child) {
    std::string result(parent);
    result.push_back('/');
    result += escape_pointer_segment(child);
    return result;
}

std::string index_pointer(std::string_view parent, std::size_t index) {
    return std::string(parent) + "/" + std::to_string(index);
}

bool is_ancestor_pointer(std::string_view ancestor, std::string_view pointer) {
    if (ancestor == pointer) return true;
    if (ancestor.empty()) return !pointer.empty() && pointer.front() == '/';
    return pointer.size() > ancestor.size() && pointer.starts_with(ancestor) &&
           pointer[ancestor.size()] == '/';
}

SourceCoordinate map_source_coordinate(std::string_view                     source_id,
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

std::optional<std::uint64_t> unsigned_integer(const json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer()) return std::nullopt;
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(signed_value);
}

std::string diagnostic_message(std::size_t count) {
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

void add_unknown_fields(DiagnosticAccumulator&                  diagnostics,
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

std::optional<std::string> required_token(DiagnosticAccumulator& diagnostics,
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

std::optional<std::string> required_nonempty_pointer(DiagnosticAccumulator& diagnostics,
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

std::optional<std::uint64_t> optional_unsigned(DiagnosticAccumulator& diagnostics,
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

std::optional<TaskGraphBudget> parse_budget(DiagnosticAccumulator& diagnostics,
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

std::optional<TaskGraphInputBinding> parse_binding(DiagnosticAccumulator& diagnostics,
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

ParsedTask parse_task(DiagnosticAccumulator& diagnostics, const json& value, std::size_t index) {
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

std::optional<TaskGraphJoinPolicy> parse_join(DiagnosticAccumulator& diagnostics, const json& value,
                                              std::string_view pointer) {
    if (!value.is_object()) {
        diagnostics.add(std::string(pointer), "P_PROPOSAL_TYPE", "Join policy must be an object",
                        json{{"expected", "object"}});
        return std::nullopt;
    }
    add_unknown_fields(diagnostics, value, pointer, {"kind"});
    const auto kind = required_token(diagnostics, value, pointer, "kind");
    if (!kind) return std::nullopt;
    if (*kind != "all") {
        diagnostics.add(child_pointer(pointer, "kind"), "P_PROPOSAL_JOIN",
                        "TaskGraphProposal-v1 supports only an all-of join",
                        json{{"supported", json::array({"all"})}});
        return std::nullopt;
    }
    return TaskGraphJoinPolicy{TaskGraphJoinKind::All};
}

bool is_allowed_field(const std::vector<std::string>& fields, std::string_view field) {
    return std::find(fields.begin(), fields.end(), field) != fields.end();
}

const TaskGraphArtifactContract* find_artifact(const TaskGraphTemplateContract& contract,
                                               std::string_view artifact) {
    const auto found = std::find_if(contract.output_artifacts.begin(), contract.output_artifacts.end(),
                                    [artifact](const auto& candidate) {
                                        return candidate.artifact == artifact;
                                    });
    return found == contract.output_artifacts.end() ? nullptr : &*found;
}

bool exceeds(std::uint64_t requested, std::uint64_t ceiling) noexcept {
    return requested > ceiling;
}

std::array<std::tuple<std::string_view, std::uint64_t, std::uint64_t>, 4>
budget_dimensions(const TaskGraphBudget& requested, const TaskGraphBudget& ceiling) {
    return {{{"wall_time_ms", requested.wall_time_ms, ceiling.wall_time_ms},
             {"model_tokens", requested.model_tokens, ceiling.model_tokens},
             {"monetary_microunits", requested.monetary_microunits,
              ceiling.monetary_microunits},
             {"output_bytes", requested.output_bytes, ceiling.output_bytes}}};
}

void validate_budget_ceiling(DiagnosticAccumulator&        diagnostics,
                             const TaskGraphBudget&         requested,
                             const TaskGraphBudget&         ceiling,
                             std::string_view               pointer,
                             std::string_view               ceiling_name) {
    for (const auto& [resource, amount, maximum] : budget_dimensions(requested, ceiling)) {
        if (!exceeds(amount, maximum)) continue;
        diagnostics.add(child_pointer(pointer, resource), "P_PROPOSAL_LIMIT_TASK_BUDGET",
                        "Task budget exceeds its permitted ceiling",
                        json{{"resource", resource}, {"requested", amount}, {"ceiling", maximum},
                             {"ceiling_name", ceiling_name}});
    }
}

struct BudgetSum {
    TaskGraphBudget total;
    std::array<bool, 4> overflow = {};
};

bool add_without_overflow(std::uint64_t& total, std::uint64_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - total) return false;
    total += amount;
    return true;
}

BudgetSum sum_budgets(const std::vector<ParsedTask>& tasks) {
    BudgetSum result;
    for (const auto& parsed : tasks) {
        result.overflow[0] |=
            !add_without_overflow(result.total.wall_time_ms, parsed.task.budget.wall_time_ms);
        result.overflow[1] |=
            !add_without_overflow(result.total.model_tokens, parsed.task.budget.model_tokens);
        result.overflow[2] |= !add_without_overflow(result.total.monetary_microunits,
                                                     parsed.task.budget.monetary_microunits);
        result.overflow[3] |=
            !add_without_overflow(result.total.output_bytes, parsed.task.budget.output_bytes);
    }
    return result;
}

void validate_total_budget(DiagnosticAccumulator& diagnostics, const BudgetSum& requested,
                           const TaskGraphBudget& ceiling) {
    std::size_t dimension = 0;
    for (const auto& [resource, amount, maximum] : budget_dimensions(requested.total, ceiling)) {
        if (requested.overflow[dimension]) {
            diagnostics.add("/tasks", "P_PROPOSAL_LIMIT_TOTAL_BUDGET",
                            "Aggregate task budget overflows the representable proposal range",
                            json{{"resource", resource}, {"ceiling", maximum}, {"overflow", true}});
        } else if (exceeds(amount, maximum)) {
            diagnostics.add("/tasks", "P_PROPOSAL_LIMIT_TOTAL_BUDGET",
                            "Aggregate task budget exceeds the proposal ceiling",
                            json{{"resource", resource}, {"requested", amount}, {"ceiling", maximum}});
        }
        ++dimension;
    }
}

bool binding_less(const TaskGraphInputBinding& lhs, const TaskGraphInputBinding& rhs) {
    return std::tie(lhs.from.task_id, lhs.from.artifact, lhs.from.field, lhs.to.field) <
           std::tie(rhs.from.task_id, rhs.from.artifact, rhs.from.field, rhs.to.field);
}

json task_to_json(const TaskGraphTask& task) {
    auto dependencies = task.depends_on;
    std::sort(dependencies.begin(), dependencies.end());
    auto bindings = task.input_bindings;
    std::sort(bindings.begin(), bindings.end(), binding_less);

    json encoded_bindings = json::array();
    for (const auto& binding : bindings) {
        encoded_bindings.push_back(
            json{{"from", json{{"task", binding.from.task_id}, {"artifact", binding.from.artifact},
                                {"field", binding.from.field}}},
                 {"to", json{{"field", binding.to.field}}}});
    }
    return json{{"id", task.id},
                {"template", task.template_id},
                {"input_bindings", std::move(encoded_bindings)},
                {"depends_on", std::move(dependencies)},
                {"budget",
                 json{{"wall_time_ms", task.budget.wall_time_ms},
                      {"model_tokens", task.budget.model_tokens},
                      {"monetary_microunits", task.budget.monetary_microunits},
                      {"output_bytes", task.budget.output_bytes}}}};
}

json proposal_to_json(const std::vector<TaskGraphTask>& tasks, const TaskGraphJoinPolicy& join) {
    json encoded_tasks = json::array();
    for (const auto& task : tasks)
        encoded_tasks.push_back(task_to_json(task));
    return json{{"schema_version", TaskGraphProposal::SCHEMA_VERSION},
                {"tasks", std::move(encoded_tasks)},
                {"join", json{{"kind", std::string(to_string(join.kind))}}}};
}

struct CanonicalTask {
    TaskGraphTask            task;
    std::size_t              source_task_index = 0;
    std::vector<std::size_t> binding_source_indices;
    std::vector<std::size_t> dependency_source_indices;
};

CanonicalTask canonicalize_task(const ParsedTask& parsed) {
    CanonicalTask result;
    result.task.id = parsed.task.id;
    result.task.template_id = parsed.task.template_id;
    result.task.budget = parsed.task.budget;
    result.source_task_index = parsed.index;

    std::vector<std::pair<std::string, std::size_t>> dependencies;
    dependencies.reserve(parsed.task.depends_on.size());
    for (std::size_t index = 0; index < parsed.task.depends_on.size(); ++index) {
        dependencies.emplace_back(parsed.task.depends_on[index],
                                  parsed.dependency_source_indices.at(index));
    }
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (auto& [dependency, source_index] : dependencies) {
        result.task.depends_on.push_back(std::move(dependency));
        result.dependency_source_indices.push_back(source_index);
    }

    std::vector<std::pair<TaskGraphInputBinding, std::size_t>> bindings;
    bindings.reserve(parsed.task.input_bindings.size());
    for (std::size_t index = 0; index < parsed.task.input_bindings.size(); ++index) {
        bindings.emplace_back(parsed.task.input_bindings[index],
                              parsed.binding_source_indices.at(index));
    }
    std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
        return binding_less(lhs.first, rhs.first);
    });
    for (auto& [binding, source_index] : bindings) {
        result.task.input_bindings.push_back(std::move(binding));
        result.binding_source_indices.push_back(source_index);
    }
    return result;
}

void append_source_map_entry(std::vector<SourceMapEntry>&     result,
                             std::string                       canonical_pointer,
                             std::string_view                  source_pointer,
                             std::string_view                  source_id,
                             const std::vector<SourceMapEntry>& source_map) {
    result.push_back(
        {std::move(canonical_pointer), map_source_coordinate(source_id, source_map, source_pointer)});
}

std::vector<SourceMapEntry> canonical_source_map(
    const json&                         source_document,
    std::string_view                    source_id,
    const std::vector<SourceMapEntry>& source_map,
    const std::vector<CanonicalTask>&  tasks) {
    std::size_t entry_count = 5;
    for (const auto& task : tasks) {
        entry_count += 10 + task.dependency_source_indices.size() +
                       7 * task.binding_source_indices.size();
    }
    std::vector<SourceMapEntry> result;
    result.reserve(entry_count);
    append_source_map_entry(result, "", "", source_id, source_map);
    append_source_map_entry(result, "/schema_version", "/schema_version", source_id, source_map);
    append_source_map_entry(result, "/tasks", "/tasks", source_id, source_map);
    append_source_map_entry(result, "/join", "/join", source_id, source_map);
    append_source_map_entry(result, "/join/kind", "/join/kind", source_id, source_map);

    constexpr std::array<std::string_view, 4> kBudgetFields = {
        "wall_time_ms", "model_tokens", "monetary_microunits", "output_bytes"};
    for (std::size_t canonical_index = 0; canonical_index < tasks.size(); ++canonical_index) {
        const auto canonical_task_pointer = index_pointer("/tasks", canonical_index);
        const auto source_task_pointer = index_pointer("/tasks", tasks[canonical_index].source_task_index);
        const auto& source_task =
            source_document.at("tasks").at(tasks[canonical_index].source_task_index);
        append_source_map_entry(result, canonical_task_pointer, source_task_pointer, source_id, source_map);
        for (const auto field : {"id", "template", "input_bindings", "depends_on", "budget"}) {
            append_source_map_entry(result, child_pointer(canonical_task_pointer, field),
                                    child_pointer(source_task_pointer, field), source_id, source_map);
        }
        for (const auto field : kBudgetFields) {
            const auto source_budget_pointer = child_pointer(source_task_pointer, "budget");
            const auto source_field_pointer =
                source_task.at("budget").contains(std::string(field))
                    ? child_pointer(source_budget_pointer, field)
                    : source_budget_pointer;
            append_source_map_entry(result,
                                    child_pointer(child_pointer(canonical_task_pointer, "budget"), field),
                                    source_field_pointer, source_id, source_map);
        }
        for (std::size_t dependency_index = 0;
             dependency_index < tasks[canonical_index].dependency_source_indices.size();
             ++dependency_index) {
            append_source_map_entry(
                result,
                index_pointer(child_pointer(canonical_task_pointer, "depends_on"), dependency_index),
                index_pointer(child_pointer(source_task_pointer, "depends_on"),
                              tasks[canonical_index].dependency_source_indices[dependency_index]),
                source_id, source_map);
        }
        for (std::size_t binding_index = 0;
             binding_index < tasks[canonical_index].binding_source_indices.size(); ++binding_index) {
            const auto canonical_binding_pointer =
                index_pointer(child_pointer(canonical_task_pointer, "input_bindings"), binding_index);
            const auto source_binding_pointer =
                index_pointer(child_pointer(source_task_pointer, "input_bindings"),
                              tasks[canonical_index].binding_source_indices[binding_index]);
            append_source_map_entry(result, canonical_binding_pointer, source_binding_pointer, source_id,
                                    source_map);
            append_source_map_entry(result, child_pointer(canonical_binding_pointer, "from"),
                                    child_pointer(source_binding_pointer, "from"), source_id, source_map);
            append_source_map_entry(result, child_pointer(canonical_binding_pointer, "to"),
                                    child_pointer(source_binding_pointer, "to"), source_id, source_map);
            for (const auto field : {"task", "artifact", "field"}) {
                append_source_map_entry(
                    result, child_pointer(child_pointer(canonical_binding_pointer, "from"), field),
                    child_pointer(child_pointer(source_binding_pointer, "from"), field), source_id,
                    source_map);
            }
            append_source_map_entry(result, child_pointer(child_pointer(canonical_binding_pointer, "to"), "field"),
                                    child_pointer(child_pointer(source_binding_pointer, "to"), "field"),
                                    source_id, source_map);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.generated_pointer, lhs.authored.source_id, lhs.authored.json_pointer) <
               std::tie(rhs.generated_pointer, rhs.authored.source_id, rhs.authored.json_pointer);
    });
    return result;
}

void validate_options(TaskGraphProposalOptions& options) {
    detail::validate_token(options.source_id, "Task graph proposal source_id");
    if (options.limits.max_tasks == 0)
        throw std::invalid_argument("Task graph proposal max_tasks must be positive");
    if (options.limits.max_depth == 0)
        throw std::invalid_argument("Task graph proposal max_depth must be positive");

    std::set<std::string> templates;
    for (auto& contract : options.template_allowlist) {
        detail::validate_token(contract.template_id, "Task graph template_id");
        if (!detail::is_sha256_identity(contract.content_identity)) {
            throw std::invalid_argument(
                "Task graph template content_identity must be a sha256 identity");
        }
        if (!templates.insert(contract.template_id).second) {
            throw std::invalid_argument(
                "Task graph template allow-list contains duplicate template_id");
        }
        if (!contract.child_binding.empty())
            detail::validate_token(contract.child_binding, "Task graph template child_binding");
        if (!contract.executable_identity.empty())
            detail::validate_token(contract.executable_identity,
                                   "Task graph template executable_identity");
        if (!contract.kind.empty())
            detail::validate_token(contract.kind, "Task graph template kind");
        std::set<std::string> capabilities;
        for (const auto& capability : contract.capabilities) {
            detail::validate_token(capability, "Task graph template capability");
            if (!capabilities.insert(capability).second)
                throw std::invalid_argument("Task graph template capabilities must be unique");
        }
        std::set<std::string> effects;
        for (const auto& effect : contract.effects) {
            detail::validate_token(effect, "Task graph template effect");
            if (!effects.insert(effect).second)
                throw std::invalid_argument("Task graph template effects must be unique");
        }
        std::set<std::string> inputs;
        for (const auto& field : contract.input_fields) {
            detail::validate_json_pointer(field);
            if (field.empty())
                throw std::invalid_argument("Task graph template input field must not be empty");
            if (!inputs.insert(field).second) {
                throw std::invalid_argument("Task graph template input fields must be unique");
            }
        }
        std::set<std::string> artifacts;
        for (const auto& artifact : contract.output_artifacts) {
            detail::validate_token(artifact.artifact, "Task graph output artifact");
            if (!artifacts.insert(artifact.artifact).second) {
                throw std::invalid_argument("Task graph template output artifacts must be unique");
            }
            std::set<std::string> fields;
            for (const auto& field : artifact.fields) {
                detail::validate_json_pointer(field);
                if (field.empty()) {
                    throw std::invalid_argument("Task graph template output field must not be empty");
                }
                if (!fields.insert(field).second) {
                    throw std::invalid_argument("Task graph template output fields must be unique");
                }
            }
        }
    }

    std::set<std::string> source_pointers;
    for (const auto& entry : options.source_map) {
        detail::validate_json_pointer(entry.generated_pointer);
        detail::validate_token(entry.authored.source_id, "Task graph source-map source_id");
        detail::validate_json_pointer(entry.authored.json_pointer);
        if (!source_pointers.insert(entry.generated_pointer).second) {
            throw std::invalid_argument(
                "Task graph proposal source map has duplicate generated pointer");
        }
    }
    std::sort(options.source_map.begin(), options.source_map.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.generated_pointer, lhs.authored.source_id,
                                  lhs.authored.json_pointer) <
                         std::tie(rhs.generated_pointer, rhs.authored.source_id,
                                  rhs.authored.json_pointer);
              });
}

}  // namespace

struct TaskGraphProposal::Impl {
    std::string                   source_id;
    std::vector<SourceMapEntry>   source_map;
    std::vector<TaskGraphTask>    tasks;
    TaskGraphJoinPolicy           join;
    json                          document;
    std::string                   id;
    std::string                   canonical_bytes;
};

TaskGraphProposalError::TaskGraphProposalError(std::vector<Diagnostic> diagnostics)
    : std::runtime_error(diagnostic_message(diagnostics.size())), diagnostics_(std::move(diagnostics)) {}

const std::vector<Diagnostic>& TaskGraphProposalError::diagnostics() const noexcept {
    return diagnostics_;
}

std::string_view to_string(TaskGraphJoinKind kind) noexcept {
    switch (kind) {
    case TaskGraphJoinKind::All:
        return "all";
    }
    return "unknown";
}

TaskGraphJoinKind task_graph_join_kind_from_string(std::string_view value) {
    if (value == "all") return TaskGraphJoinKind::All;
    throw std::invalid_argument("Unknown task graph join kind: " + std::string(value));
}

TaskGraphProposal::TaskGraphProposal(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
TaskGraphProposal::~TaskGraphProposal() = default;

TaskGraphProposal TaskGraphProposal::parse(const json& proposal, TaskGraphProposalOptions options) {
    validate_options(options);
    const auto document = detail::owned_json_copy(proposal);
    DiagnosticAccumulator diagnostics(options.source_id, options.source_map);

    if (!document.is_object()) {
        diagnostics.add("", "P_PROPOSAL_TYPE", "TaskGraphProposal must be an object",
                        json{{"expected", "object"}});
        diagnostics.throw_error();
    }
    add_unknown_fields(diagnostics, document, "", {"schema_version", "tasks", "join"});

    if (!document.contains("schema_version")) {
        diagnostics.add("/schema_version", "P_PROPOSAL_REQUIRED_FIELD",
                        "TaskGraphProposal field is required", json{{"field", "schema_version"}});
    } else if (const auto version = unsigned_integer(document["schema_version"])) {
        if (*version != SCHEMA_VERSION) {
            diagnostics.add("/schema_version", "P_PROPOSAL_SCHEMA_VERSION",
                            "TaskGraphProposal schema_version is unsupported",
                            json{{"supported", SCHEMA_VERSION}, {"actual", *version}});
        }
    } else {
        diagnostics.add("/schema_version", "P_PROPOSAL_TYPE",
                        "TaskGraphProposal schema_version must be an unsigned integer",
                        json{{"expected", "unsigned integer"}});
    }

    std::vector<ParsedTask> parsed_tasks;
    if (!document.contains("tasks")) {
        diagnostics.add("/tasks", "P_PROPOSAL_REQUIRED_FIELD", "TaskGraphProposal field is required",
                        json{{"field", "tasks"}});
    } else if (!document["tasks"].is_array()) {
        diagnostics.add("/tasks", "P_PROPOSAL_TYPE", "TaskGraphProposal tasks must be an array",
                        json{{"expected", "array"}});
    } else {
        if (document["tasks"].empty()) {
            diagnostics.add("/tasks", "P_PROPOSAL_EMPTY_TASKS",
                            "TaskGraphProposal must declare at least one task");
        }
        if (document["tasks"].size() > options.limits.max_tasks) {
            diagnostics.add("/tasks", "P_PROPOSAL_LIMIT_TASKS",
                            "TaskGraphProposal task count exceeds the proposal ceiling",
                            json{{"requested", document["tasks"].size()},
                                 {"ceiling", options.limits.max_tasks}});
        }
        std::size_t index = 0;
        for (const auto& task : document["tasks"]) {
            parsed_tasks.push_back(parse_task(diagnostics, task, index));
            ++index;
        }
    }

    auto parsed_join = std::optional<TaskGraphJoinPolicy>{};
    if (!document.contains("join")) {
        diagnostics.add("/join", "P_PROPOSAL_REQUIRED_FIELD", "TaskGraphProposal field is required",
                        json{{"field", "join"}});
    } else {
        parsed_join = parse_join(diagnostics, document["join"], "/join");
    }

    std::map<std::string, std::size_t, std::less<>> task_index;
    for (const auto& parsed : parsed_tasks) {
        if (!parsed.id_valid) continue;
        const auto [found, inserted] = task_index.emplace(parsed.task.id, parsed.index);
        if (!inserted) {
            diagnostics.add(child_pointer(index_pointer("/tasks", parsed.index), "id"),
                            "P_PROPOSAL_DUPLICATE_TASK", "Task IDs must be unique",
                            json{{"id", parsed.task.id},
                                 {"first_index", found->second}, {"duplicate_index", parsed.index}});
        }
    }

    std::map<std::string, const TaskGraphTemplateContract*, std::less<>> templates;
    for (const auto& contract : options.template_allowlist)
        templates.emplace(contract.template_id, &contract);
    for (auto& parsed : parsed_tasks) {
        if (!parsed.template_valid) continue;
        const auto found = templates.find(parsed.task.template_id);
        if (found == templates.end()) {
            diagnostics.add(child_pointer(index_pointer("/tasks", parsed.index), "template"),
                            "P_PROPOSAL_TEMPLATE", "Task selects a template outside the allow-list",
                            json{{"template", parsed.task.template_id}});
        } else {
            parsed.template_contract = found->second;
        }
    }

    std::size_t edge_count = 0;
    std::map<std::string, std::vector<std::string>, std::less<>> predecessors;
    for (const auto& parsed : parsed_tasks) {
        if (!parsed.id_valid) continue;
        const auto owner = task_index.find(parsed.task.id);
        if (owner == task_index.end() || owner->second != parsed.index) continue;
        auto& task_predecessors = predecessors[parsed.task.id];
        std::size_t dependency_index = 0;
        for (const auto& dependency : parsed.task.depends_on) {
            const auto pointer =
                index_pointer(child_pointer(index_pointer("/tasks", parsed.index), "depends_on"),
                              parsed.dependency_source_indices.at(dependency_index));
            const auto dependency_task = task_index.find(dependency);
            if (dependency_task == task_index.end()) {
                diagnostics.add(pointer, "P_PROPOSAL_DEPENDENCY", "Task dependency is not declared",
                                json{{"task", dependency}});
            } else if (dependency == parsed.task.id) {
                diagnostics.add(pointer, "P_PROPOSAL_SELF_DEPENDENCY", "Task may not depend on itself",
                                json{{"task", dependency}});
                task_predecessors.push_back(dependency);
                ++edge_count;
            } else {
                task_predecessors.push_back(dependency);
                ++edge_count;
            }
            ++dependency_index;
        }
    }
    if (edge_count > options.limits.max_edges) {
        diagnostics.add("/tasks", "P_PROPOSAL_LIMIT_EDGES",
                        "TaskGraphProposal dependency edge count exceeds the proposal ceiling",
                        json{{"requested", edge_count}, {"ceiling", options.limits.max_edges}});
    }

    for (const auto& parsed : parsed_tasks) {
        if (!parsed.id_valid) continue;
        const auto owner = task_index.find(parsed.task.id);
        if (owner == task_index.end() || owner->second != parsed.index) continue;
        std::set<std::string> target_fields;
        std::size_t           binding_index = 0;
        for (const auto& binding : parsed.task.input_bindings) {
            const auto pointer =
                index_pointer(child_pointer(index_pointer("/tasks", parsed.index), "input_bindings"),
                              parsed.binding_source_indices.at(binding_index));
            if (!target_fields.insert(binding.to.field).second) {
                diagnostics.add(child_pointer(child_pointer(pointer, "to"), "field"),
                                "P_PROPOSAL_DUPLICATE_TARGET",
                                "Each target input field may receive at most one binding",
                                json{{"field", binding.to.field}});
            }
            const auto producer = task_index.find(binding.from.task_id);
            if (producer == task_index.end()) {
                diagnostics.add(child_pointer(child_pointer(pointer, "from"), "task"),
                                "P_PROPOSAL_BINDING_PRODUCER",
                                "Input binding producer task is not declared",
                                json{{"task", binding.from.task_id}});
            } else {
                const auto& dependencies = parsed.task.depends_on;
                if (std::find(dependencies.begin(), dependencies.end(), binding.from.task_id) ==
                    dependencies.end()) {
                    diagnostics.add(child_pointer(child_pointer(pointer, "from"), "task"),
                                    "P_PROPOSAL_BINDING_DEPENDENCY",
                                    "Input binding producer must be a direct declared dependency",
                                    json{{"task", binding.from.task_id}});
                }
                const auto& producer_task = parsed_tasks[producer->second];
                if (producer_task.template_contract) {
                    const auto artifact = find_artifact(*producer_task.template_contract,
                                                        binding.from.artifact);
                    if (!artifact || !is_allowed_field(artifact->fields, binding.from.field)) {
                        diagnostics.add(child_pointer(child_pointer(pointer, "from"), "field"),
                                        "P_PROPOSAL_BINDING_OUTPUT",
                                        "Input binding source is not an output field of its template",
                                        json{{"task", binding.from.task_id},
                                             {"artifact", binding.from.artifact},
                                             {"field", binding.from.field}});
                    }
                }
            }
            if (parsed.template_contract &&
                !is_allowed_field(parsed.template_contract->input_fields, binding.to.field)) {
                diagnostics.add(child_pointer(child_pointer(pointer, "to"), "field"),
                                "P_PROPOSAL_BINDING_TARGET",
                                "Input binding target is not accepted by its template",
                                json{{"template", parsed.task.template_id}, {"field", binding.to.field}});
            }
            ++binding_index;
        }
        validate_budget_ceiling(diagnostics, parsed.task.budget,
                                options.limits.per_task_budget_ceiling,
                                child_pointer(index_pointer("/tasks", parsed.index), "budget"),
                                "proposal");
        if (parsed.template_contract) {
            validate_budget_ceiling(diagnostics, parsed.task.budget,
                                    parsed.template_contract->budget_ceiling,
                                    child_pointer(index_pointer("/tasks", parsed.index), "budget"),
                                    "template");
        }
    }
    validate_total_budget(diagnostics, sum_budgets(parsed_tasks), options.limits.total_budget_ceiling);

    std::map<std::string, std::uint64_t, std::less<>> indegree;
    std::map<std::string, std::vector<std::string>, std::less<>> successors;
    for (const auto& [id, _] : task_index)
        indegree.emplace(id, 0);
    for (const auto& [task, dependencies] : predecessors) {
        for (const auto& dependency : dependencies) {
            if (!task_index.contains(dependency)) continue;
            successors[dependency].push_back(task);
            ++indegree[task];
        }
    }
    for (auto& [_, values] : successors)
        std::sort(values.begin(), values.end());

    std::set<std::string> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.insert(id);
    }
    std::map<std::string, std::uint64_t, std::less<>> depth;
    for (const auto& [id, _] : indegree)
        depth.emplace(id, 1);
    std::vector<std::string> canonical_order;
    canonical_order.reserve(indegree.size());
    while (!ready.empty()) {
        const auto current = *ready.begin();
        ready.erase(ready.begin());
        canonical_order.push_back(current);
        for (const auto& successor : successors[current]) {
            depth[successor] = std::max(depth[successor], depth[current] + 1);
            auto& degree = indegree[successor];
            --degree;
            if (degree == 0) ready.insert(successor);
        }
    }
    if (canonical_order.size() != indegree.size()) {
        json cycle_tasks = json::array();
        for (const auto& [id, degree] : indegree) {
            if (degree != 0) cycle_tasks.push_back(id);
        }
        diagnostics.add("/tasks", "P_PROPOSAL_CYCLE", "TaskGraphProposal dependencies must be acyclic",
                        json{{"tasks", std::move(cycle_tasks)}});
    } else {
        std::uint64_t maximum_depth = 0;
        for (const auto& [_, value] : depth)
            maximum_depth = std::max(maximum_depth, value);
        if (maximum_depth > options.limits.max_depth) {
            diagnostics.add("/tasks", "P_PROPOSAL_LIMIT_DEPTH",
                            "TaskGraphProposal dependency depth exceeds the proposal ceiling",
                            json{{"requested", maximum_depth}, {"ceiling", options.limits.max_depth}});
        }
    }

    if (diagnostics.has_errors()) diagnostics.throw_error();

    std::vector<CanonicalTask> canonical_records;
    canonical_records.reserve(canonical_order.size());
    for (const auto& id : canonical_order)
        canonical_records.push_back(canonicalize_task(parsed_tasks.at(task_index.at(id))));

    auto result = std::make_shared<Impl>();
    result->source_id = std::move(options.source_id);
    result->source_map =
        canonical_source_map(document, result->source_id, options.source_map, canonical_records);
    result->tasks.reserve(canonical_records.size());
    for (auto& record : canonical_records)
        result->tasks.push_back(std::move(record.task));
    result->join = *parsed_join;
    result->document = proposal_to_json(result->tasks, result->join);
    result->canonical_bytes = detail::canonical_json_bytes(result->document);
    result->id = detail::sha256_identity("program-task-graph-proposal/v1", result->canonical_bytes);
    return TaskGraphProposal(std::move(result));
}

std::uint32_t TaskGraphProposal::schema_version() const noexcept { return SCHEMA_VERSION; }
const std::string& TaskGraphProposal::source_id() const noexcept { return impl_->source_id; }
const std::vector<SourceMapEntry>& TaskGraphProposal::source_map() const noexcept {
    return impl_->source_map;
}
const std::vector<TaskGraphTask>& TaskGraphProposal::tasks() const noexcept { return impl_->tasks; }
const TaskGraphJoinPolicy& TaskGraphProposal::join() const noexcept { return impl_->join; }
const std::string& TaskGraphProposal::id() const noexcept { return impl_->id; }
const std::string& TaskGraphProposal::proposal_hash() const noexcept { return impl_->id; }
json TaskGraphProposal::to_json() const { return detail::owned_json_copy(impl_->document); }
const std::string& TaskGraphProposal::serialize_canonical() const noexcept {
    return impl_->canonical_bytes;
}

}  // namespace neograph::program
