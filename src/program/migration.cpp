#include <neograph/program/migration.h>

#include "canonical_json.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
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

MigrationCompatibility compatibility_from_string(std::string_view value,
                                                bool allow_legacy_aliases = false) {
    if (value == "new_runs_only") return MigrationCompatibility::NewRunsOnly;
    if (value == "fork_compatible") return MigrationCompatibility::ForkCompatible;
    if (value == "drain_only") return MigrationCompatibility::DrainOnly;
    if (value == "operator_reconciliation") return MigrationCompatibility::OperatorReconciliation;
    if (value == "blocked") return MigrationCompatibility::Blocked;
    if (allow_legacy_aliases &&
        (value == "owner_mismatch" || value == "compiler_mismatch" ||
         value == "registry_mismatch" || value == "materialization_mismatch")) {
        // These P4 labels did not describe a complete compatibility class.
        // Preserve their fail-closed meaning, but never emit them as v1.
        return MigrationCompatibility::Blocked;
    }
    throw std::invalid_argument("Unknown MigrationPlan compatibility: " + std::string(value));
}

json encode_dimension(MigrationDimension dimension) {
    return std::string(to_string(dimension));
}

MigrationDimension parse_dimension(const json& value) {
    if (!value.is_string()) throw std::invalid_argument("Migration dimension must be a string");
    return migration_dimension_from_string(value.get<std::string>());
}

json encode_diagnostic(const MigrationDiagnostic& diagnostic) {
    return json{{"dimension", encode_dimension(diagnostic.dimension)},
                {"code", diagnostic.code},
                {"message", diagnostic.message},
                {"source", diagnostic.source},
                {"target", diagnostic.target}};
}

MigrationDiagnostic parse_diagnostic(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Migration diagnostic must be an object");
    detail::reject_unknown_fields(value, "Migration diagnostic",
                                  {"dimension", "code", "message", "source", "target"});
    if (!value.contains("source") || !value.contains("target"))
        throw std::invalid_argument("Migration diagnostic requires source and target values");
    return MigrationDiagnostic{parse_dimension(value.at("dimension")),
                               required_string(value, "code"),
                               required_string(value, "message"), value.at("source"),
                               value.at("target")};
}

json encode_mapping(const MigrationMapping& mapping) {
    return json{{"dimension", encode_dimension(mapping.dimension)},
                {"rule", mapping.rule},
                {"source", mapping.source},
                {"target", mapping.target}};
}

MigrationMapping parse_mapping(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Migration mapping must be an object");
    detail::reject_unknown_fields(value, "Migration mapping",
                                  {"dimension", "rule", "source", "target"});
    if (!value.contains("source") || !value.contains("target"))
        throw std::invalid_argument("Migration mapping requires source and target values");
    return MigrationMapping{parse_dimension(value.at("dimension")),
                            required_string(value, "rule"), value.at("source"),
                            value.at("target")};
}

json encode_diagnostics(const std::vector<MigrationDiagnostic>& values) {
    json result = json::array();
    for (const auto& value : values) result.push_back(encode_diagnostic(value));
    return result;
}

json encode_mappings(const std::vector<MigrationMapping>& values) {
    json result = json::array();
    for (const auto& value : values) result.push_back(encode_mapping(value));
    return result;
}

json body_for(const MigrationPlanData& data) {
    return json{{"format", std::string(FORMAT)},
                {"storage_schema_version", MigrationPlan::STORAGE_SCHEMA_VERSION},
                {"source_version_id", data.source_version_id},
                {"target_version_id", data.target_version_id},
                {"owner_scope", data.owner_scope},
                {"compatibility", std::string(to_string(data.compatibility))},
                {"blockers", data.blockers},
                {"diagnostics", encode_diagnostics(data.diagnostics)},
                {"mappings", encode_mappings(data.mappings)}};
}

