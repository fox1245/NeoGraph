/**
 * @file sse_parser.h
 * @brief Incremental Server-Sent Events parser.
 *
 * Stage 3 / Semester 1.4 — decodes the SSE wire format that
 * OpenAI's /v1/chat/completions and Anthropic's /v1/messages stream
 * over HTTP/1.1 chunked transport.
 *
 * Pure state machine: bytes in (`feed`), complete events out
 * (`drain`). The caller pairs this with a transport that delivers
 * byte chunks (typically `async_post_stream`), since one HTTP chunk
 * may contain partial, whole, or multiple SSE events.
 *
 * Not supported on purpose (per SSE spec §9 we skip the bits LLM
 * providers don't emit):
 *   - retry field → we don't auto-reconnect.
 *   - BOM detection on first byte.
 *   - Unicode line separator U+2028/U+2029.
 *
 * What's included is exactly what the two provider streams use.
 */
#pragma once

#include <neograph/api.h>

#include <string>
#include <string_view>
#include <vector>

namespace neograph::async {

struct SseEvent {
    std::string event;   ///< "event:" field. Empty ⇒ default ("message").
    std::string data;    ///< "data:" field, lines joined with '\n'.
    std::string id;      ///< "id:" field.
};

class NEOGRAPH_API SseEventParser {
public:
    /// Append raw bytes from the wire. Line terminators may be split
    /// across calls — the parser holds partial lines internally.
    void feed(std::string_view bytes);

    /// Return every event that completed since the last drain().
    /// After this call, in-progress (partial) event state is retained.
    std::vector<SseEvent> drain();

    /// Discard any partial event state. Call when the underlying
    /// stream ends mid-event and the partial should not bleed into
    /// a future reused session.
    void reset() noexcept;

private:
    void   consume_line(std::string_view line);
    void   finish_event();

    std::string              raw_;              ///< bytes awaiting line split
    std::string              cur_data_;         ///< accumulated data: lines
    std::string              cur_event_;        ///< last event: value
    std::string              cur_id_;           ///< last id: value
    bool                     cur_in_progress_ = false;
    std::vector<SseEvent>    pending_;
};

}  // namespace neograph::async
