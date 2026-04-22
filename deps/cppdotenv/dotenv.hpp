// cppdotenv — a C++17 header-only .env reader, inspired by python-dotenv.
// https://github.com/fox1245/cppdotenv
// Copyright (c) 2026 fox1245 — MIT License (see LICENSE beside this file).

#ifndef CPPDOTENV_DOTENV_HPP
#define CPPDOTENV_DOTENV_HPP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <stdlib.h>
#endif

namespace cppdotenv {

using Dict = std::map<std::string, std::string>;

namespace detail {

inline bool is_space(char c) noexcept {
    return c == ' ' || c == '\t';
}

inline void ltrim(std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    s.erase(0, i);
}

inline void rtrim(std::string& s) {
    while (!s.empty() && is_space(s.back())) s.pop_back();
}

inline bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Unescape sequences inside double-quoted values (python-dotenv semantics).
inline std::string unescape_double_quoted(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char n = raw[i + 1];
            switch (n) {
                case 'n':  out.push_back('\n'); ++i; break;
                case 'r':  out.push_back('\r'); ++i; break;
                case 't':  out.push_back('\t'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                case '\'': out.push_back('\''); ++i; break;
                case '"':  out.push_back('"');  ++i; break;
                default:
                    out.push_back(c);
                    break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Expand ${VAR} and $VAR references using the given lookup.
// Supports ${VAR:-default} and ${VAR-default} forms.
template <class Lookup>
inline std::string expand(std::string_view value, Lookup&& lookup) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\' && i + 1 < value.size() && value[i + 1] == '$') {
            out.push_back('$');
            ++i;
            continue;
        }
        if (c != '$') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= value.size()) {
            out.push_back('$');
            continue;
        }
        if (value[i + 1] == '{') {
            auto end = value.find('}', i + 2);
            if (end == std::string_view::npos) {
                out.push_back('$');
                continue;
            }
            std::string_view inner = value.substr(i + 2, end - (i + 2));
            std::string name;
            std::string fallback;
            bool has_fallback = false;
            auto sep = inner.find(":-");
            if (sep != std::string_view::npos) {
                name = std::string(inner.substr(0, sep));
                fallback = std::string(inner.substr(sep + 2));
                has_fallback = true;
            } else if ((sep = inner.find('-')) != std::string_view::npos) {
                name = std::string(inner.substr(0, sep));
                fallback = std::string(inner.substr(sep + 1));
                has_fallback = true;
            } else {
                name = std::string(inner);
            }
            auto resolved = lookup(name);
            if (resolved.has_value() && !(has_fallback && resolved->empty())) {
                out.append(*resolved);
            } else if (has_fallback) {
                out.append(fallback);
            }
            i = end;
        } else {
            std::size_t j = i + 1;
            if (!(std::isalpha(static_cast<unsigned char>(value[j])) || value[j] == '_')) {
                out.push_back('$');
                continue;
            }
            while (j < value.size() &&
                   (std::isalnum(static_cast<unsigned char>(value[j])) || value[j] == '_')) {
                ++j;
            }
            std::string name(value.substr(i + 1, j - (i + 1)));
            auto resolved = lookup(name);
            if (resolved.has_value()) out.append(*resolved);
            i = j - 1;
        }
    }
    return out;
}

struct ParsedLine {
    bool has_entry = false;
    std::string key;
    std::string value;
    bool quoted_double = false;
    bool quoted_single = false;
};

// Parse one logical line (with multiline quoted values already concatenated).
// Returns has_entry=false for blank lines and pure comments.
inline ParsedLine parse_logical_line(std::string line) {
    ParsedLine out;
    ltrim(line);
    if (line.empty() || line[0] == '#') return out;

    if (starts_with(line, "export ") || starts_with(line, "export\t")) {
        line.erase(0, 7);
        ltrim(line);
    }

    auto eq = line.find('=');
    if (eq == std::string::npos) return out;

    std::string key = line.substr(0, eq);
    rtrim(key);
    if (key.empty()) return out;

    std::string rest = line.substr(eq + 1);
    ltrim(rest);

    std::string value;
    if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
        char q = rest.front();
        std::size_t i = 1;
        std::string inner;
        bool closed = false;
        while (i < rest.size()) {
            char c = rest[i];
            if (c == '\\' && q == '"' && i + 1 < rest.size()) {
                inner.push_back(c);
                inner.push_back(rest[i + 1]);
                i += 2;
                continue;
            }
            if (c == q) { closed = true; ++i; break; }
            inner.push_back(c);
            ++i;
        }
        if (closed) {
            if (q == '"') {
                value = unescape_double_quoted(inner);
                out.quoted_double = true;
            } else {
                value = inner;
                out.quoted_single = true;
            }
        } else {
            value = rest;
        }
    } else {
        auto hash = rest.find(" #");
        if (hash == std::string::npos && !rest.empty() && rest.front() == '#') hash = 0;
        if (hash != std::string::npos) rest = rest.substr(0, hash);
        rtrim(rest);
        value = rest;
    }

