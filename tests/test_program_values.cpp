#include <neograph/program/program.h>

#include <gtest/gtest.h>

#include <string>

namespace {

using neograph::json;
using namespace neograph::program;

ProgramBundleData make_bundle_data(const ProgramSource& source) {
    ProgramBundleData data;
    data.source_hash                               = source.source_hash();
    data.canonical_program_hash                    = source.source_hash();
    data.compiler_build_id                         = "neograph-test-compiler/1";
    data.program_schema_version                    = source.schema_version();
    data.registry_snapshot_fingerprint             = "registry:test-snapshot";
    data.module_dependency_merkle_root             = "modules:none";
    data.input_contract                            = json::object();
    data.input_contract["type"]                    = "object";
    data.output_contract                           = json::object();
    data.output_contract["type"]                   = "object";
    data.orchestration_plan                        = json::object();
    data.orchestration_plan["entry"]               = "start";
    data.core_compiled_plan_identities             = json::array();
    data.capability_effect_closure                 = json::object();
    data.executable_registry_identities            = json::array();
    data.declared_budget_requirements              = json::object();
    data.declared_budget_requirements["max_steps"] = 10;

    SourceMapEntry mapping;
    mapping.generated_pointer     = "/steps/0";
    mapping.authored.source_id    = source.source_id();
    mapping.authored.json_pointer = "/steps/0";
    mapping.authored.span         = SourceSpan{32, 46, 1, 33, 1, 47};
    data.source_map.push_back(mapping);

    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::Normalize;
    diagnostic.code     = "P_TEST_NOTE";
    diagnostic.severity = DiagnosticSeverity::Note;
    diagnostic.primary  = mapping.authored;
    diagnostic.message  = "normalized test program";
    diagnostic.witness  = json::object();
    data.diagnostics.push_back(std::move(diagnostic));
    return data;
}

TEST(ProgramSourceTest, CanonicalAndBuilderInputsOwnEqualNormalizedContent) {
    std::string authored =
        R"({"steps":[{"name":"first","config":{"z":2,"a":1}}],"program_schema_version":1})";
    auto raw = ProgramSource::from_canonical_json("source-a", authored);

    json builder_document                      = json::object();
    builder_document["program_schema_version"] = 1;
    builder_document["steps"]                  = json::array();
    json step                                  = json::object();
    step["config"]                             = json::object();
    step["config"]["a"]                        = 1;
    step["config"]["z"]                        = 2;
    step["name"]                               = "first";
    builder_document["steps"].push_back(step);
    auto builder = ProgramSource::from_cpp_builder("source-b", 1, builder_document);

    authored.assign("destroyed");
    builder_document["steps"] = json::array();

    EXPECT_EQ(raw.source_hash(), builder.source_hash());
    EXPECT_EQ(raw.canonical_document(), builder.canonical_document());
    EXPECT_EQ(raw.document()["steps"].size(), 1U);
    EXPECT_EQ(builder.document()["steps"].size(), 1U);
    EXPECT_EQ(raw.kind(), SourceKind::CanonicalJson);
    EXPECT_EQ(builder.kind(), SourceKind::CppBuilder);
}

TEST(ProgramSourceTest, MalformedTextReportsStableDiagnosticAndCoordinate) {
    try {
        static_cast<void>(ProgramSource::from_canonical_json("broken-source", R"({"steps":[})"));
        FAIL() << "expected ProgramDiagnosticError";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().phase, CompilePhase::Source);
        EXPECT_EQ(error.diagnostic().code, "P_SOURCE_JSON_PARSE");
        EXPECT_EQ(error.diagnostic().primary.source_id, "broken-source");
        ASSERT_TRUE(error.diagnostic().primary.span.has_value());
        EXPECT_GT(error.diagnostic().primary.span->byte_end, 0U);
    }
}

