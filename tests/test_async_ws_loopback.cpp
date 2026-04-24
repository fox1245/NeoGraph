// Loopback integration test for the WebSocket client. Spawns a small
// RFC 6455 echo server on 127.0.0.1:<ephemeral>, then drives
// ws_connect + send_text + recv + send_close through it.
//
// Plain ws:// (no TLS) to keep the test self-contained — the TLS
// branch is exercised by the OpenAI integration test in a later
// phase, and lives off the ctest default path.

#include <neograph/async/ws_client.h>

#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using neograph::async::WsClient;
using neograph::async::WsMessage;
using neograph::async::WsOpcode;
using neograph::async::ws_connect;

namespace detail_ws = neograph::async::detail;

namespace {

// Minimal RFC 6455 echo server. Exits after the client's Close
// round-trip completes or peer disconnects. Not a general-purpose
// server — only does what the tests need.
asio::awaitable<void> serve_one(asio::ip::tcp::socket sock) {
    // ── Read handshake ──
    std::string buf;
    std::size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        std::array<char, 4096> scratch{};
        auto n = co_await sock.async_read_some(
            asio::buffer(scratch), asio::use_awaitable);
        if (n == 0) co_return;
        buf.append(scratch.data(), n);
        hdr_end = buf.find("\r\n\r\n");
    }
    auto head = buf.substr(0, hdr_end);
    std::string key;
    {
        auto pos = head.find("Sec-WebSocket-Key:");
        if (pos == std::string::npos) {
            pos = head.find("sec-websocket-key:");
        }
        if (pos == std::string::npos) co_return;
        pos = head.find(':', pos) + 1;
        while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t')) ++pos;
        auto nl = head.find("\r\n", pos);
        key = head.substr(pos, nl - pos);
    }
    std::string accept = detail_ws::compute_sec_websocket_accept(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    co_await asio::async_write(sock, asio::buffer(resp), asio::use_awaitable);
    buf.erase(0, hdr_end + 4);

    // ── Echo loop ──
    for (;;) {
        std::optional<detail_ws::WsFrameHeader> h;
        while (!(h = detail_ws::parse_frame_header(buf))) {
            std::array<char, 4096> scratch{};
            auto n = co_await sock.async_read_some(
                asio::buffer(scratch), asio::use_awaitable);
            if (n == 0) co_return;
            buf.append(scratch.data(), n);
        }
        std::size_t needed = h->header_size + h->payload_len;
        while (buf.size() < needed) {
            std::array<char, 4096> scratch{};
            auto n = co_await sock.async_read_some(
                asio::buffer(scratch), asio::use_awaitable);
            if (n == 0) co_return;
            buf.append(scratch.data(), n);
        }
        std::string payload(buf.data() + h->header_size, h->payload_len);
        if (h->masked) {
            detail_ws::apply_mask(payload.data(), payload.size(), h->mask_key);
        }
        buf.erase(0, needed);

        // Server → client frames MUST be unmasked (§5.1).
        std::string out;
        detail_ws::encode_frame_header(
            out, h->opcode, h->fin, /*masked=*/false, payload.size());
        out.append(payload);
        co_await asio::async_write(sock, asio::buffer(out), asio::use_awaitable);

        if (h->opcode == WsOpcode::Close) {
            co_return;
        }
    }
}

asio::awaitable<std::uint16_t> listen_and_serve(
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor) {
    auto sock = co_await acceptor->async_accept(asio::use_awaitable);
    co_await serve_one(std::move(sock));
    co_return std::uint16_t{0};
}

}  // namespace

class WsLoopback : public ::testing::Test {
  protected:
    void SetUp() override {
        ioc_ = std::make_unique<asio::io_context>();
        acceptor_ = std::make_shared<asio::ip::tcp::acceptor>(
            *ioc_,
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        port_ = acceptor_->local_endpoint().port();

        // Kick off the accept-and-serve coroutine on the io_context.
        asio::co_spawn(
            *ioc_, listen_and_serve(acceptor_), asio::detached);

        thread_ = std::thread([this] { ioc_->run(); });
    }

    void TearDown() override {
        ioc_->stop();
        if (thread_.joinable()) thread_.join();
    }

    std::unique_ptr<asio::io_context>         ioc_;
    std::shared_ptr<asio::ip::tcp::acceptor>  acceptor_;
    std::uint16_t                             port_ = 0;
    std::thread                               thread_;
};

TEST_F(WsLoopback, HandshakeAndTextEcho) {
    // Drive the client on a *separate* io_context so send/recv calls
    // run on a different thread than the server loop — prevents any
    // accidental same-thread re-entrancy hiding a real bug.
    asio::io_context client_ioc;

    std::string host = "127.0.0.1";
    std::string port = std::to_string(port_);

    // NOTE: pass the lambda UNINVOKED (no trailing `()`) so asio's
    // co_spawn overload that takes `F` moves the lambda into its own
    // storage. If we called the lambda inline (`[...](){...}()`), the
    // lambda temporary would die at the end of this full-expression
    // while the resulting coroutine still holds pointers back into
    // its `[&]` capture members — ASan catches this as a
    // stack-use-after-scope. Release/Debug builds happen to get away
    // with it because the stack slot isn't reused before the
    // coroutine resumes, but it's UB either way.
    auto fut = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<std::string> {
            auto ws = co_await ws_connect(
                client_ioc.get_executor(), host, port, "/",
                {}, /*tls=*/false);
            co_await ws->send_text("ping from client");
            auto msg = co_await ws->recv();
            co_await ws->send_close(1000, "bye");
            // Drain the server's close echo so the server coroutine
            // exits cleanly.
            auto echo = co_await ws->recv();
            EXPECT_EQ(echo.op, WsOpcode::Close);
            co_return std::move(msg.payload);
        },
        asio::use_future);

    client_ioc.run();
    auto echoed = fut.get();
    EXPECT_EQ(echoed, "ping from client");
}

TEST_F(WsLoopback, LargeBinaryEcho) {
    // 70 KB crosses the 16-bit extended-length boundary (65535) and
    // forces the 8-byte length path.
    std::string big(70 * 1024, '\x2A');
    asio::io_context client_ioc;
    std::string host = "127.0.0.1";
    std::string port = std::to_string(port_);

    // Same uninvoked-lambda pattern as HandshakeAndTextEcho; see
    // its comment for why.
    auto fut = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<std::string> {
            auto ws = co_await ws_connect(
                client_ioc.get_executor(), host, port, "/",
                {}, /*tls=*/false);
            co_await ws->send_binary(big);
            auto msg = co_await ws->recv();
            co_await ws->send_close();
            auto echo = co_await ws->recv();
            EXPECT_EQ(echo.op, WsOpcode::Close);
            co_return std::move(msg.payload);
        },
        asio::use_future);

    client_ioc.run();
    auto echoed = fut.get();
    EXPECT_EQ(echoed.size(), big.size());
    EXPECT_EQ(echoed, big);
}
