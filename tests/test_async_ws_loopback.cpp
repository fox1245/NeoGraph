// Loopback integration test for the WebSocket client. Spawns a small
// RFC 6455 echo server on 127.0.0.1:<ephemeral>, then drives
// ws_connect + send_text + recv + send_close through it.
//
// Plain ws:// (no TLS) to keep the test self-contained — the TLS
// branch is exercised by the OpenAI integration test in a later
// phase, and lives off the ctest default path.

#include <neograph/async/ws_client.h>
#include <neograph/graph/cancel.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/system_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using neograph::async::ws_connect;
using neograph::async::WsClient;
using neograph::async::WsClientOptions;
using neograph::async::WsMessage;
using neograph::async::WsOpcode;

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
        auto n = co_await sock.async_read_some(asio::buffer(scratch), asio::use_awaitable);
        if (n == 0) co_return;
        buf.append(scratch.data(), n);
        hdr_end = buf.find("\r\n\r\n");
    }
    auto       head                 = buf.substr(0, hdr_end);
    const bool oversized_handshake  = head.rfind("GET /oversized-handshake HTTP/1.1\r\n", 0) == 0;
    const bool oversized_data       = head.rfind("GET /oversized-data HTTP/1.1\r\n", 0) == 0;
    const bool oversized_control    = head.rfind("GET /oversized-control HTTP/1.1\r\n", 0) == 0;
    const bool fragmented_aggregate = head.rfind("GET /fragmented-aggregate HTTP/1.1\r\n", 0) == 0;
    if (head.rfind("GET / HTTP/1.1\r\n", 0) != 0 && head.rfind("GET /owned HTTP/1.1\r\n", 0) != 0 &&
        head.rfind("GET /hold HTTP/1.1\r\n", 0) != 0 && !oversized_handshake && !oversized_data &&
        !oversized_control && !fragmented_aggregate) {
        const std::string bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        co_await          asio::async_write(sock, asio::buffer(bad), asio::use_awaitable);
        co_return;
    }
    if (oversized_handshake) {
        const std::string oversized =
            "HTTP/1.1 101 Switching Protocols\r\nX-Padding: " + std::string(1024, 'x');
        co_await asio::async_write(sock, asio::buffer(oversized), asio::use_awaitable);
        co_return;
    }
    std::string key;
    {
        auto pos = head.find("Sec-WebSocket-Key:");
        if (pos == std::string::npos) {
            pos = head.find("sec-websocket-key:");
        }
        if (pos == std::string::npos) co_return;
        pos = head.find(':', pos) + 1;
        while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t'))
            ++pos;
        auto nl = head.find("\r\n", pos);
        key     = head.substr(pos, nl - pos);
    }
    std::string accept = detail_ws::compute_sec_websocket_accept(key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    co_await asio::async_write(sock, asio::buffer(resp), asio::use_awaitable);
    buf.erase(0, hdr_end + 4);

    if (oversized_data) {
        std::string header;
        detail_ws::encode_frame_header(header, WsOpcode::Binary, true, false, 1024);
        co_await asio::async_write(sock, asio::buffer(header), asio::use_awaitable);
        co_return;
    }
    if (oversized_control) {
        std::string header;
        header.push_back(static_cast<char>(0x89));  // FIN=1, Ping
        header.push_back(static_cast<char>(126));
        co_await asio::async_write(sock, asio::buffer(header), asio::use_awaitable);
        co_return;
    }
    if (fragmented_aggregate) {
        std::string frames;
        detail_ws::encode_frame_header(frames, WsOpcode::Text, false, false, 6);
        frames.append("123456");
        detail_ws::encode_frame_header(frames, WsOpcode::Continuation, true, false, 6);
        co_await asio::async_write(sock, asio::buffer(frames), asio::use_awaitable);
        co_return;
    }

    // ── Echo loop ──
    for (;;) {
        std::optional<detail_ws::WsFrameHeader> h;
        while (!(h = detail_ws::parse_frame_header(buf))) {
            std::array<char, 4096> scratch{};
            auto n = co_await sock.async_read_some(asio::buffer(scratch), asio::use_awaitable);
            if (n == 0) co_return;
            buf.append(scratch.data(), n);
        }
        std::size_t needed = h->header_size + h->payload_len;
        while (buf.size() < needed) {
            std::array<char, 4096> scratch{};
            auto n = co_await sock.async_read_some(asio::buffer(scratch), asio::use_awaitable);
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
        detail_ws::encode_frame_header(out, h->opcode, h->fin, /*masked=*/false, payload.size());
        out.append(payload);
        co_await asio::async_write(sock, asio::buffer(out), asio::use_awaitable);

        if (h->opcode == WsOpcode::Close) {
            co_return;
        }
    }
}

asio::awaitable<std::uint16_t> listen_and_serve(std::shared_ptr<asio::ip::tcp::acceptor> acceptor) {
    auto sock = co_await acceptor->async_accept(asio::use_awaitable);
    co_await             serve_one(std::move(sock));
    co_return            std::uint16_t{0};
}

std::string get_runtime_error(std::future<void> result) {
    try {
        result.get();
        ADD_FAILURE() << "Expected std::runtime_error";
    } catch (const std::runtime_error& error) {
        return error.what();
    } catch (const std::exception& error) {
        ADD_FAILURE() << "Unexpected exception: " << error.what();
    }
    return {};
}

bool set_ws_debug_environment(const char* value) {
#ifdef _WIN32
    return ::_putenv_s("NEOGRAPH_WS_DEBUG", value ? value : "") == 0;
#else
    return value ? ::setenv("NEOGRAPH_WS_DEBUG", value, 1) == 0
                 : ::unsetenv("NEOGRAPH_WS_DEBUG") == 0;
#endif
}

}  // namespace

