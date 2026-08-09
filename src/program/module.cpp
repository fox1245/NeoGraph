#include <neograph/program/module.h>

#include "canonical_json.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace neograph::program {
namespace {

void require_nonempty(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void require_digest(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value))
        throw std::invalid_argument(std::string(field) + " must be a sha256: identity");
}

void require_semver(std::string_view value, std::string_view field) {
    if (!detail::is_semantic_version(value))
        throw std::invalid_argument(std::string(field) + " must be valid SemVer");
}

std::string required_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value[key].is_string())
        throw std::invalid_argument(key + " must be a string");
    auto result = value[key].get<std::string>();
    detail::validate_utf8(result);
    return result;
}

std::uint64_t required_u64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value[key].is_number_unsigned())
        throw std::invalid_argument(key + " must be an unsigned integer");
    return value[key].get<unsigned long long>();
}

std::uint32_t required_u32(const json& value, std::string_view field) {
    const auto number = required_u64(value, field);
    if (number > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument(std::string(field) + " is outside the uint32 range");
    return static_cast<std::uint32_t>(number);
}

void require_object(const json& value, std::string_view field) {
    if (!value.is_object()) throw std::invalid_argument(std::string(field) + " must be an object");
}

void require_array(const json& value, std::string_view field) {
    if (!value.is_array()) throw std::invalid_argument(std::string(field) + " must be an array");
}

std::vector<std::string> parse_strings(const json& value, std::string_view field) {
    require_array(value, field);
    std::vector<std::string> result;
    for (const auto& item : value) {
        if (!item.is_string())
            throw std::invalid_argument(std::string(field) + " must contain strings");
        auto string_value = item.get<std::string>();
        detail::validate_utf8(string_value);
        result.push_back(std::move(string_value));
    }
    std::sort(result.begin(), result.end());
    if (std::adjacent_find(result.begin(), result.end()) != result.end())
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    return result;
}

json encode_budget(const BudgetLimits& budget) {
    return json{{"wall_time_ms", budget.wall_time_ms},
                {"model_tokens", budget.model_tokens},
                {"monetary_microunits", budget.monetary_microunits},
                {"max_concurrency", budget.max_concurrency},
                {"max_program_operations", budget.max_program_operations},
                {"max_core_steps", budget.max_core_steps},
                {"max_dynamic_compiles", budget.max_dynamic_compiles},
                {"max_child_depth", budget.max_child_depth},
                {"max_total_children", budget.max_total_children}};
}

BudgetLimits parse_budget(const json& value) {
    require_object(value, "child budget");
    detail::reject_unknown_fields(
        value, "child budget",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    return BudgetLimits{required_u64(value, "wall_time_ms"),
                        required_u64(value, "model_tokens"),
                        required_u64(value, "monetary_microunits"),
                        required_u32(value, "max_concurrency"),
                        required_u64(value, "max_program_operations"),
                        required_u64(value, "max_core_steps"),
                        required_u32(value, "max_dynamic_compiles"),
                        required_u32(value, "max_child_depth"),
                        required_u64(value, "max_total_children")};
}

json encode_coordinate_without_identity(const ModuleCoordinate& coordinate) {
    return json{{"namespace", coordinate.namespace_name},
                {"name", coordinate.name},
                {"semantic_version", coordinate.semantic_version}};
}

json encode_coordinate(const ModuleCoordinate& coordinate) {
    auto result = encode_coordinate_without_identity(coordinate);
    result["content_identity"] = coordinate.content_identity;
    return result;
}

ModuleCoordinate parse_coordinate(const json& value) {
    require_object(value, "module coordinate");
    detail::reject_unknown_fields(value, "module coordinate",
                                  {"namespace", "name", "semantic_version", "content_identity"});
    ModuleCoordinate result{required_string(value, "namespace"), required_string(value, "name"),
                            required_string(value, "semantic_version"),
                            required_string(value, "content_identity")};
    require_nonempty(result.namespace_name, "Module namespace");
    require_nonempty(result.name, "Module name");
    require_semver(result.semantic_version, "Module semantic_version");
    require_digest(result.content_identity, "Module content_identity");
    return result;
}

json encode_contract(const ContractRecord& contract) {
    return json{{"schema_version", contract.schema_version}, {"schema", contract.schema}};
}

ContractRecord parse_contract(const json& value) {
    require_object(value, "module port contract");
    detail::reject_unknown_fields(value, "module port contract", {"schema_version", "schema"});
    ContractRecord result{required_u32(value, "schema_version"),
                          value.contains("schema") ? value["schema"] : json::object()};
    validate_contract_schema(result, "module port contract");
    return result;
}

json encode_port(const ModulePort& port) {
    return json{{"name", port.name}, {"contract", encode_contract(port.contract)}};
}

ModulePort parse_port(const json& value) {
    require_object(value, "module port");
    detail::reject_unknown_fields(value, "module port", {"name", "contract"});
    ModulePort result{required_string(value, "name"), parse_contract(value.at("contract"))};
    require_nonempty(result.name, "Module port name");
    return result;
}

json encode_child(const ChildProgramDescriptor& child) {
    json inputs = json::array();
    for (const auto& port : child.inputs) inputs.push_back(encode_port(port));
    json outputs = json::array();
    for (const auto& port : child.outputs) outputs.push_back(encode_port(port));
    return json{{"name", child.name},
                {"program_version_id", child.program_version_id},
                {"inputs", std::move(inputs)},
                {"outputs", std::move(outputs)},
                {"required_capabilities", child.required_capabilities},
                {"required_effects", child.required_effects},
                {"budget", encode_budget(child.budget)},
                {"minimum_execution_guarantee",
                 std::string(to_string(child.minimum_execution_guarantee))}};
}

ChildProgramDescriptor parse_child(const json& value) {
    require_object(value, "child program");
    detail::reject_unknown_fields(value, "child program",
                                  {"name", "program_version_id", "inputs", "outputs",
                                   "required_capabilities", "required_effects", "budget",
                                   "minimum_execution_guarantee"});
    ChildProgramDescriptor result;
    result.name = required_string(value, "name");
    result.program_version_id = required_string(value, "program_version_id");
    require_digest(result.program_version_id, "Child program_version_id");
    require_array(value.at("inputs"), "child inputs");
    require_array(value.at("outputs"), "child outputs");
    for (const auto& item : value.at("inputs")) result.inputs.push_back(parse_port(item));
    for (const auto& item : value.at("outputs")) result.outputs.push_back(parse_port(item));
    result.required_capabilities = parse_strings(value.at("required_capabilities"),
                                                 "required_capabilities");
    result.required_effects = parse_strings(value.at("required_effects"), "required_effects");
    result.budget = parse_budget(value.at("budget"));
    result.minimum_execution_guarantee =
        execution_guarantee_from_string(required_string(value, "minimum_execution_guarantee"));
    return result;
}

json structural_body(const ProgramModuleData& data) {
    json dependencies = json::array();
    for (const auto& dependency : data.dependencies) dependencies.push_back(encode_coordinate(dependency));
    json children = json::array();
    for (const auto& child : data.children) children.push_back(encode_child(child));
    return json{{"owner_scope", data.owner_scope},
                {"coordinate", encode_coordinate_without_identity(data.coordinate)},
                {"attestation_id", data.attestation_id},
                {"dependencies", std::move(dependencies)},
                {"children", std::move(children)},
                {"allowed_capabilities", data.allowed_capabilities},
                {"declared_effects", data.declared_effects}};
}

std::string structural_identity(const ProgramModuleData& data) {
    return detail::sha256_identity("program-module/v1", detail::canonical_json_bytes(structural_body(data)));
}

bool contains(const std::vector<std::string>& values, std::string_view wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

void validate_ports(const std::vector<ModulePort>& ports, std::string_view field) {
    std::set<std::string> names;
    for (const auto& port : ports) {
        require_nonempty(port.name, "Module port name");
        if (!names.insert(port.name).second)
            throw std::invalid_argument(std::string(field) + " contains duplicate port names");
        validate_contract_schema(port.contract, std::string(field) + "/" + port.name);
    }
}

void normalize_child_requirements(ChildProgramDescriptor& child) {
    std::sort(child.required_capabilities.begin(), child.required_capabilities.end());
    std::sort(child.required_effects.begin(), child.required_effects.end());
    if (std::adjacent_find(child.required_capabilities.begin(),
                           child.required_capabilities.end()) != child.required_capabilities.end())
        throw std::invalid_argument("Child required_capabilities contains a duplicate");
    if (std::adjacent_find(child.required_effects.begin(), child.required_effects.end()) !=
        child.required_effects.end())
        throw std::invalid_argument("Child required_effects contains a duplicate");
}

void validate_module_data(ProgramModuleData& data) {
    require_nonempty(data.owner_scope, "Module owner_scope");
    require_nonempty(data.coordinate.namespace_name, "Module namespace");
    require_nonempty(data.coordinate.name, "Module name");
    require_semver(data.coordinate.semantic_version, "Module semantic_version");
    require_nonempty(data.attestation_id, "Module attestation_id");
    for (auto& dependency : data.dependencies) {
        require_nonempty(dependency.namespace_name, "Module dependency namespace");
        require_nonempty(dependency.name, "Module dependency name");
        require_semver(dependency.semantic_version, "Module dependency semantic_version");
        require_digest(dependency.content_identity, "Module dependency content_identity");
    }
    std::sort(data.dependencies.begin(), data.dependencies.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.namespace_name, lhs.name, lhs.semantic_version, lhs.content_identity) <
               std::tie(rhs.namespace_name, rhs.name, rhs.semantic_version, rhs.content_identity);
    });
    if (std::adjacent_find(data.dependencies.begin(), data.dependencies.end()) != data.dependencies.end())
        throw std::invalid_argument("Module dependencies contain a duplicate coordinate");

    std::sort(data.children.begin(), data.children.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.name < rhs.name;
    });
    std::sort(data.allowed_capabilities.begin(), data.allowed_capabilities.end());
    std::sort(data.declared_effects.begin(), data.declared_effects.end());
    if (std::adjacent_find(data.allowed_capabilities.begin(), data.allowed_capabilities.end()) !=
        data.allowed_capabilities.end())
        throw std::invalid_argument("Module allowed_capabilities contains a duplicate");
    if (std::adjacent_find(data.declared_effects.begin(), data.declared_effects.end()) !=
        data.declared_effects.end())
        throw std::invalid_argument("Module declared_effects contains a duplicate");
    for (auto& child : data.children) {
        normalize_child_requirements(child);
        require_nonempty(child.name, "Child name");
        require_digest(child.program_version_id, "Child program_version_id");
        validate_ports(child.inputs, "child inputs");
        validate_ports(child.outputs, "child outputs");
        for (const auto& capability : child.required_capabilities) {
            require_nonempty(capability, "Child capability");
            if (!contains(data.allowed_capabilities, capability))
                throw std::invalid_argument("Child capability widens module authority");
        }
        for (const auto& effect : child.required_effects) {
            require_nonempty(effect, "Child effect");
            if (!contains(data.declared_effects, effect))
                throw std::invalid_argument("Child effect widens module authority");
        }
        if (execution_guarantee_rank(child.minimum_execution_guarantee) == 0) {
            throw std::invalid_argument("Child minimum_execution_guarantee is unsupported");
        }
    }
    if (std::adjacent_find(data.children.begin(), data.children.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.name == rhs.name; }) !=
        data.children.end())
        throw std::invalid_argument("Module children contain a duplicate name");
}

