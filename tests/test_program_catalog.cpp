#include <neograph/graph/node.h>
#include <neograph/provider.h>
#include <neograph/tool.h>
#include <neograph/program/program.h>
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_store.h>
#endif
#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <filesystem>
#endif

#include "catalog_access.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
std::atomic<unsigned> provider_calls{0};
std::atomic<unsigned> tool_calls{0};
std::atomic<unsigned> requirement_resolver_calls{0};
std::atomic<unsigned> binder_calls{0};

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

class CatalogProvider final : public neograph::Provider {
public:
    std::string get_name() const override { return "catalog-provider"; }
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        ++provider_calls;
        return {};
    }
};

class CatalogTool final : public neograph::Tool {
public:
    explicit CatalogTool(std::string name = "catalog-tool") : name_(std::move(name)) {}

    neograph::ChatTool get_definition() const override {
        return {name_, "catalog test tool", json{{"type", "object"}}};
    }
    std::string execute(const json&) override {
        ++tool_calls;
        return "ok";
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class BoundCatalogNode final : public GraphNode {
public:
    BoundCatalogNode(std::string                         name,
                     std::shared_ptr<neograph::Provider> provider,
                     neograph::Tool*                     tool)
        : name_(std::move(name)), provider_(std::move(provider)), tool_(tool) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        (void)provider_->complete({});
        (void)tool_->execute(json::object());
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string                         name_;
    std::shared_ptr<neograph::Provider> provider_;
    neograph::Tool*                     tool_;
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

RegistrySnapshot bound_registry() {
    RegistrySnapshotBuilder builder;
    const auto provider = manifest(ExecutableKind::Provider, "catalog-provider", '3');
    const auto tool     = manifest(ExecutableKind::Tool, "catalog-tool", '4');
    builder.add_provider(provider, ProviderMetadata{json::object(), json::object()});
    builder.add_tool(tool, ToolMetadata{json::object(), json::object()});
    builder.add_node(
        manifest(ExecutableKind::Node, "bound-catalog-node", '5'),
        [](const std::string& name, const json&, const NodeContext& context) {
            ++factory_calls;
            if (!context.provider || context.tools.size() != 1) {
                throw std::runtime_error("factory did not receive owned exact bindings");
            }
            return std::make_unique<BoundCatalogNode>(name, context.provider,
                                                       context.tools.front());
        },
        json{{"type", "object"},
             {"properties",
              json{{"type", json{{"type", "string"}}},
                   {"provider", json{{"type", "string"}}},
                   {"tool_ids",
                    json{{"type", "array"}, {"items", json{{"type", "string"}}}}}}},
             {"additionalProperties", false}},
        json::object(),
        [provider = provider.identity, tool = tool.identity](const json& config) {
            ++requirement_resolver_calls;
            std::vector<ExecutableIdentity> result;
            if (config.value("provider", std::string{}) == provider.name)
                result.push_back(provider);
            if (config.contains("tool_ids") && config["tool_ids"].is_array()) {
                for (const auto& configured : config["tool_ids"]) {
                    if (configured.is_string() &&
                        configured.get<std::string>() == tool.name) {
                        result.push_back(tool);
                        break;
                    }
                }
            }
            return result;
        });
    builder.add_reducer(manifest(ExecutableKind::Reducer, "catalog-overwrite", '2'),
                        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

CatalogCapabilityBinding make_binding(const std::vector<ExecutableIdentity>& requested,
                                      char                                   route) {
    CatalogCapabilityBinding binding;
    std::vector<std::unique_ptr<neograph::Tool>> tools;
    for (const auto& identity : requested) {
        binding.receipts.push_back({identity, digest(route)});
        if (identity.kind == ExecutableKind::Provider) {
            binding.node_context.provider = std::make_shared<CatalogProvider>();
        } else if (identity.kind == ExecutableKind::Tool) {
            tools.push_back(std::make_unique<CatalogTool>());
        }
    }
    binding.tools = neograph::ToolSet(std::move(tools));
    return binding;
}

json requirements(std::uint64_t max_concurrency = 1) {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 1000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 100}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 100}},
        json{{"resource", "max_concurrency"},
             {"minimum", 1},
             {"maximum", max_concurrency}},
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

json bound_document(std::uint64_t max_concurrency = 1) {
    auto result = document();
    result["root"]["definition"]["nodes"]["work"] =
        json{{"type", "bound-catalog-node"},
             {"provider", "catalog-provider"},
             {"tool_ids", json::array({"catalog-tool"})}};
    result["declared_budget_requirements"] = requirements(max_concurrency);
    result["input_contract"]["schema"] =
        json{{"type", "object"},
             {"required", json::array({"task"})},
             {"properties", json{{"task", json{{"type", "string"}}}}},
             {"additionalProperties", false}};
    result["output_contract"]["schema"] =
        json{{"type", "object"},
             {"required", json::array({"answer"})},
             {"properties", json{{"answer", json{{"type", "string"}}}}},
             {"additionalProperties", false}};
    return result;
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

ProgramBundle compile_bound(const RegistrySnapshot& snapshot,
                            std::uint64_t            max_concurrency = 1) {
    ProgramCompiler compiler(snapshot, {"catalog-test/v1"});
    return compiler.compile(ProgramSource::from_cpp_builder(
        "test:catalog-bound", 1, bound_document(max_concurrency)));
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

    explicit CatalogFixture(
        RegistrySnapshot        value,
        CatalogCapabilityBinder binder  = {},
        std::size_t             workers = 1,
        BudgetLimits ceiling = {1000, 100, 100, 1, 1, 20, 1, 1, 1})
        : snapshot(std::move(value)),
          admission(profile(snapshot)),
          authorization(policy(admission, ceiling)),
          catalog(CatalogConfig{store, snapshot, engines, "catalog-test/v1", std::move(binder),
                                workers}) {}

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

TEST(ProgramCatalogTest, OwnerQualifiedStoreAndCatalogLookupsHideForeignPublicIds) {
    CatalogFixture fixture(registry());
    const auto     bundle  = compile(fixture.snapshot);
    const auto     version = fixture.catalog.admit(bundle, fixture.request());

    EXPECT_TRUE(fixture.store->get_version("tenant:catalog", version.id()).has_value());
    EXPECT_FALSE(fixture.store->get_version("tenant:other", version.id()).has_value());
    EXPECT_TRUE(fixture.store->get_bundle("tenant:catalog", bundle.id()).has_value());
    EXPECT_FALSE(fixture.store->get_bundle("tenant:other", bundle.id()).has_value());
    EXPECT_TRUE(fixture.catalog.find_version("tenant:catalog", version.id()).has_value());
    EXPECT_FALSE(fixture.catalog.find_version("tenant:other", version.id()).has_value());
}

TEST(ProgramCatalogTest, TrustedNativeRequiresMatchingAuthenticatedHostIdentity) {
    RegistrySnapshotBuilder registry_builder;
    auto native = manifest(ExecutableKind::Node, "catalog-node", '1');
    native.effect_mode = EffectMode::TrustedNative;
    native.attestation_id = "host:trusted";
    registry_builder.add_node(
        std::move(native),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CatalogNode>(name);
        },
        json{{"type", "object"}}, json::object());
    registry_builder.add_reducer(manifest(ExecutableKind::Reducer, "catalog-overwrite", '2'),
                                 [](const json&, const json& incoming) { return json(incoming); });
    auto snapshot = std::move(registry_builder).build();
    AdmissionProfileBuilder profile_builder;
    profile_builder.id("trusted-catalog-profile")
        .semantic_version("1.0.0")
        .registry(snapshot)
        .mode(AdmissionMode::TrustedEmbedding)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered)
        .allow_effect_mode(EffectMode::TrustedNative);
    for (const auto& identity : snapshot.identities())
        profile_builder.allow_executable(identity);
    const auto trusted_profile = std::move(profile_builder).build();
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("trusted-catalog-policy")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(trusted_profile)
        .allow_capability(std::string(TRUSTED_NATIVE_CAPABILITY))
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    const auto trusted_policy = std::move(policy_builder).build();
    const auto bundle = compile(snapshot);

    const auto try_admit = [&](std::string host_identity, std::string_view expected_code) {
        ProgramCatalog catalog(CatalogConfig{
            std::make_shared<InMemoryProgramStore>(), snapshot,
            std::make_shared<EngineGenerationCache>(), "catalog-test/v1", {}, 1,
            {}, std::move(host_identity)});
        return [&] {
            try {
                (void)catalog.admit(
                    bundle, ProgramAdmission{"tenant:catalog", trusted_profile, trusted_policy, {}});
                return false;
            } catch (const ProgramAdmissionError& error) {
                return has_code(error, std::string(expected_code));
            }
        }();
    };
    EXPECT_TRUE(try_admit({}, "P_ADMIT_HOST_IDENTITY"));
    EXPECT_TRUE(try_admit("host:other", "P_ADMIT_HOST_IDENTITY"));

    ProgramCatalog catalog(CatalogConfig{
        std::make_shared<InMemoryProgramStore>(), snapshot,
        std::make_shared<EngineGenerationCache>(), "catalog-test/v1", {}, 1, {},
        "host:trusted"});
    EXPECT_NO_THROW((void)catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog", trusted_profile, trusted_policy, {}}));
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

TEST(ProgramCatalogTest, BrokeredClosureAdmitsWhenPolicyAllows) {
    factory_calls.store(0);
    auto snapshot        = registry(false, {"catalog-capability"}, {"catalog-effect"});
    auto allowed_profile = profile(snapshot);
    auto allowed_policy =
        policy(allowed_profile, {1000, 100, 100, 1, 1, 20, 1, 1, 1}, true, true);
    auto store = std::make_shared<InMemoryProgramStore>();
    ProgramCatalog catalog(CatalogConfig{store, snapshot, std::make_shared<EngineGenerationCache>(),
                                         "catalog-test/v1"});
    const auto bundle = compile(snapshot);

    const auto version = catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog", allowed_profile, allowed_policy, {}});

    EXPECT_EQ(factory_calls.load(), 1U);
    EXPECT_TRUE(store->get_version(version.id()).has_value());
}

TEST(ProgramCatalogTest, ConfigDerivedClosureBindsExactlyAndOwnedResourcesRun) {
    factory_calls.store(0);
    provider_calls.store(0);
    tool_calls.store(0);
    binder_calls.store(0);
    requirement_resolver_calls.store(0);
    auto snapshot = bound_registry();
    CatalogFixture fixture(
        snapshot, [](const std::vector<ExecutableIdentity>& requested) {
            ++binder_calls;
            return make_binding(requested, '8');
        });
    const auto bundle         = compile_bound(snapshot);
    const auto compile_calls  = requirement_resolver_calls.load();

    const auto version = fixture.catalog.admit(bundle, fixture.request());

    EXPECT_EQ(binder_calls.load(), 1U);
    EXPECT_EQ(factory_calls.load(), 1U);
    EXPECT_GT(requirement_resolver_calls.load(), compile_calls);
    EXPECT_EQ(version.core_materialization_receipt().capability_bindings.size(), 2U);
    EXPECT_EQ(version.core_materialization_receipt().capability_bindings,
              ProgramVersion::parse(version.serialize_canonical())
                  .core_materialization_receipt()
                  .capability_bindings);
    EXPECT_EQ(bundle.input_contract().schema["required"], json::array({"task"}));
    EXPECT_EQ(bundle.output_contract().schema["required"], json::array({"answer"}));

    auto pinned = neograph::program::detail::CatalogRuntimeAccess::pin(fixture.catalog, version);
    RunConfig run;
    run.thread_id = "catalog-owned-binding";
    run.input      = json::object();
    (void)pinned->root->engine->run(run);
    EXPECT_EQ(provider_calls.load(), 1U);
    EXPECT_EQ(tool_calls.load(), 1U);
}

TEST(ProgramCatalogTest, PolicyDenialSkipsCapabilityBinder) {
    factory_calls.store(0);
    binder_calls.store(0);
    auto snapshot = bound_registry();
    const auto bundle = compile_bound(snapshot);
    auto denied_profile = profile(snapshot, true, false, true);
    auto denied_policy  = policy(denied_profile);
    ProgramCatalog catalog(CatalogConfig{
        std::make_shared<InMemoryProgramStore>(), snapshot,
        std::make_shared<EngineGenerationCache>(), "catalog-test/v1",
        [](const std::vector<ExecutableIdentity>& requested) {
            ++binder_calls;
            return make_binding(requested, '9');
        }});

    EXPECT_THROW(
        (void)catalog.admit(
            bundle, ProgramAdmission{"tenant:catalog", denied_profile, denied_policy, {}}),
        ProgramAdmissionError);
    EXPECT_EQ(binder_calls.load(), 0U);
    EXPECT_EQ(factory_calls.load(), 0U);
}

TEST(ProgramCatalogTest, InvalidBindingReceiptsRejectBeforeFactory) {
    factory_calls.store(0);
    binder_calls.store(0);
    auto snapshot = bound_registry();
    CatalogFixture fixture(
        snapshot, [](const std::vector<ExecutableIdentity>& requested) {
            ++binder_calls;
            auto binding = make_binding(requested, 'a');
            binding.receipts.pop_back();
            return binding;
        });
    const auto bundle = compile_bound(snapshot);

    try {
        (void)fixture.catalog.admit(bundle, fixture.request());
        FAIL() << "incomplete capability binding was admitted";
    } catch (const ProgramAdmissionError& error) {
        EXPECT_TRUE(has_code(error, "P_BINDING_COVERAGE"));
    }
    EXPECT_EQ(binder_calls.load(), 1U);
    EXPECT_EQ(factory_calls.load(), 0U);
}

TEST(ProgramCatalogTest, ExtraDuplicateAndNonShaReceiptsRejectBeforeFactory) {
    using Mutator = std::function<void(CatalogCapabilityBinding&)>;
    const std::vector<Mutator> mutations{
        [](CatalogCapabilityBinding& binding) {
            binding.receipts.push_back(
                CapabilityBindingReceipt{
                    ExecutableIdentity{ExecutableKind::Tool, "extra-tool", "1.0.0", digest('1')},
                    digest('2')});
        },
        [](CatalogCapabilityBinding& binding) {
            binding.receipts.push_back(binding.receipts.front());
        },
        [](CatalogCapabilityBinding& binding) {
            binding.receipts.front().binding_identity = "route:not-sha256";
        },
        [](CatalogCapabilityBinding& binding) {
            binding.receipts.front().executable.implementation_digest = "implementation:not-sha";
        },
        [](CatalogCapabilityBinding& binding) {
            std::vector<std::unique_ptr<neograph::Tool>> tools;
            tools.push_back(std::make_unique<CatalogTool>("wrong-tool"));
            binding.tools = neograph::ToolSet(std::move(tools));
        }};

    for (const auto& mutate : mutations) {
        factory_calls.store(0);
        auto snapshot      = bound_registry();
        auto admission     = profile(snapshot);
        auto authorization = policy(admission);
        ProgramCatalog catalog(CatalogConfig{
            std::make_shared<InMemoryProgramStore>(), snapshot,
            std::make_shared<EngineGenerationCache>(), "catalog-test/v1",
            [mutate](const std::vector<ExecutableIdentity>& requested) {
                auto binding = make_binding(requested, 'a');
                mutate(binding);
                return binding;
            }});
        const auto bundle = compile_bound(snapshot);

        EXPECT_THROW(
            (void)catalog.admit(
                bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}}),
            ProgramAdmissionError);
        EXPECT_EQ(factory_calls.load(), 0U);
    }
}

TEST(ProgramCatalogTest, BindingReceiptRouteChangesVersionAndEngineGeneration) {
    factory_calls.store(0);
    auto snapshot      = bound_registry();
    auto store         = std::make_shared<InMemoryProgramStore>();
    auto engines       = std::make_shared<EngineGenerationCache>();
    auto admission     = profile(snapshot);
    auto authorization = policy(admission);
    const auto bundle  = compile_bound(snapshot);
    ProgramCatalog first(CatalogConfig{
        store, snapshot, engines, "catalog-test/v1",
        [](const std::vector<ExecutableIdentity>& requested) {
            return make_binding(requested, 'b');
        }});
    ProgramCatalog second(CatalogConfig{
        store, snapshot, engines, "catalog-test/v1",
        [](const std::vector<ExecutableIdentity>& requested) {
            return make_binding(requested, 'c');
        }});

    const auto first_version = first.admit(
        bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}});
    const auto second_version = second.admit(
        bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}});

    EXPECT_NE(first_version.id(), second_version.id());
    EXPECT_NE(capability_binding_receipt_root(
                  first_version.core_materialization_receipt().capability_bindings),
              capability_binding_receipt_root(
                  second_version.core_materialization_receipt().capability_bindings));
    const auto first_materialized =
        neograph::program::detail::CatalogRuntimeAccess::pin(first, first_version);
    const auto second_materialized =
        neograph::program::detail::CatalogRuntimeAccess::pin(second, second_version);
    EXPECT_NE(first_materialized->root.get(), second_materialized->root.get());
    EXPECT_EQ(factory_calls.load(), 2U);
}

