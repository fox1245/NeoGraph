// Typed provider failures shared by SchemaProvider and OpenAIProvider.
// Internal to neograph_llm.
//
// Builds `ProviderError` / `RateLimitError` (include/neograph/provider.h) from
//   * a non-2xx HTTP response, and
//   * an error delivered inside a 2xx response or stream (an SSE `error`
//     event, `response.failed`, a gateway's HTTP 200 `{"error": ...}` body).
// Which statuses / vendor codes are transient is vendor data, so it comes
// from the schema (`connection.retryable_statuses`, `connection.retryable_codes`)
// rather than from a C++ branch. Vendor bodies are redacted and truncated
// before they reach an exception message: gateways such as OpenRouter echo
// account/user identifiers in error bodies, and messages end up in logs.
#pragma once

#include <neograph/provider.h>
#include <neograph/types.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace neograph::llm::detail {

struct RetryPolicy {
    // Statuses that are transient across vendors: 408 timeout, 429 rate
    // limit, 500/502/503/504 server-side. A schema's
    // `connection.retryable_statuses` replaces this list (Anthropic adds its
    // 529 "overloaded"); 429 is always retryable regardless (it is a
    // RateLimitError).
    std::set<int> statuses{408, 429, 500, 502, 503, 504};
    // Vendor error codes/types that mark an error as transient even when the
    // status alone (200 for an in-stream error) cannot say so.
    std::set<std::string> codes;
};

// Reads `connection.retryable_statuses` / `connection.retryable_codes`.
// Absent keys keep the defaults above.
inline RetryPolicy parse_retry_policy(const json& connection) {
    RetryPolicy policy;
    if (!connection.is_object()) return policy;
    if (connection.contains("retryable_statuses")) {
        const auto& list = connection["retryable_statuses"];
        if (!list.is_array()) {
            throw std::invalid_argument(
                "SchemaProvider: connection.retryable_statuses must be an array of integers");
        }
        policy.statuses.clear();
        for (const auto& item : list) {
            if (!item.is_number_integer()) {
                throw std::invalid_argument(
                    "SchemaProvider: connection.retryable_statuses entries must be integers");
            }
            policy.statuses.insert(item.get<int>());
        }
    }
    if (connection.contains("retryable_codes")) {
        const auto& list = connection["retryable_codes"];
        if (!list.is_array()) {
            throw std::invalid_argument(
                "SchemaProvider: connection.retryable_codes must be an array of strings");
        }
        for (const auto& item : list) {
            if (!item.is_string() || item.get<std::string>().empty()) {
                throw std::invalid_argument(
                    "SchemaProvider: connection.retryable_codes entries must be non-empty strings");
            }
            policy.codes.insert(item.get<std::string>());
        }
    }
    return policy;
}

// Cuts at a UTF-8 code point boundary so the message stays valid text.
inline std::string truncate_utf8(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    text.resize(cut);
    text += "...[truncated]";
    return text;
}

// Masks account/user/credential identifiers in a vendor error body. Works on
// the text so it also covers non-JSON bodies and identifiers embedded in
// prose ("organization org-abc..."). Not a general secret scanner: it removes
// the identifier shapes vendors are known to echo.
inline std::string redact_identifiers(const std::string& text) {
    static const std::regex kKeyed(
        R"re("(user_id|userId|account_id|accountId|organization_id|organizationId|org_id|orgId|customer_id|customerId|api_key|apiKey)"\s*:\s*("([^"\\]|\\.)*"|-?[0-9]+))re",
        std::regex::ECMAScript);
    static const std::regex kPrefixedId(
        R"re(\b(user|org|acct)[_-][A-Za-z0-9]{8,}\b)re", std::regex::ECMAScript);
    static const std::regex kSecretKey(
        R"re(\bsk-[A-Za-z0-9_-]{12,})re", std::regex::ECMAScript);
    static const std::regex kBearer(
        R"re((Bearer\s+)[A-Za-z0-9._~+/=-]{8,})re", std::regex::ECMAScript);

    std::string out = std::regex_replace(text, kKeyed, "\"$1\":\"[redacted]\"");
    out = std::regex_replace(out, kPrefixedId, "[redacted]");
    out = std::regex_replace(out, kSecretKey, "[redacted]");
    out = std::regex_replace(out, kBearer, "$1[redacted]");
    return out;
}