json module_body(const ProgramModuleData& data, std::string_view id) {
    auto body = structural_body(data);
    body["format"] = "neograph-program-module";
    body["storage_schema_version"] = ProgramModule::STORAGE_SCHEMA_VERSION;
    body["id"] = std::string(id);
    body["coordinate"]["content_identity"] = std::string(id);
    body["lifecycle"] = std::string(to_string(data.lifecycle));
    return body;
}

std::string dependency_root(std::vector<DependencyReceipt> receipts) {
    std::sort(receipts.begin(), receipts.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.dependency_id, lhs.content_identity) <
               std::tie(rhs.dependency_id, rhs.content_identity);
    });
    if (receipts.empty()) return detail::sha256_identity("program-import-empty/v1", "");
    std::vector<std::string> level;
    for (const auto& receipt : receipts) {
        const json leaf{{"source_id", receipt.dependency_id},
                        {"content_identity", receipt.content_identity}};
        level.push_back(detail::sha256_identity("program-import-leaf/v1",
                                                detail::canonical_json_bytes(leaf)));
    }
    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());
        std::vector<std::string> next;
        for (std::size_t index = 0; index < level.size(); index += 2)
            next.push_back(detail::sha256_identity("program-import-node/v1",
                                                   level[index] + level[index + 1]));
        level = std::move(next);
    }
    return level.front();
}

