#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

AdmissionProfile make_admission(const RegistrySnapshot& registry,
                                ExecutionGuarantee      minimum_guarantee =
                                    ExecutionGuarantee::Strict) {
    AdmissionProfileBuilder builder;
    builder.id("module-admission")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .minimum_execution_guarantee(minimum_guarantee)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    return std::move(builder).build();
}

PolicySnapshot make_policy(const AdmissionProfile& admission, std::string owner,
                           ExecutionGuarantee minimum_guarantee =
                               ExecutionGuarantee::Strict) {
    PolicySnapshotBuilder builder;
    builder.id("module-policy")
        .semantic_version("1.0.0")
        .owner_scope(std::move(owner))
        .admission_profile(admission)
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1})
        .minimum_execution_guarantee(minimum_guarantee);
    return std::move(builder).build();
}
ProgramBundle make_bundle(std::string    module_root,
                          std::string    registry_fingerprint,
                          ContractRecord input,
                          ContractRecord output,
                          std::string    child_binding = "child",
                          ExecutionGuarantee guarantee = ExecutionGuarantee::Strict) {
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
    data.orchestration_plan = OrchestrationPlanRecord{
        1,
        json{{"root", "root"},
             {"operations", json::array({
                                  json{{"id", "root"}, {"op", "await"}, {"body", "spawn"}},
                                  json{{"id", "spawn"},
                                       {"op", "spawn"},
                                       {"child_binding", std::move(child_binding)}}})}}};
    data.sealed_core_definitions       = {
        SealedCoreDefinition{"main", sealed_core_definition_hash(definition), definition}};
    data.core_plan_identities = {CorePlanIdentity{"main", digest('c')}};
    data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Node, "module-node", "1.0.0", digest('5')}};
    data.capability_effect_closure = CapabilityEffectClosure{{"read"}, {"tool"}};
    data.execution_guarantee = guarantee;
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

ProgramBundle make_composition_bundle(std::string         registry_fingerprint,
                                      ContractRecord      input,
                                      ContractRecord      output,
                                      const BudgetLimits& allocation) {
    const json definition = json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                                 {"nodes", json{{"main", json{{"type", "module-node"}}}}}};
    ProgramBundleData data;
    data.source_kind                   = SourceKind::CanonicalJson;
    data.source_hash                   = digest('8');
    data.canonical_program_hash        = digest('9');
    data.compiler_build_id             = "compiler:module-overflow";
    data.program_schema_version        = 1;
    data.registry_snapshot_fingerprint = std::move(registry_fingerprint);
    data.module_dependency_merkle_root = ModuleResolution{}.dependency_merkle_root();
    data.input_contract                = std::move(input);
    data.output_contract               = std::move(output);
    data.orchestration_plan            = OrchestrationPlanRecord{
        1, json{{"root", "root"},
                           {"operations",
                            json::array({
                     json{{"id", "root"},
                                     {"op", "sequence"},
                                     {"children", json::array({"spawn-a", "spawn-b"})}},
                     json{{"id", "spawn-a"}, {"op", "spawn"}, {"child_binding", "child-a"}},
                     json{{"id", "spawn-b"}, {"op", "spawn"}, {"child_binding", "child-b"}},
                 })}}};
    data.sealed_core_definitions = {
        SealedCoreDefinition{"main", sealed_core_definition_hash(definition), definition}};
    data.core_plan_identities           = {CorePlanIdentity{"main", digest('c')}};
    data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Node, "module-node", "1.0.0", digest('5')}};
    data.capability_effect_closure    = CapabilityEffectClosure{{"read"}, {"tool"}};
    data.execution_guarantee          = ExecutionGuarantee::Strict;
    data.declared_budget_requirements = {
        BudgetRequirement{"wall_time_ms", 1, allocation.wall_time_ms},
        BudgetRequirement{"model_tokens", 1, allocation.model_tokens},
        BudgetRequirement{"monetary_microunits", 1, allocation.monetary_microunits},
        BudgetRequirement{"max_concurrency", 1, allocation.max_concurrency},
        BudgetRequirement{"max_program_operations", 1, allocation.max_program_operations},
        BudgetRequirement{"max_core_steps", 1, allocation.max_core_steps},
        BudgetRequirement{"max_dynamic_compiles", 1, allocation.max_dynamic_compiles},
        BudgetRequirement{"max_child_depth", 1, allocation.max_child_depth},
        BudgetRequirement{"max_total_children", 1, allocation.max_total_children},
    };
    return ProgramBundle(std::move(data));
}

