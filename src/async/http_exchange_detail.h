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
#include <asio/buffers_iterator.hpp>
#include <asio/multiple_exceptions.hpp>
#include <asio/read.hpp>
#include <asio/streambuf.hpp>
#include <asio/system_error.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <exception>
#include <functional>
#include <istream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async::detail {

enum class ConnDirective { keep_alive, close };

[[noreturn]] inline void throw_message_size(const char* context) {
    throw asio::system_error(asio::error::message_size, context);
}

inline bool exceeds_limit(std::size_t value, std::size_t limit) noexcept {
    return limit != 0 && value > limit;
}

// Read through `delimiter` without allowing a missing delimiter to grow the
// dynamic buffer past `limit`. The returned count includes the delimiter.
template <typename Stream>
asio::awaitable<std::size_t> read_until_limited(
    Stream& stream,
    asio::streambuf& buf,
    std::string_view delimiter,
    std::size_t limit,
    const char* context) {
    constexpr std::size_t read_step = 4096;
    for (;;) {
        const auto data = buf.data();
        const auto begin = asio::buffers_begin(data);
        const auto end = asio::buffers_end(data);
        const auto match = std::search(begin, end,
                                       delimiter.begin(), delimiter.end());
        if (match != end) {
            const auto prefix = static_cast<std::size_t>(
                std::distance(begin, match));
            if (prefix > std::numeric_limits<std::size_t>::max() - delimiter.size()) {
                throw_message_size(context);
            }
            const std::size_t through_delimiter = prefix + delimiter.size();
            if (exceeds_limit(through_delimiter, limit)) {
                throw_message_size(context);
            }
            co_return through_delimiter;
        }

        if (limit != 0 && buf.size() >= limit) {
            throw_message_size(context);
        }
        const std::size_t room = limit == 0
            ? read_step
            : std::min(read_step, limit - buf.size());
        auto target = buf.prepare(room);
        const std::size_t received = co_await stream.async_read_some(
            target, asio::use_awaitable);
        buf.commit(received);
    }
}

// `awaitable_operators::operator||` reports concurrent cancellation of the
// exchange and its timeout timer as multiple_exceptions. Preserve the original
// socket error so callers can recognize operation_aborted.
[[noreturn]] inline void rethrow_first_exception(
    const asio::multiple_exceptions& error) {
    if (auto first = error.first_exception()) {
        std::rethrow_exception(first);
    }
    throw std::runtime_error("Asio multiple_exceptions has no cause");
}

inline std::string build_request(
    std::string_view host,
    std::string_view path,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    ConnDirective directive,
    std::string_view method = "POST") {
    const bool is_get = method == "GET";
    std::string out;
    out.reserve(256 + body.size());
    out.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");
    out.append("Host: ").append(host).append("\r\n");
    if (!is_get) {
        out.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    }
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
    if (!is_get && !has_ctype) out.append("Content-Type: application/json\r\n");
    out.append("\r\n");
    if (!is_get) out.append(body);
    return out;
}

struct HttpTarget {
    std::string host;
    std::string port;
    std::string path;
    bool tls = false;
};

inline std::string ascii_lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline std::optional<std::string> normalized_host(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host.remove_prefix(1);
        host.remove_suffix(1);
    }
    if (host.empty()) return std::nullopt;
    std::string out = ascii_lower(host);
    if (out.back() == '.') {
        out.pop_back();
        if (out.empty() || out.back() == '.') return std::nullopt;
    }
    if (out.front() == '.' || out.find("..") != std::string::npos) {
        return std::nullopt;
    }
    for (unsigned char c : out) {
        if (c <= 0x20 || c >= 0x7f || c == '@' || c == '%' ||
            c == '/' || c == '\\' || c == '?' || c == '#') {
            return std::nullopt;
        }
    }
    return out;
}

inline std::optional<std::string> effective_port(std::string_view port,
                                                  bool tls,
                                                  bool uri_port = false) {
    if (port.empty()) return std::string(tls ? "443" : "80");
    const std::string lower = ascii_lower(port);
    if (!uri_port && lower == "https") return std::string("443");
    if (!uri_port && lower == "http") return std::string("80");

    unsigned int numeric = 0;
    const auto [end, ec] = std::from_chars(
        port.data(), port.data() + port.size(), numeric);
    if (ec == std::errc{} && end == port.data() + port.size()) {
        if (numeric == 0 || numeric > 65535) return std::nullopt;
        return std::to_string(numeric);
    }
    if (uri_port) return std::nullopt;
    for (unsigned char c : lower) {
        if (!std::isalnum(c) && c != '-' && c != '_') return std::nullopt;
    }
    return lower;
}

