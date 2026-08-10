#include <neograph/program/admission.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::uint32_t kStorageSchemaVersion = 1;

void require_nonempty(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void require_digest(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256: identity");
    }
}

void require_semver(std::string_view value, std::string_view field) {
    if (!detail::is_semantic_version(value)) {
        throw std::invalid_argument(std::string(field) + " must be valid SemVer");
    }
}

template <typename T>
void sort_unique(std::vector<T>& values, std::string_view field) {
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    }
}

void normalize_strings(std::vector<std::string>& values,
                       std::string_view          field,
                       bool                      digest = false) {
    for (const auto& value : values) {
        if (digest)
            require_digest(value, field);
        else
            require_nonempty(value, field);
    }
    sort_unique(values, field);
}

void validate_identity(const ExecutableIdentity& identity) {
    require_nonempty(identity.name, "Executable name");
    require_semver(identity.semantic_version, "Executable semantic_version");
    require_digest(identity.implementation_digest, "Executable implementation_digest");
}

json encode_identity(const ExecutableIdentity& identity) {
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument(owned_key + " must be a string");
    }
    auto result = value[owned_key].get<std::string>();
    detail::validate_utf8(result);
    return result;
}

std::uint64_t require_u64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument(owned_key + " must be an unsigned integer");
    }
    return value[owned_key].get<unsigned long long>();
}

std::uint32_t require_u32(const json& value, std::string_view key) {
    const auto number = require_u64(value, key);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(key) + " is outside the uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

void require_object(const json& value, std::string_view name) {
    if (!value.is_object()) throw std::invalid_argument(std::string(name) + " must be an object");
}

void require_array(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_array()) {
        throw std::invalid_argument(owned_key + " must be an array");
    }
}

std::vector<std::string> parse_strings(const json&      value,
                                       std::string_view key,
                                       bool             digest = false) {
    require_array(value, key);
    const std::string        owned_key(key);
    std::vector<std::string> result;
    for (const auto& item : value[owned_key]) {
        if (!item.is_string()) {
            throw std::invalid_argument(owned_key + " entries must be strings");
        }
        result.push_back(item.get<std::string>());
    }
    normalize_strings(result, key, digest);
    return result;
}

ExecutableIdentity parse_identity(const json& value) {
    require_object(value, "Executable identity");
    detail::reject_unknown_fields(value, "Executable identity",
                                  {"kind", "name", "semantic_version", "implementation_digest"});
    ExecutableIdentity result;
    result.kind                  = executable_kind_from_string(require_string(value, "kind"));
    result.name                  = require_string(value, "name");
    result.semantic_version      = require_string(value, "semantic_version");
    result.implementation_digest = require_string(value, "implementation_digest");
    validate_identity(result);
    return result;
}

void validate_budget(const BudgetLimits& value) {
    if (value.wall_time_ms == 0 || value.model_tokens == 0 || value.monetary_microunits == 0 ||
        value.max_concurrency == 0 || value.max_program_operations == 0 ||
        value.max_core_steps == 0 || value.max_dynamic_compiles == 0) {
        throw std::invalid_argument("Every policy budget ceiling except child budgets must be positive");
    }
    if ((value.max_child_depth == 0) != (value.max_total_children == 0)) {
        throw std::invalid_argument(
            "Policy child depth and total-child ceilings must be enabled together");
    }
    if (value.max_child_depth > MAX_SUPPORTED_CHILD_DEPTH) {
        throw std::invalid_argument("Policy child depth exceeds the supported hard ceiling");
    }
}

json encode_budget(const BudgetLimits& value) {
    return json{{"wall_time_ms", value.wall_time_ms},
                {"model_tokens", value.model_tokens},
                {"monetary_microunits", value.monetary_microunits},
                {"max_concurrency", value.max_concurrency},
                {"max_program_operations", value.max_program_operations},
                {"max_core_steps", value.max_core_steps},
                {"max_dynamic_compiles", value.max_dynamic_compiles},
                {"max_child_depth", value.max_child_depth},
                {"max_total_children", value.max_total_children}};
}

