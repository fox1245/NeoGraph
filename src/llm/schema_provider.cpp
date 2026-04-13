#include <neograph/llm/schema_provider.h>
#include <builtin_schemas.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <random>
#include <chrono>
#include <algorithm>

namespace neograph::llm {

// ============================================================================
// Construction / Factory
// ============================================================================

SchemaProvider::SchemaProvider(Config config, json schema)
    : user_config_(std::move(config))
    , schema_(std::move(schema))
{
    parse_schema();
}

std::unique_ptr<SchemaProvider>
SchemaProvider::create(const Config& config)
{
    json schema;

    // Check builtin schemas first (e.g., "openai", "claude", "gemini")
    const auto& builtins = builtin::schemas();
    auto it = builtins.find(config.schema_path);
    if (it != builtins.end()) {
        schema = json::parse(it->second);
        return std::unique_ptr<SchemaProvider>(new SchemaProvider(config, std::move(schema)));
    }

    // Fall back to file path
    std::ifstream file(config.schema_path);
    if (!file.is_open()) {
        throw std::runtime_error("SchemaProvider: cannot open schema file: " + config.schema_path);
    }

    try {
        schema = json::parse(file);
    } catch (const json::parse_error& e) {
        throw std::runtime_error("SchemaProvider: invalid JSON in schema file: " + std::string(e.what()));
    }

    return std::unique_ptr<SchemaProvider>(new SchemaProvider(config, std::move(schema)));
}

std::string SchemaProvider::get_name() const {
    return provider_name_;
}

// ============================================================================
// Schema Parsing
// ============================================================================

void SchemaProvider::parse_schema()
{
    provider_name_ = schema_.value("name", "unknown");

    // --- Connection ---
    auto& c = schema_["connection"];
    conn_.base_url = c.value("base_url", "");
    conn_.endpoint = c.value("endpoint", "");
    conn_.stream_endpoint = c.value("stream_endpoint", conn_.endpoint);
    conn_.auth_header = c.value("auth_header", "");
    conn_.auth_prefix = c.value("auth_prefix", "");
    conn_.api_key_env = c.value("api_key_env", "");
    conn_.auth_query_param = c.value("auth_query_param", "");
    if (c.contains("extra_headers") && c["extra_headers"].is_object()) {
        for (auto& [k, v] : c["extra_headers"].items()) {
            conn_.extra_headers[k] = v.get<std::string>();
        }
    }

    // --- Request ---
    auto& r = schema_["request"];
    req_.model_field = r.value("model_field", "model");
    req_.messages_field = r.value("messages_field", "messages");
    req_.tools_field = r.value("tools_field", "tools");
    req_.temperature_path = r.value("temperature_path", "temperature");
    req_.max_tokens_path = r.value("max_tokens_path", "max_tokens");
    req_.max_tokens_required = r.value("max_tokens_required", false);
    req_.max_tokens_default = r.value("max_tokens_default", -1);
    req_.stream_field = r.value("stream_field", "stream");
    req_.extra_fields = r.value("extra_fields", json::object());

    // --- System Prompt ---
    auto& s = schema_["system_prompt"];
    std::string sys_strategy = s.value("strategy", "in_messages");
    if (sys_strategy == "top_level") {
        sys_.strategy = SystemPromptStrategy::TOP_LEVEL;
    } else if (sys_strategy == "top_level_parts") {
        sys_.strategy = SystemPromptStrategy::TOP_LEVEL_PARTS;
    } else {
        sys_.strategy = SystemPromptStrategy::IN_MESSAGES;
    }
    sys_.field = s.value("field", "system");
    sys_.role_name = s.value("role_name", "system");
    sys_.parts_field = s.value("parts_field", "parts");
    sys_.text_field = s.value("text_field", "text");

    // --- Messages ---
    auto& m = schema_["messages"];
    msgs_.role_field = m.value("role_field", "role");
    msgs_.content_field = m.value("content_field", "content");
    msgs_.content_is_parts = m.value("content_is_parts", false);
    if (m.contains("role_map") && m["role_map"].is_object()) {
        for (auto& [k, v] : m["role_map"].items()) {
            msgs_.role_map[k] = v.get<std::string>();
        }
    }
    if (m.contains("text_part")) {
        msgs_.text_part_template = m["text_part"];
    }

    // --- Tool Definition ---
    auto& td = schema_["tool_definition"];
    std::string wrapper = td.value("wrapper", "function");
    if (wrapper == "none") {
        tool_def_.wrapper = ToolDefWrapper::NONE;
    } else if (wrapper == "function_declarations") {
        tool_def_.wrapper = ToolDefWrapper::FUNCTION_DECLARATIONS;
    } else if (wrapper == "flat_function") {
        tool_def_.wrapper = ToolDefWrapper::FLAT_FUNCTION;
    } else {
        tool_def_.wrapper = ToolDefWrapper::FUNCTION;
    }
    tool_def_.name_field = td.value("name_field", "name");
    tool_def_.description_field = td.value("description_field", "description");
    tool_def_.parameters_field = td.value("parameters_field", "parameters");

    // --- Tool Call in Message ---
    auto& tc = schema_["tool_call_in_message"];
    std::string tc_strategy = tc.value("strategy", "tool_calls_array");
    if (tc_strategy == "content_array") {
        tool_call_.strategy = ToolCallStrategy::CONTENT_ARRAY;
    } else if (tc_strategy == "parts_array") {
        tool_call_.strategy = ToolCallStrategy::PARTS_ARRAY;
    } else if (tc_strategy == "flat_items") {
        tool_call_.strategy = ToolCallStrategy::FLAT_ITEMS;
    } else {
        tool_call_.strategy = ToolCallStrategy::TOOL_CALLS_ARRAY;
    }
    tool_call_.field = tc.value("field", "tool_calls");
    if (tc.contains("item")) tool_call_.item_template = tc["item"];
    if (tc.contains("text_item")) tool_call_.text_item_template = tc["text_item"];

    // --- Tool Result ---
    auto& tr = schema_["tool_result"];
    tool_result_.role = tr.value("role", "tool");
    std::string tr_strategy = tr.value("strategy", "flat");
    if (tr_strategy == "content_array") {
        tool_result_.strategy = ToolResultStrategy::CONTENT_ARRAY;
    } else if (tr_strategy == "parts_array") {
        tool_result_.strategy = ToolResultStrategy::PARTS_ARRAY;
    } else if (tr_strategy == "flat_item") {
        tool_result_.strategy = ToolResultStrategy::FLAT_ITEM;
    } else {
        tool_result_.strategy = ToolResultStrategy::FLAT;
    }
    tool_result_.id_field = tr.value("id_field", "tool_call_id");
    tool_result_.content_field = tr.value("content_field", "content");
    if (tr.contains("item")) tool_result_.item_template = tr["item"];

    // --- Image ---
    auto& img = schema_["image"];
    image_.strategy = img.value("strategy", "openai");
    if (img.contains("item")) image_.item_template = img["item"];
    if (img.contains("text_part")) image_.text_part_template = img["text_part"];

    // --- Response ---
    auto& resp = schema_["response"];
    std::string resp_strategy = resp.value("strategy", "choices_message");
    if (resp_strategy == "content_array") {
        resp_.strategy = ResponseStrategy::CONTENT_ARRAY;
    } else if (resp_strategy == "candidates_parts") {
        resp_.strategy = ResponseStrategy::CANDIDATES_PARTS;
    } else if (resp_strategy == "output_array") {
        resp_.strategy = ResponseStrategy::OUTPUT_ARRAY;
    } else {
        resp_.strategy = ResponseStrategy::CHOICES_MESSAGE;
    }
    resp_.message_path = resp.value("message_path", "");
    resp_.content_field = resp.value("content_field", "content");
    resp_.role_field = resp.value("role_field", "role");
    resp_.tool_calls_field = resp.value("tool_calls_field", "tool_calls");
    resp_.tool_call_id_field = resp.value("tool_call_id_field", "id");
    resp_.tool_call_name_path = resp.value("tool_call_name_path", "");
    resp_.tool_call_args_path = resp.value("tool_call_args_path", "");
    resp_.tool_call_args_is_string = resp.value("tool_call_args_is_string", true);
    resp_.content_path = resp.value("content_path", "content");
    resp_.text_type = resp.value("text_type", "text");
    resp_.text_field = resp.value("text_field", "text");
    resp_.tool_use_type = resp.value("tool_use_type", "tool_use");
    resp_.tool_call_name_field = resp.value("tool_call_name_field", "name");
    resp_.tool_call_args_field = resp.value("tool_call_args_field", "input");
    resp_.parts_path = resp.value("parts_path", "");
    resp_.function_call_field = resp.value("function_call_field", "functionCall");
    resp_.output_path = resp.value("output_path", "output");
    resp_.message_item_type = resp.value("message_item_type", "message");
    resp_.function_call_item_type = resp.value("function_call_item_type", "function_call");
    resp_.message_content_field = resp.value("message_content_field", "content");
    resp_.function_call_id_field = resp.value("function_call_id_field", "call_id");
    resp_.usage_path = resp.value("usage_path", "usage");
    resp_.prompt_tokens_field = resp.value("prompt_tokens_field", "prompt_tokens");
    resp_.completion_tokens_field = resp.value("completion_tokens_field", "completion_tokens");
    resp_.total_tokens_field = resp.value("total_tokens_field", "total_tokens");

    // --- Streaming ---
    auto& st = schema_["streaming"];
    std::string stream_format = st.value("format", "sse_data");
    if (stream_format == "sse_events") {
        stream_.format = StreamFormat::SSE_EVENTS;
    } else {
        stream_.format = StreamFormat::SSE_DATA;
    }
    stream_.prefix = st.value("prefix", "data: ");
    stream_.done_signal = st.value("done_signal", "[DONE]");
    stream_.delta_path = st.value("delta_path", "");
    stream_.content_field = st.value("content_field", "content");
    stream_.tool_calls_field = st.value("tool_calls_field", "tool_calls");
    stream_.tool_call_index_field = st.value("tool_call_index_field", "index");
    stream_.tool_call_id_field = st.value("tool_call_id_field", "id");
    stream_.tool_call_name_path = st.value("tool_call_name_path", "");
    stream_.tool_call_args_path = st.value("tool_call_args_path", "");
    stream_.delta_strategy = st.value("delta_strategy", "");
    stream_.delta_parts_path = st.value("parts_path", "");
    stream_.delta_text_field = st.value("text_field", "text");
    stream_.delta_function_call_field = st.value("function_call_field", "functionCall");
    stream_.delta_tool_call_name_field = st.value("tool_call_name_field", "name");
    stream_.delta_tool_call_args_field = st.value("tool_call_args_field", "args");
    if (st.contains("events")) {
        stream_.events_config = st["events"];
    }
}

// ============================================================================
// Utility Helpers
// ============================================================================

std::string SchemaProvider::get_api_key() const {
    if (!user_config_.api_key.empty()) return user_config_.api_key;
    if (!conn_.api_key_env.empty()) {
        const char* env = std::getenv(conn_.api_key_env.c_str());
        if (env) return env;
    }
    return "";
}

std::pair<std::string, std::string> SchemaProvider::parse_data_url(const std::string& url) {
    // "data:image/jpeg;base64,ABC123" -> {"image/jpeg", "ABC123"}
    if (url.rfind("data:", 0) != 0) return {"", url}; // not a data URL

    auto comma = url.find(',');
    if (comma == std::string::npos) return {"", url};

    std::string header = url.substr(5, comma - 5); // "image/jpeg;base64"
    std::string data = url.substr(comma + 1);

    auto semicolon = header.find(';');
    std::string mime = (semicolon != std::string::npos) ? header.substr(0, semicolon) : header;

    return {mime, data};
}

std::string SchemaProvider::generate_tool_call_id() {
    static std::mt19937 gen(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    static const char alphanum[] =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string id = "call_";
    std::uniform_int_distribution<int> dist(0, sizeof(alphanum) - 2);
    for (int i = 0; i < 24; ++i) {
        id += alphanum[dist(gen)];
    }
    return id;
}

json SchemaProvider::substitute(const json& tmpl, const std::map<std::string, json>& vars) {
    if (tmpl.is_string()) {
        std::string s = tmpl.get<std::string>();
        // Check if the entire string is a single $VAR placeholder
        if (s.size() >= 2 && s[0] == '$') {
            auto it = vars.find(s.substr(1));
            if (it != vars.end()) {
                return it->second;
            }
        }
        // Otherwise replace $VAR occurrences within the string (string result)
        for (const auto& [key, val] : vars) {
            std::string placeholder = "$" + key;
            auto pos = s.find(placeholder);
            while (pos != std::string::npos) {
                std::string replacement;
                if (val.is_string()) {
                    replacement = val.get<std::string>();
                } else {
                    replacement = val.dump();
                }
                s.replace(pos, placeholder.size(), replacement);
                pos = s.find(placeholder, pos + replacement.size());
            }
        }
        return s;
    }
    if (tmpl.is_object()) {
        json result = json::object();
        for (auto& [k, v] : tmpl.items()) {
            result[k] = substitute(v, vars);
        }
        return result;
    }
    if (tmpl.is_array()) {
        json result = json::array();
        for (const auto& item : tmpl) {
            result.push_back(substitute(item, vars));
        }
        return result;
    }
    return tmpl; // numbers, bools, null
}

// Parse base_url into host + path prefix
static std::pair<std::string, std::string> split_host_prefix(const std::string& base_url) {
    std::string host = base_url;
    std::string prefix;
    auto scheme_end = host.find("://");
    if (scheme_end != std::string::npos) {
        auto path_start = host.find('/', scheme_end + 3);
        if (path_start != std::string::npos) {
            prefix = host.substr(path_start);
            host = host.substr(0, path_start);
        }
    }
    return {host, prefix};
}

std::string SchemaProvider::build_endpoint(const std::string& model, bool streaming) const {
    std::string ep = streaming ? conn_.stream_endpoint : conn_.endpoint;

    // Substitute $MODEL in endpoint
    auto pos = ep.find("$MODEL");
    if (pos != std::string::npos) {
        ep.replace(pos, 6, model);
    }

    // Append auth query param if configured
    if (!conn_.auth_query_param.empty()) {
        std::string key = get_api_key();
        char sep = (ep.find('?') != std::string::npos) ? '&' : '?';
        ep += sep + conn_.auth_query_param + "=" + key;
    }

    return ep;
}

std::map<std::string, std::string> SchemaProvider::build_headers() const {
    std::map<std::string, std::string> headers;

    // Auth header (skip if empty, e.g., Gemini uses query param)
    if (!conn_.auth_header.empty()) {
        headers[conn_.auth_header] = conn_.auth_prefix + get_api_key();
    }

    // Extra headers (e.g., anthropic-version)
    for (const auto& [k, v] : conn_.extra_headers) {
        headers[k] = v;
    }

    return headers;
}

// ============================================================================
// Message Serialization
// ============================================================================

json SchemaProvider::serialize_single_message(const ChatMessage& msg) const {
    json j;

    // Map role
    std::string mapped_role = msg.role;
    auto it = msgs_.role_map.find(msg.role);
    if (it != msgs_.role_map.end()) {
        mapped_role = it->second;
    }

    j[msgs_.role_field] = mapped_role;

    // --- Tool result message ---
    if (msg.role == "tool") {
        switch (tool_result_.strategy) {
            case ToolResultStrategy::FLAT: {
                // OpenAI: {role:"tool", tool_call_id:"...", content:"..."}
                j[tool_result_.id_field] = msg.tool_call_id;
                j[tool_result_.content_field] = msg.content;
                break;
            }
            case ToolResultStrategy::CONTENT_ARRAY: {
                // Claude: {role:"user", content:[{type:"tool_result", tool_use_id:"...", content:"..."}]}
                std::map<std::string, json> vars;
                vars["ID"] = msg.tool_call_id;
                vars["CONTENT"] = msg.content;
                vars["NAME"] = msg.tool_name;
                json item = substitute(tool_result_.item_template, vars);
                j[msgs_.content_field] = json::array({item});
                break;
            }
            case ToolResultStrategy::PARTS_ARRAY: {
                // Gemini: {role:"user", parts:[{functionResponse:{name:"...", response:{result:"..."}}}]}
                std::map<std::string, json> vars;
                vars["ID"] = msg.tool_call_id;
                vars["NAME"] = msg.tool_name;
                // Try to parse content as JSON for Gemini; fall back to string
                json content_val;
                try {
                    content_val = json::parse(msg.content);
                } catch (...) {
                    content_val = msg.content;
                }
                vars["CONTENT"] = content_val;
                json item = substitute(tool_result_.item_template, vars);
                j[msgs_.content_field] = json::array({item});
                break;
            }
        }
        return j;
    }

    // --- Assistant message with tool calls ---
    if (!msg.tool_calls.empty()) {
        switch (tool_call_.strategy) {
            case ToolCallStrategy::TOOL_CALLS_ARRAY: {
                // OpenAI: content + tool_calls array
                j[msgs_.content_field] = msg.content.empty() ? json(nullptr) : json(msg.content);
                json tc_arr = json::array();
                for (const auto& tc : msg.tool_calls) {
                    std::map<std::string, json> vars;
                    vars["ID"] = tc.id;
                    vars["NAME"] = tc.name;
                    vars["ARGUMENTS_STRING"] = tc.arguments; // JSON string
                    tc_arr.push_back(substitute(tool_call_.item_template, vars));
                }
                j[tool_call_.field] = tc_arr;
                break;
            }
            case ToolCallStrategy::CONTENT_ARRAY: {
                // Claude: content is array of text + tool_use items
                json content_arr = json::array();
                if (!msg.content.empty()) {
                    std::map<std::string, json> text_vars;
                    text_vars["TEXT"] = msg.content;
                    content_arr.push_back(substitute(tool_call_.text_item_template, text_vars));
                }
                for (const auto& tc : msg.tool_calls) {
                    std::map<std::string, json> vars;
                    vars["ID"] = tc.id;
                    vars["NAME"] = tc.name;
                    // Claude wants input as object, not string
                    try {
                        vars["ARGUMENTS_OBJECT"] = json::parse(tc.arguments);
                    } catch (...) {
                        vars["ARGUMENTS_OBJECT"] = json::object();
                    }
                    content_arr.push_back(substitute(tool_call_.item_template, vars));
                }
                j[msgs_.content_field] = content_arr;
                break;
            }
            case ToolCallStrategy::PARTS_ARRAY: {
                // Gemini: parts array with text + functionCall items
                json parts = json::array();
                if (!msg.content.empty()) {
                    std::map<std::string, json> text_vars;
                    text_vars["TEXT"] = msg.content;
                    parts.push_back(substitute(tool_call_.text_item_template, text_vars));
                }
                for (const auto& tc : msg.tool_calls) {
                    std::map<std::string, json> vars;
                    vars["NAME"] = tc.name;
                    // Gemini wants args as object
                    try {
                        vars["ARGUMENTS_OBJECT"] = json::parse(tc.arguments);
                    } catch (...) {
                        vars["ARGUMENTS_OBJECT"] = json::object();
                    }
                    parts.push_back(substitute(tool_call_.item_template, vars));
                }
                j[msgs_.content_field] = parts;
                break;
            }
        }
        return j;
    }

    // --- Message with images ---
    if (!msg.image_urls.empty()) {
        json parts = json::array();
        if (!msg.content.empty()) {
            std::map<std::string, json> text_vars;
            text_vars["TEXT"] = msg.content;
            parts.push_back(substitute(image_.text_part_template, text_vars));
        }
        for (const auto& url : msg.image_urls) {
            auto [mime, data] = parse_data_url(url);
            std::map<std::string, json> vars;
            vars["DATA_URL"] = url;
            vars["MIME"] = mime;
            vars["DATA"] = data;
            parts.push_back(substitute(image_.item_template, vars));
        }
        j[msgs_.content_field] = parts;
        return j;
    }

    // --- Regular text message ---
    if (msgs_.content_is_parts) {
        // Gemini: content is parts array
        json parts = json::array();
        if (!msg.content.empty()) {
            std::map<std::string, json> text_vars;
            text_vars["TEXT"] = msg.content;
            parts.push_back(substitute(msgs_.text_part_template, text_vars));
        }
        j[msgs_.content_field] = parts;
    } else {
        j[msgs_.content_field] = msg.content;
    }

    return j;
}

json SchemaProvider::serialize_messages(const std::vector<ChatMessage>& messages) const {
    json arr = json::array();

    // Responses-style: tool calls and tool results are emitted as flat top-level items.
    // A single assistant ChatMessage with tool_calls becomes an optional text message +
    // N separate function_call items; a tool-role ChatMessage becomes a flat function_call_output.
    if (tool_call_.strategy == ToolCallStrategy::FLAT_ITEMS) {
        for (const auto& msg : messages) {
            if (msg.role == "system" && sys_.strategy != SystemPromptStrategy::IN_MESSAGES) {
                continue;
            }

            if (msg.role == "tool" && tool_result_.strategy == ToolResultStrategy::FLAT_ITEM) {
                std::map<std::string, json> vars;
                vars["ID"] = msg.tool_call_id;
                vars["CONTENT"] = msg.content;
                vars["NAME"] = msg.tool_name;
                arr.push_back(substitute(tool_result_.item_template, vars));
                continue;
            }

            if (!msg.tool_calls.empty()) {
                // Optional leading text message from the assistant.
                if (!msg.content.empty()) {
                    json text_msg;
                    std::string role = msg.role;
                    auto it = msgs_.role_map.find(role);
                    if (it != msgs_.role_map.end()) role = it->second;
                    text_msg[msgs_.role_field] = role;
                    text_msg[msgs_.content_field] = msg.content;
                    arr.push_back(text_msg);
                }
                // One flat function_call item per tool call.
                for (const auto& tc : msg.tool_calls) {
                    std::map<std::string, json> vars;
                    vars["ID"] = tc.id;
                    vars["NAME"] = tc.name;
                    vars["ARGUMENTS_STRING"] = tc.arguments;
                    try {
                        vars["ARGUMENTS_OBJECT"] = json::parse(tc.arguments);
                    } catch (...) {
                        vars["ARGUMENTS_OBJECT"] = json::object();
                    }
                    arr.push_back(substitute(tool_call_.item_template, vars));
                }
                continue;
            }

            arr.push_back(serialize_single_message(msg));
        }
        return arr;
    }

    // For Claude: consecutive same-role messages must be merged
    // Claude doesn't allow consecutive user messages; tool results become user role
    bool need_merge = (provider_name_ == "claude");

    if (!need_merge) {
        for (const auto& msg : messages) {
            // Skip system messages if strategy is not IN_MESSAGES (they're handled at top level)
            if (msg.role == "system" && sys_.strategy != SystemPromptStrategy::IN_MESSAGES) {
                continue;
            }
            arr.push_back(serialize_single_message(msg));
        }
        return arr;
    }

    // Claude merging logic: consecutive messages with same mapped role get content arrays merged
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];

        // Skip system messages (handled at top level for Claude)
        if (msg.role == "system") continue;

        json serialized = serialize_single_message(msg);
        std::string role = serialized.value(msgs_.role_field, "");

        // Check if we can merge with previous message
        if (!arr.empty()) {
            auto& prev = arr.back();
            std::string prev_role = prev.value(msgs_.role_field, "");

            if (role == prev_role) {
                // Merge: ensure both have content arrays
                json& prev_content = prev[msgs_.content_field];
                json& cur_content = serialized[msgs_.content_field];

                // Convert prev to array if it's a string
                if (prev_content.is_string()) {
                    std::string text = prev_content.get<std::string>();
                    prev_content = json::array();
                    if (!text.empty()) {
                        prev_content.push_back({{"type", "text"}, {"text", text}});
                    }
                } else if (!prev_content.is_array()) {
                    prev_content = json::array();
                }

                // Append current content
                if (cur_content.is_string()) {
                    std::string text = cur_content.get<std::string>();
                    if (!text.empty()) {
                        prev_content.push_back({{"type", "text"}, {"text", text}});
                    }
                } else if (cur_content.is_array()) {
                    for (auto& item : cur_content) {
                        prev_content.push_back(item);
                    }
                }
                continue; // merged, don't push
            }
        }

        arr.push_back(serialized);
    }

