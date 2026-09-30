// Environment templating for schema-declared request headers. Internal to
// neograph_llm.
//
// A schema header value may reference environment variables so a deployment
// can vary it without editing the schema (the same idea as `api_key_env`):
//   "anthropic-workspace-id": "${ANTHROPIC_WORKSPACE_ID?}"
//   "x-team": "team-${TEAM}"
//   ${NAME}   the variable must be set and non-empty, otherwise the request
//             fails with an error naming it (no silent empty header)
//   ${NAME?}  optional: when the variable is unset or empty the WHOLE header
//             is omitted
// An expanded value that would smuggle a line break is refused.
#pragma once

#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace neograph::llm::detail {

inline bool is_env_name_char(char c, bool first) {
    return c == '_' || std::isalpha(static_cast<unsigned char>(c)) ||
           (!first && std::isdigit(static_cast<unsigned char>(c)));
}

// A header name is an HTTP token; a name or value must never carry CR/LF.
inline void validate_header_name(const std::string& name) {
    if (name.empty()) throw std::invalid_argument("SchemaProvider: empty header name");
    for (const char c : name) {
        const bool token = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                           c == '.' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
                           c == '\'' || c == '*' || c == '+' || c == '^' || c == '`' || c == '|' ||
                           c == '~';
        if (!token) {
            throw std::invalid_argument("SchemaProvider: invalid character in header name '" + name +
                                        "'");
        }
    }
}

inline bool has_line_break(const std::string& value) {
    return value.find_first_of("\r\n") != std::string::npos;
}

// Syntax check, done when the schema is loaded (the environment is read per
// request, like the API key).
inline void validate_header_template(const std::string& header, const std::string& value) {
    validate_header_name(header);
    if (has_line_break(value)) {
        throw std::invalid_argument("SchemaProvider: header '" + header + "' contains a line break");
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '$' || i + 1 >= value.size() || value[i + 1] != '{') continue;
        const auto close = value.find('}', i + 2);
        if (close == std::string::npos) {
            throw std::invalid_argument("SchemaProvider: header '" + header +
                                        "' has an unterminated ${...}");
        }
        std::string name = value.substr(i + 2, close - i - 2);
        if (!name.empty() && name.back() == '?') name.pop_back();
        bool ok = !name.empty();
        for (std::size_t k = 0; ok && k < name.size(); ++k) ok = is_env_name_char(name[k], k == 0);
        if (!ok) {
            throw std::invalid_argument("SchemaProvider: header '" + header +
                                        "' has an invalid environment variable name in ${...}");
        }
        i = close;
    }
}

// nullopt = omit the header (an optional variable is unset or empty).
inline std::optional<std::string> expand_header_template(const std::string& header,
                                                         const std::string& value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '$' || i + 1 >= value.size() || value[i + 1] != '{') {
            out += value[i];
            continue;
        }
        const auto close = value.find('}', i + 2);
        std::string name = value.substr(i + 2, close - i - 2);
        const bool optional = !name.empty() && name.back() == '?';
        if (optional) name.pop_back();
        const char* env = std::getenv(name.c_str());
        if (env == nullptr || *env == '\0') {
            if (optional) return std::nullopt;
            throw std::runtime_error("SchemaProvider: header '" + header +
                                     "' needs the environment variable " + name +
                                     " (write ${" + name + "?} to omit the header when it is unset)");
        }
        out += env;
        i = close;
    }
    if (has_line_break(out)) {
        throw std::runtime_error("SchemaProvider: header '" + header +
                                 "' expanded to a value containing a line break");
    }
    return out;
}

}  // namespace neograph::llm::detail
