#include <neograph/program/bundle.h>

#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view BUNDLE_FORMAT = "neograph-program-bundle";

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program bundle field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program bundle field '" + owned_key + "' must be unsigned");
    }
    return value[owned_key].get<unsigned long long>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const auto number = require_uint64(value, key);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program bundle integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Program bundle requires field '" + owned_key + "'");
    }
    return value[owned_key];
}

void require_object(const json& value, std::string_view name) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be an object");
    }
}

void require_nonempty_utf8(std::string_view value, std::string_view name) {
    if (value.empty()) throw std::invalid_argument(std::string(name) + " must not be empty");
    detail::validate_utf8(value);
}

void require_sha256(std::string_view value, std::string_view name) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(name) + " must be a sha256 identity");
    }
}

bool is_semantic_version(std::string_view value);

void validate_module_coordinate(const ModuleCoordinate& coordinate, std::string_view name) {
    detail::validate_token(coordinate.namespace_name, std::string(name) + " namespace");
    detail::validate_token(coordinate.name, std::string(name) + " name");
    if (!is_semantic_version(coordinate.semantic_version))
        throw std::invalid_argument(std::string(name) + " semantic_version is invalid");
    require_sha256(coordinate.content_identity, std::string(name) + " content_identity");
}

json encode_module_coordinate(const ModuleCoordinate& coordinate) {
    return json{{"namespace", coordinate.namespace_name},
                {"name", coordinate.name},
                {"semantic_version", coordinate.semantic_version},
                {"content_identity", coordinate.content_identity}};
}

ModuleCoordinate parse_module_coordinate(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Module coordinate must be an object");
    detail::reject_unknown_fields(value, "Module coordinate",
                                  {"namespace", "name", "semantic_version", "content_identity"});
    ModuleCoordinate result;
    const auto       required = [&](std::string_view key) {
        const auto owned = std::string(key);
        if (!value.contains(owned) || !value[owned].is_string())
            throw std::invalid_argument("Module coordinate field '" + owned + "' must be a string");
        return value[owned].get<std::string>();
    };
    result.namespace_name   = required("namespace");
    result.name             = required("name");
    result.semantic_version = required("semantic_version");
    result.content_identity = required("content_identity");
    validate_module_coordinate(result, "Module coordinate");
    return result;
}

bool is_ascii_digit(unsigned char value) noexcept {
    return value >= '0' && value <= '9';
}

bool is_ascii_alphanumeric(unsigned char value) noexcept {
    return is_ascii_digit(value) || (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

bool valid_numeric_identifier(std::string_view value) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    return std::all_of(value.begin(), value.end(), is_ascii_digit);
}

bool valid_identifier_list(std::string_view value, bool reject_numeric_leading_zero) {
    if (value.empty()) return false;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('.', begin);
        const auto part =
            value.substr(begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (part.empty()) return false;
        const auto valid_chars = std::all_of(part.begin(), part.end(), [](unsigned char c) {
            return is_ascii_alphanumeric(c) || c == '-';
        });
        if (!valid_chars) return false;
        if (reject_numeric_leading_zero && std::all_of(part.begin(), part.end(), is_ascii_digit) &&
            part.size() > 1 && part.front() == '0') {
            return false;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

bool is_semantic_version(std::string_view value) {
    const auto build_pos = value.find('+');
    if (build_pos != std::string_view::npos &&
        value.find('+', build_pos + 1) != std::string_view::npos) {
        return false;
    }
    const auto core_and_pre = value.substr(0, build_pos);
    if (build_pos != std::string_view::npos &&
        !valid_identifier_list(value.substr(build_pos + 1), false)) {
        return false;
    }
    const auto pre_pos = core_and_pre.find('-');
    const auto core    = core_and_pre.substr(0, pre_pos);
    if (pre_pos != std::string_view::npos &&
        !valid_identifier_list(core_and_pre.substr(pre_pos + 1), true)) {
        return false;
    }
    const auto first_dot  = core.find('.');
    const auto second_dot = first_dot == std::string_view::npos ? std::string_view::npos
                                                                : core.find('.', first_dot + 1);
    if (first_dot == std::string_view::npos || second_dot == std::string_view::npos ||
        core.find('.', second_dot + 1) != std::string_view::npos) {
        return false;
    }
    return valid_numeric_identifier(core.substr(0, first_dot)) &&
           valid_numeric_identifier(core.substr(first_dot + 1, second_dot - first_dot - 1)) &&
           valid_numeric_identifier(core.substr(second_dot + 1));
}

bool valid_executable_kind(ExecutableKind kind) noexcept {
    switch (kind) {
        case ExecutableKind::Node:
        case ExecutableKind::Reducer:
        case ExecutableKind::Condition:
        case ExecutableKind::Provider:
        case ExecutableKind::Tool:
        case ExecutableKind::Imported:
            return true;
    }
    return false;
}

void validate_contract(const ContractRecord& contract, std::string_view name) {
    if (contract.schema_version == 0 || !contract.schema.is_object()) {
        throw std::invalid_argument(std::string(name) +
                                    " requires a positive schema_version and object schema");
    }
}

void validate_plan(const OrchestrationPlanRecord& plan) {
    if (plan.schema_version == 0 || !plan.plan.is_object()) {
        throw std::invalid_argument(
            "Program orchestration plan requires a positive schema_version and object plan");
    }
}

void require_unique_string_array(const json& value, std::string_view name, bool nonempty) {
    if (!value.is_array() || (nonempty && value.empty())) {
        throw std::invalid_argument(std::string(name) +
                                    (nonempty ? " must be a nonempty array" : " must be an array"));
    }
    std::set<std::string> seen;
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw std::invalid_argument(std::string(name) + " entries must be strings");
        }
        const auto item_value = item.get<std::string>();
        require_nonempty_utf8(item_value, name);
        if (!seen.insert(item_value).second) {
            throw std::invalid_argument(std::string(name) + " contains a duplicate");
        }
    }
}

void validate_routes(const json& value, std::string_view name) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be an object");
    }
    for (const auto& [route, target] : value.items()) {
        require_nonempty_utf8(route, name);
        if (!target.is_string()) {
            throw std::invalid_argument(std::string(name) + " targets must be strings");
        }
        require_nonempty_utf8(target.get<std::string>(), name);
    }
}

