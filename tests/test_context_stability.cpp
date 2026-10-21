// The data model of #329: a stability class and an anchor mode on an artifact, an order and
// a prefix claim on an epoch, and the prefix fields of version 2 of the assembly receipt.
// Nothing here changes what the assembler writes; these tests pin the stored values.
#include <neograph/runtime_context.h>

#include "json_schema_lite.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef NEOGRAPH_SOURCE_DIR
#error "NEOGRAPH_SOURCE_DIR must name the repository root"
#endif

using namespace neograph;

namespace {

std::string sha(char digit) { return "sha256:" + std::string(64, digit); }

// Stored by the code before the stability class existed (artifacts and epochs of version 1,
// receipts of version 1): the canonical bytes and the identity it gave them.
constexpr const char* kSkillV1Id = "sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a";
constexpr const char* kSkillV1 = R"json({"content":{"name":"secure-edit","text":"Verify before editing."},"content_digest":"sha256:c3779c8fbb58152f2e24b9e8ec5fe4d67e7a585220894b01cd1d37c1ad534d36","covers_from_sequence":0,"covers_through_sequence":0,"format":"neograph-context-artifact","id":"sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","kind":"required_skill","media_type":"text/markdown","placement":"before_latest_user","priority":100,"producer_id":"skill-resolver.v1","required":true,"source_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","source_feed_id":"","storage_schema_version":1})json";
constexpr const char* kSummaryV1Id = "sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0";
constexpr const char* kSummaryV1 = R"json({"content":{"text":"Summary of earlier turns."},"content_digest":"sha256:3d56bba71b7b71a84187238fce1ff29e21d15ffe3984023b587cedf193b54a09","covers_from_sequence":1,"covers_through_sequence":6,"format":"neograph-context-artifact","id":"sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0","kind":"derived_context","media_type":"text/plain","placement":"before_history","priority":5,"producer_id":"summarizer.v1","required":false,"source_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","source_feed_id":"hist_test","storage_schema_version":1})json";
constexpr const char* kEpochV1Id = "sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85";
constexpr const char* kEpochV1 = R"json({"artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0"],"feed_id":"hist_test","format":"neograph-context-epoch","guarantee_profile":"recorded","id":"sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85","raw_from_sequence":1,"raw_through_sequence":42,"raw_window_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","run_id":"run_test","sequence":1,"storage_schema_version":1})json";
constexpr const char* kReceiptV1Id = "sha256:4d2d8c0f8b669f4d2043a753fd7cba98b8311902a19e2c51a74f704493bfd120";
constexpr const char* kReceiptV1 = R"json({"artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a","sha256:8ad0c63f35275ea9af057e40aad544551b564b79dbd4d386377362c8f19ec2a0"],"context_epoch_id":"sha256:2c8465c9c6f9b883e64db20d05fc343b69b3d9cc10c936102a2cf2c336beef85","estimated_input_tokens":900,"format":"neograph-context-assembly-receipt","id":"sha256:4d2d8c0f8b669f4d2043a753fd7cba98b8311902a19e2c51a74f704493bfd120","mandatory_input_tokens":200,"message_window_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","normalized_request_digest":"sha256:2222222222222222222222222222222222222222222222222222222222222222","raw_from_sequence":1,"raw_through_sequence":42,"required_skill_artifact_ids":["sha256:31534e35691282ffb8a86fb811dbb8795ae3282cf28b364dba3e314679b4f48a"],"storage_schema_version":1})json";

ContextArtifactData skill_data() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::RequiredSkill;
    data.producer_id = "skill-resolver.v1";
    data.source_digest = sha('a');
    data.media_type = "text/markdown";
    data.placement = ContextPlacement::BeforeLatestUser;
    data.priority = 100;
    data.required = true;
    data.content = json{{"name", "secure-edit"}, {"text", "Verify before editing."}};
    return data;
}

ContextArtifactData summary_data() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::DerivedContext;
    data.producer_id = "summarizer.v1";
    data.source_digest = sha('b');
    data.source_feed_id = "hist_test";
    data.covers_from_sequence = 1;
    data.covers_through_sequence = 6;
    data.media_type = "text/plain";
    data.placement = ContextPlacement::BeforeHistory;
    data.priority = 5;
    data.content = json{{"text", "Summary of earlier turns."}};
    return data;
}

ContextEpochData epoch_v1_data(std::vector<std::string> artifact_ids) {
    ContextEpochData data;
    data.run_id = "run_test";
    data.sequence = 1;
    data.feed_id = "hist_test";
    data.raw_from_sequence = 1;
    data.raw_through_sequence = 42;
    data.raw_window_digest = sha('d');
    data.artifact_ids = std::move(artifact_ids);
    data.guarantee_profile = RuntimeGuaranteeProfile::Recorded;
    return data;
}

ContextAssemblyReceiptData receipt_v1_data(const ContextEpoch& epoch,
                                           const std::vector<ContextArtifact>& artifacts) {
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = sha('2');
    data.message_window_digest = sha('3');
    data.artifact_ids = epoch.artifact_ids();
    for (const auto& artifact : artifacts) {
        if (artifact.kind() == ContextArtifactKind::RequiredSkill) {
            data.required_skill_artifact_ids.push_back(artifact.id());
        }
    }
    data.raw_from_sequence = 1;
    data.raw_through_sequence = 42;
    data.estimated_input_tokens = 900;
    data.mandatory_input_tokens = 200;
    return data;
}

ContextArtifact artifact_of(ContextArtifactKind kind, std::string producer,
                            std::optional<StabilityClass> declared = std::nullopt) {
    ContextArtifactData data;
    data.kind = kind;
    data.producer_id = std::move(producer);
    data.source_digest = sha('c');
    data.media_type = "text/plain";
    data.required = kind == ContextArtifactKind::RequiredSkill ||
                    kind == ContextArtifactKind::HardConstraint;
    data.content = "text";
    data.stability_class = declared;
    return ContextArtifact::create(std::move(data));
}

json parsed(const std::string& bytes) { return json::parse(bytes); }

