// Vendor-neutral helpers for carrying provider reasoning items through
// SchemaProvider (see ReasoningConfig). Shared by the non-stream parser, the
// SSE/WebSocket stream parser, and the request serializer so all three treat a
// carried item identically. Internal to neograph_llm; knows no vendor names.
#pragma once

#include <neograph/types.h>

#include <optional>
#include <set>
#include <string>

namespace neograph::llm::detail {

// Readable text of a carried item field: a string, or an array of objects
// with a `text` string (for example a Responses `summary`).
inline std::string reasoning_text_of(const json& value) {
    if (value.is_string()) return value.get<std::string>();
    std::string out;
    if (value.is_array()) {
        for (const auto& part : value) {
            if (part.is_object() && part.contains("text") && part["text"].is_string()) {
                if (!out.empty()) out += "\n";
                out += part["text"].get<std::string>();
            }
        }
    }
    return out;
}

inline void append_reasoning_text(ChatMessage& msg, const std::string& text) {
    if (text.empty()) return;
    if (!msg.reasoning.empty()) msg.reasoning += "\n";
    msg.reasoning += text;
}

inline void push_reasoning_detail(ChatMessage& msg, json item) {
    if (!msg.reasoning_details.is_array()) msg.reasoning_details = json::array();
    msg.reasoning_details.push_back(std::move(item));
}

// A carried item is replayed only if its `type` is one this schema declares,
// so items captured on another route are dropped instead of sent blindly.
inline bool is_carried_item(const json& item, const std::set<std::string>& carry_types) {
    return item.is_object() && item.contains("type") && item["type"].is_string() &&
           carry_types.count(item["type"].get<std::string>()) > 0;
}

// Signature captured for `tool_call_id` (see ReasoningConfig::signature_field).
inline std::optional<json> find_tool_call_signature(const json& details,
                                                   const std::string& tool_call_id) {
    if (!details.is_array()) return std::nullopt;
    for (const auto& item : details) {
        if (item.is_object() && item.value("type", std::string()) == "tool_call_signature" &&
            item.value("tool_call_id", std::string()) == tool_call_id &&
            item.contains("signature")) {
            return item["signature"];
        }
    }
    return std::nullopt;
}

// Streaming chat APIs deliver an opaque array (for example OpenRouter
// `reasoning_details`) as many small fragments. Fragments with the same
// `type` and `index` describe one item: fields named in `concat_fields` are
// concatenated, every other field keeps its latest non-empty value. Entries
// without an `index` are appended as-is.
inline void merge_reasoning_fragments(json& details, const json& fragments,
                                      const std::set<std::string>& concat_fields) {
    if (!fragments.is_array()) return;
    json merged = details.is_array() ? details : json::array();
    for (const auto& frag : fragments) {
        if (!frag.is_object()) continue;
        bool matched = false;
        if (frag.contains("index") && frag.contains("type")) {
            json next = json::array();
            for (const auto& existing : merged) {
                if (!matched && existing.is_object() && existing.contains("index") &&
                    existing["index"] == frag["index"] &&
                    existing.value("type", std::string()) == frag.value("type", std::string())) {
                    json combined = existing;
                    for (auto [key, value] : frag.items()) {
                        if (concat_fields.count(key) > 0 && value.is_string() &&
                            combined.contains(key) && combined[key].is_string()) {
                            combined[key] = combined[key].get<std::string>() + value.get<std::string>();
                        } else if (!(value.is_string() && value.get<std::string>().empty())) {
                            combined[key] = value;
                        }
                    }
                    next.push_back(std::move(combined));
                    matched = true;
                } else {
                    next.push_back(existing);
                }
            }
            merged = std::move(next);
        }
        if (!matched) merged.push_back(frag);
    }
    details = std::move(merged);
}

}  // namespace neograph::llm::detail