void validate_core_edge(const json&      edge,
                        std::string_view name,
                        bool             conditional,
                        bool             allow_inline_type) {
    if (!edge.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be an object");
    }
    if (conditional) {
        if (allow_inline_type) {
            detail::reject_unknown_fields(edge, name, {"from", "condition", "routes", "type"});
        } else {
            detail::reject_unknown_fields(edge, name, {"from", "condition", "routes"});
        }
    } else {
        detail::reject_unknown_fields(edge, name, {"from", "to"});
    }
    require_nonempty_utf8(require_string(edge, "from"), name);
    if (conditional) {
        require_nonempty_utf8(require_string(edge, "condition"), name);
        if (edge.contains("type") &&
            (!allow_inline_type || require_string(edge, "type") != "conditional")) {
            throw std::invalid_argument(std::string(name) +
                                        " type must be 'conditional' when present");
        }
        if (edge.contains("routes")) {
            validate_routes(edge["routes"], std::string(name) + " routes");
        }
    } else {
        require_nonempty_utf8(require_string(edge, "to"), name);
    }
}

void validate_core_definition_v1(const json& definition) {
    if (!definition.is_object()) {
        throw std::invalid_argument("Sealed Core definition must be an object");
    }
    detail::reject_unknown_fields(
        definition, "Sealed Core definition",
        {"schema_version", "name", "channels", "nodes", "edges", "conditional_edges",
         "interrupt_before", "interrupt_after", "retry_policy"});

    if (!definition.contains("schema_version")) {
        throw std::invalid_argument("Sealed Core definition requires v1 strict schema_version");
    }
    const auto& encoded_version = definition["schema_version"];
    const bool  v1_version =
        (encoded_version.is_number_unsigned() &&
         encoded_version.get<unsigned long long>() ==
             SealedCoreDefinition::STORAGE_SCHEMA_VERSION) ||
        (encoded_version.is_number_integer() &&
         encoded_version.get<long long>() == SealedCoreDefinition::STORAGE_SCHEMA_VERSION);
    if (!v1_version) {
        throw std::invalid_argument("Sealed Core definition requires v1 strict schema_version");
    }
    if (definition.contains("name")) {
        require_nonempty_utf8(require_string(definition, "name"), "Sealed Core definition name");
    }

    if (definition.contains("channels")) {
        const auto& channels = definition["channels"];
        if (!channels.is_object()) {
            throw std::invalid_argument("Sealed Core channels must be an object");
        }
        for (const auto& [name, channel] : channels.items()) {
            require_nonempty_utf8(name, "Sealed Core channel name");
            if (!channel.is_object()) {
                throw std::invalid_argument("Sealed Core channel definition must be an object");
            }
            detail::reject_unknown_fields(channel, "Sealed Core channel", {"reducer", "initial"});
            if (channel.contains("reducer")) {
                require_nonempty_utf8(require_string(channel, "reducer"),
                                      "Sealed Core channel reducer");
            }
        }
    }

    if (!definition.contains("nodes") || !definition["nodes"].is_object() ||
        definition["nodes"].empty()) {
        throw std::invalid_argument("Sealed Core definition requires a nonempty nodes object");
    }
    for (const auto& [name, node] : definition["nodes"].items()) {
        require_nonempty_utf8(name, "Sealed Core node name");
        if (!node.is_object()) {
            throw std::invalid_argument("Sealed Core node definition must be an object");
        }
        require_nonempty_utf8(require_string(node, "type"), "Sealed Core node type");
        for (const auto& [key, value] : node.items()) {
            (void)value;
            require_nonempty_utf8(key, "Sealed Core node field");
        }
        if (node.contains("barrier")) {
            const auto& barrier = node["barrier"];
            if (!barrier.is_object()) {
                throw std::invalid_argument("Sealed Core node barrier must be an object");
            }
            detail::reject_unknown_fields(barrier, "Sealed Core node barrier", {"wait_for"});
            if (!barrier.contains("wait_for")) {
                throw std::invalid_argument("Sealed Core node barrier requires wait_for");
            }
            require_unique_string_array(barrier["wait_for"], "Sealed Core barrier wait_for", true);
        }
    }

    if (definition.contains("edges")) {
        const auto& edges = definition["edges"];
        if (!edges.is_array()) {
            throw std::invalid_argument("Sealed Core edges must be an array");
        }
        for (const auto& edge : edges) {
            const bool conditional =
                edge.is_object() &&
                (edge.contains("condition") || (edge.contains("type") && edge["type"].is_string() &&
                                                edge["type"].get<std::string>() == "conditional"));
            validate_core_edge(edge, "Sealed Core edge", conditional, true);
        }
    }
    if (definition.contains("conditional_edges")) {
        const auto& edges = definition["conditional_edges"];
        if (!edges.is_array()) {
            throw std::invalid_argument("Sealed Core conditional_edges must be an array");
        }
        for (const auto& edge : edges) {
            validate_core_edge(edge, "Sealed Core conditional edge", true, false);
        }
    }
    if (definition.contains("interrupt_before")) {
        require_unique_string_array(definition["interrupt_before"], "Sealed Core interrupt_before",
                                    false);
    }
    if (definition.contains("interrupt_after")) {
        require_unique_string_array(definition["interrupt_after"], "Sealed Core interrupt_after",
                                    false);
    }
    if (definition.contains("retry_policy")) {
        const auto& retry = definition["retry_policy"];
        if (!retry.is_object()) {
            throw std::invalid_argument("Sealed Core retry_policy must be an object");
        }
        detail::reject_unknown_fields(
            retry, "Sealed Core retry_policy",
            {"max_retries", "initial_delay_ms", "backoff_multiplier", "max_delay_ms"});
        for (const auto key : {"max_retries", "initial_delay_ms", "max_delay_ms"}) {
            if (!retry.contains(key)) continue;
            const auto& item = retry[key];
            const bool  valid =
                (item.is_number_unsigned() &&
                 item.get<unsigned long long>() <=
                     static_cast<unsigned long long>(std::numeric_limits<int>::max())) ||
                (item.is_number_integer() && item.get<long long>() >= 0 &&
                 item.get<long long>() <= std::numeric_limits<int>::max());
            if (!valid) {
                throw std::invalid_argument(std::string("Sealed Core retry_policy ") + key +
                                            " must be a nonnegative int32");
            }
        }
        if (retry.contains("backoff_multiplier")) {
            const auto& item = retry["backoff_multiplier"];
            if (!item.is_number()) {
                throw std::invalid_argument(
                    "Sealed Core retry_policy backoff_multiplier must be a number");
            }
            const auto multiplier = item.get<double>();
            if (!std::isfinite(multiplier) ||
                multiplier < static_cast<double>(std::numeric_limits<float>::lowest()) ||
                multiplier > static_cast<double>(std::numeric_limits<float>::max())) {
                throw std::invalid_argument(
                    "Sealed Core retry_policy backoff_multiplier exceeds float range");
            }
        }
    }
}

