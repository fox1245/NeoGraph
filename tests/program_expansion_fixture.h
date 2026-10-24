// Shared Program-runtime test fixture: admission profile and policy, stores, child bindings,
// the expansion template/policy/document builders and the linked-child helper.
//
// test_program_runtime.cpp and the expansion suites include this header and instantiate
// BasicAdmittedRuntime with the registry of the executables they need.
#pragma once

#include <neograph/graph/engine.h>
#include <neograph/hook_runtime.h>
#include "fixtures/store_handle.h"
#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using neograph::program::test_support::FragmentStoreHandle;

neograph::json orchestration_document(neograph::json root, std::string node_type);

namespace neograph::program_test {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

inline std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

inline ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:test",
                              {},
                              {},
                              {}};
}

inline json budget_requirements() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}
inline json                      program_document(std::string node_type);
inline TaskGraphTemplateContract expansion_template_contract() {
    TaskGraphTemplateContract contract;
    contract.template_id      = "runtime-map/v1";
    contract.content_identity = digest('a');
    contract.input_fields     = {"/item"};
    contract.output_artifacts = {TaskGraphArtifactContract{"result", {"/channels/value/value"}}};
    contract.budget_ceiling   = TaskGraphBudget{1000, 100, 100, 4096};
    contract.child_binding    = "child";
    contract.kind             = "program";
    return contract;
}

inline TaskGraphExpansionPolicy expansion_policy() {
    TaskGraphExpansionPolicy policy;
    policy.limits.max_tasks               = 4;
    policy.limits.max_edges               = 8;
    policy.limits.max_depth               = 4;
    policy.limits.per_task_budget_ceiling = TaskGraphBudget{1000, 100, 100, 4096};
    policy.limits.total_budget_ceiling    = TaskGraphBudget{4000, 400, 400, 8192};
    policy.template_allowlist             = {expansion_template_contract()};
    return policy;
}

inline json expansion_proposal() {
    return json{
        {"schema_version", 1},
        {"tasks", json::array({
                      json{{"id", "dependent"},
                           {"template", "runtime-map/v1"},
                           {"input_bindings",
                            json::array({json{{"from", json{{"task", "seed"},
                                                            {"artifact", "result"},
                                                            {"field", "/channels/value/value"}}},
                                              {"to", json{{"field", "/item"}}}}})},
                           {"depends_on", json::array({"seed"})},
                           {"budget", json{{"wall_time_ms", 100}, {"model_tokens", 1}}}},
                      json{{"id", "seed"},
                           {"template", "runtime-map/v1"},
                           {"input_bindings", json::array()},
                           {"depends_on", json::array()},
                           {"budget", json{{"wall_time_ms", 100}, {"model_tokens", 1}}}},
                  })},
        {"join", json{{"kind", "all"}}},
    };
}

inline json expand_task_graph_document(json          proposal,
                                std::uint64_t max_tasks       = 2,
                                std::uint64_t max_edges       = 4,
                                std::uint64_t max_depth       = 2,
                                std::uint64_t max_concurrency = 1) {
    auto document                                          = program_document("runtime-completed");
    document["program_schema_version"]                     = PROGRAM_SCHEMA_VERSION_V4;
    document["declared_budget_requirements"][3]["maximum"] = max_concurrency;
    document["declared_budget_requirements"][4]["maximum"] = max_tasks;
    document["declared_budget_requirements"][6]["minimum"] = 1;
    document["declared_budget_requirements"][6]["maximum"] = 1;
    document["declared_budget_requirements"][7]["minimum"] = max_depth;
    document["declared_budget_requirements"][7]["maximum"] = max_depth;
    document["declared_budget_requirements"][8]["minimum"] = max_tasks;
    document["declared_budget_requirements"][8]["maximum"] = max_tasks;
    const auto definition                                  = document["root"]["definition"];
    document["root"]                                       = json{{"op", "expand_task_graph"},
                                                                  {"name", "main"},
                                                                  {"definition", definition},
                                                                  {"proposal_source", json{{"inline", std::move(proposal)}}},
                                                                  {"max_tasks", max_tasks},
                                                                  {"max_edges", max_edges},
                                                                  {"max_depth", max_depth},
                                                                  {"max_dynamic_compiles", 1},
                                                                  {"max_total_children", max_tasks},
                                                                  {"max_concurrency", max_concurrency},
                                                                  {"failure_policy", "collect"}};
    return document;
}

