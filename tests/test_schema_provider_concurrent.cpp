#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;

TEST(SchemaProviderConcurrent, ParallelInvocationsNeverSendEmptyOrMalformedBody) {
    wire::Peer peer(R"({"id":"msg-fixture","type":"message","model":"fixture-model","role":"assistant","content":[{"type":"text","text":"pong"}],"stop_reason":"end_turn","usage":{"input_tokens":3,"output_tokens":2}})");
    auto provider = wire::provider("anthropic.messages", peer.origin());
    constexpr int callers = 8, iterations = 12;
    std::vector<std::future<std::vector<sp::runtime::Result>>> jobs;
    for (int i = 0; i < callers; ++i) {
        jobs.push_back(std::async(std::launch::async, [p = provider.get(), i] {
            std::vector<sp::runtime::Result> outcomes;
            for (int j = 0; j < iterations; ++j)
                outcomes.push_back(p->invoke(wire::request("anthropic.messages", ProviderMode::Collect,
                    "caller-" + std::to_string(i) + "-" + std::to_string(j))));
            return outcomes;
        }));
    }
    for (auto& job : jobs) for (const auto& outcome : job.get()) {
        ASSERT_TRUE(outcome);
        ASSERT_TRUE(std::holds_alternative<sp::Completion>(*outcome));
        EXPECT_EQ(test::text(outcome), "pong");
        const auto& usage = test::completion(outcome).usage;
        ASSERT_TRUE(usage.input_uncached);
        EXPECT_EQ(usage.input_uncached->value, 3u);
        EXPECT_FALSE(usage.input_total);
    }
    std::lock_guard lock(peer.state->mutex);
    ASSERT_EQ(peer.state->requests.size(), callers * iterations);
    std::set<std::string> observed;
    for (const auto& body : peer.state->requests) {
        EXPECT_EQ(body.at("model"), "fixture-model");
        EXPECT_EQ(body.at("max_tokens"), 64);
        const auto text = body.at("messages").at(0).at("content").at(0).at("text").get<std::string>();
        EXPECT_TRUE(observed.insert(text).second);
    }
}

TEST(SchemaProviderConcurrent, RateLimitedFailureCarriesRetryAfter) {
    wire::Peer peer(R"({"error":{"type":"rate_limit_error","message":"busy"}})");
    peer.state->status = 429;
    peer.state->retry_after = "7";
    auto provider = wire::provider("anthropic.messages", peer.origin());
    const auto result = provider->invoke(wire::request("anthropic.messages"));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::RateLimited);
    EXPECT_EQ(wire::failure(result).error.http_status, 429);
    ASSERT_TRUE(wire::failure(result).error.retry_after);
    EXPECT_EQ(*wire::failure(result).error.retry_after, std::chrono::seconds(7));
    EXPECT_EQ(peer.state->entered, 1u);
}

TEST(SchemaProviderConcurrent, MissingRetryAfterRemainsUnknown) {
    wire::Peer peer(R"({"error":{"type":"rate_limit_error","message":"busy"}})");
    peer.state->status = 429;
    auto provider = wire::provider("anthropic.messages", peer.origin());
    const auto result = provider->invoke(wire::request("anthropic.messages"));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(wire::failure(result).error.kind, sp::ErrorKind::RateLimited);
    EXPECT_FALSE(wire::failure(result).error.retry_after);
    EXPECT_EQ(peer.state->entered, 1u);
}
