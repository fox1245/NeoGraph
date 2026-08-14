// Incremental SSE parser — see header for scope + field coverage.
//
// State machine:
//
//   bytes → raw_ buffer.
//   For each fully-terminated line in raw_ (LF or CRLF):
//     - empty line → end-of-event boundary.
//     - ":prefix"  → comment, ignored.
//     - "name:val" → dispatch name to accumulator.
//     - (name alone, no colon) → treat as field name with empty value.
//
//   When a boundary fires and any of {data, event, id} accumulated
//   since the last boundary were non-default, push an SseEvent into
//   pending_ and clear the accumulators.
//
// The "accumulated something" test includes an empty-string data
// case (`data:\n\n`) since SSE spec §9.2.5 says the event fires
// whenever a field line appeared between boundaries. We track that
// with `cur_in_progress_`.

#include <neograph/async/sse_parser.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace neograph::async {

namespace {

// Split "name: value" → (name, value). Value has its single leading
// space stripped (SSE §9.2.5: "If that character is a space, it is
// ignored"). Lines with no colon report the whole string as name and
// empty value.
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

    // Scan the input before copying it so a chunk containing many bounded
    // lines does not require an equally large temporary raw_ allocation.
    while (!bytes.empty()) {
        const auto nl = bytes.find('\n');
        const auto fragment = bytes.substr(0, nl);
        ensure_can_add(raw_.size(), fragment.size(),
                       options_.max_partial_line_bytes,
                       "SSE partial line exceeds configured byte limit");

        if (nl == std::string_view::npos) {
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
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        consume_line(line);
        raw_.clear();
        bytes.remove_prefix(nl + 1);
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
    if (name == "data") {
        const std::size_t separator_bytes = cur_has_data_ ? 1u : 0u;
        ensure_can_add(cur_event_bytes_, separator_bytes,
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");
        const std::size_t bytes_with_separator =
            cur_event_bytes_ + separator_bytes;
        ensure_can_add(bytes_with_separator, value.size(),
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");

        if (cur_has_data_) cur_data_.push_back('\n');
        cur_data_.append(value);
        cur_event_bytes_ = bytes_with_separator + value.size();
        cur_in_progress_ = true;
        cur_has_data_ = true;
    } else if (name == "event") {
        const std::size_t retained_bytes =
            cur_event_bytes_ - cur_event_.size();
        ensure_can_add(retained_bytes, value.size(),
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");
        std::string replacement(value);
        cur_event_.swap(replacement);
        cur_event_bytes_ = retained_bytes + value.size();
        cur_in_progress_ = true;
    } else if (name == "id") {
        const std::size_t retained_bytes = cur_event_bytes_ - cur_id_.size();
        ensure_can_add(retained_bytes, value.size(),
                       options_.max_event_bytes,
                       "SSE event exceeds configured byte limit");
        std::string replacement(value);
        cur_id_.swap(replacement);
        cur_event_bytes_ = retained_bytes + value.size();
        cur_in_progress_ = true;
    }
    // Other fields (retry etc.) intentionally ignored.
}

void SseEventParser::finish_event() {
    if (!cur_in_progress_) return;
    ensure_can_add(pending_.size(), 1u, options_.max_pending_events,
                   "SSE pending event count exceeds configured limit");
    ensure_can_add(pending_bytes_, cur_event_bytes_,
                   options_.max_pending_bytes,
                   "SSE pending event bytes exceed configured limit");

    // Grow pending_ only after both budgets have been checked. Constructing
    // the slot first also keeps the current event intact if allocation fails.
    pending_.emplace_back();
    auto& event = pending_.back();
    event.event = std::move(cur_event_);
    event.data  = std::move(cur_data_);
    event.id    = std::move(cur_id_);
    pending_bytes_ += cur_event_bytes_;
    cur_event_.clear();
    cur_data_.clear();
    cur_id_.clear();
    cur_event_bytes_ = 0;
    cur_in_progress_ = false;
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
    cur_id_.clear();
    cur_event_bytes_ = 0;
    cur_in_progress_ = false;
    cur_has_data_ = false;
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
    std::string{}.swap(cur_id_);
    std::vector<SseEvent>{}.swap(pending_);
    cur_event_bytes_ = 0;
    pending_bytes_ = 0;
    cur_in_progress_ = false;
    cur_has_data_ = false;
    throw std::length_error(message);
}

}  // namespace neograph::async