inline json program_document(std::string node_type) {
    json nodes{{"work", json{{"type", node_type}}}};
    json edges = json::array({
        json{{"from", "__start__"}, {"to", "work"}},
        json{{"from", "work"}, {"to", "__end__"}},
    });
    if (node_type == "runtime-blocking" || node_type == "runtime-short-blocking" ||
        node_type == "runtime-scheduler-blocking") {
        nodes["followup"] = json{{"type", "runtime-followup"}};
        edges             = json::array({
            json{{"from", "__start__"}, {"to", "work"}},
            json{{"from", "work"}, {"to", "followup"}},
            json{{"from", "followup"}, {"to", "__end__"}},
        });
    }
    json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"value", json{{"reducer", "runtime-overwrite"}, {"initial", ""}}}}},
        {"nodes", std::move(nodes)},
        {"edges", std::move(edges)},
        {"conditional_edges", json::array()}};
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budget_requirements()}};
}

struct ChildBindingRegistry {
    using Key = std::pair<std::string, std::string>;

    void bind(std::string                parent_version_id,
              std::string                binding_name,
              ProgramRuntimeChildBinding binding) {
        std::lock_guard lock(mutex);
        bindings.insert_or_assign(Key{std::move(parent_version_id), std::move(binding_name)},
                                  std::move(binding));
    }

    std::optional<ProgramRuntimeChildBinding> resolve(std::string_view owner_scope,
                                                      std::string_view parent_version_id,
                                                      std::string_view binding_name) const {
        if (owner_scope != "tenant:runtime") return std::nullopt;
        std::lock_guard lock(mutex);
        const auto      found =
            bindings.find(Key{std::string(parent_version_id), std::string(binding_name)});
        return found == bindings.end() ? std::nullopt
                                       : std::optional<ProgramRuntimeChildBinding>{found->second};
    }

    void clear() {
        std::lock_guard lock(mutex);
        bindings.clear();
    }

private:
    mutable std::mutex                        mutex;
    std::map<Key, ProgramRuntimeChildBinding> bindings;
};

// The runtime, stores, catalog, policy and child bindings a Program test needs. `Registry`
// supplies the executables of the including test; the rest is shared by every suite.
template <RegistrySnapshot (*Registry)(ExecutionGuarantee)>
struct BasicAdmittedRuntime {
    RegistrySnapshot                                registry;
    AdmissionProfile                                profile;
    PolicySnapshot                                  policy;
    test_support::ProgramStoreHandle                store;
    std::shared_ptr<EngineGenerationCache>          engines;
    std::shared_ptr<ProgramCatalog>                 catalog;
    std::shared_ptr<CheckpointStore>                checkpoints;
    test_support::TransitionStoreHandle             journal;
    std::shared_ptr<ChildBindingRegistry>           child_bindings;
    test_support::FragmentStoreHandle               task_graph_fragments;
    /// When set, the runtime uses this store (typically a wrapper around task_graph_fragments).
    /// Call make_runtime() again after setting it.
    FragmentStoreHandle         fragment_store_override;
    /// What the policy resolver returns for owner "tenant:runtime"; tests may edit it.
    std::shared_ptr<TaskGraphExpansionPolicy>       expansion = std::make_shared<TaskGraphExpansionPolicy>(expansion_policy());
    ProgramChildQuotaConfig                         child_quota;
    std::size_t                                     scheduler_thread_count;
    std::shared_ptr<HookRuntime>                    hook_runtime;
    ProgramCoreToolGrantResolver                    core_tool_grant_resolver;
    ProgramCoreProviderCallResolver                 core_provider_call_resolver;
    bool                                            require_core_provider_call_broker = false;
    /// RuntimeConfig::javascript_stack_bytes of the next make_runtime(); 0 is the default.
    std::size_t                                     javascript_stack_bytes = 0;
    std::unique_ptr<ProgramRuntime>                 runtime;

    explicit BasicAdmittedRuntime(std::size_t                             scheduler_threads  = 1,
                             std::shared_ptr<CheckpointStore>        checkpoint_backend = {},
                             std::shared_ptr<ProgramTransitionStore> journal_backend    = {},
                              ProgramChildQuotaConfig                 quota              = {},
                              ExecutionGuarantee minimum_guarantee = ExecutionGuarantee::Strict,
                              bool               allow_javascript  = false,
                              std::shared_ptr<HookRuntime> hooks = {})
        : registry(Registry(minimum_guarantee)),
          profile(make_profile(registry, minimum_guarantee, allow_javascript)),
          policy(make_policy(profile, minimum_guarantee, allow_javascript)),
          store(std::make_shared<InMemoryProgramStore>()),
          engines(std::make_shared<EngineGenerationCache>()),
          catalog(std::make_shared<ProgramCatalog>(
              CatalogConfig{store, registry, engines, "program-runtime-test/v1"})),
          checkpoints(checkpoint_backend ? std::move(checkpoint_backend)
                                         : std::make_shared<InMemoryCheckpointStore>()),
          journal(journal_backend ? std::move(journal_backend)
                                  : std::make_shared<InMemoryProgramTransitionStore>()),
          child_bindings(std::make_shared<ChildBindingRegistry>()),
          task_graph_fragments(std::make_shared<InMemoryTaskGraphFragmentStore>()),
          child_quota(quota),
           scheduler_thread_count(scheduler_threads),
           hook_runtime(std::move(hooks)),
           runtime(make_runtime()) {}

