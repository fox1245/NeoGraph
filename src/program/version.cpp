#include <neograph/program/version.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view VERSION_FORMAT = "neograph-program-version";

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program version field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program version field '" + owned_key + "' must be unsigned");
    }
    const auto number = value[owned_key].get<unsigned long long>();
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program version integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Program version requires field '" + owned_key + "'");
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

void validate_core_plan(const CorePlanIdentity& plan) {
    require_nonempty_utf8(plan.name, "Core materialization plan name");
    require_sha256(plan.compiled_plan_identity, "Core materialization plan identity");
}

void normalize_data(ProgramVersionData& data) {
    require_sha256(data.bundle_id, "Program version bundle_id");
    require_nonempty_utf8(data.admission_profile.profile_id, "Admission profile id");
    require_sha256(data.admission_profile.profile_fingerprint, "Admission profile fingerprint");
    require_nonempty_utf8(data.policy_snapshot.snapshot_id, "Policy snapshot id");
    require_sha256(data.policy_snapshot.snapshot_fingerprint, "Policy snapshot fingerprint");
    require_nonempty_utf8(data.ownership_scope, "Program version ownership_scope");

    for (const auto& receipt : data.dependency_receipts) {
        require_nonempty_utf8(receipt.dependency_id, "Dependency receipt id");
        require_sha256(receipt.content_identity, "Dependency content identity");
    }
    std::sort(
        data.dependency_receipts.begin(), data.dependency_receipts.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.dependency_id < rhs.dependency_id; });
    if (std::adjacent_find(data.dependency_receipts.begin(), data.dependency_receipts.end(),
                           [](const auto& lhs, const auto& rhs) {
                               return lhs.dependency_id == rhs.dependency_id;
                           }) != data.dependency_receipts.end()) {
        throw std::invalid_argument("Program version contains duplicate dependency receipts");
    }

    auto& materialization = data.core_materialization_receipt;
    require_nonempty_utf8(materialization.compiler_build_id,
                          "Core materialization compiler build id");
    require_sha256(materialization.registry_snapshot_fingerprint,
                   "Core materialization registry snapshot fingerprint");
    if (materialization.plans.empty()) {
        throw std::invalid_argument("Core materialization receipt requires at least one plan");
    }
    for (const auto& plan : materialization.plans)
        validate_core_plan(plan);
    std::sort(materialization.plans.begin(), materialization.plans.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.name < rhs.name; });
    if (std::adjacent_find(materialization.plans.begin(), materialization.plans.end(),
                           [](const auto& lhs, const auto& rhs) { return lhs.name == rhs.name; }) !=
        materialization.plans.end()) {
        throw std::invalid_argument("Core materialization receipt contains duplicate plan names");
    }
}

json encode_admission(const AdmissionProfileBinding& binding) {
    return json{{"profile_id", binding.profile_id},
                {"profile_fingerprint", binding.profile_fingerprint}};
}

AdmissionProfileBinding parse_admission(const json& value) {
    require_object(value, "Admission profile binding");
    detail::reject_unknown_fields(value, "Admission profile binding",
                                  {"profile_id", "profile_fingerprint"});
    return AdmissionProfileBinding{require_string(value, "profile_id"),
                                   require_string(value, "profile_fingerprint")};
}

json encode_policy(const PolicySnapshotBinding& binding) {
    return json{{"snapshot_id", binding.snapshot_id},
                {"snapshot_fingerprint", binding.snapshot_fingerprint}};
}

PolicySnapshotBinding parse_policy(const json& value) {
    require_object(value, "Policy snapshot binding");
    detail::reject_unknown_fields(value, "Policy snapshot binding",
                                  {"snapshot_id", "snapshot_fingerprint"});
    return PolicySnapshotBinding{require_string(value, "snapshot_id"),
                                 require_string(value, "snapshot_fingerprint")};
}

json encode_dependency(const DependencyReceipt& receipt) {
    return json{{"dependency_id", receipt.dependency_id},
                {"content_identity", receipt.content_identity}};
}

DependencyReceipt parse_dependency(const json& value) {
    require_object(value, "Dependency receipt");
    detail::reject_unknown_fields(value, "Dependency receipt",
                                  {"dependency_id", "content_identity"});
    return DependencyReceipt{require_string(value, "dependency_id"),
                             require_string(value, "content_identity")};
}

json encode_plan(const CorePlanIdentity& plan) {
    return json{{"name", plan.name}, {"compiled_plan_identity", plan.compiled_plan_identity}};
}

CorePlanIdentity parse_plan(const json& value) {
    require_object(value, "Core materialization plan");
    detail::reject_unknown_fields(value, "Core materialization plan",
                                  {"name", "compiled_plan_identity"});
    return CorePlanIdentity{require_string(value, "name"),
                            require_string(value, "compiled_plan_identity")};
}

