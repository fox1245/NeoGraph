// Live end-to-end checks of the C++ provider adapter (tests/live/README.md).
//
// Nothing runs unless NEOGRAPH_E2E_LIVE=1 and the provider's key variable are set; each case skips otherwise.
// Keys are read from the environment only and never printed. scripts/live_e2e.py runs this binary for the
// release report; it is built with the tests but not registered with CTest.
#include <neograph/llm/schema_provider.h>
#include <neograph/graph/engine.h>

#include <descriptor/catalogue.h>
#include <descriptor/presets.h>
#include <transport/catalogue_refresh.h>
#include <transport/realtime_session.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>

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

template <class T> T run(asio::awaitable<T> operation) {
    asio::io_context io;
    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    return future.get();
}

sp::descriptor::ValidatedDescriptor descriptor(const std::string& family, const std::string& origin,
                                               const std::string& buffered, const std::string& streaming,
                                               sp::descriptor::PolicySnapshot policy = sp::descriptor::builtin_policy()) {
    const json document = {{"descriptor_version", 1}, {"revision", 1}, {"id", "live-cpp"}, {"family", family},
        {"connection", {{"base_url", origin}, {"paths", {{"buffered", buffered}, {"streaming", streaming}}}}}};
    auto loaded = sp::descriptor::load(document.dump(), std::move(policy));
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

// SchemaProvider endpoints through Provider (#531), on the provider's own client.
template <class Outcome> std::string failure_text(const Outcome& outcome) {
    const auto* failure = std::get_if<sp::Failure>(&outcome);
    if (!failure) return {};
    return "provider failure kind " + std::to_string(static_cast<int>(failure->error.kind)) + " http " +
           std::to_string(failure->error.http_status) + ": " + failure->error.safe_message;
}

sp::embeddings::Request embedding_request(std::string model) {
    sp::embeddings::Request request;
    request.model = std::move(model);
    request.inputs.push_back({"A graph of agents."});
    request.inputs.push_back({"A list of chores."});
    return request;
}

void expect_two_vectors(const sp::embeddings::Outcome& outcome) {
    const auto* result = std::get_if<sp::embeddings::Result>(&outcome);
    ASSERT_NE(result, nullptr) << failure_text(outcome);
    ASSERT_EQ(result->vectors.size(), 2u);
    EXPECT_GT(result->vectors[0].size(), 100u);
    EXPECT_EQ(result->vectors[0].size(), result->vectors[1].size());
    ASSERT_TRUE(result->usage.input_total);
    EXPECT_GT(result->usage.input_total->value, 0u);
}

TEST(LiveEndpoints, OpenAIEmbeddings) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("openai.chat", "https://api.openai.com", "/v1/chat/completions",
                                       "/v1/chat/completions"), options);
    expect_two_vectors(provider.complete_endpoint(embedding_request("text-embedding-3-small")));
}

TEST(LiveEndpoints, OpenRouterEmbeddingsAsync) {
    LIVE_KEY(key, "OPENROUTER_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    auto provider = std::make_shared<SchemaProvider>(descriptor("openai.chat", "https://openrouter.ai",
        "/api/v1/chat/completions", "/api/v1/chat/completions"), options);
    auto awaitable = provider->complete_endpoint_async(embedding_request("openai/text-embedding-3-small"));
    provider.reset();
    expect_two_vectors(run(std::move(awaitable)));
}

TEST(LiveEndpoints, OpenAIModeration) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("openai.chat", "https://api.openai.com", "/v1/chat/completions",
                                       "/v1/chat/completions"), options);
    sp::moderation::Request request;
    request.model = "omni-moderation-latest";
    request.inputs.push_back({"I would like a cup of tea."});
    auto prepared = provider.prepare_endpoint(std::move(request));
    ASSERT_EQ(prepared.error(), nullptr);
    EXPECT_EQ(prepared.prepared().endpoint(), "moderation");
    const auto outcome = provider.complete_endpoint(std::move(prepared));
    const auto* result = std::get_if<sp::moderation::Result>(&outcome);
    ASSERT_NE(result, nullptr) << failure_text(outcome);
    ASSERT_EQ(result->verdicts.size(), 1u);
    EXPECT_FALSE(result->verdicts[0].flagged);
    EXPECT_FALSE(result->verdicts[0].categories.empty());
}

