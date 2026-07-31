#include <neograph/program/program.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace neograph::program;
std::atomic<unsigned> node_calls{0};
std::atomic<unsigned> interrupt_calls{0};

std::string hash(char digit) {
    return "sha256:" + std::string(64, digit);
}

ExecutableManifest manifest(ExecutableKind kind, std::string name, char digit) {
    ExecutableManifest result;
    result.identity       = {kind, std::move(name), "1.0.0", hash(digit)};
    result.effect_mode    = EffectMode::Brokered;
    result.attestation_id = "attestation:installed";
    return result;
}

class InstalledNode final : public neograph::graph::GraphNode {
public:
    explicit InstalledNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput) override {
        ++node_calls;
        co_return neograph::graph::NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class InstalledInterruptNode final : public neograph::graph::GraphNode {
public:
    explicit InstalledInterruptNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput input) override {
        ++interrupt_calls;
        if (!input.ctx.resume_value) {
            throw neograph::graph::NodeInterrupt("installed approval");
        }
        co_return neograph::graph::NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class CountingSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent&) override { ++calls; }

    std::atomic<unsigned> calls{0};
};

RegistrySnapshot build_registry(bool reverse) {
    RegistrySnapshotBuilder builder;
    const auto              add_node = [&] {
        builder.add_node(
            manifest(ExecutableKind::Node, "installed-node", '1'),
            [](const std::string& name, const neograph::json&,
               const neograph::graph::NodeContext&) {
                return std::make_unique<InstalledNode>(name);
            },
            neograph::json{{"type", "object"}, {"additionalProperties", false}},
            neograph::json{{"writes", neograph::json::array()}});
    };
    const auto add_interrupt = [&] {
        builder.add_node(
            manifest(ExecutableKind::Node, "installed-interrupt", '6'),
            [](const std::string& name, const neograph::json&,
               const neograph::graph::NodeContext&) {
                return std::make_unique<InstalledInterruptNode>(name);
            },
            neograph::json{{"type", "object"}, {"additionalProperties", false}},
            neograph::json{{"writes", neograph::json::array()}});
    };
    const auto add_reducer = [&] {
        builder.add_reducer(manifest(ExecutableKind::Reducer, "installed-reducer", '2'),
                            [](const neograph::json&, const neograph::json& incoming) {
                                return neograph::json(incoming);
                            });
    };
    const auto add_condition = [&] {
        builder.add_condition(
            manifest(ExecutableKind::Condition, "installed-condition", '3'),
            [](const neograph::graph::GraphState&) { return std::string("yes"); },
            neograph::graph::ConditionSpec{{"yes", "no"}, false});
    };
    const auto add_provider = [&] {
        builder.add_provider(manifest(ExecutableKind::Provider, "installed-provider", '4'),
                             ProviderMetadata{neograph::json::object(), neograph::json::object()});
    };
    const auto add_tool = [&] {
        builder.add_tool(manifest(ExecutableKind::Tool, "installed-tool", '5'),
                         ToolMetadata{neograph::json::object(), neograph::json::object()});
    };

    if (reverse) {
        add_tool();
        add_provider();
        add_condition();
        add_reducer();
        add_interrupt();
        add_node();
    } else {
        add_node();
        add_interrupt();
        add_reducer();
        add_condition();
        add_provider();
        add_tool();
    }
    return std::move(builder).build();
}

neograph::json program_document(std::string node_type = "installed-node") {
    const neograph::json definition{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", neograph::json{{"value", neograph::json{{"reducer", "installed-reducer"},
                                                             {"initial", 0}}}}},
        {"nodes", neograph::json{{"main", neograph::json{{"type", std::move(node_type)}}}}},
        {"edges", neograph::json::array({neograph::json{{"from", "__start__"}, {"to", "main"}},
                                         neograph::json{{"from", "main"}, {"to", "__end__"}}})},
        {"conditional_edges", neograph::json::array()}};

    return {
        {"program_schema_version", 1},
        {"input_contract",
         neograph::json{{"schema_version", 1}, {"schema", neograph::json::object()}}},
        {"output_contract",
         neograph::json{{"schema_version", 1}, {"schema", neograph::json::object()}}},
        {"root", neograph::json{{"op", "call_core"}, {"name", "main"}, {"definition", definition}}},
        {"declared_budget_requirements",
         neograph::json::array({
             neograph::json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 60000}},
             neograph::json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
             neograph::json{
                 {"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000}},
             neograph::json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
             neograph::json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
             neograph::json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 100}},
             neograph::json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             neograph::json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
             neograph::json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
         })},
    };
}

}  // namespace

