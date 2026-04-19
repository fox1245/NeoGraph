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

void SseEventParser::feed(std::string_view bytes) {
    raw_.append(bytes.data(), bytes.size());

    // Scan for complete lines. We keep the unterminated tail in raw_.
    std::size_t start = 0;
    while (start < raw_.size()) {
        auto nl = raw_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string_view line(raw_.data() + start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        consume_line(line);
        start = nl + 1;
    }
    if (start > 0) raw_.erase(0, start);
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
        if (!cur_data_.empty()) cur_data_.push_back('\n');
        cur_data_.append(value);
        cur_in_progress_ = true;
    } else if (name == "event") {
        cur_event_.assign(value);
        cur_in_progress_ = true;
    } else if (name == "id") {
        cur_id_.assign(value);
        cur_in_progress_ = true;
    }
    // Other fields (retry etc.) intentionally ignored.
}

void SseEventParser::finish_event() {
    if (!cur_in_progress_) return;
    SseEvent e;
    e.event = std::move(cur_event_);
    e.data  = std::move(cur_data_);
    e.id    = std::move(cur_id_);
    pending_.push_back(std::move(e));
    cur_event_.clear();
    cur_data_.clear();
    cur_id_.clear();
    cur_in_progress_ = false;
}

std::vector<SseEvent> SseEventParser::drain() {
    std::vector<SseEvent> out;
    out.swap(pending_);
    return out;
}

void SseEventParser::reset() noexcept {
    raw_.clear();
    cur_data_.clear();
    cur_event_.clear();
    cur_id_.clear();
    cur_in_progress_ = false;
    pending_.clear();
}

}  // namespace neograph::async
