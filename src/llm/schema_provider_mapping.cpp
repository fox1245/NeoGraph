// Schema-driven, network-free request mapping and response decoding.
#include <neograph/llm/schema_provider.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <random>
#include <stdexcept>

namespace neograph::llm {
namespace {
bool model_omits_temperature(const std::string& model) {
    return model.rfind("gpt-5", 0) == 0;
}
}

void SchemaProvider::parse_schema()
{
    provider_name_ = schema_.value("name", "unknown");

    // --- Connection ---
    auto c = schema_["connection"];
    conn_.base_url = user_config_.base_url_override.empty()
        ? c.value("base_url", "")
        : user_config_.base_url_override;
    conn_.endpoint = c.value("endpoint", "");
    conn_.stream_endpoint = c.value("stream_endpoint", conn_.endpoint);
    conn_.auth_header = user_config_.auth_header_override.empty()
        ? c.value("auth_header", "")
        : user_config_.auth_header_override;
    conn_.auth_prefix = user_config_.auth_prefix_override.empty()
        ? c.value("auth_prefix", "")
        : user_config_.auth_prefix_override;
    conn_.api_key_env = c.value("api_key_env", "");
    conn_.auth_query_param = c.value("auth_query_param", "");
    if (c.contains("extra_headers") && c["extra_headers"].is_object()) {
        for (const auto& [k, v] : c["extra_headers"].items()) {
            conn_.extra_headers[k] = v.get<std::string>();
        }
    }

    // --- Request ---
    auto r = schema_["request"];
    req_.model_field = r.value("model_field", "model");
    req_.messages_field = r.value("messages_field", "messages");
    req_.tools_field = r.value("tools_field", "tools");
    // temperature_path: schema-side opt-out via JSON null (issue #35).
    //
    // Reasoning models (OpenAI gpt-5.x, o-series) treat `temperature`
    // and `reasoning.effort` as mutually exclusive; sending both is
    // undefined behaviour. A schema for those models needs to declare
    // "this provider doesn't accept temperature" so build_body skips
    // the field entirely.
    //
    // Convention:
    //   - `"temperature_path": "temperature"`  → write at body.temperature
    //   - `"temperature_path": "some.nested.path"` → json_path::set_path
    //   - `"temperature_path": null`           → opt out (don't write at all)
    //   - field omitted entirely               → defaults to "temperature"
    //                                            (back-compat for existing
    //                                             schemas)
    //
    // Internally an empty string is the opt-out sentinel; build_body
    // checks `!req_.temperature_path.empty()` before writing.
    if (r.contains("temperature_path") && r["temperature_path"].is_null()) {
        req_.temperature_path.clear();   // opt-out
    } else {
        req_.temperature_path = r.value("temperature_path", "temperature");
    }
    req_.max_tokens_path = r.value("max_tokens_path", "max_tokens");
    req_.max_tokens_required = r.value("max_tokens_required", false);
    req_.max_tokens_default = r.value("max_tokens_default", -1);
    req_.stream_field = r.value("stream_field", "stream");
    req_.extra_fields = r.value("extra_fields", json::object());

    // Per-call body field allowlist (issue #33). Schema declares which
    // body paths a caller can override per-call via
    // CompletionParams::extra_fields. Built once at parse time so
    // build_body's per-call check is an O(log n) set lookup, not a
    // re-parse.
    if (r.contains("per_call_fields") && r["per_call_fields"].is_array()) {
        for (const auto& path : r["per_call_fields"]) {
            if (path.is_string()) req_.per_call_fields.insert(path.get<std::string>());
        }
    }
    // --- System Prompt ---
    auto s = schema_["system_prompt"];

    // Strategy names are resolved through the explicitly injected registry.
    // The resolved value is always one of the reviewed built-in primitives;
    // the rest of SchemaProvider can therefore keep its compact enum dispatch.
    const auto system_prompt_strategy =
        strategy_registry_.resolve(SchemaStrategyFamily::SystemPrompt,
                                   s.value("strategy", "in_messages"));
    if (system_prompt_strategy == "top_level") {
        sys_.strategy = SystemPromptStrategy::TOP_LEVEL;
    } else if (system_prompt_strategy == "top_level_parts") {
        sys_.strategy = SystemPromptStrategy::TOP_LEVEL_PARTS;
    } else {
        sys_.strategy = SystemPromptStrategy::IN_MESSAGES;
    }
    sys_.field = s.value("field", "system");
    sys_.role_name = s.value("role_name", "system");
    sys_.parts_field = s.value("parts_field", "parts");
    sys_.text_field = s.value("text_field", "text");

    // --- Messages ---
    auto m = schema_["messages"];
    msgs_.role_field = m.value("role_field", "role");
    msgs_.content_field = m.value("content_field", "content");
    msgs_.content_is_parts = m.value("content_is_parts", false);
    if (m.contains("role_map") && m["role_map"].is_object()) {
        for (const auto& [k, v] : m["role_map"].items()) {
            msgs_.role_map[k] = v.get<std::string>();
        }
    }
    if (m.contains("text_part")) {
        msgs_.text_part_template = m["text_part"];
    }
    // --- Tool Definition ---
    auto td = schema_["tool_definition"];

    // Tool definition wrappers share the same value-level registry contract.
    const auto tool_definition_strategy =
        strategy_registry_.resolve(SchemaStrategyFamily::ToolDefinition,
                                   td.value("wrapper", "function"));
    if (tool_definition_strategy == "none") {
        tool_def_.wrapper = ToolDefWrapper::NONE;
    } else if (tool_definition_strategy == "function_declarations") {
        tool_def_.wrapper = ToolDefWrapper::FUNCTION_DECLARATIONS;
    } else if (tool_definition_strategy == "flat_function") {
        tool_def_.wrapper = ToolDefWrapper::FLAT_FUNCTION;
    } else {
        tool_def_.wrapper = ToolDefWrapper::FUNCTION;
    }
    tool_def_.name_field = td.value("name_field", "name");
    tool_def_.description_field = td.value("description_field", "description");
    tool_def_.parameters_field = td.value("parameters_field", "parameters");
    // --- Tool Call in Message ---
    auto tc = schema_["tool_call_in_message"];

    // Tool-call and tool-result encodings may be named independently.
    const auto tool_call_strategy =
        strategy_registry_.resolve(SchemaStrategyFamily::ToolCall,
                                   tc.value("strategy", "tool_calls_array"));
    if (tool_call_strategy == "content_array") {
        tool_call_.strategy = ToolCallStrategy::CONTENT_ARRAY;
    } else if (tool_call_strategy == "parts_array") {
        tool_call_.strategy = ToolCallStrategy::PARTS_ARRAY;
    } else if (tool_call_strategy == "flat_items") {
        tool_call_.strategy = ToolCallStrategy::FLAT_ITEMS;
    } else {
        tool_call_.strategy = ToolCallStrategy::TOOL_CALLS_ARRAY;
    }
    tool_call_.field = tc.value("field", "tool_calls");
    if (tc.contains("item")) tool_call_.item_template = tc["item"];
    if (tc.contains("text_item")) tool_call_.text_item_template = tc["text_item"];
    // --- Tool Result ---
    auto tr = schema_["tool_result"];

    // Tool result encodings may be named independently.
    const auto tool_result_strategy =
        strategy_registry_.resolve(SchemaStrategyFamily::ToolResult,
                                   tr.value("strategy", "flat"));
    if (tool_result_strategy == "content_array") {
        tool_result_.strategy = ToolResultStrategy::CONTENT_ARRAY;
    } else if (tool_result_strategy == "parts_array") {
        tool_result_.strategy = ToolResultStrategy::PARTS_ARRAY;
    } else if (tool_result_strategy == "flat_item") {
        tool_result_.strategy = ToolResultStrategy::FLAT_ITEM;
    } else {
        tool_result_.strategy = ToolResultStrategy::FLAT;
    }
    tool_result_.id_field = tr.value("id_field", "tool_call_id");
    tool_result_.content_field = tr.value("content_field", "content");
    if (tr.contains("item")) tool_result_.item_template = tr["item"];

    // --- Image ---
    auto img = schema_["image"];
    image_.strategy = img.value("strategy", "openai");
    if (img.contains("item")) image_.item_template = img["item"];
    if (img.contains("text_part")) image_.text_part_template = img["text_part"];
    if (img.contains("url_item")) image_.url_item_template = img["url_item"];
    image_.remote_url_supported = img.value("remote_url_supported", false);

    // --- Response ---
    auto resp = schema_["response"];
    const auto response_strategy =
        strategy_registry_.resolve(SchemaStrategyFamily::Response,
                                   resp.value("strategy", "choices_message"));
    if (response_strategy == "content_array") {
        resp_.strategy = ResponseStrategy::CONTENT_ARRAY;
    } else if (response_strategy == "candidates_parts") {
        resp_.strategy = ResponseStrategy::CANDIDATES_PARTS;
    } else if (response_strategy == "output_array") {
        resp_.strategy = ResponseStrategy::OUTPUT_ARRAY;
    } else {
        resp_.strategy = ResponseStrategy::CHOICES_MESSAGE;
    }
    resp_.message_path = resp.value("message_path", "");
    resp_.content_field = resp.value("content_field", "content");
    // Keep the legacy aliases when an external schema omits this field.
    resp_.reasoning_fields = resp.value("reasoning_fields",
        std::vector<std::string>{"reasoning", "reasoning_content"});
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
    resp_.stop_reason_path = resp.value("stop_reason_path", "");
    if (resp.contains("stop_reason_map") && resp["stop_reason_map"].is_object()) {
        for (const auto& [raw, normalized] : resp["stop_reason_map"].items()) {
            if (normalized.is_string()) {
                resp_.stop_reason_map[raw] = normalized.get<std::string>();
            }
        }
    }
    resp_.stop_reason_status_path = resp.value("stop_reason_status_path", "");
    if (resp.contains("stop_reason_status_map") && resp["stop_reason_status_map"].is_object()) {
        for (const auto& [raw, normalized] : resp["stop_reason_status_map"].items()) {
            if (normalized.is_string()) {
                resp_.stop_reason_status_map[raw] = normalized.get<std::string>();
            }
        }
    }
    resp_.default_stop_reason = resp.value("default_stop_reason", "unknown");

    // --- Streaming ---
    auto st = schema_["streaming"];
    const auto stream_format =
        strategy_registry_.resolve(SchemaStrategyFamily::Stream,
                                   st.value("format", "sse_data"));
    if (stream_format == "sse_events") {
        stream_.format = StreamFormat::SSE_EVENTS;
    } else {
        stream_.format = StreamFormat::SSE_DATA;
    }
    stream_.prefix = st.value("prefix", "data: ");
    stream_.done_signal = st.value("done_signal", "[DONE]");
    stream_.delta_path = st.value("delta_path", "");
    stream_.content_field = st.value("content_field", "content");
    stream_.reasoning_fields = st.value("reasoning_fields",
        std::vector<std::string>{"reasoning_content"});
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
    stream_.stop_reason_path = st.value("stop_reason_path", "");
    stream_.stop_reason_status_path = st.value("stop_reason_status_path", "");
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
    // thread_local so concurrent fan-out calls on Asio worker threads don't
    // race on the PRNG state.
    thread_local std::mt19937 gen(static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count()
        ^ reinterpret_cast<std::uintptr_t>(&gen)));
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
        for (const auto& [k, v] : tmpl.items()) {
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

std::string SchemaProvider::build_endpoint(const std::string& model,
                                           bool streaming,
                                           std::string_view api_key) const {
    std::string ep = streaming ? conn_.stream_endpoint : conn_.endpoint;

    // Substitute $MODEL in endpoint
    auto pos = ep.find("$MODEL");
    if (pos != std::string::npos) {
        ep.replace(pos, 6, model);
    }

    // Append auth query param if configured
    if (!conn_.auth_query_param.empty()) {
        char sep = (ep.find('?') != std::string::npos) ? '&' : '?';
        ep += sep + conn_.auth_query_param + "=" + std::string(api_key);
    }

    return ep;
}

std::map<std::string, std::string> SchemaProvider::build_headers(
    std::string_view api_key) const {
    std::map<std::string, std::string> headers;

    // Auth header (skip if empty, e.g., Gemini uses query param)
    if (!conn_.auth_header.empty()) {
        headers[conn_.auth_header] = conn_.auth_prefix + std::string(api_key);
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
            std::map<std::string, json> vars;
            vars["DATA_URL"] = url;

            const bool is_data_url = url.rfind("data:", 0) == 0;
            if (!is_data_url && !image_.url_item_template.is_null()) {
                parts.push_back(substitute(image_.url_item_template, vars));
                continue;
            }
            if (!is_data_url && !image_.remote_url_supported) {
                throw std::invalid_argument(
                    "SchemaProvider: image schema does not declare a remote URL representation");
            }

            auto [mime, data] = parse_data_url(url);
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
            auto prev = arr[arr.size() - 1];
            std::string prev_role = prev.value(msgs_.role_field, "");

            if (role == prev_role) {
                // Merge: ensure both have content arrays
                auto prev_content = prev[msgs_.content_field];
                auto cur_content = serialized[msgs_.content_field];

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
                    for (const auto& item : cur_content) {
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

json SchemaProvider::build_body(const CompletionParams& params, bool websocket) const {
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

    // Tools — only stamp the tools field when the caller actually passed
    // tools. Empty tools means "this call doesn't expose any to the
    // model"; sending an empty array silently changes some providers'
    // behaviour (especially OpenAI Responses), so omit it.
    if (!params.tools.empty()) {
        body[req_.tools_field] = serialize_tools(params.tools);
    }

    // Schema-declared static body fields (issue #34).
    //
    // These are vendor-specific fields a schema author wants stamped
    // into every request body — not just when tools are present. The
    // pre-#34 code accidentally gated this on `params.tools.empty()`
    // because the original use case was `tool_choice` (tool-specific).
    // But schemas declare reasoning-grade fields here too:
    // `reasoning: {effort: "medium"}`, `response_format`, vendor knobs.
    // Gating on tools silently dropped all of them whenever the caller
    // didn't pass tools — observable only by inspecting the outgoing
    // HTTP body. Now applied unconditionally.
    //
    // Tool-specific keys (`tool_choice`) ARE harmless to send without
    // tools on most providers, but OpenAI rejects that request shape.
    // Keep this one field gated on tool presence while applying all
    // other schema-declared fields unconditionally.
    for (const auto& [k, v] : req_.extra_fields.items()) {
        if (k == "tool_choice" && params.tools.empty()) continue;
        body[k] = v;
    }

    // OpenRouter provider routing is a top-level object. Keep it as a
    // schema-independent configuration default; schemas may additionally
    // expose `provider` in `per_call_fields` for a request-local override.
    // Source: https://openrouter.ai/docs/guides/routing/provider-selection
    // (verified 2026-08-08).
    if (!user_config_.provider_routing.is_null()) {
        if (!user_config_.provider_routing.is_object()) {
            throw std::invalid_argument(
                "SchemaProvider Config::provider_routing must be an object");
        }
        body["provider"] = user_config_.provider_routing;
    }
    // Per-call body field bindings (issue #33).
    //
    // The schema declares which paths a caller can override per-call
    // via `"request.per_call_fields": [path1, path2, ...]`. Caller
    // passes the values in `params.extra_fields` as a `path -> value`
    // map. Build_body applies them AFTER the schema-static
    // extra_fields above, so a per-call value wins when both bind the
    // same key — which is what the caller wants ("I'm overriding the
    // default for THIS call").
    //
    // Path is a json_path expression (dot-separated, like
    // `temperature_path` / `max_tokens_path`), so caller can target
    // nested structure (`reasoning.effort`, `thinking.budget_tokens`).
    //
    // Unknown paths (not in `req_.per_call_fields`) are silently
    // dropped. The schema, not the caller, owns the contract — same
    // discipline as `temperature_path` and `max_tokens_path`. This
    // also means a typo in caller code (`reasonin.effort`) silently
    // does nothing instead of stamping a malformed key, which is
    // safer for production.
    if (params.extra_fields.is_object()) {
        for (const auto& [path, value] : params.extra_fields.items()) {
            if (req_.per_call_fields.count(path)) {
                json_path::set_path(body, path, value);
            }
        }
    }

    // Temperature.
    //
    // Two opt-out paths combined:
    //   - Per-call: caller passes `params.temperature < 0` (sentinel).
    //   - Per-schema: schema declares `"temperature_path": null`, which
    //     leaves `req_.temperature_path` empty (issue #35).
    // Either disables the write. Reasoning-model schemas typically use
    // the per-schema path so node code doesn't have to negate
    // `params.temperature` at every call site.
    if (!websocket && params.temperature >= 0.0f &&
        !req_.temperature_path.empty() && !model_omits_temperature(model)) {
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

json SchemaProvider::build_sse_body(const CompletionParams& params) const {
    auto body = build_body(params);
    if (!req_.stream_field.empty()) {
        body[req_.stream_field] = true;
    }
    // OpenAI-compatible streaming requires explicit usage opt-in.
    // Claude event streams and Gemini candidates report usage already.
    if (stream_.format == StreamFormat::SSE_DATA &&
        stream_.delta_strategy != "candidates_parts") {
        body["stream_options"] = {{"include_usage", true}};
    }
    return body;
}

json SchemaProvider::build_ws_body(const CompletionParams& params) const {
    // Responses WebSocket mode closes silently when `temperature` is sent.
    // The socket itself represents streaming; omit HTTP transport fields.
    auto body = build_body(params, true);
    body["type"] = "response.create";
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
            // OpenAI-compatible: choices[0].message.{content, reasoning_content, tool_calls}
            auto message = json_path::at_path(resp_json, resp_.message_path);
            if (!message) {
                throw std::runtime_error("SchemaProvider: cannot find message at path: " + resp_.message_path);
            }

            msg.role = message->value(resp_.role_field, "assistant");

            if (message->contains(resp_.content_field) && !(*message)[resp_.content_field].is_null()) {
                msg.content = (*message)[resp_.content_field].get<std::string>();
            }

            // Keep provider reasoning separate from user-visible content. GLM
            // sends reasoning_content alongside tool calls, and the host may
            // require that rationale before allowing the Tool action.
            for (const auto& field : resp_.reasoning_fields) {
                if (message->contains(field) && (*message)[field].is_string()) {
                    msg.reasoning = (*message)[field].get<std::string>();
                    break;
                }
            }

            if (message->contains(resp_.tool_calls_field) &&
                (*message)[resp_.tool_calls_field].is_array()) {
                for (const auto& tc : (*message)[resp_.tool_calls_field]) {
                    ToolCall call;
                    call.id = tc.value(resp_.tool_call_id_field, "");
                    auto name_node = json_path::at_path(tc, resp_.tool_call_name_path);
                    if (name_node && name_node->is_string()) {
                        call.name = name_node->get<std::string>();
                    }
                    auto args_node = json_path::at_path(tc, resp_.tool_call_args_path);
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
            auto content = json_path::at_path(resp_json, resp_.content_path);
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
            auto output = json_path::at_path(resp_json, resp_.output_path);
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
            auto parts = json_path::at_path(resp_json, resp_.parts_path);
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
    auto u = json_path::at_path(resp_json, resp_.usage_path);
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

std::string SchemaProvider::parse_stop_reason(const json& resp_json) const {
    auto map_reason = [](const json& value,
                         const std::map<std::string, std::string>& mapping,
                         bool unknown_when_unmapped) {
        if (!value.is_string()) return std::string{};
        const auto raw = value.get<std::string>();
        const auto it = mapping.find(raw);
        if (it != mapping.end()) return it->second;
        return unknown_when_unmapped ? std::string("unknown") : std::string{};
    };

    if (!resp_.stop_reason_path.empty()) {
        if (const auto value = json_path::at_path(resp_json, resp_.stop_reason_path)) {
            if (const auto mapped = map_reason(*value, resp_.stop_reason_map, true); !mapped.empty()) {
                return mapped;
            }
        }
    }
    if (!resp_.stop_reason_status_path.empty()) {
        if (const auto value = json_path::at_path(resp_json, resp_.stop_reason_status_path)) {
            return map_reason(*value, resp_.stop_reason_status_map, false);
        }
    }
    return {};
}

std::string SchemaProvider::parse_stream_stop_reason(const json& event_json) const {
    auto map_reason = [](const json& value,
                         const std::map<std::string, std::string>& mapping,
                         bool unknown_when_unmapped) {
        if (!value.is_string()) return std::string{};
        const auto raw = value.get<std::string>();
        const auto it = mapping.find(raw);
        if (it != mapping.end()) return it->second;
        return unknown_when_unmapped ? std::string("unknown") : std::string{};
    };

    if (!stream_.stop_reason_path.empty()) {
        if (const auto value = json_path::at_path(event_json, stream_.stop_reason_path)) {
            if (const auto mapped = map_reason(*value, resp_.stop_reason_map, true); !mapped.empty()) {
                return mapped;
            }
        }
    }
    if (!stream_.stop_reason_status_path.empty()) {
        if (const auto value = json_path::at_path(event_json, stream_.stop_reason_status_path)) {
            return map_reason(*value, resp_.stop_reason_status_map, false);
        }
    }
    return {};
}

} // namespace neograph::llm
