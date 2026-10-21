#include <gtest/gtest.h>

#include <functional>
#include <optional>

#include <neograph/runtime_turn_assembler.h>
#include <neograph/graph/cancel.h>
#include "fixtures/typed_provider.h"

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

class AssemblyProvider : public test::LocalProvider {
public:
    AssemblyProvider()
        : LocalProvider([](ProviderRequest, const PreparedProviderRequest&,
                           const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
              throw std::logic_error("assembly must not dispatch");
              co_return test::success("");
          }) {}

    PreparedProviderRequest prepare(ProviderRequest request) override {
        auto messages = provider_request_messages(request);
        auto prepared = LocalProvider::prepare(std::move(request));
        if (prepared.valid()) admitted_messages = std::move(messages);
        return prepared;
    }
    std::vector<sp::Message> admitted_messages;
};

AssemblyProvider assembly_provider() { return {}; }

ProviderRequest assembly_template(ProviderMode mode = ProviderMode::Collect) {
    auto request = test::request("model", "", mode);
    std::get<sp::chat::Request>(request.payload).canonical_messages.clear();
    return request;
}

const std::vector<sp::Message>& admitted_messages(
    const AssemblyProvider& provider, const PreparedProviderRequest& request) {
    if (!request.valid()) throw std::logic_error("assembly request not admitted");
    // Observe the actual typed Provider admission boundary, independent of
    // whether a codec chooses string or content-block wire representation.
    return provider.admitted_messages;
}

void expect_text(const sp::Message& message, sp::Role role, std::string_view text) {
    EXPECT_EQ(message.role, role);
    ASSERT_EQ(message.parts.size(), 1U);
    ASSERT_TRUE(std::holds_alternative<sp::Text>(message.parts.front()));
    EXPECT_EQ(std::get<sp::Text>(message.parts.front()).value, text);
}

RuntimeHistoryRecord history(std::uint64_t sequence, std::optional<std::string> predecessor) {
    RuntimeHistoryRecordData data;
    data.feed_id = "feed";
    data.sequence = sequence;
    data.message_id = "message_" + std::to_string(sequence);
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = test::message("user_" + std::to_string(sequence), sp::Role::User);
    data.predecessor_id = std::move(predecessor);
    return RuntimeHistoryRecord::create(std::move(data));
}

ContextArtifact artifact(std::string producer, std::string text, ContextPlacement placement,
                         int priority, bool skill = false) {
    ContextArtifactData data;
    data.kind = skill ? ContextArtifactKind::RequiredSkill : ContextArtifactKind::DerivedContext;
    data.producer_id = std::move(producer);
    data.source_digest = sha(skill ? 'a' : 'b');
    data.media_type = "text/plain";
    data.placement = placement;
    data.priority = priority;
    data.required = skill;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

ContextArtifact hard_constraint(std::string text) {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::HardConstraint;
    data.producer_id = "host-policy.v1";
    data.source_digest = sha('f');
    data.media_type = "text/markdown";
    data.placement = ContextPlacement::BeforeLatestUser;
    data.priority = 1000;
    data.required = true;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

ContextArtifact untrusted_supplemental(std::string text,
                                       std::uint64_t through_sequence = 1) {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::UntrustedSupplemental;
    data.producer_id = "runtime-context.v1";
    data.source_digest = sha('9');
    data.source_feed_id = "feed";
    data.covers_from_sequence = 1;
    data.covers_through_sequence = through_sequence;
    data.media_type = "text/plain";
    data.placement = ContextPlacement::AfterHistory;
    data.priority = 100;
    data.required = true;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

}  // namespace

TEST(RuntimeTurnAssembler, VerifiesEpochAndProducesDeterministicMergedRequest) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    const auto second = history(2, first.id());
    ASSERT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    ASSERT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 2);

    const auto late = artifact("z", "late", ContextPlacement::AfterLatestUser, 0);
    const auto early = artifact("a", "early", ContextPlacement::BeforeLatestUser, 10, true);
    ASSERT_EQ(store.put_artifact("owner", late), ContextArtifactPutResult::Stored);
    ASSERT_EQ(store.put_artifact("owner", early), ContextArtifactPutResult::Stored);
    ContextEpochData epoch_data;
    epoch_data.run_id = "run";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 2;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.artifact_ids = {late.id(), early.id()};
    const auto epoch = ContextEpoch::create(std::move(epoch_data));

    auto provider = assembly_provider();
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(
                     provider, "owner", epoch, assembly_template(ProviderMode::Stream)),
                 std::invalid_argument);
    auto turn = RuntimeTurnAssembler(store, {early.id()}).assemble(
        provider, "owner", epoch, assembly_template(ProviderMode::Stream));

    ASSERT_EQ(turn.request.mode(), ProviderMode::Stream);
    const auto& messages = admitted_messages(provider, turn.request);
    ASSERT_EQ(messages.size(), 4u);
    expect_text(messages[0], sp::Role::User, "user_1");
    expect_text(messages[1], sp::Role::System, "early");
    expect_text(messages[2], sp::Role::User, "user_2");
    expect_text(messages[3], sp::Role::User, "late");
    EXPECT_EQ(turn.assembly_receipt.context_epoch_id(), epoch.id());
    EXPECT_EQ(turn.assembly_receipt.required_skill_artifact_ids(), std::vector<std::string>{early.id()});
    EXPECT_LE(turn.assembly_receipt.mandatory_input_tokens(),
              turn.assembly_receipt.estimated_input_tokens());
}