TEST(ProgramCatalogTest, WorkerCountSeparatesOtherwiseIdenticalEngineGenerations) {
    factory_calls.store(0);
    auto snapshot      = bound_registry();
    auto store         = std::make_shared<InMemoryProgramStore>();
    auto engines       = std::make_shared<EngineGenerationCache>();
    auto admission     = profile(snapshot);
    auto authorization =
        policy(admission, BudgetLimits{1000, 100, 100, 4, 1, 20, 1, 1, 1});
    const auto bundle = compile_bound(snapshot, 4);
    const auto binder = [](const std::vector<ExecutableIdentity>& requested) {
        return make_binding(requested, '6');
    };
    ProgramCatalog serial(
        CatalogConfig{store, snapshot, engines, "catalog-test/v1", binder, 1});
    ProgramCatalog parallel(
        CatalogConfig{store, snapshot, engines, "catalog-test/v1", binder, 4});

    const auto serial_version = serial.admit(
        bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}});
    const auto parallel_version = parallel.admit(
        bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}});

    EXPECT_EQ(serial_version.id(), parallel_version.id());
    const auto serial_materialized =
        neograph::program::detail::CatalogRuntimeAccess::pin(serial, serial_version);
    const auto parallel_materialized =
        neograph::program::detail::CatalogRuntimeAccess::pin(parallel, parallel_version);
    EXPECT_NE(serial_materialized->root.get(), parallel_materialized->root.get());
    EXPECT_EQ(factory_calls.load(), 2U);
}

