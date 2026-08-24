/**
 * @file types.h
 * @brief Foundation types for NeoGraph: messages, tool calls, and LLM completions.
 *
 * Defines the core data structures shared across all NeoGraph modules,
 * including ChatMessage, ToolCall, ChatCompletion, and their JSON
 * serialization helpers (ADL-based, nlohmann/json compatible).
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <neograph/json.h>

namespace neograph {

/**
 * @brief Represents a single tool invocation requested by the LLM.
 *
 * When an LLM response contains tool calls, each call is represented
 * as a ToolCall with a unique ID, the tool name, and its arguments
 * serialized as a JSON string.
 */
struct ToolCall {
    std::string id;         ///< Unique identifier for this tool call.
    std::string name;       ///< Name of the tool to invoke.
    std::string arguments;  ///< JSON-encoded string of tool arguments.
};

/**
 * @brief A message in the conversation history.
 *
 * Supports all standard roles (user, assistant, tool, system) and
 * multi-modal content via image_urls for vision-capable models.
 */
struct ChatMessage {
    std::string role;                    ///< Message role: "user", "assistant", "tool", or "system".
    std::string content;                 ///< Text content of the message.
    std::vector<ToolCall> tool_calls;    ///< Tool calls made by the assistant (if any).
    std::string tool_call_id;            ///< ID of the tool call being responded to (role == "tool").
    std::string tool_name;               ///< Name of the tool being called.
    /// Typed terminal status emitted by the shared ToolExecutionController.
    std::string tool_status;
    bool tool_retryable = false;
    bool tool_effect_uncertain = false;
    std::vector<std::string> image_urls; ///< Base64 data URLs or HTTP URLs for vision support.
    /// Provider-returned reasoning text. Keep separate from user-visible content.
    std::string reasoning;
    /// Opaque provider-native continuation blocks replayed only on a compatible route.
    json reasoning_details = json::array();
};

/**
 * @brief Tool definition metadata sent to the LLM.
 *
 * Describes a callable tool with its name, description, and parameter
 * schema (JSON Schema object) so the LLM can decide when and how to call it.
 */
struct ChatTool {
    std::string name;         ///< Tool name (must be unique within a session).
    std::string description;  ///< Human-readable description of what the tool does.
    json parameters;          ///< JSON Schema object describing the tool's parameters.
};

/**
 * @brief LLM completion response including the message and token usage.
 */
struct ChatCompletion {
    ChatMessage message;  ///< The response message from the LLM.

    /// Normalized reason the provider stopped: `end_turn`, `max_tokens`,
    /// `stop_sequence`, `tool_use`, `content_filter`, `refusal`, or `unknown`.
    /// Adding this field changes the C++ ABI; recompile consumers with this release.
    std::string stop_reason = "unknown";

    /// Token usage statistics for the completion.
    struct Usage {
        int prompt_tokens = 0;      ///< Number of tokens in the prompt.
        int completion_tokens = 0;  ///< Number of tokens in the completion.
        int total_tokens = 0;       ///< Total tokens used (prompt + completion).
        int cached_prompt_tokens = 0; ///< Prompt-token subset served from cache.
        int reasoning_tokens = 0;   ///< Completion-token subset spent on reasoning.
    } usage;
};

/**
 * @brief Running total of the token usage of a graph run (issue #88).
 *
 * One of these rides on `RunContext` for the length of a run, exactly as the
 * cancel token does, and is surfaced as `RunResult::usage` when the run ends.
 * It is shared — by the parent run and every subgraph beneath it, and by every
 * branch of a fan-out — so the counters are atomic.
 *
 * **Where it gets fed.** At the node that *receives* a completion, never at the
 * provider that produced it. `RateLimitedProvider` wraps another provider and
 * delegates to it, so a provider-layer counter would count the same completion
 * once per layer. A completion reaches a node exactly once, whatever it went
 * through on the way.
 */
class NEOGRAPH_API UsageAccumulator {
public:
    /// Fold one completion's usage into the running total.
    ///
    /// Providers that report only `prompt_tokens` and `completion_tokens` and
    /// leave `total_tokens` at zero are normalized here rather than at every
    /// call site. Invalid negative values are ignored, and a total smaller
    /// than the component sum is promoted to that sum so a provider cannot
    /// bypass a model-token ceiling by under-reporting usage.
    void add(const ChatCompletion::Usage& u) {
        std::lock_guard lock(mutex_);
        add_locked(u);
    }

