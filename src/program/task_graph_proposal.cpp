#include <neograph/program/task_graph_proposal.h>

#include "canonical_json.h"
#include "task_graph_proposal_detail.h"

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

using namespace detail::task_graph;

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

// Schema 2 (#412): a dimension the template can spend must be stated. A task that
// leaves it at 0 is counted as 0 by the aggregate but launched with the link's whole
// ceiling, so the aggregate would under-count what the planner can make the run spend.
void validate_explicit_budget(DiagnosticAccumulator& diagnostics,
                              const TaskGraphBudget& requested,
                              const TaskGraphBudget& template_ceiling,
                              std::string_view       pointer) {
    for (const auto& [resource, amount, maximum] : budget_dimensions(requested, template_ceiling)) {
        if (amount != 0 || maximum == 0) continue;
        diagnostics.add(child_pointer(pointer, resource), "P_PROPOSAL_BUDGET_ZERO",
                        "Schema 2 task budget must state a non-zero amount for every dimension "
                        "its template can spend",
                        json{{"resource", resource}, {"template_ceiling", maximum}});
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

bool multiply_without_overflow(std::uint64_t& amount, std::uint64_t factor) noexcept {
    if (factor != 0 && amount > std::numeric_limits<std::uint64_t>::max() / factor) return false;
    amount *= factor;
    return true;
}

// Each attempt has the task's whole budget, so a task counts once per attempt it may take.
BudgetSum sum_budgets(const std::vector<ParsedTask>& tasks) {
    BudgetSum result;
    for (const auto& parsed : tasks) {
        const auto attempts = static_cast<std::uint64_t>(std::max<std::uint32_t>(parsed.task.max_attempts, 1));
        auto reserve = [&](std::size_t dimension, std::uint64_t amount, std::uint64_t& total) {
            if (!multiply_without_overflow(amount, attempts) || !add_without_overflow(total, amount))
                result.overflow[dimension] = true;
        };
        reserve(0, parsed.task.budget.wall_time_ms, result.total.wall_time_ms);
        reserve(1, parsed.task.budget.model_tokens, result.total.model_tokens);
        reserve(2, parsed.task.budget.monetary_microunits, result.total.monetary_microunits);
        reserve(3, parsed.task.budget.output_bytes, result.total.output_bytes);
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
    json encoded{{"id", task.id},
                 {"template", task.template_id},
                 {"input_bindings", std::move(encoded_bindings)},
                 {"depends_on", std::move(dependencies)},
                 {"budget",
                  json{{"wall_time_ms", task.budget.wall_time_ms},
                       {"model_tokens", task.budget.model_tokens},
                       {"monetary_microunits", task.budget.monetary_microunits},
                       {"output_bytes", task.budget.output_bytes}}}};
    // Schema-2 members appear only when used, so a schema-1 task keeps its bytes.
    if (!task.literals.empty()) {
        auto literals = task.literals;
        std::sort(literals.begin(), literals.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.to.field < rhs.to.field;
        });
        json encoded_literals = json::array();
        for (const auto& literal : literals)
            encoded_literals.push_back(
                json{{"to", json{{"field", literal.to.field}}}, {"value", literal.value}});
        encoded["literals"] = std::move(encoded_literals);
    }
    if (task.max_attempts > 1) encoded["max_attempts"] = task.max_attempts;
    return encoded;
}

json proposal_to_json(const std::vector<TaskGraphTask>& tasks, const TaskGraphJoinPolicy& join,
                      std::uint32_t schema_version) {
    json encoded_tasks = json::array();
    for (const auto& task : tasks)
        encoded_tasks.push_back(task_to_json(task));
    return json{{"schema_version", schema_version},
                {"tasks", std::move(encoded_tasks)},
                {"join", json{{"kind", std::string(to_string(join.kind))}}}};
}

struct CanonicalTask {
    TaskGraphTask            task;
    std::size_t              source_task_index = 0;
    std::vector<std::size_t> binding_source_indices;
    std::vector<std::size_t> dependency_source_indices;
    std::vector<std::size_t> literal_source_indices;
};

CanonicalTask canonicalize_task(const ParsedTask& parsed) {
    CanonicalTask result;
    result.task.id = parsed.task.id;
    result.task.template_id = parsed.task.template_id;
    result.task.budget = parsed.task.budget;
    result.task.max_attempts = parsed.task.max_attempts;
    result.source_task_index = parsed.index;

    std::vector<std::pair<TaskGraphInputLiteral, std::size_t>> literals;
    for (std::size_t index = 0; index < parsed.task.literals.size(); ++index)
        literals.emplace_back(parsed.task.literals[index], parsed.literal_source_indices.at(index));
    std::sort(literals.begin(), literals.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first.to.field < rhs.first.to.field;
    });
    for (auto& [literal, source_index] : literals) {
        result.task.literals.push_back(std::move(literal));
        result.literal_source_indices.push_back(source_index);
    }

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
        entry_count += 12 + task.dependency_source_indices.size() +
                       7 * task.binding_source_indices.size() + 4 * task.literal_source_indices.size();
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
        if (!tasks[canonical_index].literal_source_indices.empty()) {
            append_source_map_entry(result, child_pointer(canonical_task_pointer, "literals"),
                                    child_pointer(source_task_pointer, "literals"), source_id,
                                    source_map);
            for (std::size_t literal_index = 0;
                 literal_index < tasks[canonical_index].literal_source_indices.size();
                 ++literal_index) {
                const auto canonical_literal = index_pointer(
                    child_pointer(canonical_task_pointer, "literals"), literal_index);
                const auto source_literal = index_pointer(
                    child_pointer(source_task_pointer, "literals"),
                    tasks[canonical_index].literal_source_indices[literal_index]);
                append_source_map_entry(result, canonical_literal, source_literal, source_id,
                                        source_map);
                append_source_map_entry(result, child_pointer(canonical_literal, "to"),
                                        child_pointer(source_literal, "to"), source_id, source_map);
                append_source_map_entry(result,
                                        child_pointer(child_pointer(canonical_literal, "to"), "field"),
                                        child_pointer(child_pointer(source_literal, "to"), "field"),
                                        source_id, source_map);
                append_source_map_entry(result, child_pointer(canonical_literal, "value"),
                                        child_pointer(source_literal, "value"), source_id,
                                        source_map);
            }
        }
        if (source_task.contains("max_attempts") && tasks[canonical_index].task.max_attempts > 1) {
            append_source_map_entry(result, child_pointer(canonical_task_pointer, "max_attempts"),
                                    child_pointer(source_task_pointer, "max_attempts"), source_id,
                                    source_map);
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
    if (options.limits.min_schema_version == 0 ||
        options.limits.min_schema_version > TaskGraphProposal::SCHEMA_VERSION_2)
        throw std::invalid_argument("Task graph proposal min_schema_version must be 1 or 2");
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
    std::uint32_t                 schema_version = SCHEMA_VERSION;
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

    std::uint32_t schema_version = SCHEMA_VERSION;
    if (!document.contains("schema_version")) {
        diagnostics.add("/schema_version", "P_PROPOSAL_REQUIRED_FIELD",
                        "TaskGraphProposal field is required", json{{"field", "schema_version"}});
    } else if (const auto version = unsigned_integer(document["schema_version"])) {
        if (*version != SCHEMA_VERSION && *version != SCHEMA_VERSION_2) {
            diagnostics.add("/schema_version", "P_PROPOSAL_SCHEMA_VERSION",
                            "TaskGraphProposal schema_version is unsupported",
                            json{{"supported", json::array({SCHEMA_VERSION, SCHEMA_VERSION_2})},
                                 {"actual", *version}});
        } else if (*version < options.limits.min_schema_version) {
            diagnostics.add("/schema_version", "P_PROPOSAL_SCHEMA_VERSION",
                            "TaskGraphProposal schema_version is below the host minimum",
                            json{{"minimum", options.limits.min_schema_version},
                                 {"actual", *version}});
        } else {
            schema_version = static_cast<std::uint32_t>(*version);
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
            parsed_tasks.push_back(parse_task(diagnostics, task, index, schema_version));
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

    std::uint64_t literal_bytes_total = 0;
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
        {
            const auto task_pointer = index_pointer("/tasks", parsed.index);
            std::size_t literal_index = 0;
            for (const auto& literal : parsed.task.literals) {
                const auto literal_pointer = index_pointer(
                    child_pointer(task_pointer, "literals"),
                    parsed.literal_source_indices.at(literal_index));
                const auto field_pointer = child_pointer(child_pointer(literal_pointer, "to"), "field");
                if (!target_fields.insert(literal.to.field).second) {
                    diagnostics.add(field_pointer, "P_PROPOSAL_LITERAL_DUPLICATE_TARGET",
                                    "Each target input field may receive at most one binding or literal",
                                    json{{"field", literal.to.field}});
                }
                if (parsed.template_contract &&
                    !is_allowed_field(parsed.template_contract->input_fields, literal.to.field)) {
                    diagnostics.add(field_pointer, "P_PROPOSAL_LITERAL_TARGET",
                                    "Input literal target is not accepted by its template",
                                    json{{"template", parsed.task.template_id},
                                         {"field", literal.to.field}});
                }
                const auto bytes = detail::canonical_json_bytes(literal.value).size();
                if (bytes > options.limits.max_literal_bytes) {
                    diagnostics.add(child_pointer(literal_pointer, "value"),
                                    "P_PROPOSAL_LITERAL_SIZE", "Input literal exceeds its size ceiling",
                                    json{{"requested", bytes},
                                         {"ceiling", options.limits.max_literal_bytes}});
                }
                literal_bytes_total += bytes;
                ++literal_index;
            }
            if (parsed.task.max_attempts == 0 ||
                parsed.task.max_attempts > std::max<std::uint32_t>(options.limits.max_attempts, 1)) {
                diagnostics.add(child_pointer(task_pointer, "max_attempts"), "P_PROPOSAL_ATTEMPTS",
                                "Task max_attempts is outside 1 to the proposal ceiling",
                                json{{"requested", parsed.task.max_attempts},
                                     {"ceiling", std::max<std::uint32_t>(options.limits.max_attempts, 1)}});
            }
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
            if (schema_version >= SCHEMA_VERSION_2) {
                validate_explicit_budget(diagnostics, parsed.task.budget,
                                         parsed.template_contract->budget_ceiling,
                                         child_pointer(index_pointer("/tasks", parsed.index),
                                                       "budget"));
            }
        }
    }
    if (literal_bytes_total > options.limits.max_total_literal_bytes) {
        diagnostics.add("/tasks", "P_PROPOSAL_LITERAL_SIZE",
                        "Input literals exceed their combined size ceiling",
                        json{{"requested", literal_bytes_total},
                             {"ceiling", options.limits.max_total_literal_bytes}});
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
    result->schema_version = schema_version;
    result->document = proposal_to_json(result->tasks, result->join, schema_version);
    result->canonical_bytes = detail::canonical_json_bytes(result->document);
    result->id = detail::sha256_identity("program-task-graph-proposal/v1", result->canonical_bytes);
    return TaskGraphProposal(std::move(result));
}

std::uint32_t TaskGraphProposal::schema_version() const noexcept { return impl_->schema_version; }
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