json contract_body(const ContractRecord& contract) {
    return json{{"schema_version", contract.schema_version}, {"schema", contract.schema}};
}

std::string contract_identity(const ContractRecord& contract) {
    return detail::sha256_identity("program-module-port-contract/v1",
                                   detail::canonical_json_bytes(contract_body(contract)));
}

bool contains_all(const std::vector<std::string>& available,
                  const std::vector<std::string>& requested) {
    return std::all_of(requested.begin(), requested.end(), [&](const auto& value) {
        return std::find(available.begin(), available.end(), value) != available.end();
    });
}

bool budget_leq(const BudgetLimits& lhs, const BudgetLimits& rhs) {
    return lhs.wall_time_ms <= rhs.wall_time_ms &&
           lhs.model_tokens <= rhs.model_tokens &&
           lhs.monetary_microunits <= rhs.monetary_microunits &&
           lhs.max_concurrency <= rhs.max_concurrency &&
           lhs.max_program_operations <= rhs.max_program_operations &&
           lhs.max_core_steps <= rhs.max_core_steps &&
           lhs.max_dynamic_compiles <= rhs.max_dynamic_compiles &&
           lhs.max_child_depth <= rhs.max_child_depth &&
           lhs.max_total_children <= rhs.max_total_children;
}

bool budget_positive(const BudgetLimits& budget) {
    // Zero is a valid attenuation for an unused resource (notably dynamic
    // compilation and nested children). Execution itself still needs a
    // positive operation, step, and concurrency allowance.
    return budget.max_concurrency != 0 && budget.max_program_operations != 0 &&
           budget.max_core_steps != 0 &&
           budget.max_child_depth <= MAX_SUPPORTED_CHILD_DEPTH;
}

bool budget_covers_requirements(const BudgetLimits& budget,
                                const std::vector<BudgetRequirement>& requirements) {
    for (const auto& requirement : requirements) {
        std::uint64_t granted = 0;
        if (requirement.resource == "wall_time_ms")
            granted = budget.wall_time_ms;
        else if (requirement.resource == "model_tokens")
            granted = budget.model_tokens;
        else if (requirement.resource == "monetary_microunits")
            granted = budget.monetary_microunits;
        else if (requirement.resource == "max_concurrency")
            granted = budget.max_concurrency;
        else if (requirement.resource == "max_program_operations")
            granted = budget.max_program_operations;
        else if (requirement.resource == "max_core_steps")
            granted = budget.max_core_steps;
        else if (requirement.resource == "max_dynamic_compiles")
            granted = budget.max_dynamic_compiles;
        else if (requirement.resource == "max_child_depth")
            granted = budget.max_child_depth;
        else if (requirement.resource == "max_total_children")
            granted = budget.max_total_children;
        else
            return false;
        if (granted < requirement.maximum) return false;
    }
    return true;
}

json link_body(const ModuleLinkReceiptData& data, std::string_view id) {
    return json{{"format", "neograph-program-module-link"},
                {"storage_schema_version", ModuleLinkReceipt::STORAGE_SCHEMA_VERSION},
                {"id", std::string(id)},
                {"owner_scope", data.owner_scope},
                {"parent_module_id", data.parent_module_id},
                {"dependency_merkle_root", data.dependency_merkle_root},
                {"child_name", data.child_name},
                {"child_program_version_id", data.child_program_version_id},
                {"child_bundle_id", data.child_bundle_id},
                {"child_input_contract_fingerprint", data.child_input_contract_fingerprint},
                {"child_output_contract_fingerprint", data.child_output_contract_fingerprint},
                {"granted_capabilities", data.granted_capabilities},
                {"granted_effects", data.granted_effects},
                {"budget", encode_budget(data.budget)},
                {"minimum_execution_guarantee",
                 std::string(to_string(data.minimum_execution_guarantee))}};
}

std::string link_identity(const ModuleLinkReceiptData& data) {
    return detail::sha256_identity(
        "program-module-link/v1",
        detail::canonical_json_bytes(link_body(data, "")));
}

void validate_link_data(ModuleLinkReceiptData& data) {
    require_nonempty(data.owner_scope, "Module link owner_scope");
    require_digest(data.parent_module_id, "Module link parent_module_id");
    require_digest(data.dependency_merkle_root, "Module link dependency_merkle_root");
    require_nonempty(data.child_name, "Module link child_name");
    require_digest(data.child_program_version_id, "Module link child_program_version_id");
    require_digest(data.child_bundle_id, "Module link child_bundle_id");
    require_digest(data.child_input_contract_fingerprint,
                   "Module link child_input_contract_fingerprint");
    require_digest(data.child_output_contract_fingerprint,
                   "Module link child_output_contract_fingerprint");
    if (!budget_positive(data.budget))
        throw std::invalid_argument("Module link budget must contain positive finite limits");
    std::sort(data.granted_capabilities.begin(), data.granted_capabilities.end());
    std::sort(data.granted_effects.begin(), data.granted_effects.end());
    if (std::adjacent_find(data.granted_capabilities.begin(),
                           data.granted_capabilities.end()) != data.granted_capabilities.end())
        throw std::invalid_argument("Module link capabilities contain a duplicate");
    if (std::adjacent_find(data.granted_effects.begin(), data.granted_effects.end()) !=
        data.granted_effects.end())
        throw std::invalid_argument("Module link effects contain a duplicate");
    for (const auto& value : data.granted_capabilities)
        require_nonempty(value, "Module link capability");
    for (const auto& value : data.granted_effects) require_nonempty(value, "Module link effect");
    if (execution_guarantee_rank(data.minimum_execution_guarantee) == 0) {
        throw std::invalid_argument("Module link minimum_execution_guarantee is unsupported");
    }
}

}  // namespace