TEST(ProgramCatalogTest, ResolveVersionIsOwnerScopedAndRebindsExactStoredReceipt) {
    auto snapshot = bound_registry();
    auto store    = std::make_shared<InMemoryProgramStore>();
    auto original_engines = std::make_shared<EngineGenerationCache>();
    auto admission        = profile(snapshot);
    auto authorization    = policy(admission);
    const auto bundle     = compile_bound(snapshot);
    ProgramCatalog original(CatalogConfig{
        store, snapshot, original_engines, "catalog-test/v1",
        [](const std::vector<ExecutableIdentity>& requested) {
            return make_binding(requested, 'd');
        }});
    const auto stored = original.admit(
        bundle, ProgramAdmission{"tenant:catalog", admission, authorization, {}});

    factory_calls.store(0);
    binder_calls.store(0);
    ProgramCatalog recovered(CatalogConfig{
        store, snapshot, std::make_shared<EngineGenerationCache>(), "catalog-test/v1",
        [](const std::vector<ExecutableIdentity>& requested) {
            ++binder_calls;
            return make_binding(requested, 'd');
        }});
    EXPECT_FALSE(recovered.resolve_version("tenant:other", stored.id()).has_value());
    EXPECT_EQ(binder_calls.load(), 0U);

    const auto resolved = recovered.resolve_version("tenant:catalog", stored.id());
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->serialize_canonical(), stored.serialize_canonical());
    EXPECT_EQ(binder_calls.load(), 1U);
    EXPECT_EQ(factory_calls.load(), 1U);

    std::vector<ExecutableIdentity> requested;
    for (const auto& receipt : stored.core_materialization_receipt().capability_bindings)
        requested.push_back(receipt.executable);
    const auto factories_before = factory_calls.load();
    try {
        (void)recovered.resolve_version_with_binding("tenant:catalog", stored.id(),
                                                     make_binding(requested, 'e'));
        FAIL() << "recorded binding with a different receipt was accepted";
    } catch (const ProgramAdmissionError& error) {
        EXPECT_TRUE(has_code(error, "P_RESOLVE_BINDING"));
    }
    EXPECT_EQ(factory_calls.load(), factories_before);
}

