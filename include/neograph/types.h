// Portable NeoGraph projections and full typed SDK outcome consumer helpers.
#pragma once

#include <algorithm>
#include <limits>
#include <map>
#include <unordered_set>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <neograph/json.h>
#include <core/value.h>
#include <neograph/provider_outcome_codec.h>
#include <json/json.h>

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

/// Generated provider media. At least one payload/reference field is populated;
/// a provider may supply both a URL and file handle for one artifact.
/// Metadata preserves provider fields not represented by the common contract.
struct GeneratedArtifact {
    std::string kind;         ///< "image", "video", or "file".
    std::string mime_type;
    std::string base64_data;  ///< Encoded bytes, not decoded or copied into a second buffer.
    std::string url;
    std::string file_id;
    json metadata = json::object();
};


// Provider reports and conservative budget authority are intentionally separate.
class NEOGRAPH_API UsageAccumulator {
public:
    struct AuthoritySnapshot {
        std::uint64_t charged = 0, reserved = 0;
        std::vector<std::string> provider_effects;
        sp::Usage reports;
        bool has_report = false;
    };
    AuthoritySnapshot authority_snapshot() const {
        std::lock_guard lock(mutex_);
        AuthoritySnapshot result;
        result.charged = charged_;
        result.reserved = reserved_;
        result.provider_effects.assign(provider_effects_.begin(), provider_effects_.end());
        std::sort(result.provider_effects.begin(), result.provider_effects.end());
        result.reports = reports_;
        result.has_report = has_report_;
        return result;
    }
    // Recorded playback may observe authenticated custody, never spend/refund it.
    void seal_observation() {
        std::lock_guard lock(mutex_);
        observation_only_ = true;
    }
    // Caller must authenticate custody and original scope/ceiling first.
    // Restoration is one-shot into a pristine bank, never replacement/reset.
    void restore_authority(AuthoritySnapshot snapshot) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        if (authority_restored_ || charged_ != 0 || reserved_ != 0 || has_report_ || !provider_effects_.empty())
            throw std::logic_error("Spending authority may only restore into a pristine bank");
        std::unordered_set<std::string> effects;
        effects.reserve(snapshot.provider_effects.size());
        for (auto& key : snapshot.provider_effects)
            if (!effects.insert(std::move(key)).second)
                throw std::invalid_argument("Spending authority snapshot has duplicate effect identities");
        charged_ = snapshot.charged;
        reserved_ = snapshot.reserved;
        reports_ = std::move(snapshot.reports);
        has_report_ = snapshot.has_report;
        provider_effects_ = std::move(effects);
        authority_restored_ = true;
    }
    bool remember_provider_effect(std::string key) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        return provider_effects_.insert(std::move(key)).second;
    }
    void observe(const sp::Usage& usage) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        add_report(usage);
    }
    void add(const sp::Usage& usage) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        add_report(usage);
        if (const auto charge = final_charge(usage)) charged_ = sum(charged_, *charge);
    }
    bool try_reserve(std::uint64_t tokens, std::uint64_t ceiling) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        if (tokens == 0 || ceiling == 0) return false;
        const auto committed = sum(charged_, reserved_);
        if (committed > ceiling || tokens > ceiling - committed) return false;
        reserved_ = sum(reserved_, tokens);
        return true;
    }
    // Only for operations proven not dispatched. Unknown delivery retains authority.
    void release_reservation(std::uint64_t tokens) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        reserved_ -= std::min(reserved_, tokens);
    }
    void settle_reservation(std::uint64_t reserved, const sp::Usage& usage) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        add_report(usage);
        const auto charge = final_charge(usage);
        if (!charge) return;
        const auto held = std::min(reserved_, reserved);
        reserved_ -= held;
        charged_ = sum(charged_, *charge);
    }
    sp::Usage snapshot() const {
        std::lock_guard lock(mutex_);
        return reports_;
    }
    std::uint64_t total_tokens_wide() const noexcept {
        std::lock_guard lock(mutex_);
        return sum(charged_, reserved_);
    }
    // Only trusted durable journals restore conservative spending authority.
    // This neither invents a provider report nor releases a reservation.
    void restore_charge(std::uint64_t amount) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        charged_ = sum(charged_, amount);
    }
    void restore_reservation(std::uint64_t amount) {
        std::lock_guard lock(mutex_);
        require_live_authority_locked();
        reserved_ = sum(reserved_, amount);
    }
    static std::optional<std::uint64_t> conservative_final_charge(const sp::Usage& usage) {
        return final_charge(usage);
    }