// The endpoint nodes in a compiled graph: one embedding, then one moderation of the same text.
TEST(LiveEndpoints, EndpointNodesInAGraph) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    neograph::graph::NodeContext context;
    context.provider = std::make_shared<SchemaProvider>(descriptor("openai.chat", "https://api.openai.com",
        "/v1/chat/completions", "/v1/chat/completions"), options);
    const json graph = {{"name", "live-endpoint-nodes"},
        {"channels", {{"input", {{"reducer", "overwrite"}}}, {"embeddings", {{"reducer", "overwrite"}}},
                      {"moderation", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"embed", {{"type", "embed"}, {"model", "text-embedding-3-small"}}},
                   {"moderate", {{"type", "moderate"}, {"model", "omni-moderation-latest"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "embed"}}, json{{"from", "embed"}, {"to", "moderate"}},
                               json{{"from", "moderate"}, {"to", "__end__"}}})}};
    auto engine = neograph::graph::GraphEngine::compile(graph, context);
    neograph::graph::RunConfig config;
    config.thread_id = "live-endpoint-nodes";
    config.input = {{"input", "I would like a cup of tea."}};
    const auto result = engine->run(config);
    const auto embeddings = result.output["channels"]["embeddings"]["value"];
    ASSERT_EQ(embeddings.size(), 1u);
    EXPECT_GT(embeddings[0].size(), 100u);
    const auto moderation = result.output["channels"]["moderation"]["value"];
    ASSERT_EQ(moderation.size(), 1u);
    EXPECT_EQ(moderation[0]["flagged"], false);
    ASSERT_TRUE(result.usage.input_total);
    EXPECT_GT(result.usage.input_total->value, 0u);
}

TEST(LiveEndpoints, AnthropicTokenCount) {
    LIVE_KEY(key, "ANTHROPIC_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("anthropic.messages", "https://api.anthropic.com", "/v1/messages",
                                       "/v1/messages"), options);
    sp::messages::Request request;
    request.model = "claude-haiku-5-5";
    request.max_tokens = 16;
    request.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{"Count the tokens of this sentence."}}});
    const auto outcome = run(provider.complete_endpoint_async(sp::tokens::Count{std::move(request)}));
    const auto* result = std::get_if<sp::tokens::Result>(&outcome);
    ASSERT_NE(result, nullptr) << failure_text(outcome);
    EXPECT_GT(result->input, 0u);
}
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string completion_text(const sp::Completion& completion) {
    std::string text;
    for (const auto& message : completion.messages)
        for (const auto& part : message.parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
    return text;
}

SchemaProvider openai_provider(const std::string& key) {
    sp::runtime::Options options;
    options.api_key = key;
    return SchemaProvider(descriptor("openai.chat", "https://api.openai.com", "/v1/chat/completions",
                                     "/v1/chat/completions"), options);
}

TEST(LiveEndpoints, OpenAIImageGeneration) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    auto provider = openai_provider(key);
    sp::images::Request request;
    request.model = "gpt-image-1-mini";
    request.prompt = "A flat red circle on a white background.";
    request.size = "1024x1024";
    request.quality = "low";
    const auto outcome = provider.complete_endpoint(std::move(request));
    const auto* result = std::get_if<sp::images::Result>(&outcome);
    ASSERT_NE(result, nullptr) << failure_text(outcome);
    ASSERT_EQ(result->images.size(), 1u);
    EXPECT_EQ(result->images[0].mime.rfind("image/", 0), 0u);
    ASSERT_TRUE(result->images[0].data);
    EXPECT_GT(result->images[0].data->size(), 1000u);
}

// The speech model of the live tests. The speech endpoint is under test here, not the model, so this is the
// model that answers promptly: in October 2026 gpt-4o-mini-tts took up to a minute to answer speech requests
// (mp3, pcm and wav alike, with or without NeoGraph), past the 30 s these tests allow, while tts-1 answered
// every format in a few seconds.
constexpr const char* kSpeechModel = "tts-1";

