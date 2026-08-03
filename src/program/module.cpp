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
    for (const auto& item : value) result.push_back(required_string(item, "array entry"));
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
                {"budget", encode_budget(child.budget)}};
}

ChildProgramDescriptor parse_child(const json& value) {
    require_object(value, "child program");
    detail::reject_unknown_fields(value, "child program",
                                  {"name", "program_version_id", "inputs", "outputs",
                                   "required_capabilities", "required_effects", "budget"});
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
    for (const auto& child : data.children) {
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
    }
    if (std::adjacent_find(data.children.begin(), data.children.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.name == rhs.name; }) !=
        data.children.end())
        throw std::invalid_argument("Module children contain a duplicate name");
    std::sort(data.allowed_capabilities.begin(), data.allowed_capabilities.end());
    std::sort(data.declared_effects.begin(), data.declared_effects.end());
    if (std::adjacent_find(data.allowed_capabilities.begin(), data.allowed_capabilities.end()) !=
        data.allowed_capabilities.end())
        throw std::invalid_argument("Module allowed_capabilities contains a duplicate");
    if (std::adjacent_find(data.declared_effects.begin(), data.declared_effects.end()) !=
        data.declared_effects.end())
        throw std::invalid_argument("Module declared_effects contains a duplicate");
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
        const auto module = store_->get(coordinate.content_identity);
        if (!module) throw std::invalid_argument("Pinned module dependency is unavailable");
        if (module->coordinate() != coordinate)
            throw std::invalid_argument("Pinned module coordinate does not match its content identity");
        if (module->owner_scope() != policy.owner_scope())
            throw std::invalid_argument("Module owner scope differs from the admitting policy");
        if (!allowed_digest(module->id()))
            throw std::invalid_argument("Module digest is not allowed by the policy snapshot");
        if (module->lifecycle() == ModuleLifecycle::Revoked ||
            module->lifecycle() == ModuleLifecycle::Quarantined)
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
    return result;
}

}  // namespace neograph::program
