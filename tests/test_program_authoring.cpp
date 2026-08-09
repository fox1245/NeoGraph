#include <neograph/program/authoring.h>

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

using namespace neograph::program;
using neograph::json;

TEST(ProgramAuthoringBoundary, JavaScriptPublicationUsesTheProgramSourceEnvelope) {
    const auto source = make_javascript_source(
        {"direct:control.js", "export function define() { return ng.graph('main'); }"});

    EXPECT_EQ(source.kind(), SourceKind::JavaScript);
    EXPECT_EQ(source.document().at("language"), "javascript");
    EXPECT_EQ(source.document().at("engine"), "quickjs");
    EXPECT_EQ(source.document().at("source"),
              "export function define() { return ng.graph('main'); }");
    EXPECT_EQ(authoring_frontend_from_string("javascript"), AuthoringFrontend::JavaScript);
    EXPECT_EQ(to_string(AuthoringFrontend::JavaScript), "javascript");
}

TEST(ProgramAuthoringBoundary, LegacyStoredArtifactRulesAreExplicitAndFailClosed) {
    const auto translated =
        classify_stored_artifact(StoredArtifactKind::LegacyCoreDefinition, true, false, false);
    EXPECT_EQ(translated.classification, StoredArtifactClassification::Translated);
    EXPECT_TRUE(translated.allows_new_publication);
    EXPECT_TRUE(translated.allows_new_run);
    EXPECT_FALSE(translated.requires_legacy_runtime);
    EXPECT_TRUE(translated.diagnostic_code.empty());

    const auto draining =
        classify_stored_artifact(StoredArtifactKind::LegacyProgramVersion, false, true, true);
    EXPECT_EQ(draining.classification, StoredArtifactClassification::DrainOnly);
    EXPECT_FALSE(draining.allows_new_publication);
    EXPECT_FALSE(draining.allows_new_run);
    EXPECT_TRUE(draining.requires_legacy_runtime);
    EXPECT_EQ(draining.diagnostic_code, "P_MIGRATION_DRAIN_ONLY");
    EXPECT_EQ(draining.kind, StoredArtifactKind::LegacyProgramVersion);

    json encoded;
    to_json(encoded, draining);
    StoredArtifactClassificationRule restored;
    from_json(encoded, restored);
    EXPECT_EQ(restored, draining);

    const auto rejected =
        classify_stored_artifact(StoredArtifactKind::LegacyProgramVersion, false, false, true);
    EXPECT_EQ(rejected.classification, StoredArtifactClassification::Rejected);
    EXPECT_EQ(rejected.diagnostic_code, "P_MIGRATION_PROGRAM_REJECTED");

    EXPECT_EQ(classify_stored_artifact(StoredArtifactKind::LegacyCoreDefinition, false, true, false)
                  .classification,
              StoredArtifactClassification::Rejected);
    EXPECT_EQ(classify_stored_artifact(StoredArtifactKind::LegacyCoreDefinition, false, false, true)
                  .classification,
              StoredArtifactClassification::Rejected);

    auto invalid           = draining;
    invalid.allows_new_run = true;
    EXPECT_THROW(to_json(encoded, invalid), std::invalid_argument);
}

TEST(ProgramAuthoringBoundary, RemovedLegacyFrontendsAreNotParseable) {
    EXPECT_EQ(authoring_frontend_from_string("trusted_cpp"), AuthoringFrontend::TrustedCpp);
    EXPECT_EQ(to_string(AuthoringFrontend::TrustedCpp), "trusted_cpp");
    EXPECT_THROW(authoring_frontend_from_string("core_dsl"), std::invalid_argument);
    EXPECT_THROW(authoring_frontend_from_string("strict_core_json"), std::invalid_argument);
    EXPECT_THROW(authoring_frontend_from_string("program_json"), std::invalid_argument);
    EXPECT_EQ(to_string(StoredArtifactClassification::DrainOnly), "drain_only");
    EXPECT_EQ(stored_artifact_classification_from_string("rejected"),
              StoredArtifactClassification::Rejected);
}

}  // namespace
