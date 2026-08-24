#include <gtest/gtest.h>

#include <neograph/runtime_turn_assembler.h>
#include <neograph/graph/cancel.h>

using namespace neograph;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

RuntimeHistoryRecord history(std::uint64_t sequence, std::optional<std::string> predecessor) {
    RuntimeHistoryRecordData data;
    data.feed_id = "feed";
    data.sequence = sequence;
    data.message_id = "message_" + std::to_string(sequence);
    data.trust = RuntimeTrustClass::UntrustedInput;
    data.message = {"user", "user_" + std::to_string(sequence)};
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

    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(
                     "owner", epoch, CompletionRequest::stream(params)),
                 std::invalid_argument);
    auto turn = RuntimeTurnAssembler(store, {early.id()}).assemble(
        "owner", epoch, CompletionRequest::stream(params));

    ASSERT_EQ(turn.request.mode(), CompletionMode::STREAM);
    ASSERT_FALSE(turn.request.on_chunk());
    ASSERT_EQ(turn.request.params().messages.size(), 4u);
    EXPECT_EQ(turn.request.params().messages[0].content, "user_1");
    EXPECT_EQ(turn.request.params().messages[0].role, "user");
    EXPECT_EQ(turn.request.params().messages[1].content, "early");
    EXPECT_EQ(turn.request.params().messages[1].role, "system");
    EXPECT_EQ(turn.request.params().messages[2].content, "user_2");
    EXPECT_EQ(turn.request.params().messages[2].role, "user");
    EXPECT_EQ(turn.request.params().messages[3].content, "late");
    EXPECT_EQ(turn.request.params().messages[3].role, "user");
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
    assistant_data.message.role = "assistant";
    assistant_data.message.content = "working";
    assistant_data.message.tool_calls = {ToolCall{"call_1", "read", "{}"}};
    assistant_data.predecessor_id = first.id();
    const auto assistant = RuntimeHistoryRecord::create(std::move(assistant_data));
    ASSERT_EQ(store.append_history(feed, assistant, first.id()),
              ContextStoreAppendResult::Appended);
    RuntimeHistoryRecordData tool_data;
    tool_data.feed_id = "feed";
    tool_data.sequence = 3;
    tool_data.message_id = "tool_3";
    tool_data.trust = RuntimeTrustClass::ToolOutput;
    tool_data.message.role = "tool";
    tool_data.message.content = "file contents";
    tool_data.message.tool_call_id = "call_1";
    tool_data.message.tool_name = "read";
    tool_data.message.tool_status = "succeeded";
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
    CompletionParams params;
    params.model = "model";
    const auto turn = RuntimeTurnAssembler(store, 4096, requirements).assemble(
        "owner", epoch, CompletionRequest::collect(params));
    ASSERT_EQ(turn.request.params().messages.size(), 5u);
    EXPECT_EQ(turn.request.params().messages[0].role, "system");
    EXPECT_EQ(turn.request.params().messages[0].content, "Host-owned constraint.");
    EXPECT_EQ(turn.request.params().messages[1].role, "user");
    EXPECT_EQ(turn.request.params().messages[1].content, "user_1");
    EXPECT_EQ(turn.request.params().messages[2].role, "assistant");
    EXPECT_EQ(turn.request.params().messages[3].role, "tool");
    EXPECT_EQ(turn.request.params().messages[4].role, "user");
    EXPECT_EQ(turn.request.params().messages[4].content,
              supplemental.content().get<std::string>());
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
    assistant_data.message.role = "assistant";
    assistant_data.message.tool_calls = {ToolCall{"call_1", "read", "{}"}};
    assistant_data.predecessor_id = first.id();
    const auto assistant = RuntimeHistoryRecord::create(std::move(assistant_data));
    ASSERT_EQ(store.append_history(feed, assistant, first.id()),
              ContextStoreAppendResult::Appended);

    RuntimeHistoryRecordData tool_data;
    tool_data.feed_id = "feed";
    tool_data.sequence = 3;
    tool_data.message_id = "tool_3";
    tool_data.trust = RuntimeTrustClass::ToolOutput;
    tool_data.message.role = "tool";
    tool_data.message.content = "recent result";
    tool_data.message.tool_call_id = "call_1";
    tool_data.message.tool_name = "read";
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
    CompletionParams params;
    params.model = "model";
    const auto turn = RuntimeTurnAssembler(store, 4096, requirements).assemble(
        "owner", epoch, CompletionRequest::collect(params));

    ASSERT_EQ(turn.request.params().messages.size(), 3U);
    EXPECT_EQ(turn.request.params().messages[0].role, "user");
    EXPECT_EQ(turn.request.params().messages[0].content,
              prefix.content().get<std::string>());
    EXPECT_EQ(turn.request.params().messages[1].role, "assistant");
    EXPECT_EQ(turn.request.params().messages[2].role, "tool");
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
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble(
                     "owner", epoch, CompletionRequest::collect(params)),
                 std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {}, 1).assemble(
                     "owner", epoch, CompletionRequest::collect(params)),
                 ContextBudgetBlocked);
    EXPECT_NO_THROW(RuntimeTurnAssembler(store, {}, 1024).assemble(
        "owner", epoch, CompletionRequest::collect(params)));
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
    CompletionParams params;
    params.model = "model";
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id()}).assemble(
                     "owner", omitted, CompletionRequest::collect(params)), std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id(), skill.id()}), std::invalid_argument);
    EXPECT_THROW(RuntimeTurnAssembler(store, {"not-an-identity"}), std::invalid_argument);

    ContextEpochData no_user_data;
    no_user_data.run_id = "run-user";
    no_user_data.sequence = 1;
    no_user_data.raw_window_digest = sha('f');
    no_user_data.artifact_ids = {skill.id()};
    const auto no_user = ContextEpoch::create(std::move(no_user_data));
    EXPECT_THROW(RuntimeTurnAssembler(store, {skill.id()}).assemble(
                     "owner", no_user, CompletionRequest::collect(params)), std::invalid_argument);
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
    CompletionParams params;
    params.model = "model";
    RuntimeContextRequirements requirements;
    requirements.required_artifact_ids = {constraint.id()};
    const auto turn = RuntimeTurnAssembler(store, 1000, requirements).assemble(
        "owner", epoch, CompletionRequest::collect(params));
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
                     "owner", omitted, CompletionRequest::collect(params)),
                 std::invalid_argument);
}