class WsLoopback : public ::testing::Test {
protected:
    void SetUp() override {
        ioc_      = std::make_unique<asio::io_context>();
        acceptor_ = std::make_shared<asio::ip::tcp::acceptor>(
            *ioc_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        port_ = acceptor_->local_endpoint().port();

        // Kick off the accept-and-serve coroutine on the io_context.
        asio::co_spawn(*ioc_, listen_and_serve(acceptor_), asio::detached);

        thread_ = std::thread([this] { ioc_->run(); });
    }

    void TearDown() override {
        ioc_->stop();
        if (thread_.joinable()) thread_.join();
    }

    std::unique_ptr<asio::io_context>        ioc_;
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::uint16_t                            port_ = 0;
    std::thread                              thread_;
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
            auto         ws =
                co_await ws_connect(client_ioc.get_executor(), host, port, "/", {}, /*tls=*/false);
            co_await     ws->send_text("ping from client");
            auto msg = co_await ws->recv();
            co_await            ws->send_close(1000, "bye");
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
    std::string      big(70 * 1024, '\x2A');
    asio::io_context client_ioc;
    std::string      host = "127.0.0.1";
    std::string      port = std::to_string(port_);

    // Same uninvoked-lambda pattern as HandshakeAndTextEcho; see
    // its comment for why.
    auto fut = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<std::string> {
            auto         ws =
                co_await ws_connect(client_ioc.get_executor(), host, port, "/", {}, /*tls=*/false);
            co_await     ws->send_binary(big);
            auto msg = co_await ws->recv();
            co_await            ws->send_close();
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

TEST_F(WsLoopback, RejectsHeaderOnlyFrameOverConfiguredLimit) {
    asio::io_context  client_ioc;
    const std::string port = std::to_string(port_);
    WsClientOptions   options;
    options.max_frame_payload_bytes = 8;

    auto result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            auto ws = co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port,
                                          "/oversized-data", {}, false, options);
            (void)co_await     ws->recv();
        },
        asio::use_future);

    client_ioc.run();
    EXPECT_NE(get_runtime_error(std::move(result)).find("inbound frame exceeds configured limit"),
              std::string::npos);
}

TEST_F(WsLoopback, RejectsHeaderOnlyOversizedControlFrame) {
    asio::io_context  client_ioc;
    const std::string port = std::to_string(port_);

    auto result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            auto ws = co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port,
                                          "/oversized-control", {}, false);
            (void)co_await     ws->recv();
        },
        asio::use_future);

    client_ioc.run();
    EXPECT_NE(get_runtime_error(std::move(result)).find("control frame length marker exceeds 125"),
              std::string::npos);
}

