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

// The cost bounds and request facts a prepared request hands a caller (Provider::worst_case_cost,
// endpoint_worst_case_cost, request_facts, endpoint_facts). Every price below is a fixture catalogue row.
namespace {
constexpr std::string_view kMessagesOrigin = "https://api.anthropic.com";
constexpr std::string_view kDirectOrigin   = "https://api.openai.com";
constexpr std::string_view kGatewayOrigin  = "https://openrouter.ai";

// 1 and 2 USD per million input and output tokens: 1,000 and 2,000 nano-USD per token.
const json kListedRates{{"input", "0.000001"}, {"output", "0.000002"}};

test::FixtureModel model_priced(std::string family, std::string model, std::uint64_t input_limit, json pricing) {
    test::FixtureModel fixture{std::move(family), std::move(model), input_limit};
    fixture.pricing = std::move(pricing);
    return fixture;
}

std::shared_ptr<Provider> provider_with(const std::string& family, std::string_view origin,
                                        const std::vector<test::FixtureModel>& models) {
    return llm::SchemaProvider::create(test::descriptor(
        family, std::string(origin),
        test::policy_with_models(json::parse(sp::config_defaults::descriptor_policy_json), models)));
}

// A Messages provider with fast rows (dearer than the standard rows) for one model and none for the other.
std::shared_ptr<Provider> messages_provider() {
    return provider_with("anthropic.messages", kMessagesOrigin,
        {model_priced("anthropic.messages", "with-fast", 1'000,
                      json{{"input", "0.000001"}, {"output", "0.000002"},
                           {"input/fast", "0.000002"}, {"output/fast", "0.000008"}}),
         model_priced("anthropic.messages", "without-fast", 1'000, kListedRates)});
}

PreparedProviderRequest messages_call(Provider& provider, const std::string& model, const json& extensions) {
    ProviderControls controls;
    controls.max_output_tokens = 100;
    if (!extensions.is_null()) controls.messages.extensions = extensions;
    return provider.prepare(make_provider_request(
        provider, model, {test::message("q", sp::Role::User)}, {}, std::move(controls)));
}

PreparedProviderRequest chat_call(Provider& provider, const std::string& model) {
    ProviderControls controls;
    controls.max_output_tokens = 100;
    return provider.prepare(make_provider_request(
        provider, model, {test::message("q", sp::Role::User)}, {}, std::move(controls)));
}

bool has_reason(const std::vector<sp::pricing::UnboundedReason>& reasons, sp::pricing::UnboundedBy by,
                std::string_view subject = {}) {
    for (const auto& reason : reasons)
        if (reason.by == by && (subject.empty() || reason.subject == subject)) return true;
    return false;
}

PreparedProviderRequest embeddings_call(Provider& provider, const std::string& model, std::string_view extensions = {}) {
    sp::embeddings::Request request;
    request.model = model;
    request.inputs.push_back({"hello"});
    if (!extensions.empty()) request.extensions = test::document(extensions);
    return provider.prepare(make_endpoint_request(std::move(request)));
}
}

TEST(PreparedProviderCost, ASpeedFastMessagesRequestIsBoundedAtTheFastRows) {
    auto provider = messages_provider();
    const auto standard = messages_call(*provider, "with-fast", json());
    const auto fast = messages_call(*provider, "with-fast", json{{"speed", "fast"}});
    ASSERT_TRUE(standard.valid());
    ASSERT_TRUE(fast.valid());
    const auto attempts = Provider::invocation_upper_bound(fast);
    ASSERT_TRUE(attempts);
    const auto standard_bound = Provider::worst_case_cost(standard);
    const auto fast_bound = Provider::worst_case_cost(fast);
    ASSERT_TRUE(standard_bound.total) << standard_bound.unbounded.size();
    ASSERT_TRUE(fast_bound.total) << fast_bound.unbounded.size();
    // 1,000 input tokens and 100 output tokens in one attempt.
    EXPECT_EQ(standard_bound.total->nano_usd, (1'000U * 1'000 + 100 * 2'000) * *attempts);
    EXPECT_EQ(fast_bound.total->nano_usd, (1'000U * 2'000 + 100 * 8'000) * *attempts);
    EXPECT_EQ(fast_bound.attempts, *attempts);
    EXPECT_TRUE(fast_bound.unbounded.empty());
    // The facts name the extension and the tier it bills under.
    EXPECT_TRUE(Provider::request_facts(standard).priced_extensions.empty());
    const auto& facts = Provider::request_facts(fast);
    ASSERT_EQ(facts.priced_extensions.size(), 1U);
    EXPECT_EQ(facts.priced_extensions[0].key, "speed");
    EXPECT_EQ(facts.priced_extensions[0].tier, "fast");
    EXPECT_FALSE(facts.priced_extensions[0].unmodeled);
}

TEST(PreparedProviderCost, AFastRequestOnAnEntryWithoutFastRowsHasNoTotal) {
    auto provider = messages_provider();
    const auto bound = Provider::worst_case_cost(messages_call(*provider, "without-fast", json{{"speed", "fast"}}));
    EXPECT_FALSE(bound.total);
    EXPECT_TRUE(has_reason(bound.unbounded, sp::pricing::UnboundedBy::TierPrice)) << bound.unbounded.size();
}

TEST(PreparedProviderCost, AnInferenceGeoThatIsNotGlobalIsUnmodeledAndGlobalIsNot) {
    auto provider = messages_provider();
    const auto regional = messages_call(*provider, "with-fast", json{{"inference_geo", "us"}});
    ASSERT_TRUE(regional.valid());
    const auto bound = Provider::worst_case_cost(regional);
    EXPECT_FALSE(bound.total);
    EXPECT_TRUE(has_reason(bound.unbounded, sp::pricing::UnboundedBy::Unmodeled, "inference_geo"));
    const auto& facts = Provider::request_facts(regional);
    ASSERT_EQ(facts.priced_extensions.size(), 1U);
    EXPECT_EQ(facts.priced_extensions[0].key, "inference_geo");
    EXPECT_TRUE(facts.priced_extensions[0].unmodeled);
    EXPECT_TRUE(Provider::worst_case_cost(messages_call(*provider, "with-fast", json{{"inference_geo", "global"}})).total);
}

TEST(PreparedProviderCost, AListedPriceTheCatalogueCannotReadLeavesNoTotal) {
    // A listed member that no price unit reads and the SDK's table does not know prices something the request may carry.
    auto unread = kListedRates;
    unread["unread_surcharge"] = "0.5";
    auto provider = provider_with("openai.chat", kDirectOrigin,
        {model_priced("openai.chat", "readable", 1'000, kListedRates), model_priced("openai.chat", "unreadable", 1'000, unread)});
    EXPECT_TRUE(Provider::worst_case_cost(chat_call(*provider, "readable")).total);
    const auto bound = Provider::worst_case_cost(chat_call(*provider, "unreadable"));
    EXPECT_FALSE(bound.total);
    EXPECT_TRUE(has_reason(bound.unbounded, sp::pricing::UnboundedBy::ModelPrice)) << bound.unbounded.size();
}

TEST(PreparedProviderCost, AStaleRouteCeilingHasNoTotalAndAFreshOneIsPinned) {
    // The ceiling of "stale" is below the input price the entry lists.
    auto provider = provider_with("openai.chat", kGatewayOrigin,
        {model_priced("openai.chat", "vendor/stale", 1'000,
                      json{{"input", "0.000002"}, {"output", "0.000004"}, {"ceiling_input", "0.000001"}, {"ceiling_output", "0.000004"}}),
         model_priced("openai.chat", "vendor/fresh", 1'000,
                      json{{"input", "0.000001"}, {"output", "0.000002"}, {"ceiling_input", "0.000003"}, {"ceiling_output", "0.000004"}})});
    const auto stale = Provider::worst_case_cost(chat_call(*provider, "vendor/stale"));
    EXPECT_FALSE(stale.total);
    EXPECT_TRUE(has_reason(stale.unbounded, sp::pricing::UnboundedBy::RouteCeiling)) << stale.unbounded.size();
    const auto prepared = chat_call(*provider, "vendor/fresh");
    const auto fresh = Provider::worst_case_cost(prepared);
    ASSERT_TRUE(fresh.total) << fresh.unbounded.size();
    // 1,000 input tokens at 3,000 and 100 output tokens at 4,000 nano-USD, per attempt.
    EXPECT_EQ(fresh.total->nano_usd, (1'000U * 3'000 + 100 * 4'000) * *Provider::invocation_upper_bound(prepared));
    EXPECT_TRUE(fresh.route_ceiling);
    ASSERT_TRUE(fresh.pin);
    EXPECT_EQ(fresh.pin->prompt_nano_usd, 3'000'000'000U);
}

TEST(PreparedProviderCost, TheCatalogueArgumentReplacesThePolicysCatalogue) {
    auto provider = messages_provider();
    const auto prepared = messages_call(*provider, "with-fast", json());
    const auto own = Provider::worst_case_cost(prepared);
    ASSERT_TRUE(own.total);
    const auto* descriptor = prepared.admitted_descriptor();
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(Provider::worst_case_cost(prepared, *descriptor->policy()->catalogue()).total->nano_usd, own.total->nano_usd);
    // A host layer that prices the model dearer is what the bound reads.
    const auto dearer = test::fixture_catalogue(
        {model_priced("anthropic.messages", "with-fast", 1'000, json{{"input", "0.000004"}, {"output", "0.000002"}})});
    const auto layered = Provider::worst_case_cost(prepared, *dearer);
    ASSERT_TRUE(layered.total);
    EXPECT_EQ(layered.total->nano_usd, (1'000U * 4'000 + 100 * 2'000) * *Provider::invocation_upper_bound(prepared));
}

TEST(PreparedProviderCost, AnEmbeddingsCallIsBoundedByItsInputLimitAtTheListedRate) {
    auto provider = provider_with("openai.chat", kDirectOrigin,
        {model_priced("openai.chat", "listed-embed", 1'000, json{{"input", "0.0000001"}})});
    const auto prepared = embeddings_call(*provider, "listed-embed");
    ASSERT_TRUE(prepared.valid());
    ASSERT_TRUE(prepared.is_endpoint());
    const auto bound = Provider::endpoint_worst_case_cost(prepared);
    ASSERT_TRUE(bound.total) << bound.unbounded.size();
    // One input of the entry's 1,000-token limit at 100 nano-USD per token, one attempt (a call charged on acceptance).
    EXPECT_EQ(bound.total->nano_usd, 1'000U * 100);
    EXPECT_EQ(bound.attempts, 1U);
    EXPECT_FALSE(bound.route_ceiling);
    // The chat accessor answers an endpoint call with the same total.
    const auto projected = Provider::worst_case_cost(prepared);
    ASSERT_TRUE(projected.total);
    EXPECT_EQ(projected.total->nano_usd, bound.total->nano_usd);
    const auto& facts = Provider::endpoint_facts(prepared);
    EXPECT_EQ(facts.endpoint, "embeddings");
    EXPECT_EQ(facts.billing, sp::descriptor::EndpointBilling::Model);
    EXPECT_EQ(facts.model, "listed-embed");
    EXPECT_EQ(facts.origin, kDirectOrigin);
    EXPECT_FALSE(facts.routed_gateway);
    EXPECT_FALSE(facts.max_price);
    EXPECT_TRUE(Provider::request_facts(prepared).priced_extensions.empty());
}

TEST(PreparedProviderCost, AnEmbeddingsCallAtAGatewayIsBoundedAtTheLowerOfItsCapAndTheRouteCeiling) {
    auto provider = provider_with("openai.chat", kGatewayOrigin,
        {model_priced("openai.chat", "vendor/embed", 1'000, json{{"input", "0.0000001"}, {"ceiling_input", "0.00000015"}})});
    // The cap is below the ceiling: the cap prices the input, and the request already carries it as the pin.
    const auto capped = embeddings_call(*provider, "vendor/embed", R"({"provider":{"sort":"price","max_price":{"prompt":0.12}}})");
    ASSERT_TRUE(capped.valid());
    const auto below = Provider::endpoint_worst_case_cost(capped);
    ASSERT_TRUE(below.total) << below.unbounded.size();
    EXPECT_EQ(below.total->nano_usd, 1'000U * 120);
    EXPECT_TRUE(below.route_ceiling);
    EXPECT_TRUE(below.pinned);
    ASSERT_TRUE(below.pin);
    EXPECT_EQ(below.pin->prompt_nano_usd, 120'000'000U);
    const auto& facts = Provider::endpoint_facts(capped);
    EXPECT_TRUE(facts.routed_gateway);
    ASSERT_TRUE(facts.max_price);
    EXPECT_EQ(facts.max_price->prompt_nano_usd, 120'000'000U);
    // The cap is above the ceiling: the route ceiling prices the input, and the request must be prepared again with the pin.
    const auto loose = embeddings_call(*provider, "vendor/embed", R"({"provider":{"sort":"price","max_price":{"prompt":0.2}}})");
    const auto above = Provider::endpoint_worst_case_cost(loose);
    ASSERT_TRUE(above.total) << above.unbounded.size();
    EXPECT_EQ(above.total->nano_usd, 1'000U * 150);
    EXPECT_FALSE(above.pinned);
    ASSERT_TRUE(above.pin);
    EXPECT_EQ(above.pin->prompt_nano_usd, 150'000'000U);
}

TEST(PreparedProviderCost, AnEndpointCallTheSdkDoesNotBoundHasTypedReasonsNotAZero) {
    auto gateway = provider_with("openai.chat", kGatewayOrigin,
        {model_priced("openai.chat", "vendor/embed", 1'000, json{{"input", "0.0000001"}, {"ceiling_input", "0.00000015"}})});
    // At a routing gateway only a capped embeddings call has a bound.
    const auto uncapped = Provider::endpoint_worst_case_cost(embeddings_call(*gateway, "vendor/embed"));
    EXPECT_FALSE(uncapped.total);
    EXPECT_TRUE(has_reason(uncapped.unbounded, sp::pricing::UnboundedBy::RouteCeiling)) << uncapped.unbounded.size();
    // No catalogue row for the model.
    auto direct = provider_with("openai.chat", kDirectOrigin, {});
    const auto unpriced = Provider::endpoint_worst_case_cost(embeddings_call(*direct, "no-such-embed"));
    EXPECT_FALSE(unpriced.total);
    EXPECT_TRUE(has_reason(unpriced.unbounded, sp::pricing::UnboundedBy::ModelPrice)) << unpriced.unbounded.size();
    // The chat accessor reports the same refusal for the endpoint call, never a zero.
    const auto projected = Provider::worst_case_cost(embeddings_call(*direct, "no-such-embed"));
    EXPECT_FALSE(projected.total);
    EXPECT_FALSE(projected.unbounded.empty());
    // A chat request is not an endpoint call.
    const auto chat = Provider::endpoint_worst_case_cost(chat_call(*direct, "no-such-model"));
    EXPECT_FALSE(chat.total);
    EXPECT_TRUE(has_reason(chat.unbounded, sp::pricing::UnboundedBy::Unmodeled));
}

TEST(PreparedProviderCost, ARequestThatDidNotPrepareHasNoTotalAndNoFacts) {
    const PreparedProviderRequest none;
    const auto chat = Provider::worst_case_cost(none);
    EXPECT_FALSE(chat.total);
    EXPECT_TRUE(has_reason(chat.unbounded, sp::pricing::UnboundedBy::Unmodeled));
    const auto endpoint = Provider::endpoint_worst_case_cost(none);
    EXPECT_FALSE(endpoint.total);
    EXPECT_TRUE(has_reason(endpoint.unbounded, sp::pricing::UnboundedBy::Unmodeled));
    EXPECT_TRUE(Provider::request_facts(none).priced_extensions.empty());
    EXPECT_TRUE(Provider::endpoint_facts(none).endpoint.empty());
    // A refused preparation is the same.
    auto provider = messages_provider();
    const auto refused = messages_call(*provider, "with-fast", json{{"unadmitted_member", 1}});
    ASSERT_FALSE(refused.valid());
    EXPECT_FALSE(Provider::worst_case_cost(refused).total);
    EXPECT_TRUE(Provider::endpoint_facts(refused).endpoint.empty());
}