json without(const json& value, const std::string& removed) {
    auto result = json::object();
    for (const auto& [key, field] : value.items()) {
        if (key != removed) result[key] = field;
    }
    return result;
}

void expect_refusal(ContextStabilityRefusal code, const std::function<void()>& call) {
    try {
        call();
    } catch (const ContextStabilityError& error) {
        EXPECT_EQ(error.code(), code) << error.what();
        EXPECT_NE(dynamic_cast<const std::invalid_argument*>(&error), nullptr);
        return;
    } catch (const std::exception& error) {
        ADD_FAILURE() << "expected a ContextStabilityError, got: " << error.what();
        return;
    }
    ADD_FAILURE() << "expected a ContextStabilityError, nothing was thrown";
}

json read_schema(const std::string& name) {
    std::ifstream input(std::filesystem::path(NEOGRAPH_SOURCE_DIR) / "schemas" / name, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open schema " + name);
    return json::parse(std::string(std::istreambuf_iterator<char>(input), {}));
}

std::string violations(const std::vector<std::string>& errors) {
    std::string joined;
    for (const auto& error : errors) joined += "\n  " + error;
    return joined;
}

// A run of epochs over one feed for the claim tests.
struct Turn {
    ContextEpoch epoch;
    std::vector<ContextArtifact> artifacts;
};

Turn turn(std::uint64_t sequence, std::optional<std::string> predecessor, std::uint64_t from,
          std::uint64_t through, std::vector<ContextArtifact> ordered,
          std::optional<ContextPrefixClaim> claim,
          std::optional<ContextBreakReason> reason = std::nullopt, bool record_order = true,
          std::string run = "run_claim") {
    ContextEpochData data;
    data.run_id = std::move(run);
    data.sequence = sequence;
    data.predecessor_id = std::move(predecessor);
    data.feed_id = "hist_claim";
    data.raw_from_sequence = from;
    data.raw_through_sequence = through;
    data.raw_window_digest = sha('e');
    std::vector<std::string> ids;
    for (const auto& artifact : ordered) ids.push_back(artifact.id());
    data.artifact_ids = ids;
    if (record_order) data.ordered_artifact_ids = ids;
    data.prefix_claim = claim;
    data.break_reason = reason;
    return {ContextEpoch::create(std::move(data)), std::move(ordered)};
}

ContextArtifact claim_skill(std::string text = "Verify before editing.") {
    auto data = skill_data();
    data.content = json{{"text", std::move(text)}};
    return ContextArtifact::create(std::move(data));
}

// A compaction summary: Slow by derivation.
ContextArtifact claim_summary() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::DerivedContext;
    data.producer_id = std::string(COMPACTION_PRODUCER_ID);
    data.source_digest = sha('b');
    data.source_feed_id = "hist_claim";
    data.covers_from_sequence = 1;
    data.covers_through_sequence = 3;
    data.media_type = "text/plain";
    data.placement = ContextPlacement::BeforeHistory;
    data.content = "Summary of the first turns.";
    return ContextArtifact::create(std::move(data));
}

ContextArtifact claim_note(std::string text) {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::HookOutput;
    data.producer_id = "hook-output.v1";
    data.source_digest = sha('c');
    data.media_type = "text/plain";
    data.placement = ContextPlacement::AfterHistory;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

// A retrieved block that stays after the record it covers.
ContextArtifact claim_anchor(std::uint64_t through, std::string feed = "hist_claim") {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::UntrustedSupplemental;
    data.producer_id = "retrieval.v1";
    data.source_digest = sha('d');
    data.source_feed_id = std::move(feed);
    data.covers_from_sequence = through;
    data.covers_through_sequence = through;
    data.media_type = "text/plain";
    data.placement = ContextPlacement::AfterHistory;
    data.content = "Retrieved for record " + std::to_string(through);
    data.anchor_mode = AnchorMode::KeepInPlace;
    return ContextArtifact::create(std::move(data));
}

}  // namespace

TEST(ContextStability, ValuesStoredBeforeTheClassExistedKeepTheirBytesAndIdentity) {
    const auto skill = ContextArtifact::parse(kSkillV1);
    const auto summary = ContextArtifact::parse(kSummaryV1);
    EXPECT_EQ(skill.id(), kSkillV1Id);
    EXPECT_EQ(summary.id(), kSummaryV1Id);
    EXPECT_EQ(skill.serialize_canonical(), kSkillV1);
    EXPECT_EQ(summary.serialize_canonical(), kSummaryV1);
    // The current code builds the same artifact byte for byte when it declares nothing.
    EXPECT_EQ(ContextArtifact::create(skill_data()).serialize_canonical(), kSkillV1);
    EXPECT_EQ(ContextArtifact::create(summary_data()).serialize_canonical(), kSummaryV1);
    // The class is read from what is stored, never written into it.
    EXPECT_FALSE(skill.declared_stability_class());
    EXPECT_FALSE(skill.declared_anchor_mode());
    EXPECT_EQ(skill.stability_class(), StabilityClass::Stable);
    EXPECT_EQ(summary.stability_class(), StabilityClass::Volatile);
    EXPECT_EQ(skill.anchor_mode(), AnchorMode::Replace);

    const auto epoch = ContextEpoch::parse(kEpochV1);
    EXPECT_EQ(epoch.id(), kEpochV1Id);
    EXPECT_EQ(epoch.serialize_canonical(), kEpochV1);
    EXPECT_EQ(ContextEpoch::create(epoch_v1_data({skill.id(), summary.id()})).serialize_canonical(),
              kEpochV1);
    EXPECT_FALSE(epoch.ordered_artifact_ids());
    EXPECT_FALSE(epoch.prefix_claim());
    EXPECT_FALSE(epoch.break_reason());

    const std::vector<ContextArtifact> artifacts{skill, summary};
    const auto receipt = ContextAssemblyReceipt::parse(kReceiptV1, epoch, artifacts);
    EXPECT_EQ(receipt.id(), kReceiptV1Id);
    EXPECT_EQ(receipt.serialize_canonical(), kReceiptV1);
    EXPECT_EQ(ContextAssemblyReceipt::create(receipt_v1_data(epoch, artifacts), epoch, artifacts)
                  .serialize_canonical(),
              kReceiptV1);
    // A version 1 receipt does not know its prefix: nothing is recorded, nothing fails.
    EXPECT_FALSE(receipt.assembled_artifact_ids());
    EXPECT_FALSE(receipt.artifact_classes());
    EXPECT_FALSE(receipt.encoder_family());
    EXPECT_FALSE(receipt.stable_boundary());
    EXPECT_FALSE(receipt.prefix_boundary());
    EXPECT_FALSE(receipt.prefix_digest());
    EXPECT_FALSE(receipt.tools_digest());
    EXPECT_FALSE(receipt.break_reason());
}

