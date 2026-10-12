/**
 * @file sse_parser.h
 * @brief Incremental Server-Sent Events parser (WHATWG HTML, "Server-sent
 *        events", event stream interpretation).
 *
 * Pure state machine: bytes in (`feed`), complete events out (`drain`). The
 * caller pairs this with a transport that delivers byte chunks (typically
 * `async_post_stream`), since one HTTP chunk may contain partial, whole, or
 * multiple SSE events.
 *
 * It follows the WHATWG processing rules, which MCP's Streamable HTTP
 * transport and the official MCP SDKs rely on:
 *   - lines end with CRLF, LF or CR, also when a CRLF is split across feeds;
 *   - one leading UTF-8 byte order mark is skipped;
 *   - `data` lines join with LF, and an event with no `data` line is not
 *     dispatched;
 *   - `id` sets the last event ID (ignored when it contains NUL), which every
 *     later event carries until another `id` changes it;
 *   - `retry` with only ASCII digits sets the reconnection time;
 *   - lines starting with ':' are comments, unknown fields are ignored;
 *   - an event the stream ends in the middle of is never dispatched.
 */
#pragma once

#include <neograph/api.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::async {

struct SseEvent {
    std::string event;   ///< "event:" field. Empty ⇒ default ("message").
    std::string data;    ///< "data:" field, lines joined with '\n'.
    std::string id;      ///< The last event ID when the event was dispatched.
};

struct SseParserOptions {
    std::size_t max_partial_line_bytes = 64u * 1024u;
    std::size_t max_event_bytes        = 1u * 1024u * 1024u;
    std::size_t max_pending_events     = 1024u;
    std::size_t max_pending_bytes      = 8u * 1024u * 1024u;
};

class NEOGRAPH_API SseEventParser {
public:
    /// All limits must be nonzero.
    explicit SseEventParser(SseParserOptions options = {});

    /// Append raw bytes from the wire. Line terminators may be split
    /// across calls — the parser holds partial lines internally.
    /// A limit violation throws std::length_error and leaves the parser
    /// failed until reset() is called.
    void feed(std::string_view bytes);

    /// Return every event that completed since the last drain().
    /// After this call, in-progress (partial) event state is retained.
    std::vector<SseEvent> drain();

    /// The last event ID seen so far (the `id` field), empty when none.
    const std::string& last_event_id() const noexcept { return last_id_; }

    /// The reconnection time the stream asked for with `retry`, if any.
    std::optional<std::uint64_t> retry_ms() const noexcept { return retry_ms_; }

    /// Discard all state, as for a new stream.
    void reset() noexcept;

private:
    void   consume_line(std::string_view line);
    void   finish_event();
    void   ensure_can_add(std::size_t current, std::size_t addition,
                          std::size_t limit, const char* message);
    [[noreturn]] void fail_limit(const char* message);

    SseParserOptions             options_;
    std::string                  raw_;              ///< bytes awaiting line split
    std::string                  cur_data_;         ///< accumulated data: lines
    std::string                  cur_event_;        ///< last event: value
    std::string                  last_id_;          ///< last event ID buffer
    std::optional<std::uint64_t> retry_ms_;
    std::size_t                  cur_event_bytes_ = 0;
    bool                         cur_has_data_ = false;
    bool                         skip_lf_ = false;  ///< the last feed ended in CR
    bool                         at_start_ = true;  ///< before the first byte (BOM)
    std::vector<SseEvent>        pending_;
    std::size_t                  pending_bytes_ = 0;
    bool                         failed_ = false;
};

}  // namespace neograph::async