void validate_sealed_definition(const SealedCoreDefinition& definition) {
    require_nonempty_utf8(definition.name, "Sealed Core definition name");
    require_sha256(definition.definition_hash, "Sealed Core definition hash");
    validate_core_definition_v1(definition.definition);
    if (definition.definition.contains("name") &&
        definition.definition["name"].get<std::string>() != definition.name) {
        throw std::invalid_argument(
            "Sealed Core definition name does not match its canonical definition");
    }
    if (definition.definition_hash != sealed_core_definition_hash(definition.definition)) {
        throw std::invalid_argument(
            "Sealed Core definition hash does not match its canonical definition");
    }
}

void validate_core_plan(const CorePlanIdentity& plan) {
    require_nonempty_utf8(plan.name, "Core plan name");
    require_sha256(plan.compiled_plan_identity, "Core compiled plan identity");
}

void validate_executable(const ExecutableIdentity& identity) {
    if (!valid_executable_kind(identity.kind)) {
        throw std::invalid_argument("Executable identity has unknown kind");
    }
    require_nonempty_utf8(identity.name, "Executable identity name");
    if (!is_semantic_version(identity.semantic_version)) {
        throw std::invalid_argument("Executable identity semantic_version is invalid");
    }
    require_sha256(identity.implementation_digest, "Executable implementation digest");
}

bool executable_identity_less(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
    return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                      lhs.implementation_digest} < std::tuple{to_string(rhs.kind), rhs.name,
                                                              rhs.semantic_version,
                                                              rhs.implementation_digest};
}

void sort_unique_strings(std::vector<std::string>& values, std::string_view name) {
    for (const auto& value : values)
        require_nonempty_utf8(value, name);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(name) + " contains a duplicate");
    }
}

void validate_execution_guarantee(ExecutionGuarantee guarantee, std::string_view field) {
    if (execution_guarantee_rank(guarantee) == 0) {
        throw std::invalid_argument(std::string(field) + " is unsupported");
    }
}