    out.has_entry = true;
    out.key = std::move(key);
    out.value = std::move(value);
    return out;
}

// Break raw file content into logical lines, honoring multiline quoted values.
inline std::vector<std::string> split_logical_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    char open_quote = 0;

    auto flush = [&](std::string&& s) {
        lines.emplace_back(std::move(s));
    };

    for (std::size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\r') continue;
        if (open_quote == 0) {
            if (c == '\n') {
                flush(std::move(current));
                current.clear();
                continue;
            }
            current.push_back(c);
            if (c == '"' || c == '\'') {
                bool before_eq = false;
                for (std::size_t k = 0; k + 1 < current.size(); ++k) {
                    if (current[k] == '=') { before_eq = true; break; }
                }
                if (before_eq) open_quote = c;
            }
        } else {
            current.push_back(c);
            if (c == '\\' && open_quote == '"' && i + 1 < content.size()) {
                current.push_back(content[i + 1]);
                ++i;
                continue;
            }
            if (c == open_quote) open_quote = 0;
        }
    }
    if (!current.empty() || !lines.empty()) flush(std::move(current));
    return lines;
}

inline std::optional<std::string> getenv_opt(const std::string& name) {
#if defined(_WIN32)
    char* buf = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&buf, &len, name.c_str()) != 0 || buf == nullptr) return std::nullopt;
    std::string v(buf);
    free(buf);
    return v;
#else
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

inline bool setenv_portable(const std::string& name, const std::string& value, bool overwrite) {
#if defined(_WIN32)
    if (!overwrite) {
        char* existing = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&existing, &len, name.c_str()) == 0 && existing != nullptr) {
            free(existing);
            return true;
        }
    }
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#else
    return ::setenv(name.c_str(), value.c_str(), overwrite ? 1 : 0) == 0;
#endif
}

inline bool unsetenv_portable(const std::string& name) {
#if defined(_WIN32)
    return _putenv_s(name.c_str(), "") == 0;
#else
    return ::unsetenv(name.c_str()) == 0;
#endif
}

}  // namespace detail

// Parse content (without touching environment) into a Dict.
// Variable expansion uses prior entries and, if enabled, the process env.
inline Dict parse_stream(std::istream& is, bool interpolate = true,
                        bool use_process_env = true) {
    std::stringstream buf;
    buf << is.rdbuf();
    std::string content = buf.str();

    Dict result;
    for (auto& raw : detail::split_logical_lines(content)) {
        auto parsed = detail::parse_logical_line(std::move(raw));
        if (!parsed.has_entry) continue;

        std::string value = parsed.value;
        if (interpolate && !parsed.quoted_single) {
            value = detail::expand(value, [&](const std::string& name) -> std::optional<std::string> {
                auto it = result.find(name);
                if (it != result.end()) return it->second;
                if (use_process_env) return detail::getenv_opt(name);
                return std::nullopt;
            });
        }
        result[parsed.key] = std::move(value);
    }
    return result;
}

// Parse a .env file into a Dict. Returns empty dict if the file is missing.
inline Dict dotenv_values(const std::filesystem::path& path = ".env",
                          bool interpolate = true) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    return parse_stream(f, interpolate, /*use_process_env=*/true);
}

// Parse an in-memory string as if it were a .env file.
inline Dict dotenv_values_from_string(const std::string& content,
                                      bool interpolate = true,
                                      bool use_process_env = true) {
    std::istringstream is(content);
    return parse_stream(is, interpolate, use_process_env);
}

// Load .env into the process environment. If override=false, keep existing values.
inline bool load_dotenv(const std::filesystem::path& path = ".env",
                        bool override_existing = false,
                        bool interpolate = true) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    auto values = parse_stream(f, interpolate, /*use_process_env=*/true);
    for (const auto& [k, v] : values) {
        detail::setenv_portable(k, v, override_existing);
    }
    return true;
}

// Walk up from starting directory looking for a file named `filename`.
// Returns an empty path if not found.
inline std::filesystem::path find_dotenv(const std::string& filename = ".env",
                                         const std::filesystem::path& start = std::filesystem::current_path()) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec);
    if (ec) return {};
    while (true) {
        fs::path candidate = dir / filename;
        if (fs::exists(candidate, ec)) return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir) return {};
        dir = dir.parent_path();
    }
}

// Convenience: find the nearest .env by walking up from cwd, then load it.
// Returns the path that was loaded, or empty path if no file was found.
inline std::filesystem::path auto_load_dotenv(const std::string& filename = ".env",
                                              bool override_existing = false,
                                              bool interpolate = true) {
    auto path = find_dotenv(filename);
    if (path.empty()) return {};
    if (!load_dotenv(path, override_existing, interpolate)) return {};
    return path;
}

