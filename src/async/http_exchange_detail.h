// Internal-only HTTP/1.1 exchange primitives shared between the
// one-shot path (async_post in http_client.cpp) and the pooled path
// (ConnPool::async_post in conn_pool.cpp). Not a public header — lives
// under src/ so the translation units can include by relative path.
//
// The exchange is template-over-Stream: works over a plain tcp::socket,
// an asio::ssl::stream owning a socket, or an asio::ssl::stream
// referencing an external socket. The caller is responsible for
// resolving, connecting, and (for TLS) handshake; this header is just
// the wire-level request/response cycle.

#pragma once

#include <neograph/async/http_client.h>

#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <functional>
#include <istream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async::detail {

enum class ConnDirective { keep_alive, close };

inline std::string build_request(
    std::string_view host,
    std::string_view path,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    ConnDirective directive) {
    std::string out;
    out.reserve(256 + body.size());
    out.append("POST ").append(path).append(" HTTP/1.1\r\n");
    out.append("Host: ").append(host).append("\r\n");
    out.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    out.append(directive == ConnDirective::keep_alive
                   ? "Connection: keep-alive\r\n"
                   : "Connection: close\r\n");
    bool has_ctype = false;
    for (const auto& [k, v] : headers) {
        std::string lower;
        lower.reserve(k.size());
        std::transform(k.begin(), k.end(), std::back_inserter(lower),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "content-type") has_ctype = true;
        if (lower == "content-length" || lower == "host" ||
            lower == "connection") continue;  // we set these
        out.append(k).append(": ").append(v).append("\r\n");
    }
    if (!has_ctype) out.append("Content-Type: application/json\r\n");
    out.append("\r\n");
    out.append(body);
    return out;
}

// Parse "HTTP/1.1 200 OK\r\n" → 200. Returns 0 on malformed.
inline int parse_status_line(std::string_view line) {
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return 0;
    auto start = sp1 + 1;
    auto sp2 = line.find(' ', start);
    auto token = (sp2 == std::string_view::npos)
                     ? line.substr(start)
                     : line.substr(start, sp2 - start);
    int status = 0;
    auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), status);
    (void)p;
    if (ec != std::errc{}) return 0;
    return status;
}

// Parse the headers block up to (and consuming) the blank line.
// Returns Content-Length (0 if absent), or -1 on unsupported chunked
// encoding. Updates `directive` to `close` if the response says so;
// caller seeds it with the HTTP/1.1 default (keep_alive).
inline long extract_headers(std::istream& in, ConnDirective& directive) {
    std::string line;
    long content_length = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // end of headers
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) value.clear();
        else value = value.substr(first);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == "content-length") {
            std::from_chars(value.data(), value.data() + value.size(), content_length);
        } else if (name == "transfer-encoding") {
            std::string lv;
            std::transform(value.begin(), value.end(), std::back_inserter(lv),
                           [](unsigned char c) { return std::tolower(c); });
            if (lv.find("chunked") != std::string::npos) return -1;
        } else if (name == "connection") {
            std::string lv;
            std::transform(value.begin(), value.end(), std::back_inserter(lv),
                           [](unsigned char c) { return std::tolower(c); });
            if (lv.find("close") != std::string::npos)
                directive = ConnDirective::close;
        }
    }
    return content_length;
}

struct ExchangeResult {
    HttpResponse  response;
    ConnDirective server_directive = ConnDirective::keep_alive;
};

// Drive one HTTP/1.1 request/response cycle on `stream`. Caller has
// already built `req` via build_request(). Throws asio::system_error /
// std::runtime_error on wire or parse failure.
// Result of a chunked-streaming exchange. No body field — the body
// was delivered to the caller's on_chunk as it arrived.
struct StreamExchangeResult {
    int           status = 0;
    ConnDirective server_directive = ConnDirective::keep_alive;
};

// Internal: scan headers from `is`, returning true if
// Transfer-Encoding: chunked was present. Also updates `directive`
// from a Connection header. Content-Length is read and discarded —
// a chunked response is the only body framing we accept here, since
// the streaming caller has no fixed end-of-body to synchronize on.
inline bool extract_headers_for_stream(std::istream& is, ConnDirective& directive) {
    std::string line;
    bool chunked = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) value.clear();
        else value = value.substr(first);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::string lv;
        std::transform(value.begin(), value.end(), std::back_inserter(lv),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == "transfer-encoding") {
            if (lv.find("chunked") != std::string::npos) chunked = true;
        } else if (name == "connection") {
            if (lv.find("close") != std::string::npos)
                directive = ConnDirective::close;
        }
    }
    return chunked;
}