inline bool same_origin(const HttpTarget& left, const HttpTarget& right) {
    if (left.tls != right.tls) return false;
    const auto left_host = normalized_host(left.host);
    const auto right_host = normalized_host(right.host);
    const auto left_port = effective_port(left.port, left.tls);
    const auto right_port = effective_port(right.port, right.tls);
    return left_host && right_host && left_port && right_port &&
           *left_host == *right_host && *left_port == *right_port;
}

inline std::optional<HttpTarget> parse_absolute_http_url(std::string_view url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::nullopt;
    const std::string scheme = ascii_lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") return std::nullopt;

    HttpTarget out;
    out.tls = scheme == "https";
    std::string_view rest = url.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?#");
    const std::string_view authority = authority_end == std::string_view::npos
        ? rest
        : rest.substr(0, authority_end);
    std::string_view suffix = authority_end == std::string_view::npos
        ? std::string_view{}
        : rest.substr(authority_end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view host;
    std::string_view port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return std::nullopt;
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') return std::nullopt;
            port = authority.substr(close + 2);
            if (port.empty()) return std::nullopt;
        }
    } else {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':', colon + 1) != std::string_view::npos) {
                return std::nullopt;
            }
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty()) return std::nullopt;
        } else {
            host = authority;
        }
    }
    if (!normalized_host(host)) return std::nullopt;
    const auto normalized_port = effective_port(port, out.tls, true);
    if (!normalized_port) return std::nullopt;
    out.host = std::string(host);
    out.port = std::move(*normalized_port);

    if (const auto fragment = suffix.find('#'); fragment != std::string_view::npos) {
        suffix = suffix.substr(0, fragment);
    }
    if (suffix.empty()) out.path = "/";
    else if (suffix.front() == '?') out.path = "/" + std::string(suffix);
    else out.path = std::string(suffix);
    return out;
}

inline std::optional<HttpTarget> resolve_redirect_target(
    const HttpTarget& current, std::string_view location) {
    if (location.empty()) return std::nullopt;
    if (location.find('\r') != std::string_view::npos ||
        location.find('\n') != std::string_view::npos) {
        return std::nullopt;
    }

    const auto scheme = location.find("://");
    if (scheme != std::string_view::npos) {
        return parse_absolute_http_url(location);
    }
    if (location.rfind("//", 0) == 0) {
        return parse_absolute_http_url(
            std::string(current.tls ? "https:" : "http:") + std::string(location));
    }
    // Reject non-HTTP URI schemes rather than treating them as path text.
    const auto colon = location.find(':');
    const auto slash = location.find('/');
    if (colon != std::string_view::npos &&
        (slash == std::string_view::npos || colon < slash)) {
        return std::nullopt;
    }

    HttpTarget out = current;
    if (const auto fragment = location.find('#'); fragment != std::string_view::npos) {
        location = location.substr(0, fragment);
    }
    if (location.empty()) return out;
    if (location.front() == '/') {
        out.path = std::string(location);
    } else if (location.front() == '?') {
        const auto query = out.path.find('?');
        if (query != std::string::npos) out.path.resize(query);
        out.path.append(location);
    } else {
        const auto query = out.path.find('?');
        if (query != std::string::npos) out.path.resize(query);
        const auto last_slash = out.path.rfind('/');
        out.path.resize(last_slash == std::string::npos ? 0 : last_slash + 1);
        if (out.path.empty()) out.path = "/";
        out.path.append(location);
    }
    return out;
}

inline std::string format_http_url(const HttpTarget& target) {
    std::string out = target.tls ? "https://" : "http://";
    if (target.host.find(':') != std::string::npos && target.host.front() != '[') {
        out.push_back('[');
        out.append(target.host);
        out.push_back(']');
    } else {
        out.append(target.host);
    }
    const auto port = effective_port(target.port, target.tls);
    if (!port || *port != (target.tls ? "443" : "80")) {
        out.push_back(':');
        out.append(target.port);
    }
    out.append(target.path.empty() ? "/" : target.path);
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
    if (ec != std::errc{} || p != token.data() + token.size() ||
        token.size() != 3 || status < 100 || status > 999) {
        return 0;
    }
    return status;
}