TEST_F(WsLoopback, RejectsFragmentedMessageBeforeOversizedPayloadRead) {
    asio::io_context  client_ioc;
    const std::string port = std::to_string(port_);
    WsClientOptions   options;
    options.max_frame_payload_bytes   = 8;
    options.max_message_payload_bytes = 10;

    auto result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            auto ws = co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port,
                                          "/fragmented-aggregate", {}, false, options);
            (void)co_await     ws->recv();
        },
        asio::use_future);

    client_ioc.run();
    EXPECT_NE(get_runtime_error(std::move(result)).find("inbound message exceeds configured limit"),
              std::string::npos);
}

TEST_F(WsLoopback, BoundsIncompleteUpgradeResponse) {
    asio::io_context  client_ioc;
    const std::string port = std::to_string(port_);
    WsClientOptions   options;
    options.max_handshake_bytes = 256;

    auto result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            (void)co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port,
                                      "/oversized-handshake", {}, false, options);
        },
        asio::use_future);

    client_ioc.run();
    EXPECT_NE(get_runtime_error(std::move(result))
                  .find("upgrade response exceeds configured handshake limit"),
              std::string::npos);
}

TEST_F(WsLoopback, EnforcesOutboundMessageFrameAndCloseLimits) {
    asio::io_context  client_ioc;
    const std::string port = std::to_string(port_);
    WsClientOptions   options;
    options.max_frame_payload_bytes   = 8;
    options.max_message_payload_bytes = 8;

    auto result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<std::vector<std::string>> {
            auto ws = co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port, "/", {},
                                           false, options);
            std::vector<std::string> errors;
            try {
                co_await ws->send_binary("123456789");
            } catch (const std::runtime_error& error) {
                errors.emplace_back(error.what());
            }
            try {
                co_await ws->send_close(1000, std::string(124, 'x'));
            } catch (const std::runtime_error& error) {
                errors.emplace_back(error.what());
            }
            co_await       ws->send_close();
            (void)co_await ws->recv();
            co_return      errors;
        },
        asio::use_future);

    client_ioc.run();
    const auto errors = result.get();
    ASSERT_EQ(errors.size(), 2u);
    EXPECT_NE(errors[0].find("outbound frame exceeds configured limit"), std::string::npos);
    EXPECT_NE(errors[1].find("close reason exceeds 123 bytes"), std::string::npos);
}

TEST_F(WsLoopback, DebugLoggingDoesNotExposeUpgradeRequestOrHeaders) {
    const char*                      old_debug = std::getenv("NEOGRAPH_WS_DEBUG");
    const std::optional<std::string> saved_debug =
        old_debug ? std::optional<std::string>(old_debug) : std::nullopt;
    ASSERT_TRUE(set_ws_debug_environment("1"));
    testing::internal::CaptureStderr();

    asio::io_context  client_ioc;
    const std::string port   = std::to_string(port_);
    auto              result = asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            auto ws = co_await ws_connect(client_ioc.get_executor(), "127.0.0.1", port, "/",
                                                       {{"Authorization", "Bearer super-secret"}}, false);
            co_await           ws->send_close();
            (void)co_await     ws->recv();
        },
        asio::use_future);
    client_ioc.run();
    result.get();

    const std::string debug = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(set_ws_debug_environment(saved_debug ? saved_debug->c_str() : nullptr));
    EXPECT_NE(debug.find("upgrade request bytes="), std::string::npos);
    EXPECT_EQ(debug.find("super-secret"), std::string::npos);
    EXPECT_EQ(debug.find("Authorization"), std::string::npos);
    EXPECT_EQ(debug.find("GET /"), std::string::npos);
}