std::string_view to_string(ModuleLifecycle state) noexcept {
    switch (state) {
        case ModuleLifecycle::Active:
            return "active";
        case ModuleLifecycle::Deprecated:
            return "deprecated";
        case ModuleLifecycle::Quarantined:
            return "quarantined";
        case ModuleLifecycle::Revoked:
            return "revoked";
    }
    return "unknown";
}

ModuleLifecycle module_lifecycle_from_string(std::string_view value) {
    if (value == "active") return ModuleLifecycle::Active;
    if (value == "deprecated") return ModuleLifecycle::Deprecated;
    if (value == "quarantined") return ModuleLifecycle::Quarantined;
    if (value == "revoked") return ModuleLifecycle::Revoked;
    throw std::invalid_argument("Unknown module lifecycle: " + std::string(value));
}

std::string ModuleCoordinate::qualified_name() const {
    return namespace_name + ":" + name + "@" + semantic_version;
}

struct ProgramModule::Impl {
    ProgramModuleData data;
    std::string       id;
};

ProgramModule::ProgramModule(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramModule ProgramModule::create(ProgramModuleData data) {
    validate_module_data(data);
    const auto computed = structural_identity(data);
    if (!data.coordinate.content_identity.empty() && data.coordinate.content_identity != computed)
        throw std::invalid_argument("Module content_identity does not match its immutable body");
    data.coordinate.content_identity = computed;
    auto result = std::make_shared<Impl>();
    result->id = computed;
    result->data = std::move(data);
    return ProgramModule(std::move(result));
}

ProgramModule ProgramModule::with_lifecycle(const ProgramModule& module, ModuleLifecycle lifecycle) {
    auto data = module.impl_->data;
    data.lifecycle = lifecycle;
    return create(std::move(data));
}

ProgramModule ProgramModule::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ProgramModule");
    detail::reject_unknown_fields(value, "Stored ProgramModule",
                                  {"format", "storage_schema_version", "id", "owner_scope",
                                   "coordinate", "attestation_id", "lifecycle", "dependencies",
                                   "children", "allowed_capabilities", "declared_effects"});
    if (required_string(value, "format") != "neograph-program-module")
        throw std::invalid_argument("Stored ProgramModule has unknown format");
    if (required_u32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ProgramModule schema version is unsupported");
    const auto stored_id = required_string(value, "id");
    require_digest(stored_id, "Stored ProgramModule id");
    ProgramModuleData data;
    data.owner_scope = required_string(value, "owner_scope");
    data.coordinate = parse_coordinate(value.at("coordinate"));
    data.attestation_id = required_string(value, "attestation_id");
    data.lifecycle = module_lifecycle_from_string(required_string(value, "lifecycle"));
    require_array(value.at("dependencies"), "module dependencies");
    for (const auto& item : value.at("dependencies")) data.dependencies.push_back(parse_coordinate(item));
    require_array(value.at("children"), "module children");
    for (const auto& item : value.at("children")) data.children.push_back(parse_child(item));
    data.allowed_capabilities = parse_strings(value.at("allowed_capabilities"), "allowed_capabilities");
    data.declared_effects = parse_strings(value.at("declared_effects"), "declared_effects");
    auto result = create(std::move(data));
    if (result.id() != stored_id || result.coordinate().content_identity != stored_id)
        throw std::invalid_argument("Stored ProgramModule id does not match its body");
    if (result.coordinate() != parse_coordinate(value.at("coordinate")))
        throw std::invalid_argument("Stored ProgramModule coordinate does not match its body");
    return result;
}

const std::string& ProgramModule::owner_scope() const noexcept { return impl_->data.owner_scope; }
const ModuleCoordinate& ProgramModule::coordinate() const noexcept { return impl_->data.coordinate; }
const std::string& ProgramModule::attestation_id() const noexcept { return impl_->data.attestation_id; }
ModuleLifecycle ProgramModule::lifecycle() const noexcept { return impl_->data.lifecycle; }
const std::vector<ModuleCoordinate>& ProgramModule::dependencies() const noexcept { return impl_->data.dependencies; }
const std::vector<ChildProgramDescriptor>& ProgramModule::children() const noexcept { return impl_->data.children; }
const std::vector<std::string>& ProgramModule::allowed_capabilities() const noexcept { return impl_->data.allowed_capabilities; }
const std::vector<std::string>& ProgramModule::declared_effects() const noexcept { return impl_->data.declared_effects; }
const std::string& ProgramModule::id() const noexcept { return impl_->id; }
std::string ProgramModule::serialize_canonical() const {
    return detail::canonical_json_bytes(module_body(impl_->data, impl_->id));
}

std::string ModuleResolution::dependency_merkle_root() const {
    return ::neograph::program::dependency_root(receipts);
}

std::vector<ImportRef> ModuleResolution::imports() const {
    std::vector<ImportRef> result;
    result.reserve(receipts.size());
    for (const auto& receipt : receipts) result.push_back({receipt.dependency_id, receipt.content_identity});
    return result;
}

std::vector<ModuleCoordinate> ModuleResolution::coordinates() const {
    std::vector<ModuleCoordinate> result;
    result.reserve(modules.size());
    for (const auto& module : modules) result.push_back(module.coordinate());
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.namespace_name, lhs.name, lhs.semantic_version,
                        lhs.content_identity) <
               std::tie(rhs.namespace_name, rhs.name, rhs.semantic_version,
                        rhs.content_identity);
    });
    return result;
}