    /// Reserve tokens before dispatching a bounded provider request.
    bool try_reserve(long long tokens, long long ceiling) {
        if (tokens <= 0 || ceiling <= 0) return false;
        std::lock_guard lock(mutex_);
        const auto actual    = total_.load(std::memory_order_relaxed);
        const auto committed = saturating_sum(actual, reserved_);
        if (committed > ceiling || tokens > ceiling - committed) return false;
        reserved_ = saturating_sum(reserved_, tokens);
        return true;
    }

    /// Release a reservation when a provider request is not dispatched.
    ///
    /// The separate reservation count makes an over-sized release harmless:
    /// it can consume only reservations, never usage already reported by a provider.
    void release_reservation(long long tokens) {
        if (tokens <= 0) return;
        std::lock_guard lock(mutex_);
        const auto released = std::min(tokens, reserved_);
        reserved_ -= released;
    }

    /// Replace a reservation with the provider's actual usage.
    void settle_reservation(long long reserved,
                            const ChatCompletion::Usage& u) {
        std::lock_guard lock(mutex_);
        const long long requested = std::max(0LL, reserved);
        const long long held      = std::min(requested, reserved_);
        if (held != 0) reserved_ -= held;
        add_locked(u);
    }

    /// Read the running total. Not a consistent snapshot across the three
    /// counters under concurrent writes — read it when the run is done.
    ChatCompletion::Usage snapshot() const noexcept {
        ChatCompletion::Usage u;
        u.prompt_tokens     = public_counter(prompt_.load(std::memory_order_relaxed));
        u.completion_tokens = public_counter(completion_.load(std::memory_order_relaxed));
        u.total_tokens      = public_counter(total_.load(std::memory_order_relaxed));
        return u;
    }

    /// Read the wide running total for hard budget comparisons.
    long long total_tokens_wide() const noexcept {
        return total_.load(std::memory_order_relaxed);
    }

private:
    static long long nonnegative(int value) noexcept {
        return value > 0 ? static_cast<long long>(value) : 0;
    }

    static long long normalized_total(const ChatCompletion::Usage& u,
                                      long long                    prompt,
                                      long long                    completion) noexcept {
        const long long components =
            prompt > std::numeric_limits<long long>::max() - completion
                ? std::numeric_limits<long long>::max()
                : prompt + completion;
        const long long reported = u.total_tokens > 0
                                        ? static_cast<long long>(u.total_tokens)
                                        : 0;
        return std::max(reported, components);
    }

    static long long saturating_sum(long long current, long long delta) noexcept {
        if (current < 0) current = 0;
        if (delta <= 0) return current;
        const auto available = std::numeric_limits<long long>::max() - current;
        return delta > available ? std::numeric_limits<long long>::max()
                                 : current + delta;
    }

    void add_locked(const ChatCompletion::Usage& u) {
        const long long prompt     = nonnegative(u.prompt_tokens);
        const long long completion = nonnegative(u.completion_tokens);
        const long long total      = normalized_total(u, prompt, completion);
        add_counters_locked(prompt, completion, total);
    }

    void add_counters_locked(long long prompt,
                             long long completion,
                             long long total) {
        prompt_.store(saturating_sum(prompt_.load(std::memory_order_relaxed), prompt),
                      std::memory_order_relaxed);
        completion_.store(
            saturating_sum(completion_.load(std::memory_order_relaxed), completion),
            std::memory_order_relaxed);
        total_.store(saturating_sum(total_.load(std::memory_order_relaxed), total),
                     std::memory_order_relaxed);
    }

    static int public_counter(long long value) noexcept {
        if (value <= 0) return 0;
        const auto maximum = static_cast<long long>(std::numeric_limits<int>::max());
        return static_cast<int>(std::min(value, maximum));
    }

    mutable std::mutex     mutex_;
    std::atomic<long long> prompt_{0};
    std::atomic<long long> completion_{0};
    std::atomic<long long> total_{0};
    long long              reserved_ = 0;
};

// --- ADL serialization: ChatMessage/ToolCall <-> json ---
// These live in the same namespace as the types for ADL lookup.

/// @brief Serialize a ToolCall to JSON.
/// @param[out] j Target JSON object.
/// @param[in] tc ToolCall to serialize.
inline void to_json(json& j, const ToolCall& tc) {
    j = json{{"id", tc.id}, {"name", tc.name}, {"arguments", tc.arguments}};
}

