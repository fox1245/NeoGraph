#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/controlled_provider.h>
#include <neograph/history.h>
#include <neograph/runtime_interposition_controller.h>
#include <neograph/strict_runtime.h>
#include "fixtures/typed_provider.h"
#include <codecs/messages.h>

#include <memory>
#include <span>
#include <mutex>
#include <string>
#include <vector>

using namespace neograph;
using history::CompactionStatus;

namespace {

constexpr std::string_view kSummaryLabel = "Summary of earlier conversation";

sp::runtime::Result stopped(std::string text, sp::StopKind kind) {
    sp::Completion completion;
    completion.messages.push_back(test::message(std::move(text)));
    completion.stop.kind = kind;
    return std::make_shared<const sp::Outcome>(std::move(completion));
}

std::string text_of(const sp::Message& message) {
    std::string value;
    for (const auto& part : message.parts)
        if (const auto* text = std::get_if<sp::Text>(&part)) value += text->value;
    return value;
}

std::string digest(std::span<const sp::Message> messages) {
    std::string value;
    for (const auto& message : messages) value += message_projection_json(message).dump() + "\n";
    return value;
}

// system + (user, assistant) x pairs; every text carries a long tail so a small
// budget is always crossed.
std::vector<sp::Message> conversation(int pairs = 10) {
    std::vector<sp::Message> messages;
    messages.push_back(test::message("be helpful", sp::Role::System));
    for (int index = 0; index < pairs; ++index) {
        messages.push_back(test::message(
            "question " + std::to_string(index) + std::string(80, 'q'), sp::Role::User));
        messages.push_back(test::message("answer " + std::to_string(index) + std::string(80, 'a')));
    }
    return messages;
}

sp::Message tool_call(std::string id) {
    sp::Message message;
    message.parts.emplace_back(sp::ToolCall{std::move(id), "lookup", sp::ToolCallKind::ClientExecuted,
                                            test::document(R"({"q":"x"})")});
    return message;
}

sp::Message tool_result(std::string id) {
    sp::Message message;
    message.role = sp::Role::Tool;
    message.parts.emplace_back(sp::ToolResult{std::move(id), "result text"});
    return message;
}

sp::messages::Request native_request(const std::vector<sp::Message>& messages) {
    sp::messages::Request request;
    request.model = "fixture-model";
    request.account_scope = "fixture-account";
    request.messages = messages;
    return request;
}

// Capture the seal from the exact prefix that will precede it in replay.
sp::Message native_text(const sp::descriptor::ValidatedDescriptor& descriptor,
                        const std::vector<sp::Message>& prefix) {
    auto encoded = sp::messages::encode(descriptor, native_request(prefix), false);
    const auto context = std::get<sp::messages::EncodedRequest>(std::move(encoded)).context;
    sp::Accumulator accumulator;
    sp::messages::Codec codec(descriptor, sp::messages::Mode::Buffered, accumulator, context);
    codec.buffered(R"({"id":"msg-native","type":"message","role":"assistant","model":"fixture-model",
        "content":[{"type":"text","text":"native text must stay intact"}],
        "stop_reason":"end_turn","stop_sequence":null,
        "usage":{"input_tokens":2,"output_tokens":3}})", {});
    codec.finish();
    return std::get<sp::Completion>(*accumulator.outcome()).messages.at(0);
}

// Scripted summarizer: returns the queued outcome and records every request it saw.
struct Script {
    std::mutex mutex;
    sp::runtime::Result reply;
    std::vector<sp::chat::Request> requests;
};

std::shared_ptr<test::LocalProvider> scripted(std::shared_ptr<Script> script) {
    return std::make_shared<test::LocalProvider>(
        [script](ProviderRequest request, const auto&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            std::lock_guard lock(script->mutex);
            script->requests.push_back(std::get<sp::chat::Request>(request.payload));
            co_return script->reply;
        });
}