json legacy_body_for(const MigrationPlanData& data, std::string_view compatibility,
                     std::uint32_t schema_version) {
    return json{{"format", std::string(FORMAT)},
                {"storage_schema_version", schema_version},
                {"source_version_id", data.source_version_id},
                {"target_version_id", data.target_version_id},
                {"owner_scope", data.owner_scope},
                {"compatibility", std::string(compatibility)},
                {"blockers", data.blockers}};
}

void add_diagnostic(MigrationPlanData& data, MigrationDimension dimension, std::string code,
                    std::string message, json source, json target) {
    data.diagnostics.push_back(MigrationDiagnostic{dimension, std::move(code), std::move(message),
                                                   std::move(source), std::move(target)});
}

void add_mapping(MigrationPlanData& data, MigrationDimension dimension, std::string rule,
                 json source, json target) {
    data.mappings.push_back(MigrationMapping{dimension, std::move(rule), std::move(source),
                                             std::move(target)});
}

json string_set(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    return values;
}

json budget_json(const BudgetLimits& budget) {
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

bool budget_covers(const BudgetLimits& target, const BudgetLimits& source) {
    return target.wall_time_ms >= source.wall_time_ms &&
           target.model_tokens >= source.model_tokens &&
           target.monetary_microunits >= source.monetary_microunits &&
           target.max_concurrency >= source.max_concurrency &&
           target.max_program_operations >= source.max_program_operations &&
           target.max_core_steps >= source.max_core_steps &&
           target.max_dynamic_compiles >= source.max_dynamic_compiles &&
           target.max_child_depth >= source.max_child_depth &&
           target.max_total_children >= source.max_total_children;
}

json executable_json(const ExecutableIdentity& executable) {
    return json{{"kind", std::string(to_string(executable.kind))},
                {"name", executable.name},
                {"semantic_version", executable.semantic_version},
                {"implementation_digest", executable.implementation_digest}};
}

json executable_list_json(std::vector<ExecutableIdentity> values) {
    std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
        return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                          lhs.implementation_digest} <
               std::tuple{to_string(rhs.kind), rhs.name, rhs.semantic_version,
                          rhs.implementation_digest};
    });
    json result = json::array();
    for (const auto& value : values) result.push_back(executable_json(value));
    return result;
}

json profile_authority_json(const AdmissionProfile& profile, const PolicySnapshot& policy) {
    std::vector<std::string> source_kind_values;
    for (const auto kind : profile.allowed_source_kinds())
        source_kind_values.push_back(std::string(to_string(kind)));
    std::sort(source_kind_values.begin(), source_kind_values.end());
    json source_kinds = source_kind_values;
    std::vector<std::string> effect_mode_values;
    for (const auto mode : profile.allowed_effect_modes())
        effect_mode_values.push_back(std::string(to_string(mode)));
    std::sort(effect_mode_values.begin(), effect_mode_values.end());
    json effect_modes = effect_mode_values;
    return json{
        {"profile_fingerprint", profile.fingerprint()},
        {"profile_registry_fingerprint", profile.registry_fingerprint()},
        {"profile_mode", std::string(to_string(profile.mode()))},
        {"profile_max_program_schema_version", profile.max_program_schema_version()},
        {"profile_allowed_source_kinds", std::move(source_kinds)},
        {"profile_allowed_executables", executable_list_json(profile.allowed_executables())},
        {"profile_allowed_effect_modes", std::move(effect_modes)},
        {"policy_fingerprint", policy.fingerprint()},
        {"policy_admission_profile_fingerprint", policy.admission_profile_fingerprint()},
        {"policy_registry_fingerprint", policy.registry_fingerprint()},
        {"capabilities", string_set(policy.allowed_capabilities())},
        {"effects", string_set(policy.allowed_effects())},
        {"modules", string_set(policy.allowed_module_digests())}};
}
json authority_closure(const json& value) {
    return json{{"profile_fingerprint", value.at("profile_fingerprint")},
                {"profile_registry_fingerprint", value.at("profile_registry_fingerprint")},
                {"profile_mode", value.at("profile_mode")},
                {"profile_max_program_schema_version",
                 value.at("profile_max_program_schema_version")},
                {"profile_allowed_source_kinds", value.at("profile_allowed_source_kinds")},
                {"profile_allowed_executables", value.at("profile_allowed_executables")},
                {"profile_allowed_effect_modes", value.at("profile_allowed_effect_modes")},
                {"policy_admission_profile_fingerprint",
                 value.at("policy_admission_profile_fingerprint")},
                {"policy_registry_fingerprint", value.at("policy_registry_fingerprint")},
                {"capabilities", value.at("capabilities")},
                {"effects", value.at("effects")},
                {"modules", value.at("modules")}};
}


