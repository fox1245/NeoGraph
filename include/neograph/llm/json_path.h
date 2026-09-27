/**
 * @file llm/json_path.h
 * @brief JSON dot-path navigation utilities.
 *
 * Provides functions for navigating and manipulating JSON objects using
 * dot-notation paths like "choices.0.message.content". Numeric segments
 * are treated as array indices.
 *
 * Unlike the prior nlohmann-based version (which returned raw `const json*`
 * pointers into the parent document), these functions return
 * `std::optional<json>` since our yyjson-backed handles are lightweight value
 * types sharing the underlying document via shared_ptr.
 */
#pragma once

#include <limits>
#include <neograph/json.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neograph::llm {

using json = neograph::json;

namespace json_path {

/// Split "a.b.0.c" into ["a","b","0","c"].
inline std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    if (path.empty()) return segments;

    std::string::size_type start = 0;
    while (start < path.size()) {
        auto dot = path.find('.', start);
        if (dot == std::string::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        segments.push_back(path.substr(start, dot - start));
        start = dot + 1;
    }
    return segments;
}

/// True if every char is a digit.
inline bool is_index(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

/// Navigate into `root` by dot-path. Returns nullopt if path doesn't resolve.
inline std::optional<json> at_path(const json& root, const std::string& path) {
    if (path.empty()) return root;
    std::optional<json> current;
    const json* cursor = &root;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto dot = path.find('.', start);
        const auto segment = path.substr(start, dot == std::string::npos
            ? std::string::npos : dot - start);
        if (cursor->is_object()) {
            if (!cursor->contains(segment)) return std::nullopt;
            current.emplace((*cursor)[segment]);
        } else if (cursor->is_array() && is_index(segment)) {
            std::size_t index = 0;
            for (const char digit : segment) {
                if (index > (std::numeric_limits<std::size_t>::max() -
                             static_cast<std::size_t>(digit - '0')) / 10) {
                    return std::nullopt;
                }
                index = index * 10 + static_cast<std::size_t>(digit - '0');
            }
            if (index >= cursor->size()) return std::nullopt;
            current.emplace((*cursor)[index]);
        } else {
            return std::nullopt;
        }
        cursor = &*current;
        if (dot == std::string::npos) return current;
        start = dot + 1;
    }
    return current; // Preserve the historical trailing-dot path convention.
}

/// True if `path` exists in `root`.
inline bool has_path(const json& root, const std::string& path) {
    return at_path(root, path).has_value();
}

/// Fetch value at `path` or return `default_val` on missing/wrong-type.
template<typename T>
inline T get_path(const json& root, const std::string& path, const T& default_val) {
    auto node = at_path(root, path);
    if (!node) return default_val;
    try {
        return node->template get<T>();
    } catch (...) {
        return default_val;
    }
}

namespace detail {
// Recursive walker that uses operator[] at each level. Each operator[]
// returns a write-through handle; the recursion hands that handle down
// by value (move), preserving parent info so mutations propagate upward.
inline void set_path_walk(json parent, const std::vector<std::string>& segs,
                          size_t i, const json& value) {
    if (i + 1 == segs.size()) {
        parent[segs[i]] = value;
        return;
    }
    set_path_walk(parent[segs[i]], segs, i + 1, value);
}
} // namespace detail

/// Set `value` at `path`, creating intermediate objects as needed.
inline void set_path(json& root, const std::string& path, const json& value) {
    if (path.empty()) {
        root = value;
        return;
    }
    auto segments = split_path(path);
    if (segments.size() == 1) {
        root[segments[0]] = value;
        return;
    }
    detail::set_path_walk(root[segments[0]], segments, 1, value);
}

} // namespace json_path
} // namespace neograph::llm
