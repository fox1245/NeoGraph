#include <neograph/program/admission.h>
#include <neograph/program/compiler.h>

#include "canonical_json.h"
#include "registry_access.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

constexpr std::array<std::string_view, 9> kBudgetResources = {
    "wall_time_ms",         "model_tokens",           "monetary_microunits",
    "max_concurrency",      "max_program_operations", "max_core_steps",
    "max_dynamic_compiles", "max_child_depth",        "max_total_children",
};

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

std::string core_pointer(std::string_view pointer) {
    if (pointer.empty()) return "/root/definition";
    return std::string("/root/definition") + std::string(pointer);
}

bool is_ancestor_pointer(std::string_view ancestor, std::string_view pointer) {
    if (ancestor == pointer) return true;
    if (ancestor.empty()) return !pointer.empty() && pointer.front() == '/';
    return pointer.size() > ancestor.size() && pointer.starts_with(ancestor) &&
           pointer[ancestor.size()] == '/';
}

json identity_json(const ExecutableIdentity& identity) {
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

bool identity_less(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
    return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                      lhs.implementation_digest} < std::tuple{to_string(rhs.kind), rhs.name,
                                                              rhs.semantic_version,
                                                              rhs.implementation_digest};
}

bool has_control_character(std::string_view value) noexcept {
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        if (character < 0x20 || character == 0x7f) return true;
        if (character == 0xc2 && index + 1 < value.size()) {
            const auto continuation = static_cast<unsigned char>(value[index + 1]);
            if (continuation >= 0x80 && continuation <= 0x9f) return true;
        }
    }
    return false;
}

std::optional<std::uint64_t> unsigned_integer(const json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (!value.is_number_integer()) return std::nullopt;
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) return std::nullopt;
    return static_cast<std::uint64_t>(signed_value);
}

struct PendingDiagnostic {
    Diagnostic  diagnostic;
    std::string generated_pointer;
    std::string canonical_witness;
};

class DiagnosticAccumulator {
public:
    explicit DiagnosticAccumulator(const ProgramSource& source)
        : source_id_(source.source_id()), mappings_(source.source_map()) {
        std::sort(mappings_.begin(), mappings_.end(), [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.generated_pointer, lhs.authored.source_id,
                            lhs.authored.json_pointer) < std::tie(rhs.generated_pointer,
                                                                  rhs.authored.source_id,
                                                                  rhs.authored.json_pointer);
        });
    }

    void add(CompilePhase       phase,
             std::string        code,
             DiagnosticSeverity severity,
             std::string        generated_pointer,
             std::string        message,
             json               witness = json::object()) {
        PendingDiagnostic pending;
        pending.generated_pointer   = std::move(generated_pointer);
        pending.diagnostic.phase    = phase;
        pending.diagnostic.code     = std::move(code);
        pending.diagnostic.severity = severity;
        pending.diagnostic.primary  = map(pending.generated_pointer);
        pending.diagnostic.message  = std::move(message);
        pending.diagnostic.witness  = detail::owned_json_copy(witness);
        pending.canonical_witness   = detail::canonical_json_bytes(pending.diagnostic.witness);
        diagnostics_.push_back(std::move(pending));
    }

    bool has_errors() const noexcept {
        return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const auto& pending) {
            return pending.diagnostic.severity == DiagnosticSeverity::Error;
        });
    }

    [[noreturn]] void throw_error() {
        sort();
        std::vector<Diagnostic> result;
        result.reserve(diagnostics_.size());
        for (auto& pending : diagnostics_)
            result.push_back(std::move(pending.diagnostic));
        throw ProgramCompileError(std::move(result));
    }

    std::vector<Diagnostic> release_warnings() {
        sort();
        std::vector<Diagnostic> result;
        result.reserve(diagnostics_.size());
        for (auto& pending : diagnostics_)
            result.push_back(std::move(pending.diagnostic));
        diagnostics_.clear();
        return result;
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
        const SourceMapEntry* best = nullptr;
        for (const auto& entry : mappings_) {
            if (!is_ancestor_pointer(entry.generated_pointer, generated_pointer)) continue;
            if (!best || entry.generated_pointer.size() > best->generated_pointer.size()) {
                best = &entry;
            }
        }
        if (!best)
            return SourceCoordinate{source_id_, std::string(generated_pointer), std::nullopt};
        SourceCoordinate result = best->authored;
        if (best->generated_pointer == generated_pointer) return result;
        result.span.reset();
        result.json_pointer += generated_pointer.substr(best->generated_pointer.size());
        return result;
    }

    std::string                    source_id_;
    std::vector<SourceMapEntry>    mappings_;
    std::vector<PendingDiagnostic> diagnostics_;
};

