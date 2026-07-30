#include <neograph/program/program.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main() {
    using namespace neograph::program;
    const auto hash = [](char digit) { return "sha256:" + std::string(64, digit); };

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
    bundle_data.registry_snapshot_fingerprint = hash('b');
    bundle_data.module_dependency_merkle_root = hash('c');
    bundle_data.input_contract                = ContractRecord{1, neograph::json::object()};
    bundle_data.output_contract               = ContractRecord{1, neograph::json::object()};
    bundle_data.orchestration_plan      = OrchestrationPlanRecord{1, neograph::json::object()};
    bundle_data.sealed_core_definitions = {SealedCoreDefinition{
        "main", sealed_core_definition_hash(core_definition), core_definition}};
    bundle_data.core_plan_identities    = {CorePlanIdentity{"main", hash('e')}};
    bundle_data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Node, "installed-node", "1.0.0", hash('f')}};
    bundle_data.declared_budget_requirements = {BudgetRequirement{"steps", 1, 10}};
    bundle_data.capability_effect_closure =
        CapabilityEffectClosure{{"installed-capability"}, {"installed-effect"}};
    bundle_data.source_map  = {mapping};
    bundle_data.diagnostics = {diagnostic};
    ProgramBundle bundle(std::move(bundle_data));
    const auto    bundle_round_trip = ProgramBundle::parse(bundle.serialize_canonical());

    ProgramVersionData version_data;
    version_data.bundle_id           = bundle.id();
    version_data.admission_profile   = AdmissionProfileBinding{"installed-profile", hash('1')};
    version_data.policy_snapshot     = PolicySnapshotBinding{"installed-policy", hash('2')};
    version_data.ownership_scope     = "installed-consumer";
    version_data.dependency_receipts = {DependencyReceipt{"module:installed", hash('9')}};
    version_data.core_materialization_receipt = CoreMaterializationReceipt{
        "installed-consumer", hash('b'), {CorePlanIdentity{"main", hash('e')}}};
    ProgramVersion version(std::move(version_data));
    const auto     version_round_trip = ProgramVersion::parse(version.serialize_canonical());
    const bool     enum_round_trips =
        source_kind_from_string(to_string(source.kind())) == source.kind() &&
        compile_phase_from_string(to_string(diagnostic.phase)) == diagnostic.phase &&
        diagnostic_severity_from_string(to_string(diagnostic.severity)) == diagnostic.severity &&
        executable_kind_from_string(to_string(ExecutableKind::Node)) == ExecutableKind::Node;
    const bool ok =
        source_round_trip.source_hash() == source.source_hash() &&
        source_round_trip.imports().size() == 1 && source_round_trip.source_map().size() == 1 &&
        import_round_trip == import && mapping_round_trip == mapping &&
        diagnostic_round_trip.code == diagnostic.code &&
        diagnostic_round_trip.primary == diagnostic.primary &&
        bundle_round_trip.id() == bundle.id() && bundle_round_trip.diagnostics().size() == 1 &&
        bundle_round_trip.diagnostics().front().code == diagnostic.code &&
        bundle_round_trip.capability_effect_closure().capabilities.size() == 1 &&
        version_round_trip.id() == version.id() &&
        version_round_trip.dependency_receipts().size() == 1 && enum_round_trips;
    std::cout << source.source_hash() << '\n' << bundle.id() << '\n' << version.id() << '\n';
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
