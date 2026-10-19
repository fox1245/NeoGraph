// Live end-to-end checks of the C++ provider adapter and the typed endpoint clients (tests/live/README.md).
//
// Nothing runs unless NEOGRAPH_E2E_LIVE=1 and the provider's key variable are set; each case skips otherwise.
// Keys are read from the environment only and never printed. scripts/live_e2e.py runs this binary for the
// release report; it is built with the tests but not registered with CTest.
#include <neograph/llm/decisions_client.h>
#include <neograph/llm/images_client.h>
#include <neograph/llm/schema_provider.h>
#include <neograph/llm/veo_client.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {
using neograph::json;
using namespace neograph::llm;

std::string live_key(const char* variable) {
    const char* live = std::getenv("NEOGRAPH_E2E_LIVE");
    const char* value = std::getenv(variable);
    if (!live || std::string(live) != "1" || !value || !*value) return {};
    return value;
}
#define LIVE_KEY(name, variable)                                                       \
    const auto name = live_key(variable);                                              \
    if (name.empty()) GTEST_SKIP() << "live: set NEOGRAPH_E2E_LIVE=1 and " variable

json shipped(const char* name) {
    std::ifstream file(std::string(NEOGRAPH_SHIPPED_SCHEMAS) + "/" + name);
    std::stringstream text;
    text << file.rdbuf();
    return json::parse(text.str());
}

template <class T> T run(asio::awaitable<T> operation) {
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return future.get();
}

sp::descriptor::ValidatedDescriptor descriptor(const std::string& family, const std::string& origin,
                                               const std::string& buffered, const std::string& streaming) {
    const json document = {{"descriptor_version", 1}, {"revision", 1}, {"id", "live-cpp"}, {"family", family},
        {"connection", {{"base_url", origin}, {"paths", {{"buffered", buffered}, {"streaming", streaming}}}}}};
    auto loaded = sp::descriptor::load(document.dump());
    return std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded));
}

template <class Request>
const sp::Completion& invoke(SchemaProvider& provider, Request request, bool stream, sp::runtime::Result& keep) {
    neograph::ProviderRequest call;
    call.payload = std::move(request);
    call.mode = stream ? neograph::ProviderMode::Stream : neograph::ProviderMode::Collect;
    call.options.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(4);
    if (stream) call.on_event = [](const sp::Event&) {};
    keep = provider.invoke(std::move(call));
    if (const auto* failure = std::get_if<sp::Failure>(keep.get()))
        ADD_FAILURE() << "provider failure kind " << static_cast<int>(failure->error.kind) << " http "
                      << failure->error.http_status << ": " << failure->error.safe_message;
    static const sp::Completion empty{};
    const auto* completion = std::get_if<sp::Completion>(keep.get());
    return completion ? *completion : empty;
}

std::size_t text_size(const sp::Completion& completion) {
    std::size_t size = 0;
    for (const auto& message : completion.messages)
        for (const auto& part : message.parts)
            if (const auto* text = std::get_if<sp::Text>(&part)) size += text->value.size();
    return size;
}

class LiveAdapter : public ::testing::TestWithParam<bool> {};
INSTANTIATE_TEST_SUITE_P(Mode, LiveAdapter, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "Stream" : "Collect"; });

// Claude 5.5 answers web_fetch_20260318 with a code-execution container and cache diagnostics.
TEST_P(LiveAdapter, ClaudeWebFetch20260318Completes) {
    LIVE_KEY(key, "ANTHROPIC_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("anthropic.messages", "https://api.anthropic.com", "/v1/messages",
                                       "/v1/messages"), options);
    sp::messages::Request request;
    request.model = "claude-haiku-5-5";
    request.max_tokens = 600;
    request.messages.push_back(sp::Message{"", sp::Role::User,
        {sp::Text{"Fetch https://example.com and tell me its page title in five words."}}});
    sp::messages::ToolDefinition tool;
    tool.type = "web_fetch_20260318";
    tool.name = "web_fetch";
    tool.max_uses = 1;
    request.tools.push_back(tool);
    sp::runtime::Result result;
    EXPECT_GT(text_size(invoke(provider, std::move(request), GetParam(), result)), 0u);
}

