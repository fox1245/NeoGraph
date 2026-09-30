// Schema-declared "this model rejects `temperature`" policy, shared by
// SchemaProvider and OpenAIProvider. Internal to neograph_llm.
//
// Some model families answer HTTP 400 when a request carries `temperature`
// (OpenAI reasoning models and gpt-6, Claude Opus/Sonnet/Fable 4.7+ / 5.x).
// Which families do is vendor data that changes with every model release, so
// it lives in the provider schema (`request.temperature_unsupported_models`)
// instead of a C++ branch.
#pragma once

#include <neograph/types.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::llm::detail {

inline std::string ascii_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A pattern is an exact model name, or a prefix when it ends in '*'.
// Matching is ASCII case-insensitive and also tries the part after the last
// '/', so gateway ids such as "openai/o4-mini" match "o4*".
inline bool model_matches_pattern(std::string_view model, std::string_view pattern) {
    const std::string m = ascii_lower(model);
    const std::string p = ascii_lower(pattern);
    const auto matches = [&p](std::string_view candidate) {
        if (!p.empty() && p.back() == '*') {
            const std::string_view prefix(p.data(), p.size() - 1);
            return candidate.substr(0, prefix.size()) == prefix;
        }
        return candidate == p;
    };
    if (matches(m)) return true;
    const auto slash = m.rfind('/');
    return slash != std::string::npos && matches(std::string_view(m).substr(slash + 1));
}

inline bool model_matches_any(std::string_view model,
                              const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& p) { return model_matches_pattern(model, p); });
}

// Reads `request.temperature_unsupported_models` from a schema document.
// Absent means "the endpoint accepts temperature for every model".
inline std::vector<std::string> parse_temperature_unsupported_models(const json& request) {
    std::vector<std::string> out;
    if (!request.is_object() || !request.contains("temperature_unsupported_models")) {
        return out;
    }
    const auto& list = request["temperature_unsupported_models"];
    if (!list.is_array()) {
        throw std::invalid_argument(
            "SchemaProvider: request.temperature_unsupported_models must be an array of strings");
    }
    for (const auto& item : list) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            throw std::invalid_argument(
                "SchemaProvider: request.temperature_unsupported_models entries "
                "must be non-empty strings");
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

}  // namespace neograph::llm::detail
