// HTTP/2 client over nghttp2 + asio. Single request per connection
// (Phase 1) — multiplexing arrives in a later commit. See header for
// scope, limits, and roadmap.
//
// Wire flow:
//   1. TCP connect → TLS handshake with ALPN advertising "h2".
//   2. Verify the server picked h2 (otherwise throw).
//   3. Send HTTP/2 client connection preface ("PRI * HTTP/2.0...").
//   4. Init nghttp2_session as client; submit one POST stream.
//   5. Drive read/write loop: feed bytes from socket into
//      nghttp2_session_mem_recv2, drain nghttp2_session_mem_send2 to
//      the socket. Stop when our stream closes.
//   6. Return assembled HttpResponse.
//
// nghttp2 is callback-driven. We stash a Stream struct as the
// nghttp2_session user_data (and per-stream user_data) so callbacks
// can fill HttpResponse fields without globals.

#include <neograph/async/http2_client.h>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neograph::async {

namespace {

// Per-call state. Lives on the coroutine stack; nghttp2 callbacks
// receive a pointer through user_data slots. Single-stream so the
// "active stream id" is implicit — set on submit_request, cleared
// when the stream closes.
struct StreamState {
    int32_t       stream_id = -1;
    int           status    = 0;
    bool          done      = false;
    std::string   body;
    std::vector<std::pair<std::string, std::string>> headers;
    // Request payload that nghttp2 will pull via data_source_read_callback.
    std::string_view req_body;
    std::size_t      req_body_offset = 0;
    // Failure capture from inside callbacks — surfaced on next
    // suspension boundary as an exception.
    std::string error;
};

// nghttp2 callbacks ────────────────────────────────────────────────

// Stream-closed: server sent END_STREAM (or RST). Mark done.
int on_stream_close_cb(nghttp2_session*, int32_t stream_id,
                       uint32_t /*error_code*/, void* user_data) {
    auto* st = static_cast<StreamState*>(user_data);
    if (st && st->stream_id == stream_id) {
        st->done = true;
    }
    return 0;
}

// Header frame received: pull the :status pseudo-header + each real
// header into the response struct.
int on_header_cb(nghttp2_session*, const nghttp2_frame* frame,
                 const uint8_t* name, std::size_t namelen,
                 const uint8_t* value, std::size_t valuelen,
                 uint8_t /*flags*/, void* user_data) {
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;
    auto* st = static_cast<StreamState*>(user_data);
    if (!st) return 0;
    std::string n(reinterpret_cast<const char*>(name), namelen);
    std::string v(reinterpret_cast<const char*>(value), valuelen);
    if (n == ":status") {
        st->status = std::stoi(v);
    } else {
        st->headers.emplace_back(std::move(n), std::move(v));
    }
    return 0;
}

// Data chunk received on a stream: append to body.
int on_data_chunk_recv_cb(nghttp2_session*, uint8_t /*flags*/,
                          int32_t stream_id,
                          const uint8_t* data, std::size_t len,
                          void* user_data) {
    auto* st = static_cast<StreamState*>(user_data);
    if (st && st->stream_id == stream_id) {
        st->body.append(reinterpret_cast<const char*>(data), len);
    }
    return 0;
}

// Provide request body bytes when nghttp2 asks. Single shot —
// returns the entire body, then NGHTTP2_DATA_FLAG_EOF.
ssize_t data_source_read_cb(nghttp2_session*, int32_t /*stream_id*/,
                            uint8_t* buf, std::size_t length,
                            uint32_t* data_flags,
                            nghttp2_data_source* source,
                            void* /*user_data*/) {
    auto* st = static_cast<StreamState*>(source->ptr);
    std::size_t remaining = st->req_body.size() - st->req_body_offset;
    std::size_t to_copy   = std::min(remaining, length);
    if (to_copy > 0) {
        std::memcpy(buf, st->req_body.data() + st->req_body_offset, to_copy);
        st->req_body_offset += to_copy;
    }
    if (st->req_body_offset >= st->req_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<ssize_t>(to_copy);
}

// RAII wrapper for nghttp2_session* + nghttp2_session_callbacks*.
struct Session {
    nghttp2_session*           sess = nullptr;
    nghttp2_session_callbacks* cbs  = nullptr;

    ~Session() {
        if (sess) nghttp2_session_del(sess);
        if (cbs)  nghttp2_session_callbacks_del(cbs);
    }

    void init(StreamState* st) {
        if (nghttp2_session_callbacks_new(&cbs) != 0)
            throw std::runtime_error("nghttp2: callbacks_new failed");
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs,
            on_stream_close_cb);
        nghttp2_session_callbacks_set_on_header_callback(cbs,
            on_header_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs,
            on_data_chunk_recv_cb);
        if (nghttp2_session_client_new(&sess, cbs, st) != 0)
            throw std::runtime_error("nghttp2: client_new failed");

        // Send initial SETTINGS — match nghttp2 CLI's defaults.
        // Cloudflare's WAF rejects (400 Bad Request) clients that send
        // empty or unusual SETTINGS, comparing against expected
        // browser/curl signatures. These two values match nghttp's
        // out-of-the-box behavior + curl's HTTP/2 fingerprint and
        // get past the WAF cleanly.
        nghttp2_settings_entry iv[2] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    65535},
        };
        if (nghttp2_submit_settings(sess, NGHTTP2_FLAG_NONE, iv, 2) != 0)
            throw std::runtime_error("nghttp2: submit_settings failed");
    }
};