struct Summarizer {
    std::shared_ptr<Script> script = std::make_shared<Script>();
    std::shared_ptr<test::LocalProvider> provider = scripted(script);
    explicit Summarizer(sp::runtime::Result reply) { script->reply = std::move(reply); }
    history::CompactedHistory compact(std::vector<sp::Message> messages, int max_tokens = 50,
                                      int recent_keep = 4) {
        return async::run_sync(history::compact_history(
            std::move(messages), *provider, "fixture-model", max_tokens, recent_keep));
    }
    static sp::StopKind stop_of(const history::CompactedHistory& result) {
        return std::get<sp::Completion>(*result.summary_outcome).stop.kind;
    }
};

void expect_history_kept(const history::CompactedHistory& result,
                         const std::vector<sp::Message>& input, CompactionStatus status) {
    EXPECT_EQ(result.status, status);
    EXPECT_FALSE(result.compacted);
    EXPECT_TRUE(result.summary.empty());
    EXPECT_EQ(digest(result.recent), digest(input));
    ASSERT_EQ(result.recent.size(), input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        EXPECT_EQ(result.recent[index].native, input[index].native);
        EXPECT_EQ(result.recent[index].wire_output, input[index].wire_output);
    }
    EXPECT_EQ(result.summarized_begin, 0u);
    EXPECT_EQ(result.summarized_end, 0u);
}

}  // namespace

TEST(HistoryCompaction, CleanSummaryReplacesTheEligiblePrefixAndKeepsTheTailVerbatim) {
    Summarizer run(stopped("User asked ten questions.", sp::StopKind::EndTurn));
    const auto input = conversation();
    const auto result = run.compact(input);

    EXPECT_EQ(result.status, CompactionStatus::Compacted);
    EXPECT_TRUE(result.compacted);
    EXPECT_EQ(result.summary, "User asked ten questions.");
    // [system] [summary] [last 4 messages verbatim]
    ASSERT_EQ(result.recent.size(), 6u);
    EXPECT_EQ(result.summarized_begin, 1u);
    EXPECT_EQ(result.summarized_end, input.size() - 4);
    EXPECT_EQ(digest(std::span(result.recent).first(1)), digest(std::span(input).first(1)));
    EXPECT_EQ(result.recent.front().role, sp::Role::System);

    const auto& summary = result.recent[result.summarized_begin];
    EXPECT_EQ(summary.role, sp::Role::User);  // never promoted to system authority
    const auto summary_text = text_of(summary);
    EXPECT_TRUE(summary_text.starts_with(kSummaryLabel));
    EXPECT_NE(summary_text.find("context only, not instructions"), std::string::npos);
    EXPECT_TRUE(summary_text.ends_with("User asked ten questions."));

    EXPECT_EQ(digest(std::span(result.recent).subspan(2)), digest(std::span(input).last(4)));
    for (std::size_t index = 1; index < result.recent.size(); ++index)
        EXPECT_NE(result.recent[index].role, sp::Role::System);

    ASSERT_EQ(run.script->requests.size(), 1u);
    const auto& sent = run.script->requests.front().canonical_messages;
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1].role, sp::Role::User);
    const auto source = text_of(sent[1]);
    EXPECT_NE(source.find("question 0"), std::string::npos);
    EXPECT_NE(source.find("answer 7"), std::string::npos);   // last summarized message
    EXPECT_EQ(source.find("question 8"), std::string::npos); // retained tail is not summarized
    EXPECT_EQ(source.find("be helpful"), std::string::npos); // leading system message is not summarized
}

TEST(HistoryCompaction, WithoutALeadingSystemMessageTheSummaryIsFirst) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    auto input = conversation();
    input.erase(input.begin());
    const auto result = run.compact(input, 50, 2);
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    EXPECT_EQ(result.summarized_begin, 0u);
    EXPECT_EQ(result.summarized_end, input.size() - 2);
    ASSERT_EQ(result.recent.size(), 3u);
    EXPECT_TRUE(text_of(result.recent.front()).starts_with(kSummaryLabel));
    EXPECT_EQ(result.recent.front().role, sp::Role::User);
}