void normalize_data(ProgramBundleData& data) {
    data.input_contract.schema   = detail::owned_json_copy(data.input_contract.schema);
    data.output_contract.schema  = detail::owned_json_copy(data.output_contract.schema);
    data.orchestration_plan.plan = detail::owned_json_copy(data.orchestration_plan.plan);
    for (auto& definition : data.sealed_core_definitions) {
        definition.definition = detail::owned_json_copy(definition.definition);
    }
    for (auto& diagnostic : data.diagnostics) {
        diagnostic.witness = detail::owned_json_copy(diagnostic.witness);
    }
    if (data.source_kind != SourceKind::CanonicalJson &&
        data.source_kind != SourceKind::CppBuilder && data.source_kind != SourceKind::JavaScript) {
        throw std::invalid_argument("Program bundle source_kind is unknown");
    }
    require_sha256(data.source_hash, "Program bundle source_hash");
    if (data.control_source) {
        if (data.source_kind != SourceKind::JavaScript ||
            data.control_source->kind() != SourceKind::JavaScript) {
            throw std::invalid_argument(
                "Program bundle control_source requires JavaScript source_kind");
        }
        if (data.control_source->source_hash() != data.source_hash) {
            throw std::invalid_argument(
                "Program bundle control_source identity does not match source_hash");
        }
    }
    require_sha256(data.canonical_program_hash, "Program bundle canonical_program_hash");
    require_nonempty_utf8(data.compiler_build_id, "Program bundle compiler_build_id");
    if (data.program_schema_version != 1) {
        throw std::invalid_argument("Program bundle program_schema_version is unsupported");
    }
    require_sha256(data.registry_snapshot_fingerprint,
                   "Program bundle registry_snapshot_fingerprint");
    require_sha256(data.module_dependency_merkle_root,
                   "Program bundle module_dependency_merkle_root");
    for (const auto& coordinate : data.module_coordinates)
        validate_module_coordinate(coordinate, "Program bundle module coordinate");
    std::sort(data.module_coordinates.begin(), data.module_coordinates.end(),
              [](const auto& lhs, const auto& rhs) {
                  return std::tie(lhs.namespace_name, lhs.name, lhs.semantic_version,
                                  lhs.content_identity) < std::tie(rhs.namespace_name, rhs.name,
                                                                   rhs.semantic_version,
                                                                   rhs.content_identity);
              });
    if (std::adjacent_find(data.module_coordinates.begin(), data.module_coordinates.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.namespace_name == rhs.namespace_name &&
                                      lhs.name == rhs.name &&
                                      lhs.semantic_version == rhs.semantic_version;
                           }) != data.module_coordinates.end())
        throw std::invalid_argument("Program bundle contains a module coordinate collision");
    if (std::adjacent_find(data.module_coordinates.begin(), data.module_coordinates.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.content_identity == rhs.content_identity;
                           }) != data.module_coordinates.end())
        throw std::invalid_argument("Program bundle contains a duplicate module identity");
    validate_contract(data.input_contract, "Program input contract");
    validate_contract(data.output_contract, "Program output contract");
    validate_plan(data.orchestration_plan);
    validate_execution_guarantee(data.execution_guarantee, "Program bundle execution_guarantee");

    if (data.sealed_core_definitions.empty() || data.core_plan_identities.empty() ||
        data.executable_registry_identities.empty() || data.declared_budget_requirements.empty()) {
        throw std::invalid_argument("Program bundle requires Core, executable, and budget records");
    }

    for (const auto& definition : data.sealed_core_definitions)
        validate_sealed_definition(definition);
    std::sort(data.sealed_core_definitions.begin(), data.sealed_core_definitions.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
    if (std::adjacent_find(data.sealed_core_definitions.begin(), data.sealed_core_definitions.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.name == rhs.name; }) !=
        data.sealed_core_definitions.end()) {
        throw std::invalid_argument(
            "Program bundle contains duplicate sealed Core definition names");
    }

    for (const auto& plan : data.core_plan_identities)
        validate_core_plan(plan);
    std::sort(data.core_plan_identities.begin(), data.core_plan_identities.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
    if (std::adjacent_find(data.core_plan_identities.begin(), data.core_plan_identities.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.name == rhs.name; }) !=
        data.core_plan_identities.end()) {
        throw std::invalid_argument("Program bundle contains duplicate Core plan names");
    }
    if (data.sealed_core_definitions.size() != data.core_plan_identities.size()) {
        throw std::invalid_argument("Every sealed Core definition requires one plan identity");
    }
    for (std::size_t index = 0; index < data.sealed_core_definitions.size(); ++index) {
        if (data.sealed_core_definitions[index].name != data.core_plan_identities[index].name) {
            throw std::invalid_argument("Sealed Core definition and plan identity names differ");
        }
    }

    for (const auto& identity : data.executable_registry_identities)
        validate_executable(identity);
    std::sort(data.executable_registry_identities.begin(),
              data.executable_registry_identities.end(), executable_identity_less);
    if (std::adjacent_find(data.executable_registry_identities.begin(),
                           data.executable_registry_identities.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.kind == rhs.kind && lhs.name == rhs.name;
                           }) != data.executable_registry_identities.end()) {
        throw std::invalid_argument(
            "Program bundle contains duplicate or ambiguous executable closure identities");
    }

    sort_unique_strings(data.capability_effect_closure.capabilities, "Capability closure");
    sort_unique_strings(data.capability_effect_closure.effects, "Effect closure");

    for (const auto& requirement : data.declared_budget_requirements) {
        require_nonempty_utf8(requirement.resource, "Budget resource");
        if (requirement.maximum < requirement.minimum) {
            throw std::invalid_argument("Budget requirement maximum precedes minimum");
        }
    }
    std::sort(data.declared_budget_requirements.begin(), data.declared_budget_requirements.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.resource < rhs.resource; });
    if (std::adjacent_find(
            data.declared_budget_requirements.begin(), data.declared_budget_requirements.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.resource == rhs.resource; }) !=
        data.declared_budget_requirements.end()) {
        throw std::invalid_argument("Program bundle contains duplicate budget resources");
    }

    for (const auto& mapping : data.source_map) {
        detail::validate_json_pointer(mapping.generated_pointer);
        json encoded;
        to_json(encoded, mapping.authored);
    }
    std::sort(data.source_map.begin(), data.source_map.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.generated_pointer, lhs.authored.source_id, lhs.authored.json_pointer) <
               std::tie(rhs.generated_pointer, rhs.authored.source_id, rhs.authored.json_pointer);
    });
    if (std::adjacent_find(data.source_map.begin(), data.source_map.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.generated_pointer == rhs.generated_pointer;
                           }) != data.source_map.end()) {
        throw std::invalid_argument("Program bundle contains duplicate source-map pointers");
    }

    for (const auto& diagnostic : data.diagnostics) {
        json encoded;
        to_json(encoded, diagnostic);
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            throw std::invalid_argument(
                "Successful Program bundle cannot contain error diagnostics");
        }
    }
}

json encode_contract(const ContractRecord& contract) {
    json value              = json::object();
    value["schema_version"] = contract.schema_version;
    value["schema"]         = contract.schema;
    return value;
}

ContractRecord parse_contract(const json& value, std::string_view name) {
    require_object(value, name);
    detail::reject_unknown_fields(value, name, {"schema_version", "schema"});
    ContractRecord result;
    result.schema_version = require_uint32(value, "schema_version");
    result.schema         = require_value(value, "schema");
    validate_contract(result, name);
    return result;
}

json encode_plan(const OrchestrationPlanRecord& plan) {
    json value              = json::object();
    value["schema_version"] = plan.schema_version;
    value["plan"]           = plan.plan;
    return value;
}

OrchestrationPlanRecord parse_plan(const json& value) {
    require_object(value, "Program orchestration plan");
    detail::reject_unknown_fields(value, "Program orchestration plan", {"schema_version", "plan"});
    OrchestrationPlanRecord result;
    result.schema_version = require_uint32(value, "schema_version");
    result.plan           = require_value(value, "plan");
    validate_plan(result);
    return result;
}

json encode_sealed_definition(const SealedCoreDefinition& definition) {
    json value               = json::object();
    value["name"]            = definition.name;
    value["definition_hash"] = definition.definition_hash;
    value["definition"]      = definition.definition;
    return value;
}

SealedCoreDefinition parse_sealed_definition(const json& value) {
    require_object(value, "Sealed Core definition");
    detail::reject_unknown_fields(value, "Sealed Core definition",
                                  {"name", "definition_hash", "definition"});
    SealedCoreDefinition result;
    result.name            = require_string(value, "name");
    result.definition_hash = require_string(value, "definition_hash");
    result.definition      = require_value(value, "definition");
    validate_sealed_definition(result);
    return result;
}

json encode_core_plan(const CorePlanIdentity& plan) {
    return json{{"name", plan.name}, {"compiled_plan_identity", plan.compiled_plan_identity}};
}

CorePlanIdentity parse_core_plan(const json& value) {
    require_object(value, "Core plan identity");
    detail::reject_unknown_fields(value, "Core plan identity", {"name", "compiled_plan_identity"});
    CorePlanIdentity result{require_string(value, "name"),
                            require_string(value, "compiled_plan_identity")};
    validate_core_plan(result);
    return result;
}

