#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <stdexcept>

namespace neograph::llm {

using json = nlohmann::json;

namespace json_path {

// Split a dot-path string into segments: "choices.0.message" -> ["choices","0","message"]
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

// Check if a string is a non-negative integer
inline bool is_index(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Navigate into a JSON value by dot-path. Numeric segments index into arrays.
// Returns nullptr (json) if path not found.
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

// Mutable version of at_path
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

// Check if a path exists in the JSON
inline bool has_path(const json& root, const std::string& path) {
    return at_path(root, path) != nullptr;
}

// Get a value at a path, with a default if not found
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

// Set a value at a dot-path, creating intermediate objects as needed.
// Numeric segments create array entries only if the parent is already an array.
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