void add_unknown_fields(DiagnosticAccumulator&                  diagnostics,
                        const json&                             value,
                        std::string_view                        pointer,
                        std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) return;
    for (const auto& [field, ignored] : value.items()) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), field) != allowed.end()) continue;
        diagnostics.add(CompilePhase::Schema, "P_SCHEMA_UNKNOWN_FIELD", DiagnosticSeverity::Error,
                        child_pointer(pointer, field), "Unknown field in closed Program-v1 object",
                        json{{"field", field}});
    }
}

void add_required(DiagnosticAccumulator& diagnostics,
                  std::string_view       parent,
                  std::string_view       field) {
    diagnostics.add(CompilePhase::Schema, "P_SCHEMA_REQUIRED", DiagnosticSeverity::Error,
                    child_pointer(parent, field), "Required Program-v1 field is absent",
                    json{{"field", field}});
}

std::string json_type_name(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "unknown";
}

void add_type(DiagnosticAccumulator& diagnostics,
              std::string_view       pointer,
              std::string_view       expected,
              const json&            actual) {
    diagnostics.add(CompilePhase::Schema, "P_SCHEMA_TYPE", DiagnosticSeverity::Error,
                    std::string(pointer), "Program-v1 field has the wrong JSON type",
                    json{{"expected", expected}, {"actual", json_type_name(actual)}});
}

bool valid_nonempty_utf8(std::string_view value) {
    if (value.empty()) return false;
    try {
        detail::validate_utf8(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct ParsedProgram {
    ContractRecord                 input_contract;
    ContractRecord                 output_contract;
    std::string                    root_name;
    json                           core_definition;
    std::vector<BudgetRequirement> budgets;
};

void validate_program_version(const ProgramSource&   source,
                              const json&            document,
                              DiagnosticAccumulator& diagnostics) {
    constexpr std::string_view pointer = "/program_schema_version";
    if (source.schema_version() != ProgramCompiler::PROGRAM_SCHEMA_VERSION) {
        diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                        std::string(pointer), "Program source metadata requires schema version 1",
                        json{{"source_schema_version", source.schema_version()},
                             {"supported", ProgramCompiler::PROGRAM_SCHEMA_VERSION}});
    }
    if (!document.contains("program_schema_version")) {
        diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                        std::string(pointer), "Program document requires explicit schema version 1",
                        json{{"supported", ProgramCompiler::PROGRAM_SCHEMA_VERSION}});
        return;
    }
    const auto encoded = unsigned_integer(document["program_schema_version"]);
    if (!encoded || *encoded != ProgramCompiler::PROGRAM_SCHEMA_VERSION) {
        diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                        std::string(pointer), "Program document requires schema version 1",
                        json{{"supported", ProgramCompiler::PROGRAM_SCHEMA_VERSION}});
    }
}

void validate_contract(const json&            document,
                       std::string_view       field,
                       ContractRecord&        output,
                       DiagnosticAccumulator& diagnostics) {
    const auto pointer = child_pointer("", field);
    if (!document.contains(std::string(field))) {
        add_required(diagnostics, "", field);
        return;
    }
    const auto& value = document[std::string(field)];
    if (!value.is_object()) {
        add_type(diagnostics, pointer, "object", value);
        return;
    }
    add_unknown_fields(diagnostics, value, pointer, {"schema_version", "schema"});
    if (!value.contains("schema_version")) {
        add_required(diagnostics, pointer, "schema_version");
    } else {
        const auto version = unsigned_integer(value["schema_version"]);
        if (!version) {
            add_type(diagnostics, child_pointer(pointer, "schema_version"), "uint64",
                     value["schema_version"]);
        } else if (*version != 1) {
            diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                            child_pointer(pointer, "schema_version"),
                            "Program contract requires schema version 1",
                            json{{"supported", 1}, {"actual", *version}});
        } else
            output.schema_version = 1;
    }
    if (!value.contains("schema")) {
        add_required(diagnostics, pointer, "schema");
    } else if (!value["schema"].is_object()) {
        add_type(diagnostics, child_pointer(pointer, "schema"), "object", value["schema"]);
    } else
        output.schema = detail::owned_json_copy(value["schema"]);
}