json encode_executable(const ExecutableIdentity& identity) {
    validate_executable(identity);
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

ExecutableIdentity parse_executable(const json& value) {
    require_object(value, "Executable identity");
    detail::reject_unknown_fields(value, "Executable identity",
                                  {"kind", "name", "semantic_version", "implementation_digest"});
    ExecutableIdentity result;
    result.kind                  = executable_kind_from_string(require_string(value, "kind"));
    result.name                  = require_string(value, "name");
    result.semantic_version      = require_string(value, "semantic_version");
    result.implementation_digest = require_string(value, "implementation_digest");
    validate_executable(result);
    return result;
}

json encode_closure(const CapabilityEffectClosure& closure) {
    json capabilities = json::array();
    for (const auto& capability : closure.capabilities)
        capabilities.push_back(capability);
    json effects = json::array();
    for (const auto& effect : closure.effects)
        effects.push_back(effect);
    return json{{"capabilities", std::move(capabilities)}, {"effects", std::move(effects)}};
}

CapabilityEffectClosure parse_closure(const json& value) {
    require_object(value, "Capability/effect closure");
    detail::reject_unknown_fields(value, "Capability/effect closure", {"capabilities", "effects"});
    const auto& capabilities = require_value(value, "capabilities");
    const auto& effects      = require_value(value, "effects");
    if (!capabilities.is_array() || !effects.is_array()) {
        throw std::invalid_argument("Capability/effect closure fields must be arrays");
    }
    CapabilityEffectClosure result;
    for (const auto& item : capabilities) {
        if (!item.is_string()) throw std::invalid_argument("Capability must be a string");
        result.capabilities.push_back(item.get<std::string>());
    }
    for (const auto& item : effects) {
        if (!item.is_string()) throw std::invalid_argument("Effect must be a string");
        result.effects.push_back(item.get<std::string>());
    }
    return result;
}

json encode_budget(const BudgetRequirement& requirement) {
    return json{{"resource", requirement.resource},
                {"minimum", requirement.minimum},
                {"maximum", requirement.maximum}};
}

BudgetRequirement parse_budget(const json& value) {
    require_object(value, "Budget requirement");
    detail::reject_unknown_fields(value, "Budget requirement", {"resource", "minimum", "maximum"});
    return BudgetRequirement{require_string(value, "resource"), require_uint64(value, "minimum"),
                             require_uint64(value, "maximum")};
}

json bundle_body(const ProgramBundleData& data) {
    json value                      = json::object();
    value["source_kind"]            = std::string(to_string(data.source_kind));
    value["source_hash"]            = data.source_hash;
    value["canonical_program_hash"] = data.canonical_program_hash;
    if (data.control_source) {
        value["control_source"] =
            detail::parse_json_strict(data.control_source->serialize_canonical());
    }
    value["compiler_build_id"]             = data.compiler_build_id;
    value["program_schema_version"]        = data.program_schema_version;
    value["registry_snapshot_fingerprint"] = data.registry_snapshot_fingerprint;
    value["module_dependency_merkle_root"] = data.module_dependency_merkle_root;
    if (!data.module_coordinates.empty()) {
        json coordinates = json::array();
        for (const auto& coordinate : data.module_coordinates)
            coordinates.push_back(encode_module_coordinate(coordinate));
        value["module_coordinates"] = std::move(coordinates);
    }
    value["input_contract"]     = encode_contract(data.input_contract);
    value["output_contract"]    = encode_contract(data.output_contract);
    value["orchestration_plan"] = encode_plan(data.orchestration_plan);

    json definitions = json::array();
    for (const auto& definition : data.sealed_core_definitions) {
        definitions.push_back(encode_sealed_definition(definition));
    }
    value["sealed_core_definitions"] = std::move(definitions);

    json plans = json::array();
    for (const auto& plan : data.core_plan_identities)
        plans.push_back(encode_core_plan(plan));
    value["core_plan_identities"]      = std::move(plans);
    value["capability_effect_closure"] = encode_closure(data.capability_effect_closure);
    value["execution_guarantee"]       = std::string(to_string(data.execution_guarantee));

    json executable_identities = json::array();
    for (const auto& identity : data.executable_registry_identities) {
        executable_identities.push_back(encode_executable(identity));
    }
    value["executable_registry_identities"] = std::move(executable_identities);

    json budgets = json::array();
    for (const auto& requirement : data.declared_budget_requirements) {
        budgets.push_back(encode_budget(requirement));
    }
    value["declared_budget_requirements"] = std::move(budgets);

    json source_map = json::array();
    for (const auto& entry : data.source_map) {
        json encoded;
        to_json(encoded, entry);
        source_map.push_back(std::move(encoded));
    }
    value["source_map"] = std::move(source_map);

    json diagnostics = json::array();
    for (const auto& diagnostic : data.diagnostics) {
        json encoded;
        to_json(encoded, diagnostic);
        diagnostics.push_back(std::move(encoded));
    }
    value["diagnostics"] = std::move(diagnostics);
    return value;
}

json bundle_identity_envelope(const ProgramBundleData& data) {
    auto value                      = bundle_body(data);
    value["format"]                 = std::string(BUNDLE_FORMAT);
    value["storage_schema_version"] = ProgramBundle::STORAGE_SCHEMA_VERSION;
    return value;
}

template <typename T, typename Parse>
std::vector<T> parse_array(const json& value, std::string_view key, Parse parse) {
    const auto& encoded = require_value(value, key);
    if (!encoded.is_array()) {
        throw std::invalid_argument("Program bundle field '" + std::string(key) +
                                    "' must be an array");
    }
    std::vector<T> result;
    result.reserve(encoded.size());
    for (const auto& item : encoded)
        result.push_back(parse(item));
    return result;
}

ProgramBundleData parse_body(const json& value) {
    ProgramBundleData data;
    data.source_kind            = source_kind_from_string(require_string(value, "source_kind"));
    data.source_hash            = require_string(value, "source_hash");
    data.canonical_program_hash = require_string(value, "canonical_program_hash");
    if (value.contains("control_source")) {
        data.control_source =
            ProgramSource::parse(detail::canonical_json_bytes(value.at("control_source")));
    }
    data.compiler_build_id             = require_string(value, "compiler_build_id");
    data.program_schema_version        = require_uint32(value, "program_schema_version");
    data.registry_snapshot_fingerprint = require_string(value, "registry_snapshot_fingerprint");
    data.module_dependency_merkle_root = require_string(value, "module_dependency_merkle_root");
    if (value.contains("module_coordinates")) {
        if (!value["module_coordinates"].is_array())
            throw std::invalid_argument("Program bundle module_coordinates must be an array");
        for (const auto& item : value["module_coordinates"])
            data.module_coordinates.push_back(parse_module_coordinate(item));
    }
    data.input_contract =
        parse_contract(require_value(value, "input_contract"), "Program input contract");
    data.output_contract =
        parse_contract(require_value(value, "output_contract"), "Program output contract");
    data.orchestration_plan      = parse_plan(require_value(value, "orchestration_plan"));
    data.sealed_core_definitions = parse_array<SealedCoreDefinition>(
        value, "sealed_core_definitions", parse_sealed_definition);
    data.core_plan_identities =
        parse_array<CorePlanIdentity>(value, "core_plan_identities", parse_core_plan);
    data.capability_effect_closure =
        parse_closure(require_value(value, "capability_effect_closure"));
    data.execution_guarantee =
        execution_guarantee_from_string(require_string(value, "execution_guarantee"));
    data.executable_registry_identities =
        parse_array<ExecutableIdentity>(value, "executable_registry_identities", parse_executable);
    data.declared_budget_requirements =
        parse_array<BudgetRequirement>(value, "declared_budget_requirements", parse_budget);

    const auto encoded_source_map = require_value(value, "source_map");
    if (!encoded_source_map.is_array()) {
        throw std::invalid_argument("Program bundle source_map must be an array");
    }
    for (const auto item : encoded_source_map) {
        SourceMapEntry entry;
        from_json(item, entry);
        data.source_map.push_back(std::move(entry));
    }

    const auto encoded_diagnostics = require_value(value, "diagnostics");
    if (!encoded_diagnostics.is_array()) {
        throw std::invalid_argument("Program bundle diagnostics must be an array");
    }
    for (const auto item : encoded_diagnostics) {
        Diagnostic diagnostic;
        from_json(item, diagnostic);
        data.diagnostics.push_back(std::move(diagnostic));
    }
    normalize_data(data);
    return data;
}

constexpr std::size_t kMaxContractSchemaDepth = 64;

void check_contract_depth(std::size_t depth, std::string_view path) {
    if (depth > kMaxContractSchemaDepth) {
        throw std::invalid_argument("Program contract schema nesting exceeds 64 levels at " +
                                    std::string(path));
    }
}

bool contract_type_matches(const json& value, std::string_view type) {
    if (type == "null") return value.is_null();
    if (type == "boolean") return value.is_boolean();
    if (type == "object") return value.is_object();
    if (type == "array") return value.is_array();
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer();
    if (type == "string") return value.is_string();
    throw std::invalid_argument("Unsupported Program contract schema type: " + std::string(type));
}

void validate_contract_type_name(const json& type, std::string_view path) {
    if (!type.is_string()) {
        throw std::invalid_argument("Program contract schema type at " + std::string(path) +
                                    " must contain strings");
    }
    static constexpr std::array<std::string_view, 7> supported = {
        "null", "boolean", "object", "array", "number", "integer", "string"};
    const auto name = type.get<std::string>();
    if (std::find(supported.begin(), supported.end(), name) == supported.end()) {
        throw std::invalid_argument("Unsupported Program contract schema type: " + name);
    }
}

void validate_contract_schema_impl(const json& schema, const std::string& path, std::size_t depth) {
    check_contract_depth(depth, path);
    if (!schema.is_object()) {
        throw std::invalid_argument("Program contract schema at " + path + " must be an object");
    }
    if (schema.contains("type")) {
        const auto& types = schema["type"];
        if (types.is_string()) {
            validate_contract_type_name(types, path);
        } else if (types.is_array() && !types.empty()) {
            for (const auto& type : types)
                validate_contract_type_name(type, path);
        } else {
            throw std::invalid_argument("Program contract schema type at " + path +
                                        " must be a string or nonempty array");
        }
    }
    if (schema.contains("enum") && !schema["enum"].is_array()) {
        throw std::invalid_argument("Program contract schema enum at " + path +
                                    " must be an array");
    }
    if (schema.contains("required")) {
        const auto& required = schema["required"];
        if (!required.is_array()) {
            throw std::invalid_argument("Program contract schema required at " + path +
                                        " must be an array");
        }
        std::set<std::string> seen;
        for (const auto& name : required) {
            if (!name.is_string() || !seen.insert(name.get<std::string>()).second) {
                throw std::invalid_argument("Program contract schema required at " + path +
                                            " must contain unique strings");
            }
        }
    }
    if (schema.contains("properties")) {
        const auto& properties = schema["properties"];
        if (!properties.is_object()) {
            throw std::invalid_argument("Program contract schema properties at " + path +
                                        " must be an object");
        }
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            validate_contract_schema_impl(it.value(), path + "/properties/" + it.key(), depth + 1);
        }
    }
    if (schema.contains("items")) {
        validate_contract_schema_impl(schema["items"], path + "/items", depth + 1);
    }
    if (schema.contains("additionalProperties")) {
        const auto& additional = schema["additionalProperties"];
        if (!additional.is_boolean() && !additional.is_object()) {
            throw std::invalid_argument("Program contract schema additionalProperties at " + path +
                                        " must be a boolean or object");
        }
        if (additional.is_object()) {
            validate_contract_schema_impl(additional, path + "/additionalProperties", depth + 1);
        }
    }
}