// Drive a chunked-body HTTP/1.1 exchange. The response body is
// delivered to `on_chunk` as each chunk arrives; when the terminator
// chunk (size 0) is seen, returns with status + directive.
//
// Throws std::runtime_error if the response isn't chunked, or if
// chunk parsing fails. HTTP-level errors (4xx, 5xx) still come
// back as StreamExchangeResult with the status set — the caller
// decides whether to care based on status alone, since 5xx bodies
// from LLM endpoints are usually small error JSON worth handing to
// the callback too.
//
// Trailers after the size-zero line are rejected (not supported).
// Real SSE emitters never use them.
template <typename Stream>
asio::awaitable<StreamExchangeResult> run_exchange_stream(
    Stream& stream,
    const std::string& req,
    const std::function<void(std::string_view)>& on_chunk) {

    co_await asio::async_write(stream, asio::buffer(req), asio::use_awaitable);

    asio::streambuf buf;
    co_await asio::async_read_until(stream, buf, "\r\n\r\n", asio::use_awaitable);

    StreamExchangeResult r;
    r.server_directive = ConnDirective::keep_alive;

    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    r.status = parse_status_line(status_line);

    bool chunked = extract_headers_for_stream(is, r.server_directive);
    if (!chunked) {
        throw std::runtime_error(
            "async_post_stream: response must be Transfer-Encoding: chunked");
    }

    // Chunk loop. `buf` already holds whatever bytes came in past
    // the header terminator; the istream above advanced past the
    // headers, so buf.data() now starts at the first chunk size.
    for (;;) {
        // Ensure we have at least one full "<hex>\r\n" size line.
        co_await asio::async_read_until(stream, buf, "\r\n", asio::use_awaitable);

        // Pull the size line out of buf. read_until leaves the
        // delimiter in buf; we extract the prefix ending at \r\n.
        auto data_cb  = buf.data();
        auto it_start = asio::buffers_begin(data_cb);
        auto it_end   = asio::buffers_end(data_cb);

        // Find \r\n in the readable area.
        auto it_cr = it_start;
        while (it_cr != it_end) {
            if (*it_cr == '\r') {
                auto it_next = it_cr;
                ++it_next;
                if (it_next != it_end && *it_next == '\n') break;
            }
            ++it_cr;
        }
        if (it_cr == it_end) {
            // read_until guarantees the delimiter is present, so this
            // path is unreachable — asserts the invariant.
            throw std::runtime_error("async_post_stream: size line missing \\r\\n");
        }

        const std::size_t size_line_len =
            static_cast<std::size_t>(std::distance(it_start, it_cr));
        std::string size_str(it_start, it_cr);
        // Drop ";ext" chunk extensions if present.
        if (auto semi = size_str.find(';'); semi != std::string::npos) {
            size_str = size_str.substr(0, semi);
        }
        // Strip surrounding whitespace — defensive; real servers don't
        // pad, but we've seen odd implementations.
        while (!size_str.empty() &&
               (size_str.back() == ' ' || size_str.back() == '\t'))
            size_str.pop_back();

        std::size_t chunk_size = 0;
        auto [p, pec] = std::from_chars(
            size_str.data(), size_str.data() + size_str.size(),
            chunk_size, 16);
        (void)p;
        if (pec != std::errc{}) {
            throw std::runtime_error("async_post_stream: malformed chunk size");
        }

        buf.consume(size_line_len + 2);  // drop "<hex>[;ext]\r\n"

        if (chunk_size == 0) {
            // Terminator. Expect \r\n (no trailers supported).
            if (buf.size() < 2) {
                co_await asio::async_read(stream, buf,
                    asio::transfer_exactly(2 - buf.size()),
                    asio::use_awaitable);
            }
            auto term = buf.data();
            auto ti = asio::buffers_begin(term);
            auto ti_next = ti;
            ++ti_next;
            if (*ti != '\r' || ti_next == asio::buffers_end(term) || *ti_next != '\n') {
                throw std::runtime_error(
                    "async_post_stream: chunked trailers not supported");
            }
            buf.consume(2);
            break;
        }

        // Ensure chunk_size + 2 bytes (payload + trailing \r\n) are buffered.
        const std::size_t needed = chunk_size + 2;
        if (buf.size() < needed) {
            co_await asio::async_read(stream, buf,
                asio::transfer_exactly(needed - buf.size()),
                asio::use_awaitable);
        }

        std::string payload(chunk_size, '\0');
        auto data_pb = buf.data();
        std::copy_n(asio::buffers_begin(data_pb), chunk_size, payload.begin());
        buf.consume(needed);

        on_chunk(std::string_view(payload));
    }

    co_return r;
}

template <typename Stream>
asio::awaitable<ExchangeResult> run_exchange(Stream& stream, const std::string& req) {
    co_await asio::async_write(stream, asio::buffer(req), asio::use_awaitable);

    asio::streambuf buf;
    co_await asio::async_read_until(stream, buf, "\r\n\r\n", asio::use_awaitable);

    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();

    ExchangeResult r;
    r.response.status = parse_status_line(status_line);
    r.server_directive = ConnDirective::keep_alive;  // HTTP/1.1 default

    long content_length = extract_headers(is, r.server_directive);
    if (content_length < 0) {
        throw std::runtime_error("async_post: chunked transfer-encoding not supported");
    }

    r.response.body.resize(content_length);
    long filled = 0;
    auto already = buf.size();
    if (already > 0) {
        long take = std::min<long>(static_cast<long>(already), content_length);
        is.read(r.response.body.data(), take);
        filled = is.gcount();
    }
    if (filled < content_length) {
        long remaining = content_length - filled;
        co_await asio::async_read(stream,
            asio::buffer(r.response.body.data() + filled, remaining),
            asio::transfer_exactly(remaining),
            asio::use_awaitable);
    }
    co_return r;
}

}  // namespace neograph::async::detail
