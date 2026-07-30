#include <neograph/program/bundle.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program bundle field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program bundle field '" + owned_key + "' must be unsigned");
    }
    const auto number = value[owned_key].get<unsigned long long>();
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

void validate_data(const ProgramBundleData& data) {
    if (!detail::is_sha256_identity(data.source_hash)) {
        throw std::invalid_argument("Program bundle source_hash must be a sha256 identity");
    }
    if (!detail::is_sha256_identity(data.canonical_program_hash)) {
        throw std::invalid_argument(
            "Program bundle canonical_program_hash must be a sha256 identity");
    }
    if (data.compiler_build_id.empty() || data.program_schema_version == 0 ||
        data.registry_snapshot_fingerprint.empty() || data.module_dependency_merkle_root.empty()) {
        throw std::invalid_argument("Program bundle requires all durable identity fields");
    }
    if (!data.input_contract.is_object() || !data.output_contract.is_object() ||
        !data.orchestration_plan.is_object() || !data.core_compiled_plan_identities.is_array() ||
        !data.capability_effect_closure.is_object() ||
        !data.executable_registry_identities.is_array() ||
        !data.declared_budget_requirements.is_object()) {
        throw std::invalid_argument("Program bundle stored value fields have invalid JSON types");
    }
}

json bundle_body(const ProgramBundleData& data) {
    json value                              = json::object();
    value["source_hash"]                    = data.source_hash;
    value["canonical_program_hash"]         = data.canonical_program_hash;
    value["compiler_build_id"]              = data.compiler_build_id;
    value["program_schema_version"]         = data.program_schema_version;
    value["registry_snapshot_fingerprint"]  = data.registry_snapshot_fingerprint;
    value["module_dependency_merkle_root"]  = data.module_dependency_merkle_root;
    value["input_contract"]                 = data.input_contract;
    value["output_contract"]                = data.output_contract;
    value["orchestration_plan"]             = data.orchestration_plan;
    value["core_compiled_plan_identities"]  = data.core_compiled_plan_identities;
    value["capability_effect_closure"]      = data.capability_effect_closure;
    value["executable_registry_identities"] = data.executable_registry_identities;
    value["declared_budget_requirements"]   = data.declared_budget_requirements;

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

ProgramBundleData parse_body(const json& value) {
    ProgramBundleData data;
    data.source_hash                    = require_string(value, "source_hash");
    data.canonical_program_hash         = require_string(value, "canonical_program_hash");
    data.compiler_build_id              = require_string(value, "compiler_build_id");
    data.program_schema_version         = require_uint32(value, "program_schema_version");
    data.registry_snapshot_fingerprint  = require_string(value, "registry_snapshot_fingerprint");
    data.module_dependency_merkle_root  = require_string(value, "module_dependency_merkle_root");
    data.input_contract                 = require_value(value, "input_contract");
    data.output_contract                = require_value(value, "output_contract");
    data.orchestration_plan             = require_value(value, "orchestration_plan");
    data.core_compiled_plan_identities  = require_value(value, "core_compiled_plan_identities");
    data.capability_effect_closure      = require_value(value, "capability_effect_closure");
    data.executable_registry_identities = require_value(value, "executable_registry_identities");
    data.declared_budget_requirements   = require_value(value, "declared_budget_requirements");

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
    validate_data(data);
    return data;
}

}  // namespace

struct ProgramBundle::Impl {
    ProgramBundleData data;
    std::string       id;
};

ProgramBundle::ProgramBundle(ProgramBundleData data) {
    validate_data(data);
    auto impl  = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->id   = detail::sha256_identity("program-bundle",
                                         detail::canonical_json_bytes(bundle_body(impl->data)));
    impl_      = std::move(impl);
}

ProgramBundle::ProgramBundle(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramBundle ProgramBundle::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = json::parse(std::string(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramBundle JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != "neograph-program-bundle") {
        throw std::invalid_argument("Stored ProgramBundle has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored ProgramBundle",
        {"format", "storage_schema_version", "id", "source_hash", "canonical_program_hash",
         "compiler_build_id", "program_schema_version", "registry_snapshot_fingerprint",
         "module_dependency_merkle_root", "input_contract", "output_contract", "orchestration_plan",
         "core_compiled_plan_identities", "capability_effect_closure",
         "executable_registry_identities", "declared_budget_requirements", "source_map",
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
const std::string& ProgramBundle::source_hash() const noexcept {
    return impl_->data.source_hash;
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
json ProgramBundle::input_contract() const {
    return impl_->data.input_contract;
}
json ProgramBundle::output_contract() const {
    return impl_->data.output_contract;
}
json ProgramBundle::orchestration_plan() const {
    return impl_->data.orchestration_plan;
}
json ProgramBundle::core_compiled_plan_identities() const {
    return impl_->data.core_compiled_plan_identities;
}
json ProgramBundle::capability_effect_closure() const {
    return impl_->data.capability_effect_closure;
}
json ProgramBundle::executable_registry_identities() const {
    return impl_->data.executable_registry_identities;
}
json ProgramBundle::declared_budget_requirements() const {
    return impl_->data.declared_budget_requirements;
}
const std::vector<SourceMapEntry>& ProgramBundle::source_map() const noexcept {
    return impl_->data.source_map;
}
const std::vector<Diagnostic>& ProgramBundle::diagnostics() const noexcept {
    return impl_->data.diagnostics;
}

std::string ProgramBundle::serialize_canonical() const {
    auto value                      = bundle_body(impl_->data);
    value["format"]                 = "neograph-program-bundle";
    value["storage_schema_version"] = STORAGE_SCHEMA_VERSION;
    value["id"]                     = impl_->id;
    return detail::canonical_json_bytes(value);
}

}  // namespace neograph::program