TEST(RuntimeTurnAssembler, DeliversOnlyExplicitUntrustedSupplementalAsUserData) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt),
              ContextStoreAppendResult::Appended);
    RuntimeHistoryRecordData assistant_data;
    assistant_data.feed_id = "feed";
    assistant_data.sequence = 2;
    assistant_data.message_id = "assistant_2";
    assistant_data.trust = RuntimeTrustClass::ModelOutput;
    assistant_data.message = test::message("working");
    assistant_data.message.parts.emplace_back(
        sp::ToolCall{"call_1", "read", sp::ToolCallKind::ClientExecuted, test::document("{}")});
    assistant_data.predecessor_id = first.id();
    const auto assistant = RuntimeHistoryRecord::create(std::move(assistant_data));
    ASSERT_EQ(store.append_history(feed, assistant, first.id()),
              ContextStoreAppendResult::Appended);
    RuntimeHistoryRecordData tool_data;
    tool_data.feed_id = "feed";
    tool_data.sequence = 3;
    tool_data.message_id = "tool_3";
    tool_data.trust = RuntimeTrustClass::ToolOutput;
    tool_data.message.role = sp::Role::Tool;
    tool_data.message.parts = {sp::ToolResult{"call_1", "file contents", false}};
    tool_data.predecessor_id = assistant.id();
    const auto tool = RuntimeHistoryRecord::create(std::move(tool_data));
    ASSERT_EQ(store.append_history(feed, tool, assistant.id()),
              ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 3);
    const auto constraint = hard_constraint("Host-owned constraint.");
    const auto supplemental = untrusted_supplemental(
        R"({"type":"history-header","format":"agentx.runtime-history.jsonl.v1"})", 3);
    ASSERT_EQ(store.put_artifact("owner", constraint),
              ContextArtifactPutResult::Stored);
    ASSERT_EQ(store.put_artifact("owner", supplemental),
              ContextArtifactPutResult::Stored);
    EXPECT_EQ(ContextArtifact::parse(supplemental.serialize_canonical()).kind(),
              ContextArtifactKind::UntrustedSupplemental);

    ContextEpochData epoch_data;
    epoch_data.run_id = "untrusted-supplemental";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 3;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.artifact_ids = {constraint.id(), supplemental.id()};
    epoch_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    const auto epoch = ContextEpoch::create(std::move(epoch_data));

    RuntimeContextRequirements requirements;
    requirements.required_artifact_ids = {constraint.id(), supplemental.id()};
    auto provider = assembly_provider();
    const auto turn = RuntimeTurnAssembler(store, 4096, requirements).assemble(
        provider, "owner", epoch, assembly_template());
    const auto& messages = admitted_messages(provider, turn.request);
    ASSERT_EQ(messages.size(), 5u);
    expect_text(messages[0], sp::Role::System, "Host-owned constraint.");
    expect_text(messages[1], sp::Role::User, "user_1");
    EXPECT_EQ(messages[2].role, sp::Role::Assistant);
    ASSERT_EQ(messages[2].parts.size(), 2U);
    EXPECT_EQ(std::get<sp::ToolCall>(messages[2].parts[1]).id, "call_1");
    EXPECT_EQ(messages[3].role, sp::Role::Tool);
    ASSERT_EQ(messages[3].parts.size(), 1U);
    const auto& result = std::get<sp::ToolResult>(messages[3].parts[0]);
    EXPECT_EQ(result.tool_use_id, "call_1");
    EXPECT_EQ(result.content, "file contents");
    expect_text(messages[4], sp::Role::User, supplemental.content().get<std::string>());
    EXPECT_GT(turn.assembly_receipt.mandatory_input_tokens(), 0u);
}

TEST(RuntimeTurnAssembler, PlacesDerivedPrefixBeforeARecentTailWithoutUser) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt),
              ContextStoreAppendResult::Appended);

    RuntimeHistoryRecordData assistant_data;
    assistant_data.feed_id = "feed";
    assistant_data.sequence = 2;
    assistant_data.message_id = "assistant_2";
    assistant_data.trust = RuntimeTrustClass::ModelOutput;
    assistant_data.message.role = sp::Role::Assistant;
    assistant_data.message.parts = {
        sp::ToolCall{"call_1", "read", sp::ToolCallKind::ClientExecuted, test::document("{}")}};
    assistant_data.predecessor_id = first.id();
    const auto assistant = RuntimeHistoryRecord::create(std::move(assistant_data));
    ASSERT_EQ(store.append_history(feed, assistant, first.id()),
              ContextStoreAppendResult::Appended);

    RuntimeHistoryRecordData tool_data;
    tool_data.feed_id = "feed";
    tool_data.sequence = 3;
    tool_data.message_id = "tool_3";
    tool_data.trust = RuntimeTrustClass::ToolOutput;
    tool_data.message.role = sp::Role::Tool;
    tool_data.message.parts = {sp::ToolResult{"call_1", "recent result", false}};
    tool_data.predecessor_id = assistant.id();
    const auto tool = RuntimeHistoryRecord::create(std::move(tool_data));
    ASSERT_EQ(store.append_history(feed, tool, assistant.id()),
              ContextStoreAppendResult::Appended);

    ContextArtifactData prefix_data;
    prefix_data.kind = ContextArtifactKind::DerivedContext;
    prefix_data.producer_id = "history-index.v1";
    prefix_data.source_digest = store.snapshot_history(feed, 1, 1).digest;
    prefix_data.source_feed_id = "feed";
    prefix_data.covers_from_sequence = 1;
    prefix_data.covers_through_sequence = 1;
    prefix_data.media_type = "text/markdown";
    prefix_data.placement = ContextPlacement::BeforeHistory;
    prefix_data.required = true;
    prefix_data.content = "<compacted-history>objective</compacted-history>";
    const auto prefix = ContextArtifact::create(std::move(prefix_data));
    ASSERT_EQ(store.put_artifact("owner", prefix),
              ContextArtifactPutResult::Stored);

    const auto tail = store.snapshot_history(feed, 2, 3);
    ContextEpochData epoch_data;
    epoch_data.run_id = "compacted-tail";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 2;
    epoch_data.raw_through_sequence = 3;
    epoch_data.raw_window_digest = tail.digest;
    epoch_data.artifact_ids = {prefix.id()};
    epoch_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    RuntimeContextRequirements requirements;
    requirements.required_artifact_ids = {prefix.id()};
    auto provider = assembly_provider();
    const auto turn = RuntimeTurnAssembler(store, 4096, requirements).assemble(
        provider, "owner", epoch, assembly_template());

    const auto& messages = admitted_messages(provider, turn.request);
    ASSERT_EQ(messages.size(), 3U);
    expect_text(messages[0], sp::Role::User, prefix.content().get<std::string>());
    EXPECT_EQ(messages[1].role, sp::Role::Assistant);
    EXPECT_EQ(messages[2].role, sp::Role::Tool);
}

TEST(RuntimeTurnAssembler, StrictAssemblyRequiresAndEnforcesInputBudget) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 1);
    ContextEpochData epoch_data;
    epoch_data.run_id = "strict-run";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 1;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    auto provider = assembly_provider();
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(
                     provider, "owner", epoch, assembly_template()),
                 std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {}, 1).assemble(
                     provider, "owner", epoch, assembly_template()),
                 ContextBudgetBlocked);
    const auto turn = RuntimeTurnAssembler(store, {}, 1024).assemble(
        provider, "owner", epoch, assembly_template());
    expect_text(admitted_messages(provider, turn.request)[0], sp::Role::User, "user_1");
}