TEST(ProgramSourceTest, StoredRoundTripPreservesIdentityAndRejectsFutureSchema) {
    auto       source = ProgramSource::from_canonical_json("round-trip",
                                                           R"({"program_schema_version":1,"steps":[]})");
    const auto bytes  = source.serialize_canonical();
    const auto parsed = ProgramSource::parse(bytes);

    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    EXPECT_EQ(parsed.source_hash(), source.source_hash());
    EXPECT_EQ(source.source_hash(),
              "sha256:8652eebe92b0dcd02b9faf63e59dd1f2"
              "a9b2e6072efbe0e665a53a67f28781d5");
    EXPECT_EQ(parsed.kind(), SourceKind::CanonicalJson);

    auto future                      = json::parse(bytes);
    future["storage_schema_version"] = 2;
    EXPECT_THROW(ProgramSource::parse(future.dump()), std::invalid_argument);

    auto unknown_field       = json::parse(bytes);
    unknown_field["unknown"] = true;
    EXPECT_THROW(ProgramSource::parse(unknown_field.dump()), std::invalid_argument);
}

TEST(ProgramSourceTest, ImportsAndSourceMapSurviveStoredRoundTrip) {
    json document                      = json::object();
    document["program_schema_version"] = 1;
    document["control"]                = json::object();
    document["control"]["kind"]        = "if";
    document["control"]["then"]        = json::array();
    document["control"]["else"]        = json::array();
    document["label"]                  = "한글 日本語 😀";

    ImportRef      imported{"shared-library", "sha256:imported-content"};
    SourceMapEntry mapping;
    mapping.generated_pointer     = "/control";
    mapping.authored.source_id    = "authored.neograph";
    mapping.authored.json_pointer = "/flow/0";
    mapping.authored.span         = SourceSpan{8, 24, 2, 1, 2, 17};

    const auto source =
        ProgramSource::from_cpp_builder("builder-source", 1, document, {imported}, {mapping});
    const auto parsed = ProgramSource::parse(source.serialize_canonical());

    ASSERT_EQ(parsed.imports().size(), 1U);
    EXPECT_EQ(parsed.imports().front(), imported);
    ASSERT_EQ(parsed.source_map().size(), 1U);
    EXPECT_EQ(parsed.source_map().front(), mapping);
    EXPECT_EQ(parsed.document()["control"]["kind"].get<std::string>(), "if");
    EXPECT_EQ(parsed.document()["label"].get<std::string>(), "한글 日本語 😀");

    auto unknown_nested                     = json::parse(source.serialize_canonical());
    unknown_nested["imports"][0]["unknown"] = true;
    EXPECT_THROW(ProgramSource::parse(unknown_nested.dump()), std::invalid_argument);

    auto zero = ProgramSource::from_cpp_builder("identity-zero", 1, json::parse(R"({"bit":0})"));
    auto one  = ProgramSource::from_cpp_builder("identity-one", 1, json::parse(R"({"bit":1})"));
    EXPECT_NE(zero.source_hash(), one.source_hash());
}

TEST(ProgramSourceTest, BuilderRejectsSchemaConflictsAndInvalidUtf8) {
    auto conflicting_document = json::parse(R"({"program_schema_version":2})");
    EXPECT_THROW(ProgramSource::from_cpp_builder("schema-conflict", 1, conflicting_document),
                 std::invalid_argument);

    json invalid_utf8    = json::object();
    invalid_utf8["text"] = std::string(1, static_cast<char>(0xff));
    EXPECT_THROW(ProgramSource::from_cpp_builder("invalid-utf8", 1, std::move(invalid_utf8)),
                 std::invalid_argument);
}

