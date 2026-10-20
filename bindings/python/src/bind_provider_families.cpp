// The family nests of ProviderControls and the SDK types they hold. Each class carries caller data only; the
// provider's encoder and policy admit or refuse it at prepare. `from_dict` (neograph_engine.provider_controls)
// builds the same objects from plain dicts.
#include "json_bridge.h"
#include "provider_bridge.h"
#include "sdk_members.h"
#include <pybind11/stl.h>

namespace py = pybind11;
namespace neograph::pybind {
namespace {
template <class T>
void extensions_member(py::class_<T>& cls, std::optional<json> T::*field) {
    cls.def_property("extensions", [field](const T& v) -> py::object {
            return v.*field ? json_to_py(*(v.*field)) : py::none();
        }, [field](T& v, py::object x) {
            if (x.is_none()) (v.*field).reset();
            else v.*field = py_to_json(x);
        });
}
}  // namespace

void init_provider_families(py::module_& m) {
    // Shared OpenRouter gateway values.
    auto image_config = py::class_<sp::OpenRouterImageConfig>(m, "OpenRouterImageConfig").def(py::init<>());
    member(image_config, "aspect_ratio", &sp::OpenRouterImageConfig::aspect_ratio);
    member(image_config, "image_size", &sp::OpenRouterImageConfig::image_size);
    member(image_config, "size", &sp::OpenRouterImageConfig::size);
    member(image_config, "quality", &sp::OpenRouterImageConfig::quality);
    member(image_config, "background", &sp::OpenRouterImageConfig::background);
    member(image_config, "output_format", &sp::OpenRouterImageConfig::output_format);
    member(image_config, "moderation", &sp::OpenRouterImageConfig::moderation);
    member(image_config, "output_compression", &sp::OpenRouterImageConfig::output_compression);
    auto plugin = py::class_<sp::OpenRouterPlugin>(m, "OpenRouterPlugin").def(py::init<>());
    member(plugin, "id", &sp::OpenRouterPlugin::id);
    json_member(plugin, "options", &sp::OpenRouterPlugin::options);

    // Chat Completions.
    py::enum_<sp::chat::OutputModality>(m, "ChatOutputModality")
        .value("Text", sp::chat::OutputModality::Text)
        .value("Audio", sp::chat::OutputModality::Audio)
        .value("Image", sp::chat::OutputModality::Image);
    auto audio = py::class_<sp::chat::AudioOutput>(m, "ChatAudioOutput").def(py::init<>());
    member(audio, "voice", &sp::chat::AudioOutput::voice);
    member(audio, "format", &sp::chat::AudioOutput::format);
    auto chat = py::class_<ChatControls>(m, "ChatControls").def(py::init<>());
    member(chat, "reasoning", &ChatControls::reasoning);
    member(chat, "include_reasoning", &ChatControls::include_reasoning);
    member(chat, "usage_include", &ChatControls::usage_include);
    member(chat, "models", &ChatControls::models);
    member(chat, "modalities", &ChatControls::modalities);
    member(chat, "audio", &ChatControls::audio);
    member(chat, "image_config", &ChatControls::image_config);
    member(chat, "plugins", &ChatControls::plugins);
    extensions_member(chat, &ChatControls::extensions);

    // Responses.
    namespace r = sp::responses;
    auto location = py::class_<r::ApproximateLocation>(m, "ResponsesApproximateLocation").def(py::init<>());
    member(location, "city", &r::ApproximateLocation::city);
    member(location, "country", &r::ApproximateLocation::country);
    member(location, "region", &r::ApproximateLocation::region);
    member(location, "timezone", &r::ApproximateLocation::timezone);
    auto web_search = py::class_<r::WebSearchTool>(m, "ResponsesWebSearchTool").def(py::init<>());
    member(web_search, "allowed_domains", &r::WebSearchTool::allowed_domains);
    member(web_search, "user_location", &r::WebSearchTool::user_location);
    member(web_search, "search_context_size", &r::WebSearchTool::search_context_size);
    member(web_search, "external_web_access", &r::WebSearchTool::external_web_access);
    auto image_generation = py::class_<r::ImageGenerationTool>(m, "ResponsesImageGenerationTool").def(py::init<>());
    member(image_generation, "size", &r::ImageGenerationTool::size);
    member(image_generation, "quality", &r::ImageGenerationTool::quality);
    member(image_generation, "output_format", &r::ImageGenerationTool::output_format);
    member(image_generation, "model", &r::ImageGenerationTool::model);
    member(image_generation, "action", &r::ImageGenerationTool::action);
    member(image_generation, "background", &r::ImageGenerationTool::background);
    member(image_generation, "moderation", &r::ImageGenerationTool::moderation);
    member(image_generation, "input_fidelity", &r::ImageGenerationTool::input_fidelity);
    member(image_generation, "output_compression", &r::ImageGenerationTool::output_compression);
    member(image_generation, "partial_images", &r::ImageGenerationTool::partial_images);
    member(image_generation, "mask_file_id", &r::ImageGenerationTool::mask_file_id);
    member(image_generation, "mask_image_url", &r::ImageGenerationTool::mask_image_url);
    auto file_search = py::class_<r::FileSearchTool>(m, "ResponsesFileSearchTool").def(py::init<>());
    member(file_search, "vector_store_ids", &r::FileSearchTool::vector_store_ids);
    member(file_search, "max_num_results", &r::FileSearchTool::max_num_results);
    py::class_<r::ToolSearchTool>(m, "ResponsesToolSearchTool").def(py::init<>());
    auto skill = py::class_<r::SkillReference>(m, "ResponsesSkillReference").def(py::init<>());
    member(skill, "skill_id", &r::SkillReference::skill_id);
    auto environment = py::class_<r::ContainerAutoEnvironment>(m, "ResponsesContainerAutoEnvironment").def(py::init<>());
    member(environment, "skills", &r::ContainerAutoEnvironment::skills);
    auto shell = py::class_<r::ShellTool>(m, "ResponsesShellTool").def(py::init<>());
    member(shell, "environment", &r::ShellTool::environment);
    // Domain secrets are not exposed here: credentials in shared controls need their own review.
    auto network = py::class_<r::ContainerNetworkPolicy>(m, "ResponsesContainerNetworkPolicy").def(py::init<>());
    member(network, "disabled", &r::ContainerNetworkPolicy::disabled);
    member(network, "allowed_domains", &r::ContainerNetworkPolicy::allowed_domains);
    auto code_interpreter = py::class_<r::CodeInterpreterTool>(m, "ResponsesCodeInterpreterTool").def(py::init<>());
    member(code_interpreter, "container_id", &r::CodeInterpreterTool::container_id);
    member(code_interpreter, "file_ids", &r::CodeInterpreterTool::file_ids);
    member(code_interpreter, "memory_limit", &r::CodeInterpreterTool::memory_limit);
    member(code_interpreter, "network_policy", &r::CodeInterpreterTool::network_policy);
    auto filter = py::class_<r::McpToolFilter>(m, "ResponsesMcpToolFilter").def(py::init<>());
    member(filter, "tool_names", &r::McpToolFilter::tool_names);
    member(filter, "read_only", &r::McpToolFilter::read_only);
    auto mcp = py::class_<r::McpTool>(m, "ResponsesMcpTool").def(py::init<>());
    member(mcp, "server_label", &r::McpTool::server_label);
    member(mcp, "server_url", &r::McpTool::server_url);
    member(mcp, "connector_id", &r::McpTool::connector_id);
    member(mcp, "tunnel_id", &r::McpTool::tunnel_id);
    member(mcp, "authorization", &r::McpTool::authorization);
    member(mcp, "headers", &r::McpTool::headers);
    member(mcp, "allowed_tools", &r::McpTool::allowed_tools);
    member(mcp, "require_approval", &r::McpTool::require_approval);
    member(mcp, "approval_always", &r::McpTool::approval_always);
    member(mcp, "approval_never", &r::McpTool::approval_never);
    member(mcp, "server_description", &r::McpTool::server_description);
    member(mcp, "defer_loading", &r::McpTool::defer_loading);
    py::class_<r::ComputerTool>(m, "ResponsesComputerTool").def(py::init<>());
    auto computer_preview = py::class_<r::ComputerUsePreviewTool>(m, "ResponsesComputerUsePreviewTool").def(py::init<>());
    member(computer_preview, "environment", &r::ComputerUsePreviewTool::environment);
    member(computer_preview, "display_width", &r::ComputerUsePreviewTool::display_width);
    member(computer_preview, "display_height", &r::ComputerUsePreviewTool::display_height);
    auto openrouter_tool = py::class_<r::OpenRouterServerTool>(m, "ResponsesOpenRouterServerTool").def(py::init<>());
    member(openrouter_tool, "type", &r::OpenRouterServerTool::type);
    json_member(openrouter_tool, "parameters", &r::OpenRouterServerTool::parameters);
    auto generic_tool = py::class_<r::GenericHostedTool>(m, "ResponsesGenericHostedTool").def(py::init<>());
    member(generic_tool, "type", &r::GenericHostedTool::type);
    json_member(generic_tool, "options", &r::GenericHostedTool::options);
    py::enum_<r::OutputModality>(m, "ResponsesOutputModality")
        .value("Text", r::OutputModality::Text)
        .value("Image", r::OutputModality::Image);
    auto approval = py::class_<r::McpApproval>(m, "ResponsesMcpApproval").def(py::init<>());
    member(approval, "request_id", &r::McpApproval::request_id);
    member(approval, "approve", &r::McpApproval::approve);
    member(approval, "reason", &r::McpApproval::reason);
    auto responses = py::class_<ResponsesControls>(m, "ResponsesControls").def(py::init<>());
    member(responses, "previous_response_id", &ResponsesControls::previous_response_id);
    member(responses, "previous_response_history", &ResponsesControls::previous_response_history);
    member(responses, "parallel_tool_calls", &ResponsesControls::parallel_tool_calls);
    member(responses, "verbosity", &ResponsesControls::verbosity);
    member(responses, "truncation", &ResponsesControls::truncation);
    member(responses, "include", &ResponsesControls::include);
    member(responses, "include_names", &ResponsesControls::include_names);
    member(responses, "hosted_tools", &ResponsesControls::hosted_tools);
    member(responses, "modalities", &ResponsesControls::modalities);
    member(responses, "image_config", &ResponsesControls::image_config);
    member(responses, "plugins", &ResponsesControls::plugins);
    member(responses, "mcp_approvals", &ResponsesControls::mcp_approvals);
    extensions_member(responses, &ResponsesControls::extensions);

    // Anthropic Messages.
    namespace a = sp::messages;
    auto user_location = py::class_<a::UserLocation>(m, "MessagesUserLocation").def(py::init<>());
    member(user_location, "city", &a::UserLocation::city);
    member(user_location, "region", &a::UserLocation::region);
    member(user_location, "country", &a::UserLocation::country);
    member(user_location, "timezone", &a::UserLocation::timezone);
    auto tool = py::class_<a::ToolDefinition>(m, "MessagesToolDefinition").def(py::init<>());
    member(tool, "name", &a::ToolDefinition::name);
    member(tool, "description", &a::ToolDefinition::description);
    json_member(tool, "input_schema", &a::ToolDefinition::input_schema);
    member(tool, "type", &a::ToolDefinition::type);
    member(tool, "max_uses", &a::ToolDefinition::max_uses);
    member(tool, "allowed_callers", &a::ToolDefinition::allowed_callers);
    member(tool, "allowed_domains", &a::ToolDefinition::allowed_domains);
    member(tool, "blocked_domains", &a::ToolDefinition::blocked_domains);
    member(tool, "user_location", &a::ToolDefinition::user_location);
    member(tool, "citations", &a::ToolDefinition::citations);
    member(tool, "max_content_tokens", &a::ToolDefinition::max_content_tokens);
    json_member(tool, "options", &a::ToolDefinition::options);
    auto messages_mcp = py::class_<a::McpServer>(m, "MessagesMcpServer").def(py::init<>());
    member(messages_mcp, "name", &a::McpServer::name);
    member(messages_mcp, "url", &a::McpServer::url);
    member(messages_mcp, "authorization_token", &a::McpServer::authorization_token);
    auto container_skill = py::class_<a::ContainerSkill>(m, "MessagesContainerSkill").def(py::init<>());
    member(container_skill, "type", &a::ContainerSkill::type);
    member(container_skill, "skill_id", &a::ContainerSkill::skill_id);
    member(container_skill, "version", &a::ContainerSkill::version);
    auto messages = py::class_<MessagesControls>(m, "MessagesControls").def(py::init<>());
    member(messages, "thinking_mode", &MessagesControls::thinking_mode);
    member(messages, "output_effort", &MessagesControls::output_effort);
    member(messages, "cache_control", &MessagesControls::cache_control);
    member(messages, "tool_choice", &MessagesControls::tool_choice);
    member(messages, "tools", &MessagesControls::tools);
    member(messages, "container", &MessagesControls::container);
    member(messages, "skills", &MessagesControls::skills);
    member(messages, "betas", &MessagesControls::betas);
    member(messages, "mcp_servers", &MessagesControls::mcp_servers);
    extensions_member(messages, &MessagesControls::extensions);

    // Google generateContent.
    namespace g = sp::gemini;
    py::enum_<g::ResponseModality>(m, "GeminiResponseModality")
        .value("Text", g::ResponseModality::Text)
        .value("Image", g::ResponseModality::Image)
        .value("Audio", g::ResponseModality::Audio);
    auto voice = py::class_<g::VoiceConfig>(m, "GeminiVoiceConfig").def(py::init<>());
    member(voice, "prebuilt_voice_name", &g::VoiceConfig::prebuilt_voice_name);
    member(voice, "voice", &g::VoiceConfig::voice);
    auto speaker_voice = py::class_<g::SpeakerVoiceConfig>(m, "GeminiSpeakerVoiceConfig").def(py::init<>());
    member(speaker_voice, "speaker", &g::SpeakerVoiceConfig::speaker);
    member(speaker_voice, "voice_config", &g::SpeakerVoiceConfig::voice_config);
    auto speech = py::class_<g::SpeechConfig>(m, "GeminiSpeechConfig").def(py::init<>());
    member(speech, "voice_config", &g::SpeechConfig::voice_config);
    member(speech, "speaker_voice_configs", &g::SpeechConfig::speaker_voice_configs);
    member(speech, "language_code", &g::SpeechConfig::language_code);
    auto gemini_image = py::class_<g::ImageConfig>(m, "GeminiImageConfig").def(py::init<>());
    member(gemini_image, "aspect_ratio", &g::ImageConfig::aspect_ratio);
    member(gemini_image, "image_size", &g::ImageConfig::image_size);
    py::enum_<g::TranscriptionMode>(m, "GeminiTranscriptionMode")
        .value("Verbatim", g::TranscriptionMode::Verbatim)
        .value("Smart", g::TranscriptionMode::Smart);
    auto transcription = py::class_<g::AudioTranscriptionConfig>(m, "GeminiAudioTranscriptionConfig").def(py::init<>());
    member(transcription, "language_codes", &g::AudioTranscriptionConfig::language_codes);
    member(transcription, "custom_vocabulary", &g::AudioTranscriptionConfig::custom_vocabulary);
    member(transcription, "word_timestamp", &g::AudioTranscriptionConfig::word_timestamp);
    member(transcription, "diarization", &g::AudioTranscriptionConfig::diarization);
    member(transcription, "mode", &g::AudioTranscriptionConfig::mode);
    auto audio_format = py::class_<g::AudioFormat>(m, "GeminiAudioFormat").def(py::init<>());
    member(audio_format, "mime_type", &g::AudioFormat::mime_type);
    member(audio_format, "sample_rate", &g::AudioFormat::sample_rate);
    member(audio_format, "bit_rate", &g::AudioFormat::bit_rate);
    member(audio_format, "delivery", &g::AudioFormat::delivery);
    auto google_search = py::class_<g::GoogleSearchTool>(m, "GeminiGoogleSearchTool").def(py::init<>());
    member(google_search, "start_time", &g::GoogleSearchTool::start_time);
    member(google_search, "end_time", &g::GoogleSearchTool::end_time);
    member(google_search, "web_search", &g::GoogleSearchTool::web_search);
    member(google_search, "image_search", &g::GoogleSearchTool::image_search);
    py::class_<g::UrlContextTool>(m, "GeminiUrlContextTool").def(py::init<>());
    py::class_<g::CodeExecutionTool>(m, "GeminiCodeExecutionTool").def(py::init<>());
    auto maps = py::class_<g::GoogleMapsTool>(m, "GeminiGoogleMapsTool").def(py::init<>());
    member(maps, "enable_widget", &g::GoogleMapsTool::enable_widget);
    auto computer_use = py::class_<g::ComputerUseTool>(m, "GeminiComputerUseTool").def(py::init<>());
    member(computer_use, "environment", &g::ComputerUseTool::environment);
    member(computer_use, "excluded_functions", &g::ComputerUseTool::excluded_functions);
    member(computer_use, "disabled_safety_policies", &g::ComputerUseTool::disabled_safety_policies);
    member(computer_use, "prompt_injection_detection", &g::ComputerUseTool::prompt_injection_detection);
    auto gemini_file_search = py::class_<g::FileSearchTool>(m, "GeminiFileSearchTool").def(py::init<>());
    member(gemini_file_search, "store_names", &g::FileSearchTool::store_names);
    member(gemini_file_search, "metadata_filter", &g::FileSearchTool::metadata_filter);
    member(gemini_file_search, "top_k", &g::FileSearchTool::top_k);
    auto gemini_mcp = py::class_<g::McpServer>(m, "GeminiMcpServer").def(py::init<>());
    member(gemini_mcp, "name", &g::McpServer::name);
    member(gemini_mcp, "url", &g::McpServer::url);
    member(gemini_mcp, "headers", &g::McpServer::headers);
    member(gemini_mcp, "timeout", &g::McpServer::timeout);
    member(gemini_mcp, "sse_read_timeout", &g::McpServer::sse_read_timeout);
    member(gemini_mcp, "terminate_on_close", &g::McpServer::terminate_on_close);
    auto retrieval = py::class_<g::RetrievalConfig>(m, "GeminiRetrievalConfig").def(py::init<>());
    member(retrieval, "latitude", &g::RetrievalConfig::latitude);
    member(retrieval, "longitude", &g::RetrievalConfig::longitude);
    member(retrieval, "language_code", &g::RetrievalConfig::language_code);
    auto generate = py::class_<GenerateControls>(m, "GenerateControls").def(py::init<>());
    member(generate, "history_mode", &GenerateControls::history_mode);
    member(generate, "thinking_level", &GenerateControls::thinking_level);
    member(generate, "safety_settings", &GenerateControls::safety_settings);
    member(generate, "tool_choice", &GenerateControls::tool_choice);
    member(generate, "response_modalities", &GenerateControls::response_modalities);
    member(generate, "speech_config", &GenerateControls::speech_config);
    member(generate, "image_config", &GenerateControls::image_config);
    member(generate, "audio_transcription_config", &GenerateControls::audio_transcription_config);
    member(generate, "audio_format", &GenerateControls::audio_format);
    member(generate, "builtin_tools", &GenerateControls::builtin_tools);
    member(generate, "mcp_servers", &GenerateControls::mcp_servers);
    member(generate, "server_tool_invocations", &GenerateControls::server_tool_invocations);
    member(generate, "retrieval_config", &GenerateControls::retrieval_config);
    extensions_member(generate, &GenerateControls::extensions);

    // Google Interactions.
    namespace i = sp::interactions;
    py::enum_<i::Delivery>(m, "InteractionsDelivery")
        .value("Inline", i::Delivery::Inline)
        .value("Uri", i::Delivery::Uri);
    auto audio_response = py::class_<i::AudioResponseFormat>(m, "InteractionsAudioResponseFormat").def(py::init<>());
    member(audio_response, "mime_type", &i::AudioResponseFormat::mime_type);
    member(audio_response, "sample_rate", &i::AudioResponseFormat::sample_rate);
    member(audio_response, "bit_rate", &i::AudioResponseFormat::bit_rate);
    member(audio_response, "delivery", &i::AudioResponseFormat::delivery);
    auto image_response = py::class_<i::ImageResponseFormat>(m, "InteractionsImageResponseFormat").def(py::init<>());
    member(image_response, "mime_type", &i::ImageResponseFormat::mime_type);
    member(image_response, "aspect_ratio", &i::ImageResponseFormat::aspect_ratio);
    member(image_response, "image_size", &i::ImageResponseFormat::image_size);
    member(image_response, "delivery", &i::ImageResponseFormat::delivery);
    auto video_response = py::class_<i::VideoResponseFormat>(m, "InteractionsVideoResponseFormat").def(py::init<>());
    member(video_response, "aspect_ratio", &i::VideoResponseFormat::aspect_ratio);
    member(video_response, "resolution", &i::VideoResponseFormat::resolution);
    member(video_response, "duration", &i::VideoResponseFormat::duration);
    member(video_response, "gcs_uri", &i::VideoResponseFormat::gcs_uri);
    member(video_response, "delivery", &i::VideoResponseFormat::delivery);
    auto text_response = py::class_<i::TextResponseFormat>(m, "InteractionsTextResponseFormat").def(py::init<>());
    member(text_response, "mime_type", &i::TextResponseFormat::mime_type);
    json_member(text_response, "schema", &i::TextResponseFormat::schema);
    auto interactions_speech = py::class_<i::SpeechConfig>(m, "InteractionsSpeechConfig").def(py::init<>());
    member(interactions_speech, "voice", &i::SpeechConfig::voice);
    member(interactions_speech, "speaker", &i::SpeechConfig::speaker);
    member(interactions_speech, "language", &i::SpeechConfig::language);
    auto speakers = py::class_<i::SpeakerConfig>(m, "InteractionsSpeakerConfig").def(py::init<>());
    member(speakers, "speakers", &i::SpeakerConfig::speakers);
    member(speakers, "conversational", &i::SpeakerConfig::conversational);
    py::enum_<i::TranscriptionMode>(m, "InteractionsTranscriptionMode")
        .value("Verbatim", i::TranscriptionMode::Verbatim)
        .value("Smart", i::TranscriptionMode::Smart);
    auto interactions_transcription =
        py::class_<i::TranscriptionConfig>(m, "InteractionsTranscriptionConfig").def(py::init<>());
    member(interactions_transcription, "language_codes", &i::TranscriptionConfig::language_codes);
    member(interactions_transcription, "custom_vocabulary", &i::TranscriptionConfig::custom_vocabulary);
    member(interactions_transcription, "mode", &i::TranscriptionConfig::mode);
    member(interactions_transcription, "speaker_diarization", &i::TranscriptionConfig::speaker_diarization);
    member(interactions_transcription, "word_timestamps", &i::TranscriptionConfig::word_timestamps);
    auto interactions_search = py::class_<i::GoogleSearchTool>(m, "InteractionsGoogleSearchTool").def(py::init<>());
    member(interactions_search, "search_types", &i::GoogleSearchTool::search_types);
    py::class_<i::UrlContextTool>(m, "InteractionsUrlContextTool").def(py::init<>());
    py::class_<i::CodeExecutionTool>(m, "InteractionsCodeExecutionTool").def(py::init<>());
    auto interactions = py::class_<InteractionsControls>(m, "InteractionsControls").def(py::init<>());
    member(interactions, "response_format", &InteractionsControls::response_format);
    member(interactions, "speech_config", &InteractionsControls::speech_config);
    member(interactions, "transcription_config", &InteractionsControls::transcription_config);
    member(interactions, "builtin_tools", &InteractionsControls::builtin_tools);
    member(interactions, "previous_interaction_id", &InteractionsControls::previous_interaction_id);
    member(interactions, "video_task", &InteractionsControls::video_task);
    member(interactions, "agent", &InteractionsControls::agent);
    member(interactions, "background", &InteractionsControls::background);
    extensions_member(interactions, &InteractionsControls::extensions);
}

}  // namespace neograph::pybind