TEST(RuntimeTurnAssembler, EnforcesConfiguredRequiredSkillsAndUserAnchor) {
    InMemoryContextStore store;
    const auto skill = artifact("skill", "skill", ContextPlacement::BeforeLatestUser, 0, true);
    ASSERT_EQ(store.put_artifact("owner", skill), ContextArtifactPutResult::Stored);
    ContextEpochData omitted_data;
    omitted_data.run_id = "run";
    omitted_data.sequence = 1;
    omitted_data.raw_window_digest = sha('e');
    const auto omitted = ContextEpoch::create(std::move(omitted_data));
    auto provider = assembly_provider();
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id()}).assemble(
                     provider, "owner", omitted, assembly_template()), std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id(), skill.id()}), std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {"not-an-identity"}), std::invalid_argument);

    ContextEpochData no_user_data;
    no_user_data.run_id = "run-user";
    no_user_data.sequence = 1;
    no_user_data.raw_window_digest = sha('f');
    no_user_data.artifact_ids = {skill.id()};
    const auto no_user = ContextEpoch::create(std::move(no_user_data));
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id()}).assemble(
                     provider, "owner", no_user, assembly_template()), std::invalid_argument);
}

TEST(RuntimeTurnAssembler, EnforcesGeneralRequiredContextAndCountsMandatoryTokens) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    ASSERT_EQ(store.append_history(feed, first, std::nullopt),
              ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 1);
    const auto constraint = hard_constraint("Never publish without verification.");
    ASSERT_EQ(store.put_artifact("owner", constraint),
              ContextArtifactPutResult::Stored);
    ContextEpochData epoch_data;
    epoch_data.run_id = "required-context";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 1;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.artifact_ids = {constraint.id()};
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    auto provider = assembly_provider();
    RuntimeContextRequirements requirements;
    requirements.required_artifact_ids = {constraint.id()};
    const auto turn = RuntimeTurnAssembler(store, 1000, requirements).assemble(
        provider, "owner", epoch, assembly_template());
    EXPECT_GT(turn.assembly_receipt.mandatory_input_tokens(), 0u);
    EXPECT_TRUE(turn.assembly_receipt.required_skill_artifact_ids().empty());

    ContextEpochData omitted_data;
    omitted_data.run_id = "required-context-omitted";
    omitted_data.sequence = 1;
    omitted_data.feed_id = "feed";
    omitted_data.raw_from_sequence = 1;
    omitted_data.raw_through_sequence = 1;
    omitted_data.raw_window_digest = raw.digest;
    const auto omitted = ContextEpoch::create(std::move(omitted_data));
    EXPECT_THROW(RuntimeTurnAssembler(store, 1000, requirements).assemble(
                     provider, "owner", omitted, assembly_template()),
                 std::invalid_argument);
}

namespace {

struct RequiredContextFixture {
    InMemoryContextStore store;
    std::vector<ContextArtifact> artifacts;
    std::optional<ContextEpoch> epoch;
    RuntimeContextRequirements requirements;
};

// One short RAW user message plus one required Skill and `constraints`
// required hard constraints, selected by a Strict epoch.
void prepare_required_context(RequiredContextFixture& fixture, std::size_t constraints,
                              const std::string& skill_text,
                              const std::string& constraint_text) {
    const ContextStoreFeed feed{"owner", "feed"};
    const auto first = history(1, std::nullopt);
    ASSERT_EQ(fixture.store.append_history(feed, first, std::nullopt),
              ContextStoreAppendResult::Appended);
    const auto raw = fixture.store.snapshot_history(feed, 1, 1);
    const auto skill = artifact("skill", skill_text, ContextPlacement::BeforeLatestUser, 0, true);
    fixture.artifacts.push_back(skill);
    fixture.requirements.required_artifact_ids.push_back(skill.id());
    fixture.requirements.required_skill_artifact_ids.push_back(skill.id());
    for (std::size_t index = 0; index < constraints; ++index) {
        const auto constraint = hard_constraint(constraint_text + " #" + std::to_string(index));
        fixture.artifacts.push_back(constraint);
        fixture.requirements.required_artifact_ids.push_back(constraint.id());
    }
    ContextEpochData epoch_data;
    epoch_data.run_id = "required-context-" + std::to_string(constraints);
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 1;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.guarantee_profile = RuntimeGuaranteeProfile::Strict;
    for (const auto& item : fixture.artifacts) {
        ASSERT_EQ(fixture.store.put_artifact("owner", item), ContextArtifactPutResult::Stored);
        epoch_data.artifact_ids.push_back(item.id());
    }
    fixture.epoch = ContextEpoch::create(std::move(epoch_data));
}

}  // namespace

// Issue #373: the mandatory estimate came from the message projection JSON and
// the input estimate from the smaller wire body, so a short history with two or
// more required artifacts failed "mandatory exceeds input". Both now count the
// bytes the prepared request carries.
TEST(RuntimeTurnAssembler, ShortHistoryWithSeveralRequiredArtifactsIsAdmitted) {
    for (const std::size_t constraints : {1u, 2u, 5u}) {
        RequiredContextFixture fixture;
        prepare_required_context(fixture, constraints, "Always run the verifier.",
                                 "Never publish without verification.");
        auto provider = assembly_provider();
        const auto turn = RuntimeTurnAssembler(fixture.store, 32'000, fixture.requirements)
                              .assemble(provider, "owner", *fixture.epoch, assembly_template());
        const auto& receipt = turn.assembly_receipt;
        EXPECT_GT(receipt.mandatory_input_tokens(), 0u) << constraints;
        EXPECT_LE(receipt.mandatory_input_tokens(), receipt.estimated_input_tokens())
            << constraints;
        EXPECT_EQ(receipt.estimated_input_tokens(),
                  RuntimeTurnAssembler::estimate_input_tokens(turn.request));
        EXPECT_EQ(admitted_messages(provider, turn.request).size(), constraints + 2);
    }
}

// Required content larger than the whole budget is a genuine overflow: the
// turn is blocked before dispatch.
TEST(RuntimeTurnAssembler, RequiredArtifactsOverTheBudgetAreBlocked) {
    RequiredContextFixture fixture;
    std::string long_text;
    for (int i = 0; i < 40; ++i) long_text += "verify before publishing. ";
    prepare_required_context(fixture, 2, long_text, long_text);
    auto provider = assembly_provider();
    EXPECT_THROW(RuntimeTurnAssembler(fixture.store, 64, fixture.requirements)
                     .assemble(provider, "owner", *fixture.epoch, assembly_template()),
                 ContextBudgetBlocked);
}

TEST(RuntimeTurnAssembler, ReceiptRejectsMandatoryTokensAboveTheInputEstimate) {
    ContextEpochData epoch_data;
    epoch_data.run_id = "receipt-run";
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = sha('1');
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = sha('2');
    data.message_window_digest = sha('3');
    data.estimated_input_tokens = 40;
    data.mandatory_input_tokens = 41;
    try {
        (void)ContextAssemblyReceipt::create(data, epoch, {});
        FAIL() << "a receipt whose mandatory tokens exceed its input was accepted";
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("(41)"), std::string::npos) << message;
        EXPECT_NE(message.find("(40)"), std::string::npos) << message;
    }
    data.mandatory_input_tokens = 40;
    EXPECT_EQ(ContextAssemblyReceipt::create(data, epoch, {}).mandatory_input_tokens(), 40u);
}