// Extra per-response bits extracted while scanning headers.
// Populated by extract_headers; empty when the corresponding header
// was absent. The `response` pointer is optional — callers that
// only want content-length pass nullptr and save the allocations.
struct ResponseHeaderBits {
    std::string retry_after;
    std::string location;
    /// Every header seen on the response, in wire order. Preserves
    /// the original-cased name exactly as the server sent it so
    /// callers that want to round-trip (e.g. proxies) don't need to
    /// reconstruct capitalization. Case-insensitive lookup against
    /// this vector is provided by HttpResponse::get_header.
    std::vector<std::pair<std::string, std::string>> all;
};

struct ParsedResponseHeaders {
    std::optional<std::size_t> content_length;
    bool chunked = false;
    bool transfer_encoding_present = false;
    bool ambiguous_framing = false;
    bool unsupported_transfer_coding = false;
};

inline std::size_t parse_decimal_size(std::string_view value,
                                      const char* malformed_context,
                                      const char* overflow_context) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    if (value.empty()) throw std::runtime_error(malformed_context);

    std::size_t result = 0;
    const auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), result, 10);
    if (ec == std::errc::result_out_of_range) {
        throw_message_size(overflow_context);
    }
    if (ec != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error(malformed_context);
    }
    return result;
}

// Parse the headers block up to (and consuming) the blank line. Updates
// `directive` to `close` if the response says so; caller seeds it with the
// HTTP/1.1 default (keep_alive).
// When `extra` is non-null, also captures Retry-After and Location
// verbatim — used by redirect / 429-retry callers.
//
// Invariant: on return, `in` has consumed the entire header block —
// including the blank-line terminator — regardless of which headers
// were seen. This matters for chunked responses: if we early-returned
// on seeing Transfer-Encoding, any headers that followed would stay
// buffered and the chunk-size parser would then misread them as a
// chunk size. Drain first, decide after.
inline ParsedResponseHeaders extract_headers(
    std::istream& in,
    ConnDirective& directive,
    ResponseHeaderBits* extra = nullptr) {
    std::string line;
    ParsedResponseHeaders parsed;
    std::vector<std::string> transfer_codings;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // end of headers
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        // Preserve original-cased name; lowercase copy for comparisons.
        std::string raw_name  = line.substr(0, colon);
        std::string value     = line.substr(colon + 1);
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) value.clear();
        else value = value.substr(first);

        std::string name = raw_name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (extra) {
            extra->all.emplace_back(raw_name, value);
        }
        if (name == "content-length") {
            const std::size_t content_length = parse_decimal_size(
                value,
                "async HTTP: malformed Content-Length",
                "async HTTP: Content-Length overflow");
            if (parsed.content_length && *parsed.content_length != content_length) {
                throw std::runtime_error(
                    "async HTTP: conflicting Content-Length headers");
            }
            parsed.content_length = content_length;
        } else if (name == "transfer-encoding") {
            std::string lv;
            std::transform(value.begin(), value.end(), std::back_inserter(lv),
                           [](unsigned char c) { return std::tolower(c); });
            std::size_t start = 0;
            while (start <= lv.size()) {
                const auto comma = lv.find(',', start);
                std::string_view token(lv.data() + start,
                    (comma == std::string::npos ? lv.size() : comma) - start);
                while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
                    token.remove_prefix(1);
                while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
                    token.remove_suffix(1);
                if (token.empty()) {
                    throw std::runtime_error(
                        "async HTTP: malformed Transfer-Encoding");
                }
                transfer_codings.emplace_back(token);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (name == "connection") {
            std::string lv;
            std::transform(value.begin(), value.end(), std::back_inserter(lv),
                           [](unsigned char c) { return std::tolower(c); });
            if (lv.find("close") != std::string::npos)
                directive = ConnDirective::close;
        } else if (extra && name == "retry-after") {
            extra->retry_after = value;
        } else if (extra && name == "location") {
            extra->location = value;
        }
    }
    if (!transfer_codings.empty()) {
        parsed.transfer_encoding_present = true;
        if (parsed.content_length) {
            parsed.ambiguous_framing = true;
        }
        if (transfer_codings.size() != 1 ||
            transfer_codings.back() != "chunked") {
            parsed.unsupported_transfer_coding = true;
        } else {
            parsed.chunked = true;
        }
    }
    return parsed;
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
    std::string   location;
};