TEST(ContextStability, TheClassOfAnArtifactThatDeclaresNoneIsDerivedFromItsKindAndProducer) {
    struct Case {
        ContextArtifactKind kind;
        std::string producer;
        StabilityClass expected;
    };
    const std::vector<Case> cases{
        {ContextArtifactKind::RequiredSkill, "skill-resolver.v1", StabilityClass::Stable},
        {ContextArtifactKind::HardConstraint, "host-policy.v1", StabilityClass::Stable},
        {ContextArtifactKind::DerivedContext, std::string(COMPACTION_PRODUCER_ID), StabilityClass::Slow},
        {ContextArtifactKind::DerivedContext, "summarizer.v1", StabilityClass::Volatile},
        {ContextArtifactKind::HookOutput, "hook-output.v1", StabilityClass::Volatile},
        {ContextArtifactKind::UntrustedSupplemental, "retrieval.v1", StabilityClass::Volatile},
        {ContextArtifactKind::RawHistory, "raw-copy.v1", StabilityClass::Volatile},
        // The compaction producer counts only for derived context.
        {ContextArtifactKind::HookOutput, std::string(COMPACTION_PRODUCER_ID), StabilityClass::Volatile},
    };
    for (const auto& item : cases) {
        const auto artifact = artifact_of(item.kind, item.producer);
        EXPECT_EQ(derive_stability_class(item.kind, item.producer), item.expected)
            << to_string(item.kind) << " " << item.producer;
        EXPECT_EQ(artifact.stability_class(), item.expected) << to_string(item.kind) << " " << item.producer;
        EXPECT_FALSE(artifact.declared_stability_class());
    }
}

TEST(ContextStability, ADeclaredClassAndAnchorModeAreInTheIdentityOnlyWhenSet) {
    const auto plain = ContextArtifact::create(summary_data());
    EXPECT_EQ(parsed(plain.serialize_canonical()).at("storage_schema_version"), 1);
    EXPECT_FALSE(parsed(plain.serialize_canonical()).contains("stability_class"));
    EXPECT_FALSE(parsed(plain.serialize_canonical()).contains("anchor_mode"));

    auto slow_data = summary_data();
    slow_data.stability_class = StabilityClass::Slow;
    const auto slow = ContextArtifact::create(slow_data);
    EXPECT_NE(slow.id(), plain.id());
    EXPECT_EQ(slow.stability_class(), StabilityClass::Slow);
    EXPECT_EQ(slow.declared_stability_class(), StabilityClass::Slow);
    EXPECT_EQ(parsed(slow.serialize_canonical()).at("storage_schema_version"), 2);
    EXPECT_EQ(parsed(slow.serialize_canonical()).at("stability_class"), "slow");
    EXPECT_EQ(ContextArtifact::parse(slow.serialize_canonical()).id(), slow.id());
    EXPECT_EQ(ContextArtifact::parse(slow.serialize_canonical()).declared_stability_class(),
              StabilityClass::Slow);

    // Declaring the class the artifact would derive is still a declaration.
    auto volatile_data = summary_data();
    volatile_data.stability_class = StabilityClass::Volatile;
    const auto declared_volatile = ContextArtifact::create(volatile_data);
    EXPECT_EQ(declared_volatile.stability_class(), plain.stability_class());
    EXPECT_NE(declared_volatile.id(), plain.id());
    EXPECT_NE(declared_volatile.id(), slow.id());

    auto anchored_data = summary_data();
    anchored_data.anchor_mode = AnchorMode::KeepInPlace;
    const auto anchored = ContextArtifact::create(anchored_data);
    EXPECT_NE(anchored.id(), plain.id());
    EXPECT_EQ(anchored.anchor_mode(), AnchorMode::KeepInPlace);
    EXPECT_EQ(plain.anchor_mode(), AnchorMode::Replace);
    EXPECT_FALSE(plain.declared_anchor_mode());
    EXPECT_EQ(parsed(anchored.serialize_canonical()).at("anchor_mode"), "keep_in_place");
    EXPECT_EQ(ContextArtifact::parse(anchored.serialize_canonical()).declared_anchor_mode(),
              AnchorMode::KeepInPlace);
    // An explicit Replace is set, so it is in the identity.
    auto replace_data = summary_data();
    replace_data.anchor_mode = AnchorMode::Replace;
    EXPECT_NE(ContextArtifact::create(replace_data).id(), plain.id());

    // Both together.
    anchored_data.stability_class = StabilityClass::Slow;
    const auto both = ContextArtifact::create(anchored_data);
    EXPECT_NE(both.id(), anchored.id());
    EXPECT_NE(both.id(), slow.id());
    const auto reparsed = ContextArtifact::parse(both.serialize_canonical());
    EXPECT_EQ(reparsed.id(), both.id());
    EXPECT_EQ(reparsed.stability_class(), StabilityClass::Slow);
    EXPECT_EQ(reparsed.anchor_mode(), AnchorMode::KeepInPlace);

    // A body of version 1 cannot carry a field of version 2, and a body of version 2 needs one.
    auto forged = parsed(plain.serialize_canonical());
    forged["stability_class"] = "stable";
    EXPECT_THROW(ContextArtifact::parse(forged.dump()), std::invalid_argument);
    auto empty_version_2 = parsed(plain.serialize_canonical());
    empty_version_2["storage_schema_version"] = 2;
    EXPECT_THROW(ContextArtifact::parse(empty_version_2.dump()), std::invalid_argument);
    auto unknown = parsed(slow.serialize_canonical());
    unknown["stability_class"] = "eternal";
    EXPECT_THROW(ContextArtifact::parse(unknown.dump()), std::invalid_argument);
    auto retagged = parsed(slow.serialize_canonical());
    retagged["stability_class"] = "stable";
    EXPECT_THROW(ContextArtifact::parse(retagged.dump()), std::invalid_argument);
}