// Vendor body -> text that is safe and short enough for an exception message.
inline std::string redact_and_truncate(std::string_view body,
                                       std::size_t limit = 1024) {
    // Bound the regex work first; an error body is never legitimately huge.
    return truncate_utf8(redact_identifiers(truncate_utf8(std::string(body), 8192)), limit);
}

struct ErrorFields {
    std::string message;
    std::string code;
    std::string request_id;
};

inline std::string scalar_to_string(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int>());
    return {};
}

// Understands the shapes the supported vendors use:
//   {"error": {"message", "code" | "type", ...}, "request_id"}   OpenAI, Anthropic, OpenRouter
//   {"type": "error", "code", "message"}                          Responses `error` event
//   {"response": {"error": {"code", "message"}}}                  Responses `response.failed`
// `error_object`, when given, is the object holding message/code for payloads
// that are not one of these shapes.
inline ErrorFields extract_error_fields(const json& payload, const json* error_object = nullptr) {
    ErrorFields fields;
    if (!payload.is_object()) return fields;
    json nested;  // const operator[] yields a value, so keep a local copy to point at
    const json* error = error_object;
    if (error == nullptr && payload.contains("error") && payload["error"].is_object()) {
        nested = payload["error"];
        error = &nested;
    }
    const json& source = error != nullptr ? *error : payload;
    if (source.is_object()) {
        if (source.contains("message")) fields.message = scalar_to_string(source["message"]);
        if (source.contains("code")) fields.code = scalar_to_string(source["code"]);
        if (fields.code.empty() && source.contains("type")) {
            fields.code = scalar_to_string(source["type"]);
        }
        if (source.contains("request_id")) fields.request_id = scalar_to_string(source["request_id"]);
    }
    if (fields.request_id.empty() && payload.contains("request_id")) {
        fields.request_id = scalar_to_string(payload["request_id"]);
    }
    return fields;
}

inline bool is_rate_limit_code(const std::string& code) {
    std::string lower(code);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("rate_limit") != std::string::npos || lower == "429" ||
           lower == "resource_exhausted" || lower == "too_many_requests";
}

inline json try_parse_json(std::string_view body) {
    try {
        return json::parse(std::string(body));
    } catch (...) {
        return json();
    }
}

// Non-2xx HTTP response.
[[noreturn]] inline void throw_http_error(int status, std::string_view body,
                                          int retry_after_seconds,
                                          std::string_view request_id_header,
                                          const RetryPolicy& policy) {
    const json parsed = try_parse_json(body);
    ErrorFields fields = extract_error_fields(parsed);
    if (!request_id_header.empty()) fields.request_id = std::string(request_id_header);
    const std::string message = "API error (HTTP " + std::to_string(status) + "): " +
                                redact_and_truncate(body);
    if (status == 429) {
        throw RateLimitError(message, retry_after_seconds, std::move(fields.code),
                             std::move(fields.request_id));
    }
    const bool retryable =
        policy.statuses.count(status) > 0 ||
        (!fields.code.empty() && policy.codes.count(fields.code) > 0);
    throw ProviderError(message, status, retryable, std::move(fields.code),
                        std::move(fields.request_id), retry_after_seconds);
}

// Error carried inside a successful response or stream (`status` is that
// response's status, normally 200). `error_object`, when given, is the
// object holding message/code inside `payload`.
[[noreturn]] inline void throw_embedded_error(std::string_view context, int status,
                                              const json& payload,
                                              const RetryPolicy& policy,
                                              const json* error_object = nullptr) {
    ErrorFields fields = extract_error_fields(payload, error_object);
    const std::string shown = fields.message.empty()
        ? (error_object != nullptr ? error_object->dump() : payload.dump())
        : fields.message;
    std::string message = std::string(context) + ": " + redact_and_truncate(shown);
    if (!fields.code.empty()) message += " (code: " + fields.code + ")";
    if (is_rate_limit_code(fields.code)) {
        throw RateLimitError(message, -1, std::move(fields.code),
                             std::move(fields.request_id));
    }
    bool retryable = !fields.code.empty() && policy.codes.count(fields.code) > 0;
    if (!retryable && !fields.code.empty() && fields.code.size() <= 4 &&
        std::all_of(fields.code.begin(), fields.code.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
        retryable = policy.statuses.count(std::stoi(fields.code)) > 0;
    }
    throw ProviderError(message, status, retryable, std::move(fields.code),
                        std::move(fields.request_id));
}

}  // namespace neograph::llm::detail
