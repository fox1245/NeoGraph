#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::program;

std::string sha(char digit) {
    return "sha256:" + std::string(64, digit);
}

SourceCoordinate coordinate(std::string pointer = "/nodes/a") {
    SourceCoordinate value;
    value.source_id    = "source.json";
    value.json_pointer = std::move(pointer);
    value.span         = SourceSpan{1, 4, 1, 2, 1, 5};
    return value;
}

Diagnostic diagnostic() {
    Diagnostic value;
    value.phase    = CompilePhase::CoreValidate;
    value.code     = "P_CORE_WARNING";
    value.severity = DiagnosticSeverity::Warning;
    value.primary  = coordinate("/nodes/a~1b/~0name");
    value.message  = "Core graph warning";
    value.witness  = json{{"node", "a"}};
    value.related.push_back(coordinate("/nodes/b"));
    return value;
}

SealedCoreDefinition sealed_definition(std::string name, json definition) {
    const auto definition_hash = sealed_core_definition_hash(definition);
    return SealedCoreDefinition{std::move(name), definition_hash, std::move(definition)};
}

ProgramSource make_source() {
    return ProgramSource::from_canonical_json(
        "source.json", R"({"program_schema_version":1,"nodes":{},"edges":[]})");
}

ProgramBundleData make_bundle_data(const ProgramSource& source) {
    ProgramBundleData data;
    data.source_kind                   = source.kind();
    data.source_hash                   = source.source_hash();
    data.canonical_program_hash        = sha('a');
    data.compiler_build_id             = "compiler:test";
    data.program_schema_version        = source.schema_version();
    data.registry_snapshot_fingerprint = sha('b');
    data.module_dependency_merkle_root = sha('c');
    data.input_contract                = ContractRecord{1, json{{"type", "object"}}};
    data.output_contract               = ContractRecord{1, json{{"type", "object"}}};
    data.orchestration_plan            = OrchestrationPlanRecord{1, json{{"entry", "alpha"}}};
    data.sealed_core_definitions       = {
        sealed_definition("beta",
                                json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                                     {"nodes", json{{"beta", json{{"type", "test-node"}}}}}}),
        sealed_definition("alpha",
                                json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                                     {"nodes", json{{"alpha", json{{"type", "test-node"}}}}}}),
    };
    data.core_plan_identities = {
        CorePlanIdentity{"beta", sha('f')},
        CorePlanIdentity{"alpha", sha('1')},
    };
    data.capability_effect_closure.capabilities = {"write", "read"};
    data.capability_effect_closure.effects      = {"tool", "state"};
    data.executable_registry_identities         = {
        ExecutableIdentity{ExecutableKind::Tool, "zeta", "2.1.0-rc.1+build.7", sha('2')},
        ExecutableIdentity{ExecutableKind::Node, "alpha", "1.0.0", sha('3')},
    };
    data.declared_budget_requirements = {
        BudgetRequirement{"steps", 1, 100},
        BudgetRequirement{"parallelism", 1, 4},
    };
    data.source_map = {
        SourceMapEntry{"/nodes/b", coordinate("/authored/b")},
        SourceMapEntry{"/nodes/a", coordinate("/authored/a")},
    };
    data.diagnostics = {diagnostic()};
    return data;
}

RegistrySnapshot make_registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_reducer(
        ExecutableManifest{{ExecutableKind::Reducer, "version-reducer", "1.0.0", sha('8')},
                           EffectMode::Brokered,
                           "attestation:test",
                           {},
                           {}},
        [](const json&, const json& incoming) { return json(incoming); });
    return std::move(builder).build();
}

AdmissionProfile make_admission_profile(const RegistrySnapshot& registry,
                                        std::string             id = "production") {
    AdmissionProfileBuilder builder;
    builder.id(std::move(id))
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CanonicalJson)
        .allow_effect_mode(EffectMode::Brokered);
    return std::move(builder).build();
}

PolicySnapshot make_policy_snapshot(const AdmissionProfile& admission,
                                    std::string             id    = "policy-7",
                                    std::string             owner = "tenant:example") {
    PolicySnapshotBuilder builder;
    builder.id(std::move(id))
        .semantic_version("1.0.0")
        .owner_scope(std::move(owner))
        .admission_profile(admission)
        .budget_ceiling(BudgetLimits{1, 1, 1, 1, 1, 1, 1, 1, 1});
    return std::move(builder).build();
}

ProgramVersionData make_version_data(const ProgramBundle& bundle) {
    const auto registry  = make_registry_snapshot();
    auto       admission = make_admission_profile(registry);
    auto       policy    = make_policy_snapshot(admission);
    return ProgramVersionData(
        bundle.id(), admission, policy,
        {DependencyReceipt{"module:zeta", sha('6')}, DependencyReceipt{"module:alpha", sha('7')}},
        policy.owner_scope(),
        CoreMaterializationReceipt{
            "compiler:test",
            registry.fingerprint(),
            {CorePlanIdentity{"beta", sha('f')}, CorePlanIdentity{"alpha", sha('1')}}});
}

TEST(ProgramSourceTest, CanonicalIdentityIgnoresObjectKeyOrder) {
    auto first = ProgramSource::from_canonical_json(
        "source-a", R"({"program_schema_version":1,"nodes":{},"edges":[]})");
    auto second = ProgramSource::from_canonical_json(
        "source-b", R"({"edges":[],"nodes":{},"program_schema_version":1})");

    EXPECT_EQ(first.source_hash(), second.source_hash());
    EXPECT_EQ(first.canonical_document(), second.canonical_document());
    EXPECT_EQ(first.kind(), SourceKind::CanonicalJson);
}

