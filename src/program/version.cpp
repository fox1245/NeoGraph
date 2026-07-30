#include <neograph/program/version.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

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

void validate_data(const ProgramVersionData& data) {
    if (!detail::is_sha256_identity(data.bundle_id)) {
        throw std::invalid_argument("Program version bundle_id must be a sha256 identity");
    }
    if (!data.admission_profile.is_object() || !data.policy_snapshot.is_object() ||
        !data.dependency_receipts.is_array() || data.ownership_scope.empty() ||
        !data.core_materialization_receipt.is_object()) {
        throw std::invalid_argument("Program version stored value fields have invalid JSON types");
    }
}

json version_body(const ProgramVersionData& data) {
    json value                            = json::object();
    value["bundle_id"]                    = data.bundle_id;
    value["admission_profile"]            = data.admission_profile;
    value["policy_snapshot"]              = data.policy_snapshot;
    value["dependency_receipts"]          = data.dependency_receipts;
    value["ownership_scope"]              = data.ownership_scope;
    value["core_materialization_receipt"] = data.core_materialization_receipt;
    return value;
}

ProgramVersionData parse_body(const json& value) {
    ProgramVersionData data;
    data.bundle_id                    = require_string(value, "bundle_id");
    data.admission_profile            = require_value(value, "admission_profile");
    data.policy_snapshot              = require_value(value, "policy_snapshot");
    data.dependency_receipts          = require_value(value, "dependency_receipts");
    data.ownership_scope              = require_string(value, "ownership_scope");
    data.core_materialization_receipt = require_value(value, "core_materialization_receipt");
    validate_data(data);
    return data;
}

}  // namespace

struct ProgramVersion::Impl {
    ProgramVersionData data;
    std::string        id;
};

ProgramVersion::ProgramVersion(ProgramVersionData data) {
    validate_data(data);
    auto impl  = std::make_shared<Impl>();
    impl->data = std::move(data);
    impl->id   = detail::sha256_identity("program-version",
                                         detail::canonical_json_bytes(version_body(impl->data)));
    impl_      = std::move(impl);
}

ProgramVersion::ProgramVersion(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramVersion ProgramVersion::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = json::parse(std::string(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored ProgramVersion JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != "neograph-program-version") {
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
json ProgramVersion::admission_profile() const {
    return impl_->data.admission_profile;
}
json ProgramVersion::policy_snapshot() const {
    return impl_->data.policy_snapshot;
}
json ProgramVersion::dependency_receipts() const {
    return impl_->data.dependency_receipts;
}
const std::string& ProgramVersion::ownership_scope() const noexcept {
    return impl_->data.ownership_scope;
}
json ProgramVersion::core_materialization_receipt() const {
    return impl_->data.core_materialization_receipt;
}

std::string ProgramVersion::serialize_canonical() const {
    auto value                      = version_body(impl_->data);
    value["format"]                 = "neograph-program-version";
    value["storage_schema_version"] = STORAGE_SCHEMA_VERSION;
    value["id"]                     = impl_->id;
    return detail::canonical_json_bytes(value);
}

}  // namespace neograph::program
