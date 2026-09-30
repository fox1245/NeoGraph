// Schema-driven, network-free request mapping and response decoding.
#include <neograph/llm/schema_provider.h>

#include "provider_error.h"
#include "reasoning_carry.h"
#include "temperature_policy.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <random>
#include <stdexcept>

namespace neograph::llm {
void SchemaProvider::parse_schema()
{
    provider_name_ = schema_.value("name", "unknown");
    auto require_primitive = [this](SchemaPrimitiveCategory category,
                                     const std::string& name,
                                     const char* path) {
        if (!primitive_registry_.contains(category, name)) {
            throw std::invalid_argument(
                std::string("SchemaProvider: unknown primitive at schema path ") +
                path + " (category " + SchemaPrimitiveRegistry::category_name(category) +
                "): " + name);
        }
    };

    auto c = schema_["connection"];
    transport_primitive_name_ = c.value("transport",
                                        c.value("transport_primitive", "http"));
    require_primitive(SchemaPrimitiveCategory::Transport,
                      transport_primitive_name_, "connection.transport");
    const auto& transport = primitive_registry_.transport(transport_primitive_name_);
    if (transport) transport_factory_ = transport;

    execution_primitive_name_ = schema_.value("execution_mode", "standard");
    if (schema_.contains("execution") && schema_["execution"].is_object()) {
        execution_primitive_name_ =
            schema_["execution"].value("mode", execution_primitive_name_);
    }
    require_primitive(SchemaPrimitiveCategory::ExecutionMode,
                      execution_primitive_name_, "execution.mode");
    const auto& execution = primitive_registry_.execution_mode(execution_primitive_name_);
    if (execution) execution_factory_ = execution;
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
    {
        const auto policy = detail::parse_retry_policy(c);
        conn_.retryable_statuses = policy.statuses;
        conn_.retryable_codes = policy.codes;
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
    // temperature_unsupported_models: the schema names the model families
    // whose endpoint answers HTTP 400 when `temperature` is present (OpenAI
    // reasoning models, Claude 4.7+/5.x, ...). Vendor data, so it lives here
    // rather than in a C++ branch.
    req_.temperature_unsupported_models =
        detail::parse_temperature_unsupported_models(r);
    req_.max_tokens_path = r.value("max_tokens_path", "max_tokens");
    req_.max_tokens_required = r.value("max_tokens_required", false);
    req_.max_tokens_default = r.value("max_tokens_default", -1);
    req_.stream_field = r.value("stream_field", "stream");
    req_.extra_fields = r.value("extra_fields", json::object());
    req_.prompt_field = r.value("prompt_field", "");
    if (r.contains("prompt_template")) {
        req_.prompt_template = r["prompt_template"];
        if (!req_.prompt_template.is_object() ||
            req_.prompt_template.dump().find("$PROMPT") == std::string::npos) {
            throw std::invalid_argument(
                "SchemaProvider: prompt_template must be an object containing $PROMPT");
        }
    }
    if (!req_.prompt_field.empty() && !req_.messages_field.empty() &&
        req_.prompt_field == req_.messages_field) {
        throw std::invalid_argument("SchemaProvider: prompt and messages fields overlap");
    }

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

    // --- Failure signals (all optional; absent = the body is never a failure) ---
    auto string_set = [](const json& object, const char* key, const char* section) {
        std::set<std::string> out;
        if (!object.contains(key)) return out;
        const auto& list = object[key];
        if (!list.is_array()) {
            throw std::invalid_argument(std::string("SchemaProvider: ") + section + "." + key +
                                        " must be an array of strings");
        }
        for (const auto& item : list) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::invalid_argument(std::string("SchemaProvider: ") + section + "." + key +
                                            " entries must be non-empty strings");
            }
            out.insert(item.get<std::string>());
        }
        return out;
    };
    resp_.error_path = resp.value("error_path", "");
    resp_.failure_status_path = resp.value("failure_status_path", "");
    resp_.failure_statuses = string_set(resp, "failure_statuses", "response");
    resp_.block_reason_path = resp.value("block_reason_path", "");
    resp_.block_stop_reason = resp.value("block_stop_reason", "content_filter");
    resp_.error_finish_reasons = string_set(resp, "error_finish_reasons", "response");