void validate_root(const json&            document,
                   ParsedProgram&         output,
                   DiagnosticAccumulator& diagnostics) {
    constexpr std::string_view pointer = "/root";
    if (!document.contains("root")) {
        add_required(diagnostics, "", "root");
        return;
    }
    const auto& root = document["root"];
    if (!root.is_object()) {
        add_type(diagnostics, pointer, "object", root);
        return;
    }
    add_unknown_fields(diagnostics, root, pointer, {"op", "name", "definition"});
    if (!root.contains("op")) {
        diagnostics.add(CompilePhase::Normalize, "P_ROOT_OPERATION", DiagnosticSeverity::Error,
                        "/root/op", "Program-v1 requires one call_core root",
                        json{{"required", "call_core"}});
    } else if (!root["op"].is_string()) {
        add_type(diagnostics, "/root/op", "string", root["op"]);
    } else if (root["op"].get<std::string>() != "call_core") {
        diagnostics.add(CompilePhase::Normalize, "P_ROOT_OPERATION", DiagnosticSeverity::Error,
                        "/root/op", "Program-v1 root operation must be call_core",
                        json{{"required", "call_core"}, {"actual", root["op"]}});
    }
    if (!root.contains("name")) {
        diagnostics.add(CompilePhase::Normalize, "P_ROOT_NAME", DiagnosticSeverity::Error,
                        "/root/name", "Program-v1 root name is required", json::object());
    } else if (!root["name"].is_string()) {
        add_type(diagnostics, "/root/name", "string", root["name"]);
    } else {
        output.root_name = root["name"].get<std::string>();
        if (!valid_nonempty_utf8(output.root_name)) {
            diagnostics.add(CompilePhase::Normalize, "P_ROOT_NAME", DiagnosticSeverity::Error,
                            "/root/name", "Program-v1 root name must be nonempty UTF-8",
                            json::object());
        }
    }
    if (!root.contains("definition")) {
        add_required(diagnostics, pointer, "definition");
        return;
    }
    if (!root["definition"].is_object()) {
        add_type(diagnostics, "/root/definition", "object", root["definition"]);
        return;
    }
    output.core_definition = detail::owned_json_copy(root["definition"]);
    const auto& definition = output.core_definition;
    if (!definition.contains("schema_version")) {
        diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                        "/root/definition/schema_version",
                        "Embedded Core definition requires explicit schema version 1",
                        json{{"supported", 1}});
    } else {
        const auto version = unsigned_integer(definition["schema_version"]);
        if (!version || *version != 1) {
            diagnostics.add(CompilePhase::Schema, "P_SCHEMA_VERSION", DiagnosticSeverity::Error,
                            "/root/definition/schema_version",
                            "Embedded Core definition requires schema version 1",
                            json{{"supported", 1}});
        }
    }
    if (!definition.contains("name") || !definition["name"].is_string()) {
        diagnostics.add(CompilePhase::Normalize, "P_ROOT_NAME", DiagnosticSeverity::Error,
                        "/root/definition/name",
                        "Embedded Core definition requires an explicit string name",
                        json::object());
    } else {
        const auto core_name = definition["name"].get<std::string>();
        if (!valid_nonempty_utf8(core_name) || core_name != output.root_name) {
            diagnostics.add(CompilePhase::Normalize, "P_ROOT_NAME", DiagnosticSeverity::Error,
                            "/root/definition/name",
                            "Root name must equal the embedded Core topology name",
                            json{{"root_name", output.root_name}, {"core_name", core_name}});
        }
    }
    if (!definition.contains("nodes")) {
        add_required(diagnostics, "/root/definition", "nodes");
    } else if (!definition["nodes"].is_object()) {
        add_type(diagnostics, "/root/definition/nodes", "nonempty object", definition["nodes"]);
    } else if (definition["nodes"].empty()) {
        diagnostics.add(CompilePhase::Normalize, "P_ROOT_OPERATION", DiagnosticSeverity::Error,
                        "/root/definition/nodes",
                        "A call_core root requires at least one Core node", json::object());
    }
}