TEST(RuntimeTurnAssembler, NormalizedDigestCoversRequestShapeButExcludesCancellationIdentity) {
    auto provider = assembly_provider();
    auto request = test::request("model", "hello");
    auto& payload = std::get<sp::chat::Request>(request.payload);
    payload.temperature = 0.2;
    payload.max_output_tokens = 12;
    payload.reasoning_effort = "low";
    payload.tools = {{"tool", "description", test::document(R"({"type":"object"})")}};
    auto digest = [&provider](ProviderRequest request) {
        const auto prepared = provider.prepare(std::move(request));
        if (!prepared.valid()) throw std::logic_error("digest input not admitted");
        return RuntimeTurnAssembler::normalized_request_digest(prepared);
    };
    const auto baseline = digest(request);
    auto cancellation_variant = request;
    cancellation_variant.cancel_token = std::make_shared<graph::CancelToken>();
    EXPECT_EQ(baseline, digest(std::move(cancellation_variant)));
    auto changed = request;
    changed.mode = ProviderMode::Stream;
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).model = "other-model";
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).canonical_messages[0] =
        test::message("other", sp::Role::User);
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).temperature = 0.3;
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).reasoning_effort = "high";
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).tools[0].description = "other description";
    EXPECT_NE(baseline, digest(std::move(changed)));
    changed = request;
    std::get<sp::chat::Request>(changed.payload).max_output_tokens = 13;
    EXPECT_NE(baseline, digest(std::move(changed)));
    // Deadline authority is pinned on the admitted handle, separate from the
    // body digest; changing it must not masquerade as a change to wire controls.
    request.options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const auto pinned = provider.prepare(request);
    ASSERT_TRUE(pinned.valid());
    EXPECT_EQ(pinned.deadline(), *request.options.deadline);
    EXPECT_EQ(baseline, RuntimeTurnAssembler::normalized_request_digest(pinned));
    request.options.deadline = *request.options.deadline + std::chrono::seconds(1);
    const auto later = provider.prepare(request);
    ASSERT_TRUE(later.valid());
    EXPECT_EQ(later.deadline(), *request.options.deadline);
    EXPECT_EQ(baseline, RuntimeTurnAssembler::normalized_request_digest(later));
}

TEST(RuntimeTurnAssembler, RendersOnlyExactTextObjectArtifacts) {
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    const auto record = history(1, std::nullopt);
    ASSERT_EQ(store.append_history(feed, record, std::nullopt), ContextStoreAppendResult::Appended);
    const auto raw = store.snapshot_history(feed, 1, 1);
    ContextArtifactData object_data;
    object_data.producer_id = "object";
    object_data.source_digest = sha('a');
    object_data.media_type = "text/markdown";
    object_data.content = json{{"text", "rendered"}};
    const auto object = ContextArtifact::create(std::move(object_data));
    ASSERT_EQ(store.put_artifact("owner", object), ContextArtifactPutResult::Stored);
    ContextEpochData epoch_data;
    epoch_data.run_id = "object-run";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 1;
    epoch_data.raw_window_digest = raw.digest;
    epoch_data.artifact_ids = {object.id()};
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    auto provider = assembly_provider();
    const auto turn = RuntimeTurnAssembler(store).assemble(
        provider, "owner", epoch, assembly_template());
    expect_text(admitted_messages(provider, turn.request)[0], sp::Role::User, "rendered");

    ContextArtifactData extra_data;
    extra_data.producer_id = "extra";
    extra_data.source_digest = sha('b');
    extra_data.media_type = "text/plain";
    extra_data.content = json{{"text", "no"}, {"extra", "reject"}};
    const auto extra = ContextArtifact::create(std::move(extra_data));
    ASSERT_EQ(store.put_artifact("owner", extra), ContextArtifactPutResult::Stored);
    ContextEpochData extra_epoch_data;
    extra_epoch_data.run_id = "extra-run";
    extra_epoch_data.sequence = 1;
    extra_epoch_data.feed_id = "feed";
    extra_epoch_data.raw_from_sequence = 1;
    extra_epoch_data.raw_through_sequence = 1;
    extra_epoch_data.raw_window_digest = raw.digest;
    extra_epoch_data.artifact_ids = {extra.id()};
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(
                     provider, "owner", ContextEpoch::create(std::move(extra_epoch_data)),
                     assembly_template()),
                 std::invalid_argument);
}

TEST(RuntimeTurnAssembler, RejectsTemplateMessagesAndUnsupportedArtifactRendering) {
    InMemoryContextStore store;
    ContextArtifactData unsupported_data;
    unsupported_data.kind = ContextArtifactKind::DerivedContext;
    unsupported_data.producer_id = "json";
    unsupported_data.source_digest = sha('c');
    unsupported_data.media_type = "application/json";
    unsupported_data.content = json::object();
    const auto unsupported = ContextArtifact::create(std::move(unsupported_data));
    ASSERT_EQ(store.put_artifact("owner", unsupported), ContextArtifactPutResult::Stored);
    ContextEpochData epoch_data;
    epoch_data.run_id = "run";
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = sha('d');
    epoch_data.artifact_ids = {unsupported.id()};
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    auto provider = assembly_provider();
    auto request = test::request("model", "injected");
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(provider, "owner", epoch, request),
                 std::invalid_argument);
    std::get<sp::chat::Request>(request.payload).canonical_messages.clear();
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(provider, "owner", epoch, request),
                 std::invalid_argument);
}

