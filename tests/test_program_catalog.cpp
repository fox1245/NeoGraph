#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "catalog_access.h"
#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

std::atomic<unsigned> factory_calls{0};
std::atomic<unsigned> node_calls{0};

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:catalog",
                              {},
                              {},
                              {}};
}

class CatalogNode final : public GraphNode {
public:
    explicit CatalogNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++node_calls;
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot registry(bool                     failing_factory = false,
                          std::vector<std::string> capabilities    = {},
                          std::vector<std::string> effects         = {}) {
    RegistrySnapshotBuilder builder;
    auto                    node = manifest(ExecutableKind::Node, "catalog-node", '1');
    node.required_capabilities   = std::move(capabilities);
    node.declared_effects        = std::move(effects);
    builder.add_node(
        std::move(node),
        [failing_factory](const std::string& name, const json&, const NodeContext&) {
            ++factory_calls;
            if (failing_factory) throw std::runtime_error("factory rejected construction");
            return std::make_unique<CatalogNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "catalog-overwrite", '2'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

json requirements() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 1000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 100}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 100}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

json document() {
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "catalog-overwrite"}, {"initial", 0}}}}},
        {"nodes", json{{"work", json{{"type", "catalog-node"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                               json{{"from", "work"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", requirements()}};
}

AdmissionProfile profile(const RegistrySnapshot& snapshot,
                         bool                    allow_source = true,
                         bool                    allow_node   = true,
                         bool                    allow_mode   = true) {
    AdmissionProfileBuilder builder;
    builder.id("catalog-profile")
        .semantic_version("1.0.0")
        .registry(snapshot)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1);
    if (allow_source) builder.allow_source_kind(SourceKind::CppBuilder);
    if (allow_mode) builder.allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : snapshot.identities()) {
        if (allow_node || identity.kind != ExecutableKind::Node) {
            builder.allow_executable(identity);
        }
    }
    return std::move(builder).build();
}

PolicySnapshot policy(const AdmissionProfile& admission,
                      BudgetLimits            ceiling = {1000, 100, 100, 1, 1, 20, 1, 1, 1},
                      bool                    allow_capability = false,
                      bool                    allow_effect     = false) {
    PolicySnapshotBuilder builder;
    builder.id("catalog-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(admission)
        .budget_ceiling(ceiling);
    if (allow_capability) builder.allow_capability("catalog-capability");
    if (allow_effect) builder.allow_effect("catalog-effect");
    return std::move(builder).build();
}

ProgramBundle compile(const RegistrySnapshot& snapshot) {
    ProgramCompiler compiler(snapshot, {"catalog-test/v1"});
    return compiler.compile(ProgramSource::from_cpp_builder("test:catalog", 1, document()));
}

ProgramBundleData copy_data(const ProgramBundle& bundle) {
    ProgramBundleData data;
    data.source_kind                    = bundle.source_kind();
    data.source_hash                    = bundle.source_hash();
    data.canonical_program_hash         = bundle.canonical_program_hash();
    data.compiler_build_id              = bundle.compiler_build_id();
    data.program_schema_version         = bundle.program_schema_version();
    data.registry_snapshot_fingerprint  = bundle.registry_snapshot_fingerprint();
    data.module_dependency_merkle_root  = bundle.module_dependency_merkle_root();
    data.input_contract                 = bundle.input_contract();
    data.output_contract                = bundle.output_contract();
    data.orchestration_plan             = bundle.orchestration_plan();
    data.sealed_core_definitions        = bundle.sealed_core_definitions();
    data.core_plan_identities           = bundle.core_plan_identities();
    data.capability_effect_closure      = bundle.capability_effect_closure();
    data.executable_registry_identities = bundle.executable_registry_identities();
    data.declared_budget_requirements   = bundle.declared_budget_requirements();
    data.source_map                     = bundle.source_map();
    data.diagnostics                    = bundle.diagnostics();
    return data;
}

bool has_code(const ProgramAdmissionError& error, const std::string& code) {
    return std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
                       [&](const Diagnostic& diagnostic) { return diagnostic.code == code; });
}

struct CatalogFixture {
    RegistrySnapshot                       snapshot;
    AdmissionProfile                       admission;
    PolicySnapshot                         authorization;
    std::shared_ptr<InMemoryProgramStore>  store   = std::make_shared<InMemoryProgramStore>();
    std::shared_ptr<EngineGenerationCache> engines = std::make_shared<EngineGenerationCache>();
    ProgramCatalog                         catalog;

    explicit CatalogFixture(RegistrySnapshot value)
        : snapshot(std::move(value)),
          admission(profile(snapshot)),
          authorization(policy(admission)),
          catalog(CatalogConfig{store, snapshot, engines, "catalog-test/v1"}) {}

    ProgramAdmission request() const {
        return ProgramAdmission{"tenant:catalog", admission, authorization, {}};
    }
};

}  // namespace

TEST(ProgramCatalogTest, AdmitsRecomputedBundleAndPublishesAtomicallyWithoutRunning) {
    factory_calls.store(0);
    node_calls.store(0);
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);

    const auto version = fixture.catalog.admit(bundle, fixture.request());

    EXPECT_EQ(factory_calls.load(), 1U);
    EXPECT_EQ(node_calls.load(), 0U);
    ASSERT_TRUE(fixture.store->get_bundle(bundle.id()).has_value());
    ASSERT_TRUE(fixture.store->get_version(version.id()).has_value());
    ASSERT_TRUE(fixture.catalog.find_version(version.id()).has_value());
    EXPECT_EQ(version.bundle_id(), bundle.id());
    EXPECT_EQ(version.core_materialization_receipt().plans, bundle.core_plan_identities());
}

TEST(ProgramCatalogTest, RejectsEveryCallerAssertedSemanticTamperBeforeFactory) {
    using Mutation = std::function<void(ProgramBundleData&)>;
    const std::vector<Mutation> mutations{
        [](ProgramBundleData& data) { data.canonical_program_hash = digest('a'); },
        [](ProgramBundleData& data) { data.compiler_build_id = "different-compiler"; },
        [](ProgramBundleData& data) { data.registry_snapshot_fingerprint = digest('b'); },
        [](ProgramBundleData& data) { data.orchestration_plan.plan["root"] = "other"; },
        [](ProgramBundleData& data) {
            data.sealed_core_definitions.front().definition["name"] = "other";
            data.sealed_core_definitions.front().definition_hash =
                sealed_core_definition_hash(data.sealed_core_definitions.front().definition);
        },
        [](ProgramBundleData& data) {
            data.core_plan_identities.front().compiled_plan_identity = digest('c');
        },
        [](ProgramBundleData& data) {
            data.executable_registry_identities.front().implementation_digest = digest('d');
        },
        [](ProgramBundleData& data) {
            data.capability_effect_closure.capabilities.push_back("invented-capability");
        },
        [](ProgramBundleData& data) {
            for (auto& budget : data.declared_budget_requirements) {
                if (budget.resource == "max_core_steps") budget.maximum = 19;
            }
        },
    };

    for (const auto& mutate : mutations) {
        factory_calls.store(0);
        CatalogFixture fixture(registry());
        auto           data = copy_data(compile(fixture.snapshot));
        mutate(data);
        bool rejected = false;
        try {
            ProgramBundle tampered(std::move(data));
            (void)fixture.catalog.admit(tampered, fixture.request());
        } catch (const std::exception&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(factory_calls.load(), 0U);
    }
}

TEST(ProgramCatalogTest, ProfileAndPolicyDenialsOccurBeforeFactory) {
    const auto run_denial = [](bool allow_source, bool allow_node, bool allow_mode,
                               BudgetLimits ceiling) {
        factory_calls.store(0);
        auto       snapshot = registry();
        auto       store    = std::make_shared<InMemoryProgramStore>();
        const auto bundle   = compile(snapshot);
        bool       rejected = false;
        try {
            auto           denied_profile = profile(snapshot, allow_source, allow_node, allow_mode);
            auto           denied_policy  = policy(denied_profile, ceiling);
            ProgramCatalog catalog(CatalogConfig{
                store, snapshot, std::make_shared<EngineGenerationCache>(), "catalog-test/v1"});
            (void)catalog.admit(
                bundle, ProgramAdmission{"tenant:catalog", denied_profile, denied_policy, {}});
        } catch (const std::exception&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
        EXPECT_EQ(factory_calls.load(), 0U);
        EXPECT_FALSE(store->get_bundle(bundle.id()).has_value());
    };

    run_denial(false, true, true, {1000, 100, 100, 1, 1, 20, 1, 1, 1});
    run_denial(true, false, true, {1000, 100, 100, 1, 1, 20, 1, 1, 1});
    run_denial(true, true, false, {1000, 100, 100, 1, 1, 20, 1, 1, 1});
    run_denial(true, true, true, {1000, 100, 100, 1, 1, 19, 1, 1, 1});
}

TEST(ProgramCatalogTest, RejectsCapabilityAndEffectClosureEvenWhenPolicyAllowsIt) {
    factory_calls.store(0);
    auto snapshot        = registry(false, {"catalog-capability"}, {"catalog-effect"});
    auto allowed_profile = profile(snapshot);
    auto allowed_policy  = policy(allowed_profile, {1000, 100, 100, 1, 1, 20, 1, 1, 1}, true, true);
    auto store           = std::make_shared<InMemoryProgramStore>();
    ProgramCatalog catalog(CatalogConfig{store, snapshot, std::make_shared<EngineGenerationCache>(),
                                         "catalog-test/v1"});
    const auto     bundle = compile(snapshot);

    try {
        (void)catalog.admit(
            bundle, ProgramAdmission{"tenant:catalog", allowed_profile, allowed_policy, {}});
        FAIL() << "effectful closure was admitted";
    } catch (const ProgramAdmissionError& error) {
        EXPECT_TRUE(has_code(error, "P_ADMIT_RUNTIME_UNSUPPORTED"));
    }
    EXPECT_EQ(factory_calls.load(), 0U);
    EXPECT_FALSE(store->get_bundle(bundle.id()).has_value());
}

TEST(ProgramCatalogTest, RejectsNonemptySchemasBeforeFactory) {
    factory_calls.store(0);
    CatalogFixture fixture(registry());
    auto           data        = copy_data(compile(fixture.snapshot));
    data.input_contract.schema = json{{"type", "object"}};
    ProgramBundle unsupported(std::move(data));

    try {
        (void)fixture.catalog.admit(unsupported, fixture.request());
        FAIL() << "nonempty schema was admitted";
    } catch (const ProgramAdmissionError& error) {
        EXPECT_TRUE(has_code(error, "P_ADMIT_SCHEMA_UNSUPPORTED"));
    }
    EXPECT_EQ(factory_calls.load(), 0U);
}

TEST(ProgramCatalogTest, FactoryFailurePublishesNeitherVersionNorBundle) {
    factory_calls.store(0);
    CatalogFixture fixture(registry(true));
    const auto     bundle = compile(fixture.snapshot);

    try {
        (void)fixture.catalog.admit(bundle, fixture.request());
        FAIL() << "factory failure was admitted";
    } catch (const ProgramAdmissionError& error) {
        EXPECT_TRUE(has_code(error, "P_MATERIALIZE_FACTORY"));
    }
    EXPECT_EQ(factory_calls.load(), 1U);
    EXPECT_FALSE(fixture.store->get_bundle(bundle.id()).has_value());
}

TEST(ProgramCatalogTest, EquivalentAdmissionIsIdempotent) {
    factory_calls.store(0);
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);

    const auto first  = fixture.catalog.admit(bundle, fixture.request());
    const auto second = fixture.catalog.admit(ProgramBundle::parse(bundle.serialize_canonical()),
                                              fixture.request());

    EXPECT_EQ(first.id(), second.id());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    EXPECT_EQ(factory_calls.load(), 1U);
}

TEST(ProgramCatalogTest, CallerFabricatedVersionCannotBePinned) {
    CatalogFixture        fixture(registry());
    const auto            bundle   = compile(fixture.snapshot);
    const auto            admitted = fixture.catalog.admit(bundle, fixture.request());
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("fabricated-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:fabricated")
        .admission_profile(admitted.admission_profile())
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    auto           fabricated_policy = std::move(policy_builder).build();
    auto           fabricated_data   = ProgramVersionData{admitted.bundle_id(),
                                              admitted.admission_profile(),
                                              fabricated_policy,
                                              admitted.dependency_receipts(),
                                              fabricated_policy.owner_scope(),
                                              admitted.core_materialization_receipt()};
    ProgramVersion fabricated(std::move(fabricated_data));

    EXPECT_THROW(
        (void)neograph::program::detail::CatalogRuntimeAccess::pin(fixture.catalog, fabricated),
        ProgramDiagnosticError);
}