    return arr;
}

// ============================================================================
// Tool Serialization
// ============================================================================

json SchemaProvider::serialize_tools(const std::vector<ChatTool>& tools) const {
    if (tools.empty()) return json::array();

    switch (tool_def_.wrapper) {
        case ToolDefWrapper::FUNCTION: {
            // OpenAI: [{type:"function", function:{name, description, parameters}}]
            json arr = json::array();
            for (const auto& tool : tools) {
                json fn;
                fn[tool_def_.name_field] = tool.name;
                fn[tool_def_.description_field] = tool.description;
                fn[tool_def_.parameters_field] = tool.parameters;
                arr.push_back({{"type", "function"}, {"function", fn}});
            }
            return arr;
        }
        case ToolDefWrapper::NONE: {
            // Claude: [{name, description, input_schema}]
            json arr = json::array();
            for (const auto& tool : tools) {
                json item;
                item[tool_def_.name_field] = tool.name;
                item[tool_def_.description_field] = tool.description;
                item[tool_def_.parameters_field] = tool.parameters;
                arr.push_back(item);
            }
            return arr;
        }
        case ToolDefWrapper::FUNCTION_DECLARATIONS: {
            // Gemini: [{function_declarations:[{name, description, parameters}]}]
            json decls = json::array();
            for (const auto& tool : tools) {
                json item;
                item[tool_def_.name_field] = tool.name;
                item[tool_def_.description_field] = tool.description;
                item[tool_def_.parameters_field] = tool.parameters;
                decls.push_back(item);
            }
            return json::array({json{{"function_declarations", decls}}});
        }
        case ToolDefWrapper::FLAT_FUNCTION: {
            // OpenAI Responses: [{type:"function", name, description, parameters}]
            json arr = json::array();
            for (const auto& tool : tools) {
                json item;
                item["type"] = "function";
                item[tool_def_.name_field] = tool.name;
                item[tool_def_.description_field] = tool.description;
                item[tool_def_.parameters_field] = tool.parameters;
                arr.push_back(item);
            }
            return arr;
        }
    }
    return json::array();
}

