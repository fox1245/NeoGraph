// The family nests of ProviderControls reach the wire through make_provider_request, each family takes only its
// own nest, and the deprecated flat members move into their nests.
#include <gtest/gtest.h>
#include "fixtures/typed_wire_peer.h"

using namespace neograph;
namespace wire = neograph::test::wire;
namespace {
struct Encoded {
    PreparedProviderRequest prepared;
    json body;
};
Encoded encode(const std::string& family, const std::string& origin, ProviderControls controls,
               std::vector<ChatTool> tools = {}) {
    auto provider = wire::provider(family, origin);
    controls.max_output_tokens = 64;
    if (family != "openai.chat") controls.account_scope = "fixture-account";
    auto request = make_provider_request(*provider, "fixture-model",
        {test::message("ping", sp::Role::User)}, std::move(tools), std::move(controls));
    Encoded result{provider->prepare(std::move(request)), json()};
    if (result.prepared.valid()) result.body = json::parse(std::string(result.prepared.encoded_body()));
    return result;
}
}  // namespace

TEST(ProviderFamilyControls, GenerateSpeechAndModalitiesReachTheWire) {
    ProviderControls controls;
    controls.generate.response_modalities = {sp::gemini::ResponseModality::Audio};
    sp::gemini::SpeechConfig speech;
    speech.voice_config = sp::gemini::VoiceConfig{std::string("Kore"), std::nullopt};
    controls.generate.speech_config = speech;
    const auto encoded = encode("google.generate", "https://generativelanguage.googleapis.com", controls);
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    const auto& config = encoded.body.at("generationConfig");
    EXPECT_EQ(config.at("responseModalities"), json::array({"AUDIO"}));
    EXPECT_EQ(config.at("speechConfig").at("voiceConfig").at("prebuiltVoiceConfig").at("voiceName"), "Kore");
}

TEST(ProviderFamilyControls, ResponsesHostedToolsReachTheWire) {
    ProviderControls controls;
    sp::responses::ImageGenerationTool image;
    image.size = "1024x1024";
    controls.responses.hosted_tools = {image, sp::responses::WebSearchTool{}};
    const auto encoded = encode("openai.responses", "https://api.openai.com", controls);
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    const auto& tools = encoded.body.at("tools");
    ASSERT_EQ(tools.size(), 2u);
    EXPECT_EQ(tools.at(0).at("type"), "image_generation");
    EXPECT_EQ(tools.at(0).at("size"), "1024x1024");
    EXPECT_EQ(tools.at(1).at("type"), "web_search");
}

TEST(ProviderFamilyControls, MessagesServerToolsFollowThePortableTools) {
    ProviderControls controls;
    sp::messages::ToolDefinition search;
    search.type = "web_search_20250305";
    search.name = "web_search";
    search.max_uses = 1;
    search.allowed_domains = {"example.com"};
    controls.messages.tools = {search};
    ChatTool lookup{"lookup", "Look up a value", json{{"type", "object"}}};
    const auto encoded = encode("anthropic.messages", "https://api.anthropic.com", controls, {lookup});
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    const auto& tools = encoded.body.at("tools");
    ASSERT_EQ(tools.size(), 2u);
    EXPECT_EQ(tools.at(0).at("name"), "lookup");
    EXPECT_EQ(tools.at(1).at("type"), "web_search_20250305");
    EXPECT_EQ(tools.at(1).at("allowed_domains"), json::array({"example.com"}));
    EXPECT_EQ(tools.at(1).at("max_uses"), 1);
}

TEST(ProviderFamilyControls, ChatAudioOutputReachesTheWire) {
    ProviderControls controls;
    controls.chat.modalities = {sp::chat::OutputModality::Text, sp::chat::OutputModality::Audio};
    controls.chat.audio = sp::chat::AudioOutput{"alloy", "wav"};
    const auto encoded = encode("openai.chat", "https://api.openai.com", controls);
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    EXPECT_EQ(encoded.body.at("modalities"), json::array({"text", "audio"}));
    EXPECT_EQ(encoded.body.at("audio").at("voice"), "alloy");
    EXPECT_EQ(encoded.body.at("audio").at("format"), "wav");
}

TEST(ProviderFamilyControls, InteractionsContinuationAndBackgroundReachTheWire) {
    ProviderControls controls;
    controls.store = true;
    controls.interactions.background = true;
    controls.interactions.previous_interaction_id = "v1_previous";
    const auto encoded = encode("google.interactions", "https://generativelanguage.googleapis.com", controls);
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    EXPECT_EQ(encoded.body.at("store"), true);
    EXPECT_EQ(encoded.body.at("background"), true);
    EXPECT_EQ(encoded.body.at("previous_interaction_id"), "v1_previous");
}

TEST(ProviderFamilyControls, AnotherFamilysNestIsIgnored) {
    ProviderControls controls;
    controls.generate.response_modalities = {sp::gemini::ResponseModality::Audio};
    controls.responses.hosted_tools = {sp::responses::WebSearchTool{}};
    controls.messages.betas = {"some-beta"};
    const auto encoded = encode("openai.chat", "https://api.openai.com", controls);
    ASSERT_TRUE(encoded.prepared.valid()) << encoded.prepared.error()->reason;
    EXPECT_FALSE(encoded.body.contains("tools"));
    EXPECT_FALSE(encoded.body.contains("modalities"));
    // A portable control the family cannot express still throws.
    ProviderControls portable;
    portable.temperature = 0.5;
    EXPECT_THROW(encode("google.interactions", "https://generativelanguage.googleapis.com", portable),
                 std::invalid_argument);
}

TEST(ProviderFamilyControls, DeprecatedMembersMoveIntoTheirNests) {
    ProviderControls alias;
    alias.gemini_thinking_level = sp::gemini::ThinkingLevel::Low;
    ProviderControls nest;
    nest.generate.thinking_level = sp::gemini::ThinkingLevel::Low;
    const auto from_alias = encode("google.generate", "https://generativelanguage.googleapis.com", alias);
    const auto from_nest = encode("google.generate", "https://generativelanguage.googleapis.com", nest);
    ASSERT_TRUE(from_alias.prepared.valid()) << from_alias.prepared.error()->reason;
    ASSERT_TRUE(from_nest.prepared.valid());
    EXPECT_EQ(from_alias.body, from_nest.body);
    // On another family the alias is ignored like its nest.
    EXPECT_TRUE(encode("openai.chat", "https://api.openai.com", alias).prepared.valid());
    ProviderControls both = nest;
    both.gemini_thinking_level = sp::gemini::ThinkingLevel::High;
    EXPECT_THROW(encode("google.generate", "https://generativelanguage.googleapis.com", both), std::invalid_argument);
}

TEST(ProviderFamilyControls, UnadmittedExtensionsAreRefusedAtPrepare) {
    ProviderControls controls;
    controls.chat.extensions = json{{"unadmitted_member", 1}};
    const auto encoded = encode("openai.chat", "https://api.openai.com", controls);
    ASSERT_FALSE(encoded.prepared.valid());
    ASSERT_NE(encoded.prepared.error(), nullptr);
    EXPECT_EQ(encoded.prepared.error()->reason.rfind("request.", 0), 0u) << encoded.prepared.error()->reason;
    ProviderControls not_object;
    not_object.chat.extensions = json::array({1});
    EXPECT_THROW(encode("openai.chat", "https://api.openai.com", not_object), std::invalid_argument);
}