void validate_module_resolution(const ModuleResolution& resolution) {
    require_digest(resolution.root.content_identity, "Module resolution root content_identity");
    if (resolution.modules.empty())
        throw std::invalid_argument("Module resolution must contain its root module");
    const auto root = std::find_if(resolution.modules.begin(), resolution.modules.end(),
                                   [&](const auto& module) {
                                       return module.id() == resolution.root.content_identity;
                                   });
    if (root == resolution.modules.end() || root->coordinate() != resolution.root)
        throw std::invalid_argument("Module resolution root is not an exact resolved module");

    std::map<std::string, const ProgramModule*, std::less<>> by_id;
    std::set<std::string, std::less<>> qualified_names;
    for (const auto& module : resolution.modules) {
        if (!by_id.emplace(module.id(), &module).second)
            throw std::invalid_argument("Module resolution contains a duplicate content identity");
        if (!qualified_names.insert(module.coordinate().qualified_name()).second)
            throw std::invalid_argument("Module resolution contains a coordinate collision");
        if (module.coordinate().content_identity != module.id())
            throw std::invalid_argument("Module coordinate identity does not match module id");
        if (module.lifecycle() != ModuleLifecycle::Active)
            throw std::invalid_argument("Module resolution contains a non-active module");
    }
    for (const auto& module : resolution.modules) {
        for (const auto& dependency : module.dependencies()) {
            const auto found = by_id.find(dependency.content_identity);
            if (found == by_id.end() || found->second->coordinate() != dependency)
                throw std::invalid_argument("Module resolution contains an unpinned dependency");
        }
    }
    std::set<std::string, std::less<>> active;
    std::set<std::string, std::less<>> visited;
    std::function<void(const ProgramModule&)> visit = [&](const ProgramModule& module) {
        if (!active.insert(module.id()).second)
            throw std::invalid_argument("Module resolution dependency cycle detected");
        if (!visited.insert(module.id()).second) {
            active.erase(module.id());
            return;
        }
        for (const auto& dependency : module.dependencies()) visit(*by_id.at(dependency.content_identity));
        active.erase(module.id());
    };
    visit(*root);
    if (visited.size() != by_id.size())
        throw std::invalid_argument("Module resolution contains an unreachable module");

    // A one-module, dependency-free link assembled by legacy callers has no
    // receipt to carry. Any real closure must carry a receipt for every exact
    // coordinate; accepting an omitted receipt in a multi-module graph would
    // make an import unpinned.
    if (resolution.receipts.empty()) {
        if (by_id.size() != 1 || !root->dependencies().empty())
            throw std::invalid_argument("Module resolution has unpinned imports");
        return;
    }
    std::set<std::string, std::less<>> receipt_ids;
    for (const auto& receipt : resolution.receipts) {
        require_nonempty(receipt.dependency_id, "Module dependency receipt id");
        require_digest(receipt.content_identity, "Module dependency receipt identity");
        if (!receipt_ids.insert(receipt.dependency_id).second)
            throw std::invalid_argument("Module resolution contains duplicate dependency receipts");
        const auto found = by_id.find(receipt.content_identity);
        if (found == by_id.end() || found->second->coordinate().qualified_name() != receipt.dependency_id)
            throw std::invalid_argument("Module dependency receipt is not bound to its coordinate");
    }
    if (receipt_ids.size() != by_id.size())
        throw std::invalid_argument("Module resolution receipts do not cover the full closure");
}

namespace {

std::uint64_t budget_value(const BudgetLimits& budget, std::string_view resource) {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    return 0;
}

void add_budget(BudgetLimits& total, const BudgetLimits& value) {
    total.wall_time_ms += value.wall_time_ms;
    total.model_tokens += value.model_tokens;
    total.monetary_microunits += value.monetary_microunits;
    total.max_concurrency += value.max_concurrency;
    total.max_program_operations += value.max_program_operations;
    total.max_core_steps += value.max_core_steps;
    total.max_dynamic_compiles += value.max_dynamic_compiles;
    total.max_child_depth += value.max_child_depth;
    total.max_total_children += value.max_total_children;
}

}  // namespace