TEST(ContextStability, AVolatileArtifactOfASystemKindIsRefusedWithATypedError) {
    for (const auto kind : {ContextArtifactKind::RequiredSkill, ContextArtifactKind::HardConstraint}) {
        EXPECT_TRUE(context_artifact_kind_renders_as_system(kind));
        expect_refusal(ContextStabilityRefusal::VolatileSystemArtifact, [&] {
            (void)artifact_of(kind, "producer.v1", StabilityClass::Volatile);
        });
        EXPECT_THROW((void)artifact_of(kind, "producer.v1", StabilityClass::Volatile),
                     std::invalid_argument);
        EXPECT_EQ(artifact_of(kind, "producer.v1", StabilityClass::Stable).stability_class(),
                  StabilityClass::Stable);
        EXPECT_EQ(artifact_of(kind, "producer.v1", StabilityClass::Slow).stability_class(),
                  StabilityClass::Slow);
    }
    for (const auto kind : {ContextArtifactKind::DerivedContext, ContextArtifactKind::HookOutput,
                            ContextArtifactKind::UntrustedSupplemental, ContextArtifactKind::RawHistory}) {
        EXPECT_FALSE(context_artifact_kind_renders_as_system(kind));
        EXPECT_EQ(artifact_of(kind, "producer.v1", StabilityClass::Volatile).stability_class(),
                  StabilityClass::Volatile);
    }
    // A value outside the enumeration is not a class.
    EXPECT_THROW((void)artifact_of(ContextArtifactKind::HookOutput, "producer.v1",
                                   static_cast<StabilityClass>(255)),
                 std::invalid_argument);
    auto bad_mode = summary_data();
    bad_mode.anchor_mode = static_cast<AnchorMode>(255);
    EXPECT_THROW(ContextArtifact::create(std::move(bad_mode)), std::invalid_argument);
}

TEST(ContextStability, AnEpochRecordsAnOrderThatIsAPermutationOfItsArtifacts) {
    const auto a = sha('1');
    const auto b = sha('2');
    const auto c = sha('3');
    auto data = epoch_v1_data({a, b, c});
    const auto legacy = ContextEpoch::create(data);
    EXPECT_EQ(parsed(legacy.serialize_canonical()).at("storage_schema_version"), 1);

    data.ordered_artifact_ids = std::vector<std::string>{c, a, b};
    const auto ordered = ContextEpoch::create(data);
    EXPECT_EQ(ordered.artifact_ids(), (std::vector<std::string>{a, b, c}));
    EXPECT_EQ(ordered.ordered_artifact_ids(), (std::vector<std::string>{c, a, b}));
    EXPECT_NE(ordered.id(), legacy.id());
    EXPECT_EQ(parsed(ordered.serialize_canonical()).at("storage_schema_version"), 2);
    const auto reparsed = ContextEpoch::parse(ordered.serialize_canonical());
    EXPECT_EQ(reparsed.id(), ordered.id());
    EXPECT_EQ(reparsed.ordered_artifact_ids(), ordered.ordered_artifact_ids());

    // The order is part of the identity; the same order is the same epoch.
    auto other = data;
    other.ordered_artifact_ids = std::vector<std::string>{a, b, c};
    EXPECT_NE(ContextEpoch::create(other).id(), ordered.id());
    EXPECT_EQ(ContextEpoch::create(data).id(), ordered.id());

    for (const auto& invalid : std::vector<std::vector<std::string>>{
             {a, b}, {a, b, c, sha('4')}, {a, a, b}, {a, b, "not-an-identity"}}) {
        auto broken = data;
        broken.ordered_artifact_ids = invalid;
        EXPECT_THROW(ContextEpoch::create(broken), std::invalid_argument);
    }
    // An epoch with no artifacts may record the empty order.
    ContextEpochData empty = epoch_v1_data({});
    empty.ordered_artifact_ids = std::vector<std::string>{};
    EXPECT_TRUE(ContextEpoch::create(empty).ordered_artifact_ids());

    auto forged = parsed(legacy.serialize_canonical());
    forged["ordered_artifact_ids"] = json::array({c, a, b});
    EXPECT_THROW(ContextEpoch::parse(forged.dump()), std::invalid_argument);
}

