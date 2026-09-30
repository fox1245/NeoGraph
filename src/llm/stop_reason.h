// Final stop-reason reconciliation shared by SchemaProvider (non-stream, SSE,
// WebSocket) and OpenAIProvider. Internal to neograph_llm.
#pragma once

#include <string>

namespace neograph::llm::detail {

// A response that carries tool calls asks the caller to run them, whatever the
// vendor's finish reason says: Gemini reports `STOP` next to a `functionCall`,
// and some OpenAI-compatible gateways send `stop` next to `tool_calls`. Left as
// `end_turn`, a loop that branches on the stop reason would treat the turn as
// finished. Only a plain `end_turn` is promoted; a truncated (`max_tokens`),
// filtered (`content_filter`) or otherwise specific reason is kept, since the
// caller needs to know why the turn ended.
inline std::string reconcile_stop_reason(std::string stop_reason, bool has_tool_calls) {
    if (has_tool_calls && stop_reason == "end_turn") return "tool_use";
    return stop_reason;
}

}  // namespace neograph::llm::detail