BudgetLimits parse_budget(const json& value) {
    require_object(value, "Policy budget_ceiling");
    detail::reject_unknown_fields(
        value, "Policy budget_ceiling",
        {"wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
         "max_program_operations", "max_core_steps", "max_dynamic_compiles", "max_child_depth",
         "max_total_children"});
    BudgetLimits result;
    result.wall_time_ms           = require_u64(value, "wall_time_ms");
    result.model_tokens           = require_u64(value, "model_tokens");
    result.monetary_microunits    = require_u64(value, "monetary_microunits");
    result.max_concurrency        = require_u32(value, "max_concurrency");
    result.max_program_operations = require_u64(value, "max_program_operations");
    result.max_core_steps         = require_u64(value, "max_core_steps");
    result.max_dynamic_compiles   = require_u32(value, "max_dynamic_compiles");
    result.max_child_depth        = require_u32(value, "max_child_depth");
    result.max_total_children     = require_u64(value, "max_total_children");
    validate_budget(result);
    return result;
}

}  // namespace

struct AdmissionProfile::Impl {
    std::string                     id;
    std::string                     semantic_version;
    std::string                     fingerprint;
    std::string                     registry_fingerprint;
    AdmissionMode                   mode                       = AdmissionMode::MultiTenant;
    std::uint32_t                   max_program_schema_version = 1;
    ExecutionGuarantee               minimum_execution_guarantee = ExecutionGuarantee::Strict;
    std::vector<SourceKind>         allowed_source_kinds;
    std::vector<ExecutableIdentity> allowed_executables;
    std::vector<EffectMode>         allowed_effect_modes;
    json                            manifest;
};

struct AdmissionProfileBuilder::Impl {
    std::string                     id;
    std::string                     semantic_version;
    std::string                     registry_fingerprint;
    std::optional<RegistrySnapshot> registry;
    AdmissionMode                   mode                       = AdmissionMode::MultiTenant;
    std::uint32_t                   max_program_schema_version = 1;
    ExecutionGuarantee               minimum_execution_guarantee = ExecutionGuarantee::Strict;
    std::vector<SourceKind>         allowed_source_kinds;
    std::vector<ExecutableIdentity> allowed_executables;
    std::vector<EffectMode>         allowed_effect_modes;
};

struct PolicySnapshot::Impl {
    std::string              id;
    std::string              semantic_version;
    std::string              owner_scope;
    std::string              fingerprint;
    std::string              admission_profile_fingerprint;
    std::string              registry_fingerprint;
    std::vector<std::string> allowed_capabilities;
    std::vector<std::string> allowed_effects;
    std::vector<std::string> allowed_module_digests;
    BudgetLimits             budget_ceiling;
    ExecutionGuarantee       minimum_execution_guarantee = ExecutionGuarantee::Strict;
    json                     manifest;
};

struct PolicySnapshotBuilder::Impl {
    std::string                                   id;
    std::string                                   semantic_version;
    std::string                                   owner_scope;
    std::shared_ptr<const AdmissionProfile::Impl> admission_profile;
    std::vector<std::string>                      allowed_capabilities;
    std::vector<std::string>                      allowed_effects;
    std::vector<std::string>                      allowed_module_digests;
    BudgetLimits                                  budget_ceiling;
    ExecutionGuarantee                            minimum_execution_guarantee =
        ExecutionGuarantee::Strict;
};