TEST(ProgramCatalogTest, PositiveFiniteConcurrencyAboveOneIsAdmitted) {
    factory_calls.store(0);
    auto snapshot = bound_registry();
    CatalogFixture fixture(
        snapshot,
        [](const std::vector<ExecutableIdentity>& requested) {
            return make_binding(requested, 'f');
        },
        4, BudgetLimits{1000, 100, 100, 4, 1, 20, 1, 1, 1});
    const auto bundle = compile_bound(snapshot, 4);

    const auto version = fixture.catalog.admit(bundle, fixture.request());

    EXPECT_FALSE(version.id().empty());
    EXPECT_EQ(factory_calls.load(), 1U);
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


TEST(ProgramCatalogTest, ActivationCompareAndSwapAndRetentionAreOwnerScoped) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    const auto     first  = fixture.catalog.admit(bundle, fixture.request());

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("catalog-policy-next")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    const auto next_policy = std::move(policy_builder).build();
    const auto second =
        fixture.catalog.admit(bundle, ProgramAdmission{"tenant:catalog", fixture.admission,
                                                        next_policy, {}});

    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", first.id(), 0),
              ProgramActivationResult::Activated);
    const auto activation = fixture.catalog.activation("tenant:catalog");
    ASSERT_TRUE(activation.has_value());
    EXPECT_EQ(activation->generation(), 1U);
    EXPECT_EQ(activation->active_version_id(), first.id());

    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", second.id(), 0),
              ProgramActivationResult::Conflict);
    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", second.id(), 1),
              ProgramActivationResult::Activated);
    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", second.id(), 2),
              ProgramActivationResult::AlreadyPresent);

    const auto report = fixture.catalog.collect_retention("tenant:catalog", {});
    EXPECT_EQ(report.versions_removed, 1U);
    EXPECT_FALSE(fixture.store->get_version(first.id()).has_value());
    EXPECT_TRUE(fixture.store->get_version(second.id()).has_value());
    EXPECT_TRUE(fixture.store->get_bundle(bundle.id()).has_value());
}

