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

namespace neograph { class RuntimeInterpositionController; }
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
NEOGRAPH_API int estimate_tokens(const std::vector<sp::Message>& messages);

/// Validate client-executed tool pairings without editing any message or part.
/// Orphan results, duplicate IDs, and unmatched calls throw std::invalid_argument.
NEOGRAPH_API void sanitize_tool_calls(std::vector<sp::Message>& messages);

/// Result of @ref compact_history.
struct CompactedHistory {
    std::string summary;             ///< LLM summary of the compacted span ("" if none).
    std::vector<sp::Message> recent; ///< Full original messages outside the summarized text-only prefix.
    bool compacted = false;          ///< true iff a summary was produced.
    sp::runtime::Result summary_outcome; ///< Full immutable summary response, including failure.
};

/**
 * @brief Summarize old turns when the history exceeds a token budget.
 *
 * If `estimate_tokens(messages) <= max_tokens`, returns the history
 * unchanged (`compacted == false`, `recent == messages`). Otherwise:
 *
 * Only an unsealed, text-only prefix is eligible for summarization. Every
 * non-text part and native replay group remains intact in recent. Empty
 * summaries leave the input unchanged; typed failures retain their full outcome.
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
    std::vector<sp::Message> messages,
    Provider& provider,
    std::string model,
    int max_tokens = 12000,
    int recent_keep = 6);

/** Controlled variant: summary instructions and rendered source are explicit
 * host slots; admitted RAW history remains the authoritative conversation. */
NEOGRAPH_API asio::awaitable<CompactedHistory> compact_history(
    std::vector<sp::Message> messages,
    Provider& provider,
    std::shared_ptr<::neograph::RuntimeInterpositionController> controller,
    std::string model,
    int max_tokens = 12000,
    int recent_keep = 6);

} // namespace neograph::history