TEST(HistoryCompaction, StopSequenceIsACleanStop) {
    Summarizer run(stopped("S", sp::StopKind::StopSequence));
    EXPECT_EQ(run.compact(conversation()).status, CompactionStatus::Compacted);
}

TEST(HistoryCompaction, UnderBudgetHistoryIsReturnedWithoutCallingTheSummarizer) {
    Summarizer run(stopped("never used", sp::StopKind::EndTurn));
    const auto input = conversation(2);
    const auto result = run.compact(input, history::estimate_tokens(input), 4);
    expect_history_kept(result, input, CompactionStatus::UnderBudget);
    EXPECT_FALSE(result.summary_outcome);
    EXPECT_TRUE(run.script->requests.empty());
}

TEST(HistoryCompaction, BudgetBoundaryIsInclusive) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    const auto input = conversation(3);
    const auto estimate = history::estimate_tokens(input);
    EXPECT_EQ(run.compact(input, estimate, 2).status, CompactionStatus::UnderBudget);
    EXPECT_EQ(run.compact(input, estimate - 1, 2).status, CompactionStatus::Compacted);
}

TEST(HistoryCompaction, NegativeLimitsAreRejected) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    EXPECT_THROW(run.compact(conversation(), -1, 4), std::invalid_argument);
    EXPECT_THROW(run.compact(conversation(), 50, -1), std::invalid_argument);
    EXPECT_TRUE(run.script->requests.empty());
}

TEST(HistoryCompaction, NothingEligibleWhenTheWholeHistoryIsTheKeptTail) {
    Summarizer run(stopped("never used", sp::StopKind::EndTurn));
    const auto input = conversation(3);
    const auto result = run.compact(input, 10, static_cast<int>(input.size()));
    expect_history_kept(result, input, CompactionStatus::NothingEligible);
    EXPECT_FALSE(result.summary_outcome);
    EXPECT_TRUE(run.script->requests.empty());
}

TEST(HistoryCompaction, EmptySummaryKeepsTheWholeHistoryAndReportsIt) {
    Summarizer run(stopped("", sp::StopKind::EndTurn));
    const auto input = conversation();
    const auto result = run.compact(input);
    expect_history_kept(result, input, CompactionStatus::EmptySummary);
    ASSERT_TRUE(result.summary_outcome);
    EXPECT_EQ(run.stop_of(result), sp::StopKind::EndTurn);
}

TEST(HistoryCompaction, TruncatedNonEmptySummaryIsNotAcceptedAsASummary) {
    // Reasoning summarizers hit max_output_tokens mid-sentence; the partial text
    // is real text but not a summary of the whole span.
    Summarizer run(stopped("The user is planning a trip to Ky", sp::StopKind::MaxTokens));
    const auto input = conversation();
    const auto result = run.compact(input);
    expect_history_kept(result, input, CompactionStatus::UnusableSummary);
    ASSERT_TRUE(result.summary_outcome);
    EXPECT_EQ(run.stop_of(result), sp::StopKind::MaxTokens);
    EXPECT_EQ(test::text(result.summary_outcome), "The user is planning a trip to Ky");
}

TEST(HistoryCompaction, EveryNonCleanStopKeepsTheHistory) {
    for (const auto kind : {sp::StopKind::MaxTokens, sp::StopKind::Refusal, sp::StopKind::ContentFilter,
                            sp::StopKind::PauseTurn, sp::StopKind::ContextLimit,
                            sp::StopKind::MalformedCall, sp::StopKind::ToolUse, sp::StopKind::Unknown}) {
        SCOPED_TRACE(static_cast<int>(kind));
        Summarizer run(stopped("plausible looking summary text", kind));
        const auto input = conversation();
        const auto result = run.compact(input);
        expect_history_kept(result, input, CompactionStatus::UnusableSummary);
        ASSERT_TRUE(result.summary_outcome);
        EXPECT_EQ(run.stop_of(result), kind);
    }
}

