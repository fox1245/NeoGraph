/**
 * @file tool_dispatch.h
 * @brief The single implementation of "execute an assistant message's tool_calls".
 *
 * Before this existed the concept lived in two places — ToolDispatchNode (graph
 * path) and Agent (standalone path) — and they had drifted: the node fanned the
 * calls out concurrently, the agent ran them one at a time through the blocking
 * execute() facade. Measured on three 300 ms async tools: 300 ms via the node,
 * 900 ms via the agent (issue #87).
 *
 * The performance gap was the symptom. The defect was the duplication: every
 * capability added at this boundary — interception, per-call interrupt, token
 * accounting — would have landed in one path and silently missed the other,
 * exactly as concurrency did. Both callers now route through the function below
 * and nowhere else.
 *
 * **Concurrency contract.** Calls are launched together, so a tool that really
 * suspends (one that overrides `execute_async`) overlaps with its siblings and
 * may be entered concurrently with itself if the model asked for it twice. A
 * tool that implements only the sync `execute()` never overlaps: the default
 * `Tool::execute_async` body runs to completion before the coroutine's first
 * suspension, so the calls serialize even inside the parallel group. That is
 * why unifying the two paths cannot introduce a data race into an existing sync
 * tool — see ToolDispatchParity.SyncToolsDoNotOverlapOnToolNode, which pins the
 * behavior at 900 ms for three 300 ms sync tools.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/types.h>
#include <neograph/tool.h>

#include <asio/awaitable.hpp>

#include <vector>

namespace neograph {

/**
 * @brief Execute every tool call in @p calls and return one tool message each.
 *
 * Results come back in **call order**, not completion order, so the caller's
 * message history is deterministic regardless of which tool finished first.
 *
 * Never throws on a per-call failure. A call naming an unregistered tool, an
 * unparseable argument payload, or a tool that throws all yield a tool message
 * whose content is an `{"error": "..."}` object — the model sees the failure and
 * can react to it, which is the behavior both callers already had.
 *
 * @param calls Tool calls from the assistant message. Taken **by value**: this
 *        is a coroutine, and a reference parameter would dangle across the first
 *        suspension.
 * @param tools Non-owning tool pointers to resolve names against. Also by value,
 *        for the same reason. The pointees must outlive the returned awaitable.
 */
NEOGRAPH_API asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools);

}  // namespace neograph
