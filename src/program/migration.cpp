#include <neograph/program/migration.h>

#include "canonical_json.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORMAT = "neograph-program-migration-plan";

std::string required_string(const json& value, std::string_view name) {
    const auto key = std::string(name);
    if (!value.contains(key) || !value[key].is_string() || value[key].get<std::string>().empty())
        throw std::invalid_argument("MigrationPlan field '" + key + "' must be a nonempty string");
    return value[key].get<std::string>();
}

MigrationCompatibility compatibility_from_string(std::string_view value) {
    if (value == "compatible") return MigrationCompatibility::Compatible;
    if (value == "owner_mismatch") return MigrationCompatibility::OwnerMismatch;
    if (value == "compiler_mismatch") return MigrationCompatibility::CompilerMismatch;
    if (value == "registry_mismatch") return MigrationCompatibility::RegistryMismatch;
    if (value == "materialization_mismatch") return MigrationCompatibility::MaterializationMismatch;
    throw std::invalid_argument("Unknown MigrationPlan compatibility: " + std::string(value));
}

json body_for(const MigrationPlanData& data) {
    return json{{"format", std::string(FORMAT)},
                {"storage_schema_version", MigrationPlan::STORAGE_SCHEMA_VERSION},
                {"source_version_id", data.source_version_id},
                {"target_version_id", data.target_version_id},
                {"owner_scope", data.owner_scope},
                {"compatibility", std::string(to_string(data.compatibility))},
                {"blockers", data.blockers}};
}

}  // namespace

struct MigrationPlan::Impl {
    explicit Impl(MigrationPlanData value) : data(std::move(value)) {}
    MigrationPlanData data;
    std::string       id;
};

MigrationPlan MigrationPlan::create(MigrationPlanData data) {
    detail::validate_token(data.source_version_id, "Migration source version id");
    detail::validate_token(data.target_version_id, "Migration target version id");
    detail::validate_token(data.owner_scope, "Migration owner scope");
    for (const auto& blocker : data.blockers)
        detail::validate_token(blocker, "Migration blocker");
    if (data.compatibility == MigrationCompatibility::Compatible && !data.blockers.empty())
        throw std::invalid_argument("Compatible MigrationPlan cannot contain blockers");
    if (data.compatibility != MigrationCompatibility::Compatible && data.blockers.empty())
        throw std::invalid_argument("Incompatible MigrationPlan must explain its blocker");

    auto impl = std::make_shared<Impl>(std::move(data));
    impl->id = detail::sha256_identity("program-migration-plan/v1",
                                       detail::canonical_json_bytes(body_for(impl->data)));
    return MigrationPlan(std::move(impl));
}

MigrationPlan MigrationPlan::between(const ProgramVersion& source, const ProgramVersion& target) {
    MigrationPlanData data{source.id(), target.id(), source.ownership_scope(),
                           MigrationCompatibility::Compatible, {}};
    if (source.ownership_scope() != target.ownership_scope()) {
        data.compatibility = MigrationCompatibility::OwnerMismatch;
        data.blockers.push_back("ownership_scope");
    } else if (source.core_materialization_receipt().compiler_build_id !=
               target.core_materialization_receipt().compiler_build_id) {
        data.compatibility = MigrationCompatibility::CompilerMismatch;
        data.blockers.push_back("compiler_build_id");
    } else if (source.core_materialization_receipt().registry_snapshot_fingerprint !=
               target.core_materialization_receipt().registry_snapshot_fingerprint) {
        data.compatibility = MigrationCompatibility::RegistryMismatch;
        data.blockers.push_back("registry_snapshot_fingerprint");
    } else if (source.core_materialization_receipt() != target.core_materialization_receipt()) {
        data.compatibility = MigrationCompatibility::MaterializationMismatch;
        data.blockers.push_back("core_materialization_receipt");
    }
    return create(std::move(data));
}

MigrationPlan MigrationPlan::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) throw std::invalid_argument("Stored MigrationPlan must be an object");
    detail::reject_unknown_fields(value, "Stored MigrationPlan",
                                  {"format", "storage_schema_version", "source_version_id",
                                   "target_version_id", "owner_scope", "compatibility", "blockers", "id"});
    if (!value.contains("format") || value["format"] != FORMAT)
        throw std::invalid_argument("Stored MigrationPlan format is unsupported");
    if (!value.contains("storage_schema_version") ||
        value["storage_schema_version"] != STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored MigrationPlan schema version is unsupported");
    if (!value.contains("compatibility") || !value["compatibility"].is_string())
        throw std::invalid_argument("Stored MigrationPlan compatibility is missing");
    if (!value.contains("blockers") || !value["blockers"].is_array())
        throw std::invalid_argument("Stored MigrationPlan blockers must be an array");
    std::vector<std::string> blockers;
    for (const auto& blocker : value["blockers"]) {
        if (!blocker.is_string()) throw std::invalid_argument("Stored MigrationPlan blocker is not a string");
        blockers.push_back(blocker.get<std::string>());
    }
    auto result = create(MigrationPlanData{required_string(value, "source_version_id"),
                                           required_string(value, "target_version_id"),
                                           required_string(value, "owner_scope"),
                                           compatibility_from_string(value["compatibility"].get<std::string>()),
                                           std::move(blockers)});
    if (!value.contains("id") || !value["id"].is_string() || value["id"] != result.id())
        throw std::invalid_argument("Stored MigrationPlan identity does not match its content");
    return result;
}

const std::string& MigrationPlan::source_version_id() const noexcept { return impl_->data.source_version_id; }
const std::string& MigrationPlan::target_version_id() const noexcept { return impl_->data.target_version_id; }
const std::string& MigrationPlan::owner_scope() const noexcept { return impl_->data.owner_scope; }
MigrationCompatibility MigrationPlan::compatibility() const noexcept { return impl_->data.compatibility; }
const std::vector<std::string>& MigrationPlan::blockers() const noexcept { return impl_->data.blockers; }
bool MigrationPlan::is_compatible() const noexcept {
    return compatibility() == MigrationCompatibility::Compatible;
}
const std::string& MigrationPlan::id() const noexcept { return impl_->id; }

std::string MigrationPlan::serialize_canonical() const {
    auto value = body_for(impl_->data);
    value["id"] = id();
    return detail::canonical_json_bytes(value);
}

MigrationPlan::MigrationPlan(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::string_view to_string(MigrationCompatibility compatibility) noexcept {
    switch (compatibility) {
        case MigrationCompatibility::Compatible: return "compatible";
        case MigrationCompatibility::OwnerMismatch: return "owner_mismatch";
        case MigrationCompatibility::CompilerMismatch: return "compiler_mismatch";
        case MigrationCompatibility::RegistryMismatch: return "registry_mismatch";
        case MigrationCompatibility::MaterializationMismatch: return "materialization_mismatch";
    }
    return "materialization_mismatch";
}

MigrationCompatibility migration_compatibility_from_string(std::string_view value) {
    return compatibility_from_string(value);
}

}  // namespace neograph::program