// Speech made by one endpoint is transcribed by another, and the words survive the round trip.
TEST(LiveEndpoints, OpenAISpeechThenTranscription) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    auto provider = openai_provider(key);
    sp::speech::Request speech;
    speech.model = kSpeechModel;
    speech.input = "The quick brown fox jumps over the lazy dog.";
    speech.voice = "alloy";
    speech.format = "mp3";
    const auto spoken = provider.complete_endpoint(std::move(speech));
    const auto* audio = std::get_if<sp::speech::Result>(&spoken);
    ASSERT_NE(audio, nullptr) << failure_text(spoken);
    EXPECT_EQ(audio->audio.mime.rfind("audio/", 0), 0u);
    ASSERT_TRUE(audio->audio.data);
    EXPECT_GT(audio->audio.data->size(), 1000u);

    sp::transcription::Request transcription;
    transcription.model = "gpt-4o-mini-transcribe";
    transcription.audio = audio->audio;
    transcription.language = "en";
    const auto heard = provider.complete_endpoint(std::move(transcription));
    const auto* text = std::get_if<sp::transcription::Result>(&heard);
    ASSERT_NE(text, nullptr) << failure_text(heard);
    EXPECT_NE(lower(text->text).find("fox"), std::string::npos) << text->text;
    EXPECT_NE(lower(text->text).find("dog"), std::string::npos) << text->text;
}

// A file uploaded to Gemini is named by a generateContent request, read back, and deleted.
TEST(LiveEndpoints, GeminiFileInAGenerateRequest) {
    LIVE_KEY(key, "GEMINI_API_KEY");
    const std::string origin = "https://generativelanguage.googleapis.com";
    const std::string model = "gemini-3.1-flash-lite";
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("google.generate", origin, "/v1beta/models/" + model + ":generateContent",
        "/v1beta/models/" + model + ":streamGenerateContent?alt=sse"), options);
    sp::files::Upload upload;
    upload.source.bytes = std::make_shared<const std::string>("Inventory note. The code word is tangerine.\n");
    upload.filename = "note.txt";
    upload.mime = "text/plain";
    auto uploaded = provider.complete_endpoint(std::move(upload));
    auto* file = std::get_if<sp::files::File>(&uploaded);
    ASSERT_NE(file, nullptr) << failure_text(uploaded);
    for (int i = 0; i != 20 && file->state == sp::files::State::Processing; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        uploaded = provider.complete_endpoint(sp::files::Get{file->id});
        file = std::get_if<sp::files::File>(&uploaded);
        ASSERT_NE(file, nullptr) << failure_text(uploaded);
    }
    ASSERT_EQ(file->state, sp::files::State::Active);
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto reference = sp::files::media(*file, origin, now);
    ASSERT_TRUE(std::holds_alternative<sp::Media>(reference)) << std::get<sp::Error>(reference).safe_message;

    sp::gemini::Request request;
    request.model = model;
    request.max_output_tokens = 32;
    request.messages.push_back(sp::Message{"", sp::Role::User, {std::get<sp::Media>(std::move(reference)),
        sp::Text{"What is the code word in the note? Reply with the word only."}}});
    sp::runtime::Result result;
    EXPECT_NE(lower(completion_text(invoke(provider, std::move(request), false, result))).find("tangerine"),
              std::string::npos);

    const auto deleted = provider.complete_endpoint(sp::files::Delete{file->id});
    EXPECT_TRUE(std::holds_alternative<sp::files::Deleted>(deleted)) << failure_text(deleted);
}

bool ended(sp::jobs::Status status) {
    return status == sp::jobs::Status::Completed || status == sp::jobs::Status::Cancelled ||
           status == sp::jobs::Status::Failed || status == sp::jobs::Status::Expired;
}

// An inline Anthropic batch is submitted, read, cancelled, and deleted once it ended.
TEST(LiveEndpoints, AnthropicBatchLifecycle) {
    LIVE_KEY(key, "ANTHROPIC_API_KEY");
    sp::runtime::Options options;
    options.api_key = key;
    SchemaProvider provider(descriptor("anthropic.messages", "https://api.anthropic.com", "/v1/messages",
                                       "/v1/messages"), options);
    sp::messages::Request item;
    item.model = "claude-haiku-4-5-20251001";
    item.max_tokens = 16;
    item.messages.push_back(sp::Message{"", sp::Role::User, {sp::Text{"Reply with one word."}}});
    sp::batches::Submit submit;
    submit.items.push_back({"live-1", std::move(item)});
    const auto submitted = provider.complete_endpoint(std::move(submit));
    const auto* batch = std::get_if<sp::batches::Batch>(&submitted);
    ASSERT_NE(batch, nullptr) << failure_text(submitted);
    EXPECT_EQ(batch->id.rfind("msgbatch_", 0), 0u);
    EXPECT_EQ(batch->counts.total, 1u);
    const auto id = batch->id;

    const auto read = provider.complete_endpoint(sp::batches::Get{id});
    ASSERT_TRUE(std::holds_alternative<sp::batches::Batch>(read)) << failure_text(read);
    EXPECT_EQ(std::get<sp::batches::Batch>(read).id, id);
    auto state = provider.complete_endpoint(sp::batches::Cancel{id});
    ASSERT_TRUE(std::holds_alternative<sp::batches::Batch>(state)) << failure_text(state);
    for (int i = 0; i != 60 && !ended(std::get<sp::batches::Batch>(state).status); ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        state = provider.complete_endpoint(sp::batches::Get{id});
        ASSERT_TRUE(std::holds_alternative<sp::batches::Batch>(state)) << failure_text(state);
    }
    ASSERT_TRUE(ended(std::get<sp::batches::Batch>(state).status)) << std::get<sp::batches::Batch>(state).raw_status;
    const auto deleted = provider.complete_endpoint(sp::batches::Delete{id});
    EXPECT_TRUE(std::holds_alternative<sp::batches::Deleted>(deleted)) << failure_text(deleted);
}