json encode_materialization(const CoreMaterializationReceipt& receipt) {
    json plans = json::array();
    for (const auto& plan : receipt.plans)
        plans.push_back(encode_plan(plan));
    return json{{"compiler_build_id", receipt.compiler_build_id},
                {"registry_snapshot_fingerprint", receipt.registry_snapshot_fingerprint},
                {"plans", std::move(plans)}};
}

CoreMaterializationReceipt parse_materialization(const json& value) {
    require_object(value, "Core materialization receipt");
    detail::reject_unknown_fields(value, "Core materialization receipt",
                                  {"compiler_build_id", "registry_snapshot_fingerprint", "plans"});
    CoreMaterializationReceipt result;
    result.compiler_build_id             = require_string(value, "compiler_build_id");
    result.registry_snapshot_fingerprint = require_string(value, "registry_snapshot_fingerprint");
    const auto& plans                    = require_value(value, "plans");
    if (!plans.is_array()) {
        throw std::invalid_argument("Core materialization plans must be an array");
    }
    result.plans.reserve(plans.size());
    for (const auto& plan : plans)
        result.plans.push_back(parse_plan(plan));
    return result;
}

json version_body(const ProgramVersionData& data) {
    json value                 = json::object();
    value["bundle_id"]         = data.bundle_id;
    value["admission_profile"] = encode_admission(data.admission_profile);
    value["policy_snapshot"]   = encode_policy(data.policy_snapshot);

    json dependencies = json::array();
    for (const auto& receipt : data.dependency_receipts) {
        dependencies.push_back(encode_dependency(receipt));
    }
    value["dependency_receipts"] = std::move(dependencies);
    value["ownership_scope"]     = data.ownership_scope;
    value["core_materialization_receipt"] =
        encode_materialization(data.core_materialization_receipt);
    return value;
}

json version_identity_envelope(const ProgramVersionData& data) {
    auto value                      = version_body(data);
    value["format"]                 = std::string(VERSION_FORMAT);
    value["storage_schema_version"] = ProgramVersion::STORAGE_SCHEMA_VERSION;
    return value;
}

ProgramVersionData parse_body(const json& value) {
    ProgramVersionData data;
    data.bundle_id         = require_string(value, "bundle_id");
    data.admission_profile = parse_admission(require_value(value, "admission_profile"));
    data.policy_snapshot   = parse_policy(require_value(value, "policy_snapshot"));

    const auto& dependencies = require_value(value, "dependency_receipts");
    if (!dependencies.is_array()) {
        throw std::invalid_argument("Program version dependency_receipts must be an array");
    }
    data.dependency_receipts.reserve(dependencies.size());
    for (const auto& receipt : dependencies) {
        data.dependency_receipts.push_back(parse_dependency(receipt));
    }
    data.ownership_scope = require_string(value, "ownership_scope");
    data.core_materialization_receipt =
        parse_materialization(require_value(value, "core_materialization_receipt"));
    normalize_data(data);
    return data;
}

}  // namespace

struct ProgramVersion::Impl {
    ProgramVersionData data;
    std::string        id;
};

ProgramVersion::ProgramVersion(ProgramVersionData data) {
    normalize_data(data);
    auto impl  = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->id   = detail::sha256_identity(
        "program-version", detail::canonical_json_bytes(version_identity_envelope(impl->data)));
    impl_ = std::move(impl);
}

ProgramVersion::ProgramVersion(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramVersion ProgramVersion::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramVersion JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != VERSION_FORMAT) {
        throw std::invalid_argument("Stored ProgramVersion has unknown format");
    }
    detail::reject_unknown_fields(value, "Stored ProgramVersion",
                                  {"format", "storage_schema_version", "id", "bundle_id",
                                   "admission_profile", "policy_snapshot", "dependency_receipts",
                                   "ownership_scope", "core_materialization_receipt"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored ProgramVersion schema version is unsupported");
    }
    ProgramVersion parsed(parse_body(value));
    const auto     stored_id = require_string(value, "id");
    if (!detail::is_sha256_identity(stored_id) || stored_id != parsed.id()) {
        throw std::invalid_argument("Stored ProgramVersion id does not match its content");
    }
    return parsed;
}

const std::string& ProgramVersion::id() const noexcept {
    return impl_->id;
}
const std::string& ProgramVersion::bundle_id() const noexcept {
    return impl_->data.bundle_id;
}
const AdmissionProfileBinding& ProgramVersion::admission_profile() const noexcept {
    return impl_->data.admission_profile;
}
const PolicySnapshotBinding& ProgramVersion::policy_snapshot() const noexcept {
    return impl_->data.policy_snapshot;
}
const std::vector<DependencyReceipt>& ProgramVersion::dependency_receipts() const noexcept {
    return impl_->data.dependency_receipts;
}
const std::string& ProgramVersion::ownership_scope() const noexcept {
    return impl_->data.ownership_scope;
}
const CoreMaterializationReceipt& ProgramVersion::core_materialization_receipt() const noexcept {
    return impl_->data.core_materialization_receipt;
}

std::string ProgramVersion::serialize_canonical() const {
    auto value  = version_identity_envelope(impl_->data);
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}

}  // namespace neograph::program
