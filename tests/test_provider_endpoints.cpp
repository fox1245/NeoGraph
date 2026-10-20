// Provider's SchemaProvider endpoint calls (#531): they run on the provider's own client, the descriptor policy
// admits them, and the async form neither blocks a thread nor needs the provider to outlive it.
#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <type_traits>

using namespace neograph;
namespace wire = neograph::test::wire;
using namespace std::chrono_literals;

namespace {
template <class Request> constexpr bool forwarded() {
    using Prepared = decltype(std::declval<sp::runtime::Client&>().prepare(
        std::declval<Request>(), std::declval<sp::runtime::RunOptions>()));
    return std::is_same_v<Prepared, sp::runtime::PreparedEndpoint<EndpointOutcomeOf<Request>>>;
}
// Every endpoint request names the outcome the SDK client prepares for it.
static_assert(forwarded<sp::images::Request>() && forwarded<sp::decisions::Request>() &&
              forwarded<sp::embeddings::Request>() && forwarded<sp::rerank::Request>() &&
              forwarded<sp::moderation::Request>() && forwarded<sp::speech::Request>() &&
              forwarded<sp::transcription::Request>());
static_assert(forwarded<sp::videos::Request>() && forwarded<sp::jobs::Poll>() && forwarded<sp::jobs::Download>());
static_assert(forwarded<sp::files::Upload>() && forwarded<sp::files::Get>() && forwarded<sp::files::List>() &&
              forwarded<sp::files::Delete>() && forwarded<sp::files::Download>() && forwarded<sp::files::Start>() &&
              forwarded<sp::files::Part>() && forwarded<sp::files::Complete>() && forwarded<sp::files::Query>() &&
              forwarded<sp::files::Cancel>());
static_assert(forwarded<sp::batches::Submit>() && forwarded<sp::batches::Get>() && forwarded<sp::batches::Cancel>() &&
              forwarded<sp::batches::Delete>() && forwarded<sp::batches::Results>());
static_assert(forwarded<sp::tokens::Count>() && forwarded<sp::generations::Get>());
// Chat goes through Provider::prepare.
static_assert(!EndpointRequest<sp::runtime::Request> && !EndpointRequest<sp::chat::Request>);

// The built-in policy, with every list that names the OpenAI origin also naming `origin`.
sp::descriptor::PolicySnapshot policy_at(const std::string& origin) {
    auto policy = json::parse(sp::config_defaults::descriptor_policy_json).dump();
    const std::string openai = "\"https://api.openai.com\"";
    for (auto at = policy.find(openai); at != std::string::npos; at = policy.find(openai, at + 1)) {
        const auto added = openai + ",\"" + origin + "\"";
        policy.replace(at, openai.size(), added);
        at += added.size() - 1;
    }
    auto admitted = sp::descriptor::load_policy(policy, sp::config_defaults::codec_defaults_json);
    if (const auto* error = std::get_if<sp::descriptor::ConfigError>(&admitted))
        throw std::invalid_argument(error->pointer + ": " + error->message);
    return std::get<sp::descriptor::PolicySnapshot>(std::move(admitted));
}

std::shared_ptr<llm::SchemaProvider> provider_at(const wire::Peer& peer, bool admitted = true) {
    sp::runtime::Options options;
    options.api_key = "fixture-key";
    options.default_timeout = 5s;
    options.retry_tokens = 0;
    options.retry_tokens_per_second = 0;
    auto policy = admitted ? policy_at(peer.origin()) : sp::descriptor::builtin_policy();
    return std::make_shared<llm::SchemaProvider>(
        test::descriptor("openai.chat", peer.origin(), std::move(policy)), std::move(options));
}

std::string embedding_body() {
    return json{{"object", "list"}, {"model", "fixture-embed"},
                {"data", json::array({{{"object", "embedding"}, {"index", 0}, {"embedding", {0.25, -0.5}}}})},
                {"usage", {{"prompt_tokens", 3}, {"total_tokens", 3}}}}.dump();
}

sp::embeddings::Request embed(std::string text = "hello") {
    sp::embeddings::Request request;
    request.model = "fixture-embed";
    request.inputs.push_back({std::move(text)});
    return request;
}

void expect_vector(const sp::embeddings::Outcome& outcome) {
    const auto* result = std::get_if<sp::embeddings::Result>(&outcome);
    ASSERT_NE(result, nullptr) << std::get<sp::Failure>(outcome).error.safe_message;
    ASSERT_EQ(result->vectors.size(), 1u);
    EXPECT_EQ(result->vectors[0], (std::vector<float>{0.25f, -0.5f}));
    ASSERT_TRUE(result->usage.input_total);
    EXPECT_EQ(result->usage.input_total->value, 3u);
}

class NoEndpoints final : public Provider {
public:
    std::string get_name() const override { return "no-endpoints"; }
    std::string_view family() const noexcept override { return "openai.chat"; }
    PreparedProviderRequest prepare(ProviderRequest) override {
        return reject_preparation(sp::Error{});
    }
};
}  // namespace

