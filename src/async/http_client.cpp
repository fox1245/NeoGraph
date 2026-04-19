// Minimal async HTTP/1.1 POST client. See header for scope + caveats.
//
// Wire shape per call:
//   resolve → connect → [TLS handshake] → write(request) →
//   read_until("\r\n\r\n") → parse status + Content-Length →
//   read_exactly(Content-Length) → close.
//
// The request line, headers, and body are assembled into a single
// std::string and written with one async_write call. Response parse is
// straightforward because we don't support chunked encoding: once we
// see the header terminator we trust Content-Length and pull that many
// bytes verbatim.
//
// The same exchange template runs over either a plain tcp::socket or
// an asio::ssl::stream<tcp::socket&>; async_post picks one based on
// the `tls` flag.

#include <neograph/async/http_client.h>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <string>

namespace neograph::async {

namespace {

std::string build_request(std::string_view host,
                          std::string_view path,
                          std::string_view body,
                          const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string out;
    out.reserve(256 + body.size());
    out.append("POST ").append(path).append(" HTTP/1.1\r\n");
    out.append("Host: ").append(host).append("\r\n");
    out.append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
    out.append("Connection: close\r\n");
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
int parse_status_line(std::string_view line) {
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

// Scan headers until the blank line. Returns Content-Length (or 0 if
// absent, or -1 on any chunked encoding which we don't support).
long extract_content_length(std::istream& in) {
    std::string line;
    long content_length = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // end of headers
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // left-trim value
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) value.clear();
        else value = value.substr(first);
        // lowercase name
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (name == "content-length") {
            std::from_chars(value.data(), value.data() + value.size(), content_length);
        } else if (name == "transfer-encoding") {
            // chunked etc. — not supported in PoC.
            std::string lv;
            std::transform(value.begin(), value.end(), std::back_inserter(lv),
                           [](unsigned char c) { return std::tolower(c); });
            if (lv.find("chunked") != std::string::npos) return -1;
        }
    }
    return content_length;
}

// Drive the HTTP exchange over any AsyncReadStream+AsyncWriteStream.
// Separate from async_post so we can share it between the plain TCP
// and the TLS-wrapped stream without duplicating parsing logic.
template <typename Stream>
asio::awaitable<HttpResponse> run_exchange(Stream& stream, const std::string& req) {
    co_await asio::async_write(stream, asio::buffer(req), asio::use_awaitable);

    // Read up to and including the blank line that terminates headers.
    asio::streambuf buf;
    co_await asio::async_read_until(stream, buf, "\r\n\r\n", asio::use_awaitable);

    std::istream is(&buf);
    std::string status_line;
    std::getline(is, status_line);
    if (!status_line.empty() && status_line.back() == '\r') status_line.pop_back();
    HttpResponse resp;
    resp.status = parse_status_line(status_line);

    long content_length = extract_content_length(is);
    if (content_length < 0) {
        throw std::runtime_error("async_post: chunked transfer-encoding not supported");
    }

    // buf may already contain part (or all) of the body beyond headers.
    // Extract whatever's there through the istream (same backing
    // streambuf, so advancing `is` also consumes from buf), then pull
    // the remaining bytes from the stream if needed.
    resp.body.resize(content_length);
    long filled = 0;
    auto already = buf.size();
    if (already > 0) {
        long take = std::min<long>(static_cast<long>(already), content_length);
        is.read(resp.body.data(), take);
        filled = is.gcount();
    }
    if (filled < content_length) {
        long remaining = content_length - filled;
        co_await asio::async_read(stream,
                                  asio::buffer(resp.body.data() + filled, remaining),
                                  asio::transfer_exactly(remaining),
                                  asio::use_awaitable);
    }
    co_return resp;
}

} // namespace

asio::awaitable<HttpResponse> async_post(
    asio::any_io_executor ex,
    std::string_view host,
    std::string_view port,
    std::string_view path,
    std::string_view body,
    std::vector<std::pair<std::string, std::string>> headers,
    bool tls) {

    asio::ip::tcp::resolver resolver{ex};
    auto endpoints = co_await resolver.async_resolve(
        std::string(host), std::string(port), asio::use_awaitable);

    asio::ip::tcp::socket sock{ex};
    co_await asio::async_connect(sock, endpoints, asio::use_awaitable);

    std::string req = build_request(host, path, body, headers);
    HttpResponse resp;

    if (!tls) {
        resp = co_await run_exchange(sock, req);

        // SO_LINGER { on, 0 } → close() sends RST instead of FIN,
        // skipping TIME_WAIT. Only safe in benches where the
        // request/response is fully exchanged before close; for
        // production HTTP a graceful shutdown is correct. Avoids
        // ephemeral-port exhaustion when 1000s of agents each open
        // one connection per LLM call. Once the keep-alive pool
        // (Semester 1.2) lands this hack goes away.
        asio::error_code ec;
        sock.set_option(asio::socket_base::linger(true, 0), ec);
        sock.close(ec);
        co_return resp;
    }

    // ── TLS path ──────────────────────────────────────────────────
    // Fresh context per call is intentional for Semester 1.1: it
    // keeps the TU standalone and avoids sharing mutable OpenSSL
    // state across coroutines without a strand. Session resumption
    // + context reuse arrives with the keep-alive pool in 1.2.
    asio::ssl::context ctx{asio::ssl::context::tls_client};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(asio::ssl::verify_peer);

    asio::ssl::stream<asio::ip::tcp::socket&> tls_stream{sock, ctx};

    // SNI: without this the server returns a generic cert (or the
    // wrong vhost) on shared hosting, and Anthropic/OpenAI both
    // require SNI for TLS 1.3 cert selection.
    std::string host_str(host);
    if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), host_str.c_str())) {
        throw asio::system_error{
            asio::error_code{static_cast<int>(::ERR_get_error()),
                             asio::error::get_ssl_category()},
            "SNI setup"};
    }
    tls_stream.set_verify_callback(asio::ssl::host_name_verification{host_str});

    co_await tls_stream.async_handshake(
        asio::ssl::stream_base::client, asio::use_awaitable);

    resp = co_await run_exchange(tls_stream, req);

    // Graceful TLS shutdown. Servers commonly close first and we
    // receive a short_read / EOF — treat both as clean. We already
    // have the full body; swallow shutdown errors rather than
    // surfacing them as a failed request.
    try {
        co_await tls_stream.async_shutdown(asio::use_awaitable);
    } catch (const std::exception&) {
        // Benign: peer closed first, or TLS record-layer EOF.
    }
    asio::error_code ec;
    sock.close(ec);
    co_return resp;
}

} // namespace neograph::async