int main() {
    using namespace neograph::program;

    const auto registry           = build_registry(false);
    const auto reordered_registry = build_registry(true);
    if (registry.fingerprint() != reordered_registry.fingerprint() ||
        registry.serialize_canonical() != reordered_registry.serialize_canonical()) {
        return EXIT_FAILURE;
    }

    const auto compiled_source =
        ProgramSource::from_cpp_builder("installed-compiler-consumer", 1, program_document());
    ProgramCompiler compiler(registry, ProgramCompilerConfig{"installed-consumer/v1"});
    ProgramCompiler reordered_compiler(reordered_registry,
                                       ProgramCompilerConfig{"installed-consumer/v1"});
    const auto      compiled_bundle        = compiler.compile(compiled_source);
    const auto      reordered_bundle       = reordered_compiler.compile(compiled_source);
    const auto      compiled_bytes         = compiled_bundle.serialize_canonical();
    const auto      parsed_compiled_bundle = ProgramBundle::parse(compiled_bytes);

    const bool compiler_deterministic =
        compiler.registry_snapshot_fingerprint() == registry.fingerprint() &&
        reordered_compiler.registry_snapshot_fingerprint() == registry.fingerprint() &&
        compiled_bundle.id() == reordered_bundle.id() &&
        compiled_bundle.canonical_program_hash() == reordered_bundle.canonical_program_hash() &&
        compiled_bundle.core_plan_identities() == reordered_bundle.core_plan_identities() &&
        compiled_bytes == reordered_bundle.serialize_canonical();
    const bool compiler_round_trip =
        parsed_compiled_bundle.id() == compiled_bundle.id() &&
        parsed_compiled_bundle.canonical_program_hash() ==
            compiled_bundle.canonical_program_hash() &&
        parsed_compiled_bundle.registry_snapshot_fingerprint() ==
            compiled_bundle.registry_snapshot_fingerprint() &&
        parsed_compiled_bundle.core_plan_identities() == compiled_bundle.core_plan_identities() &&
        parsed_compiled_bundle.serialize_canonical() == compiled_bytes;
    const bool compiler_complete = !compiled_bundle.id().empty() &&
                                   compiled_bundle.compiler_build_id() == "installed-consumer/v1" &&
                                   compiled_bundle.sealed_core_definitions().size() == 1 &&
                                   compiled_bundle.core_plan_identities().size() == 1 &&
                                   compiled_bundle.executable_registry_identities().size() == 2 &&
                                   compiled_bundle.declared_budget_requirements().size() == 9;

    AdmissionProfileBuilder admission_builder;
    admission_builder.id("installed-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_executable(
            ExecutableIdentity{ExecutableKind::Node, "installed-node", "1.0.0", hash('1')})
        .allow_executable(
            ExecutableIdentity{ExecutableKind::Node, "installed-interrupt", "1.0.0", hash('6')})
        .allow_executable(
            ExecutableIdentity{ExecutableKind::Reducer, "installed-reducer", "1.0.0", hash('2')})
        .allow_effect_mode(EffectMode::Brokered);
    const auto admission            = std::move(admission_builder).build();
    const auto admission_round_trip = AdmissionProfile::parse(admission.serialize_canonical());

    PolicySnapshotBuilder policy_builder;
    policy_builder.id("installed-policy")
        .semantic_version("1.0.0")
        .owner_scope("installed-consumer")
        .admission_profile(admission)
        .allow_capability("installed-capability")
        .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 1, 1, 100, 1, 1, 1});
    const auto policy            = std::move(policy_builder).build();
    const auto policy_round_trip = PolicySnapshot::parse(policy.serialize_canonical());

    const SourceSpan       span{0, 2, 1, 1, 1, 3};
    const SourceCoordinate authored{"installed-consumer", "/steps/0", span};
    const ImportRef        import{"module:installed", hash('9')};
    const SourceMapEntry   mapping{"/steps/0", authored};
    auto                   stored_source = ProgramSource::from_canonical_json(
        "installed-consumer", R"({"program_schema_version":1,"steps":[]})", {import}, {mapping});
    const auto source_round_trip = ProgramSource::parse(stored_source.serialize_canonical());

    Diagnostic diagnostic;
    diagnostic.phase                = CompilePhase::Seal;
    diagnostic.code                 = "P_INSTALLED_CONSUMER";
    diagnostic.severity             = DiagnosticSeverity::Note;
    diagnostic.primary.source_id    = stored_source.source_id();
    diagnostic.primary.json_pointer = "";
    diagnostic.message              = "installed consumer diagnostic";
    diagnostic.witness              = neograph::json::object();

    neograph::json encoded_diagnostic;
    to_json(encoded_diagnostic, diagnostic);
    Diagnostic diagnostic_round_trip;
    from_json(encoded_diagnostic, diagnostic_round_trip);
    neograph::json encoded_import;
    to_json(encoded_import, import);
    ImportRef import_round_trip;
    from_json(encoded_import, import_round_trip);
    neograph::json encoded_mapping;
    to_json(encoded_mapping, mapping);
    SourceMapEntry mapping_round_trip;
    from_json(encoded_mapping, mapping_round_trip);

    const neograph::json core_definition{
        {"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
        {"name", "manual"},
        {"nodes", neograph::json{{"main", neograph::json{{"type", "installed-node"}}}}}};
    const std::vector<ExecutableIdentity> executable_closure{
        ExecutableIdentity{ExecutableKind::Reducer, "installed-reducer", "1.0.0", hash('2')}};
    const SealedCoreDefinition sealed_definition{
        "manual", sealed_core_definition_hash(core_definition), core_definition};
    const CorePlanIdentity manual_plan{
        "manual", core_compiled_plan_identity(sealed_definition, "installed-consumer-manual/v1",
                                              registry.fingerprint(), executable_closure)};

    ProgramBundleData bundle_data;
    bundle_data.source_kind                   = stored_source.kind();
    bundle_data.source_hash                   = stored_source.source_hash();
    bundle_data.canonical_program_hash        = hash('a');
    bundle_data.compiler_build_id             = "installed-consumer-manual/v1";
    bundle_data.program_schema_version        = 1;
    bundle_data.registry_snapshot_fingerprint = registry.fingerprint();
    bundle_data.module_dependency_merkle_root = hash('c');
    bundle_data.input_contract                = ContractRecord{1, neograph::json::object()};
    bundle_data.output_contract               = ContractRecord{1, neograph::json::object()};
    bundle_data.orchestration_plan      = OrchestrationPlanRecord{1, neograph::json::object()};
    bundle_data.sealed_core_definitions = {sealed_definition};
    bundle_data.core_plan_identities    = {manual_plan};
    bundle_data.executable_registry_identities = executable_closure;
    bundle_data.declared_budget_requirements   = {BudgetRequirement{"steps", 1, 10}};
    bundle_data.capability_effect_closure =
        CapabilityEffectClosure{{"installed-capability"}, {"installed-effect"}};
    bundle_data.source_map  = {mapping};
    bundle_data.diagnostics = {diagnostic};
    ProgramBundle manual_bundle(std::move(bundle_data));
    const auto    bundle_round_trip = ProgramBundle::parse(manual_bundle.serialize_canonical());

    ProgramVersionData version_data(
        manual_bundle.id(), admission, policy, {DependencyReceipt{"module:installed", hash('9')}},
        policy.owner_scope(),
        CoreMaterializationReceipt{"installed-consumer", registry.fingerprint(), {manual_plan}});
    ProgramVersion version(std::move(version_data));
    const auto     version_round_trip = ProgramVersion::parse(version.serialize_canonical());

    const bool enum_round_trips =
        source_kind_from_string(to_string(stored_source.kind())) == stored_source.kind() &&
        compile_phase_from_string(to_string(diagnostic.phase)) == diagnostic.phase &&
        diagnostic_severity_from_string(to_string(diagnostic.severity)) == diagnostic.severity &&
        executable_kind_from_string(to_string(ExecutableKind::Imported)) ==
            ExecutableKind::Imported;
    const bool prior_contracts =
        source_round_trip.source_hash() == stored_source.source_hash() &&
        source_round_trip.imports().size() == 1 && source_round_trip.source_map().size() == 1 &&
        import_round_trip == import && mapping_round_trip == mapping &&
        diagnostic_round_trip.code == diagnostic.code &&
        diagnostic_round_trip.primary == diagnostic.primary &&
        bundle_round_trip.id() == manual_bundle.id() &&
        bundle_round_trip.diagnostics().size() == 1 &&
        bundle_round_trip.diagnostics().front().code == diagnostic.code &&
        bundle_round_trip.capability_effect_closure().capabilities.size() == 1 &&
        admission_round_trip.fingerprint() == admission.fingerprint() &&
        policy_round_trip.fingerprint() == policy.fingerprint() &&
        policy.registry_fingerprint() == registry.fingerprint() &&
        version_round_trip.id() == version.id() &&
        version_round_trip.admission_profile().fingerprint() == admission.fingerprint() &&
        version_round_trip.policy_snapshot().fingerprint() == policy.fingerprint() &&
        version_round_trip.dependency_receipts().size() == 1 && enum_round_trips;

    const auto interrupt_source = ProgramSource::from_cpp_builder(
        "installed-runtime-interrupt", 1, program_document("installed-interrupt"));
    const auto interrupt_bundle = compiler.compile(interrupt_source);
    auto       program_store    = std::make_shared<InMemoryProgramStore>();
    auto       engine_cache     = std::make_shared<EngineGenerationCache>();
    auto       catalog          = std::make_shared<ProgramCatalog>(
        CatalogConfig{program_store, registry, engine_cache, "installed-consumer/v1"});
    const auto admitted = catalog->admit(
        compiled_bundle, ProgramAdmission{"installed-consumer", admission, policy, {}});
    const auto admitted_interrupt = catalog->admit(
        interrupt_bundle, ProgramAdmission{"installed-consumer", admission, policy, {}});
    auto            checkpoints = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    auto            journal     = std::make_shared<InMemoryProgramJournal>();
    auto            sink        = std::make_shared<CountingSink>();
    const RunBudget runtime_budget{1000, 1000, 1000, 1, 1, 100, 0, 0, 0};
    bool            runtime_contract             = false;
    unsigned        callbacks_before_destruction = 0;
    {
        ProgramRuntime runtime(RuntimeConfig{catalog, checkpoints, {}, journal, 2});
        const auto     completed = runtime.run(
            admitted,
            ProgramInvocation{neograph::json::object(), runtime_budget, "installed-sync", sink});
        const auto interrupted =
            runtime
                .start(admitted_interrupt,
                       ProgramInvocation{neograph::json::object(), runtime_budget,
                                         "installed-interrupt", sink})
                .wait();

        asio::io_context io;
        auto             resumed_future = asio::co_spawn(
            io,
            runtime.resume_async(
                interrupted.run_id(),
                ProgramResume{neograph::json{{"decision", "approved"}}, "installed-resume", sink}),
            asio::use_future);
        io.run();
        const auto resumed           = resumed_future.get();
        const auto completed_journal = journal->latest(completed.run_id());
        const auto resumed_journal   = journal->latest(resumed.run_id());

        runtime_contract =
            completed.status() == ProgramTerminalStatus::Completed &&
            interrupted.status() == ProgramTerminalStatus::Interrupted &&
            resumed.status() == ProgramTerminalStatus::Completed &&
            completed.program_version_id() == admitted.id() &&
            resumed.program_version_id() == admitted_interrupt.id() &&
            completed.bundle_id() == compiled_bundle.id() &&
            resumed.bundle_id() == interrupt_bundle.id() && completed.checkpoint().has_value() &&
            interrupted.checkpoint().has_value() && resumed.checkpoint().has_value() &&
            interrupted.checkpoint()->core_thread_id == resumed.checkpoint()->core_thread_id &&
            completed_journal.has_value() &&
            completed_journal->continuation.state == ContinuationState::Completed &&
            resumed_journal.has_value() &&
            resumed_journal->continuation.state == ContinuationState::Completed &&
            resumed_journal->continuation.attempt == 2 && node_calls.load() == 1 &&
            interrupt_calls.load() == 2;
        callbacks_before_destruction = sink->calls.load();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    runtime_contract = runtime_contract && sink->calls.load() == callbacks_before_destruction;

    if (!compiler_deterministic || !compiler_round_trip || !compiler_complete || !prior_contracts ||
        !runtime_contract) {
        return EXIT_FAILURE;
    }
    std::cout << compiled_bundle.id() << '\n'
              << compiled_bundle.canonical_program_hash() << '\n'
              << compiled_bundle.core_plan_identities().front().compiled_plan_identity << '\n'
              << version.id() << '\n';
    return EXIT_SUCCESS;
}