TEST(ProviderEndpoints, CompletesOnTheProvidersClientWithItsCredential) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    expect_vector(provider->complete_endpoint(embed()));
    std::lock_guard lock(peer.state->mutex);
    ASSERT_EQ(peer.state->paths.size(), 1u);
    EXPECT_EQ(peer.state->paths[0], "/v1/embeddings");
    EXPECT_EQ(peer.state->authorization[0], "Bearer fixture-key");
    EXPECT_EQ(peer.state->requests[0]["model"], "fixture-embed");
}

TEST(ProviderEndpoints, APreparedCallNamesItsEndpointAndModelBeforeItRuns) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto prepared = provider->prepare_endpoint(embed());
    ASSERT_TRUE(prepared.valid());
    ASSERT_EQ(prepared.error(), nullptr);
    EXPECT_EQ(prepared.prepared().endpoint(), "embeddings");
    EXPECT_EQ(prepared.prepared().model(), "fixture-embed");
    EXPECT_EQ(peer.state->entered, 0u);
    expect_vector(provider->complete_endpoint(std::move(prepared)));
}

TEST(ProviderEndpoints, ThePolicyRefusesAnEndpointItDoesNotAdmitAtThisOrigin) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer, false);
    auto prepared = provider->prepare_endpoint(embed());
    ASSERT_NE(prepared.error(), nullptr);
    const auto outcome = provider->complete_endpoint(std::move(prepared));
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(outcome));
    EXPECT_EQ(peer.state->entered, 0u);
}

TEST(ProviderEndpoints, AProviderWithoutSchemaProviderEndpointsRefusesThem) {
    NoEndpoints provider;
    EXPECT_THROW(provider.prepare_endpoint(embed()), std::logic_error);
    EXPECT_THROW(provider.complete_endpoint(embed()), std::logic_error);
    EXPECT_THROW((void)provider.complete_endpoint_async(embed()), std::logic_error);
}

TEST(ProviderEndpoints, AsyncCompletionOwnsItsClient) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    auto awaitable = provider->complete_endpoint_async(embed());
    provider.reset();  // the awaitable holds the client and the prepared call
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(awaitable), asio::use_future);
    io.run();
    ASSERT_EQ(future.wait_for(0s), std::future_status::ready);
    expect_vector(future.get());
}

TEST(ProviderEndpoints, AsyncCompletionOfAPreparedCall) {
    wire::Peer peer(embedding_body());
    auto provider = provider_at(peer);
    asio::io_context io;
    auto future = asio::co_spawn(io, provider->complete_endpoint_async(provider->prepare_endpoint(embed())),
                                 asio::use_future);
    io.run();
    expect_vector(future.get());
}

TEST(ProviderEndpoints, CancellingTheAwaitingCoroutineCancelsTheCall) {
    wire::Peer peer(embedding_body());
    peer.state->hold = true;
    auto provider = provider_at(peer);
    asio::io_context io;
    asio::cancellation_signal cancellation;
    auto future = asio::co_spawn(io, provider->complete_endpoint_async(embed()),
                                 asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
    std::thread runner([&io] { io.run(); });
    const bool entered = peer.await_requests(1);
    asio::post(io, [&] { cancellation.emit(asio::cancellation_type::terminal); });
    const auto status = future.wait_for(2s);
    peer.release();
    runner.join();
    ASSERT_TRUE(entered);
    ASSERT_EQ(status, std::future_status::ready);
    const auto outcome = future.get();
    ASSERT_TRUE(std::holds_alternative<sp::Failure>(outcome));
    EXPECT_EQ(std::get<sp::Failure>(outcome).error.kind, sp::ErrorKind::Cancelled);
}

TEST(ProviderEndpoints, ImagesComeBackAsMedia) {
    const std::string png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==";
    wire::Peer peer(json{{"created", 1}, {"data", json::array({{{"b64_json", png}}})},
                         {"usage", {{"input_tokens", 5}, {"output_tokens", 7}, {"total_tokens", 12}}}}.dump());
    auto provider = provider_at(peer);
    sp::images::Request request;
    request.model = "fixture-image";
    request.prompt = "a square";
    request.output_format = "png";
    const auto outcome = provider->complete_endpoint(std::move(request));
    const auto* result = std::get_if<sp::images::Result>(&outcome);
    ASSERT_NE(result, nullptr) << std::get<sp::Failure>(outcome).error.safe_message;
    ASSERT_EQ(result->images.size(), 1u);
    EXPECT_EQ(result->images[0].kind, sp::MediaKind::Image);
    std::lock_guard lock(peer.state->mutex);
    EXPECT_EQ(peer.state->paths[0], "/v1/images/generations");
    EXPECT_EQ(peer.state->requests[0]["prompt"], "a square");
}
