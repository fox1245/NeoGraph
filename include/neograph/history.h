/**
 * @file history.h
 * @brief Conversation-history compaction + tool-pair sanitation.
 *
 * Long-running chat sessions grow unbounded; eventually the message
 * list exceeds the model's context window (or your token budget). The
 * usual fix is "summarize the old turns, keep the last N verbatim".
 * `compact_history` does exactly that with one LLM call.
 *
 * Truncating / windowing a message list routinely breaks an OpenAI
 * invariant: every assistant `tool_calls` must be followed by matching
 * role=="tool" responses, and every tool message must follow its call.
 * `sanitize_tool_calls` repairs a list in place so a compacted (or
 * otherwise sliced) history doesn't 400 the API.
 *
 * Ported from NexaGraph's CAF `compress_history` actor, with the actor
 * machinery dropped — the core is a single coroutine over Provider.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/provider.h>
#include <neograph/types.h>

#include <asio/awaitable.hpp>

#include <string>
#include <vector>

namespace neograph::history {

/**
 * @brief Rough token estimate for a message list.
 *
 * ~3 chars/token, deliberately conservative for mixed Korean/English
 * (Korean is ~2 chars/token, English ~4; 3 over-counts English so the
 * budget triggers early rather than late). This is a heuristic for
 * deciding *when* to compact, not an exact tokenizer.
 *
 * @param messages Conversation history.
 * @return Estimated token count.
 */
NEOGRAPH_API int estimate_tokens(const std::vector<ChatMessage>& messages);

/**
 * @brief Repair OpenAI-invalid tool pairings in place.
 *
 * Drops, in a single pass:
 *  - an assistant message's `tool_calls` entries that have no following
 *    matching role=="tool" response (the response was sliced off);
 *    if that empties `tool_calls` and `content` is also empty, the
 *    assistant message itself is removed, and
 *  - a role=="tool" message whose `tool_call_id` has no preceding
 *    assistant `tool_calls` entry (an orphaned response).
 *
 * Idempotent: running it twice is a no-op on the second pass. Call it
 * after any operation that windows/truncates a history (including
 * @ref compact_history, which calls it internally on its output).
 *
 * @param[in,out] messages History to repair.
 */
NEOGRAPH_API void sanitize_tool_calls(std::vector<ChatMessage>& messages);

/// Result of @ref compact_history.
struct CompactedHistory {
    std::string summary;             ///< LLM summary of the compacted span ("" if none).
    std::vector<ChatMessage> recent; ///< Leading system (if any) + summary-as-system + last N.
    bool compacted = false;          ///< true iff a summary was produced.
};

/**
 * @brief Summarize old turns when the history exceeds a token budget.
 *
 * If `estimate_tokens(messages) <= max_tokens`, returns the history
 * unchanged (`compacted == false`, `recent == messages`). Otherwise:
 *
 *  1. A leading system message (if present) is preserved verbatim.
 *  2. The last `recent_keep` messages are kept verbatim.
 *  3. Everything in between is rendered to text and summarized via one
 *     `provider.invoke(params, nullptr)` call (temperature 0.2).
 *  4. The result is `[system?] + [summary-as-system] + [last N]`, then
 *     run through @ref sanitize_tool_calls so a cut that lands mid
 *     tool-pair can't produce an API-invalid list.
 *
 * If the summary call yields empty content, falls back to dropping the
 * old span entirely (system + last N) rather than throwing — a degraded
 * but valid history beats a hard failure mid-conversation.
 *
 * `messages` is taken by value so the caller's vector is untouched.
 *
 * @param messages   Full conversation history.
 * @param provider   LLM used to produce the summary.
 * @param model      Model name passed to the summary completion.
 * @param max_tokens Compact only when the estimate exceeds this.
 * @param recent_keep Number of trailing messages kept verbatim.
 * @return Awaitable yielding the (possibly) compacted history.
 */
NEOGRAPH_API asio::awaitable<CompactedHistory> compact_history(
    std::vector<ChatMessage> messages,
    Provider& provider,
    std::string model,
    int max_tokens = 12000,
    int recent_keep = 6);

} // namespace neograph::history