namespace {

// A feed of two user records and the epoch that selects them with the given artifacts.
struct ReceiptFixture {
    InMemoryContextStore store;
    std::vector<ContextArtifact> artifacts;
    std::optional<ContextEpoch> epoch;

    ReceiptFixture(std::vector<ContextArtifact> chosen,
                   std::optional<std::vector<std::string>> order = std::nullopt,
                   std::optional<ContextPrefixClaim> claim = std::nullopt,
                   std::optional<ContextBreakReason> reason = std::nullopt)
        : artifacts(std::move(chosen)) {
        const ContextStoreFeed feed{"owner", "feed"};
        const auto first = history(1, std::nullopt);
        const auto second = history(2, first.id());
        EXPECT_EQ(store.append_history(feed, first, std::nullopt), ContextStoreAppendResult::Appended);
        EXPECT_EQ(store.append_history(feed, second, first.id()), ContextStoreAppendResult::Appended);
        ContextEpochData data;
        data.run_id = "run";
        data.sequence = claim ? 2 : 1;
        if (claim) data.predecessor_id = sha('9');
        data.feed_id = "feed";
        data.raw_from_sequence = 1;
        data.raw_through_sequence = 2;
        data.raw_window_digest = store.snapshot_history(feed, 1, 2).digest;
        for (const auto& artifact : artifacts) {
            EXPECT_EQ(store.put_artifact("owner", artifact), ContextArtifactPutResult::Stored);
            data.artifact_ids.push_back(artifact.id());
        }
        data.ordered_artifact_ids = std::move(order);
        data.prefix_claim = claim;
        data.break_reason = reason;
        epoch = ContextEpoch::create(std::move(data));
    }
};

ContextArtifact compaction_summary() {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::DerivedContext;
    data.producer_id = std::string(COMPACTION_PRODUCER_ID);
    data.source_digest = sha('c');
    data.source_feed_id = "feed";
    data.covers_from_sequence = 1;
    data.covers_through_sequence = 1;
    data.media_type = "text/plain";
    data.placement = ContextPlacement::BeforeHistory;
    data.content = "summary";
    return ContextArtifact::create(std::move(data));
}

}  // namespace

TEST(RuntimeTurnAssembler, ReceiptRecordsTheAssembledOrderTheClassesAndTheEncoder) {
    const auto summary = compaction_summary();
    const auto skill = artifact("a", "skill", ContextPlacement::BeforeLatestUser, 10, true);
    const auto note = artifact("b", "note", ContextPlacement::BeforeLatestUser, 0);
    const auto late = artifact("z", "late", ContextPlacement::AfterLatestUser, 0);
    ReceiptFixture fixture({late, note, skill, summary});
    auto provider = assembly_provider();
    const auto turn = RuntimeTurnAssembler(fixture.store, {skill.id()}).assemble(
        provider, "owner", *fixture.epoch, assembly_template());

    // The order is the one the prompt follows, not the epoch's sorted set.
    const std::vector<std::string> assembled{summary.id(), skill.id(), note.id(), late.id()};
    ASSERT_NE(fixture.epoch->artifact_ids(), assembled);
    const auto& receipt = turn.assembly_receipt;
    EXPECT_EQ(receipt.assembled_artifact_ids(), assembled);
    EXPECT_EQ(receipt.artifact_ids(), fixture.epoch->artifact_ids());
    ASSERT_TRUE(receipt.artifact_classes());
    EXPECT_EQ(receipt.artifact_classes()->at(summary.id()), StabilityClass::Slow);
    EXPECT_EQ(receipt.artifact_classes()->at(skill.id()), StabilityClass::Stable);
    EXPECT_EQ(receipt.artifact_classes()->at(note.id()), StabilityClass::Volatile);
    EXPECT_EQ(receipt.artifact_classes()->at(late.id()), StabilityClass::Volatile);
    EXPECT_EQ(receipt.encoder_family(), std::optional<std::string>(turn.request.family()));
    EXPECT_EQ(receipt.encoder_family(), std::optional<std::string>("openai.chat"));
    // The prompt and the boundaries are not changed or marked by this version.
    EXPECT_FALSE(receipt.tools_digest());
    EXPECT_FALSE(receipt.stable_boundary());
    EXPECT_FALSE(receipt.prefix_boundary());
    EXPECT_FALSE(receipt.prefix_digest());
    EXPECT_FALSE(receipt.break_reason());
    const auto& messages = admitted_messages(provider, turn.request);
    ASSERT_EQ(messages.size(), 6u);
    expect_text(messages[0], sp::Role::User, "summary");
    expect_text(messages[1], sp::Role::User, "user_1");
    expect_text(messages[2], sp::Role::System, "skill");
    expect_text(messages[3], sp::Role::User, "note");
    expect_text(messages[4], sp::Role::User, "user_2");
    expect_text(messages[5], sp::Role::User, "late");
    // The receipt is evidence that survives storage.
    EXPECT_EQ(ContextAssemblyReceipt::parse(receipt.serialize_canonical(), *fixture.epoch, fixture.artifacts).id(),
              receipt.id());
}

TEST(RuntimeTurnAssembler, AnOrderRecordedByTheEpochDoesNotChangeTheAssembly) {
    const auto skill = artifact("a", "skill", ContextPlacement::BeforeLatestUser, 10, true);
    const auto note = artifact("b", "note", ContextPlacement::BeforeLatestUser, 0);
    const auto late = artifact("z", "late", ContextPlacement::AfterLatestUser, 0);
    ReceiptFixture legacy({skill, note, late});
    ReceiptFixture ordered({skill, note, late}, std::vector<std::string>{late.id(), note.id(), skill.id()});
    auto legacy_provider = assembly_provider();
    auto ordered_provider = assembly_provider();
    const auto from_legacy = RuntimeTurnAssembler(legacy.store, {skill.id()}).assemble(
        legacy_provider, "owner", *legacy.epoch, assembly_template());
    const auto from_order = RuntimeTurnAssembler(ordered.store, {skill.id()}).assemble(
        ordered_provider, "owner", *ordered.epoch, assembly_template());
    ASSERT_NE(legacy.epoch->id(), ordered.epoch->id());
    EXPECT_EQ(from_order.request.encoded_body(), from_legacy.request.encoded_body());
    EXPECT_EQ(from_order.assembly_receipt.message_window_digest(),
              from_legacy.assembly_receipt.message_window_digest());
    EXPECT_EQ(from_order.assembly_receipt.normalized_request_digest(),
              from_legacy.assembly_receipt.normalized_request_digest());
    EXPECT_EQ(from_order.assembly_receipt.assembled_artifact_ids(),
              from_legacy.assembly_receipt.assembled_artifact_ids());
}