struct LinkFixture {
    ProgramModule    parent;
    ModuleResolution resolution;
    ProgramBundle    bundle;
    ProgramVersion   version;
};

LinkFixture make_fixture(std::string child_owner = "tenant:module",
                         std::string child_binding = "child",
                         ExecutionGuarantee child_guarantee = ExecutionGuarantee::Strict,
                         ExecutionGuarantee accepted_guarantee = ExecutionGuarantee::Strict) {
    const auto module_root = ModuleResolution{}.dependency_merkle_root();
    const ContractRecord contract{1, json{{"type", "object"}}};
    const auto registry  = make_registry();
    auto       bundle    = make_bundle(module_root, registry.fingerprint(), contract, contract,
                                    std::move(child_binding), child_guarantee);
    const auto admission = make_admission(registry, child_guarantee);
    const auto policy    = make_policy(admission, child_owner, child_guarantee);
    ProgramVersion version(
        ProgramVersionData{bundle.id(),
                           admission,
                           policy,
                           {},
                           child_owner,
                           CoreMaterializationReceipt{"compiler:module", registry.fingerprint(),
                                                      {CorePlanIdentity{"main", digest('c')}}},
                           child_guarantee});

    ProgramModuleData parent_data;
    parent_data.owner_scope       = "tenant:module";
    parent_data.coordinate        = ModuleCoordinate{"test", "parent", "1.0.0", ""};
    parent_data.attestation_id    = "attestation:module";
    parent_data.allowed_capabilities = {"read"};
    parent_data.declared_effects     = {"tool"};
    parent_data.children.push_back(ChildProgramDescriptor{
        "child", version.id(), {ModulePort{"input", contract}}, {ModulePort{"output", contract}},
        {"read"}, {"tool"}, BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1},
        accepted_guarantee});
    auto parent = ProgramModule::create(std::move(parent_data));

    ModuleResolution resolution;
    resolution.root = parent.coordinate();
    resolution.modules.push_back(parent);
    return LinkFixture{std::move(parent), std::move(resolution), std::move(bundle),
                       std::move(version)};
}

struct OverflowCompositionFixture {
    ProgramBundle      parent_bundle;
    ProgramComposition composition;
};

