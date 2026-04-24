// RFC 6455 WebSocket client on asio coroutines. See header for scope.
//
// Structure:
//   1. detail:: — pure frame codec (parse_frame_header, encode_frame_header,
//      apply_mask) + key derivation (SHA-1 + base64 via OpenSSL EVP).
//      No I/O; fully unit-testable.
//   2. Impl — owns the socket (plain or TLS) + read buffer. A small
//      read_some/write_all shim dispatches on whether TLS is active.
//   3. ws_connect — resolve → connect → (handshake TLS) → send Upgrade
//      request → read 101 → verify Accept.
//   4. WsClient::send_*/recv — frame the payload, drive I/O, reassemble
//      fragmented messages, auto-handle ping/pong/close.

#include <neograph/async/ws_client.h>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

namespace detail {

namespace {

constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string base64_encode(const std::uint8_t* data, std::size_t len) {
    // EVP_EncodeBlock writes ceil(len/3)*4 bytes + NUL. No newlines.
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    int written = ::EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        data, static_cast<int>(len));
    if (written < 0) {
        throw std::runtime_error("base64_encode: EVP_EncodeBlock failed");
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

}  // namespace

void apply_mask(char* data, std::size_t len,
                const std::uint8_t mask_key[4]) noexcept {
    // Byte-wise XOR is plenty fast for the payload sizes we see
    // (Responses frames are typically < 16 KB). A 32-bit SWAR pass
    // would save maybe microseconds on multi-MB frames — not worth
    // the endian branch here.
    for (std::size_t i = 0; i < len; ++i) {
        data[i] = static_cast<char>(
            static_cast<std::uint8_t>(data[i]) ^ mask_key[i & 3]);
    }
}

std::optional<WsFrameHeader> parse_frame_header(std::string_view buf) {
    if (buf.size() < 2) return std::nullopt;
    auto b0 = static_cast<std::uint8_t>(buf[0]);
    auto b1 = static_cast<std::uint8_t>(buf[1]);

    WsFrameHeader h{};
    h.fin    = (b0 & 0x80) != 0;
    // RSV1/2/3 must be zero — we don't negotiate any extension.
    if ((b0 & 0x70) != 0) {
        throw std::runtime_error("ws: reserved bits set (RSV1/2/3)");
    }
    h.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    h.masked = (b1 & 0x80) != 0;

    const std::uint8_t len7 = b1 & 0x7F;
    std::size_t cursor = 2;

    if (len7 < 126) {
        h.payload_len = len7;
    } else if (len7 == 126) {
        if (buf.size() < cursor + 2) return std::nullopt;
        h.payload_len =
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[cursor])) << 8) |
             static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[cursor + 1]));
        cursor += 2;
    } else {
        // len7 == 127 → 8-byte length. MSB must be zero per §5.2.
        if (buf.size() < cursor + 8) return std::nullopt;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) |
                static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[cursor + i]));
        }
        if ((v & (std::uint64_t{1} << 63)) != 0) {
            throw std::runtime_error("ws: 64-bit length has MSB set");
        }
        h.payload_len = v;
        cursor += 8;
    }

    if (h.masked) {
        if (buf.size() < cursor + 4) return std::nullopt;
        std::memcpy(h.mask_key, buf.data() + cursor, 4);
        cursor += 4;
    } else {
        std::memset(h.mask_key, 0, 4);
    }

    h.header_size = cursor;
    return h;
}

void encode_frame_header(
    std::string& out,
    WsOpcode opcode,
    bool fin,
    bool masked,
    std::uint64_t payload_len,
    const std::uint8_t mask_key[4]) {

    std::uint8_t b0 = static_cast<std::uint8_t>(opcode) & 0x0F;
    if (fin) b0 |= 0x80;
    out.push_back(static_cast<char>(b0));

    std::uint8_t mask_bit = masked ? 0x80 : 0x00;
    if (payload_len < 126) {
        out.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(payload_len)));
    } else if (payload_len <= 0xFFFF) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload_len & 0xFF));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((payload_len >> shift) & 0xFF));
        }
    }
    if (masked) {
        if (!mask_key) {
            throw std::runtime_error("ws: masked=true but mask_key=null");
        }
        out.append(reinterpret_cast<const char*>(mask_key), 4);
    }
}

std::string generate_sec_websocket_key() {
    std::uint8_t nonce[16];
    if (::RAND_bytes(nonce, sizeof nonce) != 1) {
        throw std::runtime_error("ws: RAND_bytes failed");
    }
    return base64_encode(nonce, sizeof nonce);
}