void validate_program_composition(const ProgramBundle& parent_bundle,
                                  const ProgramComposition& composition) {
    validate_module_resolution(composition.resolution);
    if (composition.parent.lifecycle() != ModuleLifecycle::Active)
        throw std::invalid_argument("Cannot compose a non-active parent module");
    if (composition.resolution.root != composition.parent.coordinate())
        throw std::invalid_argument("Composition resolution root does not match its parent module");
    if (parent_bundle.module_dependency_merkle_root() !=
        composition.resolution.dependency_merkle_root())
        throw std::invalid_argument("Parent bundle does not pin the module dependency closure");
    if (!composition.resolution.receipts.empty() &&
        parent_bundle.module_coordinates() != composition.resolution.coordinates())
        throw std::invalid_argument("Parent bundle module coordinates differ from the resolution");
    const auto& parent_closure = parent_bundle.capability_effect_closure();
    if (!contains_all(composition.parent.allowed_capabilities(), parent_closure.capabilities) ||
        !contains_all(composition.parent.declared_effects(), parent_closure.effects))
        throw std::invalid_argument("Parent executable closure exceeds module authority");

    std::map<std::string, const ChildProgramBinding*, std::less<>> by_name;
    std::set<std::string, std::less<>> bundle_ids;
    std::set<std::string, std::less<>> version_ids;
    for (const auto& child : composition.children) {
        if (!by_name.emplace(child.child_name, &child).second)
            throw std::invalid_argument("Composition contains a child-name collision");
        if (!bundle_ids.insert(child.bundle.id()).second ||
            !version_ids.insert(child.version.id()).second)
            throw std::invalid_argument("Composition contains a child identity collision");
        if (child.bundle.id() != child.version.bundle_id())
            throw std::invalid_argument("Composition child version is bound to another bundle");
        (void)link_module_child(composition.resolution, composition.parent, child.child_name,
                                child.bundle, child.version);
    }
    if (by_name.size() != composition.parent.children().size())
        throw std::invalid_argument("Composition does not bind every declared child");

    BudgetLimits aggregate;
    for (const auto& descriptor : composition.parent.children()) {
        const auto child = by_name.find(descriptor.name);
        if (child == by_name.end())
            throw std::invalid_argument("Composition is missing a declared child");
        if (child->second->version.id() != descriptor.program_version_id)
            throw std::invalid_argument("Composition child version does not match its descriptor");
        if (execution_guarantee_rank(child->second->version.execution_guarantee()) <
            execution_guarantee_rank(descriptor.minimum_execution_guarantee)) {
            throw std::invalid_argument(
                "Composition child guarantee falls below its explicitly accepted floor");
        }
        add_budget(aggregate, descriptor.budget);
    }
    const auto parent_plan = parent_bundle.orchestration_plan();
    std::set<std::string, std::less<>> referenced_children;
    if (!composition.parent.children().empty()) {
        if (!parent_plan.plan.is_object() || !parent_plan.plan.contains("operations") ||
            !parent_plan.plan["operations"].is_array()) {
            throw std::invalid_argument("Whole Program composition has no operation plan");
        }
        for (const auto& operation : parent_plan.plan["operations"]) {
            if (!operation.is_object() || operation.value("op", "") != "spawn") continue;
            if (!operation.contains("child_binding") || !operation["child_binding"].is_string() ||
                operation["child_binding"].get<std::string>().empty()) {
                throw std::invalid_argument(
                    "Whole Program composition has a spawn without a verified child binding");
            }
            const auto binding = operation["child_binding"].get<std::string>();
            if (!by_name.contains(binding)) {
                throw std::invalid_argument(
                    "Whole Program composition spawn references an undeclared child");
            }
            referenced_children.insert(std::move(binding));
        }
        for (const auto& descriptor : composition.parent.children()) {
            if (!referenced_children.contains(descriptor.name)) {
                throw std::invalid_argument(
                    "Whole Program composition has no spawn operation for a declared child");
            }
        }
    }

    std::map<std::string, const BudgetRequirement*, std::less<>> requirements;
    for (const auto& requirement : parent_bundle.declared_budget_requirements())
        requirements.emplace(requirement.resource, &requirement);
    for (const auto resource : {"wall_time_ms", "model_tokens", "monetary_microunits",
                                "max_concurrency", "max_program_operations", "max_core_steps",
                                "max_dynamic_compiles", "max_child_depth", "max_total_children"}) {
        const auto found = requirements.find(resource);
        if (found == requirements.end())
            throw std::invalid_argument("Whole Program composition has an incomplete budget closure");
        if (resource == std::string_view("max_child_depth") &&
            found->second->maximum > MAX_SUPPORTED_CHILD_DEPTH)
            throw std::invalid_argument("Child depth exceeds the supported hard ceiling");
        if (resource == std::string_view("max_child_depth")) {
            if (found->second->maximum < 1) throw std::invalid_argument("Child depth is not admitted");
        } else if (resource == std::string_view("max_total_children")) {
            if (found->second->maximum < composition.parent.children().size())
                throw std::invalid_argument("Child count exceeds the parent allocation");
        } else if (found->second->maximum < budget_value(aggregate, resource)) {
            throw std::invalid_argument("Child budget exceeds the parent allocation");
        }
    }
}


struct ModuleLinkReceipt::Impl {
    ModuleLinkReceiptData data;
    std::string           id;
};

ModuleLinkReceipt::ModuleLinkReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ModuleLinkReceipt ModuleLinkReceipt::create(ModuleLinkReceiptData data) {
    validate_link_data(data);
    const auto computed = link_identity(data);
    auto       result   = std::make_shared<Impl>();
    result->id          = computed;
    result->data        = std::move(data);
    return ModuleLinkReceipt(std::move(result));
}

ModuleLinkReceipt ModuleLinkReceipt::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    require_object(value, "Stored ModuleLinkReceipt");
    detail::reject_unknown_fields(
        value, "Stored ModuleLinkReceipt",
        {"format", "storage_schema_version", "id", "owner_scope", "parent_module_id",
         "dependency_merkle_root", "child_name", "child_program_version_id", "child_bundle_id",
         "child_input_contract_fingerprint", "child_output_contract_fingerprint",
         "granted_capabilities", "granted_effects", "budget", "minimum_execution_guarantee"});
    if (required_string(value, "format") != "neograph-program-module-link")
        throw std::invalid_argument("Stored ModuleLinkReceipt has unknown format");
    if (required_u32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored ModuleLinkReceipt schema version is unsupported");
    const auto stored_id = required_string(value, "id");
    require_digest(stored_id, "Stored ModuleLinkReceipt id");
    ModuleLinkReceiptData data;
    data.owner_scope = required_string(value, "owner_scope");
    data.parent_module_id = required_string(value, "parent_module_id");
    data.dependency_merkle_root = required_string(value, "dependency_merkle_root");
    data.child_name = required_string(value, "child_name");
    data.child_program_version_id = required_string(value, "child_program_version_id");
    data.child_bundle_id = required_string(value, "child_bundle_id");
    data.child_input_contract_fingerprint =
        required_string(value, "child_input_contract_fingerprint");
    data.child_output_contract_fingerprint =
        required_string(value, "child_output_contract_fingerprint");
    data.granted_capabilities = parse_strings(value.at("granted_capabilities"),
                                               "granted_capabilities");
    data.granted_effects = parse_strings(value.at("granted_effects"), "granted_effects");
    data.budget = parse_budget(value.at("budget"));
    data.minimum_execution_guarantee =
        execution_guarantee_from_string(required_string(value, "minimum_execution_guarantee"));
    auto result = create(std::move(data));
    if (result.id() != stored_id)
        throw std::invalid_argument("Stored ModuleLinkReceipt id does not match its body");
    return result;
}