json materialization_json(const CoreMaterializationReceipt& receipt) {
    json plans = json::array();
    for (const auto& plan : receipt.plans)
        plans.push_back(json{{"name", plan.name},
                             {"compiled_plan_identity", plan.compiled_plan_identity}});
    json bindings = json::array();
    for (const auto& binding : receipt.capability_bindings)
        bindings.push_back(json{{"executable", executable_json(binding.executable)},
                                {"binding_identity", binding.binding_identity}});
    return json{{"compiler_build_id", receipt.compiler_build_id},
                {"registry_snapshot_fingerprint", receipt.registry_snapshot_fingerprint},
                {"plans", std::move(plans)},
                {"capability_bindings", std::move(bindings)}};
}

}  // namespace

struct MigrationPlan::Impl {
    explicit Impl(MigrationPlanData value) : data(std::move(value)) {}
    MigrationPlanData data;
    std::string       id;
};

bool MigrationDiagnostic::operator==(const MigrationDiagnostic& other) const {
    return dimension == other.dimension && code == other.code && message == other.message &&
           detail::canonical_json_bytes(source) == detail::canonical_json_bytes(other.source) &&
           detail::canonical_json_bytes(target) == detail::canonical_json_bytes(other.target);
}

bool MigrationMapping::operator==(const MigrationMapping& other) const {
    return dimension == other.dimension && rule == other.rule &&
           detail::canonical_json_bytes(source) == detail::canonical_json_bytes(other.source) &&
           detail::canonical_json_bytes(target) == detail::canonical_json_bytes(other.target);
}

