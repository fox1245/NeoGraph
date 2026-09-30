// WebSocket (OpenAI Responses) failure classification over a real loopback
// socket. A server that hangs up before `response.completed` must surface as a
// typed error whose `retryable()` follows RFC 6455 close semantics, and a peer
// that vanishes without a Close frame is the same "stream cut", not an
// untyped socket exception.
//
// The scripted server speaks just enough RFC 6455 for one connection: it
// completes the handshake, waits for the client's first frame
// (`response.create`), replays a list of text events, then ends the
// conversation the way the test asks.

#include <gtest/gtest.h>

#include <neograph/async/run_sync.h>
#include <neograph/llm/schema_provider.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;

namespace {

struct Script {
    std::vector<std::string> events;  // text frames sent after response.create
    enum class End { Close, Drop } end = End::Close;
    std::uint16_t close_code = 1000;
    std::string close_reason;
};

// Runs one scripted connection on a background thread; destroyed = stopped.
class ScriptedWsServer {
public:
    explicit ScriptedWsServer(Script script)
        : script_(std::move(script)),
          acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
          socket_(io_),
          watchdog_(io_, std::chrono::seconds(5)) {
        port_ = acceptor_.local_endpoint().port();
        watchdog_.async_wait([this](const asio::error_code& error) {
            if (!error) {
                asio::error_code ignored;
                acceptor_.close(ignored);
                socket_.close(ignored);
            }
        });
        asio::co_spawn(io_, serve(), asio::detached);
        worker_ = std::thread([this] { io_.run(); });
    }
    ~ScriptedWsServer() {
        io_.stop();
        worker_.join();
    }
    int port() const { return port_; }

private:
    asio::awaitable<void> serve() {
        try {
            co_await acceptor_.async_accept(socket_, asio::use_awaitable);
            std::string buffer;
            char bytes[4096];
            while (buffer.find("\r\n\r\n") == std::string::npos) {
                const auto count =
                    co_await socket_.async_read_some(asio::buffer(bytes), asio::use_awaitable);
                buffer.append(bytes, count);
            }
            const auto key_start = buffer.find("Sec-WebSocket-Key: ") + 19;
            const auto key_end = buffer.find("\r\n", key_start);
            const auto accept = async::detail::compute_sec_websocket_accept(
                buffer.substr(key_start, key_end - key_start));
            const std::string handshake =
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
            co_await asio::async_write(socket_, asio::buffer(handshake), asio::use_awaitable);
            buffer.erase(0, buffer.find("\r\n\r\n") + 4);
            // Wait for the client's response.create frame.
            for (;;) {
                auto frame = async::detail::parse_frame_header(buffer);
                if (frame && buffer.size() >= frame->header_size + frame->payload_len) break;
                const auto count =
                    co_await socket_.async_read_some(asio::buffer(bytes), asio::use_awaitable);
                buffer.append(bytes, count);
            }
            std::string output;
            for (const auto& event : script_.events) {
                async::detail::encode_frame_header(output, async::WsOpcode::Text, true, false,
                                                   event.size());
                output += event;
            }
            if (script_.end == Script::End::Close) {
                // close_code 0 = a Close frame with an empty payload (no status).
                std::string payload;
                if (script_.close_code != 0) {
                    payload.push_back(static_cast<char>(script_.close_code >> 8));
                    payload.push_back(static_cast<char>(script_.close_code & 0xFF));
                    payload += script_.close_reason;
                }
                async::detail::encode_frame_header(output, async::WsOpcode::Close, true, false,
                                                   payload.size());
                output += payload;
            }
            co_await asio::async_write(socket_, asio::buffer(output), asio::use_awaitable);
            if (script_.end == Script::End::Drop) {
                asio::error_code ignored;
                socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                socket_.close(ignored);
            } else {
                // Let the client read the Close frame before the socket goes away.
                asio::steady_timer linger(io_, std::chrono::milliseconds(300));
                co_await linger.async_wait(asio::use_awaitable);
            }
        } catch (const std::exception&) {
            // The watchdog bounds a stalled test.
        }
        watchdog_.cancel();
    }