const std::string& ModuleLinkReceipt::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& ModuleLinkReceipt::parent_module_id() const noexcept {
    return impl_->data.parent_module_id;
}
const std::string& ModuleLinkReceipt::dependency_merkle_root() const noexcept {
    return impl_->data.dependency_merkle_root;
}
const std::string& ModuleLinkReceipt::child_name() const noexcept {
    return impl_->data.child_name;
}
const std::string& ModuleLinkReceipt::child_program_version_id() const noexcept {
    return impl_->data.child_program_version_id;
}
const std::string& ModuleLinkReceipt::child_bundle_id() const noexcept {
    return impl_->data.child_bundle_id;
}
const std::string& ModuleLinkReceipt::child_input_contract_fingerprint() const noexcept {
    return impl_->data.child_input_contract_fingerprint;
}
const std::string& ModuleLinkReceipt::child_output_contract_fingerprint() const noexcept {
    return impl_->data.child_output_contract_fingerprint;
}
const std::vector<std::string>& ModuleLinkReceipt::granted_capabilities() const noexcept {
    return impl_->data.granted_capabilities;
}
const std::vector<std::string>& ModuleLinkReceipt::granted_effects() const noexcept {
    return impl_->data.granted_effects;
}
const BudgetLimits& ModuleLinkReceipt::budget() const noexcept { return impl_->data.budget; }
ExecutionGuarantee ModuleLinkReceipt::minimum_execution_guarantee() const noexcept {
    return impl_->data.minimum_execution_guarantee;
}
const std::string& ModuleLinkReceipt::id() const noexcept { return impl_->id; }
std::string ModuleLinkReceipt::serialize_canonical() const {
    return detail::canonical_json_bytes(link_body(impl_->data, impl_->id));
}

ModuleLinkReceipt link_module_child(const ModuleResolution& resolution,
                                    const ProgramModule&    parent,
                                    std::string_view        child_name,
                                    const ProgramBundle&    child_bundle,
                                    const ProgramVersion&   child) {
    if (parent.lifecycle() != ModuleLifecycle::Active)
        throw std::invalid_argument("Cannot link a non-active parent module");
    validate_module_resolution(resolution);
    if (resolution.root.content_identity != parent.id())
        throw std::invalid_argument("Module resolution root does not match parent module");
    const auto parent_it = std::find_if(
        resolution.modules.begin(), resolution.modules.end(),
        [&](const ProgramModule& module) { return module.id() == parent.id(); });
    if (parent_it == resolution.modules.end())
        throw std::invalid_argument("Module resolution does not contain the parent module");
    if (parent_it->owner_scope() != parent.owner_scope() ||
        parent_it->serialize_canonical() != parent.serialize_canonical())
        throw std::invalid_argument("Module resolution parent differs from supplied module");
    if (child.ownership_scope() != parent.owner_scope())
        throw std::invalid_argument("Child Program owner_scope differs from parent module");
    if (child_bundle.id() != child.bundle_id())
        throw std::invalid_argument("Child Program bundle identity does not match its version");
    if (child_bundle.module_dependency_merkle_root() != resolution.dependency_merkle_root())
        throw std::invalid_argument("Child Program module dependency closure is not pinned");
    if (!resolution.receipts.empty() &&
        child_bundle.module_coordinates() != resolution.coordinates())
        throw std::invalid_argument("Child Program module coordinates differ from the resolution");

    const auto descriptor_it = std::find_if(
        parent.children().begin(), parent.children().end(),
        [&](const ChildProgramDescriptor& descriptor) { return descriptor.name == child_name; });
    if (descriptor_it == parent.children().end())
        throw std::invalid_argument("Unknown child descriptor: " + std::string(child_name));
    const auto& descriptor = *descriptor_it;
    if (descriptor.program_version_id != child.id())
        throw std::invalid_argument("Child Program version does not match its descriptor");
    if (execution_guarantee_rank(child.execution_guarantee()) <
        execution_guarantee_rank(descriptor.minimum_execution_guarantee)) {
        throw std::invalid_argument(
            "Child Program guarantee falls below the module descriptor floor");
    }
    if (descriptor.inputs.size() != 1 || descriptor.outputs.size() != 1)
        throw std::invalid_argument("Module links require exactly one input and output port");

    const auto child_input = child_bundle.input_contract();
    const auto child_output = child_bundle.output_contract();
    const auto input_identity = contract_identity(child_input);
    const auto output_identity = contract_identity(child_output);
    if (contract_identity(descriptor.inputs.front().contract) != input_identity)
        throw std::invalid_argument("Child input port contract does not match its bundle");
    if (contract_identity(descriptor.outputs.front().contract) != output_identity)
        throw std::invalid_argument("Child output port contract does not match its bundle");

    const auto& closure = child_bundle.capability_effect_closure();
    if (!contains_all(descriptor.required_capabilities, closure.capabilities) ||
        !contains_all(descriptor.required_effects, closure.effects))
        throw std::invalid_argument("Child descriptor does not cover its executable closure");
    if (!contains_all(parent.allowed_capabilities(), descriptor.required_capabilities))
        throw std::invalid_argument("Child capabilities exceed parent authority");
    if (!contains_all(parent.declared_effects(), descriptor.required_effects))
        throw std::invalid_argument("Child effects exceed parent authority");
    const auto child_policy = child.policy_snapshot();
    if (!contains_all(child_policy.allowed_capabilities(), closure.capabilities) ||
        !contains_all(child_policy.allowed_effects(), closure.effects))
        throw std::invalid_argument("Child policy does not cover its executable closure");
    if (!contains_all(parent.allowed_capabilities(), child_policy.allowed_capabilities()) ||
        !contains_all(parent.declared_effects(), child_policy.allowed_effects()))
        throw std::invalid_argument("Child policy authority exceeds the parent module authority");
    if (!resolution.receipts.empty()) {
        const auto allowed_modules = child_policy.allowed_module_digests();
        for (const auto& module : resolution.modules)
            if (!contains(allowed_modules, module.id()))
                throw std::invalid_argument(
                    "Child policy does not pin its module dependency closure");
    }
    if (!budget_leq(descriptor.budget, child_policy.budget_ceiling()) ||
        !budget_covers_requirements(descriptor.budget,
                                    child_bundle.declared_budget_requirements()))
        throw std::invalid_argument("Child budget exceeds policy or declared requirements");

    ModuleLinkReceiptData data;
    data.owner_scope = parent.owner_scope();
    data.parent_module_id = parent.id();
    data.dependency_merkle_root = resolution.dependency_merkle_root();
    data.child_name = descriptor.name;
    data.child_program_version_id = child.id();
    data.child_bundle_id = child_bundle.id();
    data.child_input_contract_fingerprint = input_identity;
    data.child_output_contract_fingerprint = output_identity;
    data.granted_capabilities = descriptor.required_capabilities;
    data.granted_effects = descriptor.required_effects;
    data.budget = descriptor.budget;
    data.minimum_execution_guarantee = descriptor.minimum_execution_guarantee;
    return ModuleLinkReceipt::create(std::move(data));
}
struct InMemoryModuleStore::Impl {
    mutable std::mutex mutex;
    struct Entry {
        ProgramModule module;
        ModuleLifecycle lifecycle;
    };
    std::map<std::string, Entry, std::less<>> modules;
};