MigrationPlan MigrationPlan::create(MigrationPlanData data) {
    if (!detail::is_sha256_identity(data.source_version_id) ||
        !detail::is_sha256_identity(data.target_version_id)) {
        throw std::invalid_argument("Migration version ids must be sha256 identities");
    }
    detail::validate_token(data.owner_scope, "Migration owner scope");
    for (const auto& blocker : data.blockers)
        detail::validate_token(blocker, "Migration blocker");
    for (const auto& diagnostic : data.diagnostics) {
        detail::validate_token(diagnostic.code, "Migration diagnostic code");
        detail::validate_token(diagnostic.message, "Migration diagnostic message");
        // json defaults to an object and null is a valid witness value.
    }
    for (const auto& mapping : data.mappings)
        detail::validate_token(mapping.rule, "Migration mapping rule");

    // P4 enum aliases remain source-compatible at the type level, but all
    // persisted plans use exactly one of the five v1 classes.
    if (static_cast<std::uint8_t>(data.compatibility) >=
            static_cast<std::uint8_t>(MigrationCompatibility::OwnerMismatch) &&
        static_cast<std::uint8_t>(data.compatibility) <=
            static_cast<std::uint8_t>(MigrationCompatibility::MaterializationMismatch)) {
        data.compatibility = MigrationCompatibility::Blocked;
    }
    if (data.compatibility == MigrationCompatibility::ForkCompatible && !data.blockers.empty())
        throw std::invalid_argument("Compatible MigrationPlan cannot contain blockers");
    if (data.compatibility != MigrationCompatibility::ForkCompatible && data.blockers.empty()) {
        for (const auto& diagnostic : data.diagnostics)
            data.blockers.push_back(diagnostic.code);
        if (data.blockers.empty()) data.blockers.push_back("compatibility");
    }
    if (data.compatibility != MigrationCompatibility::ForkCompatible &&
        data.diagnostics.empty()) {
        for (const auto& blocker : data.blockers)
            data.diagnostics.push_back(MigrationDiagnostic{
                MigrationDimension::Recovery, blocker,
                "Migration requires explicit handling", nullptr, nullptr});
    }

    auto impl = std::make_shared<Impl>(std::move(data));
    impl->id = detail::sha256_identity("program-migration-plan/v1",
                                       detail::canonical_json_bytes(body_for(impl->data)));
    return MigrationPlan(std::move(impl));
}
MigrationPlan MigrationPlan::between(const ProgramVersion& source,
                                     const ProgramVersion& target) {
    MigrationPlanData data{source.id(), target.id(), source.ownership_scope(),
                           MigrationCompatibility::ForkCompatible, {}, {}, {}};

    const auto source_profile = source.admission_profile();
    const auto target_profile = target.admission_profile();
    const auto source_policy  = source.policy_snapshot();
    const auto target_policy  = target.policy_snapshot();
    const auto source_auth    = profile_authority_json(source_profile, source_policy);
    const auto target_auth    = profile_authority_json(target_profile, target_policy);
    const auto source_material =
        materialization_json(source.core_materialization_receipt());
    const auto target_material =
        materialization_json(target.core_materialization_receipt());
    // Policy fingerprints identify immutable snapshots and therefore may
    // differ across admitted versions even when their authority closure is
    // exactly the same. The closure fields below are the migration proof.
    const auto source_authority = authority_closure(source_auth);
    const auto target_authority = authority_closure(target_auth);

    // Preserve the original fourteen P5 mappings and their persisted order.
    add_mapping(data, MigrationDimension::Channels, "exact_channel_reducer_and_presence",
                source.bundle_id(), target.bundle_id());
    add_mapping(data, MigrationDimension::Continuation, "exact_checkpoint_continuation",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::PendingInput, "preserve_exact_call_identity",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::PendingEffect, "preserve_exact_effect_identity",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::Retry, "preserve_attempt_and_retry_identity",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::Cancellation, "preserve_cancellation_identity",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::Interrupt, "preserve_interrupt_projection",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::Budget, "target_ceiling_must_cover_source",
                budget_json(source_policy.budget_ceiling()),
                budget_json(target_policy.budget_ceiling()));
    add_mapping(data, MigrationDimension::Authority,
                "exact_admission_profile_and_authority", source_auth, target_auth);
    add_mapping(data, MigrationDimension::Checkpoint, "exact_core_materialization_identity",
                source_material, target_material);
    add_mapping(data, MigrationDimension::Journal, "publish_plan_with_fork_head",
                source.id(), target.id());
    add_mapping(data, MigrationDimension::Effects,
                "stable_outbox_identity_and_no_redispatch", source.id(), target.id());
    add_mapping(data, MigrationDimension::Caches, "pinned_materialization_only",
                source_material, target_material);
    add_mapping(data, MigrationDimension::Output, "exact_output_contract",
                source.bundle_id(), target.bundle_id());

    const auto mismatch = [&](MigrationDimension dimension, std::string code,
                              std::string message, json source_value,
                              json target_value, MigrationCompatibility class_hint) {
        add_diagnostic(data, dimension, std::move(code), std::move(message),
                       std::move(source_value), std::move(target_value));
        // The strongest class wins.  This makes classification independent of
        // comparison order while retaining every deterministic witness.
        const auto rank = [](MigrationCompatibility value) {
            switch (value) {
                case MigrationCompatibility::ForkCompatible: return 0;
                case MigrationCompatibility::NewRunsOnly: return 1;
                case MigrationCompatibility::DrainOnly: return 2;
                case MigrationCompatibility::OperatorReconciliation: return 3;
                case MigrationCompatibility::Blocked: return 4;
                default: return 4;
            }
        };
        if (rank(class_hint) > rank(data.compatibility)) data.compatibility = class_hint;
    };

    if (source.ownership_scope() != target.ownership_scope())
        mismatch(MigrationDimension::Authority, "ownership_scope",
                 "Migration cannot cross an owner scope boundary",
                 source.ownership_scope(), target.ownership_scope(),
                 MigrationCompatibility::Blocked);

    if (source_authority != target_authority)
        mismatch(MigrationDimension::Authority, "authority_profile",
                 "Migration changes an admitted profile or authority closure",
                 source_auth, target_auth, MigrationCompatibility::Blocked);

    const auto& source_receipt = source.core_materialization_receipt();
    const auto& target_receipt = target.core_materialization_receipt();
    if (source_receipt.compiler_build_id != target_receipt.compiler_build_id)
        mismatch(MigrationDimension::Compiler, "compiler_build_id",
                 "Pinned Core materialization was built by a different compiler",
                 source_receipt.compiler_build_id, target_receipt.compiler_build_id,
                 MigrationCompatibility::NewRunsOnly);
    if (source_receipt.registry_snapshot_fingerprint !=
        target_receipt.registry_snapshot_fingerprint)
        mismatch(MigrationDimension::Registry, "registry_snapshot_fingerprint",
                 "Migration changes the admitted executable registry snapshot",
                 source_receipt.registry_snapshot_fingerprint,
                 target_receipt.registry_snapshot_fingerprint,
                 MigrationCompatibility::DrainOnly);
    if (source_receipt.plans != target_receipt.plans ||
        source_receipt.capability_bindings != target_receipt.capability_bindings)
        mismatch(MigrationDimension::Materialization, "core_materialization_receipt",
                 "Migration changes pinned Core plans or capability bindings",
                 source_material, target_material,
                 MigrationCompatibility::OperatorReconciliation);

    if (source.bundle_id() != target.bundle_id())
        mismatch(MigrationDimension::Contract, "bundle_id",
                 "Migration changes the immutable bundle and its contracts",
                 source.bundle_id(), target.bundle_id(), MigrationCompatibility::Blocked);

    if (source.dependency_receipts() != target.dependency_receipts())
        mismatch(MigrationDimension::Recovery, "dependency_receipts",
                 "Migration changes dependency identities required for recovery",
                 source.id(), target.id(), MigrationCompatibility::OperatorReconciliation);

    if (!budget_covers(target_policy.budget_ceiling(), source_policy.budget_ceiling()))
        mismatch(MigrationDimension::Recovery, "budget_ceiling",
                 "Migration cannot prove preservation of the source budget ledger",
                 budget_json(source_policy.budget_ceiling()),
                 budget_json(target_policy.budget_ceiling()),
                 MigrationCompatibility::OperatorReconciliation);


    return create(std::move(data));
}

