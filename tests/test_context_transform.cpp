#include <gtest/gtest.h>

#include <neograph/context_transform.h>

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

ContextArtifact artifact(ContextArtifactKind kind, std::string text,
                         bool required, char digest) {
    ContextArtifactData data;
    data.kind = kind;
    data.producer_id = "transform-test.v1";
    data.source_digest = sha(digest);
    data.media_type = "text/markdown";
    data.required = required;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

ContextEpoch epoch(const std::vector<ContextArtifact>& artifacts) {
    ContextEpochData data;
    data.run_id = "transform-run";
    data.sequence = 1;
    data.raw_window_digest = sha('a');
    for (const auto& value : artifacts) data.artifact_ids.push_back(value.id());
    return ContextEpoch::create(std::move(data));
}

}  // namespace

TEST(ContextTransformReceipt, PreservesRequiredArtifactsAndAllowsDerivedEvidence) {
    const auto constraint = artifact(ContextArtifactKind::HardConstraint,
                                     "Never skip verification.", true, 'b');
    const auto evidence = artifact(ContextArtifactKind::DerivedContext,
                                   "Long evidence", false, 'c');
    const auto compressed = artifact(ContextArtifactKind::DerivedContext,
                                     "Short evidence", false, 'd');
    const auto source = epoch({constraint, evidence});
    ContextTransformReceiptData data;
    data.source_context_epoch_id = source.id();
    data.transformer_identity = sha('e');
    data.input_artifact_ids = source.artifact_ids();
    data.output_artifact_ids = {constraint.id(), compressed.id()};
    data.preserved_required_artifact_ids = {constraint.id()};
    const auto receipt = ContextTransformReceipt::create(
        std::move(data), source, {constraint, evidence},
        {constraint, compressed});
    const auto parsed = ContextTransformReceipt::parse(
        receipt.serialize_canonical(), source, {constraint, evidence},
        {constraint, compressed});
    EXPECT_EQ(parsed.id(), receipt.id());
    EXPECT_EQ(parsed.preserved_required_artifact_ids(),
              std::vector<std::string>{constraint.id()});
}

TEST(ContextTransformReceipt, RejectsDroppedOrParaphrasedRequiredArtifact) {
    const auto constraint = artifact(ContextArtifactKind::HardConstraint,
                                     "Never skip verification.", true, 'b');
    const auto paraphrase = artifact(ContextArtifactKind::DerivedContext,
                                     "Verification is recommended.", false, 'c');
    const auto source = epoch({constraint});
    ContextTransformReceiptData dropped;
    dropped.source_context_epoch_id = source.id();
    dropped.transformer_identity = sha('d');
    dropped.input_artifact_ids = source.artifact_ids();
    dropped.output_artifact_ids = {paraphrase.id()};
    dropped.preserved_required_artifact_ids = {constraint.id()};
    EXPECT_THROW(ContextTransformReceipt::create(
                     std::move(dropped), source, {constraint}, {paraphrase}),
                 std::invalid_argument);

    ContextTransformReceiptData forged;
    forged.source_context_epoch_id = source.id();
    forged.transformer_identity = sha('d');
    forged.input_artifact_ids = source.artifact_ids();
    forged.output_artifact_ids = {constraint.id()};
    EXPECT_THROW(ContextTransformReceipt::create(
                     std::move(forged), source, {constraint}, {constraint}),
                 std::invalid_argument);
}

