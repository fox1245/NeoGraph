/**
 * @file llm/schema_provider.h
 * @brief Multi-vendor LLM provider adapter driven by JSON schema configuration.
 *
 * SchemaProvider reads a JSON schema file that describes how to serialize
 * requests and parse responses for any LLM API (OpenAI, Claude, Gemini, etc.).
 * Built-in schemas are embedded at build time via embed_schemas.py.
 *
 * This allows supporting new LLM providers without writing C++ code --
 * just add a new JSON schema file.
 */
#pragma once

#include <neograph/provider.h>
#include <neograph/llm/json_path.h>
#include <fstream>
#include <memory>
#include <string>
#include <map>

namespace neograph::llm {

/**
 * @brief LLM provider that adapts to any API via a JSON schema.
 *
 * The schema describes connection details, request/response formats,
 * tool call conventions, and streaming protocols for a given LLM vendor.
 * Built-in schemas: "openai", "claude", "gemini".
 *
 * @code
 * auto provider = SchemaProvider::create({
 *     .schema_path = "claude",          // built-in schema name
 *     .api_key = "sk-ant-...",
 *     .default_model = "claude-sonnet-4-20250514"
 * });
 * @endcode
 *
 * @see OpenAIProvider for a simpler OpenAI-only provider.
 */
class SchemaProvider : public Provider {
  public:
    /// Configuration for schema-based provider.
    struct Config {
        std::string schema_path;                    ///< Path to schema file, or built-in name ("openai", "claude", "gemini").
        std::string api_key;                        ///< API key (overrides env var if set).
        std::string default_model = "gpt-4o-mini";  ///< Default model name.
        int timeout_seconds = 60;                   ///< HTTP request timeout in seconds.
    };

    /**
     * @brief Create a schema-based provider instance.
     *
     * If schema_path matches a built-in name, the embedded schema is used.
     * Otherwise it is treated as a file path.
     *
     * @param config Provider configuration with schema path and API key.
     * @return A unique_ptr to the created provider.
     * @throws std::runtime_error If the schema cannot be loaded or parsed.
     */
    static std::unique_ptr<SchemaProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;

    /// @brief Get the provider name (from the schema's "name" field).
    /// @return Provider identifier string (e.g., "openai", "claude", "gemini").
    std::string get_name() const override;

  private:
    explicit SchemaProvider(Config config, json schema);

    // --- Strategies (internal) ---
    enum class SystemPromptStrategy { IN_MESSAGES, TOP_LEVEL, TOP_LEVEL_PARTS };
    // FLAT_ITEMS: OpenAI Responses — tool calls are separate top-level items in input[] (not nested in a message).
    enum class ToolCallStrategy { TOOL_CALLS_ARRAY, CONTENT_ARRAY, PARTS_ARRAY, FLAT_ITEMS };
    // FLAT_ITEM: OpenAI Responses — {type:"function_call_output", call_id, output} as a top-level input[] item.
    enum class ToolResultStrategy { FLAT, CONTENT_ARRAY, PARTS_ARRAY, FLAT_ITEM };
    // FLAT_FUNCTION: OpenAI Responses — [{type:"function", name, description, parameters}] (no nesting under "function").
    enum class ToolDefWrapper { FUNCTION, NONE, FUNCTION_DECLARATIONS, FLAT_FUNCTION };
    // OUTPUT_ARRAY: OpenAI Responses — output[] with mixed message/function_call items.
    enum class ResponseStrategy { CHOICES_MESSAGE, CONTENT_ARRAY, CANDIDATES_PARTS, OUTPUT_ARRAY };
    enum class StreamFormat { SSE_DATA, SSE_EVENTS };

    // --- Internal config parsed from schema ---
    struct ConnectionConfig {
        std::string base_url;
        std::string endpoint;
        std::string stream_endpoint;
        std::string auth_header;
        std::string auth_prefix;
        std::string api_key_env;
        std::string auth_query_param;
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
        std::string role_name;
        std::string parts_field;
        std::string text_field;
    };

    struct MessagesConfig {
        std::string role_field;
        std::string content_field;
        std::map<std::string, std::string> role_map;
        bool content_is_parts = false;
        json text_part_template;
    };

    struct ToolDefConfig {
        ToolDefWrapper wrapper;
        std::string name_field;
        std::string description_field;
        std::string parameters_field;
    };

    struct ToolCallConfig {
        ToolCallStrategy strategy;
        std::string field;
        json item_template;
        json text_item_template;
    };

    struct ToolResultConfig {
        std::string role;
        ToolResultStrategy strategy;
        std::string id_field;
        std::string content_field;
        json item_template;
    };

    struct ImageConfig {
        std::string strategy;
        json item_template;
        json text_part_template;
    };

    struct ResponseConfig {
        ResponseStrategy strategy;
        std::string message_path;
        std::string content_field;
        std::string role_field;
        std::string tool_calls_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        bool tool_call_args_is_string = true;
        std::string content_path;
        std::string text_type;
        std::string text_field;
        std::string tool_use_type;
        std::string tool_call_name_field;
        std::string tool_call_args_field;
        std::string parts_path;
        std::string function_call_field;
        // OUTPUT_ARRAY (OpenAI Responses)
        std::string output_path;             ///< Path to output[] array (default: "output").
        std::string message_item_type;       ///< Discriminator value for message items (default: "message").
        std::string function_call_item_type; ///< Discriminator value for function_call items (default: "function_call").
        std::string message_content_field;   ///< Field holding message item's content[] (default: "content").
        std::string function_call_id_field;  ///< Field for tool call id inside function_call item (default: "call_id").
        std::string usage_path;
        std::string prompt_tokens_field;
        std::string completion_tokens_field;
        std::string total_tokens_field;
    };

    struct StreamConfig {
        StreamFormat format;
        std::string prefix;
        std::string done_signal;
        std::string delta_path;
        std::string content_field;
        std::string tool_calls_field;
        std::string tool_call_index_field;
        std::string tool_call_id_field;
        std::string tool_call_name_path;
        std::string tool_call_args_path;
        std::string delta_strategy;
        std::string delta_parts_path;
        std::string delta_text_field;
        std::string delta_function_call_field;
        std::string delta_tool_call_name_field;
        std::string delta_tool_call_args_field;
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

    std::string build_endpoint(const std::string& model, bool streaming) const;
    std::map<std::string, std::string> build_headers() const;
    std::string get_api_key() const;

    static std::pair<std::string, std::string> parse_data_url(const std::string& url);
    static json substitute(const json& tmpl, const std::map<std::string, json>& vars);
    static std::string generate_tool_call_id();
};

} // namespace neograph::llm
