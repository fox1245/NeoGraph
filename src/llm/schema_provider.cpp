#include <neograph/llm/schema_provider.h>
#include <neograph/async/conn_pool.h>
#include <neograph/async/curl_h2_pool.h>
#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/async/ws_client.h>
#include <builtin_schemas.h>

#include <asio/co_spawn.hpp>
#include <asio/dispatch.hpp>
#include <asio/io_context.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/this_coro.hpp>

#include <charconv>
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

    // Stand up the long-lived HTTP loop + ConnPool. The pool is bound
    // to http_io_'s executor, so it survives the run_sync io_context
    // that drives any individual Provider::complete() call. Sync and
    // async completers both dispatch their HTTP work onto this loop;
    // successive calls to the same model host now amortise TCP+TLS.
    http_io_ = std::make_unique<asio::io_context>();
    http_work_.emplace(asio::make_work_guard(*http_io_));
    http_thread_ = std::thread([io = http_io_.get()]{ io->run(); });
    conn_pool_ = std::make_unique<async::ConnPool>(http_io_->get_executor());
    if (user_config_.prefer_libcurl) {
        curl_pool_ = std::make_unique<async::CurlH2Pool>();
    }

    // Long-lived sync-bridge thread for streaming HTTP/SSE (issue #16).
    // See header comment on `bridge_io_` for the rationale.
    bridge_io_ = std::make_unique<asio::io_context>();
    bridge_work_.emplace(asio::make_work_guard(*bridge_io_));
    bridge_thread_ = std::thread([io = bridge_io_.get()]{ io->run(); });
}

