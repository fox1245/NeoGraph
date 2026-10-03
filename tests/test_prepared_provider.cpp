#include <gtest/gtest.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include "fixtures/typed_provider.h"

using namespace neograph;
using namespace std::chrono_literals;
namespace {
struct Probe {
    unsigned preparations = 0;
    unsigned dispatches = 0;
    std::string admitted_body;
    sp::runtime::Result result = test::success("firstsecond", test::usage(0, 0, 0));
};
class PreparedProbe final : public test::LocalProvider {
public:
    explicit PreparedProbe(std::shared_ptr<Probe> state)
        : LocalProvider([state](ProviderRequest, const PreparedProviderRequest& prepared,
                                const EventCallback& observer) -> asio::awaitable<sp::runtime::Result> {
              ++state->dispatches;
              state->admitted_body = prepared.encoded_body();
              asio::steady_timer timer(co_await asio::this_coro::executor);
              timer.expires_after(20ms);
              co_await timer.async_wait(asio::use_awaitable);
              if (observer) {
                  observer(sp::Begin{"generation"});
                  observer(sp::MessageBegin{{1}, "message", sp::Role::Assistant, {}});
                  observer(sp::PartBegin{{1}, {1}, sp::PartKind::Text, {}, 0});
                  observer(sp::PartDelta{{1}, {sp::PartKind::Text, "first"}});
                  observer(sp::PartSeal{{1}, std::nullopt, {}});
                  observer(sp::PartBegin{{1}, {2}, sp::PartKind::Text, {}, 1});
                  observer(sp::PartDelta{{2}, {sp::PartKind::Text, "second"}});
                  observer(sp::PartSeal{{2}, std::nullopt, {}});
                  observer(sp::MessageSeal{{1}, {}});
              }
              co_return state->result;
          }), state_(std::move(state)) {}
    PreparedProviderRequest prepare(ProviderRequest request) override {
        ++state_->preparations;
        return LocalProvider::prepare(std::move(request));
    }
private:
    std::shared_ptr<Probe> state_;
};
}

TEST(PreparedProvider, ExactAdmittedBodyAndOutcomeSurviveProviderDestruction) {
    auto state = std::make_shared<Probe>();
    auto provider = std::make_unique<PreparedProbe>(state);
    auto request = test::request("owned-model", "owned-prompt");
    request.mode = ProviderMode::Stream;
    std::vector<std::string> deltas;
    request.on_event = [&deltas](const sp::Event& event) {
        if (const auto* delta = std::get_if<sp::PartDelta>(&event))
            deltas.emplace_back(delta->payload.bytes);
    };
    auto operation = provider->invoke_async(request);
    EXPECT_EQ(state->preparations, 1U);
    EXPECT_EQ(state->dispatches, 0U);
    std::get<sp::chat::Request>(request.payload).model = "changed-model";
    request.on_event = {};
    provider.reset();
    const auto outcome = async::run_sync(std::move(operation));
    EXPECT_EQ(outcome, state->result);
    EXPECT_EQ(state->preparations, 1U);
    EXPECT_EQ(state->dispatches, 1U);
    const auto body = json::parse(state->admitted_body);
    EXPECT_EQ(body.at("model"), "owned-model");
    EXPECT_EQ(body.at("messages").at(0).at("content").at(0).at("text"), "owned-prompt");
    EXPECT_EQ(body.at("stream"), true);
    EXPECT_EQ(deltas, (std::vector<std::string>{"first", "second"}));
}

TEST(PreparedProvider, InvalidAdmissionCannotDispatchOrEmitEvents) {
    auto state = std::make_shared<Probe>();
    PreparedProbe provider(state);
    auto request = test::request("", "hello");
    unsigned events = 0;
    request.on_event = [&events](const sp::Event&) { ++events; };
    auto prepared = provider.prepare(std::move(request));
    ASSERT_FALSE(prepared.valid());
    ASSERT_NE(prepared.error(), nullptr);
    EXPECT_EQ(prepared.error()->kind, sp::ErrorKind::InvalidRequest);
    const auto result = provider.dispatch(std::move(prepared));
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*result));
    EXPECT_EQ(std::get<sp::Failure>(*result).error.kind, sp::ErrorKind::InvalidRequest);
    EXPECT_EQ(state->dispatches, 0U);
    EXPECT_EQ(events, 0U);
}

TEST(PreparedProvider, CancellationAfterPrepareCannotStartLocalDispatch) {
    auto state = std::make_shared<Probe>();
    PreparedProbe provider(state);
    auto request = test::request();
    auto token = std::make_shared<graph::CancelToken>();
    request.cancel_token = token;
    auto prepared = provider.prepare(std::move(request));
    ASSERT_TRUE(prepared.valid());
    token->cancel();
    const auto outcome = async::run_sync(provider.dispatch_async(std::move(prepared)));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(*outcome));
    EXPECT_EQ(std::get<sp::Failure>(*outcome).error.kind, sp::ErrorKind::Cancelled);
    EXPECT_EQ(state->dispatches, 0U);
}


TEST(PreparedProvider, AwaitingDispatchLeavesOuterExecutorResponsive) {
    auto state = std::make_shared<Probe>();
    PreparedProbe provider(state);
    asio::io_context io;
    bool completed = false;
    bool other_work_ran_before_completion = false;
    std::exception_ptr error;
    asio::co_spawn(io, provider.invoke_async(test::request()),
        [&](std::exception_ptr failure, sp::runtime::Result result) {
            error = failure;
            completed = true;
            if (!failure) EXPECT_EQ(result, state->result);
        });
    asio::steady_timer heartbeat(io);
    heartbeat.expires_after(1ms);
    heartbeat.async_wait([&](const asio::error_code& failure) {
        EXPECT_FALSE(failure);
        other_work_ran_before_completion = !completed;
    });
    io.run();
    EXPECT_FALSE(error);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(other_work_ran_before_completion);
}

TEST(PreparedProvider, LocalDispatchExceptionsPropagateToSyncCaller) {
    auto state = std::make_shared<unsigned>(0);
    test::LocalProvider provider([state](ProviderRequest, const PreparedProviderRequest&,
        const test::LocalProvider::EventCallback&) -> asio::awaitable<sp::runtime::Result> {
        ++*state;
        throw std::domain_error("fixture failure");
        co_return test::success("unreachable");
    });
    EXPECT_THROW(provider.invoke(test::request()), std::domain_error);
    EXPECT_EQ(*state, 1U);
}