TEST(ProgramSourceTest, JavaScriptSourcePinsEngineAndHostAbiInItsEnvelope) {
    const auto source = ProgramSource::from_javascript(
        "control.js", R"(neograph.define({ op: "call_core", name: "main" });)");

    EXPECT_EQ(source.kind(), SourceKind::JavaScript);
    EXPECT_EQ(source.document().at("language"), "javascript");
    EXPECT_EQ(source.document().at("engine"), "quickjs");
    EXPECT_EQ(source.document().at("engine_version"), "2026-06-04");
    EXPECT_EQ(source.document().at("language_version"), 1);
    EXPECT_EQ(source.document().at("host_api_version"), 1);
    EXPECT_EQ(source.document().at("source"),
              R"(neograph.define({ op: "call_core", name: "main" });)");

    const auto restored = ProgramSource::parse(source.serialize_canonical());
    EXPECT_EQ(restored.kind(), SourceKind::JavaScript);
    EXPECT_EQ(restored.source_hash(), source.source_hash());
    EXPECT_EQ(restored.canonical_document(), source.canonical_document());
}

TEST(ProgramSourceTest, StoredJavaScriptSourceRejectsAnIncompatibleHostAbi) {
    const auto source = ProgramSource::from_javascript("control.js", "neograph.define({});");
    auto       stored = json::parse(source.serialize_canonical());
    stored["document"]["host_api_version"] = 2;

    try {
        (void)ProgramSource::parse(stored.dump());
        FAIL() << "expected incompatible JavaScript source envelope rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("host API version"), std::string::npos);
    }
}

TEST(ProgramSourceTest, ParseFailureDoesNotFabricateSpanForUtf8Source) {
    try {
        (void)ProgramSource::from_canonical_json("utf8-source", R"({"이름":"값",})");
        FAIL() << "expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_SOURCE_JSON_PARSE");
        EXPECT_EQ(error.diagnostic().primary.source_id, "utf8-source");

        EXPECT_FALSE(error.diagnostic().primary.span.has_value());
    }
}

TEST(ProgramSourceTest, InvalidSourceIdIsRejectedBeforeParseDiagnostics) {
    EXPECT_THROW((void)ProgramSource::from_canonical_json("", "{"), std::invalid_argument);
    EXPECT_THROW(
        (void)ProgramSource::from_canonical_json(std::string(1, static_cast<char>(0xff)), "{"),
        std::invalid_argument);
}

TEST(ProgramSourceTest, RejectsNegativeAndOverflowingPersistedVersions) {
    EXPECT_THROW((void)ProgramSource::from_canonical_json("negative-version",
                                                          R"({"program_schema_version":-1})"),
                 ProgramDiagnosticError);
    EXPECT_THROW((void)ProgramSource::from_canonical_json(
                     "overflowing-version", R"({"program_schema_version":4294967296})"),
                 ProgramDiagnosticError);
}

TEST(ProgramSourceTest, StoredParserRejectsDuplicateObjectMembers) {
    auto       stored = make_source().serialize_canonical();
    const auto format = stored.find(R"("format":)");
    ASSERT_NE(format, std::string::npos);
    stored.insert(format, R"("format":"neograph-program-source",)");
    EXPECT_THROW((void)ProgramSource::parse(stored), std::invalid_argument);

    auto       escaped        = make_source().serialize_canonical();
    const auto escaped_format = escaped.find(R"("format":)");
    ASSERT_NE(escaped_format, std::string::npos);
    escaped.insert(escaped_format, R"("\u0066ormat":"neograph-program-source",)");
    EXPECT_THROW((void)ProgramSource::parse(escaped), std::invalid_argument);
}

TEST(ProgramSourceTest, StrictParserBoundsBytesAndNestingBeforeMaterialization) {
    std::string deeply_nested(257, '[');
    deeply_nested += '0';
    deeply_nested.append(257, ']');
    EXPECT_THROW((void)ProgramSource::from_canonical_json("deep-source", std::move(deeply_nested)),
                 ProgramDiagnosticError);

    std::string oversized = R"({"program_schema_version":1,"payload":")";
    oversized.append(16u * 1024u * 1024u, 'x');
    oversized += R"("})";
    EXPECT_THROW((void)ProgramSource::from_canonical_json("large-source", std::move(oversized)),
                 ProgramDiagnosticError);
}
TEST(ProgramSourceTest, CppBuilderDetachesProxyJsonInput) {
    json owner = json{
        {"document",
         json{{"program_schema_version", 1}, {"nodes", json::object()}, {"edges", json::array()}}}};
    auto       source      = ProgramSource::from_cpp_builder("proxy-source", 1, owner["document"]);
    const auto source_hash = source.source_hash();
    const auto canonical   = source.canonical_document();

    owner["document"]["nodes"]["late"] = json::object();
    EXPECT_EQ(source.source_hash(), source_hash);
    EXPECT_EQ(source.canonical_document(), canonical);
    EXPECT_FALSE(source.document()["nodes"].contains("late"));
}