private:
    void require_live_authority_locked() const {
        if (observation_only_)
            throw std::logic_error("Recorded replay bank is observation-only");
    }
    static std::uint64_t sum(std::uint64_t a, std::uint64_t b) noexcept {
        return b > std::numeric_limits<std::uint64_t>::max() - a
            ? std::numeric_limits<std::uint64_t>::max() : a + b;
    }
    static std::optional<std::uint64_t> final_charge(const sp::Usage& usage) {
        if (usage.stage != sp::UsageStage::Final || usage.quality != sp::UsageQuality::Consistent ||
            !usage.input_total || !usage.output_total) return {};
        const auto input = usage.input_total->value, output = usage.output_total->value;
        if (output > std::numeric_limits<std::uint64_t>::max() - input) return {};
        const auto total = input + output;
        if ((usage.total && usage.total->value < total) ||
            (usage.provider_reported_total && usage.provider_reported_total->value < total)) return {};
        return std::max({total, usage.total ? usage.total->value : total,
            usage.provider_reported_total ? usage.provider_reported_total->value : total});
    }
    void add_report(const sp::Usage& usage) {
        if (!has_report_) { reports_ = usage; has_report_ = true; return; }
        // This bank aggregates tokens, not monetary reports. On a second report,
        // the first call's cost is no longer a valid snapshot of the aggregate.
        // Exact money evidence remains in each retained provider outcome.
        if (!provider_codec::provider_cost_absent(reports_.provider_cost))
            reports_.provider_cost = {};
        auto fold = [&](std::string_view counter, std::optional<sp::Count>& target,
                        const std::optional<sp::Count>& value) {
            if (!target || !value) { target.reset(); return; }
            if (value->value > std::numeric_limits<std::uint64_t>::max() - target->value) {
                target.reset();
                reports_.quality = sp::UsageQuality::Inconsistent;
                reports_.conflicts.push_back({std::string(counter), "aggregate counter overflow"});
                return;
            }
            target->value += value->value;
            target->evidence = sp::Evidence::Derived;
        };
        fold("input_total", reports_.input_total, usage.input_total);
        fold("output_total", reports_.output_total, usage.output_total);
        fold("total", reports_.total, usage.total);
        fold("provider_reported_total", reports_.provider_reported_total, usage.provider_reported_total);
        fold("input_uncached", reports_.input_uncached, usage.input_uncached);
        fold("cache_read", reports_.cache_read, usage.cache_read);
        fold("cache_write", reports_.cache_write, usage.cache_write);
        fold("reasoning", reports_.reasoning, usage.reasoning);
        for (auto entry = reports_.extra.begin(); entry != reports_.extra.end();) {
            const auto value = usage.extra.find(entry->first);
            if (value == usage.extra.end()) { entry = reports_.extra.erase(entry); continue; }
            if (value->second.value > std::numeric_limits<std::uint64_t>::max() - entry->second.value) {
                reports_.quality = sp::UsageQuality::Inconsistent;
                reports_.conflicts.push_back({entry->first, "aggregate counter overflow"});
                entry = reports_.extra.erase(entry);
                continue;
            }
            entry->second.value += value->second.value;
            entry->second.evidence = sp::Evidence::Derived;
            ++entry;
        }
        if (reports_.stage == sp::UsageStage::Missing && usage.stage == sp::UsageStage::Missing)
            reports_.stage = sp::UsageStage::Missing;
        else if (reports_.stage != sp::UsageStage::Final || usage.stage != sp::UsageStage::Final)
            reports_.stage = sp::UsageStage::Partial;
        if (usage.quality == sp::UsageQuality::Inconsistent) reports_.quality = usage.quality;
        reports_.conflicts.insert(reports_.conflicts.end(), usage.conflicts.begin(), usage.conflicts.end());
    }
    mutable std::mutex mutex_;
    sp::Usage reports_;
    bool has_report_ = false;
    bool authority_restored_ = false;
    bool observation_only_ = false;
    std::uint64_t charged_ = 0, reserved_ = 0;
    std::unordered_set<std::string> provider_effects_;
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