TEST_F(WsLoopback, CancellationAbortsHeldReceive) {
    asio::io_context               client_ioc;
    const std::string              port  = std::to_string(port_);
    auto                           token = std::make_shared<neograph::graph::CancelToken>();
    std::promise<void>             receive_entered;
    auto                           entered = receive_entered.get_future();
    std::promise<asio::error_code> completion;
    auto                           result = completion.get_future();

    asio::co_spawn(
        client_ioc,
        [&]() -> asio::awaitable<void> {
            try {
                auto ex                               = co_await asio::this_coro::executor;
                auto               operation          = token->fork();
                const auto         operation_executor = operation->bind_executor(ex);
                co_await           asio::post(operation_executor, asio::use_awaitable);
                operation->throw_if_cancelled("WebSocket receive test entry");
                auto ws = co_await ws_connect(operation_executor, "127.0.0.1", port, "/hold", {},
                                              /*tls=*/false);
                auto               wait_for_message = [&]() -> asio::awaitable<WsMessage> {
                    receive_entered.set_value();
                    co_return co_await ws->recv();
                };
                (void)co_await asio::co_spawn(
                    operation_executor, wait_for_message(),
                    asio::bind_cancellation_slot(operation->slot(), asio::use_awaitable));
                completion.set_value(asio::error::fault);
            } catch (const asio::system_error& error) {
                completion.set_value(error.code());
            } catch (...) {
                completion.set_value(asio::error::fault);
            }
        },
        asio::detached);
    std::thread runner([&] { client_ioc.run(); });

    if (entered.wait_for(1s) != std::future_status::ready) {
        token->cancel();
        client_ioc.stop();
        runner.join();
        ADD_FAILURE() << "WebSocket receive did not begin";
        return;
    }

    token->cancel();
    if (result.wait_for(1s) != std::future_status::ready) {
        client_ioc.stop();
        runner.join();
        ADD_FAILURE() << "WebSocket cancellation did not abort recv";
        return;
    }
    EXPECT_EQ(result.get(), asio::error::operation_aborted);
    runner.join();
}

TEST_F(WsLoopback, DelayedOperationsOwnEndpointAndPayloads) {
    asio::io_context client_ioc;
    std::string      host = "127.0.0.1";
    std::string      port = std::to_string(port_);
    std::string      path = "/owned";

    auto connect_operation =
        ws_connect(client_ioc.get_executor(), host, port, path, {}, /*tls=*/false);
    path = "/change";
    auto connect_future =
        asio::co_spawn(client_ioc, std::move(connect_operation), asio::use_future);
    client_ioc.run();
    auto ws = connect_future.get();

    client_ioc.restart();
    std::string text           = "owned-text";
    auto        text_operation = ws->send_text(text);
    text                       = "mutated-text";
    auto text_future = asio::co_spawn(client_ioc, std::move(text_operation), asio::use_future);
    client_ioc.run();
    text_future.get();

    client_ioc.restart();
    auto text_recv_future = asio::co_spawn(client_ioc, ws->recv(), asio::use_future);
    client_ioc.run();
    EXPECT_EQ(text_recv_future.get().payload, "owned-text");

    client_ioc.restart();
    std::string binary           = "owned-binary";
    auto        binary_operation = ws->send_binary(binary);
    binary                       = "mutated-binary";
    auto binary_future = asio::co_spawn(client_ioc, std::move(binary_operation), asio::use_future);
    client_ioc.run();
    binary_future.get();

    client_ioc.restart();
    auto binary_recv_future = asio::co_spawn(client_ioc, ws->recv(), asio::use_future);
    client_ioc.run();
    EXPECT_EQ(binary_recv_future.get().payload, "owned-binary");

    client_ioc.restart();
    std::string reason          = "owned-close";
    auto        close_operation = ws->send_close(1000, reason);
    reason                      = "mutated-close";
    auto close_future = asio::co_spawn(client_ioc, std::move(close_operation), asio::use_future);
    client_ioc.run();
    close_future.get();

    client_ioc.restart();
    auto close_recv_future = asio::co_spawn(client_ioc, ws->recv(), asio::use_future);
    client_ioc.run();
    auto close = close_recv_future.get();
    ASSERT_EQ(close.op, WsOpcode::Close);
    ASSERT_GE(close.payload.size(), 2u);
    EXPECT_EQ(close.payload.substr(2), "owned-close");
}