/// @brief Deserialize a ToolCall from JSON.
/// @param[in] j Source JSON object.
/// @param[out] tc Target ToolCall.
inline void from_json(const json& j, ToolCall& tc) {
    tc.id = j.value("id", "");
    tc.name = j.value("name", "");
    tc.arguments = j.value("arguments", "");
}

/// @brief Serialize a ChatMessage to JSON.
/// @param[out] j Target JSON object.
/// @param[in] msg ChatMessage to serialize.
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
    if (!msg.tool_status.empty())  j["tool_status"] = msg.tool_status;
    if (msg.tool_retryable)        j["tool_retryable"] = true;
    if (msg.tool_effect_uncertain) j["tool_effect_uncertain"] = true;
    if (!msg.image_urls.empty())   j["image_urls"] = msg.image_urls;
    if (!msg.reasoning.empty())    j["reasoning"] = msg.reasoning;
    if (!msg.reasoning_details.empty()) j["reasoning_details"] = msg.reasoning_details;
}

/// @brief Deserialize a ChatMessage from JSON.
/// @param[in] j Source JSON object.
/// @param[out] msg Target ChatMessage.
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
    msg.tool_status  = j.value("tool_status", "");
    msg.tool_retryable = j.value("tool_retryable", false);
    msg.tool_effect_uncertain = j.value("tool_effect_uncertain", false);
    if (j.contains("image_urls") && j["image_urls"].is_array()) {
        msg.image_urls = j["image_urls"].get<std::vector<std::string>>();
    }
    msg.reasoning = j.value("reasoning", "");
    if (j.contains("reasoning_details") && !j["reasoning_details"].is_array()) {
        throw std::invalid_argument("ChatMessage reasoning_details must be an array");
    }
    msg.reasoning_details = j.contains("reasoning_details")
        ? j["reasoning_details"]
        : json::array();
}

// --- JSON serialization helpers ---

/**
 * @brief Convert a vector of ChatMessages to OpenAI-compatible JSON format.
 *
 * Handles tool call messages, tool result messages, and multi-modal
 * messages (text + images in OpenAI Vision format).
 *
 * @param messages Vector of ChatMessage objects to convert.
 * @return JSON array in OpenAI messages format.
 */
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

        if (msg.role == "assistant") {
            if (!msg.reasoning_details.empty()) {
                if (!msg.reasoning_details.is_array()) {
                    throw std::invalid_argument(
                        "ChatMessage reasoning_details must be an array");
                }
                j["reasoning_details"] = msg.reasoning_details;
            } else if (!msg.reasoning.empty()) {
                j["reasoning_content"] = msg.reasoning;
            }
        }

        arr.push_back(j);
    }
    return arr;
}

/**
 * @brief Convert a vector of ChatTools to OpenAI-compatible JSON format.
 *
 * @param tools Vector of ChatTool objects to convert.
 * @return JSON array in OpenAI tool definition format.
 */
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

/**
 * @brief Parse an OpenAI API response choice into a ChatMessage.
 *
 * Extracts the message content, role, and any tool calls from
 * the `choices[n]` object of an OpenAI completion response.
 *
 * @param choice A single choice object from the OpenAI response (must contain "message").
 * @return Parsed ChatMessage with role, content, and tool_calls populated.
 * @throws json::exception If required fields are missing.
 */
inline ChatMessage parse_response_message(const json& choice) {
    ChatMessage msg;
    auto m = choice.at("message");
    msg.role = m.value("role", "assistant");
    msg.content = (m.contains("content") && !m["content"].is_null())
                  ? m["content"].get<std::string>() : "";
    if (m.contains("reasoning") && m["reasoning"].is_string()) {
        msg.reasoning = m["reasoning"].get<std::string>();
    } else if (m.contains("reasoning_content") &&
               m["reasoning_content"].is_string()) {
        msg.reasoning = m["reasoning_content"].get<std::string>();
    }
    if (m.contains("reasoning_details") && m["reasoning_details"].is_array()) {
        msg.reasoning_details = m["reasoning_details"];
    }

    if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
        for (const auto& tc : m["tool_calls"]) {
            ToolCall call;
            call.id = tc.value("id", "");
            auto fn = tc.at("function");
            call.name = fn.value("name", "");
            call.arguments = fn.value("arguments", "");
            msg.tool_calls.push_back(std::move(call));
        }
    }

    return msg;
}

} // namespace neograph