TEST(ProgramDiagnosticTest, RoundTripsValidEscapedJsonPointers) {
    auto value = diagnostic();
    json encoded;
    to_json(encoded, value);

    Diagnostic decoded;
    from_json(encoded, decoded);
    json reencoded;
    to_json(reencoded, decoded);
    EXPECT_EQ(reencoded.dump(), encoded.dump());
}

TEST(ProgramDiagnosticTest, RejectsMalformedRfc6901PointersOnBothSurfaces) {
    for (const std::string pointer : {"not-rooted", "/bad~", "/bad~2escape"}) {
        auto invalid_coordinate = coordinate(pointer);
        json encoded_coordinate;
        EXPECT_THROW(to_json(encoded_coordinate, invalid_coordinate), std::invalid_argument);

        auto source_map = SourceMapEntry{pointer, coordinate()};
        json encoded_source_map;
        EXPECT_THROW(to_json(encoded_source_map, source_map), std::invalid_argument);
    }
}

TEST(ProgramDiagnosticTest, RejectsReversedSpanCoordinates) {
    for (const SourceSpan span : {
             SourceSpan{8, 7, 1, 1, 1, 2},
             SourceSpan{0, 1, 3, 1, 2, 9},
             SourceSpan{0, 1, 2, 9, 2, 8},
         }) {
        json encoded;
        EXPECT_THROW(to_json(encoded, span), std::invalid_argument);
    }
}

TEST(ProgramDiagnosticTest, ProducerRejectsUnknownEnums) {
    auto invalid_phase  = diagnostic();
    invalid_phase.phase = static_cast<CompilePhase>(255);
    json encoded;
    EXPECT_THROW(to_json(encoded, invalid_phase), std::invalid_argument);

    auto invalid_severity     = diagnostic();
    invalid_severity.severity = static_cast<DiagnosticSeverity>(255);
    EXPECT_THROW(to_json(encoded, invalid_severity), std::invalid_argument);
}

TEST(ProgramBundleTest, RoundTripsClosedTypedRecords) {
    const auto    source = make_source();
    ProgramBundle bundle(make_bundle_data(source));
    const auto    bytes  = bundle.serialize_canonical();
    const auto    parsed = ProgramBundle::parse(bytes);

    EXPECT_EQ(parsed.id(), bundle.id());
    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    EXPECT_EQ(parsed.input_contract().schema_version, 1U);
    EXPECT_EQ(parsed.sealed_core_definitions().size(), 2U);
    EXPECT_EQ(parsed.core_plan_identities().front().name, "alpha");
    EXPECT_EQ(parsed.executable_registry_identities().front().name, "alpha");
    EXPECT_EQ(parsed.declared_budget_requirements().front().resource, "parallelism");
    ASSERT_EQ(parsed.diagnostics().size(), 1U);
    EXPECT_EQ(parsed.diagnostics().front().code, bundle.diagnostics().front().code);
}

TEST(ProgramBundleTest, ImportedExecutableKindUsesOneCanonicalWireToken) {
    EXPECT_EQ(to_string(ExecutableKind::Imported), "imported");
    EXPECT_EQ(executable_kind_from_string("imported"), ExecutableKind::Imported);
    EXPECT_THROW((void)executable_kind_from_string("import"), std::invalid_argument);
}

TEST(CoreCompiledPlanIdentityTest, CanonicalizesClosureOrderAndMatchesKnownVector) {
    const auto definition = sealed_definition(
        "main", json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                     {"name", "main"},
                     {"nodes", json{{"main", json{{"type", "vector-node"}}}}}});
    std::vector<ExecutableIdentity> closure = {
        ExecutableIdentity{ExecutableKind::Tool, "zeta", "2.1.0-rc.1+build.7", sha('2')},
        ExecutableIdentity{ExecutableKind::Node, "alpha", "1.0.0", sha('3')},
    };

    const auto identity =
        core_compiled_plan_identity(definition, "vector-compiler", sha('b'), closure);
    EXPECT_EQ(identity, "sha256:c7da5b6de16628f74114adbad0f8bd55252b371dbfa80f69c822fcc7c2cae4c7");

    std::reverse(closure.begin(), closure.end());
    EXPECT_EQ(core_compiled_plan_identity(definition, "vector-compiler", sha('b'), closure),
              identity);
    EXPECT_NE(core_compiled_plan_identity(definition, "vector-compiler-next", sha('b'), closure),
              identity);
    EXPECT_NE(core_compiled_plan_identity(definition, "vector-compiler", sha('c'), closure),
              identity);

    auto changed_closure                          = closure;
    changed_closure.front().implementation_digest = sha('4');
    EXPECT_NE(core_compiled_plan_identity(definition, "vector-compiler", sha('b'), changed_closure),
              identity);

    auto changed_definition                                = definition;
    changed_definition.definition["nodes"]["main"]["type"] = "other-node";
    changed_definition.definition_hash = sealed_core_definition_hash(changed_definition.definition);
    EXPECT_NE(core_compiled_plan_identity(changed_definition, "vector-compiler", sha('b'), closure),
              identity);
}

TEST(CoreCompiledPlanIdentityTest, RejectsDuplicateOrAmbiguousExecutableClosure) {
    const auto definition = sealed_definition(
        "main", json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                     {"name", "main"},
                     {"nodes", json{{"main", json{{"type", "vector-node"}}}}}});
    const std::vector<ExecutableIdentity> ambiguous = {
        ExecutableIdentity{ExecutableKind::Node, "alpha", "1.0.0", sha('3')},
        ExecutableIdentity{ExecutableKind::Node, "alpha", "2.0.0", sha('4')},
    };

    EXPECT_THROW(
        (void)core_compiled_plan_identity(definition, "vector-compiler", sha('b'), ambiguous),
        std::invalid_argument);

    auto exact_duplicate   = ambiguous;
    exact_duplicate.back() = exact_duplicate.front();
    EXPECT_THROW(
        (void)core_compiled_plan_identity(definition, "vector-compiler", sha('b'), exact_duplicate),
        std::invalid_argument);
}