std::string compute_sec_websocket_accept(std::string_view client_key) {
    std::string concat;
    concat.reserve(client_key.size() + sizeof(kWebSocketGuid) - 1);
    concat.append(client_key);
    concat.append(kWebSocketGuid, sizeof(kWebSocketGuid) - 1);

    std::uint8_t digest[SHA_DIGEST_LENGTH];
    ::SHA1(reinterpret_cast<const unsigned char*>(concat.data()),
           concat.size(), digest);
    return base64_encode(digest, sizeof digest);
}

}  // namespace detail

// ── Impl ──────────────────────────────────────────────────────────

struct WsClient::Impl {
    // Socket owned separately so ssl::stream<socket&> can reference it.
    asio::ip::tcp::socket socket;

    // Populated only on TLS connections. The context is kept alive
    // for the lifetime of the stream.
    std::unique_ptr<asio::ssl::context>                        ssl_ctx;
    std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> tls_stream;

    // Bytes read but not yet consumed by frame parsing.
    std::string read_buf;

    bool close_sent = false;
    bool close_received = false;

    explicit Impl(asio::any_io_executor ex) : socket(ex) {}

    asio::awaitable<std::size_t> read_some(asio::mutable_buffer buf) {
        if (tls_stream) {
            co_return co_await tls_stream->async_read_some(buf, asio::use_awaitable);
        }
        co_return co_await socket.async_read_some(buf, asio::use_awaitable);
    }

    asio::awaitable<void> write_all(asio::const_buffer buf) {
        if (tls_stream) {
            co_await asio::async_write(*tls_stream, buf, asio::use_awaitable);
        } else {
            co_await asio::async_write(socket, buf, asio::use_awaitable);
        }
        co_return;
    }

    /// Fill read_buf until it contains at least `want` bytes past
    /// `already_consumed` (which is how much of read_buf has already
    /// been parsed out by the caller in this frame). Reads are done
    /// via async_read_some on a 4 KB scratch buffer; partial reads
    /// are normal.
    asio::awaitable<void> ensure_available(std::size_t want) {
        while (read_buf.size() < want) {
            std::array<char, 4096> scratch{};
            auto n = co_await read_some(asio::buffer(scratch));
            if (n == 0) {
                throw std::runtime_error("ws: peer closed during read");
            }
            read_buf.append(scratch.data(), n);
        }
        co_return;
    }

    /// Encode + mask + write a frame. Client → server frames MUST be
    /// masked per RFC 6455 §5.3.
    asio::awaitable<void> write_frame(
        WsOpcode opcode, bool fin, std::string_view payload) {
        std::uint8_t mask_key[4];
        if (::RAND_bytes(mask_key, sizeof mask_key) != 1) {
            throw std::runtime_error("ws: RAND_bytes failed");
        }
        std::string header;
        header.reserve(14);
        detail::encode_frame_header(
            header, opcode, fin, /*masked=*/true, payload.size(), mask_key);
        std::string masked(payload);
        detail::apply_mask(masked.data(), masked.size(), mask_key);
        std::string combined;
        combined.reserve(header.size() + masked.size());
        combined.append(header).append(masked);
        co_await write_all(asio::buffer(combined));
        co_return;
    }
};

WsClient::WsClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WsClient::~WsClient() = default;
WsClient::WsClient(WsClient&&) noexcept = default;
WsClient& WsClient::operator=(WsClient&&) noexcept = default;

// ── Frame send/recv ───────────────────────────────────────────────

asio::awaitable<void> WsClient::send_text(std::string_view payload) {
    co_await impl_->write_frame(WsOpcode::Text, true, payload);
    co_return;
}

asio::awaitable<void> WsClient::send_binary(std::string_view payload) {
    co_await impl_->write_frame(WsOpcode::Binary, true, payload);
    co_return;
}

asio::awaitable<void> WsClient::send_close(
    std::uint16_t code, std::string_view reason) {
    if (impl_->close_sent) co_return;
    std::string payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload.append(reason);
    co_await impl_->write_frame(WsOpcode::Close, true, payload);
    impl_->close_sent = true;
    co_return;
}