TEST(ContextStability, AnEpochClaimIsWellFormedBeforeAnyPredecessorIsLoaded) {
    const auto a = sha('1');
    auto data = epoch_v1_data({a});
    data.sequence = 2;
    data.predecessor_id = sha('9');
    data.ordered_artifact_ids = std::vector<std::string>{a};

    auto extends = data;
    extends.prefix_claim = ContextPrefixClaim::Extends;
    const auto extending = ContextEpoch::create(extends);
    EXPECT_EQ(extending.prefix_claim(), ContextPrefixClaim::Extends);
    EXPECT_FALSE(extending.break_reason());
    EXPECT_EQ(ContextEpoch::parse(extending.serialize_canonical()).prefix_claim(), ContextPrefixClaim::Extends);

    auto breaks = data;
    breaks.prefix_claim = ContextPrefixClaim::Breaks;
    breaks.break_reason = ContextBreakReason::Compaction;
    const auto breaking = ContextEpoch::create(breaks);
    EXPECT_EQ(breaking.break_reason(), ContextBreakReason::Compaction);
    EXPECT_EQ(ContextEpoch::parse(breaking.serialize_canonical()).break_reason(),
              ContextBreakReason::Compaction);
    // A claim that breaks needs no order and no predecessor.
    auto first_break = epoch_v1_data({a});
    first_break.prefix_claim = ContextPrefixClaim::Breaks;
    first_break.break_reason = ContextBreakReason::LayoutChange;
    EXPECT_NO_THROW(ContextEpoch::create(first_break));

    // Every reason can be named, and a different reason is a different epoch.
    std::set<std::string> ids;
    for (const auto reason : {ContextBreakReason::Compaction, ContextBreakReason::StableChange,
                              ContextBreakReason::LayoutChange, ContextBreakReason::AnchorDrop,
                              ContextBreakReason::OrderChange, ContextBreakReason::Explicit}) {
        auto named = breaks;
        named.break_reason = reason;
        const auto epoch = ContextEpoch::create(named);
        EXPECT_EQ(ContextEpoch::parse(epoch.serialize_canonical()).break_reason(), reason);
        ids.insert(epoch.id());
    }
    EXPECT_EQ(ids.size(), 6u);

    // The first epoch of a run has no predecessor to extend.
    auto first = epoch_v1_data({a});
    first.ordered_artifact_ids = std::vector<std::string>{a};
    first.prefix_claim = ContextPrefixClaim::Extends;
    expect_refusal(ContextStabilityRefusal::PredecessorUnavailable,
                   [&] { (void)ContextEpoch::create(first); });
    // An extension without a recorded order cannot be checked.
    auto unordered = extends;
    unordered.ordered_artifact_ids.reset();
    EXPECT_THROW(ContextEpoch::create(unordered), std::invalid_argument);
    // A break names its reason, and only a break does.
    auto reasonless = breaks;
    reasonless.break_reason.reset();
    EXPECT_THROW(ContextEpoch::create(reasonless), std::invalid_argument);
    auto reason_on_extension = extends;
    reason_on_extension.break_reason = ContextBreakReason::Explicit;
    EXPECT_THROW(ContextEpoch::create(reason_on_extension), std::invalid_argument);
    auto reason_without_claim = data;
    reason_without_claim.break_reason = ContextBreakReason::Explicit;
    EXPECT_THROW(ContextEpoch::create(reason_without_claim), std::invalid_argument);
    auto bad_claim = data;
    bad_claim.prefix_claim = static_cast<ContextPrefixClaim>(255);
    EXPECT_THROW(ContextEpoch::create(bad_claim), std::invalid_argument);
    auto bad_reason = breaks;
    bad_reason.break_reason = static_cast<ContextBreakReason>(255);
    EXPECT_THROW(ContextEpoch::create(bad_reason), std::invalid_argument);
}

TEST(ContextStability, AReceiptOfVersion2RoundTripsItsPrefixFields) {
    const auto skill = ContextArtifact::create(skill_data());
    const auto summary = ContextArtifact::create(summary_data());
    const std::vector<ContextArtifact> artifacts{skill, summary};
    const auto epoch = ContextEpoch::create(epoch_v1_data({skill.id(), summary.id()}));
    const auto legacy = ContextAssemblyReceipt::create(receipt_v1_data(epoch, artifacts), epoch, artifacts);
    EXPECT_EQ(parsed(legacy.serialize_canonical()).at("storage_schema_version"), 1);

    auto data = receipt_v1_data(epoch, artifacts);
    data.assembled_artifact_ids = std::vector<std::string>{summary.id(), skill.id()};
    data.artifact_classes = std::map<std::string, StabilityClass>{
        {skill.id(), StabilityClass::Stable}, {summary.id(), StabilityClass::Volatile}};
    data.encoder_family = "openai.chat";
    data.tools_digest = sha('4');
    const auto receipt = ContextAssemblyReceipt::create(data, epoch, artifacts);
    EXPECT_NE(receipt.id(), legacy.id());
    const auto stored = parsed(receipt.serialize_canonical());
    EXPECT_EQ(stored.at("storage_schema_version"), 2);
    EXPECT_EQ(stored.at("assembled_artifact_ids"), json::array({summary.id(), skill.id()}));
    EXPECT_EQ(stored.at("artifact_classes").at(skill.id()), "stable");
    EXPECT_EQ(stored.at("artifact_classes").at(summary.id()), "volatile");
    EXPECT_EQ(stored.at("encoder_family"), "openai.chat");
    // The boundaries and the prefix digest are placeholders until the assembler marks them.
    EXPECT_FALSE(stored.contains("stable_boundary"));
    EXPECT_FALSE(stored.contains("prefix_boundary"));
    EXPECT_FALSE(stored.contains("prefix_digest"));
    EXPECT_FALSE(stored.contains("break_reason"));

    const auto reparsed = ContextAssemblyReceipt::parse(receipt.serialize_canonical(), epoch, artifacts);
    EXPECT_EQ(reparsed.id(), receipt.id());
    EXPECT_EQ(reparsed.assembled_artifact_ids(), receipt.assembled_artifact_ids());
    EXPECT_EQ(reparsed.artifact_classes(), receipt.artifact_classes());
    EXPECT_EQ(reparsed.encoder_family(), std::optional<std::string>("openai.chat"));
    EXPECT_EQ(reparsed.tools_digest(), std::optional<std::string>(sha('4')));
    EXPECT_FALSE(reparsed.stable_boundary());
    EXPECT_FALSE(reparsed.prefix_boundary());
    EXPECT_FALSE(reparsed.prefix_digest());

    // The fields the assembler fills later are part of the same version.
    data.stable_boundary = ContextPrefixBoundary{1, 0};
    data.prefix_boundary = ContextPrefixBoundary{3, 2};
    data.prefix_digest = sha('5');
    const auto marked = ContextAssemblyReceipt::create(data, epoch, artifacts);
    EXPECT_NE(marked.id(), receipt.id());
    const auto marked_again = ContextAssemblyReceipt::parse(marked.serialize_canonical(), epoch, artifacts);
    EXPECT_EQ(marked_again.stable_boundary(), std::optional<ContextPrefixBoundary>({1, 0}));
    EXPECT_EQ(marked_again.prefix_boundary(), std::optional<ContextPrefixBoundary>({3, 2}));
    EXPECT_EQ(marked_again.prefix_digest(), std::optional<std::string>(sha('5')));

    // A body of version 1 cannot carry a field of version 2.
    auto forged = parsed(legacy.serialize_canonical());
    forged["encoder_family"] = "openai.chat";
    EXPECT_THROW(ContextAssemblyReceipt::parse(forged.dump(), epoch, artifacts), std::invalid_argument);
    auto retagged = parsed(receipt.serialize_canonical());
    retagged["artifact_classes"][skill.id()] = "volatile";
    EXPECT_THROW(ContextAssemblyReceipt::parse(retagged.dump(), epoch, artifacts), std::invalid_argument);
}