/**
 * @brief How provider stop reasons reach callers.
 *
 * A finished model turn is a `sp::Completion` whose `stop` is an
 * `sp::StopReason`: the normalized `kind` (`sp::StopKind`) next to `raw`, the
 * vendor's own terminal string/status, which is always retained unchanged.
 * `outcome_or_throw()` throws `ProviderFailure` only for `sp::Failure`; a
 * `Completion` is returned for every kind below, `Unknown` included, so
 * callers that need success semantics must inspect `stop.kind` themselves.
 *
 *  - `EndTurn`       the model finished normally (OpenAI `stop`, Anthropic
 *                    `end_turn`, Gemini `STOP`, Responses/Interactions `completed`).
 *  - `ToolUse`       the turn ends awaiting client tool results. A normal
 *                    end (`end_turn`/`STOP`) with client tool-call intent is
 *                    normalized to this, including invalid calls; server-only calls are not.
 *  - `MaxTokens`     output limit reached (`length`, `max_tokens`, `MAX_TOKENS`,
 *                    Responses `max_output_tokens`, Interactions `incomplete`).
 *                    A tool call cut by the limit arrives as an `InvalidToolCall`
 *                    part, not as a failure.
 *  - `StopSequence`  a caller stop sequence matched (Anthropic `stop_sequence`);
 *                    `StopReason::sequence` is set exactly for this kind.
 *  - `ContentFilter` the vendor filtered the output (`content_filter`, Responses
 *                    incomplete `content_filter`). Gemini safety terminals
 *                    (`SAFETY`, `RECITATION`, `BLOCKLIST`, ...) map to this kind
 *                    but the Gemini codec reports them as `sp::Failure`
 *                    (`RemoteFailure`) carrying the partial output instead.
 *  - `Refusal`       the model declined (Anthropic `refusal` with optional
 *                    `StopReason::details`, Chat `refusal`, a Responses refusal item).
 *  - `PauseTurn`     the vendor paused a long server-side turn; resend the
 *                    history to continue (Anthropic/Chat `pause_turn`).
 *                    `llm::Agent` and the deep-research and plan-execute
 *                    tool loops do this themselves within their iteration
 *                    budget; direct provider callers resend it themselves.
 *  - `ContextLimit`  the context window was exceeded mid-turn (Anthropic
 *                    `model_context_window_exceeded`, Chat `context_length_exceeded`).
 *  - `MalformedCall` the model produced an unusable tool call (Gemini
 *                    `MALFORMED_FUNCTION_CALL`, `UNEXPECTED_TOOL_CALL`,
 *                    `TOO_MANY_TOOL_CALLS`); a `Completion`, not a failure.
 *  - `Unknown`       a terminal value no table maps (for example Gemini
 *                    `OTHER`, or an endpoint-specific string). It is never
 *                    promoted to `EndTurn`; `raw` identifies it.
 *
 * Typed `Failure` (never a stop kind) is also used for OpenAI-compatible
 * `finish_reason: "error"`, Gemini `MALFORMED_RESPONSE` and
 * `MISSING_THOUGHT_SIGNATURE`, and Interactions `failed`/`cancelled`.
 */

class NEOGRAPH_API ProviderFailure final : public std::runtime_error {
public:
    explicit ProviderFailure(std::shared_ptr<const sp::Outcome> outcome)
        : std::runtime_error(std::get<sp::Failure>(*outcome).error.safe_message),
          outcome_(std::move(outcome)) {}
    const std::shared_ptr<const sp::Outcome>& outcome() const noexcept { return outcome_; }
private:
    std::shared_ptr<const sp::Outcome> outcome_;
};

