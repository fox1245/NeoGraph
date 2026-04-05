/**
 * @file llm/json_path.h
 * @brief JSON dot-path navigation utilities.
 *
 * Provides functions for navigating and manipulating JSON objects using
 * dot-notation paths like "choices.0.message.content". Numeric segments
 * are treated as array indices.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

namespace neograph::llm {

using json = nlohmann::json;

namespace json_path {

/**
 * @brief Split a dot-path string into individual segments.
 *
 * @param path Dot-separated path (e.g., "choices.0.message").
 * @return Vector of path segments (e.g., ["choices", "0", "message"]).
 */
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

/**
 * @brief Check if a string represents a non-negative integer (array index).
 * @param s String to check.
 * @return True if the string is a valid non-negative integer.
 */
inline bool is_index(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

/**
 * @brief Navigate into a JSON value by dot-path (const).
 *
 * Numeric segments index into arrays. Returns nullptr if the path
 * does not exist.
 *
 * @param root The root JSON value to navigate from.
 * @param path Dot-separated path (e.g., "choices.0.message.content").
 * @return Pointer to the value at the path, or nullptr if not found.
 */
inline const json* at_path(const json& root, const std::string& path) {
    if (path.empty()) return &root;

    auto segments = split_path(path);
    const json* current = &root;

    for (const auto& seg : segments) {
        if (current->is_object()) {
            auto it = current->find(seg);
            if (it == current->end()) return nullptr;
            current = &(*it);
        } else if (current->is_array() && is_index(seg)) {
            size_t idx = std::stoul(seg);
            if (idx >= current->size()) return nullptr;
            current = &(*current)[idx];
        } else {
            return nullptr;
        }
    }
    return current;
}

/**
 * @brief Navigate into a JSON value by dot-path (mutable).
 * @param root The root JSON value to navigate from.
 * @param path Dot-separated path.
 * @return Mutable pointer to the value at the path, or nullptr if not found.
 */
inline json* at_path_mut(json& root, const std::string& path) {
    if (path.empty()) return &root;

    auto segments = split_path(path);
    json* current = &root;

    for (const auto& seg : segments) {
        if (current->is_object()) {
            auto it = current->find(seg);
            if (it == current->end()) return nullptr;
            current = &(*it);
        } else if (current->is_array() && is_index(seg)) {
            size_t idx = std::stoul(seg);
            if (idx >= current->size()) return nullptr;
            current = &(*current)[idx];
        } else {
            return nullptr;
        }
    }
    return current;
}

/**
 * @brief Check if a dot-path exists in the JSON value.
 * @param root The root JSON value.
 * @param path Dot-separated path to check.
 * @return True if the path exists.
 */
inline bool has_path(const json& root, const std::string& path) {
    return at_path(root, path) != nullptr;
}

/**
 * @brief Get a value at a dot-path with a default fallback.
 *
 * @tparam T The expected value type.
 * @param root The root JSON value.
 * @param path Dot-separated path to navigate.
 * @param default_val Default value returned if the path doesn't exist or type conversion fails.
 * @return The value at the path, or default_val.
 */
template<typename T>
inline T get_path(const json& root, const std::string& path, const T& default_val) {
    const json* node = at_path(root, path);
    if (!node) return default_val;
    try {
        return node->get<T>();
    } catch (...) {
        return default_val;
    }
}

/**
 * @brief Set a value at a dot-path, creating intermediate objects as needed.
 *
 * Numeric segments create array entries only if the parent is already an array.
 * Otherwise, intermediate objects are created automatically.
 *
 * @param root The root JSON value to modify.
 * @param path Dot-separated path where the value should be set.
 * @param value The JSON value to set.
 */
inline void set_path(json& root, const std::string& path, const json& value) {
    if (path.empty()) {
        root = value;
        return;
    }

    auto segments = split_path(path);
    json* current = &root;

    for (size_t i = 0; i < segments.size() - 1; ++i) {
        const auto& seg = segments[i];

        if (current->is_null() || (!current->is_object() && !current->is_array())) {
            *current = json::object();
        }

        if (current->is_object()) {
            if (!current->contains(seg)) {
                (*current)[seg] = json::object();
            }
            current = &(*current)[seg];
        } else if (current->is_array() && is_index(seg)) {
            size_t idx = std::stoul(seg);
            while (current->size() <= idx) {
                current->push_back(json::object());
            }
            current = &(*current)[idx];
        }
    }

    const auto& last_seg = segments.back();
    if (current->is_null() || !current->is_object()) {
        *current = json::object();
    }
    (*current)[last_seg] = value;
}

} // namespace json_path
} // namespace neograph::llm