TEST(HistoryCompaction, AbnormalStopWithoutTextIsUnusableNotEmpty) {
    Summarizer run(stopped("", sp::StopKind::MaxTokens));
    const auto input = conversation();
    expect_history_kept(run.compact(input), input, CompactionStatus::UnusableSummary);
}

TEST(HistoryCompaction, TypedProviderFailureThrowsAndTheCallersHistoryIsUntouched) {
    Summarizer run(test::failure(sp::ErrorKind::RateLimited));
    const auto input = conversation();
    const auto before = digest(input);
    try {
        run.compact(input);
        FAIL() << "typed failure must throw";
    } catch (const ProviderFailure& error) {
        ASSERT_TRUE(error.outcome());
        EXPECT_EQ(std::get<sp::Failure>(*error.outcome()).error.kind, sp::ErrorKind::RateLimited);
    }
    EXPECT_EQ(digest(input), before);
}

TEST(HistoryCompaction, SummarizerRequestUsesTheDocumentedDefaultControls) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    ASSERT_EQ(run.compact(conversation()).status, CompactionStatus::Compacted);
    ASSERT_EQ(run.script->requests.size(), 1u);
    const auto& request = run.script->requests.front();
    EXPECT_EQ(request.max_output_tokens, 500u);
    ASSERT_TRUE(request.temperature);
    EXPECT_DOUBLE_EQ(*request.temperature, 0.2);
    EXPECT_FALSE(request.reasoning_effort);
}

TEST(HistoryCompaction, CallerSuppliedSummarizerControlsAreSent) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    ProviderControls controls;
    controls.max_output_tokens = 4096;
    controls.temperature = 0.9;
    controls.reasoning_effort = "low";
    const auto result = async::run_sync(history::compact_history(
        conversation(), *run.provider, "fixture-model", 50, 4, controls));
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    ASSERT_EQ(run.script->requests.size(), 1u);
    const auto& request = run.script->requests.front();
    EXPECT_EQ(request.max_output_tokens, 4096u);
    ASSERT_TRUE(request.temperature);
    EXPECT_DOUBLE_EQ(*request.temperature, 0.9);
    EXPECT_EQ(request.reasoning_effort, "low");
}

TEST(HistoryCompaction, ExplicitUnsetControlsDoNotForceTemperatureOrReasoningDefaults) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    ProviderControls controls;
    controls.max_output_tokens = 4096;
    const auto result = async::run_sync(history::compact_history(
        conversation(), *run.provider, "fixture-model", 50, 4, controls));
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    ASSERT_EQ(run.script->requests.size(), 1u);
    const auto& request = run.script->requests.front();
    EXPECT_EQ(request.max_output_tokens, 4096u);
    EXPECT_FALSE(request.temperature);
    EXPECT_FALSE(request.reasoning_effort);
    EXPECT_FALSE(request.reasoning);
}

TEST(HistoryCompaction, ControlledOverloadSendsTheSummarizerControlsToo) {
    auto script = std::make_shared<Script>();
    script->reply = stopped("S", sp::StopKind::EndTurn);
    auto provider = scripted(script);
    auto contexts = std::make_shared<InMemoryContextStore>();
    auto receipts = std::make_shared<InMemoryProviderDispatchReceiptStore>();
    auto controller = std::make_shared<RuntimeInterpositionController>(
        provider, contexts, receipts, "sha256:" + std::string(64, 'f'), 100000);

    const ContextStoreFeed feed{"owner", "feed"};
    RuntimeHistoryRecordData record;
    record.feed_id = "feed";
    record.sequence = 1;
    record.message_id = "message";
    record.trust = RuntimeTrustClass::UntrustedInput;
    record.message = test::message("raw history", sp::Role::User);
    ASSERT_EQ(contexts->append_history(feed, RuntimeHistoryRecord::create(std::move(record)), std::nullopt),
              ContextStoreAppendResult::Appended);
    ContextEpochData epoch;
    epoch.run_id = "run";
    epoch.sequence = 1;
    epoch.feed_id = "feed";
    epoch.raw_from_sequence = 1;
    epoch.raw_through_sequence = 1;
    epoch.raw_window_digest = contexts->snapshot_history(feed, 1, 1).digest;
    epoch.guarantee_profile = RuntimeGuaranteeProfile::Recorded;
    controller->activate("owner", ContextEpoch::create(std::move(epoch)));

    ProviderControls controls;
    controls.max_output_tokens = 2048;
    controls.reasoning_effort = "low";
    const auto result = async::run_sync(history::compact_history(
        conversation(), *provider, controller, "fixture-model", 50, 4, controls));
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    ASSERT_EQ(script->requests.size(), 1u);
    EXPECT_EQ(script->requests.front().max_output_tokens, 2048u);
    EXPECT_EQ(script->requests.front().reasoning_effort, "low");
    EXPECT_FALSE(script->requests.front().temperature);
    EXPECT_FALSE(script->requests.front().reasoning);
}