TEST_P(LiveAdapter, OpenRouterChatKeepsGeneratedImages) {
    LIVE_KEY(key, "OPENROUTER_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("openai.chat", "https://openrouter.ai", "/api/v1/chat/completions",
                                       "/api/v1/chat/completions"), options);
    sp::chat::Request request;
    request.model = "google/gemini-3.1-flash-lite-image";
    request.messages.push_back(sp::chat::InputMessage{sp::Role::User, "A tiny flat red circle icon on white."});
    sp::runtime::Result result;
    const auto& completion = invoke(provider, std::move(request), GetParam(), result);
    std::size_t images = 0;
    for (const auto& message : completion.messages)
        for (const auto& part : message.parts)
            if (const auto* media = std::get_if<sp::Media>(&part); media && media->kind == sp::MediaKind::Image) {
                ++images;
                EXPECT_EQ(media->mime.rfind("image/", 0), 0u);
                EXPECT_TRUE(media->data && !media->data->empty());
            }
    EXPECT_GT(images, 0u);
}

TEST_P(LiveAdapter, InteractionsText) {
    LIVE_KEY(key, "GEMINI_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("google.interactions", "https://generativelanguage.googleapis.com",
                                       "/v1beta/interactions", "/v1beta/interactions"), options);
    sp::interactions::Request request;
    request.model = "gemini-3.1-flash-lite";
    request.max_output_tokens = 32;
    request.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{"Reply with one short word."}}});
    sp::runtime::Result result;
    EXPECT_GT(text_size(invoke(provider, std::move(request), GetParam(), result)), 0u);
}

// A detailed prompt makes a streamed image of more than 1 MiB of base64, above the old SSE event limit.
TEST_P(LiveAdapter, GeminiGeneratedImage) {
    LIVE_KEY(key, "GEMINI_API_KEY");
    const std::string model = "gemini-3.1-flash-lite-image";
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("google.generate", "https://generativelanguage.googleapis.com",
        "/v1beta/models/" + model + ":generateContent",
        "/v1beta/models/" + model + ":streamGenerateContent?alt=sse"), options);
    sp::gemini::Request request;
    request.model = model;
    request.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{
        "A highly detailed photorealistic market street at dusk, crowded stalls, lanterns, fruit, fabric, "
        "reflections on wet cobblestones, fine textures everywhere."}}});
    sp::runtime::Result result;
    const auto& completion = invoke(provider, std::move(request), GetParam(), result);
    std::size_t encoded = 0;
    for (const auto& message : completion.messages)
        for (const auto& part : message.parts)
            if (const auto* media = std::get_if<sp::Media>(&part); media && media->data)
                encoded += media->data->size();
    EXPECT_GT(encoded, 10000u);
}

// Every model the shipped Decisions configuration lists answers through the client.
TEST(LiveTypedClients, DecisionsShippedCatalogue) {
    LIVE_KEY(key, "OPENROUTER_API_KEY");
    const auto document = shipped("typed-decisions-client.json");
    auto admitted = DecisionsConfig::from_json(document);
    ASSERT_TRUE(std::holds_alternative<DecisionsConfig>(admitted));
    DecisionsClient client(std::get<DecisionsConfig>(std::move(admitted)), key);
    for (const auto& entry : document.at("models")) {
        const auto id = entry.at("id").get<std::string>();
        auto answered = false;
        // Some models accept noul questions only and say so in a 400; they are asked that alone.
        for (const bool all_kinds : {true, false}) {
            DecisionsRequest request;
            request.model = id;
            request.state = "Ticket: the login page returns HTTP 500 after the 2.3 deploy; users cannot sign in.";
            request.questions["is_bug"] = DecisionsNoulQuestion{json("Is this a software defect report?"), std::nullopt};
            if (all_kinds) {
                request.questions["area"] = DecisionsChoiceQuestion{json("Which area is affected?"),
                    {{"auth", "Login or sessions"}, {"billing", "Payments"}}};
                request.questions["severity"] = DecisionsScoreQuestion{json("How severe is it?"),
                    {"minor", "major", "critical"}};
            }
            auto result = run(client.submit_async(std::move(request)));
            if (const auto* response = std::get_if<DecisionsResponse>(&result)) {
                EXPECT_EQ(response->answers.size(), all_kinds ? 3u : 1u) << id;
                answered = true;
                break;
            }
            const auto& failure = std::get<EndpointFailure>(result);
            if (!all_kinds || failure.http_status != 400)
                ADD_FAILURE() << id << ": " << failure.safe_message << " http " << failure.http_status;
            if (failure.http_status != 400) break;
        }
        EXPECT_TRUE(answered) << id;
    }
}

