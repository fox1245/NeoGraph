// Incremental SSE parser following the WHATWG event stream interpretation
// rules (https://html.spec.whatwg.org/multipage/server-sent-events.html,
// "Parsing an event stream" and "Interpreting an event stream").
//
// State machine:
//
//   bytes → raw_ buffer, one leading UTF-8 BOM skipped.
//   For each line terminated by CRLF, LF or CR (a CR at the end of one feed
//   and an LF at the start of the next are one CRLF):
//     - empty line → dispatch the event.
//     - ":prefix"  → comment, ignored.
//     - "name:val" → process the field; one space after the colon is
//                    dropped. A line without a colon is a field with an
//                    empty value.
//
//   Fields: `data` appends to the data buffer (lines joined with LF),
//   `event` sets the event type, `id` sets the last event ID unless the value
//   contains NUL, `retry` with only ASCII digits sets the reconnection time.
//
//   Dispatch: an event with no data line is not dispatched (its event type is
//   dropped); every dispatched event carries the last event ID, which
//   persists across events.

#include <neograph/async/sse_parser.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neograph::async {

namespace {

constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

std::pair<std::string_view, std::string_view>
split_field(std::string_view line) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return { line, {} };
    }
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    return { name, value };
}

}  // namespace

SseEventParser::SseEventParser(SseParserOptions options)
    : options_(options) {
    if (options_.max_partial_line_bytes == 0 ||
        options_.max_event_bytes == 0 ||
        options_.max_pending_events == 0 ||
        options_.max_pending_bytes == 0) {
        throw std::invalid_argument("SSE parser limits must be nonzero");
    }
}

void SseEventParser::feed(std::string_view bytes) {
    if (failed_) {
        throw std::length_error(
            "SSE parser is in a failed state; call reset() before reuse");
    }
    if (at_start_) {
        // Decoding strips one leading byte order mark. Bytes that may still
        // become one wait in raw_, which is empty before the first line.
        while (!bytes.empty() && raw_.size() < kByteOrderMark.size() &&
               bytes.front() == kByteOrderMark[raw_.size()]) {
            raw_.push_back(bytes.front());
            bytes.remove_prefix(1);
        }
        if (raw_.size() == kByteOrderMark.size()) {
            raw_.clear();
            at_start_ = false;
        } else if (!bytes.empty()) {
            // Not a byte order mark: what was held is ordinary input.
            at_start_ = false;
            std::string held;
            held.swap(raw_);
            if (!held.empty()) feed(held);
        } else {
            return;
        }
    }
    if (skip_lf_ && !bytes.empty()) {
        skip_lf_ = false;
        if (bytes.front() == '\n') bytes.remove_prefix(1);
    }

    // Scan the input before copying it so a chunk containing many bounded
    // lines does not require an equally large temporary raw_ allocation.
    while (!bytes.empty()) {
        const auto end = bytes.find_first_of("\r\n");
        const auto fragment = bytes.substr(0, end);
        ensure_can_add(raw_.size(), fragment.size(),
                       options_.max_partial_line_bytes,
                       "SSE partial line exceeds configured byte limit");

        if (end == std::string_view::npos) {
            raw_.append(fragment.data(), fragment.size());
            return;
        }

        std::string_view line;
        if (raw_.empty()) {
            line = fragment;
        } else {
            raw_.append(fragment.data(), fragment.size());
            line = raw_;
        }
        consume_line(line);
        raw_.clear();

        std::size_t terminator = 1;
        if (bytes[end] == '\r') {
            if (end + 1 < bytes.size()) {
                if (bytes[end + 1] == '\n') terminator = 2;
            } else {
                // A CR ends this feed: an LF that starts the next one belongs
                // to the same line terminator.
                skip_lf_ = true;
            }
        }
        bytes.remove_prefix(end + terminator);
    }
}