TEST(ProgramBundleTest, IdentityAndBytesIgnoreSetLikeInputOrder) {
    const auto source      = make_source();
    auto       first_data  = make_bundle_data(source);
    auto       second_data = first_data;
    std::reverse(second_data.sealed_core_definitions.begin(),
                 second_data.sealed_core_definitions.end());
    std::reverse(second_data.core_plan_identities.begin(), second_data.core_plan_identities.end());
    std::reverse(second_data.capability_effect_closure.capabilities.begin(),
                 second_data.capability_effect_closure.capabilities.end());
    std::reverse(second_data.capability_effect_closure.effects.begin(),
                 second_data.capability_effect_closure.effects.end());
    std::reverse(second_data.executable_registry_identities.begin(),
                 second_data.executable_registry_identities.end());
    std::reverse(second_data.declared_budget_requirements.begin(),
                 second_data.declared_budget_requirements.end());
    std::reverse(second_data.source_map.begin(), second_data.source_map.end());

    ProgramBundle first(std::move(first_data));
    ProgramBundle second(std::move(second_data));
    EXPECT_EQ(first.id(), second.id());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
}

TEST(ProgramBundleTest, RejectsInvalidTypedRecordsAndNestedUnknownFields) {
    const auto source                                               = make_source();
    auto       invalid                                              = make_bundle_data(source);
    invalid.executable_registry_identities.front().semantic_version = "01.0.0";
    EXPECT_THROW((void)ProgramBundle(std::move(invalid)), std::invalid_argument);
    auto invalid_kind                                        = make_bundle_data(source);
    invalid_kind.executable_registry_identities.front().kind = static_cast<ExecutableKind>(255);
    EXPECT_THROW((void)ProgramBundle(std::move(invalid_kind)), std::invalid_argument);

    auto non_ascii_version = make_bundle_data(source);
    non_ascii_version.executable_registry_identities.front().semantic_version = "1.0.0-\xc3\xa9";
    EXPECT_THROW((void)ProgramBundle(std::move(non_ascii_version)), std::invalid_argument);

    ProgramBundle valid(make_bundle_data(source));
    auto          stored               = json::parse(valid.serialize_canonical());
    stored["input_contract"]["future"] = true;
    EXPECT_THROW((void)ProgramBundle::parse(stored.dump()), std::invalid_argument);
}

TEST(ProgramBundleTest, RejectsUnsupportedSourceSchemaAndErrorDiagnostics) {
    const auto source = make_source();

    auto unsupported_schema                   = make_bundle_data(source);
    unsupported_schema.program_schema_version = 2;
    EXPECT_THROW((void)ProgramBundle(std::move(unsupported_schema)), std::invalid_argument);

    auto failed_compilation                         = make_bundle_data(source);
    failed_compilation.diagnostics.front().severity = DiagnosticSeverity::Error;
    EXPECT_THROW((void)ProgramBundle(std::move(failed_compilation)), std::invalid_argument);
    ProgramBundle valid(make_bundle_data(source));
    auto          negative_storage             = json::parse(valid.serialize_canonical());
    negative_storage["storage_schema_version"] = -1;
    EXPECT_THROW((void)ProgramBundle::parse(negative_storage.dump()), std::invalid_argument);

    auto overflowing_schema = json::parse(valid.serialize_canonical());
    overflowing_schema["input_contract"]["schema_version"] = 4294967296ULL;
    EXPECT_THROW((void)ProgramBundle::parse(overflowing_schema.dump()), std::invalid_argument);
}

TEST(ProgramBundleTest, SourceKindIsRequiredAndRoundTripsExactly) {
    const auto cpp_source =
        ProgramSource::from_cpp_builder("source.cpp", 1, make_source().document());
    ProgramBundle cpp_bundle(make_bundle_data(cpp_source));
    EXPECT_EQ(cpp_bundle.source_kind(), SourceKind::CppBuilder);
    EXPECT_EQ(ProgramBundle::parse(cpp_bundle.serialize_canonical()).source_kind(),
              SourceKind::CppBuilder);

    const auto    js_source = ProgramSource::from_javascript("source.js", "neograph.define({});");
    ProgramBundle js_bundle(make_bundle_data(js_source));
    EXPECT_EQ(js_bundle.source_kind(), SourceKind::JavaScript);
    EXPECT_EQ(ProgramBundle::parse(js_bundle.serialize_canonical()).source_kind(),
              SourceKind::JavaScript);

    auto omitted        = make_bundle_data(make_source());
    omitted.source_kind = static_cast<SourceKind>(255);
    EXPECT_THROW((void)ProgramBundle(std::move(omitted)), std::invalid_argument);

    auto unknown           = json::parse(cpp_bundle.serialize_canonical());
    unknown["source_kind"] = "future_source_kind";
    EXPECT_THROW((void)ProgramBundle::parse(unknown.dump()), std::invalid_argument);

    auto       missing = cpp_bundle.serialize_canonical();
    const auto marker  = std::string(R"("source_kind":"cpp_builder",)");
    const auto offset  = missing.find(marker);
    ASSERT_NE(offset, std::string::npos);
    missing.erase(offset, marker.size());
    EXPECT_THROW((void)ProgramBundle::parse(missing), std::invalid_argument);
}