TEST(ProgramCatalogTest, RollbackIsAnOwnerScopedActivationCasAndChecksPolicyIdentity) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    const auto     first  = fixture.catalog.admit(bundle, fixture.request());

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("catalog-policy-rollback")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    const auto second_policy = std::move(policy_builder).build();
    const auto second = fixture.catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog", fixture.admission, second_policy, {}});

    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", first.id(), 0),
              ProgramActivationResult::Activated);
    EXPECT_EQ(fixture.catalog.activate("tenant:catalog", second.id(), 1),
              ProgramActivationResult::Activated);
    EXPECT_EQ(fixture.catalog.rollback("tenant:catalog", first.id(), 1),
              ProgramActivationResult::Conflict);
    EXPECT_EQ(fixture.catalog.rollback("tenant:catalog", first.id(), 2),
              ProgramActivationResult::Activated);
    ASSERT_TRUE(fixture.catalog.activation("tenant:catalog").has_value());
    EXPECT_EQ(fixture.catalog.activation("tenant:catalog")->active_version_id(), first.id());

    EXPECT_THROW(fixture.store->compare_activate("tenant:catalog", 3, first.id(), digest('z')),
                 std::invalid_argument);
    EXPECT_THROW(fixture.store->compare_activate("tenant:other", 0, first.id(),
                                                 first.policy_snapshot().fingerprint()),
                 std::invalid_argument);
}