void SseEventParser::consume_line(std::string_view line) {
    if (line.empty()) {
        finish_event();
        return;
    }
    if (line.front() == ':') {
        // comment — ignore
        return;
    }
    auto [name, value] = split_field(line);
    // The event in progress counts its data, its type and the last event ID
    // it will carry.
    if (name == "data") {
        const std::size_t separator_bytes = cur_has_data_ ? 1u : 0u;
        ensure_can_add(cur_event_bytes_ + last_id_.size(), separator_bytes,
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");
        const std::size_t bytes_with_separator =
            cur_event_bytes_ + separator_bytes;
        ensure_can_add(bytes_with_separator + last_id_.size(), value.size(),
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");

        if (cur_has_data_) cur_data_.push_back('\n');
        cur_data_.append(value);
        cur_event_bytes_ = bytes_with_separator + value.size();
        cur_has_data_ = true;
    } else if (name == "event") {
        const std::size_t retained_bytes =
            cur_event_bytes_ - cur_event_.size();
        ensure_can_add(retained_bytes + last_id_.size(), value.size(),
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");
        std::string replacement(value);
        cur_event_.swap(replacement);
        cur_event_bytes_ = retained_bytes + value.size();
    } else if (name == "id") {
        if (value.find('\0') == std::string_view::npos) {
            ensure_can_add(cur_event_bytes_, value.size(), options_.max_event_bytes,
                           "SSE event exceeds configured byte limit");
            last_id_.assign(value);
        }
    } else if (name == "retry") {
        if (!value.empty() &&
            value.find_first_not_of("0123456789") == std::string_view::npos) {
            std::uint64_t milliseconds = 0;
            const auto* last = value.data() + value.size();
            const auto [end, error] = std::from_chars(value.data(), last, milliseconds);
            if (error == std::errc{} && end == last) retry_ms_ = milliseconds;
        }
    }
    // Other fields are ignored.
}

void SseEventParser::finish_event() {
    if (!cur_has_data_) {
        // No data line: nothing is dispatched, and the event type is dropped.
        cur_event_.clear();
        cur_event_bytes_ = 0;
        return;
    }
    const std::size_t event_bytes = cur_event_bytes_ + last_id_.size();
    ensure_can_add(pending_.size(), 1u, options_.max_pending_events,
                   "SSE pending event count exceeds configured limit");
    ensure_can_add(pending_bytes_, event_bytes,
                   options_.max_pending_bytes,
                   "SSE pending event bytes exceed configured limit");

    // Grow pending_ only after both budgets have been checked. Constructing
    // the slot first also keeps the current event intact if allocation fails.
    pending_.emplace_back();
    auto& event = pending_.back();
    event.event = std::move(cur_event_);
    event.data  = std::move(cur_data_);
    event.id    = last_id_;
    pending_bytes_ += event_bytes;
    cur_event_.clear();
    cur_data_.clear();
    cur_event_bytes_ = 0;
    cur_has_data_ = false;
}

std::vector<SseEvent> SseEventParser::drain() {
    if (failed_) {
        throw std::length_error(
            "SSE parser is in a failed state; call reset() before reuse");
    }
    std::vector<SseEvent> out;
    out.swap(pending_);
    pending_bytes_ = 0;
    return out;
}

void SseEventParser::reset() noexcept {
    raw_.clear();
    cur_data_.clear();
    cur_event_.clear();
    last_id_.clear();
    retry_ms_.reset();
    cur_event_bytes_ = 0;
    cur_has_data_ = false;
    skip_lf_ = false;
    at_start_ = true;
    pending_.clear();
    pending_bytes_ = 0;
    failed_ = false;
}

void SseEventParser::ensure_can_add(std::size_t current,
                                    std::size_t addition,
                                    std::size_t limit,
                                    const char* message) {
    if (current > limit || addition > limit - current) {
        fail_limit(message);
    }
}

[[noreturn]] void SseEventParser::fail_limit(const char* message) {
    failed_ = true;
    std::string{}.swap(raw_);
    std::string{}.swap(cur_data_);
    std::string{}.swap(cur_event_);
    std::vector<SseEvent>{}.swap(pending_);
    cur_event_bytes_ = 0;
    pending_bytes_ = 0;
    cur_has_data_ = false;
    throw std::length_error(message);
}

}  // namespace neograph::async