void expect_image(ImagesClient::Config config, const std::string& key) {
    ImagesClient client(std::move(config), key);
    auto result = run(client.async_generate(client.request("A tiny flat red circle icon on white.")));
    if (const auto* failure = std::get_if<EndpointFailure>(&result)) {
        ADD_FAILURE() << failure->safe_message << " http " << failure->http_status;
        return;
    }
    const auto& generation = std::get<ImagesClient::Generation>(result);
    std::size_t bytes = 0;
    for (const auto& output : generation.outputs)
        if (const auto* artifact = std::get_if<EndpointArtifact>(&output); artifact && artifact->bytes)
            bytes += artifact->bytes->size();
    EXPECT_GT(bytes, 1000u);
}

TEST(LiveTypedClients, ImagesShippedGeminiConfiguration) {
    LIVE_KEY(key, "GEMINI_API_KEY");
    auto admitted = ImagesClient::Config::from_json(shipped("typed-images-client.json"));
    ASSERT_TRUE(std::holds_alternative<ImagesClient::Config>(admitted));
    expect_image(std::get<ImagesClient::Config>(std::move(admitted)), key);
}

TEST(LiveTypedClients, ImagesGptImage2) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    const json document = {{"version", 1}, {"provider", "openai"}, {"model_profile", "openai_gpt_image"},
        {"model", "gpt-image-2"},
        {"endpoint", {{"host", "api.openai.com"}, {"port", "443"}, {"path", "/v1/images/generations"}}},
        {"limits", {{"timeout_ms", 180000}, {"max_prompt_bytes", 4096}, {"max_response_header_bytes", 65536},
                    {"max_response_body_bytes", 67108864}, {"max_response_chunk_bytes", 67108864},
                    {"max_artifact_bytes", 67108864}, {"max_outputs", 1}}},
        {"defaults", {{"size", "1024x1024"}, {"quality", "low"}, {"n", 1}, {"response_format", "native"}}}};
    auto admitted = ImagesClient::Config::from_json(document);
    ASSERT_TRUE(std::holds_alternative<ImagesClient::Config>(admitted));
    expect_image(std::get<ImagesClient::Config>(std::move(admitted)), key);
}

TEST(LiveTypedClients, VeoShippedConfiguration) {
    LIVE_KEY(key, "GEMINI_API_KEY");
    auto document = shipped("typed-veo-client.json");
    document["limits"]["deadline_ms"] = 900000;
    document["limits"]["max_status_queries"] = 120;
    VeoClient client(document, key);
    auto work = [&]() -> asio::awaitable<std::size_t> {
        VeoRequest request;
        request.prompt = "A small blue paper kite drifting in a clear sky.";
        auto submitted = co_await client.submit(co_await asio::this_coro::executor, request);
        if (submitted.failure) throw std::runtime_error("submit: " + submitted.failure->safe_message);
        auto waited = co_await client.wait(*submitted.operation);
        if (waited.failure) throw std::runtime_error("wait: " + waited.failure->safe_message);
        auto downloaded = co_await client.download(*submitted.operation, 0);
        if (downloaded.failure) throw std::runtime_error("download: " + downloaded.failure->safe_message);
        EXPECT_EQ(downloaded.artifact->mime_type, "video/mp4");
        co_return downloaded.artifact->bytes->size();
    };
    EXPECT_GT(run(work()), 100000u);
}
} // namespace