    // --- Reasoning carry (optional; absent = reasoning items are ignored) ---
    reasoning_ = {};
    if (schema_.contains("reasoning")) {
        const auto& rj = schema_["reasoning"];
        if (!rj.is_object()) {
            throw std::invalid_argument("SchemaProvider: reasoning must be an object");
        }
        auto string_field = [&rj](const char* key) {
            if (!rj.contains(key)) return std::string();
            if (!rj[key].is_string()) {
                throw std::invalid_argument(
                    std::string("SchemaProvider: reasoning.") + key + " must be a string");
            }
            return rj[key].get<std::string>();
        };
        if (rj.contains("carry_types")) {
            if (!rj["carry_types"].is_array()) {
                throw std::invalid_argument(
                    "SchemaProvider: reasoning.carry_types must be an array of strings");
            }
            for (const auto& t : rj["carry_types"]) {
                if (!t.is_string() || t.get<std::string>().empty()) {
                    throw std::invalid_argument(
                        "SchemaProvider: reasoning.carry_types entries must be non-empty strings");
                }
                reasoning_.carry_types.insert(t.get<std::string>());
            }
        }
        reasoning_.text_field = string_field("text_field");
        reasoning_.thought_flag_field = string_field("thought_flag_field");
        reasoning_.signature_field = string_field("signature_field");
        reasoning_.foreign_signature = string_field("foreign_signature");
        if (!reasoning_.foreign_signature.empty() && reasoning_.signature_field.empty()) {
            throw std::invalid_argument(
                "SchemaProvider: reasoning.foreign_signature requires reasoning.signature_field");
        }
        reasoning_.message_field = string_field("message_field");
        if (rj.contains("delta_fields")) {
            if (!rj["delta_fields"].is_object()) {
                throw std::invalid_argument(
                    "SchemaProvider: reasoning.delta_fields must be an object of strings");
            }
            for (auto [delta_type, target] : rj["delta_fields"].items()) {
                if (!target.is_string() || target.get<std::string>().empty()) {
                    throw std::invalid_argument(
                        "SchemaProvider: reasoning.delta_fields values must be non-empty strings");
                }
                reasoning_.delta_fields[delta_type] = target.get<std::string>();
            }
        }
        if (rj.contains("stream_concat_fields")) {
            if (!rj["stream_concat_fields"].is_array()) {
                throw std::invalid_argument(
                    "SchemaProvider: reasoning.stream_concat_fields must be an array of strings");
            }
            for (const auto& f : rj["stream_concat_fields"]) {
                if (!f.is_string() || f.get<std::string>().empty()) {
                    throw std::invalid_argument(
                        "SchemaProvider: reasoning.stream_concat_fields entries must be non-empty strings");
                }
                reasoning_.stream_concat_fields.insert(f.get<std::string>());
            }
        }
    }