TEST(ProgramBundleTest, RejectsDuplicateOrAmbiguousExecutableClosureIdentities) {
    const auto source = make_source();

    auto ambiguous = make_bundle_data(source);
    ambiguous.executable_registry_identities.push_back(
        ExecutableIdentity{ExecutableKind::Node, "alpha", "2.0.0", sha('4')});
    EXPECT_THROW((void)ProgramBundle(std::move(ambiguous)), std::invalid_argument);

    auto exact_duplicate = make_bundle_data(source);
    exact_duplicate.executable_registry_identities.push_back(
        exact_duplicate.executable_registry_identities.front());
    EXPECT_THROW((void)ProgramBundle(std::move(exact_duplicate)), std::invalid_argument);
}

TEST(ProgramBundleTest, RejectsMismatchedSealedDefinitions) {
    const auto source                                       = make_source();
    auto       data                                         = make_bundle_data(source);
    data.sealed_core_definitions.front().definition["name"] = "tampered";
    EXPECT_THROW((void)ProgramBundle(std::move(data)), std::invalid_argument);

    auto  mismatched_name                    = make_bundle_data(source);
    auto& mismatched_definition              = mismatched_name.sealed_core_definitions.front();
    mismatched_definition.definition["name"] = "not-beta";
    mismatched_definition.definition_hash =
        sealed_core_definition_hash(mismatched_definition.definition);
    EXPECT_THROW((void)ProgramBundle(std::move(mismatched_name)), std::invalid_argument);

    auto  future_schema                            = make_bundle_data(source);
    auto& future_definition                        = future_schema.sealed_core_definitions.front();
    future_definition.definition["schema_version"] = 2;
    future_definition.definition_hash = sealed_core_definition_hash(future_definition.definition);
    EXPECT_THROW((void)ProgramBundle(std::move(future_schema)), std::invalid_argument);

    auto  unknown_field                            = make_bundle_data(source);
    auto& unknown_definition                       = unknown_field.sealed_core_definitions.front();
    unknown_definition.definition["unknown_field"] = true;
    unknown_definition.definition_hash = sealed_core_definition_hash(unknown_definition.definition);
    EXPECT_THROW((void)ProgramBundle(std::move(unknown_field)), std::invalid_argument);

    auto  unknown_nested                     = make_bundle_data(source);
    auto& nested_definition                  = unknown_nested.sealed_core_definitions.front();
    nested_definition.definition["channels"] = json{{"state", json{{"reducerr", "append"}}}};
    nested_definition.definition_hash = sealed_core_definition_hash(nested_definition.definition);
    EXPECT_THROW((void)ProgramBundle(std::move(unknown_nested)), std::invalid_argument);

    for (const double multiplier : {1e300, -1e300}) {
        auto  out_of_range            = make_bundle_data(source);
        auto& out_of_range_definition = out_of_range.sealed_core_definitions.front();
        out_of_range_definition.definition["retry_policy"] =
            json{{"backoff_multiplier", multiplier}};
        out_of_range_definition.definition_hash =
            sealed_core_definition_hash(out_of_range_definition.definition);
        EXPECT_THROW((void)ProgramBundle(std::move(out_of_range)), std::invalid_argument);
    }

    auto  nonfinite_definition = make_bundle_data(source);
    auto& nonfinite            = nonfinite_definition.sealed_core_definitions.front();
    nonfinite.definition["retry_policy"] =
        json{{"backoff_multiplier", std::numeric_limits<double>::infinity()}};
    nonfinite.definition_hash = sha('9');
    EXPECT_THROW((void)ProgramBundle(std::move(nonfinite_definition)), std::invalid_argument);

    auto  empty_node_field = make_bundle_data(source);
    auto& empty_field      = empty_node_field.sealed_core_definitions.front();
    empty_field.definition["nodes"]["node"]["type"] = "test-node";
    empty_field.definition["nodes"]["node"][""]     = true;
    empty_field.definition_hash = sealed_core_definition_hash(empty_field.definition);
    EXPECT_THROW((void)ProgramBundle(std::move(empty_node_field)), std::invalid_argument);

    ProgramBundle valid(make_bundle_data(source));
    auto          stored = json::parse(valid.serialize_canonical());
    stored["sealed_core_definitions"][0]["definition"]["name"] = "tampered";
    EXPECT_THROW((void)ProgramBundle::parse(stored.dump()), std::invalid_argument);
}

TEST(ProgramBundleTest, StoredParserRejectsNestedDuplicateObjectMembers) {
    auto              stored = ProgramBundle(make_bundle_data(make_source())).serialize_canonical();
    const std::string marker = R"("input_contract":{)";
    const auto        contract = stored.find(marker);
    ASSERT_NE(contract, std::string::npos);
    stored.insert(contract + marker.size(), R"("schema_version":1,)");
    EXPECT_THROW((void)ProgramBundle::parse(stored), std::invalid_argument);
}