TEST(RuntimeTurnAssembler, ReceiptNamesTheBreakItsEpochClaims) {
    const auto skill = artifact("a", "skill", ContextPlacement::BeforeLatestUser, 10, true);
    const std::vector<std::string> order{skill.id()};
    ReceiptFixture breaking({skill}, order, ContextPrefixClaim::Breaks, ContextBreakReason::Compaction);
    ReceiptFixture extending({skill}, order, ContextPrefixClaim::Extends);
    auto provider = assembly_provider();
    const auto broken = RuntimeTurnAssembler(breaking.store, {skill.id()}).assemble(
        provider, "owner", *breaking.epoch, assembly_template());
    EXPECT_EQ(broken.assembly_receipt.break_reason(), ContextBreakReason::Compaction);
    // The claim to extend is checked when the epoch is activated; assembly only carries the reason.
    const auto extended = RuntimeTurnAssembler(extending.store, {skill.id()}).assemble(
        provider, "owner", *extending.epoch, assembly_template());
    EXPECT_FALSE(extended.assembly_receipt.break_reason());
    EXPECT_TRUE(extended.assembly_receipt.assembled_artifact_ids());
}

// ---------------------------------------------------------------------------------------------------------
// The stable-first layout (#329)
// ---------------------------------------------------------------------------------------------------------

namespace {

// A feed of records and the epoch of the stable-first layout over it. The records are user messages unless
// a message is given.
struct LayoutFixture {
    InMemoryContextStore store;
    std::vector<ContextArtifact> artifacts;
    std::optional<ContextEpoch> epoch;
    std::vector<std::string> order;