InMemoryModuleStore::InMemoryModuleStore() : impl_(std::make_unique<Impl>()) {}
InMemoryModuleStore::InMemoryModuleStore(InMemoryModuleStore&&) noexcept = default;
InMemoryModuleStore& InMemoryModuleStore::operator=(InMemoryModuleStore&&) noexcept = default;
InMemoryModuleStore::~InMemoryModuleStore() = default;

void InMemoryModuleStore::publish(const ProgramModule& module) {
    std::lock_guard lock(impl_->mutex);
    const auto [it, inserted] = impl_->modules.emplace(
        module.id(), Impl::Entry{module, module.lifecycle()});
    if (!inserted && it->second.module.serialize_canonical() != module.serialize_canonical())
        throw std::invalid_argument("Module content identity collision");
}

std::optional<ProgramModule> InMemoryModuleStore::get(std::string_view content_identity) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->modules.find(content_identity);
    if (found == impl_->modules.end()) return std::nullopt;
    return ProgramModule::with_lifecycle(found->second.module, found->second.lifecycle);
}

std::optional<ProgramModule> InMemoryModuleStore::get(std::string_view owner_scope,
                                                       std::string_view content_identity) const {
    if (owner_scope.empty()) return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->modules.find(content_identity);
    if (found == impl_->modules.end() || found->second.module.owner_scope() != owner_scope)
        return std::nullopt;
    return ProgramModule::with_lifecycle(found->second.module, found->second.lifecycle);
}

void InMemoryModuleStore::set_lifecycle(std::string_view content_identity, ModuleLifecycle lifecycle) {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->modules.find(content_identity);
    if (found == impl_->modules.end()) throw std::invalid_argument("Unknown module identity");
    if (found->second.lifecycle == ModuleLifecycle::Revoked && lifecycle != ModuleLifecycle::Revoked)
        throw std::invalid_argument("Revoked module lifecycle cannot be reopened");
    found->second.lifecycle = lifecycle;
}

ModuleResolver::ModuleResolver(std::shared_ptr<const ModuleStore> store) : store_(std::move(store)) {
    if (!store_) throw std::invalid_argument("ModuleResolver requires a ModuleStore");
}

ModuleResolution ModuleResolver::resolve(const ModuleCoordinate& root,
                                          const PolicySnapshot& policy) const {
    require_digest(root.content_identity, "Module root content_identity");
    if (policy.owner_scope().empty()) throw std::invalid_argument("Module resolution requires an owner");
    std::map<std::string, ProgramModule, std::less<>> resolved;
    std::set<std::string, std::less<>> visiting;
    const auto allowed_digest = [&](std::string_view digest) {
        const auto allowed = policy.allowed_module_digests();
        return std::find(allowed.begin(), allowed.end(), digest) != allowed.end();
    };
    std::function<void(const ModuleCoordinate&)> visit = [&](const ModuleCoordinate& coordinate) {
        if (!visiting.insert(coordinate.content_identity).second)
            throw std::invalid_argument("Module dependency cycle detected at " + coordinate.qualified_name());
        const auto module = store_->get(policy.owner_scope(), coordinate.content_identity);
        if (!module) throw std::invalid_argument("Pinned module dependency is unavailable");
        if (module->coordinate() != coordinate)
            throw std::invalid_argument("Pinned module coordinate does not match its content identity");
        if (module->owner_scope() != policy.owner_scope())
            throw std::invalid_argument("Module owner scope differs from the admitting policy");
        if (!allowed_digest(module->id()))
            throw std::invalid_argument("Module digest is not allowed by the policy snapshot");
        if (module->lifecycle() == ModuleLifecycle::Revoked ||
            module->lifecycle() == ModuleLifecycle::Quarantined ||
            module->lifecycle() == ModuleLifecycle::Deprecated)
            throw std::invalid_argument("Module lifecycle is not admissible");
        for (const auto& capability : module->allowed_capabilities())
            if (!contains(policy.allowed_capabilities(), capability))
                throw std::invalid_argument("Module capability exceeds the policy authority");
        for (const auto& effect : module->declared_effects())
            if (!contains(policy.allowed_effects(), effect))
                throw std::invalid_argument("Module effect exceeds the policy authority");
        for (const auto& dependency : module->dependencies()) visit(dependency);
        visiting.erase(coordinate.content_identity);
        resolved.emplace(module->id(), *module);
    };
    visit(root);
    ModuleResolution result;
    result.root = root;
    for (const auto& [id, module] : resolved) {
        (void)id;
        result.modules.push_back(module);
        result.receipts.push_back({module.coordinate().qualified_name(), module.id()});
    }
    validate_module_resolution(result);
    return result;
}

}  // namespace neograph::program