    Script script_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer watchdog_;
    std::thread worker_;
    int port_ = 0;
};

const char* kPartial =
    R"({"type":"response.output_item.added","item":{"type":"message","id":"m1"}})";
const char* kDelta = R"({"type":"response.output_text.delta","delta":"partial "})";

// Runs one streamed call against the script; returns what it threw.
struct Outcome {
    bool threw = false;
    bool rate_limited = false;
    int status = -1;
    bool retryable = false;
    std::string code;
    std::string what;
    std::string streamed;
};

Outcome run_against(Script script) {
    ScriptedWsServer server(std::move(script));
    llm::SchemaProvider::Config config;
    config.schema_path = "openai_responses";
    config.api_key = "test-key";
    config.base_url_override = "http://127.0.0.1:" + std::to_string(server.port());
    config.allow_insecure_loopback = true;
    config.use_websocket = true;
    config.timeout_seconds = 3;
    auto provider = llm::SchemaProvider::create(config);
    CompletionParams params;
    params.messages.push_back({"user", "hi"});
    Outcome outcome;
    try {
        async::run_sync(provider->invoke(
            params, [&](const std::string& chunk) { outcome.streamed += chunk; }));
    } catch (const RateLimitError& error) {
        outcome = {true, true, error.status(), error.retryable(), error.code(), error.what(),
                   outcome.streamed};
    } catch (const ProviderError& error) {
        outcome = {true, false, error.status(), error.retryable(), error.code(), error.what(),
                   outcome.streamed};
    } catch (const std::exception& error) {
        outcome.threw = true;
        outcome.what = std::string("UNTYPED: ") + error.what();
    }
    return outcome;
}

}  // namespace

TEST(SchemaProviderWsFailures, CloseCodesClassifyByRfc6455) {
    struct Case {
        std::uint16_t code;
        bool transient;
    };
    for (const Case c : {Case{1001, true}, Case{1011, true}, Case{1012, true}, Case{1013, true},
                         Case{1014, true}, Case{1000, false}, Case{1002, false},
                         Case{1003, false}, Case{1007, false}, Case{1008, false},
                         Case{1009, false}, Case{1015, false}, Case{4000, false}}) {
        Script script;
        script.events = {kPartial, kDelta};
        script.close_code = c.code;
        script.close_reason = "test";
        const auto outcome = run_against(script);
        ASSERT_TRUE(outcome.threw) << c.code;
        EXPECT_EQ(outcome.code, "stream_truncated") << c.code << ": " << outcome.what;
        EXPECT_EQ(outcome.retryable, c.transient) << c.code;
        EXPECT_NE(outcome.what.find("close=" + std::to_string(c.code)), std::string::npos)
            << outcome.what;
        EXPECT_EQ(outcome.streamed, "partial ") << c.code;  // delivered before the failure
    }
}

TEST(SchemaProviderWsFailures, CloseWithoutStatusIsTransient) {
    // A Close frame carrying no status (empty payload): nothing says the server
    // rejected the request, so it is treated like a dropped connection.
    Script script;
    script.events = {kPartial, kDelta};
    script.close_code = 0;
    const auto outcome = run_against(script);
    ASSERT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.code, "stream_truncated") << outcome.what;
    EXPECT_TRUE(outcome.retryable) << outcome.what;
}

TEST(SchemaProviderWsFailures, PeerVanishingWithoutCloseFrameIsATypedRetryableCut) {
    Script script;
    script.events = {kPartial, kDelta};
    script.end = Script::End::Drop;
    const auto outcome = run_against(script);
    ASSERT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.what.rfind("UNTYPED", 0), std::string::npos) << outcome.what;
    EXPECT_EQ(outcome.code, "stream_truncated") << outcome.what;
    EXPECT_TRUE(outcome.retryable);
    EXPECT_NE(outcome.what.find("connection lost"), std::string::npos) << outcome.what;
    EXPECT_EQ(outcome.streamed, "partial ");
}

TEST(SchemaProviderWsFailures, ResponseFailedEventIsATypedErrorOverTheWire) {
    Script script;
    script.events = {
        kPartial, kDelta,
        R"({"type":"response.failed","response":{"status":"failed","error":{"code":"server_error","message":"The model failed"}}})"};
    const auto outcome = run_against(script);
    ASSERT_TRUE(outcome.threw);
    EXPECT_EQ(outcome.code, "server_error") << outcome.what;
    EXPECT_TRUE(outcome.retryable);
    EXPECT_EQ(outcome.status, 200);
}

TEST(SchemaProviderWsFailures, RateLimitEventIsATypedRateLimitOverTheWire) {
    Script script;
    script.events = {
        R"({"type":"error","code":"rate_limit_exceeded","message":"Slow down"})"};
    const auto outcome = run_against(script);
    ASSERT_TRUE(outcome.threw);
    EXPECT_TRUE(outcome.rate_limited) << outcome.what;
    EXPECT_EQ(outcome.status, 429);
}

TEST(SchemaProviderWsFailures, CompletedResponseStillSucceeds) {
    Script script;
    script.events = {
        kPartial, kDelta,
        R"({"type":"response.completed","response":{"status":"completed","usage":{"input_tokens":3,"output_tokens":4,"total_tokens":7}}})"};
    script.end = Script::End::Close;
    script.close_code = 1000;
    const auto outcome = run_against(script);
    EXPECT_FALSE(outcome.threw) << outcome.what;
    EXPECT_EQ(outcome.streamed, "partial ");
}