    static AdmissionProfile make_profile(
        const RegistrySnapshot& registry,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        AdmissionProfileBuilder builder;
        builder.id("runtime-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(PROGRAM_SCHEMA_VERSION_V4)
            .minimum_execution_guarantee(minimum_guarantee)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        if (allow_javascript) builder.allow_source_kind(SourceKind::JavaScript);
        for (const auto& identity : registry.identities())
            builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(
        const AdmissionProfile& profile,
        ExecutionGuarantee      minimum_guarantee = ExecutionGuarantee::Strict,
        bool                    allow_javascript  = false) {
        const auto            budget_ceiling = allow_javascript
                                                   ? BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32}
                                                   : BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32};
        PolicySnapshotBuilder builder;
        builder.id("runtime-policy")
            .semantic_version("1.0.0")
            .owner_scope("tenant:runtime")
            .admission_profile(profile)
            .budget_ceiling(budget_ceiling)
            .minimum_execution_guarantee(minimum_guarantee);
        return std::move(builder).build();
    }
    std::unique_ptr<ProgramRuntime> make_runtime_with_transitions(
        std::shared_ptr<ProgramTransitionStore> transitions,
        ProgramRuntimeRecoveryHandler recovery_handler = {}) const {
        RuntimeConfig config{catalog,
                              checkpoints,
                              {},
                              std::move(transitions),
                             scheduler_thread_count,
                             [bindings = child_bindings](std::string_view owner_scope,
                                                         std::string_view parent_version_id,
                                                         std::string_view binding_name) {
                                 return bindings->resolve(owner_scope, parent_version_id,
                                                          binding_name);
                             }};
        config.task_graph_fragments =
            fragment_store_override ? fragment_store_override : task_graph_fragments;
        config.task_graph_policy_resolver =
            [policy = expansion](std::string_view owner_scope, std::string_view,
                                 std::string_view) -> std::optional<TaskGraphExpansionPolicy> {
            if (owner_scope != "tenant:runtime") return std::nullopt;
            return *policy;
        };
        config.child_quota = child_quota;
        config.hook_runtime = hook_runtime;
        config.core_tool_grant_resolver = core_tool_grant_resolver;
        config.core_provider_call_resolver = core_provider_call_resolver;
        config.require_core_provider_call_broker = require_core_provider_call_broker;
        config.runtime_recovery_handler = std::move(recovery_handler);
        config.javascript_stack_bytes = javascript_stack_bytes;
        return std::make_unique<ProgramRuntime>(std::move(config));
    }
    std::unique_ptr<ProgramRuntime> make_runtime() const {
        return make_runtime_with_transitions(journal);
    }

    ProgramVersion admit(std::string node_type) {
        return admit_document(program_document(std::move(node_type)));
    }

    ProgramVersion admit_document(json document) {
        const auto schema_version = document.contains("program_schema_version") &&
                                            document["program_schema_version"].is_number_unsigned()
                                        ? document["program_schema_version"].get<std::uint32_t>()
                                        : PROGRAM_SCHEMA_VERSION_V1;
        return admit_source(
            ProgramSource::from_cpp_builder("test:runtime", schema_version, std::move(document)));
    }

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
    ProgramVersion admit_javascript(std::string                   source,
                                    std::optional<ContractRecord> output_contract = std::nullopt) {
        return admit_source(
            ProgramSource::from_javascript("test:runtime-control.js", std::move(source)),
            std::move(output_contract));
    }
#endif