TEST(RuntimeTurnAssembler, NormalizedDigestCoversRequestShapeButExcludesCancellationIdentity) {
    CompletionParams params;
    params.model = "model";
    params.messages = {{"user", "hello"}};
    params.temperature = 0.2f;
    params.max_tokens = 12;
    params.timeout_seconds = 3;
    params.extra_fields = json{{"reasoning", "low"}};
    params.tools = {{"tool", "description", json{{"type", "object"}}}};
    const auto baseline = RuntimeTurnAssembler::normalized_request_digest(
        CompletionRequest::collect(params));
    auto cancellation_variant = params;
    cancellation_variant.cancel_token = std::make_shared<graph::CancelToken>();
    EXPECT_EQ(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(cancellation_variant)));
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::stream(params)));
    auto changed = params;
    changed.model = "other-model";
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.messages[0].content = "other";
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.temperature = 0.3f;
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.extra_fields = json{{"reasoning", "high"}};
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.tools[0].description = "other description";
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.max_tokens = 13;
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
    changed = params;
    changed.timeout_seconds = 4;
    EXPECT_NE(baseline, RuntimeTurnAssembler::normalized_request_digest(
                            CompletionRequest::collect(changed)));
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
    CompletionParams params;
    params.model = "model";
    const auto turn = RuntimeTurnAssembler(store).assemble(
        "owner", epoch, CompletionRequest::collect(params));
    EXPECT_EQ(turn.request.params().messages[0].content, "rendered");

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
                     "owner", ContextEpoch::create(std::move(extra_epoch_data)),
                     CompletionRequest::collect(params)),
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
    CompletionParams params;
    params.model = "model";
    params.messages.push_back({"user", "injected"});
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble("owner", epoch, CompletionRequest::collect(params)),
                 std::invalid_argument);
    params.messages.clear();
    EXPECT_THROW(RuntimeTurnAssembler(store).assemble("owner", epoch, CompletionRequest::collect(params)),
                 std::invalid_argument);
}
