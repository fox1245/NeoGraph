#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;

namespace {
void socket_cancel(ProviderMode mode) {
    wire::Peer peer(wire::chat_sse(), true);
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request("openai.chat", mode);
    auto token = std::make_shared<graph::CancelToken>();
    request.cancel_token = token;
    asio::io_context io;
    auto result = asio::co_spawn(io, provider->invoke_async(std::move(request)), asio::use_future);
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(1);
    token->cancel();
    const auto status = result.wait_for(1s);
    peer.release();
    runner.join();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    const auto outcome = result.get();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::Cancelled);
    EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
    const auto& partial = wire::failure(outcome).partial;
    EXPECT_TRUE(partial.messages.empty());
    EXPECT_TRUE(partial.raw_events.empty());
    EXPECT_EQ(partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(partial.usage.input_total);
    EXPECT_FALSE(partial.usage.output_total);
    EXPECT_FALSE(partial.usage.total);
    EXPECT_FALSE(partial.usage.provider_reported_total);
}
}

TEST(SchemaProviderAsync, CancelTokenAbortsBufferedSocket) { socket_cancel(ProviderMode::Collect); }
TEST(SchemaProviderAsync, CancelTokenAbortsHttpSseSocket) { socket_cancel(ProviderMode::Stream); }

TEST(SchemaProviderAsync, PerCallDeadlineRemainsTyped) {
    wire::Peer peer(wire::chat_response());
    peer.state->hold = true;
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request();
    request.options.deadline = std::chrono::steady_clock::now() + 100ms;
    auto result = std::async(std::launch::async, [p = provider.get(), owned = std::move(request)]() mutable {
        return p->invoke(std::move(owned));
    });
    const bool entered = peer.await_requests(1);
    const auto status = result.wait_for(1s);
    peer.release();
    const auto outcome = result.get();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::DeadlineExceeded);
    EXPECT_TRUE(wire::failure(outcome).error.attempt.request_may_have_left);
    EXPECT_EQ(wire::failure(outcome).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(wire::failure(outcome).partial.usage.total);
}

TEST(SchemaProviderAsync, PreparedDeadlineIsNotRenewedAtDispatch) {
    wire::Peer peer(wire::chat_response());
    auto provider = wire::provider("openai.chat", peer.origin());
    auto request = wire::request();
    request.options.deadline = std::chrono::steady_clock::now() + 30ms;
    auto prepared = provider->prepare(std::move(request));
    ASSERT_TRUE(prepared.valid());
    std::this_thread::sleep_for(50ms);
    const auto outcome = provider->dispatch(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(wire::failure(outcome).error.kind, sp::ErrorKind::DeadlineExceeded);
    EXPECT_FALSE(wire::failure(outcome).error.attempt.request_may_have_left);
    EXPECT_EQ(wire::failure(outcome).error.attempt.attempts, 0u);
    EXPECT_EQ(wire::failure(outcome).partial.usage.stage, sp::UsageStage::Missing);
    EXPECT_FALSE(wire::failure(outcome).partial.usage.total);
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->entered, 0u);
}