    LayoutFixture(std::vector<ContextArtifact> ordered, std::size_t records = 2,
                  std::optional<sp::Message> system_record = std::nullopt,
                  AssemblyLayout layout = AssemblyLayout::StableFirst)
        : artifacts(std::move(ordered)) {
        const ContextStoreFeed feed{"owner", "feed"};
        std::optional<std::string> head;
        for (std::size_t sequence = 1; sequence <= records; ++sequence) {
            RuntimeHistoryRecordData data;
            data.feed_id = "feed";
            data.sequence = sequence;
            data.message_id = "message_" + std::to_string(sequence);
            data.trust = RuntimeTrustClass::UntrustedInput;
            data.message = test::message("user_" + std::to_string(sequence), sp::Role::User);
            if (system_record && sequence == 2) {
                data.trust = RuntimeTrustClass::HostPolicy;
                data.message = *system_record;
            }
            data.predecessor_id = head;
            const auto record = RuntimeHistoryRecord::create(std::move(data));
            EXPECT_EQ(store.append_history(feed, record, head), ContextStoreAppendResult::Appended);
            head = record.id();
        }
        ContextEpochData data;
        data.run_id = "run";
        data.sequence = 1;
        data.feed_id = "feed";
        data.raw_from_sequence = 1;
        data.raw_through_sequence = records;
        data.raw_window_digest = store.snapshot_history(feed, 1, records).digest;
        for (const auto& artifact : artifacts) {
            EXPECT_EQ(store.put_artifact("owner", artifact), ContextArtifactPutResult::Stored);
            data.artifact_ids.push_back(artifact.id());
            order.push_back(artifact.id());
        }
        data.ordered_artifact_ids = order;
        data.assembly_layout = layout;
        epoch = ContextEpoch::create(std::move(data));
    }
};

ContextArtifact head_skill(std::string text = "skill", ContextPlacement placement = ContextPlacement::BeforeHistory,
                           int priority = 10) {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::RequiredSkill;
    data.producer_id = "skill-resolver.v1";
    data.source_digest = sha('a');
    data.media_type = "text/plain";
    data.placement = placement;
    data.priority = priority;
    data.required = true;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

ContextArtifact slow_summary(ContextArtifactKind kind = ContextArtifactKind::DerivedContext,
                             std::string text = "summary") {
    ContextArtifactData data;
    data.kind = kind;
    data.producer_id = kind == ContextArtifactKind::DerivedContext ? std::string(COMPACTION_PRODUCER_ID)
                                                                    : "summary-skill.v1";
    data.source_digest = sha('c');
    data.media_type = "text/plain";
    data.placement = ContextPlacement::BeforeHistory;
    data.required = kind == ContextArtifactKind::HardConstraint;
    data.stability_class = StabilityClass::Slow;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

ContextArtifact tail_note(std::string text, std::string producer = "hook-output.v1") {
    ContextArtifactData data;
    data.kind = ContextArtifactKind::HookOutput;
    data.producer_id = std::move(producer);
    data.source_digest = sha('b');
    data.media_type = "text/plain";
    data.placement = ContextPlacement::AfterHistory;
    data.content = std::move(text);
    return ContextArtifact::create(std::move(data));
}

RuntimeTurn assemble_for(LayoutFixture& fixture, AssemblyProvider& provider,
                         std::vector<sp::Message> host = {}, std::vector<sp::Message> supplemental = {},
                         ProviderMode mode = ProviderMode::Collect) {
    std::vector<std::string> skills;
    for (const auto& artifact : fixture.artifacts) {
        if (artifact.kind() == ContextArtifactKind::RequiredSkill) skills.push_back(artifact.id());
    }
    return RuntimeTurnAssembler(fixture.store, skills).assemble(
        provider, "owner", *fixture.epoch, assembly_template(mode), std::move(host), std::move(supplemental));
}

void expect_layout_refusal(ContextStabilityRefusal code, const std::function<void()>& call) {
    try {
        call();
    } catch (const ContextStabilityError& error) {
        EXPECT_EQ(error.code(), code) << error.what();
        return;
    } catch (const std::exception& error) {
        ADD_FAILURE() << "expected a ContextStabilityError, got: " << error.what();
        return;
    }
    ADD_FAILURE() << "expected a ContextStabilityError";
}

}  // namespace

TEST(StableFirstAssembly, FollowsTheOrderTheEpochRecordsAndNotTheOrderOfAHash) {
    const auto skill = head_skill();
    const auto summary = slow_summary();
    const auto first = tail_note("note one", "producer-a.v1");
    const auto second = tail_note("note two", "producer-a.v1");
    // Whatever the identities of the two notes are, the epoch puts the second in front of the first.
    LayoutFixture fixture({skill, summary, second, first});
    auto provider = assembly_provider();
    const auto turn = assemble_for(fixture, provider);
    const auto& messages = admitted_messages(provider, turn.request);
    ASSERT_EQ(messages.size(), 6u);
    expect_text(messages[0], sp::Role::System, "skill");
    expect_text(messages[1], sp::Role::User, "summary");
    expect_text(messages[2], sp::Role::User, "user_1");
    expect_text(messages[3], sp::Role::User, "user_2");
    expect_text(messages[4], sp::Role::User, "note two");
    expect_text(messages[5], sp::Role::User, "note one");
    EXPECT_EQ(turn.assembly_receipt.assembled_artifact_ids(), fixture.order);
    EXPECT_EQ(turn.assembly_receipt.artifact_classes()->at(summary.id()), StabilityClass::Slow);

    // The notes in the other order give the other prompt: nothing but the epoch decides.
    LayoutFixture swapped({skill, summary, first, second});
    const auto other = assemble_for(swapped, provider);
    const auto& swapped_messages = admitted_messages(provider, other.request);
    expect_text(swapped_messages[4], sp::Role::User, "note one");
    expect_text(swapped_messages[5], sp::Role::User, "note two");
    EXPECT_NE(turn.request.encoded_body(), other.request.encoded_body());
}

TEST(StableFirstAssembly, WritesTheBodyTheLegacyAssemblyWritesWhereTheTwoAgree) {
    const auto skill = head_skill();
    const auto note = tail_note("note");
    LayoutFixture stable_first({skill, note});
    LayoutFixture legacy({skill, note}, 2, std::nullopt, AssemblyLayout::Legacy);
    auto stable_provider = assembly_provider();
    auto legacy_provider = assembly_provider();
    const auto from_stable_first = assemble_for(stable_first, stable_provider);
    const auto from_legacy = assemble_for(legacy, legacy_provider);
    ASSERT_NE(stable_first.epoch->id(), legacy.epoch->id());
    EXPECT_EQ(from_stable_first.request.encoded_body(), from_legacy.request.encoded_body());
    EXPECT_EQ(from_stable_first.assembly_receipt.normalized_request_digest(),
              from_legacy.assembly_receipt.normalized_request_digest());
    EXPECT_EQ(from_stable_first.assembly_receipt.message_window_digest(),
              from_legacy.assembly_receipt.message_window_digest());
    // Only the stable-first assembly names where its prefix ends.
    EXPECT_FALSE(from_legacy.assembly_receipt.prefix_boundary());
    EXPECT_FALSE(from_legacy.assembly_receipt.prefix_digest());
    EXPECT_TRUE(from_stable_first.assembly_receipt.prefix_boundary());
}

TEST(StableFirstAssembly, RefusesWhatItCannotPlaceWithATypedError) {
    auto provider = assembly_provider();
    const auto note = tail_note("note");
    for (const auto placement : {ContextPlacement::BeforeLatestUser, ContextPlacement::AfterLatestUser,
                                 ContextPlacement::AfterHistory}) {
        LayoutFixture fixture({head_skill("skill", placement), note});
        expect_layout_refusal(ContextStabilityRefusal::StableOutsideHistoryHead,
                              [&] { (void)assemble_for(fixture, provider); });
    }
    for (const auto placement : {ContextPlacement::BeforeHistory, ContextPlacement::BeforeLatestUser,
                                 ContextPlacement::AfterLatestUser}) {
        ContextArtifactData data;
        data.kind = ContextArtifactKind::HookOutput;
        data.producer_id = "hook-output.v1";
        data.source_digest = sha('b');
        data.media_type = "text/plain";
        data.placement = placement;
        data.content = "moved";
        LayoutFixture fixture({head_skill(), ContextArtifact::create(data)});
        expect_layout_refusal(ContextStabilityRefusal::VolatileOutsideTail,
                              [&] { (void)assemble_for(fixture, provider); });
    }
    LayoutFixture misordered({note, head_skill()});
    expect_layout_refusal(ContextStabilityRefusal::TierOrderViolated,
                          [&] { (void)assemble_for(misordered, provider); });
}

TEST(StableFirstAssembly, AllowsASystemMessageOnlyAsAHostInstructionOrAStableOrSlowArtifact) {
    auto provider = assembly_provider();
    sp::Message system = test::message("standing rule", sp::Role::System);
    // A host instruction, a Stable artifact and a Slow one may be System messages.
    LayoutFixture accepted({head_skill(), slow_summary(ContextArtifactKind::HardConstraint, "slow rule"),
                            tail_note("note")});
    EXPECT_NO_THROW((void)assemble_for(accepted, provider, {system}));

    // A RAW record or a supplemental input that is a System message would be hoisted to the start of the
    // request on some families, so the layout does not take it.
    LayoutFixture raw({head_skill(), tail_note("note")}, 2, system);
    expect_layout_refusal(ContextStabilityRefusal::VolatileSystemText,
                          [&] { (void)assemble_for(raw, provider); });
    LayoutFixture supplemental({head_skill(), tail_note("note")});
    expect_layout_refusal(ContextStabilityRefusal::VolatileSystemText,
                          [&] { (void)assemble_for(supplemental, provider, {}, {system}); });
}

TEST(StableFirstAssembly, NamesWhereTheStableAndTheAppendOnlyPartsEndInTheEncodedBody) {
    auto provider = assembly_provider();
    const auto skill = head_skill();
    const auto summary = slow_summary();
    LayoutFixture fixture({skill, summary, tail_note("reading 1")});
    const auto turn = assemble_for(fixture, provider);
    const auto& receipt = turn.assembly_receipt;
    // openai.chat: system(skill), user(summary), user(user_1), user(user_2), user(reading 1).
    EXPECT_EQ(receipt.stable_boundary(), std::optional<ContextPrefixBoundary>({1, 0}));
    EXPECT_EQ(receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({4, 0}));
    ASSERT_TRUE(receipt.prefix_digest());

    // The digest is the prefix's: a new volatile text leaves it as it was, a new RAW record moves it.
    LayoutFixture next({skill, summary, tail_note("reading 2")});
    const auto again = assemble_for(next, provider);
    EXPECT_EQ(again.assembly_receipt.prefix_digest(), receipt.prefix_digest());
    EXPECT_EQ(again.assembly_receipt.prefix_boundary(), receipt.prefix_boundary());
    LayoutFixture longer({skill, summary, tail_note("reading 2")}, 3);
    const auto grown = assemble_for(longer, provider);
    EXPECT_NE(grown.assembly_receipt.prefix_digest(), receipt.prefix_digest());
    EXPECT_EQ(grown.assembly_receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({5, 0}));
    // A change of the Slow part moves the prefix digest and not the place of the boundaries.
    LayoutFixture compacted({skill, slow_summary(ContextArtifactKind::DerivedContext, "another summary"),
                             tail_note("reading 1")});
    const auto changed = assemble_for(compacted, provider);
    EXPECT_NE(changed.assembly_receipt.prefix_digest(), receipt.prefix_digest());
    EXPECT_EQ(changed.assembly_receipt.stable_boundary(), receipt.stable_boundary());

    // The receipt survives storage with its boundaries.
    const auto stored = ContextAssemblyReceipt::parse(receipt.serialize_canonical(), *fixture.epoch, fixture.artifacts);
    EXPECT_EQ(stored.id(), receipt.id());
    EXPECT_EQ(stored.stable_boundary(), receipt.stable_boundary());
    EXPECT_EQ(stored.prefix_boundary(), receipt.prefix_boundary());
    EXPECT_EQ(stored.prefix_digest(), receipt.prefix_digest());
}

TEST(StableFirstAssembly, CountsHostInstructionsAndTheSupplementalInputInTheBoundaries) {
    auto provider = assembly_provider();
    const auto skill = head_skill();
    LayoutFixture fixture({skill, tail_note("reading")});
    const auto turn = assemble_for(fixture, provider, {test::message("host rule", sp::Role::System)},
                                   {test::message("the task", sp::Role::User)});
    // system(host rule), system(skill), user_1, user_2, reading, the task.
    EXPECT_EQ(turn.assembly_receipt.stable_boundary(), std::optional<ContextPrefixBoundary>({2, 0}));
    EXPECT_EQ(turn.assembly_receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({4, 0}));

    // Nothing volatile at all: the prefix is the whole body, and it is also where the Stable part ends.
    LayoutFixture stable_only({skill});
    const auto whole = assemble_for(stable_only, provider);
    EXPECT_EQ(whole.assembly_receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({3, 0}));
    // Nothing Stable and nothing in front: no Stable boundary to mark.
    LayoutFixture none({tail_note("reading")});
    const auto bare = assemble_for(none, provider);
    EXPECT_FALSE(bare.assembly_receipt.stable_boundary());
    EXPECT_EQ(bare.assembly_receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({2, 0}));
}

TEST(StableFirstAssembly, ThePrefixDigestCoversEveryMemberButTheStreamFlagAndTheOutputCap) {
    auto provider = assembly_provider();
    LayoutFixture fixture({head_skill(), tail_note("reading")});
    const auto buffered = assemble_for(fixture, provider);
    const auto streamed = assemble_for(fixture, provider, {}, {}, ProviderMode::Stream);
    ASSERT_NE(buffered.request.encoded_body(), streamed.request.encoded_body());
    // A call repeats its digest; a streamed call carries members that a buffered one has not, such as the
    // usage option of the stream, and they are part of the request start like any other member.
    EXPECT_EQ(assemble_for(fixture, provider).assembly_receipt.prefix_digest(), buffered.assembly_receipt.prefix_digest());
    EXPECT_EQ(assemble_for(fixture, provider, {}, {}, ProviderMode::Stream).assembly_receipt.prefix_digest(),
              streamed.assembly_receipt.prefix_digest());
    EXPECT_NE(buffered.assembly_receipt.prefix_digest(), streamed.assembly_receipt.prefix_digest());
}

namespace {

// A provider that encodes every request except the ones that end with the marker message of a probe.
class ProbeRefusingProvider final : public AssemblyProvider {
public:
    PreparedProviderRequest prepare(ProviderRequest request) override {
        const auto& messages = provider_request_messages(request);
        if (!messages.empty() && !messages.back().parts.empty()) {
            if (const auto* text = std::get_if<sp::Text>(&messages.back().parts.front());
                text && text->value.find("neograph-prefix-boundary-marker") != std::string::npos) {
                sp::Error error;
                error.kind = sp::ErrorKind::InvalidRequest;
                error.safe_message = "probe refused";
                return reject_preparation(std::move(error));
            }
        }
        return AssemblyProvider::prepare(std::move(request));
    }
};

}  // namespace

TEST(StableFirstAssembly, RefusesTheEpochWhenTheEncodedRequestDoesNotShowItsBoundaries) {
    LayoutFixture fixture({head_skill(), tail_note("reading")});
    ProbeRefusingProvider provider;
    std::vector<std::string> skills{fixture.artifacts.front().id()};
    expect_layout_refusal(ContextStabilityRefusal::PrefixBoundaryUnavailable, [&] {
        (void)RuntimeTurnAssembler(fixture.store, skills).assemble(
            provider, "owner", *fixture.epoch, assembly_template());
    });
    // With nothing Stable and nothing Volatile the prefix is the whole body: nothing has to be encoded again.
    LayoutFixture slow_only({slow_summary()});
    const auto turn = RuntimeTurnAssembler(slow_only.store).assemble(
        provider, "owner", *slow_only.epoch, assembly_template());
    EXPECT_EQ(turn.assembly_receipt.prefix_boundary(), std::optional<ContextPrefixBoundary>({3, 0}));
    EXPECT_FALSE(turn.assembly_receipt.stable_boundary());
    // A Stable artifact needs its own marker to find the end of the Stable part.
    LayoutFixture stable_only({head_skill()});
    expect_layout_refusal(ContextStabilityRefusal::PrefixBoundaryUnavailable, [&] {
        (void)RuntimeTurnAssembler(stable_only.store, std::vector<std::string>{stable_only.artifacts.front().id()})
            .assemble(provider, "owner", *stable_only.epoch, assembly_template());
    });
}

TEST(StableFirstAssembly, RefusesAConversationThatContainsTheTextOfTheMarker) {
    auto provider = assembly_provider();
    sp::Message user = test::message("neograph-prefix-boundary-marker-6f1d0c", sp::Role::User);
    LayoutFixture fixture({head_skill(), tail_note("reading")});
    expect_layout_refusal(ContextStabilityRefusal::PrefixBoundaryUnavailable, [&] {
        (void)assemble_for(fixture, provider, {}, {user});
    });
}