void validate_budgets(const json&            document,
                      ParsedProgram&         output,
                      DiagnosticAccumulator& diagnostics) {
    constexpr std::string_view pointer = "/declared_budget_requirements";
    if (!document.contains("declared_budget_requirements")) {
        diagnostics.add(CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                        std::string(pointer), "Program-v1 requires all nine finite budget records",
                        json::object());
        return;
    }
    const auto& values = document["declared_budget_requirements"];
    if (!values.is_array()) {
        add_type(diagnostics, pointer, "array", values);
        return;
    }
    std::map<std::string, std::size_t> seen;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto  item_pointer = child_pointer(pointer, std::to_string(index));
        const auto& value        = values[index];
        if (!value.is_object()) {
            add_type(diagnostics, item_pointer, "object", value);
            continue;
        }
        add_unknown_fields(diagnostics, value, item_pointer, {"resource", "minimum", "maximum"});
        bool valid = true;
        for (const auto field : {"resource", "minimum", "maximum"}) {
            if (!value.contains(field)) {
                diagnostics.add(CompilePhase::Normalize, "P_BUDGET_INVALID",
                                DiagnosticSeverity::Error, child_pointer(item_pointer, field),
                                "Budget record is missing a required field",
                                json{{"field", field}});
                valid = false;
            }
        }
        if (!valid) continue;
        if (!value["resource"].is_string()) {
            add_type(diagnostics, child_pointer(item_pointer, "resource"), "string",
                     value["resource"]);
            continue;
        }
        const auto resource = value["resource"].get<std::string>();
        const auto minimum  = unsigned_integer(value["minimum"]);
        const auto maximum  = unsigned_integer(value["maximum"]);
        if (!minimum)
            add_type(diagnostics, child_pointer(item_pointer, "minimum"), "uint64",
                     value["minimum"]);
        if (!maximum)
            add_type(diagnostics, child_pointer(item_pointer, "maximum"), "uint64",
                     value["maximum"]);
        if (!minimum || !maximum) continue;
        if (std::find(kBudgetResources.begin(), kBudgetResources.end(), resource) ==
            kBudgetResources.end()) {
            diagnostics.add(CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                            child_pointer(item_pointer, "resource"),
                            "Budget resource is not part of the closed Program-v1 set",
                            json{{"resource", resource}});
            continue;
        }
        if (!seen.emplace(resource, index).second) {
            diagnostics.add(CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                            child_pointer(item_pointer, "resource"),
                            "Budget resource occurs more than once", json{{"resource", resource}});
            continue;
        }
        if (*maximum < *minimum) {
            diagnostics.add(
                CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                item_pointer, "Budget maximum must be greater than or equal to minimum",
                json{{"resource", resource}, {"minimum", *minimum}, {"maximum", *maximum}});
            continue;
        }
        bool structural_valid = true;
        if (resource == "max_program_operations")
            structural_valid = *minimum == 1 && *maximum == 1;
        else if (resource == "max_concurrency" || resource == "max_core_steps")
            structural_valid = *minimum >= 1;
        else if (resource == "max_dynamic_compiles" || resource == "max_child_depth" ||
                 resource == "max_total_children")
            structural_valid = *minimum == 0 && *maximum == 0;
        if (!structural_valid) {
            diagnostics.add(
                CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                item_pointer, "Budget record violates the single-root Program-v1 structural floor",
                json{{"resource", resource}, {"minimum", *minimum}, {"maximum", *maximum}});
            continue;
        }
        output.budgets.push_back({resource, *minimum, *maximum});
    }
    for (const auto resource : kBudgetResources) {
        if (!seen.contains(std::string(resource))) {
            diagnostics.add(CompilePhase::Normalize, "P_BUDGET_INVALID", DiagnosticSeverity::Error,
                            std::string(pointer), "Budget closure is missing a required resource",
                            json{{"resource", std::string(resource)}});
        }
    }
    std::sort(output.budgets.begin(), output.budgets.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.resource < rhs.resource; });
}