// Get a single key's value from a .env file (no env side effects).
inline std::optional<std::string> get_key(const std::filesystem::path& path,
                                          const std::string& key,
                                          bool interpolate = true) {
    auto values = dotenv_values(path, interpolate);
    auto it = values.find(key);
    if (it == values.end()) return std::nullopt;
    return it->second;
}

namespace detail {

inline std::string encode_value(const std::string& value, char quote) {
    std::string encoded;
    encoded.reserve(value.size() + 2);
    if (quote == '"') {
        encoded.push_back('"');
        for (char c : value) {
            switch (c) {
                case '\\': encoded += "\\\\"; break;
                case '"':  encoded += "\\\""; break;
                case '\n': encoded += "\\n";  break;
                case '\r': encoded += "\\r";  break;
                case '\t': encoded += "\\t";  break;
                default:   encoded.push_back(c);
            }
        }
        encoded.push_back('"');
    } else if (quote == '\'') {
        encoded.push_back('\'');
        encoded.append(value);
        encoded.push_back('\'');
    } else {
        encoded = value;
    }
    return encoded;
}

// Given a raw line like `FOO="bar"` or `export FOO='bar'`, return the quote
// character used for the value, or '\0' if unquoted.
inline char detect_quote_in_line(const std::string& line) {
    std::string probe = line;
    ltrim(probe);
    if (starts_with(probe, "export ") || starts_with(probe, "export\t")) {
        probe.erase(0, 7);
        ltrim(probe);
    }
    auto eq = probe.find('=');
    if (eq == std::string::npos) return '\0';
    std::string rhs = probe.substr(eq + 1);
    ltrim(rhs);
    if (!rhs.empty() && (rhs.front() == '"' || rhs.front() == '\'')) return rhs.front();
    return '\0';
}

}  // namespace detail

// Set or update a key in a .env file. Creates the file if it doesn't exist.
// When updating an existing key, preserves the quote style already in use.
// For newly inserted keys, uses `quote` ('"' by default, '\'' for single,
// or '\0' for unquoted).
inline bool set_key(const std::filesystem::path& path,
                    const std::string& key,
                    const std::string& value,
                    char quote = '"') {
    std::string content;
    {
        std::ifstream in(path);
        if (in.is_open()) {
            std::stringstream ss;
            ss << in.rdbuf();
            content = ss.str();
        }
    }

    std::vector<std::string> output_lines;
    bool replaced = false;
    std::string line;
    std::stringstream ss(content);
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string probe = line;
        detail::ltrim(probe);
        bool had_export = false;
        if (detail::starts_with(probe, "export ") || detail::starts_with(probe, "export\t")) {
            probe.erase(0, 7);
            detail::ltrim(probe);
            had_export = true;
        }
        auto eq = probe.find('=');
        if (!replaced && eq != std::string::npos) {
            std::string k = probe.substr(0, eq);
            detail::rtrim(k);
            if (k == key) {
                char existing_quote = detail::detect_quote_in_line(line);
                std::string encoded = detail::encode_value(value, existing_quote);
                std::string rebuilt = (had_export ? std::string("export ") : std::string())
                                    + key + "=" + encoded;
                output_lines.push_back(rebuilt);
                replaced = true;
                continue;
            }
        }
        output_lines.push_back(line);
    }
    if (!replaced) {
        std::string encoded = detail::encode_value(value, quote);
        output_lines.push_back(key + "=" + encoded);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (std::size_t i = 0; i < output_lines.size(); ++i) {
        out << output_lines[i];
        if (i + 1 < output_lines.size()) out << '\n';
    }
    if (!content.empty() && content.back() == '\n') out << '\n';
    else if (content.empty()) out << '\n';
    return true;
}

// Remove a key from a .env file. Returns true if the key was present and removed.
inline bool unset_key(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    std::vector<std::string> output_lines;
    bool removed = false;
    std::string line;
    std::stringstream reader(content);
    while (std::getline(reader, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string probe = line;
        detail::ltrim(probe);
        if (detail::starts_with(probe, "export ")) probe.erase(0, 7);
        auto eq = probe.find('=');
        if (eq != std::string::npos) {
            std::string k = probe.substr(0, eq);
            detail::rtrim(k);
            if (k == key) {
                removed = true;
                continue;
            }
        }
        output_lines.push_back(line);
    }
    if (!removed) return false;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    for (std::size_t i = 0; i < output_lines.size(); ++i) {
        out << output_lines[i];
        if (i + 1 < output_lines.size()) out << '\n';
    }
    if (!content.empty() && content.back() == '\n') out << '\n';
    return true;
}

}  // namespace cppdotenv

#endif  // CPPDOTENV_DOTENV_HPP