TEST(ContextStability, AReceiptOfVersion2IsCheckedAgainstItsEpochAndArtifacts) {
    const auto skill = ContextArtifact::create(skill_data());
    const auto summary = ContextArtifact::create(summary_data());
    const std::vector<ContextArtifact> artifacts{skill, summary};
    const auto epoch = ContextEpoch::create(epoch_v1_data({skill.id(), summary.id()}));
    auto data = receipt_v1_data(epoch, artifacts);
    data.assembled_artifact_ids = std::vector<std::string>{skill.id(), summary.id()};
    data.artifact_classes = std::map<std::string, StabilityClass>{
        {skill.id(), StabilityClass::Stable}, {summary.id(), StabilityClass::Volatile}};
    ASSERT_NO_THROW(ContextAssemblyReceipt::create(data, epoch, artifacts));

    // The class table is the classes of the artifacts, derived or declared.
    auto wrong_class = data;
    (*wrong_class.artifact_classes)[summary.id()] = StabilityClass::Slow;
    EXPECT_THROW(ContextAssemblyReceipt::create(wrong_class, epoch, artifacts), std::invalid_argument);
    // The order and the table come together and cover exactly the artifacts.
    auto order_only = data;
    order_only.artifact_classes.reset();
    EXPECT_THROW(ContextAssemblyReceipt::create(order_only, epoch, artifacts), std::invalid_argument);
    auto table_only = data;
    table_only.assembled_artifact_ids.reset();
    EXPECT_THROW(ContextAssemblyReceipt::create(table_only, epoch, artifacts), std::invalid_argument);
    auto short_order = data;
    short_order.assembled_artifact_ids = std::vector<std::string>{skill.id()};
    EXPECT_THROW(ContextAssemblyReceipt::create(short_order, epoch, artifacts), std::invalid_argument);
    auto repeated_order = data;
    repeated_order.assembled_artifact_ids = std::vector<std::string>{skill.id(), skill.id()};
    EXPECT_THROW(ContextAssemblyReceipt::create(repeated_order, epoch, artifacts), std::invalid_argument);
    auto short_table = data;
    short_table.artifact_classes->erase(summary.id());
    EXPECT_THROW(ContextAssemblyReceipt::create(short_table, epoch, artifacts), std::invalid_argument);
    auto bad_class = data;
    (*bad_class.artifact_classes)[summary.id()] = static_cast<StabilityClass>(255);
    EXPECT_THROW(ContextAssemblyReceipt::create(bad_class, epoch, artifacts), std::invalid_argument);
    for (const auto& edit : std::vector<std::function<void(ContextAssemblyReceiptData&)>>{
             [](auto& d) { d.tools_digest = "tools"; }, [](auto& d) { d.prefix_digest = "prefix"; },
             [](auto& d) { d.encoder_family = ""; },
             [](auto& d) { d.break_reason = static_cast<ContextBreakReason>(255); }}) {
        auto broken = data;
        edit(broken);
        EXPECT_THROW(ContextAssemblyReceipt::create(broken, epoch, artifacts), std::invalid_argument);
    }

    // The receipt names the break its epoch claims.
    const auto breaking = ContextEpoch::create([&] {
        auto d = epoch_v1_data({skill.id(), summary.id()});
        d.sequence = 2;
        d.predecessor_id = sha('9');
        d.prefix_claim = ContextPrefixClaim::Breaks;
        d.break_reason = ContextBreakReason::Compaction;
        return d;
    }());
    auto for_break = data;
    for_break.context_epoch_id = breaking.id();
    EXPECT_THROW(ContextAssemblyReceipt::create(for_break, breaking, artifacts), std::invalid_argument);
    for_break.break_reason = ContextBreakReason::StableChange;
    EXPECT_THROW(ContextAssemblyReceipt::create(for_break, breaking, artifacts), std::invalid_argument);
    for_break.break_reason = ContextBreakReason::Compaction;
    const auto named = ContextAssemblyReceipt::create(for_break, breaking, artifacts);
    EXPECT_EQ(named.break_reason(), ContextBreakReason::Compaction);
    EXPECT_EQ(ContextAssemblyReceipt::parse(named.serialize_canonical(), breaking, artifacts).break_reason(),
              ContextBreakReason::Compaction);
    // The break reason of a receipt that claims one is not the epoch's when the epoch claims none.
    auto unclaimed = data;
    unclaimed.break_reason = ContextBreakReason::Compaction;
    EXPECT_THROW(ContextAssemblyReceipt::create(unclaimed, epoch, artifacts), std::invalid_argument);
    // A receipt of version 1 predates the claim and says nothing about it.
    auto for_legacy = receipt_v1_data(breaking, artifacts);
    EXPECT_NO_THROW(ContextAssemblyReceipt::create(for_legacy, breaking, artifacts));
}

