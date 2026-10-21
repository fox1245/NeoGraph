// The family nests of ProviderControls reach the wire through make_provider_request, each family takes only its
// own nest, and the deprecated flat members move into their nests.
#include <gtest/gtest.h>
#include <neograph/context_store.h>
#include <neograph/runtime_turn_assembler.h>
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
    // Another family refuses the alias, as it did before the nests, while it ignores the nest.
    EXPECT_THROW(encode("openai.chat", "https://api.openai.com", alias), std::invalid_argument);
    EXPECT_TRUE(encode("openai.chat", "https://api.openai.com", nest).prepared.valid());
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

// The assembly receipt records a digest of the tools in the encoded body (#329). Every family writes them
// as the top-level `tools` member, so one reading serves all of them, and it moves with the tools alone.
TEST(ProviderFamilyControls, TheAssemblyReceiptDigestsTheEncodedToolsOfEveryFamily) {
    const std::vector<std::pair<std::string, std::string>> families{
        {"openai.chat", "https://api.openai.com"},
        {"openai.responses", "https://api.openai.com"},
        {"anthropic.messages", "https://api.anthropic.com"},
        {"google.generate", "https://generativelanguage.googleapis.com"},
        {"google.interactions", "https://generativelanguage.googleapis.com"}};
    const std::string sha_a = "sha256:" + std::string(64, 'a');
    InMemoryContextStore store;
    const ContextStoreFeed feed{"owner", "feed"};
    RuntimeHistoryRecordData record_data;
    record_data.feed_id = "feed";
    record_data.sequence = 1;
    record_data.message_id = "message_1";
    record_data.trust = RuntimeTrustClass::UntrustedInput;
    record_data.message = test::message("question", sp::Role::User);
    const auto record = RuntimeHistoryRecord::create(std::move(record_data));
    ASSERT_EQ(store.append_history(feed, record, std::nullopt), ContextStoreAppendResult::Appended);
    ContextArtifactData skill_data;
    skill_data.kind = ContextArtifactKind::RequiredSkill;
    skill_data.producer_id = "skill-resolver.v1";
    skill_data.source_digest = sha_a;
    skill_data.media_type = "text/plain";
    skill_data.placement = ContextPlacement::BeforeHistory;
    skill_data.required = true;
    skill_data.content = "Verify before editing.";
    const auto skill = ContextArtifact::create(std::move(skill_data));
    ASSERT_EQ(store.put_artifact("owner", skill), ContextArtifactPutResult::Stored);
    ContextEpochData epoch_data;
    epoch_data.run_id = "run";
    epoch_data.sequence = 1;
    epoch_data.feed_id = "feed";
    epoch_data.raw_from_sequence = 1;
    epoch_data.raw_through_sequence = 1;
    epoch_data.raw_window_digest = store.snapshot_history(feed, 1, 1).digest;
    epoch_data.artifact_ids = {skill.id()};
    const auto epoch = ContextEpoch::create(std::move(epoch_data));

    const ChatTool read{"read", "Read a file.", json{{"type", "object"}}, std::nullopt};
    const ChatTool write{"write", "Write a file.", json{{"type", "object"}}, std::nullopt};
    for (const auto& [family, origin] : families) {
        SCOPED_TRACE(family);
        const auto provider = wire::provider(family, origin);
        const auto assemble = [&](std::vector<ChatTool> tools) {
            ProviderControls controls;
            controls.max_output_tokens = 64;
            if (family != "openai.chat") controls.account_scope = "fixture-account";
            auto request = make_provider_request(*provider, "fixture-model",
                                                 {test::message("template", sp::Role::User)},
                                                 std::move(tools), std::move(controls));
            clear_provider_request_messages(request);
            return RuntimeTurnAssembler(store, {skill.id()}).assemble(
                *provider, "owner", epoch, std::move(request)).assembly_receipt;
        };
        const auto none = assemble({});
        const auto one = assemble({read});
        const auto two = assemble({read, write});
        EXPECT_EQ(none.encoder_family(), std::optional<std::string>(family));
        EXPECT_FALSE(none.tools_digest());
        ASSERT_TRUE(one.tools_digest());
        ASSERT_TRUE(two.tools_digest());
        EXPECT_NE(*one.tools_digest(), *two.tools_digest());
        EXPECT_EQ(assemble({read}).tools_digest(), one.tools_digest());
    }
}