    artifact_parser_primitive_name_ = resp.value("artifact_parser", "rules");
    require_primitive(SchemaPrimitiveCategory::ArtifactParser,
                      artifact_parser_primitive_name_, "response.artifact_parser");
    const auto& artifact_parser =
        primitive_registry_.artifact_parser(artifact_parser_primitive_name_);
    if (artifact_parser) artifact_parser_factory_ = artifact_parser;
    if (resp.contains("artifacts")) {
        if (!resp["artifacts"].is_array()) {
            throw std::invalid_argument("SchemaProvider: response.artifacts must be an array");
        }
        for (const auto& entry : resp["artifacts"]) {
            ResponseConfig::ArtifactRule rule;
            rule.items_path = entry.value("items_path", "");
            rule.type_path = entry.value("type_path", "");
            rule.type = entry.value("type", "");
            rule.match_path = entry.value("match_path", "");
            rule.kind = entry.value("kind", "");
            rule.mime_type = entry.value("mime_type", "");
            rule.mime_path = entry.value("mime_path", "");
            rule.base64_path = entry.value("base64_path", "");
            rule.url_path = entry.value("url_path", "");
            rule.file_id_path = entry.value("file_id_path", "");
            rule.metadata_path = entry.value("metadata_path", "");
            if (rule.items_path.empty() || rule.kind.empty() ||
                (rule.base64_path.empty() && rule.url_path.empty() &&
                 rule.file_id_path.empty()) ||
                (rule.type_path.empty() != rule.type.empty())) {
                throw std::invalid_argument("SchemaProvider: invalid artifact mapping");
            }
            resp_.artifacts.push_back(std::move(rule));
        }
    }
    if (schema_.contains("operation")) {
        const auto& op = schema_["operation"];
        operation_.id_path = op.value("id_path", "");
        operation_.done_path = op.value("done_path", "");
        operation_.error_path = op.value("error_path", "");
        operation_.result_path = op.value("result_path", "");
        operation_.poll_endpoint = op.value("poll_endpoint", "");
        operation_.poll_method = op.value("poll_method", "GET");
        operation_.finalize_endpoint = op.value("finalize_endpoint", "");
        operation_.poll_interval_ms = op.value("poll_interval_ms", 1000);
        const auto absent_status = op.value("absent_status", "error");
        if (absent_status != "error" && absent_status != "pending") {
            throw std::invalid_argument(
                "SchemaProvider: operation.absent_status must be error or pending");
        }
        operation_.absent_status_pending = absent_status == "pending";
        if (operation_.id_path.empty() || operation_.done_path.empty() ||
            operation_.poll_endpoint.find("$OPERATION") == std::string::npos ||
            (!operation_.finalize_endpoint.empty() &&
             operation_.finalize_endpoint.find("$OPERATION") == std::string::npos) ||
            (operation_.poll_method != "GET" && operation_.poll_method != "POST") ||
            operation_.poll_interval_ms <= 0) {
            throw std::invalid_argument("SchemaProvider: invalid operation mapping");
        }
    }

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
    stream_.error_path = st.value("error_path", "");
    stream_.require_terminal_event = st.value("require_terminal_event", false);
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

std::string SchemaProvider::operation_endpoint(
    const std::string& endpoint, const std::string& operation_id,
    std::string_view api_key) const {
    if (operation_id.empty() || operation_id.front() == '/' ||
        operation_id.back() == '/' || operation_id.find("..") != std::string::npos) {
        throw OperationError("SchemaProvider: invalid operation identifier");
    }
    for (const unsigned char c : operation_id) {
        if (!(std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == ':')) {
            throw OperationError("SchemaProvider: unsafe operation identifier");
        }
    }
    std::string path = endpoint;
    const auto pos = path.find("$OPERATION");
    path.replace(pos, sizeof("$OPERATION") - 1, operation_id);
    if (!conn_.auth_query_param.empty()) {
        path += path.find('?') == std::string::npos ? '?' : '&';
        path += conn_.auth_query_param + "=" + std::string(api_key);
    }
    return path;
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
                if (!reasoning_.message_field.empty() && msg.reasoning_details.is_array() &&
                    !msg.reasoning_details.empty()) {
                    j[reasoning_.message_field] = msg.reasoning_details;
                }
                break;
            }
            case ToolCallStrategy::CONTENT_ARRAY: {
                // Claude: content is array of text + tool_use items
                json content_arr = json::array();
                // Carried reasoning blocks go first, unmodified.
                if (msg.reasoning_details.is_array()) {
                    for (const auto& item : msg.reasoning_details) {
                        if (detail::is_carried_item(item, reasoning_.carry_types)) content_arr.push_back(item);
                    }
                }
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
                bool signed_call = false;
                std::optional<std::size_t> first_call_part;
                for (const auto& tc : msg.tool_calls) {
                    std::map<std::string, json> vars;
                    vars["NAME"] = tc.name;
                    // Gemini wants args as object
                    try {
                        vars["ARGUMENTS_OBJECT"] = json::parse(tc.arguments);
                    } catch (...) {
                        vars["ARGUMENTS_OBJECT"] = json::object();
                    }
                    json part = substitute(tool_call_.item_template, vars);
                    if (!reasoning_.signature_field.empty()) {
                        if (auto sig = detail::find_tool_call_signature(msg.reasoning_details, tc.id)) {
                            part[reasoning_.signature_field] = *sig;
                            signed_call = true;
                        }
                    }
                    if (!first_call_part) first_call_part = parts.size();
                    parts.push_back(std::move(part));
                }
                // No captured signature anywhere in this turn: the history is
                // foreign. Only the first call of a turn is validated.
                if (!signed_call && first_call_part && !reasoning_.foreign_signature.empty()) {
                    json first = parts[*first_call_part];
                    first[reasoning_.signature_field] = reasoning_.foreign_signature;
                    parts[*first_call_part] = std::move(first);
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
    if (msg.role == "assistant" && !reasoning_.message_field.empty() &&
        msg.reasoning_details.is_array() && !msg.reasoning_details.empty()) {
        j[reasoning_.message_field] = msg.reasoning_details;
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
                // Carried reasoning items precede the items they produced.
                if (msg.reasoning_details.is_array()) {
                    for (const auto& item : msg.reasoning_details) {
                        if (detail::is_carried_item(item, reasoning_.carry_types)) arr.push_back(item);
                    }
                }
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
    std::string model = params.model.empty() ? user_config_.default_model : params.model;
    const bool prompt_envelope = !req_.prompt_field.empty() || !req_.prompt_template.is_null();
    if (prompt_envelope &&
        (params.prompt.empty() || !params.messages.empty() || !params.tools.empty())) {
        throw std::invalid_argument("SchemaProvider: prompt envelope requires a prompt without messages/tools");
    }
    json body = req_.prompt_template.is_null() ? json::object()
        : substitute(req_.prompt_template, {{"PROMPT", params.prompt}, {"MODEL", model}});
    if (!req_.prompt_field.empty()) {
        json_path::set_path(body, req_.prompt_field, params.prompt);
    }
    if (!req_.model_field.empty()) {
        body[req_.model_field] = model;
    }

    if (!prompt_envelope) {
        // System prompt and chat messages are exclusively a chat-envelope feature.
        std::vector<ChatMessage> non_system_messages;
        std::string system_content;
        for (const auto& msg : params.messages) {
            if (msg.role == "system") {
                if (!system_content.empty()) system_content += "\n\n";
                system_content += msg.content;
            } else {
                non_system_messages.push_back(msg);
            }
        }
        switch (sys_.strategy) {
            case SystemPromptStrategy::IN_MESSAGES:
                body[req_.messages_field] = serialize_messages(params.messages);
                break;
            case SystemPromptStrategy::TOP_LEVEL:
                if (!system_content.empty()) body[sys_.field] = system_content;
                body[req_.messages_field] = serialize_messages(non_system_messages);
                break;
            case SystemPromptStrategy::TOP_LEVEL_PARTS:
                if (!system_content.empty()) {
                    json part;
                    part[sys_.text_field] = system_content;
                    json sys_obj;
                    sys_obj[sys_.parts_field] = json::array({part});
                    body[sys_.field] = sys_obj;
                }
                body[req_.messages_field] = serialize_messages(non_system_messages);
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
        !req_.temperature_path.empty() &&
        !detail::model_matches_any(model, req_.temperature_unsupported_models)) {
        json_path::set_path(body, req_.temperature_path, params.temperature);
    }

    // Max tokens
    int max_tokens = params.max_tokens;
    if (max_tokens <= 0 && req_.max_tokens_required) {
        max_tokens = req_.max_tokens_default;
    }
    if (max_tokens > 0 && !req_.max_tokens_path.empty()) {
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
    check_response_failure(resp_json);
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

            // Opaque provider continuation (for example OpenRouter
            // `reasoning_details`), kept verbatim and replayed on this message.
            if (!reasoning_.message_field.empty() &&
                message->contains(reasoning_.message_field) &&
                (*message)[reasoning_.message_field].is_array()) {
                msg.reasoning_details = (*message)[reasoning_.message_field];
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
                } else if (reasoning_.carry_types.count(type) > 0) {
                    detail::push_reasoning_detail(msg, block);
                    if (!reasoning_.text_field.empty() && block.contains(reasoning_.text_field)) {
                        detail::append_reasoning_text(msg, detail::reasoning_text_of(block[reasoning_.text_field]));
                    }
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
                else if (reasoning_.carry_types.count(type) > 0) {
                    detail::push_reasoning_detail(msg, item);
                    if (!reasoning_.text_field.empty() && item.contains(reasoning_.text_field)) {
                        detail::append_reasoning_text(msg, detail::reasoning_text_of(item[reasoning_.text_field]));
                    }
                }
                // Other item types (web_search_call, etc.) are ignored.
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
                // A "thought" part is reasoning, not user-visible content.
                const bool is_thought =
                    !reasoning_.thought_flag_field.empty() &&
                    part.contains(reasoning_.thought_flag_field) &&
                    part[reasoning_.thought_flag_field].is_boolean() &&
                    part[reasoning_.thought_flag_field].get<bool>();
                if (part.contains(resp_.text_field) && part[resp_.text_field].is_string()) {
                    if (is_thought) {
                        detail::append_reasoning_text(msg, part[resp_.text_field].get<std::string>());
                    } else {
                        if (!full_text.empty()) full_text += "\n";
                        full_text += part[resp_.text_field].get<std::string>();
                    }
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
                    // The signature sits beside the functionCall and must be
                    // echoed on that same part; key it by the tool-call id.
                    if (!reasoning_.signature_field.empty() &&
                        part.contains(reasoning_.signature_field)) {
                        detail::push_reasoning_detail(msg, json{
                            {"type", "tool_call_signature"},
                            {"tool_call_id", call.id},
                            {"signature", part[reasoning_.signature_field]}});
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

std::vector<GeneratedArtifact> SchemaProvider::parse_artifacts(
    const json& response,
    const SchemaPrimitiveRequestContext* request_context) const {
    if (artifact_parser_factory_) {
        if (!request_context) {
            throw OperationError(
                "SchemaProvider: custom artifact parser requires a request context");
        }
        return artifact_parser_factory_(response, *request_context);
    }
    std::vector<GeneratedArtifact> artifacts;
    for (const auto& rule : resp_.artifacts) {
        auto items = json_path::at_path(response, rule.items_path);
        if (!items) continue;
        if (!items->is_array()) {
            throw OperationError("SchemaProvider: artifact items are not an array");
        }
        for (const auto& item : *items) {
            if (!rule.match_path.empty() &&
                !json_path::has_path(item, rule.match_path)) continue;
            if (!rule.type_path.empty()) {
                const auto type = json_path::at_path(item, rule.type_path);
                if (!type || !type->is_string() ||
                    type->get<std::string>() != rule.type) continue;
            }
            GeneratedArtifact artifact;
            artifact.kind = rule.kind;
            artifact.mime_type = rule.mime_type;
            auto string_field = [&item](const std::string& path) -> std::string {
                if (path.empty()) return {};
                const auto value = json_path::at_path(item, path);
                if (!value || value->is_null()) return {};
                if (!value->is_string()) {
                    throw OperationError("SchemaProvider: artifact payload field must be a string");
                }
                return value->get<std::string>();
            };
            if (!rule.mime_path.empty()) {
                const auto mime = string_field(rule.mime_path);
                if (!mime.empty()) artifact.mime_type = mime;
            }
            artifact.base64_data = string_field(rule.base64_path);
            artifact.url = string_field(rule.url_path);
            artifact.file_id = string_field(rule.file_id_path);
            const int payload_count = static_cast<int>(!artifact.base64_data.empty()) +
                                      static_cast<int>(!artifact.url.empty()) +
                                      static_cast<int>(!artifact.file_id.empty());
            if (payload_count == 0) {
                throw OperationError("SchemaProvider: artifact requires a payload");
            }
            if (!rule.metadata_path.empty()) {
                const auto metadata = json_path::at_path(item, rule.metadata_path);
                if (metadata) artifact.metadata = *metadata;
            }
            artifacts.push_back(std::move(artifact));
        }
    }
    return artifacts;
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

// A blocked prompt returns no candidates at all; without this the empty body
// normalizes to a plain end of turn.
static bool prompt_blocked(const json& body, const std::string& block_reason_path) {
    if (block_reason_path.empty()) return false;
    const auto reason = json_path::at_path(body, block_reason_path);
    return reason && reason->is_string() && !reason->get<std::string>().empty();
}

std::string SchemaProvider::parse_stop_reason(const json& resp_json) const {
    if (prompt_blocked(resp_json, resp_.block_reason_path)) return resp_.block_stop_reason;
    auto map_reason = [this](const json& value,
                             const std::map<std::string, std::string>& mapping,
                             bool unknown_when_unmapped) {
        if (!value.is_string()) return std::string{};
        const auto raw = value.get<std::string>();
        if (resp_.error_finish_reasons.count(raw) > 0) {
            // The model failed; a normalized "unknown" stop would let the
            // agent loop carry on as if it had answered.
            throw ProviderError(
                "SchemaProvider (" + provider_name_ + "): generation failed with finish reason '" +
                    raw + "'",
                200, false, raw);
        }
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
    if (prompt_blocked(event_json, resp_.block_reason_path)) return resp_.block_stop_reason;
    auto map_reason = [this](const json& value,
                             const std::map<std::string, std::string>& mapping,
                             bool unknown_when_unmapped) {
        if (!value.is_string()) return std::string{};
        const auto raw = value.get<std::string>();
        if (resp_.error_finish_reasons.count(raw) > 0) {
            // The model failed; a normalized "unknown" stop would let the
            // agent loop carry on as if it had answered.
            throw ProviderError(
                "SchemaProvider (" + provider_name_ + "): generation failed with finish reason '" +
                    raw + "'",
                200, false, raw);
        }
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

void SchemaProvider::check_response_failure(const json& resp_json) const {
    if (!resp_json.is_object()) return;
    const detail::RetryPolicy policy{conn_.retryable_statuses, conn_.retryable_codes};
    if (!resp_.error_path.empty()) {
        const auto error = json_path::at_path(resp_json, resp_.error_path);
        if (error && error->is_string()) {
            throw ProviderError(
                "API error (HTTP 200 with error body): " +
                    detail::redact_and_truncate(error->get<std::string>()),
                200, false);
        }
        if (error && error->is_object()) {
            detail::throw_embedded_error("API error (HTTP 200 with error body)", 200,
                                         resp_json, policy, &*error);
        }
    }
    if (!resp_.failure_status_path.empty() && !resp_.failure_statuses.empty()) {
        const auto status = json_path::at_path(resp_json, resp_.failure_status_path);
        if (status && status->is_string() &&
            resp_.failure_statuses.count(status->get<std::string>()) > 0) {
            const auto value = status->get<std::string>();
            throw ProviderError(
                "SchemaProvider (" + provider_name_ + "): response status '" + value + "'",
                200, false, value);
        }
    }
}

void SchemaProvider::throw_stream_event_error(const std::string& event_type,
                                              const json& payload,
                                              const json& event_cfg) const {
    const detail::RetryPolicy policy{conn_.retryable_statuses, conn_.retryable_codes};
    const std::string path = event_cfg.value("error_path", "error");
    std::optional<json> error;
    if (!path.empty()) error = json_path::at_path(payload, path);
    const bool fail = event_cfg.value("action", "error") == "fail";
    const std::string context = std::string(fail ? "response failed" : "stream error event") +
                                " '" + event_type + "' (" + provider_name_ + ")";
    detail::throw_embedded_error(context, 200, payload, policy,
                                 (error && error->is_object()) ? &*error : nullptr);
}

void SchemaProvider::check_stream_chunk_error(const json& chunk) const {
    if (stream_.error_path.empty() || !chunk.is_object()) return;
    const auto error = json_path::at_path(chunk, stream_.error_path);
    if (!error || !(error->is_object() || error->is_string())) return;
    const detail::RetryPolicy policy{conn_.retryable_statuses, conn_.retryable_codes};
    if (error->is_string()) {
        throw ProviderError(
            "stream error chunk (" + provider_name_ + "): " +
                detail::redact_and_truncate(error->get<std::string>()),
            200, false);
    }
    detail::throw_embedded_error("stream error chunk (" + provider_name_ + ")", 200, chunk,
                                 policy, &*error);
}

} // namespace neograph::llm