ParsedProgram parse_program(const ProgramSource&   source,
                            const json&            document,
                            DiagnosticAccumulator& diagnostics) {
    ParsedProgram result;
    if (!document.is_object()) {
        add_type(diagnostics, "", "object", document);
        return result;
    }
    add_unknown_fields(diagnostics, document, "",
                       {"program_schema_version", "input_contract", "output_contract", "root",
                        "declared_budget_requirements"});
    validate_program_version(source, document, diagnostics);
    validate_contract(document, "input_contract", result.input_contract, diagnostics);
    validate_contract(document, "output_contract", result.output_contract, diagnostics);
    validate_root(document, result, diagnostics);
    validate_budgets(document, result, diagnostics);
    return result;
}

json with_core_code(const json& witness, std::string_view core_code) {
    if (witness.is_object()) {
        json result         = detail::owned_json_copy(witness);
        result["core_code"] = std::string(core_code);
        return result;
    }
    return json{{"core_code", std::string(core_code)}, {"core_witness", witness}};
}

void add_core_parse_diagnostics(const graph::ParseReport& report,
                                DiagnosticAccumulator&    diagnostics) {
    for (const auto& core : report.diagnostics) {
        diagnostics.add(CompilePhase::CoreParse, "P_CORE_" + core.code, DiagnosticSeverity::Error,
                        core_pointer(core.json_pointer), core.message,
                        with_core_code(core.witness, core.code));
    }
}

void add_roundtrip_diagnostics(const graph::RoundTripReport& report,
                               DiagnosticAccumulator&        diagnostics) {
    for (const auto& core : report.diagnostics) {
        diagnostics.add(CompilePhase::Seal, "P_CORE_ROUNDTRIP", DiagnosticSeverity::Error,
                        core_pointer(core.json_pointer), core.message,
                        with_core_code(core.witness, core.code));
    }
}

void add_validation_diagnostics(const graph::ValidationReport& report,
                                DiagnosticAccumulator&         diagnostics) {
    for (const auto& core : report.diagnostics) {
        const auto severity =
            core.severity == "warning" ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
        diagnostics.add(CompilePhase::CoreValidate, "P_CORE_" + core.code, severity,
                        core_pointer(core.json_pointer), core.message,
                        with_core_code(core.witness, core.code));
    }
}

struct DirectReference {
    ExecutableKind kind;
    std::string    name;
    std::string    pointer;
};

std::vector<DirectReference> direct_references(const graph::TopologySpec& topology) {
    std::vector<DirectReference> references;
    for (const auto& [name, definition] : topology.node_defs) {
        references.push_back({ExecutableKind::Node, definition.at("type").get<std::string>(),
                              "/root/definition/nodes/" + escape_pointer_segment(name) + "/type"});
    }
    for (const auto& channel : topology.channel_defs) {
        references.push_back(
            {ExecutableKind::Reducer, channel.reducer_name,
             "/root/definition/channels/" + escape_pointer_segment(channel.name) + "/reducer"});
    }
    for (std::size_t index = 0; index < topology.conditional_edges.size(); ++index) {
        references.push_back(
            {ExecutableKind::Condition, topology.conditional_edges[index].condition,
             "/root/definition/conditional_edges/" + std::to_string(index) + "/condition"});
    }
    std::sort(references.begin(), references.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.kind, lhs.name, lhs.pointer) <
               std::tie(rhs.kind, rhs.name, rhs.pointer);
    });
    return references;
}

struct ClosureResult {
    std::vector<ExecutableIdentity> identities;
    CapabilityEffectClosure         closure;
};