SchemaProvider::~SchemaProvider()
{
    // Order: drop the work guards so each io_context.run() can return,
    // stop the io_contexts (cancels in-flight ops), then join the
    // worker threads.
    if (http_work_) http_work_.reset();
    if (bridge_work_) bridge_work_.reset();
    if (http_io_) http_io_->stop();
    if (bridge_io_) bridge_io_->stop();
    if (http_thread_.joinable()) http_thread_.join();
    if (bridge_thread_.joinable()) bridge_thread_.join();
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
    auto c = schema_["connection"];
    conn_.base_url = user_config_.base_url_override.empty()
        ? c.value("base_url", "")
        : user_config_.base_url_override;
    conn_.endpoint = c.value("endpoint", "");
    conn_.stream_endpoint = c.value("stream_endpoint", conn_.endpoint);
    conn_.auth_header = c.value("auth_header", "");
    conn_.auth_prefix = c.value("auth_prefix", "");
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

    // --- System Prompt ---
    auto s = schema_["system_prompt"];
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
    auto tc = schema_["tool_call_in_message"];
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
    auto tr = schema_["tool_result"];
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
    auto img = schema_["image"];
    image_.strategy = img.value("strategy", "openai");
    if (img.contains("item")) image_.item_template = img["item"];
    if (img.contains("text_part")) image_.text_part_template = img["text_part"];

    // --- Response ---
    auto resp = schema_["response"];
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
    auto st = schema_["streaming"];
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
    // thread_local so concurrent fan-out calls (parallel Sends calling
    // complete() from Taskflow workers) don't race on the PRNG state.
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

// Extract the `Retry-After` header value from an httplib response, in
// seconds. Accepts either the numeric-seconds form ("30") or falls back
// to the `x-ratelimit-reset` / `anthropic-ratelimit-*` families. Returns
// -1 when no usable header is present.
//
// HTTP-date form (RFC 7231) is intentionally NOT parsed here — Anthropic
// emits the numeric-seconds form in practice, and HTTP-date parsing
// would drag locale-sensitive strptime into hot path. If you hit a
// server that only emits HTTP-date, add that branch.
static int retry_after_seconds(const httplib::Result& res) {
    if (!res) return -1;
    auto try_int = [](const std::string& v) -> int {
        if (v.empty()) return -1;
        try {
            long n = std::stol(v);
            if (n < 0) return -1;
            if (n > 600) return 600;  // clamp absurd values
            return static_cast<int>(n);
        } catch (...) { return -1; }
    };
    if (res->has_header("Retry-After")) {
        int s = try_int(res->get_header_value("Retry-After"));
        if (s >= 0) return s;
    }
    if (res->has_header("retry-after")) {
        int s = try_int(res->get_header_value("retry-after"));
        if (s >= 0) return s;
    }
    // Anthropic also emits `anthropic-ratelimit-input-tokens-reset` as
    // an ISO-8601 timestamp — skip that path for now; the standard
    // Retry-After header is always present on 429 per their docs.
    return -1;
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

// Parse Retry-After (seconds-integer shape only, matching the
// httplib-based retry_after_seconds() above). Returns -1 when missing
// or unparsable; clamps absurd values at 600s like the sync path.
static int parse_retry_after_string(std::string_view raw) {
    if (raw.empty()) return -1;
    int seconds = 0;
    auto begin = raw.data();
    auto end = raw.data() + raw.size();
    auto [ptr, ec] = std::from_chars(begin, end, seconds);
    if (ec != std::errc{} || ptr != end || seconds < 0) return -1;
    if (seconds > 600) return 600;
    return seconds;
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
    // tools — most providers ignore them when there's nothing to
    // choose from. Schema authors who want a key gated on tool
    // presence can move it to a per-call binding once that surface
    // exists (issue #33).
    for (const auto& [k, v] : req_.extra_fields.items()) {
        body[k] = v;
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
    if (params.temperature >= 0.0f && !req_.temperature_path.empty()) {
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
            auto message = json_path::at_path(resp_json, resp_.message_path);
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

// ============================================================================
// HTTP: complete()
// ============================================================================

asio::awaitable<ChatCompletion>
SchemaProvider::complete_async(const CompletionParams& params)
{
    // Build the request body under the schema lock so concurrent callers
    // don't race on shared yyjson_mut_doc templates. HTTP is issued OUTSIDE
    // the lock so parallel fan-out still overlaps on the wire.
    std::string body_str;
    std::string endpoint_path;
    std::vector<std::pair<std::string, std::string>> headers;
    async::AsyncEndpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        auto body = build_body(params);
        body_str = body.dump();
        std::string model = params.model.empty() ? user_config_.default_model : params.model;
        endpoint = async::split_async_endpoint(conn_.base_url);
        endpoint_path = endpoint.prefix + build_endpoint(model, false);
        for (const auto& [k, v] : build_headers()) {
            headers.emplace_back(k, v);
        }
        // async_post computes Content-Length from body but does not
        // default Content-Type; httplib used to set it for us.
        bool has_ct = false;
        for (const auto& [k, _] : headers) {
            if (k == "Content-Type" || k == "content-type") { has_ct = true; break; }
        }
        if (!has_ct) headers.emplace_back("Content-Type", "application/json");
    }

    async::RequestOptions opts;
    if (user_config_.timeout_seconds > 0) {
        opts.timeout = std::chrono::seconds(user_config_.timeout_seconds);
    }

    // Two transports — pick at construction time via Config:
    //   * default (ConnPool, HTTP/1.1 keep-alive): fewer hops over
    //     asio, lower per-call constant cost. Best for the typical
    //     1-3 concurrent inflight pattern.
    //   * libcurl (HTTP/2 + multiplexing + Cloudflare-friendly): for
    //     wide-fan-out workloads against WAF-protected endpoints.
    async::HttpResponse res;
    if (curl_pool_) {
        std::string url = (endpoint.tls ? "https://" : "http://") + endpoint.host
                        + (endpoint.port == "443" || endpoint.port == "80"
                            ? "" : ":" + endpoint.port)
                        + endpoint_path;
        res = co_await curl_pool_->async_post(
            std::move(url),
            body_str,
            std::move(headers),
            opts);
    } else {
        res = co_await conn_pool_->async_post(
            endpoint.host,
            endpoint.port,
            endpoint_path,
            body_str,
            std::move(headers),
            endpoint.tls,
            opts);
    }

    if (res.status != 200) {
        // Surface 429s as a typed exception so callers (typically a
        // RateLimitedProvider decorator) can honour Retry-After without
        // parsing strings. SchemaProvider itself stays policy-free: it
        // neither sleeps nor retries — that's a separate concern.
        if (res.status == 429) {
            throw RateLimitError(
                "API error (HTTP 429): " + res.body,
                parse_retry_after_string(res.retry_after));
        }
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res.status) + "): " + res.body);
    }

    auto resp_json = json::parse(res.body);

    // parse_response / parse_usage read config strings + walk the freshly
    // parsed resp_json (thread-local). Still holding the lock is cheapest
    // correctness — they don't touch schema_ templates but they do read
    // resp_.*_field members (std::string) which are safe; lock kept for
    // symmetry + to guard against any future edits that introduce template
    // substitution during response parse.
    ChatCompletion completion;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        completion.message = parse_response(resp_json);
        completion.usage = parse_usage(resp_json);
    }

    co_return completion;
}

// ============================================================================
// HTTP: complete_stream()
// ============================================================================

ChatCompletion SchemaProvider::complete_stream(const CompletionParams& params,
                                               const StreamCallback& on_chunk)
{
    // WebSocket mode dispatch — only the openai-responses schema is
    // supported (that's the one OpenAI's WS endpoint speaks). Other
    // providers fall through to the HTTP/SSE path below.
    if (user_config_.use_websocket) {
        if (provider_name_ != "openai-responses") {
            throw std::runtime_error(
                "SchemaProvider: use_websocket is only supported for "
                "the openai-responses schema (got: " + provider_name_ + ")");
        }
        return async::run_sync(complete_stream_ws_responses(params, on_chunk));
    }

    // See complete() for the locking rationale. Same pattern: build the
    // request under the schema lock, issue the streaming HTTP call outside.
    std::string body_str;
    std::string endpoint;
    httplib::Headers headers;
    std::string host, prefix;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        auto body = build_body(params);
        if (!req_.stream_field.empty()) {
            body[req_.stream_field] = true;
        }
        // OpenAI/Chat-completions-compatible endpoints omit `usage` from
        // streamed responses unless this opt-in is set. Other providers
        // (Claude via SSE_EVENTS, Gemini via usageMetadata on every chunk)
        // emit usage unconditionally, so the flag is a no-op for them —
        // worst case the server ignores an unknown field.
        if (stream_.format == StreamFormat::SSE_DATA &&
            stream_.delta_strategy != "candidates_parts") {
            body["stream_options"] = {{"include_usage", true}};
        }
        body_str = body.dump();
        std::string model = params.model.empty() ? user_config_.default_model : params.model;
        std::tie(host, prefix) = split_host_prefix(conn_.base_url);
        endpoint = prefix + build_endpoint(model, true);
        for (const auto& [k, v] : build_headers()) {
            headers.emplace(k, v);
        }
    }

    httplib::Client cli(host);
    cli.set_read_timeout(user_config_.timeout_seconds, 0);
    cli.set_connection_timeout(10, 0);

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
        endpoint, headers, body_str, "application/json",
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

                        // Usage capture: OpenAI-compatible endpoints emit
                        // `usage` on the FINAL chunk (with empty choices,
                        // enabled by include_usage). Gemini emits
                        // `usageMetadata` on EVERY chunk. Either way,
                        // overwrite-on-seen gives us the latest numbers.
                        if (!resp_.usage_path.empty()) {
                            auto u = json_path::at_path(j, resp_.usage_path);
                            if (u && u->is_object()) {
                                int p = u->value(resp_.prompt_tokens_field, 0);
                                int c = u->value(resp_.completion_tokens_field, 0);
                                if (p > 0) completion.usage.prompt_tokens = p;
                                if (c > 0) completion.usage.completion_tokens = c;
                                if (!resp_.total_tokens_field.empty()) {
                                    int t = u->value(resp_.total_tokens_field, 0);
                                    if (t > 0) completion.usage.total_tokens = t;
                                }
                            }
                        }

                        if (stream_.delta_strategy == "candidates_parts") {
                            // Gemini streaming: candidates[0].content.parts[]
                            auto parts = json_path::at_path(j, stream_.delta_parts_path);
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
                            auto delta = json_path::at_path(j, stream_.delta_path);
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
                                    auto name_node = json_path::at_path(tc, stream_.tool_call_name_path);
                                    if (name_node && name_node->is_string()) {
                                        tc_map[idx].name += name_node->get<std::string>();
                                    }
                                    auto args_node = json_path::at_path(tc, stream_.tool_call_args_path);
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
                        auto event_cfg = stream_.events_config[current_event_type];
                        std::string action = event_cfg.value("action", "ignore");

                        if (action == "ignore") {
                            // noop
                        }
                        else if (action == "usage") {
                            // SSE event dedicated to emitting usage numbers.
                            // Schema declares `prompt_path` / `completion_path`
                            // / `total_path` relative to the event's JSON
                            // payload; any that resolves to a non-zero integer
                            // overwrites the cumulative usage.
                            auto read_int = [&](const std::string& p) -> int {
                                if (p.empty()) return 0;
                                auto v = json_path::at_path(j, p);
                                if (!v || !v->is_number_integer()) return 0;
                                return v->template get<int>();
                            };
                            int p = read_int(event_cfg.value("prompt_path", ""));
                            int c = read_int(event_cfg.value("completion_path", ""));
                            int t = read_int(event_cfg.value("total_path", ""));
                            if (p > 0) completion.usage.prompt_tokens = p;
                            if (c > 0) completion.usage.completion_tokens = c;
                            if (t > 0) completion.usage.total_tokens = t;
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

                            auto block = json_path::at_path(j, block_path);
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
                            auto delta = json_path::at_path(j, delta_path);
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
                            // OpenAI Responses' `response.completed` event
                            // carries the full response object (including
                            // `usage`) under the "response" key. Reuse
                            // parse_usage so the streaming and non-stream
                            // paths report identical numbers — without this,
                            // every streamed call returned usage={0,0,0}
                            // even though the server sent input/output
                            // token counts on the final event.
                            if (j.contains("response") &&
                                j["response"].is_object()) {
                                completion.usage =
                                    parse_usage(j["response"]);
                            }
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
        if (res->status == 429) {
            throw RateLimitError(
                "API error (HTTP 429): " + res->body,
                retry_after_seconds(res));
        }
        throw std::runtime_error(
            "API error (HTTP " + std::to_string(res->status) + "): " + res->body);
    }

    completion.message.content = full_content;
    for (auto& [idx, tc] : tc_map) {
        completion.message.tool_calls.push_back(tc);
    }

    return completion;
}

// ============================================================================
// Async streaming bridge — native override (issue #4)
// ============================================================================

asio::awaitable<ChatCompletion>
SchemaProvider::complete_stream_async(const CompletionParams& params,
                                      const StreamCallback& on_chunk)
{
    // Native fast path for the WebSocket Responses transport: it's
    // already an async-native co_await, so we drop the bridge thread
    // + nested run_sync entirely. Fixes issue #4 for the WS branch.
    if (user_config_.use_websocket && provider_name_ == "openai-responses") {
        co_return co_await complete_stream_ws_responses(params, on_chunk);
    }

    // HTTP/SSE branch (issue #16): dispatch the synchronous
    // `complete_stream` work onto our long-lived `bridge_thread_`
    // instead of letting Provider::complete_stream_async's base
    // default spawn a fresh `std::thread` per call.
    //
    // Why: a fresh thread starts with cold thread-local state in
    // glibc's resolver / NSS plugins / OpenSSL. The first
    // `getaddrinfo` on that thread can SEGV on `internal_strlen` when
    // the cold-init path races with the spawn pattern (observed on
    // some downstream Linux + glibc combinations under nested HTTP
    // server contexts; see #16). Routing through `bridge_thread_`
    // matches the working `complete_async` shape (which lives on
    // `http_thread_`): the thread is warm after the first call, all
    // subsequent calls reuse the warmed state.
    //
    // Tokens are still dispatched onto the awaiter's executor (so the
    // user's `on_chunk` runs single-threaded with the awaiting
    // coroutine — same invariant the post-PR-#10 base default
    // provides).
    auto exec = co_await asio::this_coro::executor;
    auto bridge_exec = bridge_io_->get_executor();

    struct Shared {
        std::optional<ChatCompletion> result;
        std::exception_ptr err;
    };
    auto shared = std::make_shared<Shared>();

    auto done = std::make_shared<asio::steady_timer>(exec);
    done->expires_at(std::chrono::steady_clock::time_point::max());

    StreamCallback wrapped = [exec, on_chunk](const std::string& chunk) {
        asio::dispatch(exec, [on_chunk, chunk]() { on_chunk(chunk); });
    };

    // params copied by value so the bridge thread's work item doesn't
    // outlive the caller's stack-allocated CompletionParams.
    asio::dispatch(bridge_exec,
        [this, params, wrapped, shared, done, exec]() mutable {
            try {
                shared->result = this->complete_stream(params, wrapped);
            } catch (...) {
                shared->err = std::current_exception();
            }
            asio::dispatch(exec, [done]() { done->cancel(); });
        });

    asio::error_code ec;
    co_await done->async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (shared->err) std::rethrow_exception(shared->err);
    co_return std::move(*shared->result);
}

// ============================================================================
// WebSocket: complete_stream_ws_responses()
// ============================================================================
//
// OpenAI's WebSocket mode for /v1/responses (per
// developers.openai.com/api/docs/guides/websocket-mode):
//
//   - Connect: wss://<host>/v1/responses with Authorization: Bearer header
//   - Send:    a JSON text frame `{"type":"response.create", ...body}`
//              where ...body mirrors the HTTP Responses request shape
//              (model, input, instructions, tools, ...).
//   - Recv:    the same SSE event payloads, but each one as a discrete
//              text frame (one event per frame). The `event:` SSE prefix
//              is replaced by an inline `"type"` field on the JSON
//              itself, so dispatch is `j["type"]` instead of parsing a
//              separate event line.
//
// The dispatch state machine here is a focused subset of what
// complete_stream's SSE_EVENTS branch handles: it processes only the
// openai-responses event vocabulary. No `usage` or `delta`-action paths
// — those are Anthropic shapes that the openai-responses schema doesn't
// emit. If we extend WS to other providers later, lifting the dispatcher
// into a shared helper becomes worthwhile; until then, inline keeps the
// scope contained and the SSE path untouched (no test regression risk).

asio::awaitable<ChatCompletion>
SchemaProvider::complete_stream_ws_responses(const CompletionParams& params,
                                             const StreamCallback& on_chunk)
{
    // Build request body under the schema lock — same pattern as the
    // HTTP path. The lock is released before any I/O.
    json request_body;
    async::AsyncEndpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        // OpenAI's Responses WS endpoint has a known quirk: when the
        // request body contains a `temperature` field, the server
        // accepts the WebSocket upgrade, then immediately closes with
        // code 1000 and zero events instead of returning an error.
        // The HTTP /v1/responses endpoint accepts temperature happily,
        // so this is WS-specific. Cross-checked against the official
        // Python `websockets` client — same key, same model, same
        // body MINUS temperature → 9 events delivered cleanly.
        //
        // CompletionParams::temperature defaults to 0.7, so this hits
        // every caller that doesn't opt out. Force temperature off
        // for the WS body while leaving HTTP behavior untouched.
        CompletionParams ws_params = params;
        ws_params.temperature = -1.0f;  // suppresses temperature in build_body
        request_body = build_body(ws_params);
        endpoint = async::split_async_endpoint(conn_.base_url);
    }

    // OpenAI WS framing: a single JSON object with `"type":"response.create"`
    // merged with the standard Responses request body (model, input,
    // tools, ...). Per the docs, transport-only fields like `stream`
    // and `background` are omitted on the wire — the socket itself IS
    // the streaming transport. build_body() doesn't add either, and
    // we don't add them here, so this is by construction.
    request_body["type"] = "response.create";

    std::vector<std::pair<std::string, std::string>> ws_headers = {
        {"Authorization", "Bearer " + get_api_key()},
    };
    // Schema-declared extra headers (rarely set for OpenAI but the
    // contract is that build_headers() reflects them; we apply the
    // same set here, minus auth which we just put in).
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        for (const auto& [k, v] : conn_.extra_headers) {
            ws_headers.emplace_back(k, v);
        }
    }

    auto ex = co_await asio::this_coro::executor;
    auto ws = co_await async::ws_connect(
        ex,
        endpoint.host,
        endpoint.port,
        endpoint.prefix.empty() ? std::string("/v1/responses")
                                : (endpoint.prefix + "/v1/responses"),
        std::move(ws_headers),
        endpoint.tls);

    auto dumped = request_body.dump();
    if (std::getenv("NEOGRAPH_WS_DEBUG")) {
        std::cerr << "[WS DEBUG] sending: " << dumped << "\n";
    }
    co_await ws->send_text(dumped);

    // ── Streaming dispatch state ──
    ChatCompletion completion;
    completion.message.role = "assistant";
    std::string full_content;
    std::map<int, ToolCall> tc_map;

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

    bool got_done = false;

    // Snapshot the bits we need from stream_/resp_ under the lock so
    // the recv loop runs lock-free.
    json   events_config;
    std::string usage_path, prompt_field, completion_field, total_field;
    {
        std::lock_guard<std::mutex> lock(schema_mutex_);
        events_config = stream_.events_config;
        usage_path        = resp_.usage_path;
        prompt_field      = resp_.prompt_tokens_field;
        completion_field  = resp_.completion_tokens_field;
        total_field       = resp_.total_tokens_field;
    }

    auto read_usage_into = [&](const json& container) {
        if (usage_path.empty()) return;
        auto u = json_path::at_path(container, usage_path);
        if (!u || !u->is_object()) return;
        int p = u->value(prompt_field, 0);
        int c = u->value(completion_field, 0);
        if (p > 0) completion.usage.prompt_tokens = p;
        if (c > 0) completion.usage.completion_tokens = c;
        if (!total_field.empty()) {
            int t = u->value(total_field, 0);
            if (t > 0) completion.usage.total_tokens = t;
        }
        if (completion.usage.total_tokens == 0) {
            completion.usage.total_tokens =
                completion.usage.prompt_tokens +
                completion.usage.completion_tokens;
        }
    };

    bool ws_debug = std::getenv("NEOGRAPH_WS_DEBUG") != nullptr;
    while (!got_done) {
        auto msg = co_await ws->recv();
        if (ws_debug) {
            std::cerr << "[WS DEBUG] recv op=" << static_cast<int>(msg.op)
                      << " bytes=" << msg.payload.size()
                      << " payload=" << msg.payload << "\n";
        }
        if (msg.op == async::WsOpcode::Close) {
            // Server closed before sending response.completed — surface
            // as an error rather than silently returning a partial
            // ChatCompletion (which would be hard to distinguish from
            // a successful empty completion). The Close frame payload
            // is `[uint16 status BE][optional UTF-8 reason]` per RFC
            // 6455 §5.5.1; lift both into the message so auth / quota
            // / model-not-found rejections are debuggable.
            std::string detail;
            if (msg.payload.size() >= 2) {
                std::uint16_t code =
                    (static_cast<std::uint8_t>(msg.payload[0]) << 8) |
                     static_cast<std::uint8_t>(msg.payload[1]);
                detail = " (close=" + std::to_string(code);
                if (msg.payload.size() > 2) {
                    detail += " reason=\"" + msg.payload.substr(2) + "\"";
                }
                detail += ")";
            }
            throw std::runtime_error(
                "openai-responses ws: server closed before response.completed"
                + detail);
        }

        json j;
        try {
            j = json::parse(msg.payload);
        } catch (const json::parse_error&) {
            // Skip malformed events to mirror the HTTP/SSE path's
            // tolerance — the server occasionally sends keep-alive
            // shaped frames that aren't application events.
            continue;
        }

        std::string event_type = j.value("type", "");
        if (event_type.empty()) continue;

        // Server-side error frames terminate the stream with detail.
        if (event_type == "error") {
            std::string err_msg = j.value("message", "");
            if (err_msg.empty() && j.contains("error") && j["error"].is_object()) {
                err_msg = j["error"].value("message", "unknown");
            }
            throw std::runtime_error(
                "openai-responses ws error: " + err_msg);
        }

        if (!events_config.contains(event_type)) continue;
        const auto& event_cfg = events_config[event_type];
        const std::string action = event_cfg.value("action", "ignore");

        if (action == "ignore") {
            continue;
        } else if (action == "block_start") {
            // Mirrors complete_stream's block_start: openai-responses
            // schema sets block_path="item", tool_call_type="function_call",
            // id_field="call_id".
            std::string block_path = event_cfg.value("block_path", "item");
            std::string type_field = event_cfg.value("type_field", "type");
            std::string tool_type   = event_cfg.value("tool_call_type", "function_call");
            std::string id_fld      = event_cfg.value("id_field", "call_id");
            std::string name_fld    = event_cfg.value("name_field", "name");

            auto block = json_path::at_path(j, block_path);
            if (block) {
                EventBlock cb;
                cb.type = block->value(type_field, "");
                cb.index = static_cast<int>(event_blocks.size());
                if (cb.type == tool_type) {
                    cb.id   = block->value(id_fld, "");
                    cb.name = block->value(name_fld, "");
                }
                event_blocks.push_back(cb);
                event_block_index = cb.index;
            }
        } else if (action == "text_delta") {
            if (event_block_index < 0 ||
                event_block_index >= static_cast<int>(event_blocks.size())) continue;
            auto& cur = event_blocks[event_block_index];
            std::string fld = event_cfg.value("delta_field", "delta");
            std::string token = j.value(fld, "");
            full_content += token;
            cur.text += token;
            if (on_chunk) on_chunk(token);
        } else if (action == "tool_args_delta") {
            if (event_block_index < 0 ||
                event_block_index >= static_cast<int>(event_blocks.size())) continue;
            auto& cur = event_blocks[event_block_index];
            std::string fld = event_cfg.value("delta_field", "delta");
            cur.args += j.value(fld, "");
        } else if (action == "block_stop") {
            std::string tool_type = event_cfg.value("tool_call_type", "function_call");
            if (event_block_index >= 0 &&
                event_block_index < static_cast<int>(event_blocks.size())) {
                auto& cb = event_blocks[event_block_index];
                if (cb.type == tool_type) {
                    ToolCall call;
                    call.id        = cb.id;
                    call.name      = cb.name;
                    call.arguments = cb.args;
                    tc_map[cb.index] = call;
                }
            }
            event_block_index = -1;
        } else if (action == "done") {
            // response.completed carries the full response object
            // (including usage) under "response". Same shape as the
            // HTTP path so we reuse the usage extractor.
            if (j.contains("response") && j["response"].is_object()) {
                read_usage_into(j["response"]);
            }
            got_done = true;
        }
        // Other action values (delta/usage etc.) are Anthropic shapes
        // that the openai-responses schema doesn't emit — ignore.
    }

    // Polite close so the server doesn't log a transport reset. Drain
    // the close echo to keep the socket state clean.
    try {
        co_await ws->send_close(1000, "done");
        auto echo = co_await ws->recv();
        (void)echo;
    } catch (const std::exception&) {
        // Server may have already closed; ignore.
    }

    completion.message.content = full_content;
    for (auto& [_, tc] : tc_map) {
        completion.message.tool_calls.push_back(tc);
    }
    co_return completion;
}

} // namespace neograph::llm
