// What a provider failure costs the token bank, against a real SDK client and a
// loopback peer.
//
// SDK 0.3.1 labels a completed HTTP 4xx answer other than 408, 425 and 429 as
// RetrySafety::RejectedBeforeOutput: the server declined the request, nothing
// ran and nothing is billed. Every other failure that may follow started work
// (408, 425, 429, 5xx, a cut stream, a transport failure after the request
// left) stays PossiblyAccepted, and its reservation stays held. The same SDK
// labels a call's usage as Missing (no evidence), Partial (what was seen) or
// Final, and derives the output and total counts Gemini omits when the output
// budget is spent while thinking. These tests pin what the bank does with each.
#include <gtest/gtest.h>

#include "fixtures/typed_wire_peer.h"

#include <neograph/controlled_provider.h>
#include <neograph/runtime_turn_assembler.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <string>

using namespace neograph;
namespace wire = neograph::test::wire;

namespace {

std::string sha(char value) { return "sha256:" + std::string(64, value); }

ContextAssemblyReceipt assembly(const PreparedProviderRequest& request) {
    ContextEpochData epoch_data;
    epoch_data.run_id = "run";
    epoch_data.sequence = 1;
    epoch_data.raw_window_digest = sha('a');
    const auto epoch = ContextEpoch::create(std::move(epoch_data));
    ContextAssemblyReceiptData data;
    data.context_epoch_id = epoch.id();
    data.normalized_request_digest = RuntimeTurnAssembler::normalized_request_digest(request);
    data.message_window_digest = sha('c');
    return ContextAssemblyReceipt::create(std::move(data), epoch, {});
}

// "fixture-model" with an admitted input limit of 100 and an output limit of 64,
// so a bounded call reserves 164 tokens.
constexpr std::uint64_t kReservation = 164;

sp::descriptor::ValidatedDescriptor bounded_descriptor(const std::string& family,
                                                       const std::string& origin) {
    // A family's own default output cap would exceed the model's admitted limit.
    return test::descriptor(family, origin, test::policy_with_models(
        json::parse(sp::config_defaults::descriptor_policy_json),
        {{family, "fixture-model", 100, 64, json{{"max_output_tokens", nullptr}}}}));
}

std::shared_ptr<Provider> loopback_provider(const std::string& family, const std::string& origin) {
    sp::runtime::Options options;
    options.workers = 1;
    options.default_timeout = std::chrono::seconds(5);
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    return std::shared_ptr<Provider>(
        llm::SchemaProvider::create(bounded_descriptor(family, origin), std::move(options)));
}

// One bounded call through the controlled provider, the way a host admits it:
// a token reservation, a durable dispatch receipt, the dispatch, the settlement.
struct Bounded {
    sp::runtime::Result result;
    std::shared_ptr<UsageAccumulator> bank = std::make_shared<UsageAccumulator>();
    std::shared_ptr<InMemoryProviderDispatchReceiptStore> receipts =
        std::make_shared<InMemoryProviderDispatchReceiptStore>();
    ProviderDispatchState state = ProviderDispatchState::Unavailable;
    std::uint64_t claimed = 0;
    std::uint64_t charged() const { return bank->authority_snapshot().charged; }
    std::uint64_t reserved() const { return bank->authority_snapshot().reserved; }
};

Bounded dispatch_bounded(const std::string& family, wire::Peer& peer,
                         ProviderMode mode = ProviderMode::Collect) {
    Bounded call;
    auto provider = loopback_provider(family, peer.origin());
    auto prepared = provider->prepare(wire::request(family, mode));
    if (!prepared.valid() || prepared.error()) throw std::runtime_error("fixture preparation rejected");
    const auto receipt = assembly(prepared);
    const auto ceiling = Provider::conservative_token_upper_bound(prepared);
    if (!ceiling) throw std::runtime_error("fixture call cannot be bounded");
    auto claim = reserve_provider_dispatch(prepared, ProviderDispatchBudget{
        call.bank, *ceiling, std::make_shared<std::atomic_bool>(false)});
    call.claimed = claim.amount();
    ControlledProvider controlled(provider, call.receipts, sha('f'));
    call.result = controlled.dispatch_prepared("call-1", receipt, std::move(prepared), std::move(claim));
    call.state = call.receipts->state("call-1");
    return call;
}

std::size_t requests_seen(wire::Peer& peer) {
    std::lock_guard lock(peer.state->mutex);
    return peer.state->entered;
}

const sp::Failure& failure_of(const sp::runtime::Result& result) {
    if (!result || !std::holds_alternative<sp::Failure>(*result))
        throw std::runtime_error("the call was expected to fail");
    return std::get<sp::Failure>(*result);
}

std::string error_body(int status) {
    return json{{"error", {{"message", "marker-not-for-logs"}, {"type", "fixture_error"},
                           {"code", status}}}}.dump();
}

// ---------------------------------------------------------------------------
// HTTP answers
// ---------------------------------------------------------------------------

class Declined : public ::testing::TestWithParam<int> {};

TEST_P(Declined, ACompletedHttp4xxAnswerReleasesTheHoldAndChargesNothing) {
    const int status = GetParam();
    wire::Peer peer(error_body(status));
    peer.state->status = status;
    const auto call = dispatch_bounded("openai.chat", peer);
    const auto& failed = failure_of(call.result);
    EXPECT_EQ(failed.error.http_status, status);
    EXPECT_EQ(failed.error.retry_safety, sp::RetrySafety::RejectedBeforeOutput);
    EXPECT_EQ(call.claimed, kReservation);
    EXPECT_EQ(call.charged(), 0U) << "the provider declined the request: nothing is billed";
    EXPECT_EQ(call.reserved(), 0U) << "so the reservation is released";
    EXPECT_EQ(call.bank->total_tokens_wide(), 0U);
    EXPECT_EQ(call.state, ProviderDispatchState::Failed)
        << "a declined request leaves nothing to reconcile";
    EXPECT_EQ(requests_seen(peer), 1U);
}

INSTANTIATE_TEST_SUITE_P(Statuses, Declined, ::testing::Values(400, 401, 402, 403, 404, 413, 422),
                         [](const auto& info) { return "Http" + std::to_string(info.param); });

class MayHaveStartedWork : public ::testing::TestWithParam<int> {};

TEST_P(MayHaveStartedWork, AnAnswerThatCanFollowStartedWorkKeepsTheHold) {
    const int status = GetParam();
    wire::Peer peer(error_body(status));
    peer.state->status = status;
    const auto call = dispatch_bounded("openai.chat", peer);
    const auto& failed = failure_of(call.result);
    EXPECT_EQ(failed.error.http_status, status);
    EXPECT_EQ(failed.error.retry_safety, sp::RetrySafety::PossiblyAccepted);
    EXPECT_EQ(call.charged(), 0U);
    EXPECT_EQ(call.reserved(), kReservation) << "the provider may have started: the hold stays";
    EXPECT_EQ(call.bank->total_tokens_wide(), kReservation);
    EXPECT_EQ(call.state, ProviderDispatchState::ReconciliationRequired);
    EXPECT_EQ(requests_seen(peer), 1U);
}

INSTANTIATE_TEST_SUITE_P(Statuses, MayHaveStartedWork, ::testing::Values(408, 425, 429, 500, 502, 503, 504),
                         [](const auto& info) { return "Http" + std::to_string(info.param); });

// ---------------------------------------------------------------------------
// Usage labels
// ---------------------------------------------------------------------------

TEST(UsageLabels, AGeminiCallWhoseCandidateCountIsOmittedSettlesFromTheDerivedTotals) {
    // The output budget is spent while thinking: the prompt, thoughts and total
    // counts are reported and the candidates count is not (SchemaProvider #29).
    const json body = {{"modelVersion", "fixture-model"}, {"responseId", "generation"},
        {"candidates", json::array({{{"content", {{"role", "model"}}}, {"finishReason", "MAX_TOKENS"}, {"index", 0}}})},
        {"usageMetadata", {{"promptTokenCount", 13}, {"thoughtsTokenCount", 12}, {"totalTokenCount", 25}}}};
    wire::Peer peer(body.dump());
    const auto call = dispatch_bounded("google.generate", peer);
    ASSERT_TRUE(std::holds_alternative<sp::Completion>(*call.result))
        << (std::holds_alternative<sp::Failure>(*call.result) ? failure_of(call.result).error.safe_message : "");
    const auto& usage = std::get<sp::Completion>(*call.result).usage;
    EXPECT_EQ(usage.stage, sp::UsageStage::Final);
    ASSERT_TRUE(usage.output_total);
    EXPECT_EQ(usage.output_total->value, 12U);
    EXPECT_EQ(usage.output_total->evidence, sp::Evidence::Derived);
    ASSERT_TRUE(usage.total);
    EXPECT_EQ(usage.total->value, 25U);
    EXPECT_EQ(usage.total->evidence, sp::Evidence::Derived);
    // A derived total is evidence the bank settles on: the call is charged what
    // it used, and the rest of the reservation is returned.
    EXPECT_EQ(call.claimed, kReservation);
    EXPECT_EQ(call.charged(), 25U);
    EXPECT_EQ(call.reserved(), 0U);
    EXPECT_EQ(call.state, ProviderDispatchState::Succeeded);
}

TEST(UsageLabels, ACutMessagesStreamIsNotChargedTheStartPlaceholderAndKeepsItsHold) {
    // message_start reports a placeholder output count (the first token, often
    // none). A stream cut before message_delta has the input counters, labelled
    // partial, and no output counter: no evidence of the output, not zero.
    const std::string events =
        "event: message_start\ndata: " + json{{"type", "message_start"}, {"message", {
            {"id", "msg-1"}, {"type", "message"}, {"role", "assistant"}, {"content", json::array()},
            {"model", "fixture-model"}, {"stop_reason", nullptr}, {"stop_sequence", nullptr},
            {"usage", {{"input_tokens", 2}, {"output_tokens", 1}, {"cache_read_input_tokens", 0},
                       {"cache_creation_input_tokens", 0}}}}}}.dump() + "\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"partial\"}}\n\n";
    wire::Peer peer(events, true);
    const auto call = dispatch_bounded("anthropic.messages", peer, ProviderMode::Stream);
    const auto& failed = failure_of(call.result);
    EXPECT_EQ(failed.error.kind, sp::ErrorKind::Truncated);
    const auto& usage = failed.partial.usage;
    EXPECT_EQ(usage.stage, sp::UsageStage::Partial);
    EXPECT_FALSE(usage.output_total) << "the placeholder of message_start is not an output count";
    EXPECT_FALSE(usage.total);
    // What the bank reports is what was seen, and the whole hold is charged.
    const auto reported = call.bank->snapshot();
    EXPECT_EQ(reported.stage, sp::UsageStage::Partial);
    EXPECT_FALSE(reported.output_total);
    EXPECT_FALSE(reported.total);
    EXPECT_EQ(call.charged(), 0U);
    EXPECT_EQ(call.reserved(), kReservation) << "a cut call is not settled by partial usage";
    EXPECT_EQ(call.state, ProviderDispatchState::ReconciliationRequired);
}

// ---------------------------------------------------------------------------
// The rule itself
// ---------------------------------------------------------------------------

// What the SDK returns for a completed HTTP 4xx answer: the attempt evidence of
// a request that went out and was answered, no output, no usage, and the
// diagnostic http.error body.
sp::Failure rejection(int status) {
    sp::Failure failure;
    failure.error.kind = sp::ErrorKind::InvalidRequest;
    failure.error.http_status = status;
    failure.error.retry_safety = sp::RetrySafety::RejectedBeforeOutput;
    failure.error.attempt.request_may_have_left = true;
    failure.error.attempt.request_body_bytes = 128;
    failure.error.attempt.response_head_seen = true;
    failure.error.attempt.attempts = 1;
    failure.partial.raw_events.push_back(sp::RawWire{"http.error", nullptr});
    return failure;
}

TEST(ProviderFailureRule, ACompletedHttp4xxRejectionProvesNothingWasAccepted) {
    for (const int status : {400, 401, 402, 403, 404, 405, 409, 410, 413, 422, 451, 499}) {
        SCOPED_TRACE(status);
        const auto failure = rejection(status);
        EXPECT_TRUE(provider_failure_proves_not_accepted(failure));
        EXPECT_TRUE(provider_failure_proves_rejected_before_output(failure));
        EXPECT_FALSE(provider_failure_proves_not_sent(failure))
            << "the request went out: the older, narrower rule still means no request left";
    }
}

TEST(ProviderFailureRule, NothingElseThanAnHttp4xxRejectionCountsAsOne) {
    // 408, 425 and 429 may follow started work, whatever label a failure carries.
    for (const int status : {0, 200, 301, 408, 425, 429, 500, 502, 503, 504, 600}) {
        SCOPED_TRACE(status);
        EXPECT_FALSE(provider_failure_proves_not_accepted(rejection(status)));
    }
    // The label comes from the SDK: a status alone proves nothing.
    for (const auto safety : {sp::RetrySafety::PossiblyAccepted, sp::RetrySafety::OutputObserved}) {
        auto failure = rejection(401);
        failure.error.retry_safety = safety;
        EXPECT_FALSE(provider_failure_proves_not_accepted(failure));
    }
}

TEST(ProviderFailureRule, ContradictoryEvidenceKeepsTheHold) {
    const std::vector<std::pair<const char*, std::function<void(sp::Failure&)>>> contradictions{
        {"an earlier attempt that could have been billed", [](sp::Failure& f) { f.error.attempt.prior_usage_unknown = true; }},
        {"a transport resend", [](sp::Failure& f) { f.error.attempt.transport_internal_resends = 1; }},
        {"no sign that the request went out", [](sp::Failure& f) { f.error.attempt.request_may_have_left = false; }},
        {"no response head", [](sp::Failure& f) { f.error.attempt.response_head_seen = false; }},
        {"partial output", [](sp::Failure& f) { f.partial.messages.push_back(test::message("partial")); }},
        {"a stop reason", [](sp::Failure& f) { f.partial.stop = sp::StopReason{}; }},
        {"a wire envelope", [](sp::Failure& f) { f.partial.wire_envelope = test::document("{}"); }},
        {"an event that is not the http.error body", [](sp::Failure& f) { f.partial.raw_events.push_back(sp::RawWire{"message.delta", nullptr}); }},
        {"a usage stage", [](sp::Failure& f) { f.partial.usage.stage = sp::UsageStage::Partial; }},
        {"a counter", [](sp::Failure& f) { f.partial.usage.input_total = sp::Count{1, sp::Evidence::Reported}; }},
        {"an inconsistent usage record", [](sp::Failure& f) { f.partial.usage.quality = sp::UsageQuality::Inconsistent; }},
        {"a reported cost", [](sp::Failure& f) { f.partial.usage.provider_cost.total = sp::UsdAmount{1}; }},
    };
    for (const auto& [name, change] : contradictions) {
        SCOPED_TRACE(name);
        auto failure = rejection(401);
        change(failure);
        EXPECT_FALSE(provider_failure_proves_not_accepted(failure));
    }
}

TEST(ProviderFailureRule, NotSentKeepsItsOwnEvidenceRequirements) {
    sp::Failure failure;
    failure.error.retry_safety = sp::RetrySafety::NotSent;
    EXPECT_TRUE(provider_failure_proves_not_sent(failure));
    EXPECT_TRUE(provider_failure_proves_not_accepted(failure));
    failure.error.attempt.request_may_have_left = true;
    EXPECT_FALSE(provider_failure_proves_not_accepted(failure));
}

}  // namespace
