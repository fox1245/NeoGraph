#include <neograph/llm/images_client.h>
#include <neograph/async/http_client.h>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <future>
#include <stdexcept>

namespace {
using Client = neograph::llm::ImagesClient;
using Failure = neograph::llm::EndpointFailure;
using Kind = neograph::llm::EndpointFailureKind;
using Artifact = neograph::llm::EndpointArtifact;
using neograph::json;
using namespace std::chrono_literals;

std::string port() {
    const auto* value = std::getenv("NEOGRAPH_IMAGES_TEST_PORT");
    if (!value) throw std::runtime_error("Run through tests/typed_images_peer.py");
    return value;
}
json configuration(const std::string& profile = "nano_banana_2_lite") {
    const bool google = profile == "nano_banana_2_lite";
    const std::string model = google ? "gemini-3.1-flash-lite-image" : profile == "openai_gpt_image" ? "gpt-image-1-mini" : profile == "openai_dall_e_2" ? "dall-e-2" : "dall-e-3";
    return json{{"version", 1}, {"provider", google ? "gemini" : "openai"}, {"model_profile", profile}, {"model", model},
        {"endpoint", json{{"host", "localhost"}, {"port", port()}, {"path", google ? "/v1beta/models/" + model + ":generateContent" : "/v1/images/generations"}}},
        {"limits", json{{"timeout_ms", 5000}, {"max_prompt_bytes", 32000}, {"max_response_header_bytes", 65536}, {"max_response_body_bytes", 1048576}, {"max_response_chunk_bytes", 1048576}, {"max_artifact_bytes", 65536}, {"max_outputs", 16}}},
        {"defaults", google ? json{{"aspect_ratio", "1:1"}, {"image_size", "1K"}, {"thinking", "minimal"}, {"max_output_tokens", 2048}} :
            json{{"size", "1024x1024"}, {"quality", profile == "openai_gpt_image" ? "low" : "standard"}, {"n", 1}, {"response_format", profile == "openai_gpt_image" ? "native" : "b64_json"}}}};
}
Client client(json config) {
    auto admitted = Client::Config::from_json(config);
    if (auto* error = std::get_if<Failure>(&admitted)) throw std::runtime_error(error->safe_message);
    return Client(std::get<Client::Config>(std::move(admitted)), "credential-private");
}
template<class T> T run(asio::awaitable<T> operation) {
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return future.get();
}
std::uint64_t dispatches() {
    auto operation = []() -> asio::awaitable<std::uint64_t> {
        auto ex = co_await asio::this_coro::executor;
        auto response = co_await neograph::async::async_get(ex, "localhost", port(), "/state", {}, true);
        co_return json::parse(response.body)["requests"].get<unsigned long long>();
    };
    return run(operation());
}
void expect_failure(const Client::Result& result, Kind kind, bool sent) {
    ASSERT_TRUE(std::holds_alternative<Failure>(result));
    const auto& error = std::get<Failure>(result);
    EXPECT_EQ(error.kind, kind);
    EXPECT_EQ(error.request_may_have_left, sent);
    EXPECT_EQ(error.safe_message.find("credential-private"), std::string::npos);
    EXPECT_EQ(error.safe_message.find("body-private"), std::string::npos);
    EXPECT_EQ(error.safe_message.find("token="), std::string::npos);
    EXPECT_TRUE(error.provider_code.empty());
}

TEST(TypedImagesClient, GeminiInterleavedOrderOwnedBinaryAndMetadata) {
    auto make_operation = [] {
        auto config = configuration();
        auto image_client = client(config);
        auto request = image_client.request("ordered");
        auto operation = image_client.async_generate(request);
        // Config, client, credentials and original request expire before execution.
        request.prompt = "malformed";
        config["model"] = "must-not-affect-admitted-client";
        return operation;
    };
    auto result = run(make_operation());
    ASSERT_TRUE(std::holds_alternative<Client::Generation>(result));
    const auto& generated = std::get<Client::Generation>(result);
    ASSERT_EQ(generated.outputs.size(), 3u);
    EXPECT_EQ(std::get<Client::TextOutput>(generated.outputs[0]).text, "before");
    EXPECT_EQ(std::get<Client::TextOutput>(generated.outputs[2]).text, "after");
    const auto& image = std::get<Artifact>(generated.outputs[1]);
    EXPECT_EQ(image.mime_type, "image/png");
    ASSERT_TRUE(image.bytes);
    ASSERT_EQ(image.bytes->size(), 68u);
    EXPECT_EQ(std::to_integer<unsigned char>((*image.bytes)[0]), 0x89u);
    EXPECT_EQ(std::to_integer<unsigned char>(image.bytes->back()), 0x82u);
    EXPECT_TRUE(image.uri.empty());
    ASSERT_TRUE(image.metadata);
    EXPECT_TRUE(image.metadata->is_object());
    EXPECT_FALSE(generated.usage.has_value());
    EXPECT_EQ(generated.response_id, "fixture-image-1");
}

TEST(TypedImagesClient, OpenAIOrderedArtifactsPreserveRevisionAndUri) {
    auto image_client = client(configuration("openai_dall_e_2"));
    auto request = image_client.request("ordered");
    auto& controls = std::get<Client::OpenAIControls>(request.controls);
    controls.n = 2;
    auto binary_result = run(image_client.async_generate(request));
    ASSERT_TRUE(std::holds_alternative<Client::Generation>(binary_result));
    const auto& binary = std::get<Client::Generation>(binary_result);
    ASSERT_EQ(binary.outputs.size(), 2u);
    EXPECT_EQ(binary.created, 42u);
    const auto& first = std::get<Artifact>(binary.outputs[0]);
    const auto& second = std::get<Artifact>(binary.outputs[1]);
    EXPECT_EQ((*first.metadata)["revised_prompt"].get<std::string>(), "revision-0");
    EXPECT_EQ((*second.metadata)["revised_prompt"].get<std::string>(), "revision-1");
    ASSERT_TRUE(first.bytes && second.bytes);
    EXPECT_EQ(*first.bytes, *second.bytes);
    controls.response_format = Client::ResponseFormat::Url;
    auto uri_result = run(image_client.async_generate(std::move(request)));
    ASSERT_TRUE(std::holds_alternative<Client::Generation>(uri_result));
    const auto& uri = std::get<Artifact>(std::get<Client::Generation>(uri_result).outputs[1]);
    EXPECT_EQ(uri.uri, "https://images.example/image-1?token=owned-private");
    EXPECT_FALSE(uri.bytes);
    EXPECT_TRUE(uri.mime_type.empty()); // A URL alone does not prove a MIME type.
}

TEST(TypedImagesClient, UnknownUsageIsPreservedNotInvented) {
    for (const auto* profile : {"nano_banana_2_lite", "openai_gpt_image"}) {
        auto image_client = client(configuration(profile));
        auto result = run(image_client.async_generate(image_client.request("usage")));
        ASSERT_TRUE(std::holds_alternative<Client::Generation>(result));
        const auto& generated = std::get<Client::Generation>(result);
        ASSERT_TRUE(generated.usage);
        const auto& usage = *generated.usage;
        EXPECT_EQ(usage.input_tokens, std::string(profile) == "nano_banana_2_lite" ? 9u : 7u);
        EXPECT_FALSE(usage.output_tokens);
        EXPECT_FALSE(usage.cached_input_tokens);
        ASSERT_TRUE(usage.reported);
        EXPECT_TRUE(usage.reported->contains(std::string(profile) == "nano_banana_2_lite" ? "imageNovelMetric" : "unknown_tokens"));
    }
}

TEST(TypedImagesClient, MalformedBinaryMimeMissingImageAndResponseFailClosed) {
    for (const auto* prompt : {"bad_base64", "noncanonical_base64", "truncated_png", "bad_mime", "malformed", "no_image", "bad_usage"}) {
        auto image_client = client(configuration());
        expect_failure(run(image_client.async_generate(image_client.request(prompt))), Kind::Protocol, true);
    }
    auto image_client = client(configuration("openai_gpt_image"));
    expect_failure(run(image_client.async_generate(image_client.request("bad_mime"))), Kind::Protocol, true);
}

TEST(TypedImagesClient, ProviderErrorBlockedAndRedirectHaveNoRetryOrSecretEcho) {
    auto image_client = client(configuration());
    for (const auto* prompt : {"provider_error", "blocked", "redirect"}) {
        const auto before = dispatches();
        auto result = run(image_client.async_generate(image_client.request(prompt)));
        expect_failure(result, Kind::Provider, true);
        EXPECT_EQ(dispatches(), before + 1);
        if (std::string(prompt) == "provider_error") EXPECT_EQ(std::get<Failure>(result).http_status, 429);
        if (std::string(prompt) == "redirect") EXPECT_EQ(std::get<Failure>(result).http_status, 307);
    }
}

TEST(TypedImagesClient, IncompatibleControlsAndAdmissionCauseZeroDispatch) {
    const auto before = dispatches();
    auto image_client = client(configuration());
    auto request = image_client.request("ordered");
    request.controls = Client::OpenAIControls{"1024x1024", "low", 1, Client::ResponseFormat::Native};
    expect_failure(run(image_client.async_generate(request)), Kind::InvalidRequest, false);
    request = image_client.request("ordered");
    std::get<Client::GeminiControls>(request.controls).image_size = "2K";
    expect_failure(run(image_client.async_generate(request)), Kind::InvalidRequest, false);
    std::get<Client::GeminiControls>(request.controls).image_size = "1K";
    std::get<Client::GeminiControls>(request.controls).max_output_tokens = 100;
    expect_failure(run(image_client.async_generate(request)), Kind::InvalidRequest, false);
    auto gpt = client(configuration("openai_gpt_image"));
    auto gpt_request = gpt.request("ordered");
    std::get<Client::OpenAIControls>(gpt_request.controls).response_format = Client::ResponseFormat::Url;
    expect_failure(run(gpt.async_generate(gpt_request)), Kind::InvalidRequest, false);
    auto dalle = client(configuration("openai_dall_e_3"));
    auto dalle_request = dalle.request("ordered");
    std::get<Client::OpenAIControls>(dalle_request.controls).n = 2;
    expect_failure(run(dalle.async_generate(dalle_request)), Kind::InvalidRequest, false);
    auto config = configuration();
    config["api_key"] = "must-not-be-admitted";
    auto admitted = Client::Config::from_json(config);
    ASSERT_TRUE(std::holds_alternative<Failure>(admitted));
    EXPECT_EQ(std::get<Failure>(admitted).kind, Kind::InvalidConfig);
    EXPECT_EQ(dispatches(), before);
}

TEST(TypedImagesClient, CancelBeforeDispatchAndExpiredDeadlineAreUnbilled) {
    const auto before = dispatches();
    auto image_client = client(configuration());
    auto request = image_client.request("ordered");
    request.cancel_token = std::make_shared<neograph::graph::CancelToken>();
    request.cancel_token->cancel();
    expect_failure(run(image_client.async_generate(request)), Kind::Cancelled, false);
    request = image_client.request("ordered");
    request.deadline = std::chrono::steady_clock::now() - 1ms;
    expect_failure(run(image_client.async_generate(request)), Kind::DeadlineExceeded, false);
    EXPECT_EQ(dispatches(), before);
}

TEST(TypedImagesClient, InFlightCancellationAbortsAndDoesNotReplay) {
    auto image_client = client(configuration());
    auto request = image_client.request("slow");
    request.cancel_token = std::make_shared<neograph::graph::CancelToken>();
    asio::io_context io;
    auto future = asio::co_spawn(io, image_client.async_generate(request), asio::use_future);
    asio::steady_timer timer(io, 80ms);
    timer.async_wait([token = request.cancel_token](const asio::error_code& error) { if (!error) token->cancel(); });
    const auto before = dispatches();
    const auto start = std::chrono::steady_clock::now();
    io.run();
    expect_failure(future.get(), Kind::Cancelled, true);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 400ms);
    EXPECT_EQ(dispatches(), before + 1);
}

TEST(TypedImagesClient, AbsoluteDeadlineAndArtifactBoundAreEnforced) {
    auto image_client = client(configuration());
    auto request = image_client.request("slow");
    request.deadline = std::chrono::steady_clock::now() + 80ms;
    const auto before = dispatches();
    expect_failure(run(image_client.async_generate(request)), Kind::DeadlineExceeded, true);
    EXPECT_EQ(dispatches(), before + 1);
    auto config = configuration();
    config["limits"]["max_artifact_bytes"] = 16;
    auto bounded = client(config);
    expect_failure(run(bounded.async_generate(bounded.request("ordered"))), Kind::ResourceLimit, true);
}
} // namespace
