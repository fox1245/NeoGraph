// neograph::history — conversation compaction + tool-pair sanitation.
// See include/neograph/history.h. Ported from NexaGraph's CAF
// compress_history actor; the actor is gone, the core is one coroutine.

#include <neograph/history.h>

#include <sstream>
#include <unordered_set>

namespace neograph::history {

int estimate_tokens(const std::vector<ChatMessage>& messages) {
    // ~3 chars/token (conservative for mixed KO/EN). Count content and
    // tool-call argument blobs — both are real prompt tokens.
    std::size_t chars = 0;
    for (const auto& m : messages) {
        chars += m.content.size();
        for (const auto& tc : m.tool_calls)
            chars += tc.name.size() + tc.arguments.size();
    }
    return static_cast<int>(chars / 3);
}

void sanitize_tool_calls(std::vector<ChatMessage>& messages) {
    // Pass 1 — drop orphaned tool responses: a role=="tool" message is
    // valid only if some *preceding* assistant message announced its
    // tool_call_id.
    std::unordered_set<std::string> announced;
    std::vector<ChatMessage> pass1;
    pass1.reserve(messages.size());
    for (auto& m : messages) {
        if (m.role == "assistant") {
            for (const auto& tc : m.tool_calls)
                if (!tc.id.empty()) announced.insert(tc.id);
            pass1.push_back(std::move(m));
        } else if (m.role == "tool") {
            if (!m.tool_call_id.empty() &&
                announced.count(m.tool_call_id))
                pass1.push_back(std::move(m));
            // else: orphaned response — drop.
        } else {
            pass1.push_back(std::move(m));
        }
    }

    // Pass 2 — drop unanswered calls: an assistant tool_call is valid
    // only if a surviving tool message responds to it. If that empties
    // tool_calls and content is also empty, drop the assistant turn.
    std::unordered_set<std::string> answered;
    for (const auto& m : pass1)
        if (m.role == "tool" && !m.tool_call_id.empty())
            answered.insert(m.tool_call_id);

    std::vector<ChatMessage> out;
    out.reserve(pass1.size());
    for (auto& m : pass1) {
        if (m.role == "assistant" && !m.tool_calls.empty()) {
            std::vector<ToolCall> kept;
            for (auto& tc : m.tool_calls)
                if (answered.count(tc.id)) kept.push_back(std::move(tc));
            m.tool_calls = std::move(kept);
            if (m.tool_calls.empty() && m.content.empty())
                continue; // nothing left in this turn
        }
        out.push_back(std::move(m));
    }
    messages = std::move(out);
}

asio::awaitable<CompactedHistory> compact_history(
    std::vector<ChatMessage> messages,
    Provider& provider,
    std::string model,
    int max_tokens,
    int recent_keep) {

    CompactedHistory result;

    if (estimate_tokens(messages) <= max_tokens) {
        result.recent = std::move(messages);
        co_return result;
    }

    const std::size_t n = messages.size();
    const std::size_t recent_start =
        n > static_cast<std::size_t>(recent_keep)
            ? n - static_cast<std::size_t>(recent_keep)
            : 0;

    // A leading system message is steering, not history — keep it.
    std::size_t compress_start = 0;
    if (!messages.empty() && messages[0].role == "system") {
        compress_start = 1;
        result.recent.push_back(messages[0]);
    }

    std::ostringstream conv;
    for (std::size_t i = compress_start; i < recent_start; ++i)
        conv << messages[i].role << ": " << messages[i].content << '\n';

    const std::string conversation = conv.str();
    if (conversation.empty()) {
        // Nothing between system and the recent window — can't compact.
        result.recent = std::move(messages);
        co_return result;
    }

    CompletionParams params;
    params.model = std::move(model);
    params.temperature = 0.2f;
    params.max_tokens = 500;
    {
        ChatMessage sys;
        sys.role = "system";
        sys.content =
            "Summarize the following conversation concisely in 3-5 "
            "sentences. Preserve key facts, user preferences, and "
            "important context. Respond in the same language as the "
            "conversation.";
        ChatMessage usr;
        usr.role = "user";
        usr.content = conversation;
        params.messages = {std::move(sys), std::move(usr)};
    }

    ChatCompletion completion = co_await provider.invoke(params, nullptr);
    const std::string& summary = completion.message.content;

    if (!summary.empty()) {
        result.summary = summary;
        result.compacted = true;
        ChatMessage sm;
        sm.role = "system";
        sm.content = "Previous conversation summary:\n" + summary;
        result.recent.push_back(std::move(sm));
    }
    // else: degraded fallback — old span is simply dropped. A shorter
    // valid history beats throwing mid-conversation.

    for (std::size_t i = recent_start; i < n; ++i)
        result.recent.push_back(std::move(messages[i]));

    // A recent_keep cut can land between an assistant tool_call and its
    // tool response; repair so the compacted list can't 400 the API.
    sanitize_tool_calls(result.recent);
    co_return result;
}

} // namespace neograph::history