TEST(ProgramBundleTest, IdentityEnvelopeRejectsFormatStorageAndContentTampering) {
    ProgramBundle bundle(make_bundle_data(make_source()));

    auto wrong_format      = json::parse(bundle.serialize_canonical());
    wrong_format["format"] = "future-program-bundle";
    EXPECT_THROW((void)ProgramBundle::parse(wrong_format.dump()), std::invalid_argument);

    auto wrong_storage                      = json::parse(bundle.serialize_canonical());
    wrong_storage["storage_schema_version"] = 2;
    EXPECT_THROW((void)ProgramBundle::parse(wrong_storage.dump()), std::invalid_argument);

    auto wrong_content                 = json::parse(bundle.serialize_canonical());
    wrong_content["compiler_build_id"] = "tampered";
    EXPECT_THROW((void)ProgramBundle::parse(wrong_content.dump()), std::invalid_argument);
}

TEST(ProgramContractTest, ValidatesRetainedSchemaWithoutMcpDependency) {
    ContractRecord contract{1, json{{"type", "object"},
                                    {"required", json::array({"task"})},
                                    {"properties", json{{"task", json{{"type", "string"}}}}},
                                    {"additionalProperties", false}}};

    EXPECT_NO_THROW(validate_contract_schema(contract));
    EXPECT_NO_THROW(validate_contract_value(json{{"task", "ship"}}, contract));
    EXPECT_THROW(validate_contract_value(json::object(), contract), std::invalid_argument);
    EXPECT_THROW(validate_contract_value(json{{"task", 7}}, contract), std::invalid_argument);
    EXPECT_THROW(
        validate_contract_value(json{{"task", "ship"}, {"raw_route", "forbidden"}}, contract),
        std::invalid_argument);
}

TEST(ProgramVersionTest, RoundTripsAndNormalizesReceiptOrder) {
    ProgramBundle bundle(make_bundle_data(make_source()));
    auto          first_data                                    = make_version_data(bundle);
    first_data.core_materialization_receipt.capability_bindings = {
        CapabilityBindingReceipt{
            ExecutableIdentity{ExecutableKind::Tool, "tool", "1.0.0", sha('8')}, sha('9')},
        CapabilityBindingReceipt{
            ExecutableIdentity{ExecutableKind::Provider, "provider", "1.0.0", sha('a')}, sha('b')}};
    auto second_data = first_data;
    std::reverse(second_data.dependency_receipts.begin(), second_data.dependency_receipts.end());
    std::reverse(second_data.core_materialization_receipt.plans.begin(),
                 second_data.core_materialization_receipt.plans.end());
    std::reverse(second_data.core_materialization_receipt.capability_bindings.begin(),
                 second_data.core_materialization_receipt.capability_bindings.end());

    ProgramVersion first(std::move(first_data));
    ProgramVersion second(std::move(second_data));
    EXPECT_EQ(first.id(), second.id());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());

    const auto parsed = ProgramVersion::parse(first.serialize_canonical());
    EXPECT_EQ(parsed.id(), first.id());
    EXPECT_EQ(parsed.dependency_receipts().front().dependency_id, "module:alpha");
    EXPECT_EQ(parsed.core_materialization_receipt().plans.front().name, "alpha");
    EXPECT_EQ(parsed.core_materialization_receipt().capability_bindings.front().executable.name,
              "provider");
    EXPECT_EQ(
        capability_binding_receipt_root(parsed.core_materialization_receipt().capability_bindings),
        capability_binding_receipt_root(first.core_materialization_receipt().capability_bindings));
}

TEST(ProgramVersionTest, RejectsSchemaInvalidTokenStringsInConstructorAndParser) {
    ProgramBundle bundle(make_bundle_data(make_source()));

    auto padded_dependency                                      = make_version_data(bundle);
    padded_dependency.dependency_receipts.front().dependency_id = " module:zeta";
    EXPECT_THROW((void)ProgramVersion(std::move(padded_dependency)), std::invalid_argument);

    auto controlled_compiler                                           = make_version_data(bundle);
    controlled_compiler.core_materialization_receipt.compiler_build_id = "compiler\nbuild";
    EXPECT_THROW((void)ProgramVersion(std::move(controlled_compiler)), std::invalid_argument);

    auto padded_plan                                            = make_version_data(bundle);
    padded_plan.core_materialization_receipt.plans.front().name = "beta ";
    EXPECT_THROW((void)ProgramVersion(std::move(padded_plan)), std::invalid_argument);

    auto padded_scope            = make_version_data(bundle);
    padded_scope.ownership_scope = " tenant:example";
    EXPECT_THROW((void)ProgramVersion(std::move(padded_scope)), std::invalid_argument);

    const ProgramVersion valid(make_version_data(bundle));
    auto                 stored                       = json::parse(valid.serialize_canonical());
    stored["dependency_receipts"][0]["dependency_id"] = "module:alpha ";
    EXPECT_THROW((void)ProgramVersion::parse(stored.dump()), std::invalid_argument);
}

TEST(ProgramVersionTest, RejectsMalformedAndDuplicateCapabilityBindingReceipts) {
    ProgramBundle            bundle(make_bundle_data(make_source()));
    const ExecutableIdentity provider{ExecutableKind::Provider, "provider", "1.0.0", sha('c')};

    auto malformed = make_version_data(bundle);
    malformed.core_materialization_receipt.capability_bindings.push_back(
        CapabilityBindingReceipt{provider, "route:not-a-digest"});
    EXPECT_THROW((void)ProgramVersion(std::move(malformed)), std::invalid_argument);

    auto duplicate                                             = make_version_data(bundle);
    duplicate.core_materialization_receipt.capability_bindings = {
        CapabilityBindingReceipt{provider, sha('d')}, CapabilityBindingReceipt{provider, sha('e')}};
    EXPECT_THROW((void)ProgramVersion(std::move(duplicate)), std::invalid_argument);
}