TEST(ProgramCatalogTest, RetentionRejectsUnknownAndForeignPinsBeforeMutation) {
    CatalogFixture fixture(registry());
    const auto     bundle  = compile(fixture.snapshot);
    const auto     version = fixture.catalog.admit(bundle, fixture.request());
    ASSERT_TRUE(fixture.store->get_version("tenant:catalog", version.id()).has_value());
    EXPECT_FALSE(fixture.store->get_version("tenant:other", version.id()).has_value());
    ASSERT_TRUE(fixture.store->get_bundle("tenant:catalog", bundle.id()).has_value());
    EXPECT_FALSE(fixture.store->get_bundle("tenant:other", bundle.id()).has_value());
    EXPECT_THROW(fixture.catalog.collect_retention("tenant:catalog", {"sha256:missing"}),
                 std::invalid_argument);
    EXPECT_THROW(fixture.catalog.collect_retention("tenant:other", {version.id()}),
                 std::invalid_argument);
    EXPECT_THROW(fixture.store->collect_garbage("tenant:catalog", {"sha256:missing"}),
                 std::invalid_argument);
    EXPECT_TRUE(fixture.store->get_version(version.id()).has_value());
}

TEST(ProgramCatalogTest, RetentionKeepsExplicitPinnedVersion) {
    CatalogFixture fixture(registry());
    const auto     bundle  = compile(fixture.snapshot);
    const auto     version = fixture.catalog.admit(bundle, fixture.request());

    const auto report = fixture.catalog.collect_retention("tenant:catalog", {version.id()});
    EXPECT_EQ(report.versions_removed, 0U);
    EXPECT_EQ(report.bundles_removed, 0U);
    ASSERT_TRUE(fixture.store->get_version("tenant:catalog", version.id()).has_value());
    EXPECT_TRUE(fixture.store->get_bundle("tenant:catalog", bundle.id()).has_value());
}

TEST(ProgramCatalogTest, ReAdmissionRestoresTupleAfterCachedVersionIsCollected) {
    CatalogFixture fixture(registry());
    const auto     bundle  = compile(fixture.snapshot);
    const auto     version = fixture.catalog.admit(bundle, fixture.request());

    const auto removed = fixture.catalog.collect_retention("tenant:catalog", {});
    EXPECT_EQ(removed.versions_removed, 1U);
    EXPECT_EQ(removed.bundles_removed, 1U);
    EXPECT_FALSE(fixture.store->get_version("tenant:catalog", version.id()).has_value());
    EXPECT_FALSE(fixture.store->get_bundle("tenant:catalog", bundle.id()).has_value());

    const auto readmitted = fixture.catalog.admit(
        ProgramBundle::parse(bundle.serialize_canonical()), fixture.request());
    EXPECT_EQ(readmitted.id(), version.id());
    EXPECT_EQ(readmitted.serialize_canonical(), version.serialize_canonical());
    ASSERT_TRUE(fixture.store->get_version("tenant:catalog", version.id()).has_value());
    ASSERT_TRUE(fixture.store->get_bundle("tenant:catalog", bundle.id()).has_value());
    EXPECT_EQ(fixture.store->get_version("tenant:catalog", version.id())
                  ->serialize_canonical(),
              version.serialize_canonical());
}

TEST(ProgramCatalogTest, MigrationPlanRecordsCompatibilityProofAndRoundTrips) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    const auto     first  = fixture.catalog.admit(bundle, fixture.request());

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("catalog-policy-migration")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    const auto target_policy = std::move(policy_builder).build();
    const auto target =
        fixture.catalog.admit(bundle, ProgramAdmission{"tenant:catalog", fixture.admission,
                                                        target_policy, {}});

    const auto plan = fixture.catalog.plan_migration("tenant:catalog", first.id(), target.id());
    EXPECT_TRUE(plan.is_compatible());
    EXPECT_TRUE(plan.blockers().empty());
    EXPECT_EQ(MigrationPlan::parse(plan.serialize_canonical()).id(), plan.id());

    const auto explicit_plan = MigrationPlan::create(
        MigrationPlanData{first.id(), target.id(), "tenant:catalog",
                          MigrationCompatibility::CompilerMismatch, {"compiler_build_id"}});
    EXPECT_FALSE(explicit_plan.is_compatible());
    EXPECT_EQ(explicit_plan.blockers(), std::vector<std::string>{"compiler_build_id"});
}

