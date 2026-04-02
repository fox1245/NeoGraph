#pragma once

#include <neograph/provider.h>
#include <neograph/llm/json_path.h>
#include <fstream>
#include <memory>
#include <string>
#include <map>

namespace neograph::llm {

class SchemaProvider : public Provider {
  public:
    struct Config {
        std::string schema_path;   // path to provider JSON config file
        std::string api_key;       // API key (overrides env var if set)
        std::string default_model = "gpt-4o-mini";
        int timeout_seconds = 60;
    };

    static std::unique_ptr<SchemaProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;

  private:
    explicit SchemaProvider(Config config, json schema);

    // --- Strategies ---
    enum class SystemPromptStrategy {
        IN_MESSAGES,      // OpenAI: system message as role in messages array
        TOP_LEVEL,        // Claude: top-level "system" string field
        TOP_LEVEL_PARTS   // Gemini: top-level "system_instruction":{parts:[{text:...}]}
    };

    enum class ToolCallStrategy {
        TOOL_CALLS_ARRAY, // OpenAI: tool_calls array with function wrapper
        CONTENT_ARRAY,    // Claude: content array with tool_use items
        PARTS_ARRAY       // Gemini: parts array with functionCall items
    };

    enum class ToolResultStrategy {
        FLAT,             // OpenAI: {role:"tool", tool_call_id, content}
        CONTENT_ARRAY,    // Claude: {role:"user", content:[{type:"tool_result",...}]}
        PARTS_ARRAY       // Gemini: {role:"user", parts:[{functionResponse:{...}}]}
    };

    enum class ToolDefWrapper {
        FUNCTION,             // OpenAI: [{type:"function", function:{...}}]
        NONE,                 // Claude: [{name, description, input_schema}]
        FUNCTION_DECLARATIONS // Gemini: [{function_declarations:[{...}]}]
    };

    enum class ResponseStrategy {
        CHOICES_MESSAGE,  // OpenAI: choices[0].message
        CONTENT_ARRAY,    // Claude: content[] array
        CANDIDATES_PARTS  // Gemini: candidates[0].content.parts[]
    };

    enum class StreamFormat {
        SSE_DATA,         // OpenAI/Gemini: "data: {json}\n\n"
        SSE_EVENTS        // Claude: "event: type\ndata: {json}\n\n"
    };

    // --- Internal config parsed from schema ---
    struct ConnectionConfig {
        std::string base_url;
        std::string endpoint;
        std::string stream_endpoint;
        std::string auth_header;
        std::string auth_prefix;
        std::string api_key_env;
        std::string auth_query_param;  // e.g., "key" for Gemini
        std::map<std::string, std::string> extra_headers;
    };

    struct RequestConfig {
        std::string model_field;
        std::string messages_field;
        std::string tools_field;
        std::string temperature_path;
        std::string max_tokens_path;
        bool max_tokens_required = false;
        int max_tokens_default = -1;
        std::string stream_field;
        json extra_fields;
    };

    struct SystemPromptConfig {
        SystemPromptStrategy strategy;
        std::string field;
        std::string role_name;     // for IN_MESSAGES
        std::string parts_field;   // for TOP_LEVEL_PARTS
        std::string text_field;    // for TOP_LEVEL_PARTS
    };

    struct MessagesConfig {
        std::string role_field;
        std::string content_field;
        std::map<std::string, std::string> role_map;
        bool content_is_parts = false;
        json text_part_template;  // for parts-based content
    };

    struct ToolDefConfig {
        ToolDefWrapper wrapper;
        std::string name_field;
        std::string description_field;
        std::string parameters_field;
    };

    struct ToolCallConfig {
        ToolCallStrategy strategy;
        std::string field;        // for TOOL_CALLS_ARRAY
        json item_template;
        json text_item_template;  // for content_array/parts_array text items
    };

    struct ToolResultConfig {
        std::string role;
        ToolResultStrategy strategy;
        std::string id_field;      // for FLAT
        std::string content_field; // for FLAT
        json item_template;
    };

    struct ImageConfig {
        std::string strategy;   // "openai", "claude", "gemini"
        json item_template;
        json text_part_template;
    };

    struct ResponseConfig {
        ResponseStrategy strategy;
        // CHOICES_MESSAGE
        std::string message_path;
        std::string content_field;
        std::string role_field;
        std::string tool_calls_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        bool tool_call_args_is_string = true;
        // CONTENT_ARRAY
        std::string content_path;
        std::string text_type;
        std::string text_field;
        std::string tool_use_type;
        std::string tool_call_name_field;
        std::string tool_call_args_field;
        // CANDIDATES_PARTS
        std::string parts_path;
        std::string function_call_field;
        // Usage
        std::string usage_path;
        std::string prompt_tokens_field;
        std::string completion_tokens_field;
        std::string total_tokens_field;
    };

    struct StreamConfig {
        StreamFormat format;
        // SSE_DATA
        std::string prefix;
        std::string done_signal;
        std::string delta_path;
        std::string content_field;
        std::string tool_calls_field;
        std::string tool_call_index_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        // SSE_DATA with candidates_parts delta (Gemini)
        std::string delta_strategy;  // "candidates_parts" for Gemini
        std::string delta_parts_path;
        std::string delta_text_field;
        std::string delta_function_call_field;
        std::string delta_tool_call_name_field;
        std::string delta_tool_call_args_field;
        // SSE_EVENTS (Claude)
        json events_config;
    };

    // --- Parsed config ---
    Config user_config_;
    json schema_;
    std::string provider_name_;
    ConnectionConfig conn_;
    RequestConfig req_;
    SystemPromptConfig sys_;
    MessagesConfig msgs_;
    ToolDefConfig tool_def_;
    ToolCallConfig tool_call_;
    ToolResultConfig tool_result_;
    ImageConfig image_;
    ResponseConfig resp_;
    StreamConfig stream_;

    // --- Internal methods ---
    void parse_schema();

    json build_body(const CompletionParams& params) const;
    json serialize_messages(const std::vector<ChatMessage>& messages) const;
    json serialize_tools(const std::vector<ChatTool>& tools) const;
    json serialize_single_message(const ChatMessage& msg) const;

    ChatMessage parse_response(const json& resp_json) const;
    ChatCompletion::Usage parse_usage(const json& resp_json) const;

    // Build endpoint URL with model substitution and auth query param
    std::string build_endpoint(const std::string& model, bool streaming) const;

    // Build HTTP headers
    std::map<std::string, std::string> build_headers() const;

    // Get the resolved API key
    std::string get_api_key() const;

    // Parse a data URL: "data:image/jpeg;base64,ABC" -> {mime, data}
    static std::pair<std::string, std::string> parse_data_url(const std::string& url);

    // Substitute $VAR placeholders in a JSON template
    static json substitute(const json& tmpl, const std::map<std::string, json>& vars);

    // Generate a unique tool call ID
    static std::string generate_tool_call_id();
};

} // namespace neograph::llm