// Pump pending output frames from nghttp2 to the wire.
template <typename Stream>
asio::awaitable<void> pump_send(nghttp2_session* sess, Stream& s) {
    while (nghttp2_session_want_write(sess)) {
        const uint8_t* data = nullptr;
        ssize_t n = nghttp2_session_mem_send(sess, &data);
        if (n < 0) {
            throw std::runtime_error(
                std::string("nghttp2 mem_send: ") + nghttp2_strerror(static_cast<int>(n)));
        }
        if (n == 0) break;
        if (const char* dbg = std::getenv("NEOGRAPH_H2_DEBUG"); dbg && *dbg == '1') {
            std::fprintf(stderr, "[h2 send] %zd bytes\n", n);
        }
        co_await asio::async_write(
            s, asio::buffer(data, static_cast<std::size_t>(n)),
            asio::use_awaitable);
    }
}

// Read one TLS record's worth and feed nghttp2.
template <typename Stream>
asio::awaitable<void> pump_recv(nghttp2_session* sess, Stream& s) {
    std::array<uint8_t, 8192> buf{};
    auto n = co_await s.async_read_some(asio::buffer(buf), asio::use_awaitable);
    ssize_t consumed = nghttp2_session_mem_recv(sess, buf.data(), n);
    if (consumed < 0) {
        throw std::runtime_error(
            std::string("nghttp2 mem_recv: ") + nghttp2_strerror(static_cast<int>(consumed)));
    }
}

// Drive the request/response cycle on a connected, h2-negotiated
// stream until our stream completes.
template <typename Stream>
asio::awaitable<void> drive_exchange(
    nghttp2_session* sess, Stream& s, StreamState& st) {
    while (!st.done) {
        co_await pump_send(sess, s);
        if (st.done) break;
        if (!nghttp2_session_want_read(sess)) {
            throw std::runtime_error("nghttp2: stream incomplete but no read");
        }
        co_await pump_recv(sess, s);
    }
    // Final flush — server may have queued WINDOW_UPDATE / GOAWAY.
    co_await pump_send(sess, s);
}

} // namespace