void validate_contract_value_impl(const json&        value,
                                  const json&        schema,
                                  std::string_view   subject,
                                  const std::string& path,
                                  std::size_t        depth) {
    check_contract_depth(depth, path);
    if (schema.contains("const") && value != schema["const"]) {
        throw std::invalid_argument(std::string(subject) + " at " + path + " does not match const");
    }
    if (schema.contains("enum")) {
        bool matched = false;
        for (const auto& candidate : schema["enum"]) {
            if (candidate == value) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::invalid_argument(std::string(subject) + " at " + path + " is not in enum");
        }
    }
    if (schema.contains("type")) {
        const auto& types   = schema["type"];
        bool        matched = false;
        if (types.is_string()) {
            matched = contract_type_matches(value, types.get<std::string>());
        } else {
            for (const auto& type : types) {
                if (contract_type_matches(value, type.get<std::string>())) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            throw std::invalid_argument(std::string(subject) + " at " + path +
                                        " has the wrong JSON type");
        }
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& name : schema["required"]) {
                const auto property = name.get<std::string>();
                if (!value.contains(property)) {
                    throw std::invalid_argument(std::string(subject) + " at " + path +
                                                " is missing required property " + property);
                }
            }
        }
        const auto properties = schema.value("properties", json::object());
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (value.contains(it.key())) {
                validate_contract_value_impl(value[it.key()], it.value(), subject,
                                             path + "/" + it.key(), depth + 1);
            }
        }
        if (schema.contains("additionalProperties")) {
            const auto& additional = schema["additionalProperties"];
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (properties.contains(it.key())) continue;
                if (additional.is_boolean() && !additional.get<bool>()) {
                    throw std::invalid_argument(std::string(subject) + " at " + path +
                                                " has unexpected property " + it.key());
                }
                if (additional.is_object()) {
                    validate_contract_value_impl(it.value(), additional, subject,
                                                 path + "/" + it.key(), depth + 1);
                }
            }
        }
    }
    if (value.is_array() && schema.contains("items")) {
        for (std::size_t index = 0; index < value.size(); ++index) {
            validate_contract_value_impl(value[index], schema["items"], subject,
                                         path + "/" + std::to_string(index), depth + 1);
        }
    }
}
}  // namespace