ClosureResult resolve_closure(const RegistrySnapshot&    registry,
                              const graph::TopologySpec& topology,
                              DiagnosticAccumulator&     diagnostics) {
    struct Work {
        ExecutableIdentity                expected;
        std::string                       pointer;
        std::optional<ExecutableIdentity> required_by;
    };
    std::vector<Work> work;
    for (const auto& reference : direct_references(topology)) {
        try {
            const auto& manifest = detail::RegistrySnapshotAccess::require_manifest(
                registry, reference.kind, reference.name);
            work.push_back({manifest.identity, reference.pointer, std::nullopt});
        } catch (const std::out_of_range&) {
            diagnostics.add(
                CompilePhase::Resolve, "P_REGISTRY_MISSING", DiagnosticSeverity::Error,
                reference.pointer,
                "Referenced executable is absent from the local RegistrySnapshot",
                json{{"kind", std::string(to_string(reference.kind))}, {"name", reference.name}});
        }
    }
    if (diagnostics.has_errors()) return {};
    std::map<std::pair<std::string, std::string>, ExecutableIdentity> visited;
    std::set<std::string>                                             capabilities;
    std::set<std::string>                                             effects;
    for (std::size_t index = 0; index < work.size(); ++index) {
        const auto current = work[index];
        const auto key =
            std::pair{std::string(to_string(current.expected.kind)), current.expected.name};
        const auto previous = visited.find(key);
        if (previous != visited.end()) {
            if (previous->second != current.expected) {
                json witness{{"expected", identity_json(current.expected)},
                             {"resolved", identity_json(previous->second)}};
                if (current.required_by)
                    witness["required_by"] = identity_json(*current.required_by);
                diagnostics.add(CompilePhase::Resolve, "P_REGISTRY_IDENTITY_MISMATCH",
                                DiagnosticSeverity::Error, current.pointer,
                                "Executable closure contains conflicting exact identities",
                                std::move(witness));
            }
            continue;
        }
        const ExecutableManifest* manifest = nullptr;
        try {
            manifest = &detail::RegistrySnapshotAccess::require_manifest(
                registry, current.expected.kind, current.expected.name);
        } catch (const std::out_of_range&) {
            json witness{{"expected", identity_json(current.expected)}};
            if (current.required_by) witness["required_by"] = identity_json(*current.required_by);
            diagnostics.add(CompilePhase::Resolve, "P_REGISTRY_DEPENDENCY_MISSING",
                            DiagnosticSeverity::Error, current.pointer,
                            "Required executable dependency is absent from the RegistrySnapshot",
                            std::move(witness));
            continue;
        }
        if (manifest->identity != current.expected) {
            json witness{{"expected", identity_json(current.expected)},
                         {"resolved", identity_json(manifest->identity)}};
            if (current.required_by) witness["required_by"] = identity_json(*current.required_by);
            diagnostics.add(CompilePhase::Resolve, "P_REGISTRY_IDENTITY_MISMATCH",
                            DiagnosticSeverity::Error, current.pointer,
                            "Required executable dependency has a mismatched exact identity",
                            std::move(witness));
            continue;
        }
        visited.emplace(key, manifest->identity);
        capabilities.insert(manifest->required_capabilities.begin(),
                            manifest->required_capabilities.end());
        effects.insert(manifest->declared_effects.begin(), manifest->declared_effects.end());
        if (manifest->effect_mode == EffectMode::TrustedNative)
            capabilities.insert(std::string(TRUSTED_NATIVE_CAPABILITY));
        for (const auto& dependency : manifest->required_executables)
            work.push_back({dependency, "/root/definition", manifest->identity});
    }
    ClosureResult result;
    for (const auto& [key, identity] : visited) {
        (void)key;
        result.identities.push_back(identity);
    }
    std::sort(result.identities.begin(), result.identities.end(), identity_less);
    result.closure.capabilities.assign(capabilities.begin(), capabilities.end());
    result.closure.effects.assign(effects.begin(), effects.end());
    return result;
}

std::string import_merkle_root(const ProgramSource& source, DiagnosticAccumulator& diagnostics) {
    auto imports = source.imports();
    std::sort(imports.begin(), imports.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.source_id, lhs.content_identity) <
               std::tie(rhs.source_id, rhs.content_identity);
    });
    for (std::size_t index = 1; index < imports.size(); ++index) {
        if (imports[index - 1].source_id != imports[index].source_id) continue;
        diagnostics.add(CompilePhase::Normalize, "P_IMPORT_CONFLICT", DiagnosticSeverity::Error,
                        "/imports", "A Program import source_id must occur exactly once",
                        json{{"source_id", imports[index].source_id},
                             {"first_content_identity", imports[index - 1].content_identity},
                             {"second_content_identity", imports[index].content_identity}});
    }
    if (diagnostics.has_errors()) return {};
    if (imports.empty()) return detail::sha256_identity("program-import-empty/v1", "");
    std::vector<std::string> level;
    for (const auto& import_ref : imports) {
        const json leaf{{"source_id", import_ref.source_id},
                        {"content_identity", import_ref.content_identity}};
        level.push_back(
            detail::sha256_identity("program-import-leaf/v1", detail::canonical_json_bytes(leaf)));
    }
    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());
        std::vector<std::string> next;
        for (std::size_t index = 0; index < level.size(); index += 2)
            next.push_back(
                detail::sha256_identity("program-import-node/v1", level[index] + level[index + 1]));
        level = std::move(next);
    }
    return level.front();
}