inline std::shared_ptr<const sp::Outcome> outcome_or_throw(std::shared_ptr<const sp::Outcome> result) {
    if (!result) throw std::invalid_argument("Provider returned no owned outcome");
    if (std::holds_alternative<sp::Failure>(*result)) throw ProviderFailure(result);
    return result;
}
inline const std::vector<sp::Message>& outcome_messages(const sp::Outcome& outcome) noexcept {
    if (const auto* completion = std::get_if<sp::Completion>(&outcome)) return completion->messages;
    return std::get<sp::Failure>(outcome).partial.messages;
}
inline const sp::Usage& outcome_usage(const sp::Outcome& outcome) noexcept {
    if (const auto* completion = std::get_if<sp::Completion>(&outcome)) return completion->usage;
    return std::get<sp::Failure>(outcome).partial.usage;
}
inline bool provider_failure_proves_not_sent(const sp::Failure& failure) noexcept {
    const auto& attempt = failure.error.attempt;
    const auto& usage = failure.partial.usage;
    return failure.error.retry_safety == sp::RetrySafety::NotSent &&
        !attempt.request_may_have_left && attempt.request_body_bytes == 0 &&
        !attempt.response_head_seen && attempt.transport_internal_resends == 0 && !attempt.prior_usage_unknown &&
        failure.partial.messages.empty() && !failure.partial.stop && !failure.partial.wire_envelope &&
        failure.partial.raw_events.empty() && usage.stage == sp::UsageStage::Missing &&
        usage.quality == sp::UsageQuality::Consistent &&
        !usage.input_total && !usage.output_total && !usage.total && !usage.provider_reported_total &&
        !usage.input_uncached && !usage.cache_read && !usage.cache_write && !usage.reasoning &&
        usage.extra.empty() && usage.conflicts.empty() &&
        provider_codec::provider_cost_absent(usage.provider_cost);
}
inline std::string outcome_text(const sp::Outcome& outcome) {
    std::string text;
    for (const auto& message : outcome_messages(outcome))
        for (const auto& part : message.parts)
            if (const auto* value = std::get_if<sp::Text>(&part)) text += value->value;
    return text;
}
inline std::vector<ToolCall> client_tool_calls(const sp::Message& message) {
    std::vector<ToolCall> result;
    for (const auto& part : message.parts) if (const auto* call = std::get_if<sp::ToolCall>(&part);
        call && call->kind == sp::ToolCallKind::ClientExecuted && call->input && call->input->root().is_object())
        result.push_back({call->id, call->name, call->input->root().dump()});
    return result;
}
// A completed turn the vendor paused (`sp::StopKind::PauseTurn`). The documented
// continuation resends the history, including that assistant turn, unchanged.
inline bool paused_turn(const sp::Outcome& outcome) noexcept {
    const auto* completion = std::get_if<sp::Completion>(&outcome);
    return completion && completion->stop.kind == sp::StopKind::PauseTurn;
}
inline std::vector<ToolCall> pending_client_tool_calls(const std::vector<sp::Message>& history) {
    std::map<std::string_view, const sp::ToolCall*> pending;
    std::vector<const sp::ToolCall*> order;
    for (const auto& message : history) for (const auto& part : message.parts) {
        if (const auto* call = std::get_if<sp::ToolCall>(&part);
            call && call->kind == sp::ToolCallKind::ClientExecuted && call->input &&
            call->input->root().is_object()) {
            if (!pending.emplace(call->id, call).second)
                throw std::invalid_argument("Client tool call history has duplicate unresolved identities");
            order.push_back(call);
        } else if (const auto* result = std::get_if<sp::ToolResult>(&part)) {
            pending.erase(result->tool_use_id);
        }
    }
    std::vector<ToolCall> result;
    result.reserve(pending.size());
    for (const auto* call : order) {
        const auto found = pending.find(call->id);
        if (found != pending.end() && found->second == call)
            result.push_back({call->id, call->name, call->input->root().dump()});
    }
    return result;
}
inline sp::Role portable_role(std::string_view role) {
    if (role == "system") return sp::Role::System;
    if (role == "developer") return sp::Role::Developer;
    if (role == "user") return sp::Role::User;
    if (role == "assistant") return sp::Role::Assistant;
    if (role == "tool") return sp::Role::Tool;
    throw std::invalid_argument("Invalid portable message role");
}
inline std::string_view portable_role_name(sp::Role role) {
    switch (role) {
        case sp::Role::System: return "system";
        case sp::Role::Developer: return "developer";
        case sp::Role::User: return "user";
        case sp::Role::Assistant: return "assistant";
        case sp::Role::Tool: return "tool";
    }
    throw std::invalid_argument("Invalid typed message role");
}
inline sp::Message portable_message(const ChatMessage& source) {
    if (!source.reasoning.empty() || !source.reasoning_details.empty())
        throw std::invalid_argument("Portable projection cannot import native reasoning authority");
    sp::Message message;
    message.role = portable_role(source.role);
    if (message.role == sp::Role::Tool) {
        if (!source.tool_calls.empty() || !source.image_urls.empty())
            throw std::invalid_argument("Portable tool result cannot contain calls or images");
        message.parts.emplace_back(sp::ToolResult{source.tool_call_id, source.content,
            source.tool_status != "succeeded" && !source.tool_status.empty(),
            sp::ToolResultHostMetadata{source.tool_name, source.tool_status,
                source.tool_retryable, source.tool_effect_uncertain}});
        return message;
    }
    if (!source.content.empty()) message.parts.emplace_back(sp::Text{source.content});
    for (const auto& url : source.image_urls) {
        const auto separator = url.find(";base64,");
        if (!url.starts_with("data:") || separator == std::string::npos)
            throw std::invalid_argument("Portable images require admitted inline base64, not URL fetching");
        sp::Image image;
        image.mime = url.substr(5, separator - 5);
        image.data = std::make_shared<const std::string>(url.substr(separator + 8));
        if (!sp::valid_image(image)) throw std::invalid_argument("Invalid portable inline image");
        message.parts.emplace_back(std::move(image));
    }
    for (const auto& call : source.tool_calls) {
        auto parsed = sp::json::parse(call.arguments);
        auto* document = std::get_if<sp::json::Document>(&parsed);
        if (!document || !document->root().is_object())
            throw std::invalid_argument("Portable tool call requires admitted object arguments");
        sp::ToolCall typed;
        typed.id = call.id;
        typed.name = call.name;
        typed.input = std::make_shared<const sp::json::Document>(std::move(*document));
        message.parts.emplace_back(std::move(typed));
    }
    return message;
}
// Deliberately an observational projection, never used to replay provider history.
inline ChatMessage project_message(const sp::Message& message) {
    ChatMessage result;
    result.role = portable_role_name(message.role);
    result.tool_calls = client_tool_calls(message);
    for (const auto& part : message.parts) {
        if (const auto* text = std::get_if<sp::Text>(&part)) result.content += text->value;
        else if (const auto* thinking = std::get_if<sp::Thinking>(&part)) result.reasoning += thinking->text;
        else if (const auto* tool = std::get_if<sp::ToolResult>(&part)) {
            result.tool_call_id = tool->tool_use_id;
            result.content += tool->content;
            result.tool_status = tool->is_error ? "failed" : "succeeded";
            if (tool->host) {
                result.tool_name = tool->host->name;
                result.tool_status = tool->host->status;
                result.tool_retryable = tool->host->retryable;
                result.tool_effect_uncertain = tool->host->effect_uncertain;
            }
        } else if (const auto* image = std::get_if<sp::Image>(&part); image && image->data)
            result.image_urls.push_back("data:" + image->mime + ";base64," + *image->data);
    }
    return result;
}
inline json message_projection_json(const sp::Message& message) {
    return provider_codec::encode_message(message);
}
inline json usage_to_json(const sp::Usage& usage) { return provider_codec::encode_usage(usage); }
inline json outcome_projection_json(const sp::Outcome& outcome) {
    return provider_codec::observe_outcome(outcome);
}
NEOGRAPH_API std::string message_digest(const sp::Message& message);

} // namespace neograph