// An OpenAI batch reads its requests from an uploaded JSONL file; the batch is cancelled and the file deleted.
TEST(LiveEndpoints, OpenAIBatchFromAnUploadedFile) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    auto provider = openai_provider(key);
    sp::chat::Request item;
    item.model = "gpt-5-nano";
    item.reasoning_effort = "low";
    item.max_output_tokens = 200;
    item.messages.push_back(sp::chat::InputMessage{sp::Role::User, "Reply with one word."});
    const auto lines = sp::batches::jsonl(
        descriptor("openai.chat", "https://api.openai.com", "/v1/chat/completions", "/v1/chat/completions"),
        {sp::batches::Item{"live-1", std::move(item)}});
    ASSERT_TRUE(std::holds_alternative<std::string>(lines)) << std::get<sp::Error>(lines).safe_message;

    sp::files::Upload upload;
    upload.source.bytes = std::make_shared<const std::string>(std::get<std::string>(lines));
    upload.filename = "live-batch.jsonl";
    upload.mime = "application/jsonl";
    upload.purpose = "batch";
    const auto uploaded = provider.complete_endpoint(std::move(upload));
    const auto* file = std::get_if<sp::files::File>(&uploaded);
    ASSERT_NE(file, nullptr) << failure_text(uploaded);
    EXPECT_EQ(file->purpose, "batch");

    sp::batches::Submit submit;
    submit.input_file = file->id;
    submit.endpoint = "/v1/chat/completions";
    const auto submitted = provider.complete_endpoint(std::move(submit));
    const auto* batch = std::get_if<sp::batches::Batch>(&submitted);
    EXPECT_NE(batch, nullptr) << failure_text(submitted);
    if (batch) {
        EXPECT_EQ(batch->id.rfind("batch_", 0), 0u);
        const auto cancelled = provider.complete_endpoint(sp::batches::Cancel{batch->id});
        ASSERT_TRUE(std::holds_alternative<sp::batches::Batch>(cancelled)) << failure_text(cancelled);
        EXPECT_EQ(std::get<sp::batches::Batch>(cancelled).id, batch->id);
    }
    const auto deleted = provider.complete_endpoint(sp::files::Delete{file->id});
    EXPECT_TRUE(std::holds_alternative<sp::files::Deleted>(deleted)) << failure_text(deleted);
}