inline std::size_t parse_hex_chunk_size(std::string_view value,
                                        const char* malformed_context) {
    if (const auto semicolon = value.find(';'); semicolon != std::string_view::npos) {
        value = value.substr(0, semicolon);
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    if (value.empty()) throw std::runtime_error(malformed_context);

    std::size_t chunk_size = 0;
    const auto [end, ec] = std::from_chars(
        value.data(), value.data() + value.size(), chunk_size, 16);
    if (ec == std::errc::result_out_of_range) {
        throw_message_size("async HTTP: chunk size overflow");
    }
    if (ec != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error(malformed_context);
    }
    return chunk_size;
}

template <typename Stream>
asio::awaitable<std::size_t> read_chunk_size(
    Stream& stream,
    asio::streambuf& buf,
    const RequestOptions& opts,
    const char* malformed_context) {
    const std::size_t line_bytes = co_await read_until_limited(
        stream, buf, "\r\n", opts.max_response_header_bytes,
        "async HTTP: chunk-size line limit");

    const auto data = buf.data();
    const auto begin = asio::buffers_begin(data);
    std::string size_line(
        begin, begin + static_cast<std::ptrdiff_t>(line_bytes - 2));
    const std::size_t chunk_size = parse_hex_chunk_size(
        size_line, malformed_context);
    buf.consume(line_bytes);
    co_return chunk_size;
}

template <typename Stream>
asio::awaitable<void> read_chunk_trailers(
    Stream& stream,
    asio::streambuf& buf,
    const RequestOptions& opts) {
    std::size_t trailer_bytes = 0;
    for (;;) {
        const std::size_t remaining = opts.max_response_header_bytes == 0
            ? 0
            : opts.max_response_header_bytes -
                std::min(trailer_bytes, opts.max_response_header_bytes);
        if (opts.max_response_header_bytes != 0 && remaining == 0) {
            throw_message_size("async HTTP: trailer limit");
        }
        const std::size_t line_bytes = co_await read_until_limited(
            stream, buf, "\r\n", remaining,
            "async HTTP: trailer limit");
        if (line_bytes > std::numeric_limits<std::size_t>::max() - trailer_bytes) {
            throw_message_size("async HTTP: trailer limit");
        }
        trailer_bytes += line_bytes;
        buf.consume(line_bytes);
        if (line_bytes == 2) co_return;
    }
}

template <typename Stream>
asio::awaitable<void> buffer_exactly(Stream& stream,
                                     asio::streambuf& buf,
                                     std::size_t bytes) {
    if (buf.size() >= bytes) co_return;
    co_await asio::async_read(stream, buf,
        asio::transfer_exactly(bytes - buf.size()), asio::use_awaitable);
}

inline void validate_chunk_payload(const asio::streambuf& buf,
                                   std::size_t chunk_size,
                                   const char* context) {
    const auto data = buf.data();
    auto cr = asio::buffers_begin(data) + static_cast<std::ptrdiff_t>(chunk_size);
    auto lf = cr;
    ++lf;
    if (*cr != '\r' || lf == asio::buffers_end(data) || *lf != '\n') {
        throw std::runtime_error(context);
    }
}

inline void check_chunk_limits(std::size_t chunk_size,
                               std::size_t body_bytes,
                               const RequestOptions& opts,
                               const char* chunk_context,
                               const char* body_context) {
    if (exceeds_limit(chunk_size, opts.max_response_chunk_bytes)) {
        throw_message_size(chunk_context);
    }
    if (chunk_size > std::numeric_limits<std::size_t>::max() - body_bytes ||
        (opts.max_response_body_bytes != 0 &&
         chunk_size > opts.max_response_body_bytes -
             std::min(body_bytes, opts.max_response_body_bytes))) {
        throw_message_size(body_context);
    }
    if (chunk_size > std::numeric_limits<std::size_t>::max() - 2 ||
        chunk_size > static_cast<std::size_t>(
            std::numeric_limits<std::ptrdiff_t>::max()) ||
        chunk_size > std::string{}.max_size()) {
        throw_message_size("async HTTP: chunk size overflow");
    }
}

template <typename Stream>
asio::awaitable<StreamExchangeResult> run_exchange_stream(
    Stream& stream,
    const std::string& req,
    const std::function<void(std::string_view)>& on_chunk,
    const RequestOptions& opts) {
    co_await asio::async_write(stream, asio::buffer(req), asio::use_awaitable);

    asio::streambuf buf;
    StreamExchangeResult r;
    ParsedResponseHeaders parsed;
    std::size_t header_bytes = 0;
    int interim_responses = 0;
    for (;;) {
        const std::size_t remaining = opts.max_response_header_bytes == 0
            ? 0
            : opts.max_response_header_bytes -
                std::min(header_bytes, opts.max_response_header_bytes);
        if (opts.max_response_header_bytes != 0 && remaining == 0) {
            throw_message_size("async_post_stream: response header limit");
        }
        const std::size_t block_bytes = co_await read_until_limited(
            stream, buf, "\r\n\r\n", remaining,
            "async_post_stream: response header limit");
        header_bytes += block_bytes;

        std::istream is(&buf);
        std::string status_line;
        std::getline(is, status_line);
        if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
        r.status = parse_status_line(status_line);
        if (r.status == 0) {
            throw std::runtime_error("async_post_stream: malformed HTTP status line");
        }

        ResponseHeaderBits extra;
        parsed = extract_headers(is, r.server_directive, &extra);
        if (r.status >= 100 && r.status < 200 && r.status != 101) {
            if (++interim_responses > 16) {
                throw std::runtime_error(
                    "async_post_stream: too many interim HTTP responses");
            }
            continue;
        }
        r.location = std::move(extra.location);
        break;
    }
    const bool response_can_have_body =
        !(r.status >= 100 && r.status < 200) &&
        r.status != 204 && r.status != 304;
    if (!response_can_have_body) {
        if (buf.size() != 0) {
            throw std::runtime_error(
                "async_post_stream: surplus bytes after bodyless response");
        }
        co_return r;
    }
    if (parsed.ambiguous_framing) {
        throw std::runtime_error(
            "async HTTP: response has both Transfer-Encoding and Content-Length");
    }
    if (parsed.unsupported_transfer_coding) {
        throw std::runtime_error(
            "async HTTP: unsupported or non-final transfer coding");
    }
    if (!parsed.chunked) {
        const bool redirect = r.status == 301 || r.status == 302 ||
                              r.status == 303 || r.status == 307 ||
                              r.status == 308;
        if (redirect) {
            const std::size_t body_size = parsed.content_length.value_or(0);
            if (exceeds_limit(body_size, opts.max_response_body_bytes)) {
                throw_message_size("async_post_stream: response body limit");
            }
            if (buf.size() > body_size) {
                throw std::runtime_error(
                    "async_post_stream: surplus bytes after fixed response body");
            }
            co_await buffer_exactly(stream, buf, body_size);
            buf.consume(body_size);
            co_return r;
        }
        throw std::runtime_error(
            "async_post_stream: response must be Transfer-Encoding: chunked");
    }

    std::size_t body_bytes = 0;
    for (;;) {
        const std::size_t chunk_size = co_await read_chunk_size(
            stream, buf, opts,
            "async_post_stream: malformed chunk size");
        if (chunk_size == 0) {
            co_await read_chunk_trailers(stream, buf, opts);
            if (buf.size() != 0) {
                throw std::runtime_error(
                    "async_post_stream: surplus bytes after chunked response");
            }
            co_return r;
        }

        check_chunk_limits(chunk_size, body_bytes, opts,
                           "async_post_stream: response chunk limit",
                           "async_post_stream: response body limit");
        const std::size_t needed = chunk_size + 2;
        co_await buffer_exactly(stream, buf, needed);
        validate_chunk_payload(buf, chunk_size,
                               "async_post_stream: malformed chunk terminator");

        std::string payload(chunk_size, '\0');
        std::copy_n(asio::buffers_begin(buf.data()), chunk_size, payload.begin());
        buf.consume(needed);
        body_bytes += chunk_size;
        on_chunk(payload);
    }
}

template <typename Stream>
asio::awaitable<void> read_chunked_body(Stream& stream,
                                        asio::streambuf& buf,
                                        std::string& out,
                                        const RequestOptions& opts) {
    for (;;) {
        const std::size_t chunk_size = co_await read_chunk_size(
            stream, buf, opts,
            "async_post: malformed chunk size");
        if (chunk_size == 0) {
            co_await read_chunk_trailers(stream, buf, opts);
            co_return;
        }

        check_chunk_limits(chunk_size, out.size(), opts,
                           "async_post: response chunk limit",
                           "async_post: response body limit");
        const std::size_t needed = chunk_size + 2;
        co_await buffer_exactly(stream, buf, needed);
        validate_chunk_payload(buf, chunk_size,
                               "async_post: malformed chunk terminator");

        const auto begin = asio::buffers_begin(buf.data());
        out.append(begin, begin + static_cast<std::ptrdiff_t>(chunk_size));
        buf.consume(needed);
    }
}

template <typename Stream>
asio::awaitable<ExchangeResult> run_exchange(Stream& stream,
                                             const std::string& req,
                                             const RequestOptions& opts) {
    co_await asio::async_write(stream, asio::buffer(req), asio::use_awaitable);

    asio::streambuf buf;
    ExchangeResult r;
    r.server_directive = ConnDirective::keep_alive;  // HTTP/1.1 default
    ParsedResponseHeaders parsed;
    std::size_t header_bytes = 0;
    int interim_responses = 0;
    for (;;) {
        const std::size_t remaining = opts.max_response_header_bytes == 0
            ? 0
            : opts.max_response_header_bytes -
                std::min(header_bytes, opts.max_response_header_bytes);
        if (opts.max_response_header_bytes != 0 && remaining == 0) {
            throw_message_size("async_post: response header limit");
        }
        const std::size_t block_bytes = co_await read_until_limited(
            stream, buf, "\r\n\r\n", remaining,
            "async_post: response header limit");
        header_bytes += block_bytes;

        std::istream is(&buf);
        std::string status_line;
        std::getline(is, status_line);
        if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
        r.response.status = parse_status_line(status_line);
        if (r.response.status == 0) {
            throw std::runtime_error("async_post: malformed HTTP status line");
        }

        ResponseHeaderBits extra;
        parsed = extract_headers(is, r.server_directive, &extra);
        if (r.response.status >= 100 && r.response.status < 200 &&
            r.response.status != 101) {
            if (++interim_responses > 16) {
                throw std::runtime_error(
                    "async_post: too many interim HTTP responses");
            }
            continue;
        }
        r.response.retry_after = std::move(extra.retry_after);
        r.response.location    = std::move(extra.location);
        r.response.headers     = std::move(extra.all);
        break;
    }

    const bool response_can_have_body =
        !(r.response.status >= 100 && r.response.status < 200) &&
        r.response.status != 204 && r.response.status != 304;
    if (!response_can_have_body) {
        if (buf.size() != 0) {
            throw std::runtime_error(
                "async_post: surplus bytes after bodyless response");
        }
        if (r.response.status == 101) {
            r.server_directive = ConnDirective::close;
        }
        co_return r;
    }
    if (parsed.ambiguous_framing) {
        throw std::runtime_error(
            "async HTTP: response has both Transfer-Encoding and Content-Length");
    }
    if (parsed.unsupported_transfer_coding) {
        throw std::runtime_error(
            "async HTTP: unsupported or non-final transfer coding");
    }

    if (parsed.chunked) {
        // Chunked transfer-encoding. OpenAI HTTP/1.1, fastmcp
        // Streamable HTTP, and most modern servers behind a reverse
        // proxy all fall into this path — Content-Length only shows up
        // when the body was fully buffered before headers were flushed.
        // After draining chunks, the body is the concatenation of
        // payloads as if Content-Length had been set.
        //
        // The `is` istream above drained the headers up to and
        // including the "\r\n\r\n" terminator, leaving the first chunk
        // size (or more) sitting in `buf`.
        co_await read_chunked_body(
            stream, buf, r.response.body, opts);
        if (buf.size() != 0) {
            throw std::runtime_error(
                "async_post: surplus bytes after chunked response");
        }
        co_return r;
    }

    if (!parsed.content_length) {
        // Without self-delimiting framing, unread bytes cannot safely remain
        // on a pooled connection. One-shot callers close it anyway.
        r.server_directive = ConnDirective::close;
    }

    const std::size_t content_length = parsed.content_length.value_or(0);
    if (exceeds_limit(content_length, opts.max_response_body_bytes) ||
        content_length > r.response.body.max_size()) {
        throw_message_size("async_post: response body limit");
    }
    r.response.body.resize(content_length);
    std::size_t filled = 0;
    if (buf.size() > 0) {
        if (buf.size() > content_length) {
            throw std::runtime_error(
                "async_post: surplus bytes after fixed response body");
        }
        const std::size_t take = std::min(buf.size(), content_length);
        std::copy_n(asio::buffers_begin(buf.data()), take,
                    r.response.body.begin());
        buf.consume(take);
        filled = take;
    }
    if (filled < content_length) {
        const std::size_t remaining = content_length - filled;
        co_await asio::async_read(stream,
            asio::buffer(r.response.body.data() + filled, remaining),
            asio::transfer_exactly(remaining),
            asio::use_awaitable);
    }
    co_return r;
}

}  // namespace neograph::async::detail