TEST(ContextStability, AnEpochThatExtendsItsPredecessorIsAcceptedWhenTheStructureHolds) {
    const auto skill = claim_skill();
    const auto summary = claim_summary();
    ASSERT_EQ(summary.stability_class(), StabilityClass::Slow);
    const auto first = turn(1, std::nullopt, 1, 5, {skill, summary, claim_note("reading 1"), claim_anchor(4)},
                            std::nullopt);

    // Only the Volatile material changed, and the RAW window grew at its end.
    const auto next = turn(2, first.epoch.id(), 1, 6,
                           {skill, summary, claim_note("reading 2"), claim_anchor(4)},
                           ContextPrefixClaim::Extends);
    EXPECT_NO_THROW(verify_context_epoch_claim(next.epoch, next.artifacts, &first.epoch, first.artifacts));
    // Where the Volatile artifacts sit in the order is theirs to choose.
    const auto reordered = turn(2, first.epoch.id(), 1, 6,
                                {claim_note("reading 2"), claim_anchor(4), skill, summary},
                                ContextPrefixClaim::Extends);
    EXPECT_NO_THROW(verify_context_epoch_claim(reordered.epoch, reordered.artifacts, &first.epoch,
                                               first.artifacts));
    // A further epoch extends the one before it, not only the first.
    const auto third = turn(3, next.epoch.id(), 1, 9,
                            {skill, summary, claim_note("reading 3"), claim_anchor(4), claim_anchor(8)},
                            ContextPrefixClaim::Extends);
    EXPECT_NO_THROW(verify_context_epoch_claim(third.epoch, third.artifacts, &next.epoch, next.artifacts));

    // An epoch that claims nothing, or breaks, is not checked: a break costs a cache entry at most.
    const auto unclaimed = turn(2, first.epoch.id(), 4, 9, {skill}, std::nullopt);
    EXPECT_NO_THROW(verify_context_epoch_claim(unclaimed.epoch, unclaimed.artifacts, nullptr, {}));
    const auto breaking = turn(2, first.epoch.id(), 4, 9, {skill, claim_note("after")},
                               ContextPrefixClaim::Breaks, ContextBreakReason::Compaction);
    EXPECT_NO_THROW(verify_context_epoch_claim(breaking.epoch, breaking.artifacts, nullptr, {}));
}

TEST(ContextStability, AnEpochThatExtendsItsPredecessorIsRefusedWhenTheStructureDoesNotHold) {
    const auto skill = claim_skill();
    const auto summary = claim_summary();
    const auto note = claim_note("reading 1");
    const auto first = turn(1, std::nullopt, 1, 5, {skill, summary, note, claim_anchor(4)}, std::nullopt);
    const auto check = [&](const Turn& next, ContextStabilityRefusal code) {
        expect_refusal(code, [&] {
            verify_context_epoch_claim(next.epoch, next.artifacts, &first.epoch, first.artifacts);
        });
    };
    const auto extend = [&](std::uint64_t from, std::uint64_t through, std::vector<ContextArtifact> artifacts) {
        return turn(2, first.epoch.id(), from, through, std::move(artifacts), ContextPrefixClaim::Extends);
    };
    const auto next = extend(1, 6, {skill, summary, claim_note("reading 2"), claim_anchor(4)});
    ASSERT_NO_THROW(verify_context_epoch_claim(next.epoch, next.artifacts, &first.epoch, first.artifacts));

    // No predecessor to compare with.
    expect_refusal(ContextStabilityRefusal::PredecessorUnavailable,
                   [&] { verify_context_epoch_claim(next.epoch, next.artifacts, nullptr, {}); });
    // Another epoch than the one the claim names.
    const auto stranger = turn(1, std::nullopt, 1, 5, {skill, claim_note("stranger")}, std::nullopt);
    expect_refusal(ContextStabilityRefusal::PredecessorUnavailable, [&] {
        verify_context_epoch_claim(next.epoch, next.artifacts, &stranger.epoch, stranger.artifacts);
    });
    // A predecessor from before epochs recorded an order cannot be compared.
    const auto legacy = turn(1, std::nullopt, 1, 5, {skill, summary, note, claim_anchor(4)}, std::nullopt,
                             std::nullopt, false);
    const auto after_legacy = turn(2, legacy.epoch.id(), 1, 6, {skill, summary, claim_anchor(4)},
                                   ContextPrefixClaim::Extends);
    expect_refusal(ContextStabilityRefusal::PredecessorOrderUnknown, [&] {
        verify_context_epoch_claim(after_legacy.epoch, after_legacy.artifacts, &legacy.epoch, legacy.artifacts);
    });
    // The artifacts handed over are the artifacts the epochs select.
    expect_refusal(ContextStabilityRefusal::ArtifactsMismatch, [&] {
        verify_context_epoch_claim(next.epoch, {skill, summary}, &first.epoch, first.artifacts);
    });
    expect_refusal(ContextStabilityRefusal::ArtifactsMismatch, [&] {
        verify_context_epoch_claim(next.epoch, next.artifacts, &first.epoch, {skill});
    });

    // The RAW window starts where it started, and grows.
    check(extend(2, 6, {skill, summary, claim_anchor(4)}), ContextStabilityRefusal::RawWindowMoved);
    check(turn(2, first.epoch.id(), 1, 6, {skill, summary, claim_anchor(4)}, ContextPrefixClaim::Extends,
               std::nullopt, true, "another_run"),
          ContextStabilityRefusal::RawWindowMoved);
    check(extend(1, 5, {skill, summary, claim_anchor(4)}), ContextStabilityRefusal::RawWindowNotGrown);

    // The Stable and Slow artifacts are the same, in the same order.
    check(extend(1, 6, {skill, claim_anchor(4)}), ContextStabilityRefusal::StablePrefixChanged);
    check(extend(1, 6, {summary, skill, claim_anchor(4)}), ContextStabilityRefusal::StablePrefixChanged);
    check(extend(1, 6, {claim_skill("Verify twice."), summary, claim_anchor(4)}),
          ContextStabilityRefusal::StablePrefixChanged);
    check(extend(1, 6, {skill, summary, claim_skill("A second rule."), claim_anchor(4)}),
          ContextStabilityRefusal::StablePrefixChanged);
    // A Volatile artifact that is declared Stable is part of the prefix it joins.
    ContextArtifactData promoted;
    promoted.kind = ContextArtifactKind::HookOutput;
    promoted.producer_id = "hook-output.v1";
    promoted.source_digest = sha('c');
    promoted.media_type = "text/plain";
    promoted.content = "reading 2";
    promoted.stability_class = StabilityClass::Stable;
    check(extend(1, 6, {skill, summary, ContextArtifact::create(promoted), claim_anchor(4)}),
          ContextStabilityRefusal::StablePrefixChanged);

    // An anchor stays for as long as the window that holds the records it covers.
    check(extend(1, 6, {skill, summary, claim_note("reading 2")}), ContextStabilityRefusal::AnchorDropped);
    check(extend(1, 6, {skill, summary, claim_anchor(4), claim_anchor(9)}),
          ContextStabilityRefusal::AnchorOutOfRange);
    check(extend(1, 6, {skill, summary, claim_anchor(4), claim_anchor(5, "another_feed")}),
          ContextStabilityRefusal::AnchorOutOfRange);
    // An anchor whose records have left the window belongs to a break, not to an extension.
    const auto late_first = turn(1, std::nullopt, 3, 5, {skill, claim_anchor(4)}, std::nullopt);
    const auto late_next = turn(2, late_first.epoch.id(), 3, 6, {skill, claim_anchor(4), claim_anchor(2)},
                                ContextPrefixClaim::Extends);
    expect_refusal(ContextStabilityRefusal::AnchorOutOfRange, [&] {
        verify_context_epoch_claim(late_next.epoch, late_next.artifacts, &late_first.epoch,
                                   late_first.artifacts);
    });
}

