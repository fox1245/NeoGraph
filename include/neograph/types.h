#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace neograph {

using json = nlohmann::json;

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments; // JSON string
};

struct ChatMessage {
    std::string role;
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::string tool_call_id;
    std::string tool_name;
    std::vector<std::string> image_urls; // base64 data URLs or http URLs for Vision
};

struct ChatTool {
    std::string name;
    std::string description;
    json parameters; // JSON Schema object
};

struct ChatCompletion {
    ChatMessage message;
    struct Usage {
        int prompt_tokens = 0;
        int completion_tokens = 0;
        int total_tokens = 0;
    } usage;
};

// --- ADL serialization: ChatMessage/ToolCall <-> json ---
// These live in the same namespace as the types for ADL lookup.

inline void to_json(json& j, const ToolCall& tc) {
    j = json{{"id", tc.id}, {"name", tc.name}, {"arguments", tc.arguments}};
}

inline void from_json(const json& j, ToolCall& tc) {
    tc.id = j.value("id", "");
    tc.name = j.value("name", "");
    tc.arguments = j.value("arguments", "");
}

inline void to_json(json& j, const ChatMessage& msg) {
    j["role"] = msg.role;
    j["content"] = msg.content;
    if (!msg.tool_calls.empty()) {
        j["tool_calls"] = json::array();
        for (const auto& tc : msg.tool_calls) {
            json tc_j;
            to_json(tc_j, tc);
            j["tool_calls"].push_back(tc_j);
        }
    }
    if (!msg.tool_call_id.empty()) j["tool_call_id"] = msg.tool_call_id;
    if (!msg.tool_name.empty())    j["tool_name"] = msg.tool_name;
    if (!msg.image_urls.empty())   j["image_urls"] = msg.image_urls;
}

inline void from_json(const json& j, ChatMessage& msg) {
    msg.role    = j.value("role", "");
    msg.content = j.value("content", "");
    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        for (const auto& tc_j : j["tool_calls"]) {
            ToolCall tc;
            from_json(tc_j, tc);
            msg.tool_calls.push_back(tc);
        }
    }
    msg.tool_call_id = j.value("tool_call_id", "");
    msg.tool_name    = j.value("tool_name", "");
    if (j.contains("image_urls") && j["image_urls"].is_array()) {
        msg.image_urls = j["image_urls"].get<std::vector<std::string>>();
    }
}

// --- JSON serialization helpers ---

inline json messages_to_json(const std::vector<ChatMessage>& messages) {
    json arr = json::array();
    for (const auto& msg : messages) {
        json j;
        j["role"] = msg.role;

        if (msg.role == "tool") {
            j["content"] = msg.content;
            j["tool_call_id"] = msg.tool_call_id;
        } else if (!msg.tool_calls.empty()) {
            j["content"] = msg.content.empty() ? json(nullptr) : json(msg.content);
            json tc_arr = json::array();
            for (const auto& tc : msg.tool_calls) {
                tc_arr.push_back({
                    {"id", tc.id},
                    {"type", "function"},
                    {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}
                });
            }
            j["tool_calls"] = tc_arr;
        } else if (!msg.image_urls.empty()) {
            // Multi-modal: text + images (OpenAI Vision format)
            json parts = json::array();
            if (!msg.content.empty()) {
                parts.push_back({{"type", "text"}, {"text", msg.content}});
            }
            for (auto& url : msg.image_urls) {
                parts.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
            }
            j["content"] = parts;
        } else {
            j["content"] = msg.content;
        }

        arr.push_back(j);
    }
    return arr;
}

inline json tools_to_json(const std::vector<ChatTool>& tools) {
    json arr = json::array();
    for (const auto& tool : tools) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.parameters}
            }}
        });
    }
    return arr;
}

inline ChatMessage parse_response_message(const json& choice) {
    ChatMessage msg;
    auto& m = choice.at("message");
    msg.role = m.value("role", "assistant");
    msg.content = (m.contains("content") && !m["content"].is_null())
                  ? m["content"].get<std::string>() : "";

    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            auto& fn = tc.at("function");
            call.name = fn.value("name", "");
            call.arguments = fn.value("arguments", "");
            msg.tool_calls.push_back(std::move(call));
        }
    }

    return msg;
}

} // namespace neograph
