#include <neograph/program/admission.h>
#include <neograph/program/version.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

RegistrySnapshot registry_snapshot(std::string name = "reducer") {
    RegistrySnapshotBuilder builder;
    builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, std::move(name), "1.0.0", digest('1')},
                           EffectMode::Brokered,
                           "attestation:test",
                           {},
                           {}},
        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry,
                                   AdmissionMode           mode    = AdmissionMode::MultiTenant,
                                   bool                    reverse = false) {
    AdmissionProfileBuilder builder;
    builder.id("profile")
        .semantic_version("1.2.3")
        .registry(registry)
        .mode(mode)
        .max_program_schema_version(1);
    if (reverse) {
        builder.allow_source_kind(SourceKind::CppBuilder)
            .allow_source_kind(SourceKind::JavaScript);
    } else {
        builder.allow_source_kind(SourceKind::JavaScript)
            .allow_source_kind(SourceKind::CppBuilder);
    }
    builder.allow_executable(registry.identities().front());
    builder.allow_effect_mode(EffectMode::Brokered);
    if (mode == AdmissionMode::TrustedEmbedding) {
        builder.allow_effect_mode(EffectMode::TrustedNative);
    }
    return std::move(builder).build();
}

BudgetLimits finite_budget() {
    return BudgetLimits{100, 200, 300, 4, 500, 600, 7, 8, 9};
}

PolicySnapshot policy_snapshot(const AdmissionProfile& admission, bool reverse = false) {
    PolicySnapshotBuilder builder;
    builder.id("policy")
        .semantic_version("2.0.0")
        .owner_scope("tenant:one")
        .admission_profile(admission)
        .budget_ceiling(finite_budget());
    if (reverse) {
        builder.allow_capability("zeta").allow_capability("alpha");
        builder.allow_effect("write").allow_effect("read");
    } else {
        builder.allow_capability("alpha").allow_capability("zeta");
        builder.allow_effect("read").allow_effect("write");
    }
    return std::move(builder).build();
}

ProgramVersionData version_data(const AdmissionProfile& admission, const PolicySnapshot& policy) {
    return ProgramVersionData(
        digest('a'), admission, policy, {}, policy.owner_scope(),
        CoreMaterializationReceipt{
            "compiler", admission.registry_fingerprint(), {CorePlanIdentity{"main", digest('b')}}});
}

}  // namespace

TEST(AdmissionProfileTest, CanonicalFingerprintIsOrderIndependentAndRoundTripsStrictly) {
    const auto registry = registry_snapshot();
    const auto first    = admission_profile(registry, AdmissionMode::MultiTenant, false);
    const auto second   = admission_profile(registry, AdmissionMode::MultiTenant, true);

    EXPECT_EQ(first.fingerprint(), second.fingerprint());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    const auto parsed = AdmissionProfile::parse(first.serialize_canonical());
    EXPECT_EQ(parsed.fingerprint(), first.fingerprint());
    EXPECT_EQ(parsed.allowed_source_kinds().size(), 2U);

    auto unknown      = first.manifest();
    unknown["future"] = true;
    EXPECT_THROW((void)AdmissionProfile::parse(unknown.dump()), std::invalid_argument);
    auto tampered                          = first.manifest();
    tampered["max_program_schema_version"] = 2;
    EXPECT_THROW((void)AdmissionProfile::parse(tampered.dump()), std::invalid_argument);
    auto negative_version                          = first.manifest();
    negative_version["max_program_schema_version"] = -1;
    EXPECT_THROW((void)AdmissionProfile::parse(negative_version.dump()), std::invalid_argument);
    auto overflowing_version                          = first.manifest();
    overflowing_version["max_program_schema_version"] = 4294967296ULL;
    EXPECT_THROW((void)AdmissionProfile::parse(overflowing_version.dump()), std::invalid_argument);
}

TEST(AdmissionProfileTest, EmptyAllowlistsFormAValidDenyAllProfile) {
    AdmissionProfileBuilder builder;
    builder.id("deny-all").semantic_version("1.0.0").registry(registry_snapshot());
    const auto profile = std::move(builder).build();

    EXPECT_TRUE(profile.allowed_source_kinds().empty());
    EXPECT_TRUE(profile.allowed_executables().empty());
    EXPECT_TRUE(profile.allowed_effect_modes().empty());
    EXPECT_EQ(AdmissionProfile::parse(profile.serialize_canonical()).fingerprint(),
              profile.fingerprint());
}