AdmissionProfile::AdmissionProfile(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
AdmissionProfile::~AdmissionProfile() = default;
const std::string& AdmissionProfile::id() const noexcept {
    return impl_->id;
}
const std::string& AdmissionProfile::semantic_version() const noexcept {
    return impl_->semantic_version;
}
const std::string& AdmissionProfile::fingerprint() const noexcept {
    return impl_->fingerprint;
}
const std::string& AdmissionProfile::registry_fingerprint() const noexcept {
    return impl_->registry_fingerprint;
}
AdmissionMode AdmissionProfile::mode() const noexcept {
    return impl_->mode;
}
std::uint32_t AdmissionProfile::max_program_schema_version() const noexcept {
    return impl_->max_program_schema_version;
}
ExecutionGuarantee AdmissionProfile::minimum_execution_guarantee() const noexcept {
    return impl_->minimum_execution_guarantee;
}
std::vector<SourceKind> AdmissionProfile::allowed_source_kinds() const {
    return impl_->allowed_source_kinds;
}
std::vector<ExecutableIdentity> AdmissionProfile::allowed_executables() const {
    return impl_->allowed_executables;
}
std::vector<EffectMode> AdmissionProfile::allowed_effect_modes() const {
    return impl_->allowed_effect_modes;
}
json AdmissionProfile::manifest() const {
    return detail::owned_json_copy(impl_->manifest);
}
std::string AdmissionProfile::serialize_canonical() const {
    return detail::canonical_json_bytes(impl_->manifest);
}

AdmissionProfileBuilder::AdmissionProfileBuilder() : impl_(std::make_unique<Impl>()) {}
AdmissionProfileBuilder::AdmissionProfileBuilder(AdmissionProfileBuilder&&) noexcept = default;
AdmissionProfileBuilder& AdmissionProfileBuilder::operator=(AdmissionProfileBuilder&&) noexcept =
    default;
AdmissionProfileBuilder::~AdmissionProfileBuilder() = default;
AdmissionProfileBuilder& AdmissionProfileBuilder::id(std::string value) {
    impl_->id = std::move(value);
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::semantic_version(std::string value) {
    impl_->semantic_version = std::move(value);
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::registry(RegistrySnapshot value) {
    impl_->registry_fingerprint = value.fingerprint();
    impl_->registry             = std::move(value);
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::mode(AdmissionMode value) {
    impl_->mode = value;
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::max_program_schema_version(std::uint32_t value) {
    impl_->max_program_schema_version = value;
    return *this;
}
AdmissionProfileBuilder&
AdmissionProfileBuilder::minimum_execution_guarantee(ExecutionGuarantee value) {
    impl_->minimum_execution_guarantee = value;
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::allow_source_kind(SourceKind value) {
    impl_->allowed_source_kinds.push_back(value);
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::allow_executable(ExecutableIdentity value) {
    impl_->allowed_executables.push_back(std::move(value));
    return *this;
}
AdmissionProfileBuilder& AdmissionProfileBuilder::allow_effect_mode(EffectMode value) {
    impl_->allowed_effect_modes.push_back(value);
    return *this;
}

AdmissionProfile AdmissionProfileBuilder::build() && {
    if (!impl_) throw std::logic_error("AdmissionProfileBuilder was already consumed");
    require_nonempty(impl_->id, "Admission profile id");
    require_semver(impl_->semantic_version, "Admission profile semantic_version");
    require_digest(impl_->registry_fingerprint, "Admission profile registry_fingerprint");
    if (impl_->max_program_schema_version == 0) {
        throw std::invalid_argument("Admission max_program_schema_version must be positive");
    }
    if (to_string(impl_->mode) == "unknown") {
        throw std::invalid_argument("Admission mode is unsupported");
    }
    if (execution_guarantee_rank(impl_->minimum_execution_guarantee) == 0) {
        throw std::invalid_argument("Admission minimum_execution_guarantee is unsupported");
    }
    for (const auto value : impl_->allowed_source_kinds) {
        if (to_string(value) == "unknown") {
            throw std::invalid_argument("Admission source kind is unsupported");
        }
    }
    for (const auto value : impl_->allowed_effect_modes) {
        if (to_string(value) == "unknown") {
            throw std::invalid_argument("Admission effect mode is unsupported");
        }
    }
    sort_unique(impl_->allowed_source_kinds, "allowed_source_kinds");
    sort_unique(impl_->allowed_effect_modes, "allowed_effect_modes");
    for (const auto& identity : impl_->allowed_executables)
        validate_identity(identity);
    if (impl_->registry) {
        for (const auto& identity : impl_->allowed_executables) {
            const auto manifest = impl_->registry->find(identity.kind, identity.name);
            if (!manifest || manifest->identity != identity) {
                throw std::invalid_argument(
                    "Admission profile allows an executable absent from its RegistrySnapshot");
            }
            if (std::find(impl_->allowed_effect_modes.begin(), impl_->allowed_effect_modes.end(),
                          manifest->effect_mode) == impl_->allowed_effect_modes.end()) {
                throw std::invalid_argument(
                    "Admission profile omits an effect mode required by an allowed executable");
            }
        }
    }
    std::sort(impl_->allowed_executables.begin(), impl_->allowed_executables.end(),
              [](const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
                  return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                                    lhs.implementation_digest} <
                         std::tuple{to_string(rhs.kind), rhs.name, rhs.semantic_version,
                                    rhs.implementation_digest};
              });
    if (std::adjacent_find(impl_->allowed_executables.begin(), impl_->allowed_executables.end()) !=
        impl_->allowed_executables.end()) {
        throw std::invalid_argument("allowed_executables contains a duplicate identity");
    }
    if (impl_->mode == AdmissionMode::MultiTenant &&
        std::find(impl_->allowed_effect_modes.begin(), impl_->allowed_effect_modes.end(),
                  EffectMode::TrustedNative) != impl_->allowed_effect_modes.end()) {
        throw std::invalid_argument("MultiTenant admission cannot allow TrustedNative effects");
    }

    json source_kinds = json::array();
    for (const auto value : impl_->allowed_source_kinds) {
        source_kinds.push_back(std::string(to_string(value)));
    }
    json executables = json::array();
    for (const auto& value : impl_->allowed_executables)
        executables.push_back(encode_identity(value));
    json effect_modes = json::array();
    for (const auto value : impl_->allowed_effect_modes) {
        effect_modes.push_back(std::string(to_string(value)));
    }
    json       body{{"format", "neograph-admission-profile"},
                    {"storage_schema_version", kStorageSchemaVersion},
                    {"id", impl_->id},
                    {"semantic_version", impl_->semantic_version},
                    {"registry_fingerprint", impl_->registry_fingerprint},
                    {"mode", std::string(to_string(impl_->mode))},
                    {"max_program_schema_version", impl_->max_program_schema_version},
                    {"minimum_execution_guarantee",
                     std::string(to_string(impl_->minimum_execution_guarantee))},
                    {"allowed_source_kinds", std::move(source_kinds)},
                    {"allowed_executables", std::move(executables)},
                    {"allowed_effect_modes", std::move(effect_modes)}};
    const auto fingerprint =
        detail::sha256_identity("admission-profile-v1", detail::canonical_json_bytes(body));
    body["fingerprint"] = fingerprint;

    auto result                        = std::make_shared<AdmissionProfile::Impl>();
    result->id                         = std::move(impl_->id);
    result->semantic_version           = std::move(impl_->semantic_version);
    result->fingerprint                = fingerprint;
    result->registry_fingerprint       = std::move(impl_->registry_fingerprint);
    result->mode                       = impl_->mode;
    result->max_program_schema_version = impl_->max_program_schema_version;
    result->minimum_execution_guarantee = impl_->minimum_execution_guarantee;
    result->allowed_source_kinds       = std::move(impl_->allowed_source_kinds);
    result->allowed_executables        = std::move(impl_->allowed_executables);
    result->allowed_effect_modes       = std::move(impl_->allowed_effect_modes);
    result->manifest                   = detail::owned_json_copy(body);
    impl_.reset();
    return AdmissionProfile(std::move(result));
}

AdmissionProfile AdmissionProfile::parse(std::string_view bytes) {
    auto value = detail::parse_json_strict(bytes);
    require_object(value, "Stored AdmissionProfile");
    detail::reject_unknown_fields(
        value, "Stored AdmissionProfile",
        {"format", "storage_schema_version", "fingerprint", "id", "semantic_version",
         "registry_fingerprint", "mode", "max_program_schema_version",
         "minimum_execution_guarantee", "allowed_source_kinds", "allowed_executables",
         "allowed_effect_modes"});
    if (require_string(value, "format") != "neograph-admission-profile") {
        throw std::invalid_argument("Stored AdmissionProfile has unknown format");
    }
    if (require_u32(value, "storage_schema_version") != kStorageSchemaVersion) {
        throw std::invalid_argument("Stored AdmissionProfile schema version is unsupported");
    }
    const auto supplied_fingerprint = require_string(value, "fingerprint");
    require_digest(supplied_fingerprint, "Admission profile fingerprint");

    auto builder = AdmissionProfileBuilder();
    builder.id(require_string(value, "id"))
        .semantic_version(require_string(value, "semantic_version"))
        .mode(admission_mode_from_string(require_string(value, "mode")))
        .max_program_schema_version(require_u32(value, "max_program_schema_version"))
        .minimum_execution_guarantee(
            execution_guarantee_from_string(require_string(value, "minimum_execution_guarantee")));
    builder.impl_->registry_fingerprint = require_string(value, "registry_fingerprint");
    require_array(value, "allowed_source_kinds");
    for (const auto& item : value["allowed_source_kinds"]) {
        if (!item.is_string())
            throw std::invalid_argument("allowed_source_kinds entries must be strings");
        builder.allow_source_kind(source_kind_from_string(item.get<std::string>()));
    }
    require_array(value, "allowed_executables");
    for (const auto& item : value["allowed_executables"]) {
        builder.allow_executable(parse_identity(item));
    }
    require_array(value, "allowed_effect_modes");
    for (const auto& item : value["allowed_effect_modes"]) {
        if (!item.is_string())
            throw std::invalid_argument("allowed_effect_modes entries must be strings");
        builder.allow_effect_mode(effect_mode_from_string(item.get<std::string>()));
    }
    auto result = std::move(builder).build();
    if (result.fingerprint() != supplied_fingerprint) {
        throw std::invalid_argument("Stored AdmissionProfile fingerprint does not match its body");
    }
    return result;
}

PolicySnapshot::PolicySnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
PolicySnapshot::~PolicySnapshot() = default;
const std::string& PolicySnapshot::id() const noexcept {
    return impl_->id;
}
const std::string& PolicySnapshot::semantic_version() const noexcept {
    return impl_->semantic_version;
}
const std::string& PolicySnapshot::owner_scope() const noexcept {
    return impl_->owner_scope;
}
const std::string& PolicySnapshot::fingerprint() const noexcept {
    return impl_->fingerprint;
}
const std::string& PolicySnapshot::admission_profile_fingerprint() const noexcept {
    return impl_->admission_profile_fingerprint;
}
const std::string& PolicySnapshot::registry_fingerprint() const noexcept {
    return impl_->registry_fingerprint;
}
std::vector<std::string> PolicySnapshot::allowed_capabilities() const {
    return impl_->allowed_capabilities;
}
std::vector<std::string> PolicySnapshot::allowed_effects() const {
    return impl_->allowed_effects;
}
std::vector<std::string> PolicySnapshot::allowed_module_digests() const {
    return impl_->allowed_module_digests;
}
const BudgetLimits& PolicySnapshot::budget_ceiling() const noexcept {
    return impl_->budget_ceiling;
}
ExecutionGuarantee PolicySnapshot::minimum_execution_guarantee() const noexcept {
    return impl_->minimum_execution_guarantee;
}
json PolicySnapshot::manifest() const {
    return detail::owned_json_copy(impl_->manifest);
}
std::string PolicySnapshot::serialize_canonical() const {
    return detail::canonical_json_bytes(impl_->manifest);
}

PolicySnapshotBuilder::PolicySnapshotBuilder() : impl_(std::make_unique<Impl>()) {}
PolicySnapshotBuilder::PolicySnapshotBuilder(PolicySnapshotBuilder&&) noexcept            = default;
PolicySnapshotBuilder& PolicySnapshotBuilder::operator=(PolicySnapshotBuilder&&) noexcept = default;
PolicySnapshotBuilder::~PolicySnapshotBuilder()                                           = default;
PolicySnapshotBuilder& PolicySnapshotBuilder::id(std::string value) {
    impl_->id = std::move(value);
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::semantic_version(std::string value) {
    impl_->semantic_version = std::move(value);
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::owner_scope(std::string value) {
    impl_->owner_scope = std::move(value);
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::admission_profile(AdmissionProfile value) {
    impl_->admission_profile = value.impl_;
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::allow_capability(std::string value) {
    impl_->allowed_capabilities.push_back(std::move(value));
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::allow_effect(std::string value) {
    impl_->allowed_effects.push_back(std::move(value));
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::allow_module_digest(std::string value) {
    impl_->allowed_module_digests.push_back(std::move(value));
    return *this;
}
PolicySnapshotBuilder& PolicySnapshotBuilder::budget_ceiling(BudgetLimits value) {
    impl_->budget_ceiling = value;
    return *this;
}
PolicySnapshotBuilder&
PolicySnapshotBuilder::minimum_execution_guarantee(ExecutionGuarantee value) {
    impl_->minimum_execution_guarantee = value;
    return *this;
}

PolicySnapshot PolicySnapshotBuilder::build() && {
    if (!impl_) throw std::logic_error("PolicySnapshotBuilder was already consumed");
    require_nonempty(impl_->id, "Policy id");
    require_semver(impl_->semantic_version, "Policy semantic_version");
    require_nonempty(impl_->owner_scope, "Policy owner_scope");
    if (!impl_->admission_profile) {
        throw std::invalid_argument("Policy requires an AdmissionProfile");
    }
    normalize_strings(impl_->allowed_capabilities, "allowed_capabilities");
    normalize_strings(impl_->allowed_effects, "allowed_effects");
    normalize_strings(impl_->allowed_module_digests, "allowed_module_digests", true);
    validate_budget(impl_->budget_ceiling);
    if (execution_guarantee_rank(impl_->minimum_execution_guarantee) == 0) {
        throw std::invalid_argument("Policy minimum_execution_guarantee is unsupported");
    }
    if (execution_guarantee_rank(impl_->minimum_execution_guarantee) <
        execution_guarantee_rank(impl_->admission_profile->minimum_execution_guarantee)) {
        throw std::invalid_argument(
            "Policy minimum_execution_guarantee weakens its AdmissionProfile floor");
    }
    const bool profile_allows_trusted =
        std::find(impl_->admission_profile->allowed_effect_modes.begin(),
                  impl_->admission_profile->allowed_effect_modes.end(),
                  EffectMode::TrustedNative) !=
        impl_->admission_profile->allowed_effect_modes.end();
    const bool policy_grants_trusted =
        std::find(impl_->allowed_capabilities.begin(), impl_->allowed_capabilities.end(),
                  TRUSTED_NATIVE_CAPABILITY) != impl_->allowed_capabilities.end();
    if (impl_->admission_profile->mode == AdmissionMode::MultiTenant && policy_grants_trusted) {
        throw std::invalid_argument(
            "MultiTenant policy cannot grant neograph.native.trusted capability");
    }
    if (!profile_allows_trusted && policy_grants_trusted) {
        throw std::invalid_argument(
            "Policy grants neograph.native.trusted capability outside its AdmissionProfile");
    }
    if (profile_allows_trusted && !policy_grants_trusted) {
        throw std::invalid_argument(
            "TrustedNative policy requires neograph.native.trusted capability");
    }

    json       body{{"format", "neograph-policy-snapshot"},
                    {"storage_schema_version", kStorageSchemaVersion},
                    {"id", impl_->id},
                    {"semantic_version", impl_->semantic_version},
                    {"owner_scope", impl_->owner_scope},
                    {"admission_profile_fingerprint", impl_->admission_profile->fingerprint},
                    {"registry_fingerprint", impl_->admission_profile->registry_fingerprint},
                    {"allowed_capabilities", impl_->allowed_capabilities},
                    {"allowed_effects", impl_->allowed_effects},
                    {"allowed_module_digests", impl_->allowed_module_digests},
                    {"budget_ceiling", encode_budget(impl_->budget_ceiling)},
                    {"minimum_execution_guarantee",
                     std::string(to_string(impl_->minimum_execution_guarantee))}};
    const auto fingerprint =
        detail::sha256_identity("policy-snapshot-v1", detail::canonical_json_bytes(body));
    body["fingerprint"] = fingerprint;

    auto result                           = std::make_shared<PolicySnapshot::Impl>();
    result->id                            = std::move(impl_->id);
    result->semantic_version              = std::move(impl_->semantic_version);
    result->owner_scope                   = std::move(impl_->owner_scope);
    result->fingerprint                   = fingerprint;
    result->admission_profile_fingerprint = impl_->admission_profile->fingerprint;
    result->registry_fingerprint          = impl_->admission_profile->registry_fingerprint;
    result->allowed_capabilities          = std::move(impl_->allowed_capabilities);
    result->allowed_effects               = std::move(impl_->allowed_effects);
    result->allowed_module_digests        = std::move(impl_->allowed_module_digests);
    result->budget_ceiling                = impl_->budget_ceiling;
    result->minimum_execution_guarantee     = impl_->minimum_execution_guarantee;
    result->manifest                      = detail::owned_json_copy(body);
    impl_.reset();
    return PolicySnapshot(std::move(result));
}

PolicySnapshot PolicySnapshot::parse(std::string_view bytes) {
    auto value = detail::parse_json_strict(bytes);
    require_object(value, "Stored PolicySnapshot");
    detail::reject_unknown_fields(
        value, "Stored PolicySnapshot",
        {"format", "storage_schema_version", "fingerprint", "id", "semantic_version", "owner_scope",
         "admission_profile_fingerprint", "registry_fingerprint", "allowed_capabilities",
         "allowed_effects", "allowed_module_digests", "budget_ceiling",
         "minimum_execution_guarantee"});
    if (require_string(value, "format") != "neograph-policy-snapshot") {
        throw std::invalid_argument("Stored PolicySnapshot has unknown format");
    }
    if (require_u32(value, "storage_schema_version") != kStorageSchemaVersion) {
        throw std::invalid_argument("Stored PolicySnapshot schema version is unsupported");
    }
    auto result         = std::make_shared<Impl>();
    result->fingerprint = require_string(value, "fingerprint");
    require_digest(result->fingerprint, "Policy fingerprint");
    result->id = require_string(value, "id");
    require_nonempty(result->id, "Policy id");
    result->semantic_version = require_string(value, "semantic_version");
    require_semver(result->semantic_version, "Policy semantic_version");
    result->owner_scope = require_string(value, "owner_scope");
    require_nonempty(result->owner_scope, "Policy owner_scope");
    result->admission_profile_fingerprint = require_string(value, "admission_profile_fingerprint");
    require_digest(result->admission_profile_fingerprint, "Policy admission_profile_fingerprint");
    result->registry_fingerprint = require_string(value, "registry_fingerprint");
    require_digest(result->registry_fingerprint, "Policy registry_fingerprint");
    result->allowed_capabilities   = parse_strings(value, "allowed_capabilities");
    result->allowed_effects        = parse_strings(value, "allowed_effects");
    result->allowed_module_digests = parse_strings(value, "allowed_module_digests", true);
    if (!value.contains("budget_ceiling")) {
        throw std::invalid_argument("Policy budget_ceiling is required");
    }
    result->budget_ceiling = parse_budget(value["budget_ceiling"]);
    result->minimum_execution_guarantee =
        execution_guarantee_from_string(require_string(value, "minimum_execution_guarantee"));

    json       body{{"format", "neograph-policy-snapshot"},
                    {"storage_schema_version", kStorageSchemaVersion},
                    {"id", result->id},
                    {"semantic_version", result->semantic_version},
                    {"owner_scope", result->owner_scope},
                    {"admission_profile_fingerprint", result->admission_profile_fingerprint},
                    {"registry_fingerprint", result->registry_fingerprint},
                    {"allowed_capabilities", result->allowed_capabilities},
                    {"allowed_effects", result->allowed_effects},
                    {"allowed_module_digests", result->allowed_module_digests},
                    {"budget_ceiling", encode_budget(result->budget_ceiling)},
                    {"minimum_execution_guarantee",
                     std::string(to_string(result->minimum_execution_guarantee))}};
    const auto computed =
        detail::sha256_identity("policy-snapshot-v1", detail::canonical_json_bytes(body));
    if (computed != result->fingerprint) {
        throw std::invalid_argument("Stored PolicySnapshot fingerprint does not match its body");
    }
    body["fingerprint"] = result->fingerprint;
    result->manifest    = std::move(body);
    return PolicySnapshot(std::move(result));
}

std::string_view to_string(AdmissionMode mode) noexcept {
    switch (mode) {
        case AdmissionMode::MultiTenant:
            return "multi_tenant";
        case AdmissionMode::TrustedEmbedding:
            return "trusted_embedding";
    }
    return "unknown";
}

AdmissionMode admission_mode_from_string(std::string_view value) {
    if (value == "multi_tenant") return AdmissionMode::MultiTenant;
    if (value == "trusted_embedding") return AdmissionMode::TrustedEmbedding;
    throw std::invalid_argument("Unknown Program admission mode: " + std::string(value));
}

}  // namespace neograph::program