TEST(ProgramCatalogTest, MigrationPlanDefersNarrowerTargetBudgetToForkInvocation) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);

    PolicySnapshotBuilder source_policy_builder;
    source_policy_builder.id("catalog-policy-migration-source")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{2000, 200, 200, 2, 2, 40, 2, 2, 2});
    const auto source = fixture.catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog", fixture.admission,
                                 std::move(source_policy_builder).build(), {}});
    const auto target = fixture.catalog.admit(bundle, fixture.request());

    const auto plan = fixture.catalog.plan_migration("tenant:catalog", source.id(), target.id());
    EXPECT_TRUE(plan.is_compatible());
    EXPECT_TRUE(plan.diagnostics().empty());
    ASSERT_GT(plan.mappings().size(), 7U);
    EXPECT_EQ(plan.mappings()[7].dimension, MigrationDimension::Budget);
    EXPECT_EQ(plan.mappings()[7].rule,
              "runtime_request_must_fit_source_remainder_and_target_admitted_bounds");
}

TEST(ProgramCatalogTest, ComposedAdmissionRequiresVerifiedModuleStore) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:catalog";
    module_data.coordinate     = ModuleCoordinate{"catalog", "parent", "1.0.0", ""};
    module_data.attestation_id = "attestation:catalog";
    const auto parent = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = parent.coordinate();
    resolution.modules.push_back(parent);
    const ProgramComposition composition{parent, resolution, {}};

    EXPECT_THROW((void)fixture.catalog.admit_composed(bundle, fixture.request(), composition),
                 std::invalid_argument);
}

TEST(ProgramCatalogTest, MigrationPlanCoversEveryP5DimensionWithNarrowMappings) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    const auto     source = fixture.catalog.admit(bundle, fixture.request());

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("catalog-policy-p5-mapping")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{1000, 100, 100, 1, 1, 20, 1, 1, 1});
    const auto target = fixture.catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog", fixture.admission,
                                 std::move(policy_builder).build(), {}});

    const auto plan = fixture.catalog.plan_migration("tenant:catalog", source.id(), target.id());
    ASSERT_TRUE(plan.is_compatible());
    EXPECT_TRUE(plan.diagnostics().empty());
    ASSERT_EQ(plan.mappings().size(), 14U);
    for (std::uint8_t index = 0; index < 14; ++index) {
        EXPECT_EQ(static_cast<std::uint8_t>(plan.mappings()[index].dimension), index);
        EXPECT_FALSE(plan.mappings()[index].rule.empty());
    }
    const auto reparsed = MigrationPlan::parse(plan.serialize_canonical());
    for (std::size_t index = 0; index < plan.mappings().size(); ++index) {
        EXPECT_EQ(reparsed.mappings()[index].dimension, plan.mappings()[index].dimension);
        EXPECT_EQ(reparsed.mappings()[index].rule, plan.mappings()[index].rule);
    }
}

TEST(ProgramCatalogTest, MigrationPlanFailsClosedForUnknownCompatibilityAndMismatches) {
    CatalogFixture fixture(registry());
    const auto     bundle = compile(fixture.snapshot);
    const auto     source = fixture.catalog.admit(bundle, fixture.request());
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("catalog-policy-p5-blocked")
        .semantic_version("1.0.0")
        .owner_scope("tenant:catalog")
        .admission_profile(fixture.admission)
        .budget_ceiling(BudgetLimits{2000, 200, 200, 2, 2, 40, 2, 2, 2});
    const auto target = fixture.catalog.admit(
        bundle, ProgramAdmission{"tenant:catalog",
                                 fixture.admission,
                                 std::move(policy_builder).build(), {}});
    const auto expanded =
        fixture.catalog.plan_migration("tenant:catalog", source.id(), target.id());
    EXPECT_TRUE(expanded.is_compatible());

    const auto blocked = MigrationPlan::create(MigrationPlanData{
        source.id(), target.id(), "tenant:catalog", MigrationCompatibility::Blocked,
        {"budget_ceiling"},
        {MigrationDiagnostic{MigrationDimension::Budget,
                             "budget_ceiling",
                             "source budget is not covered",
                             json{{"max_core_steps", 20}},
                             json{{"max_core_steps", 10}}}},
        {}});
    EXPECT_FALSE(blocked.is_compatible());
    EXPECT_FALSE(blocked.diagnostics().empty());
    EXPECT_EQ(blocked.diagnostics().front().dimension, MigrationDimension::Budget);

    const auto compatible = MigrationPlan::between(source, source);
    auto       stored     = compatible.serialize_canonical();
    const auto marker     = std::string("fork_compatible");
    const auto position   = stored.find(marker);
    ASSERT_NE(position, std::string::npos);
    stored.replace(position, marker.size(), "unknown_compatibility");
    EXPECT_THROW((void)MigrationPlan::parse(stored), std::invalid_argument);
}

