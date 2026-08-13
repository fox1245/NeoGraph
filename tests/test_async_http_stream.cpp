// Integration tests for neograph::async::async_post_stream +
// SseEventParser (Stage 3 Semester 1.4).
//
// Plain TCP only (matches test_async_conn_pool's scope). An
// in-process asio server emits a fixed chunked response and the
// test asserts on_chunk delivery ordering, then pipes chunks into
// SseEventParser to reconstruct SSE events.

#include <neograph/async/http_client.h>
#include <neograph/async/sse_parser.h>

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/redirect_error.hpp>
#include <asio/streambuf.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <istream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Chunked-response mock. For each inbound request, writes response
// headers + the given vector of chunks + the terminator 0\r\n\r\n.
// Each chunk is framed as "<hex len>\r\n<body>\r\n".
struct ChunkedMockServer {
    asio::io_context         io;
    asio::ip::tcp::acceptor  acceptor{io};
    std::thread              worker;
    std::vector<std::string> chunks;
    std::string              expected_path;
    unsigned short           port = 0;

    ChunkedMockServer(std::vector<std::string> chs, std::string expected = {})
        : chunks(std::move(chs)), expected_path(std::move(expected)) {
        acceptor.open(asio::ip::tcp::v4());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
        acceptor.listen();
        port = acceptor.local_endpoint().port();
        asio::co_spawn(io, accept_loop(), asio::detached);
        worker = std::thread([this] { io.run(); });
    }

    ~ChunkedMockServer() {
        asio::error_code ec;
        acceptor.close(ec);
        io.stop();
        if (worker.joinable()) worker.join();
    }

    asio::awaitable<void> handle(asio::ip::tcp::socket sock) {
        try {
            // Consume request (best-effort; we don't validate).
            asio::streambuf buf;
            co_await        asio::async_read_until(sock, buf, "\r\n\r\n", asio::use_awaitable);
            std::istream    is(&buf);
            std::string     request_line;
            std::getline(is, request_line);
            std::string request_path;
            auto        sp1 = request_line.find(' ');
            auto        sp2 = request_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                request_path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            // (Body ignored for this mock.)

            // Write response headers.
            std::string head;
            head.append("HTTP/1.1 200 OK\r\n");
            head.append("Content-Type: text/event-stream\r\n");
            head.append("Transfer-Encoding: chunked\r\n");
            head.append("Connection: close\r\n\r\n");
            co_await asio::async_write(sock, asio::buffer(head), asio::use_awaitable);

            // Emit each configured chunk.
            const std::vector<std::string> mutated_chunks = {"mutated"};
            const auto&                    response_chunks =
                expected_path.empty() || request_path == expected_path ? chunks : mutated_chunks;
            for (const auto& c : response_chunks) {
                std::string frame;
                frame.reserve(16 + c.size());
                // Hex size, \r\n, payload, \r\n
                char hex[32];
                std::snprintf(hex, sizeof(hex), "%zx", c.size());
                frame.append(hex).append("\r\n");
                frame.append(c).append("\r\n");
                co_await asio::async_write(sock, asio::buffer(frame), asio::use_awaitable);
            }
            // Terminator chunk.
            const char* term = "0\r\n\r\n";
            co_await    asio::async_write(sock, asio::buffer(term, 5), asio::use_awaitable);
        } catch (...) {}
        asio::error_code ec;
        sock.close(ec);
    }

    asio::awaitable<void> accept_loop() {
        for (;;) {
            asio::ip::tcp::socket sock{io};
            asio::error_code      ec;
            co_await acceptor.async_accept(sock, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            asio::co_spawn(io, handle(std::move(sock)), asio::detached);
        }
    }
};

TEST(AsyncPostStream, ChunksDeliveredInOrder) {
    ChunkedMockServer srv({"alpha", "beta", "gamma"});
    asio::io_context  client_io;

    std::vector<std::string> received;
    int                      status = 0;
    asio::co_spawn(
        client_io,
        [&]() -> asio::awaitable<void> {
            auto resp = co_await neograph::async::async_post_stream(
                client_io.get_executor(), "127.0.0.1", std::to_string(srv.port), "/v1/stream", "{}",
                {}, false, [&](std::string_view c) { received.emplace_back(c); });
            status = resp.status;
        },
        asio::detached);

    client_io.run();

    EXPECT_EQ(status, 200);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0], "alpha");
    EXPECT_EQ(received[1], "beta");
    EXPECT_EQ(received[2], "gamma");
}