    ProgramVersion admit_source(ProgramSource                 source,
                                std::optional<ContractRecord> output_contract = std::nullopt) {
        ProgramCompiler              compiler(registry, {"program-runtime-test/v1"});
        std::optional<ProgramBundle> bundle;
        try {
            if (output_contract) {
                bundle = compiler.compile(source, RunBudget{10000, 1000, 1000, 1, 32, 20, 0, 0, 0},
                                          ContractRecord{}, *output_contract);
            } else {
                bundle = compiler.compile(source);
            }
        } catch (const ProgramCompileError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
        try {
            return catalog->admit(*bundle, ProgramAdmission{"tenant:runtime", profile, policy, {}});
        } catch (const ProgramAdmissionError& error) {
            std::string message = error.what();
            for (const auto& diagnostic : error.diagnostics()) {
                message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer + ": " +
                           diagnostic.message + " " + diagnostic.witness.dump();
            }
            throw std::runtime_error(message);
        }
    }

    void bind_child(const ProgramVersion&      parent,
                    std::string                binding_name,
                    ProgramRuntimeChildBinding binding) {
        child_bindings->bind(parent.id(), std::move(binding_name), std::move(binding));
    }

    void clear_child_bindings() { child_bindings->clear(); }

    void recreate_catalog_and_runtime() {
        runtime.reset();
        catalog.reset();
        engines = std::make_shared<EngineGenerationCache>();
        catalog = std::make_shared<ProgramCatalog>(
            CatalogConfig{store, registry, engines, "program-runtime-test/v1"});
        runtime = make_runtime();
    }
};


struct LinkedChildAdmission {
    ProgramVersion    parent_version;
    ProgramVersion    child_version;
    ModuleLinkReceipt receipt;
};

template <class Fixture>
LinkedChildAdmission link_child_versions(
    Fixture&           fixture,
    ProgramVersion     parent_version,
    ProgramVersion     child_version,
    BudgetLimits       child_budget       = BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
    ExecutionGuarantee accepted_guarantee = ExecutionGuarantee::Strict) {
    const auto child_bundle = fixture.store->get_bundle(child_version.bundle_id());
    if (!child_bundle) throw std::runtime_error("child bundle was not admitted");

    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:runtime";
    module_data.coordinate     = ModuleCoordinate{"runtime", "parent", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    module_data.children.push_back(
        ChildProgramDescriptor{"child",
                               child_version.id(),
                               {ModulePort{"input", child_bundle->input_contract()}},
                               {ModulePort{"output", child_bundle->output_contract()}},
                               child_bundle->capability_effect_closure().capabilities,
                               child_bundle->capability_effect_closure().effects,
                               std::move(child_budget),
                               accepted_guarantee});
    module_data.allowed_capabilities = child_bundle->capability_effect_closure().capabilities;
    module_data.declared_effects     = child_bundle->capability_effect_closure().effects;
    const auto       parent_module   = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = parent_module.coordinate();
    resolution.modules.push_back(parent_module);
    auto receipt =
        link_module_child(resolution, parent_module, "child", *child_bundle, child_version);
    fixture.bind_child(parent_version, "child", ProgramRuntimeChildBinding{receipt, child_version});
    return LinkedChildAdmission{std::move(parent_version), std::move(child_version),
                                std::move(receipt)};
}


/// Links several named children of one parent Program. `children` maps the binding name a
/// template contract uses (`child_binding`) to the admitted child version.
template <class Fixture>
void link_children(Fixture& fixture, const ProgramVersion& parent_version,
                   const std::vector<std::pair<std::string, ProgramVersion>>& children,
                   BudgetLimits child_budget = BudgetLimits{10000, 1000, 1000, 1, 1, 20, 0, 1, 1},
                   ExecutionGuarantee accepted_guarantee = ExecutionGuarantee::Strict) {
    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:runtime";
    module_data.coordinate     = ModuleCoordinate{"runtime", "parent", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    std::vector<ProgramBundle> bundles;
    for (const auto& [name, version] : children) {
        const auto bundle = fixture.store->get_bundle(version.bundle_id());
        if (!bundle) throw std::runtime_error("child bundle was not admitted");
        bundles.push_back(*bundle);
        module_data.children.push_back(
            ChildProgramDescriptor{name,
                                   version.id(),
                                   {ModulePort{"input", bundle->input_contract()}},
                                   {ModulePort{"output", bundle->output_contract()}},
                                   bundle->capability_effect_closure().capabilities,
                                   bundle->capability_effect_closure().effects,
                                   child_budget,
                                   accepted_guarantee});
        for (const auto& capability : bundle->capability_effect_closure().capabilities)
            if (std::find(module_data.allowed_capabilities.begin(),
                          module_data.allowed_capabilities.end(),
                          capability) == module_data.allowed_capabilities.end())
                module_data.allowed_capabilities.push_back(capability);
        for (const auto& effect : bundle->capability_effect_closure().effects)
            if (std::find(module_data.declared_effects.begin(), module_data.declared_effects.end(),
                          effect) == module_data.declared_effects.end())
                module_data.declared_effects.push_back(effect);
    }
    const auto       parent_module = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = parent_module.coordinate();
    resolution.modules.push_back(parent_module);
    for (std::size_t index = 0; index < children.size(); ++index) {
        auto receipt = link_module_child(resolution, parent_module, children[index].first,
                                         bundles[index], children[index].second);
        fixture.bind_child(parent_version, children[index].first,
                           ProgramRuntimeChildBinding{receipt, children[index].second});
    }
}

}  // namespace neograph::program_test