TEST(ProgramVersionTest, RejectsNestedUnknownFieldsAndIdentityEnvelopeTampering) {
    ProgramBundle  bundle(make_bundle_data(make_source()));
    ProgramVersion version(make_version_data(bundle));

    auto nested                           = json::parse(version.serialize_canonical());
    nested["admission_profile"]["future"] = true;
    EXPECT_THROW((void)ProgramVersion::parse(nested.dump()), std::invalid_argument);

    auto wrong_format      = json::parse(version.serialize_canonical());
    wrong_format["format"] = "future-program-version";
    EXPECT_THROW((void)ProgramVersion::parse(wrong_format.dump()), std::invalid_argument);

    auto wrong_storage                      = json::parse(version.serialize_canonical());
    wrong_storage["storage_schema_version"] = 2;
    EXPECT_THROW((void)ProgramVersion::parse(wrong_storage.dump()), std::invalid_argument);

    auto wrong_content               = json::parse(version.serialize_canonical());
    wrong_content["ownership_scope"] = "tenant:tampered";
    EXPECT_THROW((void)ProgramVersion::parse(wrong_content.dump()), std::invalid_argument);
}

TEST(ProgramVersionTest, StoredParserRejectsDuplicateObjectMembers) {
    ProgramBundle     bundle(make_bundle_data(make_source()));
    auto              stored  = ProgramVersion(make_version_data(bundle)).serialize_canonical();
    const std::string marker  = R"("admission_profile":{)";
    const auto        profile = stored.find(marker);
    ASSERT_NE(profile, std::string::npos);
    stored.insert(profile + marker.size(), R"("id":"production",)");
    EXPECT_THROW((void)ProgramVersion::parse(stored), std::invalid_argument);
}

TEST(ProgramStoredValueTest, BundleV1MatchesKnownVectorAndVersionReceiptIsCanonical) {
    ProgramBundleData bundle_data;
    bundle_data.source_kind                    = SourceKind::CanonicalJson;
    bundle_data.source_hash                    = make_source().source_hash();
    bundle_data.canonical_program_hash         = sha('a');
    bundle_data.compiler_build_id              = "vector-compiler";
    bundle_data.program_schema_version         = 1;
    bundle_data.registry_snapshot_fingerprint  = sha('b');
    bundle_data.module_dependency_merkle_root  = sha('c');
    bundle_data.input_contract                 = ContractRecord{1, json::object()};
    bundle_data.output_contract                = ContractRecord{1, json::object()};
    bundle_data.orchestration_plan             = OrchestrationPlanRecord{1, json::object()};
    bundle_data.sealed_core_definitions        = {sealed_definition(
        "main", json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                            {"nodes", json{{"main", json{{"type", "vector-node"}}}}}})};
    bundle_data.core_plan_identities           = {CorePlanIdentity{"main", sha('d')}};
    bundle_data.executable_registry_identities = {
        ExecutableIdentity{ExecutableKind::Node, "main", "1.0.0", sha('e')}};
    bundle_data.declared_budget_requirements = {BudgetRequirement{"steps", 0, 1}};
    ProgramBundle bundle(std::move(bundle_data));

    const std::string expected_bundle =
        R"JSON({"canonical_program_hash":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)JSON"
        R"JSON("capability_effect_closure":{"capabilities":[],"effects":[]},"compiler_build_id":"vector-compiler",")JSON"
        R"JSON(core_plan_identities":[{"compiled_plan_identity":"sha256:ddddddddddddddddddddddddddddddddddddddddddd)JSON"
        R"JSON(ddddddddddddddddddddd","name":"main"}],"declared_budget_requirements":[{"maximum":1,"minimum":0,"res)JSON"
        R"JSON(ource":"steps"}],"diagnostics":[],"executable_registry_identities":[{"implementation_digest":"sha256)JSON"
        R"JSON(:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","kind":"node","name":"main","sema)JSON"
        R"JSON(ntic_version":"1.0.0"}],"execution_guarantee":"strict","format":"neograph-program-bundle","id":"sha256:f90312429fafc252b96a0c18c986a0e5a525b7357e19a69959b83bb1787a4d65","input_contra)JSON"
        R"JSON(ct":{"schema":{},"schema_version":1},"module_dependency_merkle_root":"sha256:cccccccccccccccccccccccccccccccc)JSON"
        R"JSON(cccccccccccccccccccccccccccccccc","orchestration_plan":{"plan":{},"schema_version":1},"output_contract":)JSON"
        R"JSON({"schema":{},"schema_version":1},"program_schema_version":1,"registry_snapshot_fingerprint":"sha256:bbbbbb)JSON"
        R"JSON(bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","sealed_core_definitions":[{"definition":{"nodes":{"main":{"type":"vector-node"}},"schema_version":1},)JSON"
        R"JSON("definition_hash":"sha256:322450017d0c7116b10d44ab43b62815f9423729ee240b0bb8f25cb6352208aa","name":"main"}],)JSON"
        R"JSON("source_hash":"sha256:d0de8671b55a589345c561ca87b14a55755e8934e1c82fadb0ab9c934a313e3a","source_kind")JSON"
        R"JSON(:"canonical_json","source_map":[],"storage_schema_version":1})JSON";
    EXPECT_EQ(bundle.id(),
              "sha256:f90312429fafc252b96a0c18c986a0e5a525b7357e19a69959b83bb1787a4d65");
    EXPECT_EQ(bundle.serialize_canonical(), expected_bundle);

    const auto         registry  = make_registry_snapshot();
    auto               admission = make_admission_profile(registry, "vector-profile");
    auto               policy = make_policy_snapshot(admission, "vector-policy", "tenant:vector");
    ProgramVersionData version_data(
        bundle.id(), admission, policy, {}, policy.owner_scope(),
        CoreMaterializationReceipt{
            "vector-compiler", registry.fingerprint(), {CorePlanIdentity{"main", sha('d')}}});
    ProgramVersion version(std::move(version_data));

    const auto encoded_version = json::parse(version.serialize_canonical());
    ASSERT_TRUE(encoded_version["core_materialization_receipt"].contains("capability_bindings"));
    EXPECT_EQ(encoded_version["core_materialization_receipt"]["capability_bindings"],
              json::array());
    EXPECT_EQ(ProgramVersion::parse(version.serialize_canonical()).id(), version.id());
}