TEST(ProgramBundleTest, IdentityAndBytesAreCanonicalAndContentAddressed) {
    auto source      = ProgramSource::from_canonical_json("bundle-source",
                                                          R"({"program_schema_version":1,"steps":[]})");
    auto first_data  = make_bundle_data(source);
    auto second_data = make_bundle_data(source);

    first_data.input_contract              = json::object();
    first_data.input_contract["required"]  = json::array();
    first_data.input_contract["type"]      = "object";
    second_data.input_contract             = json::object();
    second_data.input_contract["type"]     = "object";
    second_data.input_contract["required"] = json::array();

    ProgramBundle first(std::move(first_data));
    ProgramBundle second(std::move(second_data));
    EXPECT_EQ(first.id(), second.id());
    EXPECT_EQ(first.serialize_canonical(), second.serialize_canonical());
    EXPECT_EQ(first.id().substr(0, 7), "sha256:");

    const auto parsed = ProgramBundle::parse(first.serialize_canonical());
    EXPECT_EQ(parsed.id(), first.id());
    EXPECT_EQ(parsed.source_map().front().authored.json_pointer, "/steps/0");
    EXPECT_EQ(parsed.diagnostics().front().code, "P_TEST_NOTE");

    auto changed_data                       = make_bundle_data(source);
    changed_data.output_contract["changed"] = true;
    ProgramBundle changed(std::move(changed_data));
    EXPECT_NE(changed.id(), first.id());
}

TEST(ProgramBundleTest, StoredBundleRejectsFutureSchemaAndTamperedIdentity) {
    auto          source = ProgramSource::from_canonical_json("bundle-reject",
                                                              R"({"program_schema_version":1,"steps":[]})");
    ProgramBundle bundle(make_bundle_data(source));

    auto future                      = json::parse(bundle.serialize_canonical());
    future["storage_schema_version"] = 2;
    EXPECT_THROW(ProgramBundle::parse(future.dump()), std::invalid_argument);

    auto tampered                 = json::parse(bundle.serialize_canonical());
    tampered["compiler_build_id"] = "tampered";
    EXPECT_THROW(ProgramBundle::parse(tampered.dump()), std::invalid_argument);

    auto unknown_top       = json::parse(bundle.serialize_canonical());
    unknown_top["unknown"] = true;
    EXPECT_THROW(ProgramBundle::parse(unknown_top.dump()), std::invalid_argument);

    auto unknown_nested                         = json::parse(bundle.serialize_canonical());
    unknown_nested["diagnostics"][0]["unknown"] = true;
    EXPECT_THROW(ProgramBundle::parse(unknown_nested.dump()), std::invalid_argument);
}

TEST(ProgramVersionTest, StoredRoundTripIsImmutableAndContentAddressed) {
    auto          source = ProgramSource::from_canonical_json("version-source",
                                                              R"({"program_schema_version":1,"steps":[]})");
    ProgramBundle bundle(make_bundle_data(source));

    ProgramVersionData data;
    data.bundle_id                            = bundle.id();
    data.admission_profile                    = json::object();
    data.admission_profile["profile"]         = "strict";
    data.policy_snapshot                      = json::object();
    data.policy_snapshot["network"]           = false;
    data.dependency_receipts                  = json::array();
    data.ownership_scope                      = "tenant:test";
    data.core_materialization_receipt         = json::object();
    data.core_materialization_receipt["plan"] = "core-plan:test";

    ProgramVersion version(data);
    data.ownership_scope = "mutated";
    EXPECT_EQ(version.ownership_scope(), "tenant:test");
    EXPECT_EQ(version.id().substr(0, 7), "sha256:");

    const auto bytes  = version.serialize_canonical();
    const auto parsed = ProgramVersion::parse(bytes);
    EXPECT_EQ(parsed.serialize_canonical(), bytes);
    EXPECT_EQ(parsed.id(), version.id());
    EXPECT_EQ(parsed.bundle_id(), bundle.id());

    auto changed_data            = data;
    changed_data.bundle_id       = bundle.id();
    changed_data.ownership_scope = "tenant:other";
    ProgramVersion changed(std::move(changed_data));
    EXPECT_NE(changed.id(), version.id());

    auto future                      = json::parse(bytes);
    future["storage_schema_version"] = 2;
    EXPECT_THROW(ProgramVersion::parse(future.dump()), std::invalid_argument);

    auto unknown       = json::parse(bytes);
    unknown["unknown"] = true;
    EXPECT_THROW(ProgramVersion::parse(unknown.dump()), std::invalid_argument);
}

}  // namespace
