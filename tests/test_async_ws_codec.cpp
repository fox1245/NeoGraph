// Unit tests for the RFC 6455 frame codec exposed via
// neograph::async::detail. Pure byte-level — no sockets.

#include <neograph/async/ws_client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

using neograph::async::WsOpcode;
using neograph::async::detail::apply_mask;
using neograph::async::detail::compute_sec_websocket_accept;
using neograph::async::detail::encode_frame_header;
using neograph::async::detail::generate_sec_websocket_key;
using neograph::async::detail::parse_frame_header;

namespace {

std::string encode_and_mask(WsOpcode op, bool fin, std::string_view payload,
                            const std::uint8_t mask_key[4]) {
    std::string out;
    encode_frame_header(out, op, fin, /*masked=*/true, payload.size(), mask_key);
    auto header_len = out.size();
    out.append(payload);
    apply_mask(out.data() + header_len, payload.size(), mask_key);
    return out;
}

}  // namespace

TEST(WsCodec, SmallTextFrameRoundTrip) {
    // 5-byte text "Hello" with known mask from RFC 6455 §5.7 example.
    const std::uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
    std::string frame = encode_and_mask(WsOpcode::Text, true, "Hello", mask);

    // RFC 6455 §5.7 expected bytes: 81 85 37 fa 21 3d 7f 9f 4d 51 58
    ASSERT_EQ(frame.size(), 11u);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[0]), 0x81);  // FIN=1, op=Text
    EXPECT_EQ(static_cast<std::uint8_t>(frame[1]), 0x85);  // mask=1, len=5
    EXPECT_EQ(static_cast<std::uint8_t>(frame[2]), 0x37);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[3]), 0xfa);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[4]), 0x21);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[5]), 0x3d);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[6]), 0x7f);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[7]), 0x9f);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[8]), 0x4d);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[9]), 0x51);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[10]), 0x58);

    // Parse it back.
    auto h = parse_frame_header(frame);
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(h->fin);
    EXPECT_EQ(h->opcode, WsOpcode::Text);
    EXPECT_TRUE(h->masked);
    EXPECT_EQ(h->payload_len, 5u);
    EXPECT_EQ(h->header_size, 6u);

    std::string decoded(frame.data() + h->header_size, h->payload_len);
    apply_mask(decoded.data(), decoded.size(), h->mask_key);
    EXPECT_EQ(decoded, "Hello");
}

TEST(WsCodec, ExtendedLength16Boundary) {
    // 126-byte payload → 2-byte extended length.
    std::string payload(126, 'A');
    std::string out;
    encode_frame_header(out, WsOpcode::Binary, true, false, payload.size());
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[0]), 0x82);
    EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 126);  // marker
    EXPECT_EQ(static_cast<std::uint8_t>(out[2]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[3]), 126);

    auto h = parse_frame_header(out);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->payload_len, 126u);
    EXPECT_EQ(h->header_size, 4u);
    EXPECT_FALSE(h->masked);
}

TEST(WsCodec, ExtendedLength64) {
    // 0x1_0000 = 65536 bytes → 8-byte extended length.
    std::uint64_t len = 0x10000;
    std::string out;
    encode_frame_header(out, WsOpcode::Binary, true, false, len);
    ASSERT_EQ(out.size(), 10u);
    EXPECT_EQ(static_cast<std::uint8_t>(out[1]), 127);
    // bytes 2..9 are big-endian 8-byte length
    EXPECT_EQ(static_cast<std::uint8_t>(out[2]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[3]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[4]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[5]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[6]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[7]), 0x01);
    EXPECT_EQ(static_cast<std::uint8_t>(out[8]), 0x00);
    EXPECT_EQ(static_cast<std::uint8_t>(out[9]), 0x00);

    auto h = parse_frame_header(out);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(h->payload_len, len);
    EXPECT_EQ(h->header_size, 10u);
}

TEST(WsCodec, ParseReturnsNulloptOnShortBuffer) {
    // Frame claims 126 payload (→ 2-byte extension), but we only
    // have 3 bytes so the length field is incomplete.
    std::string partial;
    partial.push_back(static_cast<char>(0x81));
    partial.push_back(static_cast<char>(126));
    partial.push_back(static_cast<char>(0x00));
    EXPECT_FALSE(parse_frame_header(partial).has_value());

    // Single byte — can't even get past b0/b1.
    std::string one(1, '\x81');
    EXPECT_FALSE(parse_frame_header(one).has_value());

    // Empty.
    EXPECT_FALSE(parse_frame_header(std::string_view{}).has_value());
}

TEST(WsCodec, ParseRejectsReservedBits) {
    // RSV1 set — any rsv bit MUST be 0 if no extension is negotiated.
    std::string bad;
    bad.push_back(static_cast<char>(0xC1));  // FIN=1, RSV1=1, op=Text
    bad.push_back(static_cast<char>(0x00));  // mask=0, len=0
    EXPECT_THROW(parse_frame_header(bad), std::runtime_error);
}

TEST(WsCodec, Parse64BitLengthWithHighBitRejected) {
    // MSB of the 8-byte length must be 0 per §5.2.
    std::string bad;
    bad.push_back(static_cast<char>(0x82));       // FIN=1, op=Binary
    bad.push_back(static_cast<char>(127));        // 64-bit ext marker
    bad.push_back(static_cast<char>(0x80));       // MSB set → illegal
    for (int i = 0; i < 7; ++i) bad.push_back(0);
    EXPECT_THROW(parse_frame_header(bad), std::runtime_error);
}

TEST(WsCodec, MaskXorIsItsOwnInverse) {
    const std::uint8_t key[4] = {0x11, 0x22, 0x33, 0x44};
    std::string payload = "The quick brown fox jumps over the lazy dog";
    std::string working = payload;
    apply_mask(working.data(), working.size(), key);
    EXPECT_NE(working, payload);  // actually masked
    apply_mask(working.data(), working.size(), key);
    EXPECT_EQ(working, payload);  // round-trip restores original
}

TEST(WsCodec, EncodeMaskedFrameWithNullMaskKeyThrows) {
    std::string out;
    EXPECT_THROW(
        encode_frame_header(out, WsOpcode::Text, true, /*masked=*/true, 5),
        std::runtime_error);
}

// ── Sec-WebSocket-Accept key derivation ─────────────────────────────

TEST(WsCodec, AcceptKeyMatchesRfcExample) {
    // RFC 6455 §1.3 example test vector:
    //   Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
    //   Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
    EXPECT_EQ(
        compute_sec_websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsCodec, GenerateKeyIs24CharBase64) {
    // 16 random bytes → base64 → 24 chars (with padding).
    auto k = generate_sec_websocket_key();
    EXPECT_EQ(k.size(), 24u);
    EXPECT_EQ(k.back(), '=');
    // Ensure successive calls produce different keys — collision is
    // astronomically unlikely with 128 bits of entropy.
    auto k2 = generate_sec_websocket_key();
    EXPECT_NE(k, k2);
}