void validate_contract_schema(const ContractRecord& contract, std::string_view path) {
    if (contract.schema_version != 1) {
        throw std::invalid_argument("Program contract schema_version must be 1");
    }
    validate_contract_schema_impl(contract.schema, std::string(path), 0);
}

void validate_contract_value(const json&           value,
                             const ContractRecord& contract,
                             std::string_view      subject,
                             std::string_view      path) {
    validate_contract_schema(contract, path);
    validate_contract_value_impl(value, contract.schema, subject, std::string(path), 0);
}

std::string_view to_string(ExecutionGuarantee guarantee) noexcept {
    switch (guarantee) {
        case ExecutionGuarantee::Strict:
            return "strict";
        case ExecutionGuarantee::Recorded:
            return "recorded";
        case ExecutionGuarantee::Unmanaged:
            return "unmanaged";
    }
    return "unknown";
}

ExecutionGuarantee execution_guarantee_from_string(std::string_view value) {
    if (value == "strict") return ExecutionGuarantee::Strict;
    if (value == "recorded") return ExecutionGuarantee::Recorded;
    if (value == "unmanaged") return ExecutionGuarantee::Unmanaged;
    throw std::invalid_argument("Unknown Program execution guarantee: " + std::string(value));
}

std::uint8_t execution_guarantee_rank(ExecutionGuarantee guarantee) noexcept {
    switch (guarantee) {
        case ExecutionGuarantee::Strict:
            return 3;
        case ExecutionGuarantee::Recorded:
            return 2;
        case ExecutionGuarantee::Unmanaged:
            return 1;
    }
    return 0;
}

std::string_view to_string(ExecutableKind kind) noexcept {
    switch (kind) {
        case ExecutableKind::Node:
            return "node";
        case ExecutableKind::Reducer:
            return "reducer";
        case ExecutableKind::Condition:
            return "condition";
        case ExecutableKind::Provider:
            return "provider";
        case ExecutableKind::Tool:
            return "tool";
        case ExecutableKind::Imported:
            return "imported";
    }
    return "unknown";
}

ExecutableKind executable_kind_from_string(std::string_view value) {
    if (value == "node") return ExecutableKind::Node;
    if (value == "reducer") return ExecutableKind::Reducer;
    if (value == "condition") return ExecutableKind::Condition;
    if (value == "provider") return ExecutableKind::Provider;
    if (value == "tool") return ExecutableKind::Tool;
    if (value == "imported") return ExecutableKind::Imported;
    throw std::invalid_argument("Unknown Program executable kind: " + std::string(value));
}

std::string sealed_core_definition_hash(const json& definition) {
    if (!definition.is_object()) {
        throw std::invalid_argument("Sealed Core definition must be an object");
    }
    return detail::sha256_identity("sealed-core-definition/v1",
                                   detail::canonical_json_bytes(definition));
}

std::string core_compiled_plan_identity(const SealedCoreDefinition& definition,
                                        std::string_view            compiler_build_id,
                                        std::string_view            registry_snapshot_fingerprint,
                                        const std::vector<ExecutableIdentity>& executable_closure) {
    validate_sealed_definition(definition);
    require_nonempty_utf8(compiler_build_id, "Core compiled-plan compiler_build_id");
    require_sha256(registry_snapshot_fingerprint,
                   "Core compiled-plan registry_snapshot_fingerprint");

    auto sorted_closure = executable_closure;
    for (const auto& identity : sorted_closure)
        validate_executable(identity);
    std::sort(sorted_closure.begin(), sorted_closure.end(), executable_identity_less);
    if (std::adjacent_find(sorted_closure.begin(), sorted_closure.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.kind == rhs.kind && lhs.name == rhs.name;
                           }) != sorted_closure.end()) {
        throw std::invalid_argument(
            "Core compiled-plan executable closure contains duplicate or ambiguous identities");
    }

    json encoded_closure = json::array();
    for (const auto& identity : sorted_closure)
        encoded_closure.push_back(encode_executable(identity));

    json envelope                             = json::object();
    envelope["definition_hash"]               = definition.definition_hash;
    envelope["definition"]                    = definition.definition;
    envelope["compiler_build_id"]             = std::string(compiler_build_id);
    envelope["registry_snapshot_fingerprint"] = std::string(registry_snapshot_fingerprint);
    envelope["executable_closure"]            = std::move(encoded_closure);
    return detail::sha256_identity("core-compiled-plan/v1", detail::canonical_json_bytes(envelope));
}