OverflowCompositionFixture make_overflow_composition(BudgetLimits first_budget,
                                                     BudgetLimits second_budget,
                                                     BudgetLimits parent_allocation) {
    const auto           module_root = ModuleResolution{}.dependency_merkle_root();
    const ContractRecord contract{1, json{{"type", "object"}}};
    const auto           registry  = make_registry();
    const auto           admission = make_admission(registry);
    auto                 first_bundle =
        make_bundle(module_root, registry.fingerprint(), contract, contract, "unused-a");
    auto second_bundle =
        make_bundle(module_root, registry.fingerprint(), contract, contract, "unused-b");

    const auto make_child_policy = [&](std::string id, const BudgetLimits& ceiling) {
        PolicySnapshotBuilder builder;
        builder.id(std::move(id))
            .semantic_version("1.0.0")
            .owner_scope("tenant:module")
            .admission_profile(admission)
            .allow_capability("read")
            .allow_effect("tool")
            .budget_ceiling(ceiling);
        return std::move(builder).build();
    };
    const auto     first_policy  = make_child_policy("overflow-policy-a", first_budget);
    const auto     second_policy = make_child_policy("overflow-policy-b", second_budget);
    ProgramVersion first_version(ProgramVersionData{
        first_bundle.id(),
        admission,
        first_policy,
        {},
        "tenant:module",
        CoreMaterializationReceipt{
            "compiler:module", registry.fingerprint(), {CorePlanIdentity{"main", digest('c')}}},
        ExecutionGuarantee::Strict});
    ProgramVersion second_version(ProgramVersionData{
        second_bundle.id(),
        admission,
        second_policy,
        {},
        "tenant:module",
        CoreMaterializationReceipt{
            "compiler:module", registry.fingerprint(), {CorePlanIdentity{"main", digest('c')}}},
        ExecutionGuarantee::Strict});

    auto parent_bundle =
        make_composition_bundle(registry.fingerprint(), contract, contract, parent_allocation);
    ProgramModuleData parent_data;
    parent_data.owner_scope          = "tenant:module";
    parent_data.coordinate           = ModuleCoordinate{"test", "overflow-parent", "1.0.0", ""};
    parent_data.attestation_id       = "attestation:module";
    parent_data.allowed_capabilities = {"read"};
    parent_data.declared_effects     = {"tool"};
    parent_data.children             = {
        ChildProgramDescriptor{"child-a",
                               first_version.id(),
                                           {ModulePort{"input", contract}},
                                           {ModulePort{"output", contract}},
                                           {"read"},
                                           {"tool"},
                               first_budget,
                               ExecutionGuarantee::Strict},
        ChildProgramDescriptor{"child-b",
                               second_version.id(),
                                           {ModulePort{"input", contract}},
                                           {ModulePort{"output", contract}},
                                           {"read"},
                                           {"tool"},
                               second_budget,
                               ExecutionGuarantee::Strict},
    };
    auto             parent = ProgramModule::create(std::move(parent_data));
    ModuleResolution resolution;
    resolution.root = parent.coordinate();
    resolution.modules.push_back(parent);
    ProgramComposition composition{
        std::move(parent),
        std::move(resolution),
        {ChildProgramBinding{"child-a", std::move(first_bundle), std::move(first_version)},
         ChildProgramBinding{"child-b", std::move(second_bundle), std::move(second_version)}}};
    return OverflowCompositionFixture{std::move(parent_bundle), std::move(composition)};
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

TEST(ProgramModuleTest, RequiresExplicitChildGuaranteeAcceptanceAndPersistsIt) {
    auto rejected =
        make_fixture("tenant:module", "child", ExecutionGuarantee::Recorded,
                     ExecutionGuarantee::Strict);
    EXPECT_THROW(link_module_child(rejected.resolution, rejected.parent, "child", rejected.bundle,
                                   rejected.version),
                 std::invalid_argument);

    auto accepted =
        make_fixture("tenant:module", "child", ExecutionGuarantee::Recorded,
                     ExecutionGuarantee::Recorded);
    const auto receipt = link_module_child(accepted.resolution, accepted.parent, "child",
                                           accepted.bundle, accepted.version);
    EXPECT_EQ(receipt.minimum_execution_guarantee(), ExecutionGuarantee::Recorded);
    EXPECT_EQ(ModuleLinkReceipt::parse(receipt.serialize_canonical()).minimum_execution_guarantee(),
              ExecutionGuarantee::Recorded);
}

TEST(ProgramModuleTest, RejectsChildOwnerOutsideParentScope) {
    auto fixture = make_fixture("tenant:other");
    EXPECT_THROW(link_module_child(fixture.resolution, fixture.parent, "child", fixture.bundle,
                                   fixture.version), std::invalid_argument);
}

TEST(ProgramModuleTest, ResolverRejectsCrossScopeAndUnapprovedCapabilities) {
    auto fixture = make_fixture();
    auto store   = std::make_shared<InMemoryModuleStore>();
    store->publish(fixture.parent);

    PolicySnapshotBuilder owner_policy_builder;
    owner_policy_builder.id("resolver-owner-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:other")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(fixture.parent.id())
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto owner_policy = std::move(owner_policy_builder).build();

    EXPECT_THROW(ModuleResolver(store).resolve(fixture.parent.coordinate(), owner_policy),
                 std::invalid_argument);

    PolicySnapshotBuilder capability_policy_builder;
    capability_policy_builder.id("resolver-capability-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(fixture.parent.id())
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto capability_policy = std::move(capability_policy_builder).build();

    EXPECT_THROW(ModuleResolver(store).resolve(fixture.parent.coordinate(), capability_policy),
                 std::invalid_argument);

    PolicySnapshotBuilder effect_policy_builder;
    effect_policy_builder.id("resolver-effect-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(fixture.parent.id())
        .allow_capability("read")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto effect_policy = std::move(effect_policy_builder).build();
    EXPECT_THROW(ModuleResolver(store).resolve(fixture.parent.coordinate(), effect_policy),
                 std::invalid_argument);
}

TEST(ProgramModuleTest, ResolverReturnsCompletePinnedDependencyClosure) {
    ProgramModuleData dependency_data;
    dependency_data.owner_scope    = "tenant:module";
    dependency_data.coordinate     = ModuleCoordinate{"test", "dependency", "1.0.0", ""};
    dependency_data.attestation_id = "attestation:module";
    dependency_data.allowed_capabilities = {"read"};
    dependency_data.declared_effects     = {"tool"};
    const auto dependency = ProgramModule::create(std::move(dependency_data));

    ProgramModuleData root_data;
    root_data.owner_scope    = "tenant:module";
    root_data.coordinate     = ModuleCoordinate{"test", "root", "1.0.0", ""};
    root_data.attestation_id = "attestation:module";
    root_data.dependencies   = {dependency.coordinate()};
    root_data.allowed_capabilities = {"read"};
    root_data.declared_effects     = {"tool"};
    const auto root = ProgramModule::create(std::move(root_data));

    auto store = std::make_shared<InMemoryModuleStore>();
    store->publish(dependency);
    store->publish(root);

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("resolver-closure-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(root.id())
        .allow_module_digest(dependency.id())
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto policy = std::move(policy_builder).build();

    const auto resolution = ModuleResolver(store).resolve(root.coordinate(), policy);
    EXPECT_EQ(resolution.modules.size(), 2U);
    EXPECT_EQ(resolution.receipts.size(), 2U);
    EXPECT_NO_THROW(validate_module_resolution(resolution));
    EXPECT_EQ(resolution.root, root.coordinate());
}

TEST(ProgramModuleTest, VerifiedResolverIsAnExactImmutableReceiptAllowlist) {
    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:module";
    module_data.coordinate     = ModuleCoordinate{"sealed", "allowlisted", "1.0.0", ""};
    module_data.attestation_id = "attestation:module";
    const auto module          = ProgramModule::create(std::move(module_data));

    ModuleResolution resolution;
    resolution.root = module.coordinate();
    resolution.modules.push_back(module);
    resolution.receipts.push_back({module.coordinate().qualified_name(), module.id()});

    VerifiedModuleResolver resolver(resolution);
    const auto coordinate = resolver.resolve(module.coordinate().qualified_name());
    ASSERT_TRUE(coordinate.has_value());
    EXPECT_EQ(*coordinate, module.coordinate());
    EXPECT_FALSE(resolver.resolve("sealed:allowlisted@2.0.0").has_value());
    EXPECT_FALSE(resolver.resolve("file:///etc/passwd").has_value());

    resolution.receipts.clear();
    resolution.modules.clear();
    EXPECT_TRUE(resolver.resolve(module.coordinate().qualified_name()).has_value());
    EXPECT_EQ(resolver.resolution().receipts.size(), 1U);
}

TEST(ProgramModuleTest, ResolutionRejectsUnpinnedDependency) {
    ProgramModuleData root_data;
    root_data.owner_scope    = "tenant:module";
    root_data.coordinate     = ModuleCoordinate{"test", "unresolved", "1.0.0", ""};
    root_data.attestation_id = "attestation:module";
    root_data.dependencies = {
        ModuleCoordinate{"test", "missing", "1.0.0", digest('d')}};
    const auto root = ProgramModule::create(std::move(root_data));

    ModuleResolution resolution;
    resolution.root = root.coordinate();
    resolution.modules.push_back(root);
    EXPECT_THROW(validate_module_resolution(resolution), std::invalid_argument);
}

TEST(ProgramModuleTest, OwnerQualifiedLookupHidesForeignModuleIdentity) {
    auto fixture = make_fixture();
    auto store   = std::make_shared<InMemoryModuleStore>();
    store->publish(fixture.parent);

    EXPECT_TRUE(store->get("tenant:module", fixture.parent.id()).has_value());
    EXPECT_FALSE(store->get("tenant:other", fixture.parent.id()).has_value());
}

TEST(ProgramModuleTest, ResolverRejectsQuarantinedModuleEvenWithPinnedIdentity) {
    auto fixture = make_fixture();
    auto store   = std::make_shared<InMemoryModuleStore>();
    store->publish(fixture.parent);
    store->set_lifecycle(fixture.parent.id(), ModuleLifecycle::Quarantined);

    PolicySnapshotBuilder builder;
    builder.id("resolver-lifecycle-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(fixture.parent.id())
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto policy = std::move(builder).build();

    EXPECT_THROW(ModuleResolver(store).resolve(fixture.parent.coordinate(), policy),
                 std::invalid_argument);
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

TEST(ProgramModuleTest, RejectsChildPolicyAuthorityWiderThanParent) {
    auto fixture = make_fixture();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("broader-child-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_capability("read")
        .allow_capability("write")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto broad_policy = std::move(policy_builder).build();
    const ProgramVersion broad_version(
        ProgramVersionData{fixture.bundle.id(), fixture.version.admission_profile(), broad_policy, {},
                           "tenant:module",
                           CoreMaterializationReceipt{"compiler:module",
                                                      fixture.version.admission_profile().registry_fingerprint(),
                                                      {CorePlanIdentity{"main", digest('c')}}}});

    ProgramModuleData parent_data;
    parent_data.owner_scope          = fixture.parent.owner_scope();
    parent_data.coordinate           = fixture.parent.coordinate();
    parent_data.attestation_id       = fixture.parent.attestation_id();
    parent_data.allowed_capabilities = fixture.parent.allowed_capabilities();
    parent_data.declared_effects     = fixture.parent.declared_effects();
    parent_data.children             = fixture.parent.children();
    parent_data.children.front().program_version_id = broad_version.id();
    parent_data.coordinate.content_identity.clear();
    const auto parent = ProgramModule::create(std::move(parent_data));
    ModuleResolution resolution;
    resolution.root = parent.coordinate();
    resolution.modules.push_back(parent);

    EXPECT_THROW(link_module_child(resolution, parent, "child", fixture.bundle, broad_version),
                 std::invalid_argument);
}

TEST(ProgramModuleTest, WholeCompositionValidatesBeforeChildDispatch) {
    auto fixture = make_fixture();
    const ProgramComposition composition{
        fixture.parent,
        fixture.resolution,
        {ChildProgramBinding{"child", fixture.bundle, fixture.version}}};

    EXPECT_NO_THROW(validate_program_composition(fixture.bundle, composition));
}

TEST(ProgramModuleTest, WholeCompositionRejectsUint64ChildBudgetAggregationOverflow) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    auto fixture = make_overflow_composition(BudgetLimits{maximum, 10, 10, 1, 10, 10, 1, 1, 1},
                                             BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1},
                                             BudgetLimits{maximum, 20, 20, 2, 20, 20, 2, 1, 2});

    try {
        validate_program_composition(fixture.parent_bundle, fixture.composition);
        FAIL() << "expected uint64 child-budget aggregation overflow";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "Child budget aggregation overflows wall_time_ms");
    }
}

TEST(ProgramModuleTest, WholeCompositionRejectsUint32ChildBudgetAggregationOverflow) {
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    auto fixture = make_overflow_composition(BudgetLimits{10, 10, 10, maximum, 10, 10, 1, 1, 1},
                                             BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1},
                                             BudgetLimits{20, 20, 20, maximum, 20, 20, 2, 1, 2});

    try {
        validate_program_composition(fixture.parent_bundle, fixture.composition);
        FAIL() << "expected uint32 child-budget aggregation overflow";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "Child budget aggregation overflows max_concurrency");
    }
}

TEST(ProgramModuleTest, WholeCompositionAggregatesChildDepthByMaximum) {
    auto fixture = make_overflow_composition(
        BudgetLimits{10, 10, 10, 1, 10, 10, 1, MAX_SUPPORTED_CHILD_DEPTH, 1},
        BudgetLimits{10, 10, 10, 1, 10, 10, 1, MAX_SUPPORTED_CHILD_DEPTH, 1},
        BudgetLimits{20, 20, 20, 2, 20, 20, 2, MAX_SUPPORTED_CHILD_DEPTH, 2});

    EXPECT_NO_THROW(validate_program_composition(fixture.parent_bundle, fixture.composition));
}

TEST(ProgramModuleTest, WholeCompositionRejectsUndeclaredSpawnBinding) {
    auto fixture = make_fixture("tenant:module", "missing-child");
    const ProgramComposition composition{
        fixture.parent,
        fixture.resolution,
        {ChildProgramBinding{"child", fixture.bundle, fixture.version}}};

    EXPECT_THROW(validate_program_composition(fixture.bundle, composition), std::invalid_argument);
}

TEST(ProgramModuleTest, WholeCompositionDeniesChildIdentityCollision) {
    auto fixture = make_fixture();
    const ProgramComposition composition{
        fixture.parent,
        fixture.resolution,
        {ChildProgramBinding{"child", fixture.bundle, fixture.version},
         ChildProgramBinding{"child", fixture.bundle, fixture.version}}};

    EXPECT_THROW(validate_program_composition(fixture.bundle, composition), std::invalid_argument);
}

TEST(ProgramModuleTest, ResolverRejectsDeprecatedPinnedModule) {
    auto fixture = make_fixture();
    auto store   = std::make_shared<InMemoryModuleStore>();
    store->publish(fixture.parent);
    store->set_lifecycle(fixture.parent.id(), ModuleLifecycle::Deprecated);

    PolicySnapshotBuilder builder;
    builder.id("resolver-deprecation-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:module")
        .admission_profile(make_admission(make_registry()))
        .allow_module_digest(fixture.parent.id())
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    EXPECT_THROW(ModuleResolver(store).resolve(fixture.parent.coordinate(),
                                                std::move(builder).build()),
                 std::invalid_argument);
}

}  // namespace