asio::awaitable<WsMessage> WsClient::recv() {
    // Reassemble fragmented messages: the first data frame carries
    // the real opcode (Text/Binary) with FIN=0; subsequent
    // Continuation frames carry FIN=0 until the final FIN=1. Control
    // frames (§5.5) may be interleaved and are handled inline.
    WsOpcode message_op = WsOpcode::Continuation;
    std::string assembled;
    bool started = false;

    for (;;) {
        // Parse header (may need multiple reads to have enough bytes).
        std::optional<detail::WsFrameHeader> header;
        while (true) {
            header = detail::parse_frame_header(impl_->read_buf);
            if (header) break;
            co_await impl_->ensure_available(impl_->read_buf.size() + 1);
        }

        // RFC 6455 §5.1: server MUST NOT mask frames sent to client.
        if (header->masked) {
            throw std::runtime_error("ws: server sent masked frame");
        }

        // Pull payload bytes into read_buf.
        std::size_t needed = header->header_size + header->payload_len;
        if (impl_->read_buf.size() < needed) {
            co_await impl_->ensure_available(needed);
        }

        std::string_view payload(
            impl_->read_buf.data() + header->header_size,
            static_cast<std::size_t>(header->payload_len));

        // Handle control frames (§5.5) independently of assembly.
        // Control frames MUST NOT be fragmented and payload ≤ 125.
        switch (header->opcode) {
            case WsOpcode::Ping: {
                if (!header->fin || header->payload_len > 125) {
                    throw std::runtime_error("ws: invalid control frame");
                }
                std::string pong_payload(payload);
                // Consume this frame before writing so state is consistent.
                impl_->read_buf.erase(0, needed);
                co_await impl_->write_frame(WsOpcode::Pong, true, pong_payload);
                continue;
            }
            case WsOpcode::Pong: {
                if (!header->fin || header->payload_len > 125) {
                    throw std::runtime_error("ws: invalid control frame");
                }
                impl_->read_buf.erase(0, needed);
                // Unsolicited pongs are legal (§5.5.3) — ignore.
                continue;
            }
            case WsOpcode::Close: {
                if (!header->fin || header->payload_len > 125) {
                    throw std::runtime_error("ws: invalid control frame");
                }
                std::string close_payload(payload);
                impl_->read_buf.erase(0, needed);
                impl_->close_received = true;
                // Echo close back if we haven't already (§5.5.1).
                if (!impl_->close_sent) {
                    co_await impl_->write_frame(
                        WsOpcode::Close, true, close_payload);
                    impl_->close_sent = true;
                }
                co_return WsMessage{WsOpcode::Close, std::move(close_payload)};
            }
            default:
                break;
        }

        // Data frame: Text / Binary / Continuation.
        if (!started) {
            if (header->opcode == WsOpcode::Continuation) {
                throw std::runtime_error(
                    "ws: continuation frame without initial data frame");
            }
            if (header->opcode != WsOpcode::Text &&
                header->opcode != WsOpcode::Binary) {
                throw std::runtime_error("ws: unknown opcode");
            }
            message_op = header->opcode;
            started = true;
        } else {
            if (header->opcode != WsOpcode::Continuation) {
                throw std::runtime_error(
                    "ws: new data frame mid-fragmentation");
            }
        }

        assembled.append(payload);
        bool fin = header->fin;
        impl_->read_buf.erase(0, needed);
        if (fin) {
            co_return WsMessage{message_op, std::move(assembled)};
        }
        // else: keep looping, waiting for continuation / final.
    }
}

// ── ws_connect ────────────────────────────────────────────────────

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Parse an HTTP/1.1 response starting at buf[0]. Returns the offset
/// just past the terminating "\r\n\r\n", or nullopt if headers are
/// not yet complete. Extracts status code and a case-insensitive map
/// of header name → last value (we don't care about duplicates for
/// the handshake check).
struct ParsedUpgradeResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::size_t consumed = 0;
};

std::optional<ParsedUpgradeResponse> try_parse_upgrade_response(
    std::string_view buf) {
    auto end = buf.find("\r\n\r\n");
    if (end == std::string_view::npos) return std::nullopt;
    std::string_view head = buf.substr(0, end);

    ParsedUpgradeResponse out;
    out.consumed = end + 4;

    auto line_end = head.find("\r\n");
    std::string_view status_line = head.substr(0, line_end);
    // "HTTP/1.1 101 Switching Protocols"
    auto sp1 = status_line.find(' ');
    if (sp1 == std::string_view::npos) {
        throw std::runtime_error("ws: malformed status line");
    }
    auto rest = status_line.substr(sp1 + 1);
    auto sp2 = rest.find(' ');
    std::string_view code_str =
        sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);
    int code = 0;
    for (char c : code_str) {
        if (c < '0' || c > '9') {
            throw std::runtime_error("ws: non-numeric status code");
        }
        code = code * 10 + (c - '0');
    }
    out.status = code;

    std::size_t p = (line_end == std::string_view::npos ? head.size() : line_end + 2);
    while (p < head.size()) {
        auto nl = head.find("\r\n", p);
        std::string_view line = head.substr(
            p, nl == std::string_view::npos ? head.size() - p : nl - p);
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view name = line.substr(0, colon);
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.remove_suffix(1);
            }
            out.headers.emplace_back(std::string(name), std::string(value));
        }
        if (nl == std::string_view::npos) break;
        p = nl + 2;
    }
    return out;
}

std::string_view find_header(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view name) {
    for (const auto& [k, v] : headers) {
        if (ieq(k, name)) return v;
    }
    return {};
}