TEST(ProgramCatalogTest, MigrationPlanClassMatrixAndEvidenceAreFailClosed) {
    const auto source = digest('a');
    const auto target = digest('b');
    const auto evidence = MigrationDiagnostic{MigrationDimension::Recovery,
                                              "recovery_required",
                                              "operator recovery is required",
                                              json{{"state", "source"}},
                                              json{{"state", "target"}}};
    const std::vector classes = {MigrationCompatibility::NewRunsOnly,
                                 MigrationCompatibility::ForkCompatible,
                                 MigrationCompatibility::DrainOnly,
                                 MigrationCompatibility::OperatorReconciliation,
                                 MigrationCompatibility::Blocked};
    for (const auto compatibility : classes) {
        MigrationPlanData data{source, target, "tenant:catalog", compatibility, {}, {}, {}};
        if (compatibility == MigrationCompatibility::ForkCompatible)
            data.mappings.push_back(MigrationMapping{MigrationDimension::Continuation,
                                                      "exact_checkpoint_continuation", source,
                                                      target});
        else
            data.diagnostics.push_back(evidence);
        const auto plan = MigrationPlan::create(std::move(data));
        EXPECT_EQ(plan.compatibility(), compatibility);
        EXPECT_EQ(MigrationPlan::parse(plan.serialize_canonical()).id(), plan.id());
    }

    const auto alias = MigrationPlan::create(
        MigrationPlanData{source, target, "tenant:catalog",
                          MigrationCompatibility::CompilerMismatch, {"compiler_build_id"}});
    EXPECT_EQ(alias.compatibility(), MigrationCompatibility::Blocked);
    EXPECT_FALSE(alias.is_compatible());
}

TEST(ProgramCatalogTest, MigrationPlanRejectsMissingEvidenceAndArbitraryIdentity) {
    const auto plan = MigrationPlan::create(
        MigrationPlanData{digest('c'), digest('d'), "tenant:catalog",
                          MigrationCompatibility::ForkCompatible, {}, {}, {}});
    auto missing_diagnostics = plan.serialize_canonical();
    const auto diagnostics_key = missing_diagnostics.find("\"diagnostics\"");
    ASSERT_NE(diagnostics_key, std::string::npos);
    const auto diagnostics_end = missing_diagnostics.find(",\"mappings\"", diagnostics_key);
    ASSERT_NE(diagnostics_end, std::string::npos);
    missing_diagnostics.erase(diagnostics_key, diagnostics_end - diagnostics_key);
    EXPECT_ANY_THROW((void)MigrationPlan::parse(missing_diagnostics));

    auto arbitrary_id = plan.serialize_canonical();
    const auto id_key = arbitrary_id.find("\"id\":\"");
    ASSERT_NE(id_key, std::string::npos);
    const auto id_start = id_key + 6;
    const auto id_end = arbitrary_id.find('"', id_start);
    arbitrary_id.replace(id_start, id_end - id_start, "not-an-identity");
    EXPECT_THROW((void)MigrationPlan::parse(arbitrary_id), std::invalid_argument);
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramCatalogTest, SQLiteProgramStoreReopensActivationAndRetention) {
    static std::atomic<unsigned> sequence{0};
    const auto path =
        (std::filesystem::temp_directory_path() /
         ("neograph-program-store-" + std::to_string(sequence.fetch_add(1)) + ".db"))
            .string();
    std::filesystem::remove(path);

    CatalogFixture fixture(registry());
    const auto bundle = compile(fixture.snapshot);
    const auto version = fixture.catalog.admit(bundle, fixture.request());

    {
        SQLiteProgramStore store(path);
        store.publish_admitted(bundle, version);
        EXPECT_EQ(store.compare_activate("tenant:catalog", 0, version.id(),
                                         version.policy_snapshot().fingerprint()),
                  ProgramActivationResult::Activated);
    }
    {
        SQLiteProgramStore store(path);
        ASSERT_TRUE(store.get_bundle(bundle.id()).has_value());
        ASSERT_TRUE(store.get_version(version.id()).has_value());
        ASSERT_TRUE(store.get_version("tenant:catalog", version.id()).has_value());
        EXPECT_FALSE(store.get_version("tenant:other", version.id()).has_value());
        ASSERT_TRUE(store.get_bundle("tenant:catalog", bundle.id()).has_value());
        EXPECT_FALSE(store.get_bundle("tenant:other", bundle.id()).has_value());
        const auto activation = store.get_activation("tenant:catalog");
        ASSERT_TRUE(activation.has_value());
        EXPECT_EQ(activation->generation(), 1U);
        EXPECT_EQ(activation->active_version_id(), version.id());
        EXPECT_EQ(store.compare_activate("tenant:catalog", 0, version.id(),
                                         version.policy_snapshot().fingerprint()),
                  ProgramActivationResult::Conflict);
        EXPECT_THROW(store.compare_activate("tenant:catalog", 1, version.id(), digest('z')),
                     std::invalid_argument);
        EXPECT_THROW(store.collect_garbage("tenant:catalog", {"sha256:missing"}),
                     std::invalid_argument);
        EXPECT_EQ(store.collect_garbage("tenant:catalog", {}).versions_removed, 0U);
    }

    std::filesystem::remove(path);
}
#endif
