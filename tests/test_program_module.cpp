#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::program;

std::string digest(char value) { return "sha256:" + std::string(64, value); }

RegistrySnapshot make_registry() {
    RegistrySnapshotBuilder builder;
    builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "module-reducer", "1.0.0", digest('1')},
                           EffectMode::Brokered,
                           "attestation:module",
                           {},
                           {}},
        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

AdmissionProfile make_admission(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("module-admission")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    return std::move(builder).build();
}

PolicySnapshot make_policy(const AdmissionProfile& admission, std::string owner) {
    PolicySnapshotBuilder builder;
    builder.id("module-policy")
        .semantic_version("1.0.0")
        .owner_scope(std::move(owner))
        .admission_profile(admission)
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    return std::move(builder).build();
}
ProgramBundle make_bundle(std::string    module_root,
                          std::string    registry_fingerprint,
                          ContractRecord input,
                          ContractRecord output) {
    const json definition = json{
        {"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
        {"nodes", json{{"main", json{{"type", "module-node"}}}}}};
    ProgramBundleData data;
    data.source_kind                   = SourceKind::CanonicalJson;
    data.source_hash                   = digest('2');
    data.canonical_program_hash        = digest('3');
    data.compiler_build_id             = "compiler:module";
    data.program_schema_version        = 1;
    data.registry_snapshot_fingerprint = std::move(registry_fingerprint);
    data.module_dependency_merkle_root = std::move(module_root);
    data.input_contract                = std::move(input);
    data.output_contract               = std::move(output);
    data.orchestration_plan            = OrchestrationPlanRecord{1, json{{"entry", "main"}}};
    data.sealed_core_definitions       = {
        SealedCoreDefinition{"main", sealed_core_definition_hash(definition), definition}};
    data.core_plan_identities = {CorePlanIdentity{"main", digest('c')}};
    data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Node, "module-node", "1.0.0", digest('5')}};
    data.capability_effect_closure = CapabilityEffectClosure{{"read"}, {"tool"}};
    data.declared_budget_requirements = {
        BudgetRequirement{"wall_time_ms", 1, 10},
        BudgetRequirement{"model_tokens", 1, 10},
        BudgetRequirement{"monetary_microunits", 1, 10},
        BudgetRequirement{"max_concurrency", 1, 1},
        BudgetRequirement{"max_program_operations", 1, 10},
        BudgetRequirement{"max_core_steps", 1, 10},
        BudgetRequirement{"max_dynamic_compiles", 1, 1},
        BudgetRequirement{"max_child_depth", 1, 1},
        BudgetRequirement{"max_total_children", 1, 1},
    };
    return ProgramBundle(std::move(data));
}

struct LinkFixture {
    ProgramModule    parent;
    ModuleResolution resolution;
    ProgramBundle    bundle;
    ProgramVersion   version;
};

LinkFixture make_fixture(std::string child_owner = "tenant:module") {
    const auto module_root = ModuleResolution{}.dependency_merkle_root();
    const ContractRecord contract{1, json{{"type", "object"}}};
    const auto registry  = make_registry();
    auto       bundle    = make_bundle(module_root, registry.fingerprint(), contract, contract);
    const auto admission = make_admission(registry);
    const auto policy    = make_policy(admission, child_owner);
    ProgramVersion version(
        ProgramVersionData{bundle.id(),
                           admission,
                           policy,
                           {},
                           child_owner,
                           CoreMaterializationReceipt{"compiler:module", registry.fingerprint(),
                                                      {CorePlanIdentity{"main", digest('c')}}}});

    ProgramModuleData parent_data;
    parent_data.owner_scope       = "tenant:module";
    parent_data.coordinate        = ModuleCoordinate{"test", "parent", "1.0.0", ""};
    parent_data.attestation_id    = "attestation:module";
    parent_data.allowed_capabilities = {"read"};
    parent_data.declared_effects     = {"tool"};
    parent_data.children.push_back(ChildProgramDescriptor{
        "child", version.id(), {ModulePort{"input", contract}}, {ModulePort{"output", contract}},
        {"read"}, {"tool"}, BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1}});
    auto parent = ProgramModule::create(std::move(parent_data));

    ModuleResolution resolution;
    resolution.root = parent.coordinate();
    resolution.modules.push_back(parent);
    return LinkFixture{std::move(parent), std::move(resolution), std::move(bundle),
                       std::move(version)};
}

TEST(ProgramModuleTest, LinksExactChildAndRoundTripsImmutableReceipt) {
    auto fixture = make_fixture();
    const auto receipt = link_module_child(fixture.resolution, fixture.parent, "child",
                                           fixture.bundle, fixture.version);

    EXPECT_EQ(receipt.owner_scope(), "tenant:module");
    EXPECT_EQ(receipt.parent_module_id(), fixture.parent.id());
    EXPECT_EQ(receipt.child_program_version_id(), fixture.version.id());
    EXPECT_EQ(receipt.child_bundle_id(), fixture.bundle.id());
    EXPECT_EQ(receipt.granted_capabilities(), std::vector<std::string>({"read"}));
    EXPECT_EQ(receipt.granted_effects(), std::vector<std::string>({"tool"}));
    EXPECT_EQ(receipt.budget(), (BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1}));

    const auto parsed = ModuleLinkReceipt::parse(receipt.serialize_canonical());
    EXPECT_EQ(parsed.id(), receipt.id());
    EXPECT_EQ(parsed.serialize_canonical(), receipt.serialize_canonical());
}

TEST(ProgramModuleTest, RejectsChildOwnerOutsideParentScope) {
    auto fixture = make_fixture("tenant:other");
    EXPECT_THROW(link_module_child(fixture.resolution, fixture.parent, "child", fixture.bundle,
                                   fixture.version), std::invalid_argument);
}

TEST(ProgramModuleTest, RejectsDescriptorVersionSubstitution) {
    auto fixture = make_fixture();
    auto forged = fixture.version;
    ProgramModuleData parent_data;
    parent_data.owner_scope          = fixture.parent.owner_scope();
    parent_data.coordinate           = fixture.parent.coordinate();
    parent_data.attestation_id       = fixture.parent.attestation_id();
    parent_data.allowed_capabilities = fixture.parent.allowed_capabilities();
    parent_data.declared_effects     = fixture.parent.declared_effects();
    parent_data.children             = fixture.parent.children();
    parent_data.children.front().program_version_id = digest('4');
    parent_data.coordinate.content_identity.clear();
    const auto substituted_parent = ProgramModule::create(std::move(parent_data));
    auto resolution               = fixture.resolution;
    resolution.root               = substituted_parent.coordinate();
    resolution.modules.front()    = substituted_parent;

    EXPECT_THROW(link_module_child(resolution, substituted_parent, "child", fixture.bundle, forged),
                 std::invalid_argument);
}

}  // namespace