TEST(AdmissionProfileTest, MultiTenantRejectsTrustedNativeAndDuplicatesFailClosed) {
    const auto              registry = registry_snapshot();
    AdmissionProfileBuilder native;
    native.id("profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered)
        .allow_effect_mode(EffectMode::TrustedNative);
    EXPECT_THROW((void)std::move(native).build(), std::invalid_argument);

    AdmissionProfileBuilder duplicate;
    duplicate.id("profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    EXPECT_THROW((void)std::move(duplicate).build(), std::invalid_argument);

    AdmissionProfileBuilder unknown_executable;
    unknown_executable.id("profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_executable(ExecutableIdentity{ExecutableKind::Node, "missing", "1.0.0", digest('9')})
        .allow_effect_mode(EffectMode::Brokered);
    EXPECT_THROW((void)std::move(unknown_executable).build(), std::invalid_argument);

    AdmissionProfileBuilder omitted_effect;
    omitted_effect.id("profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::TrustedEmbedding)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_executable(registry.identities().front())
        .allow_effect_mode(EffectMode::TrustedNative);
    EXPECT_THROW((void)std::move(omitted_effect).build(), std::invalid_argument);
}

TEST(AdmissionProfileTest, CanonicalJsonIsNotAdmissibleForNewPublication) {
    AdmissionProfileBuilder builder;
    builder.id("profile").semantic_version("1.0.0").registry(registry_snapshot());
    EXPECT_THROW(builder.allow_source_kind(SourceKind::CanonicalJson), std::invalid_argument);
}

TEST(PolicySnapshotTest, CanonicalFingerprintIsOrderIndependentAndRoundTripsStrictly) {
    const auto admission = admission_profile(registry_snapshot());
    const auto first     = policy_snapshot(admission, false);
    const auto second    = policy_snapshot(admission, true);

    EXPECT_EQ(first.fingerprint(), second.fingerprint());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    const auto parsed = PolicySnapshot::parse(first.serialize_canonical());
    EXPECT_EQ(parsed.fingerprint(), first.fingerprint());
    EXPECT_EQ(parsed.budget_ceiling(), finite_budget());
    auto reordered                    = first.manifest();
    reordered["allowed_capabilities"] = json::array({"zeta", "alpha"});
    reordered["allowed_effects"]      = json::array({"write", "read"});
    const auto normalized             = PolicySnapshot::parse(reordered.dump());
    EXPECT_EQ(normalized.fingerprint(), first.fingerprint());
    EXPECT_EQ(normalized.serialize_canonical(), first.serialize_canonical());

    auto nested_unknown                        = first.manifest();
    nested_unknown["budget_ceiling"]["future"] = 1;
    EXPECT_THROW((void)PolicySnapshot::parse(nested_unknown.dump()), std::invalid_argument);
    auto tampered           = first.manifest();
    tampered["owner_scope"] = "tenant:other";
    EXPECT_THROW((void)PolicySnapshot::parse(tampered.dump()), std::invalid_argument);
    auto negative_budget                              = first.manifest();
    negative_budget["budget_ceiling"]["wall_time_ms"] = -1;
    EXPECT_THROW((void)PolicySnapshot::parse(negative_budget.dump()), std::invalid_argument);
    auto overflowing_budget                                 = first.manifest();
    overflowing_budget["budget_ceiling"]["max_concurrency"] = 4294967296ULL;
    EXPECT_THROW((void)PolicySnapshot::parse(overflowing_budget.dump()), std::invalid_argument);
    auto overflowing_dynamic                                      = first.manifest();
    overflowing_dynamic["budget_ceiling"]["max_dynamic_compiles"] = 4294967296ULL;
    EXPECT_THROW((void)PolicySnapshot::parse(overflowing_dynamic.dump()), std::invalid_argument);
}

TEST(PolicySnapshotTest, AllowsLeafOnlyChildPolicyAndRejectsDepthAboveHardCeiling) {
    const auto registry  = registry_snapshot();
    const auto admission = admission_profile(registry);

    auto leaf_budget             = finite_budget();
    leaf_budget.max_child_depth  = 0;
    leaf_budget.max_total_children = 0;
    PolicySnapshotBuilder leaf;
    leaf.id("leaf-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:one")
        .admission_profile(admission)
        .budget_ceiling(leaf_budget);
    EXPECT_NO_THROW((void)std::move(leaf).build());

    auto oversized_budget              = finite_budget();
    oversized_budget.max_child_depth   = MAX_SUPPORTED_CHILD_DEPTH + 1;
    oversized_budget.max_total_children = 2;
    PolicySnapshotBuilder oversized;
    oversized.id("oversized-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:one")
        .admission_profile(admission)
        .budget_ceiling(oversized_budget);
    EXPECT_THROW((void)std::move(oversized).build(), std::invalid_argument);
}

TEST(PolicySnapshotTest, ConfiguresChildDepthCeilingAndRejectsInvalidValues) {
    const auto registry = registry_snapshot();

    AdmissionProfileBuilder zero_profile;
    zero_profile.id("zero-depth-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) zero_profile.allow_executable(identity);
    EXPECT_THROW(zero_profile.max_supported_child_depth(0), std::invalid_argument);

    AdmissionProfileBuilder overflow_profile;
    overflow_profile.id("overflow-depth-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) overflow_profile.allow_executable(identity);
    EXPECT_THROW(
        overflow_profile.max_supported_child_depth(
            static_cast<std::uint64_t>(MAX_REPRESENTABLE_CHILD_DEPTH) + 1u),
        std::invalid_argument);

    const auto default_admission = admission_profile(registry);
    AdmissionProfileBuilder explicit_default_builder;
    explicit_default_builder.id("profile")
        .semantic_version("1.2.3")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_executable(registry.identities().front())
        .allow_effect_mode(EffectMode::Brokered)
        .max_supported_child_depth(MAX_SUPPORTED_CHILD_DEPTH);
    const auto explicit_default_admission = std::move(explicit_default_builder).build();
    EXPECT_EQ(explicit_default_admission.max_supported_child_depth(), MAX_SUPPORTED_CHILD_DEPTH);
    EXPECT_EQ(explicit_default_admission.fingerprint(), default_admission.fingerprint());

    auto default_json = json::parse(default_admission.serialize_canonical());
    default_json["max_supported_child_depth"] = MAX_SUPPORTED_CHILD_DEPTH;
    const auto parsed_explicit_default = AdmissionProfile::parse(default_json.dump());
    EXPECT_EQ(parsed_explicit_default.max_supported_child_depth(), MAX_SUPPORTED_CHILD_DEPTH);
    EXPECT_EQ(parsed_explicit_default.fingerprint(), default_admission.fingerprint());

    default_json["max_supported_child_depth"] = 0;
    EXPECT_THROW((void)AdmissionProfile::parse(default_json.dump()), std::invalid_argument);
    default_json["max_supported_child_depth"] =
        static_cast<std::uint64_t>(MAX_REPRESENTABLE_CHILD_DEPTH) + 1u;
    EXPECT_THROW((void)AdmissionProfile::parse(default_json.dump()), std::invalid_argument);

    AdmissionProfileBuilder deep_profile_builder;
    deep_profile_builder.id("deep-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .allow_effect_mode(EffectMode::Brokered)
        .max_supported_child_depth(12);
    for (const auto& identity : registry.identities())
        deep_profile_builder.allow_executable(identity);
    const auto deep_admission = std::move(deep_profile_builder).build();
    EXPECT_EQ(deep_admission.max_supported_child_depth(), 12u);
    const auto roundtrip_deep_admission =
        AdmissionProfile::parse(deep_admission.serialize_canonical());
    EXPECT_EQ(roundtrip_deep_admission.max_supported_child_depth(), 12u);
    EXPECT_EQ(roundtrip_deep_admission.fingerprint(), deep_admission.fingerprint());

    auto depth_12_budget               = finite_budget();
    depth_12_budget.max_child_depth    = 12;
    depth_12_budget.max_total_children = 12;
    PolicySnapshotBuilder deep_policy_builder;
    deep_policy_builder.id("deep-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:one")
        .admission_profile(deep_admission)
        .budget_ceiling(depth_12_budget);
    const auto deep_policy = std::move(deep_policy_builder).build();
    EXPECT_EQ(deep_policy.max_supported_child_depth(), 12u);
    EXPECT_EQ(deep_policy.budget_ceiling().max_child_depth, 12u);
    const auto roundtrip_deep_policy =
        PolicySnapshot::parse(deep_policy.serialize_canonical());
    EXPECT_EQ(roundtrip_deep_policy.max_supported_child_depth(), 12u);
    EXPECT_EQ(roundtrip_deep_policy.fingerprint(), deep_policy.fingerprint());

    PolicySnapshotBuilder default_policy_builder;
    default_policy_builder.id("default-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:one")
        .admission_profile(default_admission)
        .budget_ceiling(finite_budget());
    const auto default_policy = std::move(default_policy_builder).build();
    const auto roundtrip_default_policy =
        PolicySnapshot::parse(default_policy.serialize_canonical());
    EXPECT_EQ(roundtrip_default_policy.max_supported_child_depth(), MAX_SUPPORTED_CHILD_DEPTH);
    EXPECT_EQ(roundtrip_default_policy.fingerprint(), default_policy.fingerprint());

    auto depth_13_budget               = depth_12_budget;
    depth_13_budget.max_child_depth    = 13;
    depth_13_budget.max_total_children = 13;
    PolicySnapshotBuilder exceeded_policy;
    exceeded_policy.id("exceeded-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:one")
        .admission_profile(deep_admission)
        .budget_ceiling(depth_13_budget);
    EXPECT_THROW((void)std::move(exceeded_policy).build(), std::invalid_argument);
}

TEST(PolicySnapshotTest, EnforcesTrustedNativeCapabilityAndFiniteBudgets) {
    const auto trusted = admission_profile(registry_snapshot(), AdmissionMode::TrustedEmbedding);
    PolicySnapshotBuilder missing_capability;
    missing_capability.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(trusted)
        .budget_ceiling(finite_budget());
    EXPECT_THROW((void)std::move(missing_capability).build(), std::invalid_argument);

    PolicySnapshotBuilder allowed;
    allowed.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(trusted)
        .allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY))
        .budget_ceiling(finite_budget());
    EXPECT_NO_THROW((void)std::move(allowed).build());

    const auto            ordinary = admission_profile(registry_snapshot());
    PolicySnapshotBuilder overprivileged;
    overprivileged.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(ordinary)
        .allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY))
        .budget_ceiling(finite_budget());
    EXPECT_THROW((void)std::move(overprivileged).build(), std::invalid_argument);

    AdmissionProfileBuilder restricted_profile_builder;
    restricted_profile_builder.id("restricted")
        .semantic_version("1.0.0")
        .registry(registry_snapshot())
        .mode(AdmissionMode::TrustedEmbedding)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    const auto            restricted_profile = std::move(restricted_profile_builder).build();
    PolicySnapshotBuilder restricted_overgrant;
    restricted_overgrant.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(restricted_profile)
        .allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY))
        .budget_ceiling(finite_budget());
    EXPECT_THROW((void)std::move(restricted_overgrant).build(), std::invalid_argument);

    PolicySnapshotBuilder unbounded;
    unbounded.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(ordinary)
        .budget_ceiling(BudgetLimits{});
    EXPECT_THROW((void)std::move(unbounded).build(), std::invalid_argument);
}

TEST(PolicySnapshotTest, ZeroBudgetCeilingErrorNamesEachZeroField) {
    const auto admission = admission_profile(registry_snapshot());
    auto       ceiling   = finite_budget();
    ceiling.model_tokens         = 0;
    ceiling.max_dynamic_compiles = 0;
    PolicySnapshotBuilder builder;
    builder.id("policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant")
        .admission_profile(admission)
        .budget_ceiling(ceiling);
    try {
        (void)std::move(builder).build();
        FAIL() << "a zero budget ceiling field must be rejected";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(),
                     "Every policy budget ceiling except child budgets must be positive; "
                     "zero: model_tokens, max_dynamic_compiles");
    }

    auto parsed = json::parse(policy_snapshot(admission).serialize_canonical());
    parsed["budget_ceiling"]["monetary_microunits"] = 0;
    try {
        (void)PolicySnapshot::parse(parsed.dump());
        FAIL() << "a zero budget ceiling field must be rejected on parse";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("zero: monetary_microunits"),
                  std::string::npos)
            << error.what();
    }
}

