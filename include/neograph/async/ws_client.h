/**
 * @file ws_client.h
 * @brief Minimal async WebSocket (RFC 6455) client on asio coroutines.
 *
 * Phase A primitive — transport only, no protocol-specific framing
 * above RFC 6455. Target use: OpenAI Responses WebSocket mode
 * (wss://api.openai.com/v1/responses), but the client is generic:
 * any wss:// or ws:// endpoint that speaks vanilla RFC 6455.
 *
 * Scope covered:
 *   - HTTP/1.1 Upgrade handshake (client side) with Bearer-style
 *     custom header injection.
 *   - Sec-WebSocket-Accept verification against the randomly-generated
 *     Sec-WebSocket-Key (SHA-1 + base64, per RFC 6455 §4.2.2).
 *   - TLS via asio::ssl::stream (reused from http_client.cpp path —
 *     OpenSSL default trust store + SNI + hostname verification).
 *   - Frame codec: text + binary + ping + pong + close opcodes;
 *     7 / 16-bit / 64-bit extended payload lengths; client-to-server
 *     masking per §5.3.
 *   - recv() auto-replies to ping with pong and surfaces close frames
 *     to the caller.
 *   - Message reassembly across fragmented frames (FIN=0 continuation).
 *
 * Scope NOT covered (deliberate — keep the primitive lean):
 *   - Permessage-deflate / extensions.
 *   - Subprotocol negotiation (Sec-WebSocket-Protocol is not set).
 *   - Back-pressure beyond what asio's async_write already provides.
 *   - Auto-reconnect / ping scheduling (caller drives recv loop).
 *
 * Threading: a WsClient instance is NOT thread-safe. Run all
 * send/recv on the same executor. Concurrent send + recv on the same
 * instance is also unsafe — serialize at the caller.
 */
#pragma once

#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::async {

enum class WsOpcode : std::uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct WsMessage {
    /// Text or Binary — the assembled opcode of the first frame in a
    /// (possibly fragmented) message. Control frames (Close/Ping/Pong)
    /// never surface here — recv() handles them internally, except
    /// Close which terminates recv() with op == Close and payload
    /// containing the RFC 6455 §5.5.1 close-reason bytes verbatim
    /// (2-byte status code + optional UTF-8 reason, or empty).
    WsOpcode    op;
    std::string payload;
};

/// Async WebSocket client. Obtain via ws_connect; use send_* / recv
/// on the returned instance. Destruction closes the socket
/// unconditionally (no graceful close unless you called send_close
/// first).
class WsClient {
  public:
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;
    WsClient(WsClient&&) noexcept;
    WsClient& operator=(WsClient&&) noexcept;

    /// Send a text frame (FIN=1, no fragmentation). Payload must be
    /// valid UTF-8 per RFC 6455 §5.6 — this client does NOT validate;
    /// the caller is responsible. Masked per §5.3.
    asio::awaitable<void> send_text(std::string_view payload);

    /// Send a binary frame (FIN=1). Masked per §5.3.
    asio::awaitable<void> send_binary(std::string_view payload);

    /// Send a close frame with the given status code and optional
    /// UTF-8 reason. After send_close the caller should drain recv()
    /// until it returns an op==Close frame (peer echo), then let the
    /// destructor close the socket. Calling send_close twice is a
    /// no-op.
    asio::awaitable<void> send_close(
        std::uint16_t code = 1000, std::string_view reason = "");

    /// Block until the next application message arrives. Pings are
    /// auto-replied with pongs and consumed; close frames are echoed
    /// back and surfaced to the caller as op==Close (after which the
    /// connection is no longer usable).
    ///
    /// Throws asio::system_error on transport failure or
    /// std::runtime_error on malformed frames (reserved bits set,
    /// masked server-to-client frame, unknown opcode).
    asio::awaitable<WsMessage> recv();

  private:
    friend asio::awaitable<std::unique_ptr<WsClient>> ws_connect(
        asio::any_io_executor, std::string_view, std::string_view,
        std::string_view, std::vector<std::pair<std::string, std::string>>,
        bool);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit WsClient(std::unique_ptr<Impl>);
};

/// Establish a WebSocket connection.
///
/// @param ex      Executor hosting the connection.
/// @param host    Target host (SNI + Host header + cert verification).
/// @param port    Port string ("443", "80", etc.).
/// @param path    Origin-form path starting with '/'. Query string
///                allowed; fragment not.
/// @param headers Extra HTTP headers injected into the Upgrade request.
///                Typical use: {"Authorization","Bearer sk-..."}.
///                Do NOT set Upgrade/Connection/Sec-WebSocket-* here —
///                the client sets those itself.
/// @param tls     Use wss:// (TLS) vs ws:// (plain TCP).
///
/// Throws asio::system_error on transport failure, std::runtime_error
/// if the server refuses the upgrade (non-101 status or bad Accept).
asio::awaitable<std::unique_ptr<WsClient>> ws_connect(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bool tls = true);

namespace detail {

/// Parsed RFC 6455 frame header. Populated by parse_frame_header.
struct WsFrameHeader {
    WsOpcode      opcode;
    bool          fin;
    bool          masked;
    std::uint64_t payload_len;
    std::uint8_t  mask_key[4];
    /// Offset past the header where payload bytes begin.
    std::size_t   header_size;
};

/// Parse a frame header from the front of `buf`. Returns nullopt if
/// `buf` is too short to contain a complete header (caller must read
/// more bytes and retry). Throws std::runtime_error on malformed
/// framing (RSV bits set — we don't negotiate extensions).
std::optional<WsFrameHeader> parse_frame_header(std::string_view buf);

/// Append an encoded frame header to `out`. Writes either 2, 4, 6,
/// 10, or 14 bytes depending on payload_len and the `masked` flag.
void encode_frame_header(
    std::string& out,
    WsOpcode opcode,
    bool fin,
    bool masked,
    std::uint64_t payload_len,
    const std::uint8_t mask_key[4] = nullptr);

/// XOR `data` in place with the 4-byte `mask_key` per §5.3.
void apply_mask(char* data, std::size_t len, const std::uint8_t mask_key[4]) noexcept;

/// Generate the 16-byte Sec-WebSocket-Key value (random) as its
/// base64 encoding per §4.1. Exposed for testability.
std::string generate_sec_websocket_key();

/// Compute the expected Sec-WebSocket-Accept value for a given
/// client key per §4.2.2: base64(SHA-1(key + GUID)).
std::string compute_sec_websocket_accept(std::string_view client_key);

}  // namespace detail

}  // namespace neograph::async