TEST(ContextStability, TheValuesValidateAgainstTheirSchemas) {
    json_schema_lite::Checker artifact_v1(read_schema("context-artifact-v1.schema.json"), read_schema);
    json_schema_lite::Checker artifact_v2(read_schema("context-artifact-v2.schema.json"), read_schema);
    json_schema_lite::Checker epoch_v1(read_schema("context-epoch-v1.schema.json"), read_schema);
    json_schema_lite::Checker epoch_v2(read_schema("context-epoch-v2.schema.json"), read_schema);
    json_schema_lite::Checker receipt_v1(read_schema("context-assembly-receipt-v1.schema.json"), read_schema);
    json_schema_lite::Checker receipt_v2(read_schema("context-assembly-receipt-v2.schema.json"), read_schema);
    const auto accepts = [](const json_schema_lite::Checker& checker, const std::string& bytes) {
        const auto errors = checker.check(json::parse(bytes));
        EXPECT_TRUE(errors.empty()) << violations(errors) << "\n" << bytes;
    };
    const auto rejects = [](const json_schema_lite::Checker& checker, const json& value) {
        EXPECT_FALSE(checker.check(value).empty()) << value.dump();
    };

    // Version 1 values, as stored before, and the artifacts of every kind.
    accepts(artifact_v1, kSkillV1);
    accepts(artifact_v1, kSummaryV1);
    accepts(epoch_v1, kEpochV1);
    accepts(receipt_v1, kReceiptV1);
    rejects(artifact_v2, parsed(kSkillV1));
    rejects(epoch_v2, parsed(kEpochV1));
    rejects(receipt_v2, parsed(kReceiptV1));

    // Version 2 values.
    auto declared_data = summary_data();
    declared_data.stability_class = StabilityClass::Slow;
    declared_data.anchor_mode = AnchorMode::KeepInPlace;
    const auto declared = ContextArtifact::create(declared_data);
    accepts(artifact_v2, declared.serialize_canonical());
    rejects(artifact_v1, parsed(declared.serialize_canonical()));
    auto volatile_skill = parsed(ContextArtifact::create([&] {
        auto d = skill_data();
        d.stability_class = StabilityClass::Stable;
        return d;
    }()).serialize_canonical());
    accepts(artifact_v2, volatile_skill.dump());
    volatile_skill["stability_class"] = "volatile";
    rejects(artifact_v2, volatile_skill);

    const auto skill = ContextArtifact::create(skill_data());
    const auto summary = ContextArtifact::create(summary_data());
    const std::vector<ContextArtifact> artifacts{skill, summary};
    auto ordered_data = epoch_v1_data({skill.id(), summary.id()});
    ordered_data.sequence = 2;
    ordered_data.predecessor_id = sha('9');
    ordered_data.ordered_artifact_ids = std::vector<std::string>{summary.id(), skill.id()};
    const auto ordered = ContextEpoch::create(ordered_data);
    accepts(epoch_v2, ordered.serialize_canonical());
    rejects(epoch_v1, parsed(ordered.serialize_canonical()));
    auto extending_data = ordered_data;
    extending_data.prefix_claim = ContextPrefixClaim::Extends;
    accepts(epoch_v2, ContextEpoch::create(extending_data).serialize_canonical());
    auto breaking_data = ordered_data;
    breaking_data.prefix_claim = ContextPrefixClaim::Breaks;
    breaking_data.break_reason = ContextBreakReason::OrderChange;
    accepts(epoch_v2, ContextEpoch::create(breaking_data).serialize_canonical());
    rejects(epoch_v2, without(parsed(ContextEpoch::create(extending_data).serialize_canonical()),
                              "ordered_artifact_ids"));
    auto reason_without_break = parsed(ContextEpoch::create(extending_data).serialize_canonical());
    reason_without_break["break_reason"] = "explicit";
    rejects(epoch_v2, reason_without_break);

    auto receipt_data = receipt_v1_data(ordered, artifacts);
    receipt_data.assembled_artifact_ids = std::vector<std::string>{skill.id(), summary.id()};
    receipt_data.artifact_classes = std::map<std::string, StabilityClass>{
        {skill.id(), StabilityClass::Stable}, {summary.id(), StabilityClass::Volatile}};
    receipt_data.encoder_family = "anthropic.messages";
    receipt_data.tools_digest = sha('4');
    receipt_data.stable_boundary = ContextPrefixBoundary{1, 0};
    receipt_data.prefix_boundary = ContextPrefixBoundary{2, 1};
    receipt_data.prefix_digest = sha('5');
    const auto receipt = ContextAssemblyReceipt::create(receipt_data, ordered, artifacts);
    accepts(receipt_v2, receipt.serialize_canonical());
    rejects(receipt_v1, parsed(receipt.serialize_canonical()));
    rejects(receipt_v2, without(parsed(receipt.serialize_canonical()), "assembled_artifact_ids"));
    auto unknown_class = parsed(receipt.serialize_canonical());
    unknown_class["artifact_classes"][skill.id()] = "eternal";
    rejects(receipt_v2, unknown_class);
}