// ============================================================================
// Request Body Building
// ============================================================================

json SchemaProvider::build_body(const CompletionParams& params) const {
    json body;

    std::string model = params.model.empty() ? user_config_.default_model : params.model;

    // Model field (empty string means model goes in URL, not body - e.g., Gemini)
    if (!req_.model_field.empty()) {
        body[req_.model_field] = model;
    }

    // System prompt handling
    std::vector<ChatMessage> non_system_messages;
    std::string system_content;

    for (const auto& msg : params.messages) {
        if (msg.role == "system") {
            if (system_content.empty()) {
                system_content = msg.content;
            } else {
                system_content += "\n\n" + msg.content;
            }
        } else {
            non_system_messages.push_back(msg);
        }
    }

    switch (sys_.strategy) {
        case SystemPromptStrategy::IN_MESSAGES: {
            // OpenAI: system messages stay in the messages array
            json msgs = serialize_messages(params.messages);
            body[req_.messages_field] = msgs;
            break;
        }
        case SystemPromptStrategy::TOP_LEVEL: {
            // Claude: system is a top-level string field
            if (!system_content.empty()) {
                body[sys_.field] = system_content;
            }
            json msgs = serialize_messages(non_system_messages);
            body[req_.messages_field] = msgs;
            break;
        }
        case SystemPromptStrategy::TOP_LEVEL_PARTS: {
            // Gemini: system_instruction:{parts:[{text:"..."}]}
            if (!system_content.empty()) {
                json parts = json::array();
                json part;
                part[sys_.text_field] = system_content;
                parts.push_back(part);
                json sys_obj;
                sys_obj[sys_.parts_field] = parts;
                body[sys_.field] = sys_obj;
            }
            json msgs = serialize_messages(non_system_messages);
            body[req_.messages_field] = msgs;
            break;
        }
    }

    // Tools
    if (!params.tools.empty()) {
        body[req_.tools_field] = serialize_tools(params.tools);

        // Add tool-related extra fields (like tool_choice) only when tools present
        for (auto& [k, v] : req_.extra_fields.items()) {
            body[k] = v;
        }
    }

    // Temperature
    if (params.temperature >= 0.0f) {
        json_path::set_path(body, req_.temperature_path, params.temperature);
    }

    // Max tokens
    int max_tokens = params.max_tokens;
    if (max_tokens <= 0 && req_.max_tokens_required) {
        max_tokens = req_.max_tokens_default;
    }
    if (max_tokens > 0) {
        json_path::set_path(body, req_.max_tokens_path, max_tokens);
    }

    return body;
}