// The model catalogue refreshed from the four providers' listings (SchemaProvider's refresh_catalogue): every
// listing answers, a second refresh over the first finds no change, and a policy carrying the refreshed catalogue
// serves a model that only the refresh lists.
TEST(LiveCatalogue, RefreshFromEveryListing) {
    LIVE_KEY(openai, "OPENAI_API_KEY");
    LIVE_KEY(anthropic, "ANTHROPIC_API_KEY");
    LIVE_KEY(gemini, "GEMINI_API_KEY");
    LIVE_KEY(openrouter, "OPENROUTER_API_KEY");
    using sp::descriptor::Listing;
    sp::transport::RefreshRequest request;
    request.sources = {{Listing::OpenRouter, {}, openrouter, {}}, {Listing::Gemini, {}, gemini, {}},
                       {Listing::Anthropic, {}, anthropic, {}}, {Listing::OpenAI, {}, openai, {}}};
    request.overlays.push_back(sp::descriptor::builtin_catalogue());
    sp::transport::Transport transport;
    auto first = sp::transport::refresh_catalogue(transport, request);
    ASSERT_TRUE(std::holds_alternative<sp::transport::RefreshResult>(first));
    const auto refreshed = std::get<sp::transport::RefreshResult>(std::move(first));
    ASSERT_EQ(refreshed.sources.size(), 4u);
    for (const auto& source : refreshed.sources) {
        EXPECT_TRUE(source.ok) << sp::descriptor::listing_name(source.listing) << ": " << source.error;
        EXPECT_GT(source.models, 0u) << sp::descriptor::listing_name(source.listing);
    }

    request.previous = refreshed.generated;
    auto second = sp::transport::refresh_catalogue(transport, request);
    ASSERT_TRUE(std::holds_alternative<sp::transport::RefreshResult>(second));
    const auto& again = std::get<sp::transport::RefreshResult>(second).diff;
    EXPECT_TRUE(again.removed.empty()) << again.removed.size() << " removed";
    EXPECT_LT(again.added.size() + again.changed.size(), 5u);

    const std::string model = "gpt-5-nano";
    const auto& builtin = sp::descriptor::builtin_catalogue()->entries();
    EXPECT_TRUE(std::none_of(builtin.begin(), builtin.end(), [&](const auto& entry) { return entry.id == model; }));
    const auto& listed = refreshed.catalogue->entries();
    EXPECT_TRUE(std::any_of(listed.begin(), listed.end(), [&](const auto& entry) { return entry.id == model; }));
    auto policy = sp::descriptor::builtin_policy()->with_catalogue(refreshed.catalogue);
    ASSERT_TRUE(std::holds_alternative<sp::descriptor::PolicySnapshot>(policy));
    sp::runtime::Options options;
    options.api_key = openai;
    SchemaProvider provider(descriptor("openai.chat", "https://api.openai.com", "/v1/chat/completions",
        "/v1/chat/completions", std::get<sp::descriptor::PolicySnapshot>(std::move(policy))), options);
    sp::chat::Request chat;
    chat.model = model;
    chat.reasoning_effort = "low";
    chat.max_output_tokens = 300;
    chat.messages.push_back(sp::chat::InputMessage{sp::Role::User, "Reply with the single word: ok"});
    sp::runtime::Result result;
    EXPECT_GT(text_size(invoke(provider, std::move(chat), false, result)), 0u);
}

// A model no catalogue lists is still sent: the provider, not the SDK, answers that it does not exist.
TEST(LiveCatalogue, UnlistedModelIsSent) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    auto provider = openai_provider(key);
    sp::chat::Request chat;
    chat.model = "live-unlisted-model";
    chat.max_output_tokens = 16;
    chat.messages.push_back(sp::chat::InputMessage{sp::Role::User, "Reply with one word."});
    neograph::ProviderRequest call;
    call.payload = std::move(chat);
    const auto outcome = provider.invoke(std::move(call));
    const auto* failure = std::get_if<sp::Failure>(outcome.get());
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error.http_status, 404);
}

// SchemaProvider realtime transcription sessions (#512): speech made by OpenAI's speech endpoint goes in, and a
// final transcript and one committed user turn come out.
std::string from_base64(const std::string& text) {
    std::string out;
    unsigned value = 0;
    int bits = 0;
    for (const unsigned char c : text) {
        int digit = -1;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        if (digit < 0) continue;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((value >> bits) & 0xff));
        }
    }
    return out;
}

// 16-bit little-endian mono PCM resampled from 24 kHz to 16 kHz by linear interpolation.
std::string pcm_16k(const std::string& pcm_24k) {
    const auto sample = [&](std::size_t i) {
        return static_cast<std::int16_t>(static_cast<unsigned char>(pcm_24k[2 * i]) |
                                         (static_cast<unsigned char>(pcm_24k[2 * i + 1]) << 8));
    };
    const std::size_t samples = pcm_24k.size() / 2;
    std::string out;
    for (std::size_t i = 0; 3 * i + 2 < 2 * samples; ++i) {
        const std::size_t at = 3 * i / 2;
        const double fraction = (3.0 * i / 2.0) - static_cast<double>(at);
        const double value = sample(at) * (1.0 - fraction) + (at + 1 < samples ? sample(at + 1) : 0) * fraction;
        const auto rounded = static_cast<std::int16_t>(value);
        out.push_back(static_cast<char>(rounded & 0xff));
        out.push_back(static_cast<char>((rounded >> 8) & 0xff));
    }
    return out;
}