// Non-streaming async_post must also handle Transfer-Encoding: chunked
// — modern HTTP/1.1 servers (OpenAI, fastmcp 3.x) use chunked even for
// single-shot JSON responses when the body isn't buffered up front.
TEST(AsyncPost, ChunkedResponseReassembledIntoBody) {
    ChunkedMockServer srv({
        R"({"part1":"hello",)",
        R"("part2":"world"})",
    });
    asio::io_context  client_io;

    int         status = 0;
    std::string body;
    std::string err;
    asio::co_spawn(
        client_io,
        [&]() -> asio::awaitable<void> {
            try {
                auto resp = co_await neograph::async::async_post(
                    client_io.get_executor(), "127.0.0.1", std::to_string(srv.port),
                    "/v1/chat/completions", "{}", {}, false);
                status = resp.status;
                body   = resp.body;
            } catch (const std::exception& e) {
                err = e.what();
            }
        },
        asio::detached);

    client_io.run();

    EXPECT_EQ(err, "") << "async_post threw: " << err;
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, R"({"part1":"hello","part2":"world"})");
}

TEST(AsyncPostStream, SseEventsReconstructed) {
    // Split SSE data across multiple chunks including mid-event and
    // mid-newline boundaries — the parser must handle both.
    ChunkedMockServer srv({
        "event: start\ndata: first\n",
        "\n",                                   // completes event 1
        "event: tick\ndata: a",                 // partial event 2
        "lpha\n\nevent: tick\ndata: beta\n\n",  // completes event 2+3
    });
    asio::io_context  client_io;

    neograph::async::SseEventParser parser;
    int                             status = 0;
    asio::co_spawn(
        client_io,
        [&]() -> asio::awaitable<void> {
            auto resp = co_await neograph::async::async_post_stream(
                client_io.get_executor(), "127.0.0.1", std::to_string(srv.port), "/v1/stream", "{}",
                {}, false, [&](std::string_view c) { parser.feed(c); });
            status = resp.status;
        },
        asio::detached);

    client_io.run();

    EXPECT_EQ(status, 200);
    auto events = parser.drain();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].event, "start");
    EXPECT_EQ(events[0].data, "first");
    EXPECT_EQ(events[1].event, "tick");
    EXPECT_EQ(events[1].data, "alpha");
    EXPECT_EQ(events[2].event, "tick");
    EXPECT_EQ(events[2].data, "beta");
}

TEST(AsyncPostStream, ServerErrorStatusStillDelivered) {
    // 500 response with a chunked error body should surface status
    // through HttpStreamResponse while still piping the body to on_chunk.
    struct ErrorServer {
        asio::io_context        io;
        asio::ip::tcp::acceptor acc{io};
        std::thread             worker;
        unsigned short          port = 0;

        ErrorServer() {
            acc.open(asio::ip::tcp::v4());
            acc.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acc.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
            acc.listen();
            port = acc.local_endpoint().port();
            asio::co_spawn(io, loop(), asio::detached);
            worker = std::thread([this] { io.run(); });
        }
        ~ErrorServer() {
            asio::error_code ec;
            acc.close(ec);
            io.stop();
            if (worker.joinable()) worker.join();
        }
        asio::awaitable<void> loop() {
            for (;;) {
                asio::ip::tcp::socket sock{io};
                asio::error_code      ec;
                co_await acc.async_accept(sock, asio::redirect_error(asio::use_awaitable, ec));
                if (ec) co_return;
                try {
                    asio::streambuf buf;
                    co_await    asio::async_read_until(sock, buf, "\r\n\r\n", asio::use_awaitable);
                    std::string resp =
                        "HTTP/1.1 500 Internal Server Error\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "Connection: close\r\n\r\n"
                        "9\r\noops nope\r\n"
                        "0\r\n\r\n";
                    co_await asio::async_write(sock, asio::buffer(resp), asio::use_awaitable);
                } catch (...) {}
                asio::error_code ec2;
                sock.close(ec2);
            }
        }
    } srv;

    asio::io_context client_io;
    std::string      got;
    int              status = 0;
    asio::co_spawn(
        client_io,
        [&]() -> asio::awaitable<void> {
            auto resp = co_await neograph::async::async_post_stream(
                client_io.get_executor(), "127.0.0.1", std::to_string(srv.port), "/v1/stream", "{}",
                {}, false, [&](std::string_view c) { got.append(c); });
            status = resp.status;
        },
        asio::detached);

    client_io.run();

    EXPECT_EQ(status, 500);
    EXPECT_EQ(got, "oops nope");
}