struct ProgramBundle::Impl {
    ProgramBundleData          data;
    std::string                id;
    std::string                canonical_bytes;
    std::optional<ProgramPlan> typed_plan;
};

ProgramBundle::ProgramBundle(ProgramBundleData data) {
    ProgramBundleData owned(data);
    normalize_data(owned);
    auto impl   = std::make_shared<Impl>();
    impl->data  = std::move(owned);
    auto value  = bundle_identity_envelope(impl->data);
    impl->id    = detail::sha256_identity("program-bundle", detail::canonical_json_bytes(value));
    value["id"] = impl->id;
    impl->canonical_bytes = detail::canonical_json_bytes(value);
    // Keep legacy raw bundle construction permissive, but eagerly seal compiler-produced
    // orchestration plans into the typed immutable scheduler view when possible. Admission and
    // runtime both fail closed if this field is unavailable for an admitted artifact.
    try {
        impl->typed_plan = ProgramPlan::from_json(impl->data.orchestration_plan.plan);
    } catch (const std::exception&) {
        impl->typed_plan.reset();
    }
    impl_ = std::move(impl);
}

ProgramBundle::ProgramBundle(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramBundle ProgramBundle::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramBundle JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != BUNDLE_FORMAT) {
        throw std::invalid_argument("Stored ProgramBundle has unknown format");
    }
    detail::reject_unknown_fields(value, "Stored ProgramBundle",
                                  {"format",
                                   "storage_schema_version",
                                   "id",
                                   "source_kind",
                                   "source_hash",
                                   "control_source",
                                   "canonical_program_hash",
                                   "compiler_build_id",
                                   "program_schema_version",
                                   "registry_snapshot_fingerprint",
                                   "module_dependency_merkle_root",
                                   "module_coordinates",
                                   "input_contract",
                                   "output_contract",
                                   "orchestration_plan",
                                   "sealed_core_definitions",
                                   "core_plan_identities",
                                   "capability_effect_closure",
                                   "execution_guarantee",
                                   "executable_registry_identities",
                                   "declared_budget_requirements",
                                   "source_map",
                                   "diagnostics"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramBundle schema version is unsupported");
    }
    ProgramBundle parsed(parse_body(value));
    const auto    stored_id = require_string(value, "id");
    if (!detail::is_sha256_identity(stored_id) || stored_id != parsed.id()) {
        throw std::invalid_argument("Stored ProgramBundle id does not match its content");
    }
    return parsed;
}

const std::string& ProgramBundle::id() const noexcept {
    return impl_->id;
}
SourceKind ProgramBundle::source_kind() const noexcept {
    return impl_->data.source_kind;
}
const std::string& ProgramBundle::source_hash() const noexcept {
    return impl_->data.source_hash;
}
std::optional<ProgramSource> ProgramBundle::control_source() const {
    return impl_->data.control_source;
}
const std::string& ProgramBundle::canonical_program_hash() const noexcept {
    return impl_->data.canonical_program_hash;
}
const std::string& ProgramBundle::compiler_build_id() const noexcept {
    return impl_->data.compiler_build_id;
}
std::uint32_t ProgramBundle::program_schema_version() const noexcept {
    return impl_->data.program_schema_version;
}
const std::string& ProgramBundle::registry_snapshot_fingerprint() const noexcept {
    return impl_->data.registry_snapshot_fingerprint;
}
const std::string& ProgramBundle::module_dependency_merkle_root() const noexcept {
    return impl_->data.module_dependency_merkle_root;
}
const std::vector<ModuleCoordinate>& ProgramBundle::module_coordinates() const noexcept {
    return impl_->data.module_coordinates;
}
ContractRecord ProgramBundle::input_contract() const {
    auto copy   = impl_->data.input_contract;
    copy.schema = detail::owned_json_copy(copy.schema);
    return copy;
}
ContractRecord ProgramBundle::output_contract() const {
    auto copy   = impl_->data.output_contract;
    copy.schema = detail::owned_json_copy(copy.schema);
    return copy;
}
OrchestrationPlanRecord ProgramBundle::orchestration_plan() const {
    auto copy = impl_->data.orchestration_plan;
    copy.plan = detail::owned_json_copy(copy.plan);
    return copy;
}
const ProgramPlan& ProgramBundle::typed_orchestration_plan() const {
    if (!impl_->typed_plan)
        throw std::invalid_argument(
            "Program bundle does not contain a valid typed orchestration plan");
    return *impl_->typed_plan;
}
std::vector<SealedCoreDefinition> ProgramBundle::sealed_core_definitions() const {
    auto copy = impl_->data.sealed_core_definitions;
    for (auto& definition : copy) {
        definition.definition = detail::owned_json_copy(definition.definition);
    }
    return copy;
}
const std::vector<CorePlanIdentity>& ProgramBundle::core_plan_identities() const noexcept {
    return impl_->data.core_plan_identities;
}
const CapabilityEffectClosure& ProgramBundle::capability_effect_closure() const noexcept {
    return impl_->data.capability_effect_closure;
}
ExecutionGuarantee ProgramBundle::execution_guarantee() const noexcept {
    return impl_->data.execution_guarantee;
}
const std::vector<ExecutableIdentity>& ProgramBundle::executable_registry_identities()
    const noexcept {
    return impl_->data.executable_registry_identities;
}
const std::vector<BudgetRequirement>& ProgramBundle::declared_budget_requirements() const noexcept {
    return impl_->data.declared_budget_requirements;
}
const std::vector<SourceMapEntry>& ProgramBundle::source_map() const noexcept {
    return impl_->data.source_map;
}
std::vector<Diagnostic> ProgramBundle::diagnostics() const {
    auto copy = impl_->data.diagnostics;
    for (auto& diagnostic : copy) {
        diagnostic.witness = detail::owned_json_copy(diagnostic.witness);
    }
    return copy;
}

std::string ProgramBundle::serialize_canonical() const {
    return impl_->canonical_bytes;
}

}  // namespace neograph::program