std::string build_upgrade_request(
    std::string_view host,
    std::string_view path,
    std::string_view sec_key,
    const std::vector<std::pair<std::string, std::string>>& extra_headers) {

    std::string req;
    req.reserve(256 + extra_headers.size() * 32);
    req.append("GET ").append(path).append(" HTTP/1.1\r\n");
    req.append("Host: ").append(host).append("\r\n");
    req.append("Upgrade: websocket\r\n");
    req.append("Connection: Upgrade\r\n");
    req.append("Sec-WebSocket-Key: ").append(sec_key).append("\r\n");
    req.append("Sec-WebSocket-Version: 13\r\n");
    for (const auto& [k, v] : extra_headers) {
        // Block the handshake-controlled headers — callers don't
        // need them and silently overwriting is worse than throwing.
        if (ieq(k, "Upgrade") || ieq(k, "Connection") ||
            ieq(k, "Host") ||
            (k.size() >= 14 && ieq(k.substr(0, 14), "Sec-WebSocket-"))) {
            throw std::runtime_error(
                "ws: handshake-reserved header in extra headers: " + k);
        }
        req.append(k).append(": ").append(v).append("\r\n");
    }
    req.append("\r\n");
    return req;
}

}  // namespace

asio::awaitable<std::unique_ptr<WsClient>> ws_connect(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls) {

    auto impl = std::make_unique<WsClient::Impl>(ex);

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        std::string(host), std::string(port), asio::use_awaitable);
    co_await asio::async_connect(impl->socket, endpoints, asio::use_awaitable);

    if (tls) {
        impl->ssl_ctx = std::make_unique<asio::ssl::context>(
            asio::ssl::context::tls_client);
        impl->ssl_ctx->set_default_verify_paths();
        impl->ssl_ctx->set_verify_mode(asio::ssl::verify_peer);

        impl->tls_stream = std::make_unique<
            asio::ssl::stream<asio::ip::tcp::socket&>>(impl->socket, *impl->ssl_ctx);
        std::string host_str(host);
        if (!SSL_set_tlsext_host_name(
                impl->tls_stream->native_handle(), host_str.c_str())) {
            throw asio::system_error{
                asio::error_code{static_cast<int>(::ERR_get_error()),
                                 asio::error::get_ssl_category()},
                "ws: SNI setup"};
        }
        impl->tls_stream->set_verify_callback(
            asio::ssl::host_name_verification{host_str});
        co_await impl->tls_stream->async_handshake(
            asio::ssl::stream_base::client, asio::use_awaitable);
    }

    std::string sec_key = detail::generate_sec_websocket_key();
    std::string req = build_upgrade_request(host, path, sec_key, headers);
    co_await impl->write_all(asio::buffer(req));

    // Read until headers complete.
    std::optional<ParsedUpgradeResponse> parsed;
    while (true) {
        parsed = try_parse_upgrade_response(impl->read_buf);
        if (parsed) break;
        std::array<char, 4096> scratch{};
        auto n = co_await impl->read_some(asio::buffer(scratch));
        if (n == 0) {
            throw std::runtime_error(
                "ws: peer closed before handshake response");
        }
        impl->read_buf.append(scratch.data(), n);
    }

    if (parsed->status != 101) {
        throw std::runtime_error(
            "ws: upgrade refused, status " + std::to_string(parsed->status));
    }
    auto upgrade_h    = find_header(parsed->headers, "Upgrade");
    auto connection_h = find_header(parsed->headers, "Connection");
    auto accept_h     = find_header(parsed->headers, "Sec-WebSocket-Accept");
    if (!ieq(upgrade_h, "websocket")) {
        throw std::runtime_error("ws: missing or wrong Upgrade header");
    }
    // Connection header can be a comma-separated list; look for Upgrade token.
    {
        bool saw_upgrade = false;
        std::size_t p = 0;
        while (p < connection_h.size()) {
            auto comma = connection_h.find(',', p);
            std::string_view tok = connection_h.substr(
                p, comma == std::string_view::npos ? connection_h.size() - p : comma - p);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                tok.remove_prefix(1);
            while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                tok.remove_suffix(1);
            if (ieq(tok, "Upgrade")) { saw_upgrade = true; break; }
            if (comma == std::string_view::npos) break;
            p = comma + 1;
        }
        if (!saw_upgrade) {
            throw std::runtime_error("ws: missing Connection: Upgrade");
        }
    }
    std::string expected_accept = detail::compute_sec_websocket_accept(sec_key);
    if (accept_h != expected_accept) {
        throw std::runtime_error("ws: Sec-WebSocket-Accept mismatch");
    }

    // Drop consumed response bytes — anything trailing is the first
    // server frame arriving eagerly and stays in read_buf.
    impl->read_buf.erase(0, parsed->consumed);

    co_return std::unique_ptr<WsClient>(new WsClient(std::move(impl)));
}

}  // namespace neograph::async