std::vector<SourceMapEntry> generated_source_map(const ProgramSource&   source,
                                                 DiagnosticAccumulator& diagnostics) {
    auto                  result = source.source_map();
    std::set<std::string> seen;
    for (const auto& entry : result) {
        if (!seen.insert(entry.generated_pointer).second) {
            diagnostics.add(CompilePhase::Schema, "P_SCHEMA_TYPE", DiagnosticSeverity::Error,
                            entry.generated_pointer,
                            "Source map contains a duplicate generated pointer",
                            json{{"generated_pointer", entry.generated_pointer}});
        }
    }
    for (const auto pointer : {"/root", "/root/definition", "/operations/0"}) {
        if (seen.contains(pointer)) continue;
        result.push_back({pointer, {source.source_id(), pointer, std::nullopt}});
        seen.insert(pointer);
    }
    return result;
}

json contract_json(const ContractRecord& contract) {
    return json{{"schema_version", contract.schema_version}, {"schema", contract.schema}};
}

std::string canonical_program_hash(const ParsedProgram&           parsed,
                                   const OrchestrationPlanRecord& plan,
                                   const SealedCoreDefinition&    definition) {
    json budgets = json::array();
    for (const auto& budget : parsed.budgets)
        budgets.push_back(json{{"resource", budget.resource},
                               {"minimum", budget.minimum},
                               {"maximum", budget.maximum}});
    const json semantic{
        {"program_schema_version", ProgramCompiler::PROGRAM_SCHEMA_VERSION},
        {"input_contract", contract_json(parsed.input_contract)},
        {"output_contract", contract_json(parsed.output_contract)},
        {"orchestration_plan", json{{"schema_version", plan.schema_version}, {"plan", plan.plan}}},
        {"sealed_core_definitions",
         json::array({json{{"name", definition.name},
                           {"definition_hash", definition.definition_hash},
                           {"definition", definition.definition}}})},
        {"declared_budget_requirements", std::move(budgets)}};
    return detail::sha256_identity("canonical-program/v1", detail::canonical_json_bytes(semantic));
}

}  // namespace

ProgramCompileError::ProgramCompileError(std::vector<Diagnostic> diagnostics)
    : std::runtime_error("Program compilation failed with " + std::to_string(diagnostics.size()) +
                         " diagnostic(s)"),
      diagnostics_(std::move(diagnostics)) {}

const std::vector<Diagnostic>& ProgramCompileError::diagnostics() const noexcept {
    return diagnostics_;
}

struct ProgramCompiler::Impl {
    RegistrySnapshot      registry;
    ProgramCompilerConfig config;