TEST(HistoryCompaction, EstimateCountsReasoningParts) {
    auto reasoning = test::message("ok");
    const auto plain = history::estimate_tokens({reasoning});
    reasoning.parts.emplace(reasoning.parts.begin(), sp::Thinking{std::string(3000, 't'), std::nullopt});
    EXPECT_GE(history::estimate_tokens({reasoning}), plain + 900);
}

TEST(HistoryCompaction, EstimateCountsTypedReasoningSummaryAndEncryptedContent) {
    auto message = test::message("ok");
    const auto plain = history::estimate_tokens({message});
    sp::Reasoning reasoning;
    reasoning.summary = {std::string(3000, 's')};
    reasoning.encrypted_content = std::string(3000, 'e');
    message.parts.emplace_back(std::move(reasoning));
    EXPECT_GE(history::estimate_tokens({message}), plain + 1900);
}

TEST(HistoryCompaction, ReasoningBytesInTheRetainedTailTriggerCompaction) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    std::vector<sp::Message> input;
    for (int index = 0; index < 4; ++index) {
        input.push_back(test::message("hi", sp::Role::User));
        input.push_back(test::message("hello"));
    }
    constexpr int budget = 600;
    ASSERT_LE(history::estimate_tokens(input), budget);
    EXPECT_EQ(run.compact(input, budget, 2).status, CompactionStatus::UnderBudget);

    // Hidden reasoning is replayed and billed as input, so it must count toward
    // the trigger even though the visible text is tiny.
    input.back().parts.emplace(input.back().parts.begin(), sp::Thinking{std::string(3000, 't'), std::nullopt});
    ASSERT_GT(history::estimate_tokens(input), budget);
    const auto result = run.compact(input, budget, 2);
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    EXPECT_EQ(result.summarized_end, input.size() - 2);
    EXPECT_EQ(digest(std::span(result.recent).last(2)), digest(std::span(input).last(2)));
}

TEST(HistoryCompaction, ToolAndStructuredMessagesAreNeverSummarizedAcrossOrFlattened) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    auto input = conversation(3);  // system + 6 text messages
    input.push_back(tool_call("call-1"));
    input.push_back(tool_result("call-1"));
    for (int index = 0; index < 4; ++index) {
        input.push_back(test::message("later question " + std::to_string(index) + std::string(80, 'z'),
                                      sp::Role::User));
        input.push_back(test::message("later answer " + std::to_string(index)));
    }
    const auto result = run.compact(input, 50, 2);
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    // The prefix stops at the first non-text message even though text follows it.
    EXPECT_EQ(result.summarized_begin, 1u);
    EXPECT_EQ(result.summarized_end, 7u);
    ASSERT_EQ(result.recent.size(), input.size() - 7 + 2);
    EXPECT_EQ(digest(std::span(result.recent).subspan(2)), digest(std::span(input).subspan(7)));
    auto recent = result.recent;
    EXPECT_NO_THROW(history::sanitize_tool_calls(recent));
    const auto source = text_of(run.script->requests.front().canonical_messages.at(1));
    EXPECT_EQ(source.find("later question"), std::string::npos);
    EXPECT_EQ(source.find("result text"), std::string::npos);
}

