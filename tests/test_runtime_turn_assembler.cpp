#include <gtest/gtest.h>

#include <neograph/runtime_turn_assembler.h>
#include <neograph/graph/cancel.h>
#include "fixtures/typed_provider.h"

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

class AssemblyProvider final : public test::LocalProvider {
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