MigrationPlan MigrationPlan::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object()) throw std::invalid_argument("Stored MigrationPlan must be an object");
    detail::reject_unknown_fields(
        value, "Stored MigrationPlan",
        {"format", "storage_schema_version", "legacy_schema_version",
         "source_version_id", "target_version_id", "owner_scope", "compatibility",
         "blockers", "diagnostics", "mappings", "id"});
    if (!value.contains("format") || value["format"] != FORMAT)
        throw std::invalid_argument("Stored MigrationPlan format is unsupported");
    if (!value.contains("storage_schema_version") ||
        !value["storage_schema_version"].is_number_unsigned())
        throw std::invalid_argument("Stored MigrationPlan schema version is missing");
    const auto schema_version = value["storage_schema_version"].get<std::uint32_t>();
    const bool explicit_legacy =
        value.contains("legacy_schema_version") &&
        value["legacy_schema_version"].is_number_unsigned() &&
        value["legacy_schema_version"].get<std::uint32_t>() ==
            MigrationPlan::LEGACY_STORAGE_SCHEMA_VERSION;
    const bool legacy = schema_version == MigrationPlan::LEGACY_STORAGE_SCHEMA_VERSION ||
                        explicit_legacy;
    if (!legacy && schema_version != MigrationPlan::STORAGE_SCHEMA_VERSION)
        throw std::invalid_argument("Stored MigrationPlan schema version is unsupported");
    if (legacy && value.contains("legacy_schema_version") &&
        (!value["legacy_schema_version"].is_number_unsigned() ||
         value["legacy_schema_version"].get<std::uint32_t>() !=
             MigrationPlan::LEGACY_STORAGE_SCHEMA_VERSION))
        throw std::invalid_argument("Stored MigrationPlan legacy schema is unsupported");

    if (!value.contains("compatibility") || !value["compatibility"].is_string())
        throw std::invalid_argument("Stored MigrationPlan compatibility is missing");
    if (!value.contains("blockers") || !value["blockers"].is_array())
        throw std::invalid_argument("Stored MigrationPlan blockers must be an array");
    const bool has_diagnostics = value.contains("diagnostics");
    const bool has_mappings    = value.contains("mappings");
    if (!legacy && (!has_diagnostics || !has_mappings))
        throw std::invalid_argument(
            "Stored v1 MigrationPlan requires diagnostics and mappings");
    if (legacy && (has_diagnostics != has_mappings))
        throw std::invalid_argument(
            "Stored legacy MigrationPlan must omit both diagnostics and mappings");

    std::vector<std::string> blockers;
    for (const auto& blocker : value["blockers"]) {
        if (!blocker.is_string() || blocker.get<std::string>().empty())
            throw std::invalid_argument("Stored MigrationPlan blocker is not a nonempty string");
        blockers.push_back(blocker.get<std::string>());
    }
    std::vector<MigrationDiagnostic> diagnostics;
    if (has_diagnostics) {
        if (!value["diagnostics"].is_array())
            throw std::invalid_argument("Stored MigrationPlan diagnostics must be an array");
        for (const auto& diagnostic : value["diagnostics"])
            diagnostics.push_back(parse_diagnostic(diagnostic));
    }
    std::vector<MigrationMapping> mappings;
    if (has_mappings) {
        if (!value["mappings"].is_array())
            throw std::invalid_argument("Stored MigrationPlan mappings must be an array");
        for (const auto& mapping : value["mappings"])
            mappings.push_back(parse_mapping(mapping));
    }

    const auto stored_compatibility = value["compatibility"].get<std::string>();
    MigrationPlanData data{required_string(value, "source_version_id"),
                           required_string(value, "target_version_id"),
                           required_string(value, "owner_scope"),
                           compatibility_from_string(stored_compatibility, legacy),
                           std::move(blockers), std::move(diagnostics), std::move(mappings)};
    auto result = create(data);
    if (!value.contains("id") || !value["id"].is_string())
        throw std::invalid_argument("Stored MigrationPlan identity is missing");

    if (legacy) {
        const auto expected_legacy_id = detail::sha256_identity(
            "program-migration-plan/v1",
            detail::canonical_json_bytes(
                legacy_body_for(data, stored_compatibility, schema_version)));
        if (value["id"] != expected_legacy_id)
            throw std::invalid_argument(
                "Stored legacy MigrationPlan identity does not match its content");
    } else if (value["id"] != result.id()) {
        throw std::invalid_argument("Stored MigrationPlan identity does not match its content");
    }
    return result;
}

