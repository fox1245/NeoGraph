#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neograph::program;

std::string hash(char digit) {
    return "sha256:" + std::string(64, digit);
}

ExecutableManifest manifest(ExecutableKind kind, std::string name, char digit) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", hash(digit)},
                              EffectMode::Brokered,
                              "attestation:installed",
                              {},
                              {}};
}

class InstalledNode final : public neograph::graph::GraphNode {
public:
    explicit InstalledNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput) override {
        co_return neograph::graph::NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
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
            neograph::json{{"type", "object"}},
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
        add_node();
    } else {
        add_node();
        add_reducer();
        add_condition();
        add_provider();
        add_tool();
    }
    return std::move(builder).build();
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

    AdmissionProfileBuilder admission_builder;
    admission_builder.id("installed-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
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
        .allow_effect("installed-effect")
        .budget_ceiling(BudgetLimits{100, 100, 100, 1, 100, 100, 1, 1, 1});
    const auto policy            = std::move(policy_builder).build();
    const auto policy_round_trip = PolicySnapshot::parse(policy.serialize_canonical());

    const SourceSpan       span{0, 2, 1, 1, 1, 3};
    const SourceCoordinate authored{"installed-consumer", "/steps/0", span};
    const ImportRef        import{"module:installed", hash('9')};
    const SourceMapEntry   mapping{"/steps/0", authored};
    auto                   source = ProgramSource::from_canonical_json(
        "installed-consumer", R"({"program_schema_version":1,"steps":[]})", {import}, {mapping});
    const auto source_round_trip = ProgramSource::parse(source.serialize_canonical());

    Diagnostic diagnostic;
    diagnostic.phase                = CompilePhase::Seal;
    diagnostic.code                 = "P_INSTALLED_CONSUMER";
    diagnostic.severity             = DiagnosticSeverity::Note;
    diagnostic.primary.source_id    = source.source_id();
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
        {"nodes", neograph::json{{"main", neograph::json{{"type", "installed-node"}}}}}};

    ProgramBundleData bundle_data;
    bundle_data.source_hash                   = source.source_hash();
    bundle_data.canonical_program_hash        = hash('a');
    bundle_data.compiler_build_id             = "installed-consumer";
    bundle_data.program_schema_version        = 1;
    bundle_data.registry_snapshot_fingerprint = registry.fingerprint();
    bundle_data.module_dependency_merkle_root = hash('c');
    bundle_data.input_contract                = ContractRecord{1, neograph::json::object()};
    bundle_data.output_contract               = ContractRecord{1, neograph::json::object()};
    bundle_data.orchestration_plan      = OrchestrationPlanRecord{1, neograph::json::object()};
    bundle_data.sealed_core_definitions = {SealedCoreDefinition{
        "main", sealed_core_definition_hash(core_definition), core_definition}};
    bundle_data.core_plan_identities    = {CorePlanIdentity{"main", hash('e')}};
    bundle_data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Reducer, "installed-reducer", "1.0.0", hash('2')}};
    bundle_data.declared_budget_requirements = {BudgetRequirement{"steps", 1, 10}};
    bundle_data.capability_effect_closure =
        CapabilityEffectClosure{{"installed-capability"}, {"installed-effect"}};
    bundle_data.source_map  = {mapping};
    bundle_data.diagnostics = {diagnostic};
    ProgramBundle bundle(std::move(bundle_data));
    const auto    bundle_round_trip = ProgramBundle::parse(bundle.serialize_canonical());

    ProgramVersionData version_data(
        bundle.id(), admission, policy, {DependencyReceipt{"module:installed", hash('9')}},
        policy.owner_scope(),
        CoreMaterializationReceipt{
            "installed-consumer", registry.fingerprint(), {CorePlanIdentity{"main", hash('e')}}});
    ProgramVersion version(std::move(version_data));
    const auto     version_round_trip = ProgramVersion::parse(version.serialize_canonical());
    const bool     enum_round_trips =
        source_kind_from_string(to_string(source.kind())) == source.kind() &&
        compile_phase_from_string(to_string(diagnostic.phase)) == diagnostic.phase &&
        diagnostic_severity_from_string(to_string(diagnostic.severity)) == diagnostic.severity &&
        executable_kind_from_string(to_string(ExecutableKind::Imported)) ==
            ExecutableKind::Imported;
    const bool ok =
        source_round_trip.source_hash() == source.source_hash() &&
        source_round_trip.imports().size() == 1 && source_round_trip.source_map().size() == 1 &&
        import_round_trip == import && mapping_round_trip == mapping &&
        diagnostic_round_trip.code == diagnostic.code &&
        diagnostic_round_trip.primary == diagnostic.primary &&
        bundle_round_trip.id() == bundle.id() && bundle_round_trip.diagnostics().size() == 1 &&
        bundle_round_trip.diagnostics().front().code == diagnostic.code &&
        bundle_round_trip.capability_effect_closure().capabilities.size() == 1 &&
        admission_round_trip.fingerprint() == admission.fingerprint() &&
        policy_round_trip.fingerprint() == policy.fingerprint() &&
        policy.registry_fingerprint() == registry.fingerprint() &&
        version_round_trip.id() == version.id() &&
        version_round_trip.admission_profile().fingerprint() == admission.fingerprint() &&
        version_round_trip.policy_snapshot().fingerprint() == policy.fingerprint() &&
        version_round_trip.dependency_receipts().size() == 1 && enum_round_trips;
    std::cout << source.source_hash() << '\n' << bundle.id() << '\n' << version.id() << '\n';
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