TEST(HistoryCompaction, StructuredWireOutputMessageEndsTheEligiblePrefix) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    auto input = conversation(4);
    input[5].wire_output = test::document("{}");
    const auto result = run.compact(input, 50, 2);
    ASSERT_EQ(result.status, CompactionStatus::Compacted);
    EXPECT_EQ(result.summarized_end, 5u);
    EXPECT_EQ(digest(std::span(result.recent).subspan(2)), digest(std::span(input).subspan(5)));
}

TEST(HistoryCompaction, FirstMessageNonTextLeavesNothingEligible) {
    Summarizer run(stopped("never used", sp::StopKind::EndTurn));
    auto input = conversation(4);
    input[1].wire_output = test::document("{}");
    expect_history_kept(run.compact(input, 50, 2), input, CompactionStatus::NothingEligible);
    EXPECT_TRUE(run.script->requests.empty());
}

TEST(HistoryCompaction, NativeReplayProtectsTheExactBoundPrefixWithoutSummarizerDispatch) {
    Summarizer run(stopped("S", sp::StopKind::EndTurn));
    const auto descriptor = test::descriptor("anthropic.messages");
    auto input = conversation(5);
    input.erase(input.begin());  // Messages history uses user/assistant, not system.
    input.resize(5);             // Exact portable prefix ends with a user turn.
    input.push_back(native_text(descriptor, input));
    input.push_back(test::message("continue", sp::Role::User));
    ASSERT_TRUE(input[5].native);
    ASSERT_EQ(input[5].parts.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<sp::Text>(input[5].parts.front()));
    ASSERT_TRUE(std::holds_alternative<sp::messages::EncodedRequest>(
        sp::messages::encode(descriptor, native_request(input), false)));

    // A changed prefix really breaks SDK replay, even if the seal is retained.
    auto changed = input;
    std::get<sp::Text>(changed[0].parts[0]).value = "changed source";
    const auto rejected = sp::messages::encode(descriptor, native_request(changed), false);
    ASSERT_TRUE(std::holds_alternative<sp::Error>(rejected));
    EXPECT_EQ(std::get<sp::Error>(rejected).kind, sp::ErrorKind::ReplayIneligible);

    // Protect native messages both inside and outside the requested recent tail.
    for (const int keep : {1, 3}) {
        const auto result = run.compact(input, 50, keep);
        expect_history_kept(result, input, CompactionStatus::NativeReplayProtected);
        EXPECT_FALSE(result.summary_outcome);
        EXPECT_TRUE(run.script->requests.empty());
        EXPECT_TRUE(std::holds_alternative<sp::messages::EncodedRequest>(
            sp::messages::encode(descriptor, native_request(result.recent), false)));
    }
}

TEST(HistoryCompaction, UnusableSummaryPreservesFullTypedHistory) {
    Summarizer run(stopped("partial summary", sp::StopKind::MaxTokens));
    auto input = conversation(4);
    input.back().parts.emplace_back(sp::Thinking{std::string(3000, 't'), "signature"});
    input.push_back(tool_call("call-1"));
    input.push_back(tool_result("call-1"));
    const auto result = run.compact(input, 50, 2);
    expect_history_kept(result, input, CompactionStatus::UnusableSummary);
    EXPECT_EQ(result.summary_outcome, run.script->reply);
}

TEST(HistoryCompaction, WhitespaceOnlyCleanSummaryPreservesHistoryAndOutcome) {
    for (const auto stop : {sp::StopKind::EndTurn, sp::StopKind::StopSequence}) {
        Summarizer run(stopped(" \t\n\r\f\v ", stop));
        const auto input = conversation();
        const auto result = run.compact(input);
        expect_history_kept(result, input, CompactionStatus::EmptySummary);
        EXPECT_EQ(result.summary_outcome, run.script->reply);
        EXPECT_EQ(run.stop_of(result), stop);
        EXPECT_EQ(test::text(result.summary_outcome), " \t\n\r\f\v ");
    }
}