const std::string& MigrationPlan::source_version_id() const noexcept { return impl_->data.source_version_id; }
const std::string& MigrationPlan::target_version_id() const noexcept { return impl_->data.target_version_id; }
const std::string& MigrationPlan::owner_scope() const noexcept { return impl_->data.owner_scope; }
MigrationCompatibility MigrationPlan::compatibility() const noexcept { return impl_->data.compatibility; }
const std::vector<std::string>& MigrationPlan::blockers() const noexcept { return impl_->data.blockers; }
const std::vector<MigrationDiagnostic>& MigrationPlan::diagnostics() const noexcept { return impl_->data.diagnostics; }
const std::vector<MigrationMapping>& MigrationPlan::mappings() const noexcept { return impl_->data.mappings; }
bool MigrationPlan::is_compatible() const noexcept { return compatibility() == MigrationCompatibility::ForkCompatible; }
const std::string& MigrationPlan::id() const noexcept { return impl_->id; }

std::string MigrationPlan::serialize_canonical() const {
    auto value  = body_for(impl_->data);
    value["id"] = id();
    return detail::canonical_json_bytes(value);
}

MigrationPlan::MigrationPlan(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::string_view to_string(MigrationCompatibility compatibility) noexcept {
    switch (compatibility) {
        case MigrationCompatibility::NewRunsOnly: return "new_runs_only";
        case MigrationCompatibility::ForkCompatible: return "fork_compatible";
        case MigrationCompatibility::DrainOnly: return "drain_only";
        case MigrationCompatibility::OperatorReconciliation: return "operator_reconciliation";
        case MigrationCompatibility::Blocked: return "blocked";
        case MigrationCompatibility::OwnerMismatch: return "owner_mismatch";
        case MigrationCompatibility::CompilerMismatch: return "compiler_mismatch";
        case MigrationCompatibility::RegistryMismatch: return "registry_mismatch";
        case MigrationCompatibility::MaterializationMismatch: return "materialization_mismatch";
    }
    return "blocked";
}

MigrationCompatibility migration_compatibility_from_string(std::string_view value) {
    return compatibility_from_string(value);
}

std::string_view to_string(MigrationDimension dimension) noexcept {
    switch (dimension) {
        case MigrationDimension::Channels: return "channels";
        case MigrationDimension::Continuation: return "continuation";
        case MigrationDimension::PendingInput: return "pending_input";
        case MigrationDimension::PendingEffect: return "pending_effect";
        case MigrationDimension::Retry: return "retry";
        case MigrationDimension::Cancellation: return "cancellation";
        case MigrationDimension::Interrupt: return "interrupt";
        case MigrationDimension::Budget: return "budget";
        case MigrationDimension::Authority: return "authority";
        case MigrationDimension::Checkpoint: return "checkpoint";
        case MigrationDimension::Journal: return "journal";
        case MigrationDimension::Effects: return "effects";
        case MigrationDimension::Caches: return "caches";
        case MigrationDimension::Output: return "output";
        case MigrationDimension::Compiler: return "compiler";
        case MigrationDimension::Registry: return "registry";
        case MigrationDimension::Materialization: return "materialization";
        case MigrationDimension::Contract: return "contract";
        case MigrationDimension::Recovery: return "recovery";
    }
    return "continuation";
}
MigrationDimension migration_dimension_from_string(std::string_view value) {
    if (value == "channels") return MigrationDimension::Channels;
    if (value == "continuation") return MigrationDimension::Continuation;
    if (value == "pending_input") return MigrationDimension::PendingInput;
    if (value == "pending_effect") return MigrationDimension::PendingEffect;
    if (value == "retry") return MigrationDimension::Retry;
    if (value == "cancellation") return MigrationDimension::Cancellation;
    if (value == "interrupt") return MigrationDimension::Interrupt;
    if (value == "budget") return MigrationDimension::Budget;
    if (value == "authority") return MigrationDimension::Authority;
    if (value == "checkpoint") return MigrationDimension::Checkpoint;
    if (value == "journal") return MigrationDimension::Journal;
    if (value == "effects") return MigrationDimension::Effects;
    if (value == "caches") return MigrationDimension::Caches;
    if (value == "output") return MigrationDimension::Output;
    if (value == "compiler") return MigrationDimension::Compiler;
    if (value == "registry") return MigrationDimension::Registry;
    if (value == "materialization") return MigrationDimension::Materialization;
    if (value == "contract") return MigrationDimension::Contract;
    if (value == "recovery") return MigrationDimension::Recovery;
    throw std::invalid_argument("Unknown MigrationPlan dimension: " + std::string(value));
}

}  // namespace neograph::program