// Speech from OpenAI's speech endpoint as raw 24 kHz PCM.
std::string spoken_pcm(const std::string& key, const std::string& text) {
    auto provider = openai_provider(key);
    sp::speech::Request speech;
    speech.model = kSpeechModel;
    speech.input = text;
    speech.voice = "alloy";
    speech.format = "pcm";
    const auto spoken = provider.complete_endpoint(std::move(speech));
    const auto* audio = std::get_if<sp::speech::Result>(&spoken);
    if (!audio || !audio->audio.data) {
        ADD_FAILURE() << failure_text(spoken);
        return {};
    }
    return from_base64(*audio->audio.data);
}

// A SchemaProvider transcription session (no output modality): the speech goes in, a final transcript and one
// committed user turn come out.
std::string transcribe_live(const sp::descriptor::ValidatedDescriptor& descriptor, const std::string& key,
                            std::string model, const std::string& pcm, std::size_t chunk) {
    sp::realtime::Setup setup;
    setup.model = std::move(model);
    setup.output = {};
    sp::realtime::Options options;
    options.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    options.media_bytes = std::size_t{1} << 20;
    options.renew = [](const sp::Usage&) { return true; };
    sp::transport::Transport transport;
    auto opened = sp::realtime::open(transport, descriptor, key, setup, options);
    if (const auto* error = std::get_if<sp::Error>(&opened)) {
        ADD_FAILURE() << "open refused: " << error->safe_message;
        return {};
    }
    auto& session = std::get<sp::realtime::Session>(opened);
    std::string transcript;
    bool ready = false, committed = false;
    for (int i = 0; i < 600 && !committed && !session.ended(); ++i) {
        const auto event = session.next_event(std::chrono::milliseconds(100));
        if (!event) continue;
        if (std::holds_alternative<sp::realtime::Ready>(*event) && !ready) {
            ready = true;
            for (std::size_t at = 0; at < pcm.size(); at += chunk)
                session.send(sp::realtime::Audio{pcm.substr(at, chunk)});
            session.send(sp::realtime::Audio{std::string(chunk * 10, '\0')});
            session.send(sp::realtime::EndAudio{});
        } else if (const auto* text = std::get_if<sp::realtime::Transcript>(&*event); text && !text->partial) {
            transcript += text->text;
        } else if (const auto* turn = std::get_if<sp::realtime::TurnCommitted>(&*event)) {
            committed = true;
            EXPECT_EQ(turn->turn.messages.at(0).role, sp::Role::User);
        } else if (const auto* error = std::get_if<sp::realtime::ProviderError>(&*event)) {
            ADD_FAILURE() << "provider error: " << error->error.safe_message;
        }
    }
    session.close();
    const auto outcome = session.join();
    EXPECT_TRUE(committed);
    EXPECT_EQ(outcome.media_dropped, 0u);
    EXPECT_FALSE(outcome.error) << outcome.error->safe_message;
    return transcript;
}

TEST(LiveRealtime, OpenAITranscriptionSession) {
    LIVE_KEY(key, "OPENAI_API_KEY");
    const auto pcm = spoken_pcm(key, "The quick brown fox jumps over the lazy dog.");
    ASSERT_GT(pcm.size(), 10000u);
    const auto text = lower(transcribe_live(sp::descriptor::presets::openai_chat(), key, "gpt-live-transcribe", pcm, 4800));
    EXPECT_NE(text.find("fox"), std::string::npos) << text;
    EXPECT_NE(text.find("dog"), std::string::npos) << text;
}

TEST(LiveRealtime, GoogleTranscriptionSession) {
    LIVE_KEY(openai, "OPENAI_API_KEY");
    LIVE_KEY(gemini, "GEMINI_API_KEY");
    const auto pcm = pcm_16k(spoken_pcm(openai, "The quick brown fox jumps over the lazy dog."));
    ASSERT_GT(pcm.size(), 10000u);
    const auto text = lower(transcribe_live(sp::descriptor::presets::google_generate(), gemini,
                                            "gemini-3.5-transcribe-live", pcm, 3200));
    EXPECT_NE(text.find("fox"), std::string::npos) << text;
    EXPECT_NE(text.find("dog"), std::string::npos) << text;
}
} // namespace