    ProgramBundle compile(const ProgramSource& source) const {
        DiagnosticAccumulator diagnostics(source);
        try {
            const json document = source.document();
            auto       parsed   = parse_program(source, document, diagnostics);
            if (diagnostics.has_errors()) diagnostics.throw_error();
            const auto parse_report = detail::RegistrySnapshotAccess::parse_local_report(
                registry, parsed.core_definition);
            add_core_parse_diagnostics(parse_report, diagnostics);
            if (!parse_report.topology && !diagnostics.has_errors()) {
                diagnostics.add(CompilePhase::Seal, "P_COMPILER_INTERNAL",
                                DiagnosticSeverity::Error, "/root/definition",
                                "Core parser returned neither topology nor diagnostics",
                                json::object());
            }
            if (diagnostics.has_errors()) diagnostics.throw_error();
            const auto& topology = *parse_report.topology;
            if (topology.name != parsed.root_name) {
                diagnostics.add(
                    CompilePhase::Normalize, "P_ROOT_NAME", DiagnosticSeverity::Error,
                    "/root/definition/name",
                    "Normalized Core topology name differs from the Program root name",
                    json{{"root_name", parsed.root_name}, {"normalized_core_name", topology.name}});
                diagnostics.throw_error();
            }
            const auto roundtrip = detail::RegistrySnapshotAccess::verify_roundtrip_report(
                parsed.core_definition, topology);
            add_roundtrip_diagnostics(roundtrip, diagnostics);
            if (diagnostics.has_errors()) diagnostics.throw_error();
            const auto validation =
                detail::RegistrySnapshotAccess::validate_local(registry, topology);
            add_validation_diagnostics(validation, diagnostics);
            if (diagnostics.has_errors()) diagnostics.throw_error();
            auto closure = resolve_closure(registry, topology, diagnostics);
            if (diagnostics.has_errors()) diagnostics.throw_error();
            auto                 sealed_json = topology.to_json();
            SealedCoreDefinition sealed{parsed.root_name, sealed_core_definition_hash(sealed_json),
                                        std::move(sealed_json)};
            const auto           compiled_plan = core_compiled_plan_identity(
                sealed, config.compiler_build_id, registry.fingerprint(), closure.identities);
            OrchestrationPlanRecord orchestration{
                1, json{{"root", "root"},
                        {"operations",
                         json::array({json{
                             {"id", "root"}, {"op", "call_core"}, {"core", parsed.root_name}}})}}};
            const auto program_hash = canonical_program_hash(parsed, orchestration, sealed);
            const auto merkle_root  = import_merkle_root(source, diagnostics);
            auto       source_map   = generated_source_map(source, diagnostics);
            if (diagnostics.has_errors()) diagnostics.throw_error();
            ProgramBundleData data;
            data.source_hash                    = source.source_hash();
            data.canonical_program_hash         = program_hash;
            data.compiler_build_id              = config.compiler_build_id;
            data.program_schema_version         = PROGRAM_SCHEMA_VERSION;
            data.registry_snapshot_fingerprint  = registry.fingerprint();
            data.module_dependency_merkle_root  = merkle_root;
            data.input_contract                 = std::move(parsed.input_contract);
            data.output_contract                = std::move(parsed.output_contract);
            data.orchestration_plan             = std::move(orchestration);
            data.sealed_core_definitions        = {std::move(sealed)};
            data.core_plan_identities           = {{parsed.root_name, compiled_plan}};
            data.capability_effect_closure      = std::move(closure.closure);
            data.executable_registry_identities = std::move(closure.identities);
            data.declared_budget_requirements   = std::move(parsed.budgets);
            data.source_map                     = std::move(source_map);
            data.diagnostics                    = diagnostics.release_warnings();
            return ProgramBundle(std::move(data));
        } catch (const ProgramCompileError&) {
            throw;
        } catch (...) {
            diagnostics.add(CompilePhase::Seal, "P_COMPILER_INTERNAL", DiagnosticSeverity::Error,
                            "", "Program compilation failed closed on an unexpected internal error",
                            json::object());
            diagnostics.throw_error();
        }
    }
};

ProgramCompiler::ProgramCompiler(RegistrySnapshot registry, ProgramCompilerConfig config)
    : impl_(std::make_unique<Impl>(Impl{std::move(registry), std::move(config)})) {
    if (impl_->config.compiler_build_id.empty())
        throw std::invalid_argument("Program compiler_build_id must not be empty");
    detail::validate_utf8(impl_->config.compiler_build_id);
    if (has_control_character(impl_->config.compiler_build_id))
        throw std::invalid_argument(
            "Program compiler_build_id must not contain control characters");
}

ProgramCompiler::ProgramCompiler(ProgramCompiler&&) noexcept            = default;
ProgramCompiler& ProgramCompiler::operator=(ProgramCompiler&&) noexcept = default;
ProgramCompiler::~ProgramCompiler()                                     = default;

const std::string& ProgramCompiler::compiler_build_id() const noexcept {
    return impl_->config.compiler_build_id;
}

const std::string& ProgramCompiler::registry_snapshot_fingerprint() const noexcept {
    return impl_->registry.fingerprint();
}

ProgramBundle ProgramCompiler::compile(const ProgramSource& source) const {
    return impl_->compile(source);
}

}  // namespace neograph::program