TEST(ProgramVersionAdmissionTest, RejectsProfilePolicyOwnerAndRegistryMismatches) {
    const auto registry_a = registry_snapshot("a");
    const auto registry_b = registry_snapshot("b");
    const auto profile_a  = admission_profile(registry_a);
    const auto profile_b  = admission_profile(registry_b);
    const auto policy_a   = policy_snapshot(profile_a);
    const auto policy_b   = policy_snapshot(profile_b);

    EXPECT_THROW((void)ProgramVersion(version_data(profile_a, policy_b)), std::invalid_argument);

    auto wrong_owner            = version_data(profile_a, policy_a);
    wrong_owner.ownership_scope = "tenant:other";
    EXPECT_THROW((void)ProgramVersion(std::move(wrong_owner)), std::invalid_argument);

    auto wrong_registry = version_data(profile_a, policy_a);
    wrong_registry.core_materialization_receipt.registry_snapshot_fingerprint = digest('f');
    EXPECT_THROW((void)ProgramVersion(std::move(wrong_registry)), std::invalid_argument);
}

TEST(ProgramVersionAdmissionTest, StoresTypedSnapshotsAndRejectsNestedTampering) {
    const auto     profile = admission_profile(registry_snapshot());
    const auto     policy  = policy_snapshot(profile);
    ProgramVersion version(version_data(profile, policy));

    const auto parsed = ProgramVersion::parse(version.serialize_canonical());
    EXPECT_EQ(parsed.admission_profile().fingerprint(), profile.fingerprint());
    EXPECT_EQ(parsed.policy_snapshot().fingerprint(), policy.fingerprint());

    auto tampered                       = json::parse(version.serialize_canonical());
    tampered["admission_profile"]["id"] = "other";
    EXPECT_THROW((void)ProgramVersion::parse(tampered.dump()), std::invalid_argument);
    auto negative_storage                      = json::parse(version.serialize_canonical());
    negative_storage["storage_schema_version"] = -1;
    EXPECT_THROW((void)ProgramVersion::parse(negative_storage.dump()), std::invalid_argument);
}