// ============================================================================
// Response Parsing
// ============================================================================

ChatMessage SchemaProvider::parse_response(const json& resp_json) const {
    ChatMessage msg;
    msg.role = "assistant";

    switch (resp_.strategy) {
        case ResponseStrategy::CHOICES_MESSAGE: {
            // OpenAI: choices[0].message.{content, tool_calls}
            const json* message = json_path::at_path(resp_json, resp_.message_path);
            if (!message) {
                throw std::runtime_error("SchemaProvider: cannot find message at path: " + resp_.message_path);
            }

            msg.role = message->value(resp_.role_field, "assistant");

            if (message->contains(resp_.content_field) && !(*message)[resp_.content_field].is_null()) {
                msg.content = (*message)[resp_.content_field].get<std::string>();
            }

            if (message->contains(resp_.tool_calls_field) &&
                (*message)[resp_.tool_calls_field].is_array()) {
                for (const auto& tc : (*message)[resp_.tool_calls_field]) {
                    ToolCall call;
                    call.id = tc.value(resp_.tool_call_id_field, "");
                    const json* name_node = json_path::at_path(tc, resp_.tool_call_name_path);
                    if (name_node && name_node->is_string()) {
                        call.name = name_node->get<std::string>();
                    }
                    const json* args_node = json_path::at_path(tc, resp_.tool_call_args_path);
                    if (args_node) {
                        if (resp_.tool_call_args_is_string && args_node->is_string()) {
                            call.arguments = args_node->get<std::string>();
                        } else {
                            call.arguments = args_node->dump();
                        }
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
            }
            break;
        }
        case ResponseStrategy::CONTENT_ARRAY: {
            // Claude: content[] with text and tool_use items
            const json* content = json_path::at_path(resp_json, resp_.content_path);
            if (!content || !content->is_array()) break;

            msg.role = resp_json.value(resp_.role_field, "assistant");

            std::string full_text;
            for (const auto& block : *content) {
                std::string type = block.value("type", "");
                if (type == resp_.text_type) {
                    if (!full_text.empty()) full_text += "\n";
                    full_text += block.value(resp_.text_field, "");
                } else if (type == resp_.tool_use_type) {
                    ToolCall call;
                    call.id = block.value(resp_.tool_call_id_field, "");
                    call.name = block.value(resp_.tool_call_name_field, "");
                    if (block.contains(resp_.tool_call_args_field)) {
                        const auto& args = block[resp_.tool_call_args_field];
                        if (resp_.tool_call_args_is_string && args.is_string()) {
                            call.arguments = args.get<std::string>();
                        } else {
                            call.arguments = args.dump();
                        }
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
            }
            msg.content = full_text;
            break;
        }
        case ResponseStrategy::OUTPUT_ARRAY: {
            // OpenAI Responses: output[] with mixed item types
            //   {type:"message", role:"assistant", content:[{type:"output_text", text:"..."}]}
            //   {type:"function_call", call_id:"...", name:"...", arguments:"..."}
            //   {type:"reasoning", ...} (ignored)
            const json* output = json_path::at_path(resp_json, resp_.output_path);
            if (!output || !output->is_array()) break;

            std::string full_text;
            for (const auto& item : *output) {
                std::string type = item.value("type", "");
                if (type == resp_.message_item_type) {
                    if (item.contains(resp_.message_content_field) &&
                        item[resp_.message_content_field].is_array()) {
                        for (const auto& part : item[resp_.message_content_field]) {
                            if (part.value("type", "") == resp_.text_type) {
                                if (!full_text.empty()) full_text += "\n";
                                full_text += part.value(resp_.text_field, "");
                            }
                        }
                    }
                } else if (type == resp_.function_call_item_type) {
                    ToolCall call;
                    call.id = item.value(resp_.function_call_id_field, "");
                    call.name = item.value(resp_.tool_call_name_field, "");
                    if (item.contains(resp_.tool_call_args_field)) {
                        const auto& args = item[resp_.tool_call_args_field];
                        if (resp_.tool_call_args_is_string && args.is_string()) {
                            call.arguments = args.get<std::string>();
                        } else {
                            call.arguments = args.dump();
                        }
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
                // Other item types (reasoning, web_search_call, etc.) are ignored.
            }
            msg.content = full_text;
            break;
        }
        case ResponseStrategy::CANDIDATES_PARTS: {
            // Gemini: candidates[0].content.parts[]
            const json* parts = json_path::at_path(resp_json, resp_.parts_path);
            if (!parts || !parts->is_array()) break;

            std::string full_text;
            for (const auto& part : *parts) {
                if (part.contains(resp_.text_field) && part[resp_.text_field].is_string()) {
                    if (!full_text.empty()) full_text += "\n";
                    full_text += part[resp_.text_field].get<std::string>();
                }
                if (part.contains(resp_.function_call_field)) {
                    const auto& fc = part[resp_.function_call_field];
                    ToolCall call;
                    call.id = generate_tool_call_id(); // Gemini doesn't provide IDs
                    call.name = fc.value(resp_.tool_call_name_field, "");
                    if (fc.contains(resp_.tool_call_args_field)) {
                        const auto& args = fc[resp_.tool_call_args_field];
                        call.arguments = args.dump();
                    }
                    msg.tool_calls.push_back(std::move(call));
                }
            }
            msg.content = full_text;
            break;
        }
    }

    return msg;
}

ChatCompletion::Usage SchemaProvider::parse_usage(const json& resp_json) const {
    ChatCompletion::Usage usage;
    const json* u = json_path::at_path(resp_json, resp_.usage_path);
    if (!u) return usage;

    usage.prompt_tokens = u->value(resp_.prompt_tokens_field, 0);
    usage.completion_tokens = u->value(resp_.completion_tokens_field, 0);
    if (!resp_.total_tokens_field.empty()) {
        usage.total_tokens = u->value(resp_.total_tokens_field, 0);
    } else {
        usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
    }
    return usage;
}

// ============================================================================
// HTTP: complete()
// ============================================================================

ChatCompletion SchemaProvider::complete(const CompletionParams& params)
{
    auto body = build_body(params);
    std::string model = params.model.empty() ? user_config_.default_model : params.model;

    auto [host, prefix] = split_host_prefix(conn_.base_url);
    std::string endpoint = prefix + build_endpoint(model, false);

    httplib::Client cli(host);
    cli.set_read_timeout(user_config_.timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers;
    for (const auto& [k, v] : build_headers()) {
        headers.emplace(k, v);
    }

    auto res = cli.Post(endpoint, headers, body.dump(), "application/json");

    if (!res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
    }

    if (res->status != 200) {
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    auto resp_json = json::parse(res->body);

    ChatCompletion completion;
    completion.message = parse_response(resp_json);
    completion.usage = parse_usage(resp_json);

    return completion;
}

// ============================================================================
// HTTP: complete_stream()
// ============================================================================

ChatCompletion SchemaProvider::complete_stream(const CompletionParams& params,
                                               const StreamCallback& on_chunk)
{
    auto body = build_body(params);
    std::string model = params.model.empty() ? user_config_.default_model : params.model;

    // Add stream flag if the provider uses one
    if (!req_.stream_field.empty()) {
        body[req_.stream_field] = true;
    }

    auto [host, prefix] = split_host_prefix(conn_.base_url);
    std::string endpoint = prefix + build_endpoint(model, true);

    httplib::Client cli(host);
    cli.set_read_timeout(user_config_.timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

    httplib::Headers headers;
    for (const auto& [k, v] : build_headers()) {
        headers.emplace(k, v);
    }

    ChatCompletion completion;
    completion.message.role = "assistant";
    std::string full_content;
    std::map<int, ToolCall> tc_map;
    std::string line_buffer;

    // SSE_EVENTS: track current block/item (used by Claude and OpenAI Responses)
    struct EventBlock {
        std::string type;
        std::string id;
        std::string name;
        std::string text;
        std::string args;
        int index = -1;
    };
    std::vector<EventBlock> event_blocks;
    int event_block_index = -1;

    // For Gemini: track tool call index
    int gemini_tc_index = 0;

    // Event type tracking for SSE_EVENTS
    std::string current_event_type;

    auto res = cli.Post(
        endpoint, headers, body.dump(), "application/json",
        [&](const char* data, size_t len) -> bool {
            line_buffer.append(data, len);

            size_t pos;
            while ((pos = line_buffer.find('\n')) != std::string::npos) {
                std::string line = line_buffer.substr(0, pos);
                line_buffer.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') line.pop_back();

                // --- SSE_DATA format (OpenAI / Gemini) ---
                if (stream_.format == StreamFormat::SSE_DATA) {
                    if (line.rfind(stream_.prefix, 0) != 0) continue;
                    std::string payload = line.substr(stream_.prefix.size());

                    if (!stream_.done_signal.empty() && payload == stream_.done_signal) continue;
                    if (payload.empty()) continue;

                    try {
                        auto j = json::parse(payload);

                        if (stream_.delta_strategy == "candidates_parts") {
                            // Gemini streaming: candidates[0].content.parts[]
                            const json* parts = json_path::at_path(j, stream_.delta_parts_path);
                            if (parts && parts->is_array()) {
                                for (const auto& part : *parts) {
                                    if (part.contains(stream_.delta_text_field) &&
                                        part[stream_.delta_text_field].is_string()) {
                                        std::string token = part[stream_.delta_text_field].get<std::string>();
                                        full_content += token;
                                        if (on_chunk) on_chunk(token);
                                    }
                                    if (part.contains(stream_.delta_function_call_field)) {
                                        const auto& fc = part[stream_.delta_function_call_field];
                                        ToolCall call;
                                        call.id = generate_tool_call_id();
                                        call.name = fc.value(stream_.delta_tool_call_name_field, "");
                                        if (fc.contains(stream_.delta_tool_call_args_field)) {
                                            call.arguments = fc[stream_.delta_tool_call_args_field].dump();
                                        }
                                        tc_map[gemini_tc_index++] = call;
                                    }
                                }
                            }
                        } else {
                            // OpenAI streaming: choices[0].delta
                            const json* delta = json_path::at_path(j, stream_.delta_path);
                            if (!delta) continue;

                            // Content token
                            if (delta->contains(stream_.content_field) &&
                                !(*delta)[stream_.content_field].is_null()) {
                                std::string token = (*delta)[stream_.content_field].get<std::string>();
                                full_content += token;
                                if (on_chunk) on_chunk(token);
                            }

                            // Tool calls (streamed incrementally)
                            if (delta->contains(stream_.tool_calls_field)) {
                                for (const auto& tc : (*delta)[stream_.tool_calls_field]) {
                                    int idx = tc.value(stream_.tool_call_index_field, 0);
                                    if (tc.contains(stream_.tool_call_id_field)) {
                                        tc_map[idx].id = tc[stream_.tool_call_id_field].get<std::string>();
                                    }
                                    const json* name_node = json_path::at_path(tc, stream_.tool_call_name_path);
                                    if (name_node && name_node->is_string()) {
                                        tc_map[idx].name += name_node->get<std::string>();
                                    }
                                    const json* args_node = json_path::at_path(tc, stream_.tool_call_args_path);
                                    if (args_node && args_node->is_string()) {
                                        tc_map[idx].arguments += args_node->get<std::string>();
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        // Skip malformed chunks
                    }
                }

                // --- SSE_EVENTS format (Claude) ---
                else if (stream_.format == StreamFormat::SSE_EVENTS) {
                    // Parse "event: <type>" lines
                    if (line.rfind("event: ", 0) == 0) {
                        current_event_type = line.substr(7);
                        continue;
                    }

                    // Parse "data: <json>" lines
                    if (line.rfind("data: ", 0) != 0) continue;
                    std::string payload = line.substr(6);
                    if (payload.empty()) continue;

                    try {
                        auto j = json::parse(payload);

                        if (!stream_.events_config.contains(current_event_type)) continue;
                        auto& event_cfg = stream_.events_config[current_event_type];
                        std::string action = event_cfg.value("action", "ignore");

                        if (action == "ignore") {
                            // noop
                        }
                        else if (action == "block_start") {
                            // New content block / output item starting.
                            // Claude default: block_path="content_block", tool_type="tool_use", id in "id".
                            // Responses:      block_path="item",          tool_type="function_call", id in "call_id".
                            std::string block_path = event_cfg.value("block_path", "content_block");
                            std::string type_field = event_cfg.value("type_field", "type");
                            std::string tool_type   = event_cfg.value("tool_call_type", "tool_use");
                            std::string id_fld      = event_cfg.value("id_field", "id");
                            std::string name_fld    = event_cfg.value("name_field", "name");

                            const json* block = json_path::at_path(j, block_path);
                            if (block) {
                                EventBlock cb;
                                cb.type = block->value(type_field, "");
                                cb.index = static_cast<int>(event_blocks.size());
                                if (cb.type == tool_type) {
                                    cb.id = block->value(id_fld, "");
                                    cb.name = block->value(name_fld, "");
                                }
                                event_blocks.push_back(cb);
                                event_block_index = cb.index;
                            }
                        }
                        else if (action == "delta") {
                            // Claude-style delta: single event carries both text and tool args,
                            // discriminated by a nested "type" field inside the delta object.
                            if (event_block_index < 0 ||
                                event_block_index >= static_cast<int>(event_blocks.size())) continue;

                            auto& cur_block = event_blocks[event_block_index];
                            std::string delta_path = event_cfg.value("delta_path", "delta");
                            const json* delta = json_path::at_path(j, delta_path);
                            if (!delta) continue;

                            std::string delta_type = delta->value("type", "");
                            std::string text_delta_type = event_cfg.value("text_delta_type", "text_delta");
                            std::string tool_delta_type = event_cfg.value("tool_delta_type", "input_json_delta");

                            if (delta_type == text_delta_type) {
                                std::string text_fld = event_cfg.value("text_field", "text");
                                std::string token = delta->value(text_fld, "");
                                full_content += token;
                                cur_block.text += token;
                                if (on_chunk) on_chunk(token);
                            }
                            else if (delta_type == tool_delta_type) {
                                std::string args_fld = event_cfg.value("tool_args_field", "partial_json");
                                std::string chunk = delta->value(args_fld, "");
                                cur_block.args += chunk;
                            }
                        }
                        else if (action == "text_delta") {
                            // Responses-style: dedicated text-delta event.
                            // Reads a single field directly from the event payload.
                            if (event_block_index < 0 ||
                                event_block_index >= static_cast<int>(event_blocks.size())) continue;
                            auto& cur_block = event_blocks[event_block_index];
                            std::string delta_fld = event_cfg.value("delta_field", "delta");
                            std::string token = j.value(delta_fld, "");
                            full_content += token;
                            cur_block.text += token;
                            if (on_chunk) on_chunk(token);
                        }
                        else if (action == "tool_args_delta") {
                            // Responses-style: dedicated function-call-arguments delta event.
                            if (event_block_index < 0 ||
                                event_block_index >= static_cast<int>(event_blocks.size())) continue;
                            auto& cur_block = event_blocks[event_block_index];
                            std::string delta_fld = event_cfg.value("delta_field", "delta");
                            cur_block.args += j.value(delta_fld, "");
                        }
                        else if (action == "block_stop") {
                            // Block / item finished.
                            std::string tool_type = event_cfg.value("tool_call_type", "tool_use");
                            if (event_block_index >= 0 &&
                                event_block_index < static_cast<int>(event_blocks.size())) {
                                auto& cb = event_blocks[event_block_index];
                                if (cb.type == tool_type) {
                                    ToolCall call;
                                    call.id = cb.id;
                                    call.name = cb.name;
                                    call.arguments = cb.args;
                                    tc_map[cb.index] = call;
                                }
                            }
                            event_block_index = -1;
                        }
                        else if (action == "done") {
                            // Stream complete
                        }
                    } catch (...) {
                        // Skip malformed
                    }
                }
            }
            return true; // continue receiving
        }
    );

    if (!res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(res.error()));
    }

    if (res->status != 200) {
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    completion.message.content = full_content;
    for (auto& [idx, tc] : tc_map) {
        completion.message.tool_calls.push_back(tc);
    }

    return completion;
}

} // namespace neograph::llm