TEST(ProgramStoredValueTest, DeepCopiesCallerOwnedJson) {
    auto source                = make_source();
    auto data                  = make_bundle_data(source);
    json input_schema          = json{{"type", "object"}};
    data.input_contract.schema = input_schema;
    ProgramBundle bundle(std::move(data));
    input_schema["type"] = "array";
    EXPECT_EQ(bundle.input_contract().schema["type"].get<std::string>(), "object");

    auto returned           = bundle.input_contract();
    returned.schema["type"] = "integer";
    EXPECT_EQ(bundle.input_contract().schema["type"].get<std::string>(), "object");
}

TEST(ProgramStoredValueTest, BundleDetachesProxyJsonAndJsonBearingAccessors) {
    json owner = json{
        {"schema", json{{"type", "object"}}},
        {"plan", json{{"entry", "alpha"}}},
        {"definition", json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                            {"nodes", json{{"owned", json{{"type", "test-node"}}}}}}},
        {"witness", json{{"nested", json::object()}}},
    };
    auto data                                       = make_bundle_data(make_source());
    data.input_contract.schema                      = owner["schema"];
    data.orchestration_plan.plan                    = owner["plan"];
    data.sealed_core_definitions.front().definition = owner["definition"];
    data.sealed_core_definitions.front().definition_hash =
        sealed_core_definition_hash(owner["definition"]);
    data.diagnostics.front().witness = owner["witness"];
    ProgramBundle bundle(std::move(data));
    const auto    bundle_id = bundle.id();
    const auto    bytes     = bundle.serialize_canonical();

    owner["schema"]["type"]              = "array";
    owner["plan"]["entry"]               = "late";
    owner["definition"]["nodes"]["late"] = json::object();
    owner["witness"]["nested"]["late"]   = true;
    EXPECT_EQ(bundle.id(), bundle_id);
    EXPECT_EQ(bundle.serialize_canonical(), bytes);

    bundle.sealed_core_definitions().front().definition["nodes"]["external"] = json::object();
    bundle.diagnostics().front().witness["nested"]["external"]               = true;
    EXPECT_EQ(bundle.id(), bundle_id);
    EXPECT_EQ(bundle.serialize_canonical(), bytes);
}

TEST(ProgramStoredValueTest, BundleDetachesDirectlyInitializedJsonProxies) {
    json owner = json{
        {"definition", json{{"schema_version", SealedCoreDefinition::STORAGE_SCHEMA_VERSION},
                            {"nodes", json{{"alpha", json{{"type", "test-node"}}}}}}},
        {"witness", json{{"nested", json::object()}}},
    };
    SealedCoreDefinition direct_definition{
        "alpha", sealed_core_definition_hash(owner["definition"]), owner["definition"]};
    Diagnostic direct_diagnostic{
        CompilePhase::Seal, "P_PROXY", DiagnosticSeverity::Note, coordinate(""), "direct proxy",
        owner["witness"],   {}};

    auto data = make_bundle_data(make_source());
    data.sealed_core_definitions.clear();
    data.sealed_core_definitions.emplace_back(std::move(direct_definition));
    data.core_plan_identities = {CorePlanIdentity{"alpha", sha('1')}};
    data.diagnostics.clear();
    data.diagnostics.emplace_back(std::move(direct_diagnostic));
    ProgramBundle bundle(std::move(data));
    const auto    bundle_id = bundle.id();
    const auto    bytes     = bundle.serialize_canonical();

    owner["definition"]["nodes"]["alpha"]["late"] = true;
    owner["witness"]["nested"]["late"]            = true;
    EXPECT_EQ(bundle.id(), bundle_id);
    EXPECT_EQ(bundle.serialize_canonical(), bytes);
}

TEST(ProgramResultTest, DefaultValueIsCoherentAsioExceptionSentinel) {
    ProgramResult result;
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Failed);
    EXPECT_EQ(result.operation_id(), "root");
    EXPECT_EQ(result.attempt(), 0U);
    ASSERT_TRUE(result.failure().has_value());
    EXPECT_EQ(result.failure()->code, "P_RESULT_EMPTY");
    EXPECT_THROW(result.serialize_canonical(), std::invalid_argument);
}

}  // namespace