asio::awaitable<HttpResponse> async_post_h2(
    asio::any_io_executor ex,
    std::string host,
    std::string port,
    std::string path,
    std::string body,
    std::vector<std::pair<std::string, std::string>> req_headers,
    RequestOptions opts) {

    (void)opts;  // timeout/redirect not yet wired — see header roadmap

    // ── TCP + TLS with ALPN h2 ─────────────────────────────────────
    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        host, port, asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);

    // ALPN: advertise only "h2" so the server has to either pick HTTP/2
    // or fail the negotiation. Bytes are length-prefixed per RFC 7301.
    static const std::array<uint8_t, 3> alpn = {2, 'h', '2'};
    SSL_CTX_set_alpn_protos(ctx.native_handle(), alpn.data(), alpn.size());

    asio::ssl::stream<asio::ip::tcp::socket&> tls_stream{sock, ctx};
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    tls_stream.set_verify_callback(asio::ssl::host_name_verification{host});
    co_await tls_stream.async_handshake(
        asio::ssl::stream_base::client, asio::use_awaitable);

    // Confirm the server picked h2.
    const unsigned char* selected = nullptr;
    unsigned int selected_len = 0;
    SSL_get0_alpn_selected(tls_stream.native_handle(), &selected, &selected_len);
    if (selected_len != 2 || std::memcmp(selected, "h2", 2) != 0) {
        throw std::runtime_error(
            "async_post_h2: server did not negotiate h2 (ALPN mismatch). "
            "Use async_post (HTTP/1.1) for this endpoint.");
    }

    // ── nghttp2 session + connection preface ───────────────────────
    StreamState st;
    st.req_body = body;
    Session session;
    session.init(&st);

    // Build :method/:scheme/:authority/:path + caller headers as
    // nghttp2_nv. nghttp2_nv references the bytes by pointer +
    // length — strings must out-live the submit_request call. Move
    // ownership into a local vector so dangling refs aren't a worry.
    std::string method = "POST";
    std::string scheme = "https";
    // :authority follows RFC 3986 — omit port for HTTPS default (443).
    // Cloudflare in particular rejects "host:443" with a 400.
    std::string authority = (port == "443") ? host : (host + ":" + port);
    std::string len_str = std::to_string(body.size());

    auto nv = [](const std::string& n, const std::string& v) {
        return nghttp2_nv{
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(n.data())),
            const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(v.data())),
            n.size(), v.size(),
            NGHTTP2_NV_FLAG_NONE};
    };

    std::vector<nghttp2_nv> nva;
    nva.reserve(4 + req_headers.size() + 2);
    nva.push_back(nv(":method",    method));
    nva.push_back(nv(":scheme",    scheme));
    nva.push_back(nv(":authority", authority));
    nva.push_back(nv(":path",      path));
    bool has_ctype = false;
    bool has_clen  = false;
    for (auto& [k, v] : req_headers) {
        // Lowercase the name in-place — HTTP/2 mandates lowercase
        // header names. RFC 7540 §8.1.2.
        std::transform(k.begin(), k.end(), k.begin(),
            [](unsigned char c){ return std::tolower(c); });
        if (k == "content-type") has_ctype = true;
        if (k == "content-length") has_clen = true;
        // host pseudo-header is :authority; drop a literal Host.
        if (k == "host") continue;
        nva.push_back(nv(k, v));
    }
    std::string ctype_default = "application/json";
    if (!has_ctype) nva.push_back(nv("content-type", ctype_default));
    if (!has_clen)  nva.push_back(nv("content-length", len_str));

    nghttp2_data_provider data_prd{};
    data_prd.source.ptr = &st;
    data_prd.read_callback = data_source_read_cb;

    int32_t stream_id = nghttp2_submit_request(
        session.sess, nullptr, nva.data(), nva.size(), &data_prd, nullptr);
    if (stream_id < 0) {
        throw std::runtime_error(
            std::string("nghttp2_submit_request: ") + nghttp2_strerror(stream_id));
    }
    st.stream_id = stream_id;
    nghttp2_session_set_stream_user_data(session.sess, stream_id, &st);

    // ── Drive the I/O until our stream completes ───────────────────
    co_await drive_exchange(session.sess, tls_stream, st);

    // ── Graceful shutdown of TLS + socket ──────────────────────────
    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Peer may have closed first.
    }
    asio::error_code ec;
    sock.close(ec);

    HttpResponse resp;
    resp.status  = st.status;
    resp.headers = std::move(st.headers);
    resp.body    = std::move(st.body);
    co_return resp;
}

} // namespace neograph::async