TEST(AsyncHttpOwnership, StreamSnapshotsEndpointAndCallbackBeforeFirstResume) {
    ChunkedMockServer                     srv({"owned"}, "/v1/stream");
    asio::io_context                      io;
    std::string                           host = "127.0.0.1";
    std::string                           port = std::to_string(srv.port);
    std::string                           path = "/v1/stream";
    std::string                           body = "{}";
    std::string                           received;
    std::function<void(std::string_view)> callback = [&](std::string_view chunk) {
        received.append(chunk);
    };

    auto operation = neograph::async::async_post_stream(io.get_executor(), host, port, path, body,
                                                        {}, false, callback);
    path           = "/mutated";
    callback       = nullptr;

    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();
    auto resp = future.get();

    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(received, "owned");
}

TEST(AsyncHttpOwnership, StreamCancellationOwnsCallbackUntilAwaitableCompletes) {
    struct SlowStreamServer {
        asio::io_context        io;
        asio::ip::tcp::acceptor acceptor{io};
        std::thread             worker;
        unsigned short          port = 0;

        SlowStreamServer() {
            acceptor.open(asio::ip::tcp::v4());
            acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acceptor.bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
            acceptor.listen();
            port = acceptor.local_endpoint().port();
            asio::co_spawn(io, accept_loop(), asio::detached);
            worker = std::thread([this] { io.run(); });
        }

        ~SlowStreamServer() {
            io.stop();
            if (worker.joinable()) worker.join();
            asio::error_code ec;
            acceptor.close(ec);
        }

        asio::awaitable<void> accept_loop() {
            asio::ip::tcp::socket sock{io};
            asio::error_code      ec;
            co_await acceptor.async_accept(sock, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) co_return;
            try {
                asio::streambuf buf;
                co_await        asio::async_read_until(sock, buf, "\r\n\r\n", asio::use_awaitable);
                asio::steady_timer timer(io);
                timer.expires_after(std::chrono::milliseconds(200));
                co_await          timer.async_wait(asio::use_awaitable);
                const std::string resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Connection: close\r\n\r\n"
                    "4\r\nlate\r\n"
                    "0\r\n\r\n";
                co_await asio::async_write(sock, asio::buffer(resp), asio::use_awaitable);
            } catch (...) {}
            asio::error_code ignored;
            sock.close(ignored);
        }
    } srv;

    struct CallbackState {
        std::atomic<int>* chunks;
        explicit CallbackState(std::atomic<int>* chunks) : chunks(chunks) {}
        void operator()(std::string_view) const { chunks->fetch_add(1, std::memory_order_relaxed); }
    };

    asio::io_context             io;
    std::atomic<int>             chunks{0};
    std::weak_ptr<CallbackState> weak_callback;

    asio::awaitable<neograph::async::HttpStreamResponse> operation;
    {
        auto callback_state = std::make_shared<CallbackState>(&chunks);
        weak_callback       = callback_state;
        neograph::async::RequestOptions opts;
        opts.timeout = std::chrono::milliseconds(25);
        operation    = neograph::async::async_post_stream(
            io.get_executor(), "127.0.0.1", std::to_string(srv.port), "/v1/stream", "{}", {}, false,
            [callback_state](std::string_view chunk) { (*callback_state)(chunk); }, opts);
    }

    ASSERT_FALSE(weak_callback.expired())
        << "stored awaitable must own the callback before first resume";

    auto future = asio::co_spawn(io, std::move(operation), asio::use_future);
    io.run();

    EXPECT_THROW((void)future.get(), asio::system_error);
    EXPECT_TRUE(weak_callback.expired())
        << "cancelled stream should release callback captures on completion";
    EXPECT_EQ(chunks.load(std::memory_order_relaxed), 0)
        << "timeout cancellation must not deliver late chunks";
}

}  // namespace
