// The experimental remote transports of the ACP server: Streamable HTTP and
// WebSocket on one endpoint (include/neograph/acp/http_server.h).
//
// Each transport is driven with a complete session (initialize, session/new,
// session/prompt with its streamed update, session/cancel, a permission round
// trip and an fs/* round trip), then through its guard (foreign Host and
// Origin, oversize bodies and messages, missing authentication), with two
// connections at once, and with a connection that goes away in the middle of
// a prompt. The stdio server is not touched by any of this; its own tests are
// in test_acp_server.cpp.

#include <neograph/acp/http_server.h>
#include <neograph/acp/server.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>

#include <gtest/gtest.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::acp;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

using namespace std::chrono_literals;

constexpr auto kWait = 10s;

// --- The agent ---------------------------------------------------------------

struct Probe {
    std::atomic<int>  entered{0};
    std::atomic<int>  cancelled{0};
    std::atomic<int>  completed{0};
    std::atomic<bool> release{false};
};

/// Behaves by what the prompt says: "wait" holds the turn until it is
/// released or cancelled, "ask" interrupts and resumes with the answer,
/// "read:<path>" reads the file through the client, anything else is echoed.
class ProbeNode final : public GraphNode {
  public:
    ProbeNode(std::string name, std::shared_ptr<Probe> probe, std::shared_ptr<ACPClient> client)
        : name_(std::move(name)), probe_(std::move(probe)), client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto raw  = in.state.get("prompt");
        const auto text = raw.is_string() ? raw.get<std::string>() : raw.dump();
        NodeOutput out;
        if (text == "wait") {
            probe_->entered.fetch_add(1);
            const auto deadline = std::chrono::steady_clock::now() + 20s;
            while (std::chrono::steady_clock::now() < deadline) {
                if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
                    probe_->cancelled.fetch_add(1);
                    throw neograph::graph::CancelledException();
                }
                if (probe_->release.load()) break;
                std::this_thread::sleep_for(2ms);
            }
            probe_->completed.fetch_add(1);
            out.writes.push_back(ChannelWrite{"response", json("released")});
        } else if (text == "ask") {
            if (!in.ctx.resume_value) {
                throw neograph::graph::NodeInterrupt("approval needed",
                                                     json{{"action", "deploy"}});
            }
            const auto& answer = *in.ctx.resume_value;
            out.writes.push_back(ChannelWrite{
                "response",
                json("resumed:" + (answer.is_string() ? answer.get<std::string>() : answer.dump()))});
        } else if (text.rfind("read:", 0) == 0) {
            const auto sid = in.state.get("_acp_session_id");
            const auto content =
                client_->read_text_file(sid.is_string() ? sid.get<std::string>() : "", text.substr(5));
            out.writes.push_back(ChannelWrite{"response", json("read:" + content)});
        } else {
            out.writes.push_back(ChannelWrite{"response", json("echo:" + text)});
        }
        co_return out;
    }
    std::string get_name() const override { return name_; }

  private:
    std::string                name_;
    std::shared_ptr<Probe>     probe_;
    std::shared_ptr<ACPClient> client_;
};

std::shared_ptr<GraphEngine> build_engine(const std::shared_ptr<Probe>&     probe,
                                          const std::shared_ptr<ACPClient>& client) {
    NodeFactory::instance().register_type(
        "acp_http_probe",
        [probe, client](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ProbeNode>(name, probe, client);
        });
    const json def = {
        {"name", "acp-http"},
        {"channels",
         {{"prompt", {{"reducer", "overwrite"}}},
          {"response", {{"reducer", "overwrite"}}},
          {"_acp_session_id", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"probe", {{"type", "acp_http_probe"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "probe"}},
                               json{{"from", "probe"}, {"to", "__end__"}}})},
    };
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}, store));
}

ACPHttpConfig test_config() {
    ACPHttpConfig config;
    config.keepalive_interval = 100ms;
    config.http_threads       = 4;
    config.max_http_threads   = 32;
    return config;
}

/// A server on loopback and a free port, stopped when it goes out of scope.
class Fixture {
  public:
    explicit Fixture(ACPHttpConfig config = test_config(),
                     const std::function<void(ACPServer&)>& tune = {})
        : probe(std::make_shared<Probe>()), client(std::make_shared<ACPClient>()) {
        engine = build_engine(probe, client);
        server = std::make_unique<ACPHttpServer>(
            [engine = engine, tune](std::string_view) {
                ACPHttpConnection connection;
                connection.server = std::make_unique<ACPServer>(
                    engine, json{{"name", "acp-http-test"}, {"version", "0.0.1"}});
                if (tune) tune(*connection.server);
                return connection;
            },
            std::move(config));
        server->attach_client(client);
        if (!server->start_async()) throw std::runtime_error("the server did not start");
    }
    ~Fixture() { server->stop(); }

    int port() const { return server->port(); }

    /// Wait until the server holds `count` connections.
    bool wait_connections(std::size_t count, std::chrono::milliseconds limit = kWait) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (server->connection_count() == count) return true;
            std::this_thread::sleep_for(5ms);
        }
        return server->connection_count() == count;
    }

    bool wait_for(const std::function<bool()>& condition,
                  std::chrono::milliseconds limit = kWait) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    std::shared_ptr<Probe>      probe;
    std::shared_ptr<ACPClient>  client;
    std::shared_ptr<GraphEngine> engine;
    std::unique_ptr<ACPHttpServer> server;
};

// --- JSON-RPC messages ---------------------------------------------------------

json request(int id, const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json notification(const std::string& method, json params) {
    return json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

json initialize_request(int id = 1) {
    return request(id, "initialize",
                   {{"protocolVersion", 1},
                    {"clientCapabilities",
                     {{"fs", {{"readTextFile", true}, {"writeTextFile", false}}}}}});
}

json new_session_request(int id) {
    return request(id, "session/new", {{"cwd", "/work"}, {"mcpServers", json::array()}});
}

json prompt_request(int id, const std::string& session, const std::string& text) {
    return request(id, "session/prompt",
                   {{"sessionId", session},
                    {"prompt", json::array({json{{"type", "text"}, {"text", text}}})}});
}

json cancel_notification(const std::string& session) {
    return notification("session/cancel", {{"sessionId", session}});
}

json response_to(const json& agent_request, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", agent_request["id"]}, {"result", std::move(result)}};
}

json selected(const std::string& option) {
    return json{{"outcome", {{"outcome", "selected"}, {"optionId", option}}}};
}

bool is_response_to(const json& message, int id) {
    return !message.contains("method") && message.contains("id") &&
           message["id"].is_number_integer() && message["id"].get<int>() == id;
}

bool is_method(const json& message, const std::string& method) {
    return message.contains("method") && message["method"].is_string() &&
           message["method"].get<std::string>() == method;
}

bool has_agent_text(const json& message, const std::string& text) {
    if (!is_method(message, "session/update")) return false;
    const auto update = message["params"]["update"];
    return update.value("sessionUpdate", std::string()) == "agent_message_chunk" &&
           update["content"].value("text", std::string()) == text;
}

// --- Streamable HTTP client --------------------------------------------------------

struct Reply {
    int                      status = -1;  ///< -1: no response
    std::string              body;
    std::string              content_type;
    std::string              connection_id;
    json                     json_body() const { return json::parse(body); }
};

Reply to_reply(const httplib::Result& result) {
    if (!result) return {};
    return {result->status, result->body, result->get_header_value("Content-Type"),
            result->get_header_value("Acp-Connection-Id")};
}

Reply post_json(int port, const json& message, httplib::Headers headers = {}) {
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(30, 0);
    return to_reply(client.Post("/acp", headers, message.dump(), "application/json"));
}

Reply delete_connection(int port, const std::string& connection, httplib::Headers headers = {}) {
    httplib::Client client("127.0.0.1", port);
    headers.emplace("Acp-Connection-Id", connection);
    return to_reply(client.Delete("/acp", headers));
}

/// A server-sent-event stream read on its own thread. Every `data:` event is
/// kept in arrival order.
class SseStream {
  public:
    SseStream(int port, httplib::Headers headers) {
        thread_ = std::thread([this, port, headers = std::move(headers)]() mutable {
            httplib::Client client("127.0.0.1", port);
            client.set_read_timeout(60, 0);
            headers.emplace("Accept", "text/event-stream");
            client.Get(
                "/acp", headers,
                [this](const httplib::Response& response) {
                    {
                        std::lock_guard lock(mutex_);
                        status_ = response.status;
                    }
                    changed_.notify_all();
                    return response.status == 200;
                },
                [this](const char* data, std::size_t size) {
                    on_data(data, size);
                    return !stop_.load();
                });
            {
                std::lock_guard lock(mutex_);
                ended_ = true;
            }
            changed_.notify_all();
        });
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, kWait, [this] { return status_ != 0 || ended_; });
    }
    ~SseStream() { close(); }

    SseStream(const SseStream&)            = delete;
    SseStream& operator=(const SseStream&) = delete;

    /// The status of the stream's response; 0 when none arrived.
    int status() {
        std::lock_guard lock(mutex_);
        return status_;
    }

    /// The first message so far (or one that arrives within `limit`) that
    /// `match` accepts.
    std::optional<json> wait_for(const std::function<bool(const json&)>& match,
                                 std::chrono::milliseconds limit = kWait) {
        std::unique_lock lock(mutex_);
        std::optional<json> found;
        changed_.wait_for(lock, limit, [&] {
            for (const auto& message : messages_) {
                if (match(message)) {
                    found = message;
                    return true;
                }
            }
            return ended_;
        });
        return found;
    }

    std::optional<json> response(int id) {
        return wait_for([id](const json& message) { return is_response_to(message, id); });
    }
    std::optional<json> agent_request(const std::string& method) {
        return wait_for([&](const json& message) {
            return is_method(message, method) && message.contains("id");
        });
    }

    std::vector<json> messages() {
        std::lock_guard lock(mutex_);
        return messages_;
    }

    bool wait_ended(std::chrono::milliseconds limit = kWait) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, limit, [this] { return ended_; });
    }

    void close() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

  private:
    void on_data(const char* data, std::size_t size) {
        std::lock_guard lock(mutex_);
        buffer_.append(data, size);
        for (;;) {
            const auto end = buffer_.find("\n\n");
            if (end == std::string::npos) break;
            const auto frame = buffer_.substr(0, end);
            buffer_.erase(0, end + 2);
            if (frame.rfind("data: ", 0) == 0) {
                messages_.push_back(json::parse(frame.substr(6)));
            } else if (frame.rfind(":", 0) == 0) {
                comments_.push_back(frame);
            }
        }
        changed_.notify_all();
    }

    std::mutex              mutex_;
    std::condition_variable changed_;
    std::string             buffer_;
    std::vector<json>       messages_;
    std::vector<std::string> comments_;
    int                     status_ = 0;
    bool                    ended_  = false;
    std::atomic<bool>       stop_{false};
    std::thread             thread_;
};

/// One Streamable HTTP connection: initialize, then requests whose responses
/// are read from the streams.
class HttpAgent {
  public:
    explicit HttpAgent(int port, httplib::Headers base = {})
        : port_(port), base_(std::move(base)) {}
    ~HttpAgent() {
        // Streams first: they end when the connection does.
        if (!connection_.empty()) delete_connection(port_, connection_, base_);
    }

    Reply initialize(const json& message = initialize_request()) {
        auto reply = post_json(port_, message, base_);
        if (reply.status == 200) connection_ = reply.connection_id;
        return reply;
    }

    const std::string& connection() const { return connection_; }

    httplib::Headers headers(const std::string& session = {}) const {
        auto headers = base_;
        headers.emplace("Acp-Connection-Id", connection_);
        if (!session.empty()) headers.emplace("Acp-Session-Id", session);
        return headers;
    }

    Reply post(const json& message, const std::string& session = {}) {
        return post_json(port_, message, headers(session));
    }

    std::unique_ptr<SseStream> stream(const std::string& session = {}) {
        return std::make_unique<SseStream>(port_, headers(session));
    }

    /// session/new on the connection stream; returns the session id.
    std::string new_session(SseStream& connection_stream, int id) {
        EXPECT_EQ(post(new_session_request(id)).status, 202);
        const auto answer = connection_stream.response(id);
        if (!answer || !answer->contains("result")) {
            ADD_FAILURE() << "no session/new result";
            return {};
        }
        return (*answer)["result"].value("sessionId", std::string());
    }

  private:
    int              port_;
    httplib::Headers base_;
    std::string      connection_;
};

// --- WebSocket client ------------------------------------------------------------------

class WsAgent {
  public:
    explicit WsAgent(int port, httplib::Headers headers = {})
        : client_("ws://127.0.0.1:" + std::to_string(port) + "/acp", headers) {
        client_.set_read_timeout(100ms);
    }
    ~WsAgent() { client_.close(); }

    bool connect() {
        const auto result = client_.connect();
        status_ = result.status();
        return static_cast<bool>(result);
    }
    int status() const { return status_; }

    bool send(const json& message) { return client_.send(message.dump()); }

    std::optional<json> wait_for(const std::function<bool(const json&)>& match,
                                 std::chrono::milliseconds limit = kWait) {
        for (const auto& message : seen_) {
            if (match(message)) return message;
        }
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string text;
            const auto  result = client_.read(text);
            if (result == httplib::ws::Timeout) continue;
            if (result == httplib::ws::Fail) return std::nullopt;
            seen_.push_back(json::parse(text));
            if (match(seen_.back())) return seen_.back();
        }
        return std::nullopt;
    }

    std::optional<json> response(int id) {
        return wait_for([id](const json& message) { return is_response_to(message, id); });
    }
    std::optional<json> agent_request(const std::string& method) {
        return wait_for([&](const json& message) {
            return is_method(message, method) && message.contains("id");
        });
    }

    /// True when the peer has closed the connection (a read fails) within
    /// `limit`.
    bool wait_closed(std::chrono::milliseconds limit = kWait) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string text;
            if (client_.read(text) == httplib::ws::Fail) return true;
        }
        return false;
    }

    void close() { client_.close(); }

    /// Everything received so far, for a failure message.
    std::string dump() const {
        std::string out;
        for (const auto& message : seen_) out += message.dump() + "\n";
        return out;
    }

    std::string initialize_and_new_session() {
        EXPECT_TRUE(send(initialize_request(1)));
        EXPECT_TRUE(response(1).has_value());
        EXPECT_TRUE(send(new_session_request(2)));
        const auto answer = response(2);
        if (!answer || !answer->contains("result")) {
            ADD_FAILURE() << "no session/new result";
            return {};
        }
        return (*answer)["result"].value("sessionId", std::string());
    }

  private:
    httplib::ws::WebSocketClient client_;
    std::vector<json>            seen_;
    int                          status_ = -1;
};

// --- A raw WebSocket (frames the library client never sends) ---------------------------------

class RawWs {
  public:
    explicit RawWs(int port) : socket_(io_) {
        socket_.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    }

    /// Send the upgrade request and return the status line's code.
    int handshake(const std::string& extra_headers = {}) {
        const std::string request =
            "GET /acp HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n" + extra_headers + "\r\n";
        asio::write(socket_, asio::buffer(request));
        const auto head = read_until("\r\n\r\n", kWait);
        if (head.size() < 12) return -1;
        return std::stoi(head.substr(9, 3));
    }

    /// One frame; `mask` false sends it the way a server would.
    void send_frame(std::uint8_t opcode, const std::string& payload, bool fin = true,
                    bool mask = true) {
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | opcode));
        const std::uint8_t mask_bit = mask ? 0x80 : 0x00;
        if (payload.size() < 126) {
            frame.push_back(static_cast<char>(mask_bit | payload.size()));
        } else if (payload.size() < 65536) {
            frame.push_back(static_cast<char>(mask_bit | 126));
            frame.push_back(static_cast<char>(payload.size() >> 8));
            frame.push_back(static_cast<char>(payload.size() & 0xff));
        } else {
            frame.push_back(static_cast<char>(mask_bit | 127));
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xff));
            }
        }
        const std::array<std::uint8_t, 4> key = {0x12, 0x34, 0x56, 0x78};
        if (mask) {
            for (auto byte : key) frame.push_back(static_cast<char>(byte));
            for (std::size_t i = 0; i < payload.size(); ++i) {
                frame.push_back(static_cast<char>(payload[i] ^ static_cast<char>(key[i % 4])));
            }
        } else {
            frame += payload;
        }
        asio::error_code ignored;
        asio::write(socket_, asio::buffer(frame), ignored);
    }

    struct Frame {
        std::uint8_t opcode = 0;
        std::string  payload;
    };

    /// Frames the server sent until it closed the socket or `limit` passed.
    std::vector<Frame> read_frames(std::chrono::milliseconds limit, std::size_t want = 0) {
        std::vector<Frame> frames;
        const auto         deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            pull(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            while (auto frame = take_frame()) {
                frames.push_back(std::move(*frame));
            }
            if ((want != 0 && frames.size() >= want) || closed_) break;
        }
        return frames;
    }

    bool closed() const { return closed_; }

  private:
    std::optional<Frame> take_frame() {
        if (buffer_.size() < 2) return std::nullopt;
        const auto first  = static_cast<std::uint8_t>(buffer_[0]);
        std::uint64_t length = static_cast<std::uint8_t>(buffer_[1]) & 0x7f;
        std::size_t   offset = 2;
        if (length == 126) {
            if (buffer_.size() < 4) return std::nullopt;
            length = (static_cast<std::uint8_t>(buffer_[2]) << 8) | static_cast<std::uint8_t>(buffer_[3]);
            offset = 4;
        } else if (length == 127) {
            if (buffer_.size() < 10) return std::nullopt;
            length = 0;
            for (int i = 2; i < 10; ++i) length = (length << 8) | static_cast<std::uint8_t>(buffer_[i]);
            offset = 10;
        }
        if (buffer_.size() < offset + length) return std::nullopt;
        Frame frame;
        frame.opcode  = first & 0x0f;
        frame.payload = buffer_.substr(offset, static_cast<std::size_t>(length));
        buffer_.erase(0, offset + static_cast<std::size_t>(length));
        return frame;
    }

    void pull(std::chrono::milliseconds limit) {
        std::array<char, 4096> chunk{};
        std::size_t            received = 0;
        bool                   done     = false;
        asio::error_code       error;
        socket_.async_read_some(asio::buffer(chunk),
                                [&](const asio::error_code& ec, std::size_t count) {
                                    error    = ec;
                                    received = count;
                                    done     = true;
                                });
        io_.restart();
        io_.run_for(std::max<std::chrono::milliseconds>(limit, 1ms));
        if (!done) {
            socket_.cancel();
            io_.restart();
            io_.run();
        }
        if (received > 0) buffer_.append(chunk.data(), received);
        if (done && error && error != asio::error::operation_aborted) closed_ = true;
    }

    std::string read_until(const std::string& marker, std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        std::string head;
        while (std::chrono::steady_clock::now() < deadline) {
            pull(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
            const auto at = buffer_.find(marker);
            if (at != std::string::npos) {
                head = buffer_.substr(0, at + marker.size());
                buffer_.erase(0, at + marker.size());
                return head;
            }
            if (closed_) break;
        }
        head = buffer_;
        return head;
    }

    asio::io_context      io_;
    asio::ip::tcp::socket socket_;
    std::string           buffer_;
    bool                  closed_ = false;
};

httplib::Headers with_host(const std::string& host) { return {{"Host", host}}; }
httplib::Headers with_origin(const std::string& origin) { return {{"Origin", origin}}; }

std::string raw_http(int port, const std::string& message, std::chrono::milliseconds limit) {
    asio::io_context      io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    asio::error_code ignored;
    asio::write(socket, asio::buffer(message), ignored);
    std::string            received;
    std::array<char, 4096> buffer{};
    std::function<void()>  read_more = [&] {
        socket.async_read_some(asio::buffer(buffer),
                               [&](const asio::error_code& error, std::size_t count) {
                                   received.append(buffer.data(), count);
                                   if (!error) read_more();
                               });
    };
    read_more();
    io.run_for(limit);
    return received;
}

}  // namespace

// ===========================================================================
// Streamable HTTP
// ===========================================================================

TEST(ACPHttpServer, AFullSessionOverStreamableHttp) {
    Fixture   fixture;
    HttpAgent agent(fixture.port());

    // initialize creates the connection: 200, the response as the body, the
    // id in the header and in the result.
    const auto init = agent.initialize();
    ASSERT_EQ(init.status, 200) << init.body;
    EXPECT_EQ(init.content_type, "application/json");
    ASSERT_FALSE(init.connection_id.empty());
    const auto init_json = init.json_body();
    EXPECT_EQ(init_json["id"], 1);
    EXPECT_EQ(init_json["result"]["protocolVersion"], 1);
    EXPECT_FALSE(init_json["result"].contains("connectionId"));
    EXPECT_EQ(fixture.server->connection_count(), 1u);

    auto connection_stream = agent.stream();
    ASSERT_EQ(connection_stream->status(), 200);

    // Everything else is answered 202 and answered on a stream.
    const auto session = agent.new_session(*connection_stream, 2);
    ASSERT_FALSE(session.empty());
    auto session_stream = agent.stream(session);
    ASSERT_EQ(session_stream->status(), 200);

    const auto accepted = agent.post(prompt_request(3, session, "hello"), session);
    EXPECT_EQ(accepted.status, 202);
    EXPECT_TRUE(accepted.body.empty());
    const auto done = session_stream->response(3);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->contains("result")) << done->dump();
    EXPECT_EQ((*done)["result"]["stopReason"], "end_turn");
    // The streamed update is on the session stream, ahead of the response,
    // and the connection stream carries neither.
    const auto messages = session_stream->messages();
    bool       saw_update = false;
    for (const auto& message : messages) {
        if (has_agent_text(message, "echo:hello")) saw_update = true;
        if (is_response_to(message, 3)) break;
    }
    EXPECT_TRUE(saw_update);
    for (const auto& message : connection_stream->messages()) {
        EXPECT_FALSE(is_method(message, "session/update")) << message.dump();
        EXPECT_FALSE(is_response_to(message, 3));
    }

    // DELETE ends the connection: 202, the streams end, the id is gone.
    EXPECT_EQ(delete_connection(fixture.port(), init.connection_id).status, 202);
    EXPECT_TRUE(connection_stream->wait_ended());
    EXPECT_TRUE(session_stream->wait_ended());
    EXPECT_TRUE(fixture.wait_connections(0));
    EXPECT_EQ(agent.post(prompt_request(4, session, "again"), session).status, 404);
}

TEST(ACPHttpServer, SessionCancelEndsAPromptWithTheCancelledStopReason) {
    Fixture   fixture;
    HttpAgent agent(fixture.port());
    ASSERT_EQ(agent.initialize().status, 200);
    auto       connection_stream = agent.stream();
    const auto session           = agent.new_session(*connection_stream, 2);
    auto       session_stream    = agent.stream(session);

    ASSERT_EQ(agent.post(prompt_request(3, session, "wait"), session).status, 202);
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 1; }));
    EXPECT_EQ(agent.post(cancel_notification(session), session).status, 202);

    const auto done = session_stream->response(3);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->contains("result")) << done->dump();
    EXPECT_EQ((*done)["result"]["stopReason"], "cancelled");
    EXPECT_EQ(fixture.probe->cancelled.load(), 1);
}

TEST(ACPHttpServer, APermissionRequestTravelsOnTheSessionStreamAndItsAnswerIsAPost) {
    Fixture   fixture;
    HttpAgent agent(fixture.port());
    ASSERT_EQ(agent.initialize().status, 200);
    auto       connection_stream = agent.stream();
    const auto session           = agent.new_session(*connection_stream, 2);
    auto       session_stream    = agent.stream(session);

    ASSERT_EQ(agent.post(prompt_request(3, session, "ask"), session).status, 202);
    const auto ask = session_stream->agent_request("session/request_permission");
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ((*ask)["params"]["sessionId"], session);
    EXPECT_EQ((*ask)["params"]["toolCall"]["title"], "approval needed");

    // The client's answer is a POST of a response; the session header is
    // optional on it, and validated when it is there.
    EXPECT_EQ(agent.post(response_to(*ask, selected("approve")), session).status, 202);
    const auto done = session_stream->response(3);
    ASSERT_TRUE(done.has_value());
    ASSERT_TRUE(done->contains("result")) << done->dump();
    EXPECT_EQ((*done)["result"]["stopReason"], "end_turn");
    EXPECT_TRUE(session_stream->wait_for(
        [](const json& message) { return has_agent_text(message, "resumed:approve"); }));
    // Nothing of the session is on the connection stream.
    for (const auto& message : connection_stream->messages()) {
        EXPECT_FALSE(is_method(message, "session/request_permission")) << message.dump();
    }
}

TEST(ACPHttpServer, AFileRequestOfTheAgentReachesTheClientOfTheOwningConnection) {
    // Two connections share the engine and the one ACPClient its nodes
    // captured; each read goes to the connection that owns the session.
    Fixture   fixture;
    HttpAgent first(fixture.port());
    HttpAgent second(fixture.port());
    ASSERT_EQ(first.initialize().status, 200);
    ASSERT_EQ(second.initialize().status, 200);
    auto first_connection  = first.stream();
    auto second_connection = second.stream();
    const auto first_session  = first.new_session(*first_connection, 2);
    const auto second_session = second.new_session(*second_connection, 2);
    ASSERT_NE(first_session, second_session);
    auto first_stream  = first.stream(first_session);
    auto second_stream = second.stream(second_session);

    ASSERT_EQ(first.post(prompt_request(3, first_session, "read:/work/a.txt"), first_session).status, 202);
    ASSERT_EQ(second.post(prompt_request(3, second_session, "read:/work/b.txt"), second_session).status, 202);
    const auto first_read  = first_stream->agent_request("fs/read_text_file");
    const auto second_read = second_stream->agent_request("fs/read_text_file");
    ASSERT_TRUE(first_read.has_value());
    ASSERT_TRUE(second_read.has_value());
    EXPECT_EQ((*first_read)["params"]["path"], "/work/a.txt");
    EXPECT_EQ((*second_read)["params"]["path"], "/work/b.txt");

    // Answer in the opposite order.
    EXPECT_EQ(second.post(response_to(*second_read, {{"content", "from b"}}), second_session).status, 202);
    EXPECT_EQ(first.post(response_to(*first_read, {{"content", "from a"}}), first_session).status, 202);
    ASSERT_TRUE(first_stream->response(3).has_value());
    ASSERT_TRUE(second_stream->response(3).has_value());
    EXPECT_TRUE(first_stream->wait_for([](const json& m) { return has_agent_text(m, "read:from a"); }));
    EXPECT_TRUE(second_stream->wait_for([](const json& m) { return has_agent_text(m, "read:from b"); }));
    for (const auto& message : first_stream->messages()) {
        EXPECT_FALSE(has_agent_text(message, "read:from b")) << message.dump();
    }
    for (const auto& message : second_stream->messages()) {
        EXPECT_FALSE(has_agent_text(message, "read:from a")) << message.dump();
    }
}

TEST(ACPHttpServer, TwoConnectionsRunPromptsAtTheSameTimeAndStayApart) {
    Fixture   fixture;
    HttpAgent first(fixture.port());
    HttpAgent second(fixture.port());
    ASSERT_EQ(first.initialize().status, 200);
    ASSERT_EQ(second.initialize().status, 200);
    EXPECT_NE(first.connection(), second.connection());
    EXPECT_EQ(fixture.server->connection_count(), 2u);
    auto first_connection  = first.stream();
    auto second_connection = second.stream();
    const auto first_session  = first.new_session(*first_connection, 2);
    const auto second_session = second.new_session(*second_connection, 2);
    auto       first_stream   = first.stream(first_session);
    auto       second_stream  = second.stream(second_session);

    // Both turns are inside the node together.
    ASSERT_EQ(first.post(prompt_request(3, first_session, "wait"), first_session).status, 202);
    ASSERT_EQ(second.post(prompt_request(3, second_session, "wait"), second_session).status, 202);
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 2; }));

    // A cancel of one connection's session does not touch the other.
    EXPECT_EQ(first.post(cancel_notification(first_session), first_session).status, 202);
    const auto cancelled = first_stream->response(3);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ((*cancelled)["result"]["stopReason"], "cancelled");
    EXPECT_EQ(fixture.probe->cancelled.load(), 1);
    EXPECT_FALSE(second_stream->wait_for(
        [](const json& message) { return is_response_to(message, 3); }, 300ms).has_value());

    fixture.probe->release.store(true);
    const auto released = second_stream->response(3);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ((*released)["result"]["stopReason"], "end_turn");
    // Each stream saw only its own session.
    for (const auto& message : first_stream->messages()) {
        if (message.contains("params")) EXPECT_EQ(message["params"].value("sessionId", first_session), first_session);
    }
    for (const auto& message : second_stream->messages()) {
        if (message.contains("params")) EXPECT_EQ(message["params"].value("sessionId", second_session), second_session);
    }
}

TEST(ACPHttpServer, ALostStreamEndsTheConnectionAndCancelsItsPrompt) {
    auto config                    = test_config();
    config.connection_idle_timeout = 300ms;
    Fixture   fixture(config);
    HttpAgent agent(fixture.port());
    ASSERT_EQ(agent.initialize().status, 200);
    auto       connection_stream = agent.stream();
    const auto session           = agent.new_session(*connection_stream, 2);
    auto       session_stream    = agent.stream(session);
    ASSERT_EQ(agent.post(prompt_request(3, session, "wait"), session).status, 202);
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 1; }));

    // The client goes away without DELETE: both streams drop.
    session_stream->close();
    connection_stream->close();
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->cancelled.load() == 1; }));
    EXPECT_TRUE(fixture.wait_connections(0));
    EXPECT_EQ(agent.post(cancel_notification(session), session).status, 404);
}

TEST(ACPHttpServer, ADeletedConnectionCancelsItsPrompt) {
    Fixture   fixture;
    HttpAgent agent(fixture.port());
    ASSERT_EQ(agent.initialize().status, 200);
    auto       connection_stream = agent.stream();
    const auto session           = agent.new_session(*connection_stream, 2);
    ASSERT_EQ(agent.post(prompt_request(3, session, "wait"), session).status, 202);
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 1; }));
    EXPECT_EQ(delete_connection(fixture.port(), agent.connection()).status, 202);
    EXPECT_TRUE(fixture.wait_for([&] { return fixture.probe->cancelled.load() == 1; }));
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, MessagesBeforeTheStreamOpensWaitForIt) {
    // The agent answers session/new before the client has opened any stream;
    // the answer is kept and sent when one opens.
    Fixture   fixture;
    HttpAgent agent(fixture.port());
    ASSERT_EQ(agent.initialize().status, 200);
    ASSERT_EQ(agent.post(new_session_request(2)).status, 202);
    std::this_thread::sleep_for(100ms);
    auto connection_stream = agent.stream();
    const auto answer      = connection_stream->response(2);
    ASSERT_TRUE(answer.has_value());
    EXPECT_FALSE((*answer)["result"].value("sessionId", std::string()).empty());
}

TEST(ACPHttpServer, ASessionBelongsToOneLiveConnection) {
    Fixture   fixture;
    HttpAgent first(fixture.port());
    HttpAgent second(fixture.port());
    ASSERT_EQ(first.initialize().status, 200);
    ASSERT_EQ(second.initialize().status, 200);
    auto first_connection  = first.stream();
    auto second_connection = second.stream();
    const auto session = first.new_session(*first_connection, 2);
    auto       stream  = first.stream(session);
    ASSERT_EQ(first.post(prompt_request(3, session, "hello"), session).status, 202);
    ASSERT_TRUE(stream->response(3).has_value());

    // While the first connection lives, the second may not take the session.
    const json resume = request(2, "session/resume", {{"sessionId", session}, {"cwd", "/work"}});
    ASSERT_EQ(second.post(resume, session).status, 202);
    const auto refused = second_connection->response(2);
    ASSERT_TRUE(refused.has_value());
    ASSERT_TRUE(refused->contains("error")) << refused->dump();
    EXPECT_EQ((*refused)["error"]["data"]["reason"], "session_busy");
    EXPECT_NE(refused->dump().find("held by another connection"), std::string::npos);

    // Once it is gone the session can be resumed from the checkpoint store.
    EXPECT_EQ(delete_connection(fixture.port(), first.connection()).status, 202);
    ASSERT_TRUE(fixture.wait_connections(1));
    ASSERT_EQ(second.post(request(5, "session/resume", {{"sessionId", session}, {"cwd", "/work"}}), session).status, 202);
    const auto resumed = second_connection->response(5);
    ASSERT_TRUE(resumed.has_value());
    EXPECT_TRUE(resumed->contains("result")) << resumed->dump();
}

TEST(ACPHttpServer, RequestsAreValidatedAsTheDraftListsThem) {
    Fixture         fixture;
    HttpAgent       agent(fixture.port());
    httplib::Client client("127.0.0.1", fixture.port());
    const auto      init_body = initialize_request().dump();

    // Content type, accept, batch and syntax.
    EXPECT_EQ(to_reply(client.Post("/acp", init_body, "text/plain")).status, 415);
    EXPECT_EQ(to_reply(client.Post("/acp", "[]", "application/json")).status, 501);
    const auto malformed = to_reply(client.Post("/acp", "{", "application/json"));
    EXPECT_EQ(malformed.status, 400);
    EXPECT_EQ(malformed.json_body()["error"]["code"], -32700);
    EXPECT_EQ(to_reply(client.Post("/acp", "3", "application/json")).status, 400);

    // Only initialize may come without a connection id.
    EXPECT_EQ(to_reply(client.Post("/acp", new_session_request(2).dump(), "application/json")).status, 400);
    ASSERT_EQ(agent.initialize().status, 200);
    const auto session_less = agent.post(new_session_request(2));
    EXPECT_EQ(session_less.status, 202);

    // Unknown ids.
    httplib::Headers unknown = {{"Acp-Connection-Id", "conn-nope"}};
    EXPECT_EQ(to_reply(client.Post("/acp", unknown, new_session_request(3).dump(), "application/json")).status, 404);
    EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Acp-Connection-Id", "conn-nope"}, {"Accept", "text/event-stream"}})).status, 404);
    EXPECT_EQ(to_reply(client.Delete("/acp", unknown)).status, 404);
    EXPECT_EQ(to_reply(client.Delete("/acp")).status, 400);

    // GET needs Accept: text/event-stream and an id.
    EXPECT_EQ(to_reply(client.Get("/acp", agent.headers())).status, 406);
    EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Accept", "text/event-stream"}})).status, 400);

    // A session request names its session in the header, and the session must
    // be one of the connection.
    auto connection_stream = agent.stream();
    const auto session = agent.new_session(*connection_stream, 7);
    EXPECT_EQ(agent.post(prompt_request(8, session, "x")).status, 400);                  // no header
    EXPECT_EQ(agent.post(prompt_request(8, session, "x"), "sess-other").status, 400);    // mismatch
    EXPECT_EQ(agent.post(cancel_notification("sess-other"), "sess-other").status, 404);  // unknown
    EXPECT_EQ(to_reply(client.Get("/acp", agent.headers("sess-other"))).status, 406);
    auto absent = agent.stream("sess-other");
    EXPECT_EQ(absent->status(), 404);
}

TEST(ACPHttpServer, ARefusedInitializeLeavesNoConnection) {
    Fixture fixture;
    const auto reply = post_json(fixture.port(), request(1, "initialize", {{"protocolVersion", "one"}}));
    EXPECT_EQ(reply.status, 200);
    EXPECT_TRUE(reply.connection_id.empty());
    EXPECT_TRUE(reply.json_body().contains("error"));
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, TheConnectionLimitIsEnforced) {
    auto config            = test_config();
    config.max_connections = 2;
    Fixture   fixture(config);
    HttpAgent a(fixture.port()), b(fixture.port()), c(fixture.port());
    ASSERT_EQ(a.initialize().status, 200);
    ASSERT_EQ(b.initialize().status, 200);
    EXPECT_EQ(c.initialize().status, 503);
    EXPECT_EQ(delete_connection(fixture.port(), a.connection()).status, 202);
    ASSERT_TRUE(fixture.wait_connections(1));
    EXPECT_EQ(c.initialize().status, 200);
}

// ===========================================================================
// Guard and authentication (both transports)
// ===========================================================================

TEST(ACPHttpServer, ALoopbackBindAnswersOnlyLoopbackHosts) {
    Fixture         fixture;
    httplib::Client client("127.0.0.1", fixture.port());
    const auto      port = std::to_string(fixture.port());
    const auto      body = initialize_request().dump();

    for (const auto& host : {"127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port,
                             "LOCALHOST:" + port, std::string("localhost")}) {
        const auto reply = to_reply(client.Post("/acp", with_host(host), body, "application/json"));
        EXPECT_EQ(reply.status, 200) << host;
        if (reply.status == 200) {
            EXPECT_EQ(delete_connection(fixture.port(), reply.connection_id).status, 202);
        }
    }
    // A DNS-rebinding page sends its own name in Host, with or without an
    // Origin: every route refuses it, streams and unknown paths included.
    for (const auto& host : {"evil.example:" + port, std::string("evil.example"),
                             "127.0.0.2:" + port, "localhost.:" + port}) {
        EXPECT_EQ(to_reply(client.Post("/acp", with_host(host), body, "application/json")).status, 403) << host;
        EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Host", host}, {"Accept", "text/event-stream"}})).status, 403) << host;
        EXPECT_EQ(to_reply(client.Delete("/acp", with_host(host))).status, 403) << host;
        EXPECT_EQ(to_reply(client.Get("/nowhere", with_host(host))).status, 403) << host;
    }
    for (const auto& host : {std::string("localhost:http"), std::string("[::1"), std::string(""),
                             ":" + port}) {
        EXPECT_EQ(to_reply(client.Post("/acp", with_host(host), body, "application/json")).status, 400) << host;
    }
    EXPECT_EQ(to_reply(client.Get("/nowhere")).status, 404);
    EXPECT_EQ(fixture.server->connection_count(), 0u);

    // No Host at all.
    const auto reply = raw_http(fixture.port(),
                                "POST /acp HTTP/1.1\r\nContent-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(body.size()) +
                                    "\r\nConnection: close\r\n\r\n" + body,
                                3s);
    EXPECT_EQ(reply.rfind("HTTP/1.1 400", 0), 0u) << reply;
}

TEST(ACPHttpServer, AnOriginIsRefusedUnlessListedOrItsOwnLoopbackOrigin) {
    Fixture         fixture;
    httplib::Client client("127.0.0.1", fixture.port());
    const auto      port = std::to_string(fixture.port());
    const auto      body = initialize_request().dump();

    EXPECT_EQ(to_reply(client.Post("/acp", with_origin("https://attacker.example"), body, "application/json")).status, 403);
    EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Origin", "https://attacker.example"}, {"Accept", "text/event-stream"}})).status, 403);
    EXPECT_EQ(to_reply(client.Delete("/acp", with_origin("https://attacker.example"))).status, 403);
    // A page served by this machine at this port needs no listing (and says
    // so once on stderr).
    testing::internal::CaptureStderr();
    const auto own = to_reply(client.Post("/acp", with_origin("http://localhost:" + port), body, "application/json"));
    const auto warning = testing::internal::GetCapturedStderr();
    EXPECT_EQ(own.status, 200);
    EXPECT_NE(warning.find("admits Origin http://localhost:" + port), std::string::npos) << warning;
    EXPECT_EQ(to_reply(client.Post("/acp", with_origin("http://localhost:1"), body, "application/json")).status, 403);

    auto config            = test_config();
    config.allowed_origins = {"https://app.example"};
    config.allowed_hosts   = {"agent.example"};
    Fixture listed(config);
    httplib::Client other("127.0.0.1", listed.port());
    EXPECT_EQ(to_reply(other.Post("/acp", {{"Origin", "https://app.example"}, {"Host", "agent.example"}}, body, "application/json")).status, 200);
    EXPECT_EQ(to_reply(other.Post("/acp", {{"Origin", "https://app.example"}, {"Host", "evil.example"}}, body, "application/json")).status, 403);
    EXPECT_EQ(to_reply(other.Post("/acp", {{"Origin", "https://other.example"}, {"Host", "agent.example"}}, body, "application/json")).status, 403);
}

TEST(ACPHttpServer, ABodyOverTheLimitIsRefusedBeforeItIsRead) {
    auto config              = test_config();
    config.max_payload_bytes = 4096;
    Fixture         fixture(config);
    httplib::Client client("127.0.0.1", fixture.port());

    json padded = initialize_request();
    padded["params"]["clientInfo"] = {{"name", std::string(8192, 'x')}};
    EXPECT_EQ(to_reply(client.Post("/acp", padded.dump(), "application/json")).status, 413);
    EXPECT_EQ(fixture.server->connection_count(), 0u);
    EXPECT_EQ(to_reply(client.Post("/acp", initialize_request().dump(), "application/json")).status, 200);
}

TEST(ACPHttpServer, ARefusalComesBeforeTheBodyIsRead) {
    // The request promises a 100 MB body and sends none. A server that read
    // the body first would wait for it; each check answers from the headers.
    auto config = test_config();
    config.authenticator = [](std::string_view header) -> std::optional<std::string> {
        if (header == "Bearer ok") return std::string("p");
        return std::nullopt;
    };
    Fixture    fixture(config);
    const auto port = std::to_string(fixture.port());
    const auto head = [&](const std::string& extra) {
        return "POST /acp HTTP/1.1\r\nContent-Type: application/json\r\n"
               "Content-Length: 100000000\r\nConnection: close\r\n" + extra + "\r\n";
    };
    const auto status_of = [&](const std::string& request) {
        const auto reply = raw_http(fixture.port(), request, 2s);
        return reply.substr(0, std::min<std::size_t>(12, reply.size()));
    };
    const std::string host = "Host: 127.0.0.1:" + port + "\r\n";
    const std::string auth = "Authorization: Bearer ok\r\n";
    EXPECT_EQ(status_of(head("Host: evil.example\r\n" + auth)), "HTTP/1.1 403");
    EXPECT_EQ(status_of(head(host + auth + "Origin: https://attacker.example\r\n")), "HTTP/1.1 403");
    EXPECT_EQ(status_of(head(host)), "HTTP/1.1 401");
    // Everything passes: only the size limit is left, and it answers from
    // the Content-Length.
    EXPECT_EQ(status_of(head(host + auth)), "HTTP/1.1 413");
}

TEST(ACPHttpServer, EveryRouteNeedsTheAuthenticatorWhenOneIsSet) {
    auto config = test_config();
    std::atomic<int> calls{0};
    config.authenticator = [&calls](std::string_view header) -> std::optional<std::string> {
        calls.fetch_add(1);
        if (header == "Bearer token-a") return std::string("alice");
        if (header == "Bearer token-b") return std::string("bob");
        return std::nullopt;
    };
    Fixture         fixture(config);
    httplib::Client client("127.0.0.1", fixture.port());
    const auto      body = initialize_request().dump();

    // Refused before anything is read or created: 401 with a challenge.
    const auto refused = client.Post("/acp", body, "application/json");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 401);
    EXPECT_EQ(refused->get_header_value("WWW-Authenticate"), "Bearer");
    EXPECT_EQ(to_reply(client.Post("/acp", {{"Authorization", "Bearer wrong"}}, body, "application/json")).status, 401);
    EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Accept", "text/event-stream"}})).status, 401);
    EXPECT_EQ(to_reply(client.Delete("/acp")).status, 401);
    EXPECT_EQ(to_reply(client.Get("/nowhere")).status, 401);
    WsAgent unauthenticated(fixture.port());
    EXPECT_FALSE(unauthenticated.connect());
    EXPECT_EQ(unauthenticated.status(), 401);
    EXPECT_EQ(fixture.server->connection_count(), 0u);

    // A connection answers only to the principal that made it.
    HttpAgent alice(fixture.port(), {{"Authorization", "Bearer token-a"}});
    ASSERT_EQ(alice.initialize().status, 200);
    auto alice_stream = alice.stream();
    EXPECT_EQ(alice_stream->status(), 200);
    HttpAgent bob(fixture.port(), {{"Authorization", "Bearer token-b"}});
    httplib::Headers stolen = {{"Authorization", "Bearer token-b"}, {"Acp-Connection-Id", alice.connection()}};
    EXPECT_EQ(to_reply(client.Post("/acp", stolen, new_session_request(2).dump(), "application/json")).status, 404);
    EXPECT_EQ(to_reply(client.Delete("/acp", stolen)).status, 404);
    EXPECT_EQ(to_reply(client.Get("/acp", httplib::Headers{{"Authorization", "Bearer token-b"}, {"Acp-Connection-Id", alice.connection()}, {"Accept", "text/event-stream"}})).status, 404);
    EXPECT_EQ(fixture.server->connection_count(), 1u);
    EXPECT_GT(calls.load(), 0);
}

TEST(ACPHttpServer, TheFactoryGetsThePrincipal) {
    std::mutex               mutex;
    std::vector<std::string> principals;
    auto                     engine = build_engine(std::make_shared<Probe>(), std::make_shared<ACPClient>());
    ACPHttpConfig            config = test_config();
    config.authenticator = [](std::string_view header) -> std::optional<std::string> {
        if (header.rfind("Bearer ", 0) == 0) return std::string(header.substr(7));
        return std::nullopt;
    };
    ACPHttpServer server(
        [&](std::string_view principal) {
            {
                std::lock_guard lock(mutex);
                principals.emplace_back(principal);
            }
            ACPHttpConnection connection;
            connection.server = std::make_unique<ACPServer>(engine, json{{"name", "x"}});
            return connection;
        },
        config);
    ASSERT_TRUE(server.start_async());
    const auto reply = post_json(server.port(), initialize_request(), {{"Authorization", "Bearer carol"}});
    EXPECT_EQ(reply.status, 200);
    // An upgrade carries its principal to the factory as well.
    WsAgent socket(server.port(), {{"Authorization", "Bearer dave"}});
    EXPECT_TRUE(socket.connect());
    {
        std::lock_guard lock(mutex);
        ASSERT_EQ(principals.size(), 2u);
        EXPECT_EQ(principals[0], "carol");
        EXPECT_EQ(principals[1], "dave");
    }
    socket.close();
    server.stop();
}

TEST(ACPHttpServer, TheConfigurationIsValidated) {
    auto engine = build_engine(std::make_shared<Probe>(), std::make_shared<ACPClient>());
    const ACPHttpServer::ConnectionFactory factory = [engine](std::string_view) {
        ACPHttpConnection connection;
        connection.server = std::make_unique<ACPServer>(engine, json{{"name", "x"}});
        return connection;
    };
    EXPECT_THROW(ACPHttpServer(nullptr), std::invalid_argument);

    // A remote bind needs an authenticator; loopback names do not.
    ACPHttpConfig remote;
    remote.host = "0.0.0.0";
    EXPECT_THROW(ACPHttpServer(factory, remote), std::invalid_argument);
    remote.authenticator = [](std::string_view) { return std::optional<std::string>("x"); };
    EXPECT_NO_THROW(ACPHttpServer(factory, remote));
    for (const char* host : {"127.0.0.1", "::1", "localhost"}) {
        ACPHttpConfig local;
        local.host = host;
        EXPECT_NO_THROW(ACPHttpServer(factory, local)) << host;
    }

    const auto invalid = [&](const std::function<void(ACPHttpConfig&)>& change) {
        ACPHttpConfig config;
        change(config);
        EXPECT_THROW(ACPHttpServer(factory, config), std::invalid_argument);
    };
    invalid([](ACPHttpConfig& c) { c.endpoint = "acp"; });
    invalid([](ACPHttpConfig& c) { c.endpoint = ""; });
    invalid([](ACPHttpConfig& c) { c.port = 70000; });
    invalid([](ACPHttpConfig& c) { c.max_connections = 0; });
    invalid([](ACPHttpConfig& c) { c.max_payload_bytes = 0; });
    invalid([](ACPHttpConfig& c) { c.max_payload_bytes = 32 * 1024 * 1024; });
    invalid([](ACPHttpConfig& c) { c.http_threads = 0; });
    invalid([](ACPHttpConfig& c) { c.max_http_threads = 1; });
    invalid([](ACPHttpConfig& c) { c.keepalive_interval = 0ms; });
    invalid([](ACPHttpConfig& c) { c.allowed_hosts = {"host:80"}; });
    invalid([](ACPHttpConfig& c) { c.streamable_http = false; c.websocket = false; });
    ACPHttpConfig large;
    large.websocket         = false;
    large.max_payload_bytes = 32 * 1024 * 1024;
    EXPECT_NO_THROW(ACPHttpServer(factory, large));
}

// ===========================================================================
// WebSocket
// ===========================================================================

TEST(ACPHttpServer, AFullSessionOverWebSocket) {
    Fixture fixture;
    WsAgent agent(fixture.port());
    ASSERT_TRUE(agent.connect());
    EXPECT_TRUE(fixture.wait_connections(1));

    // The first message is initialize; the response arrives as a frame.
    ASSERT_TRUE(agent.send(initialize_request(1)));
    const auto init = agent.response(1);
    ASSERT_TRUE(init.has_value());
    EXPECT_EQ((*init)["result"]["protocolVersion"], 1);

    ASSERT_TRUE(agent.send(new_session_request(2)));
    const auto created = agent.response(2);
    ASSERT_TRUE(created.has_value());
    const auto session = (*created)["result"].value("sessionId", std::string());
    ASSERT_FALSE(session.empty());

    ASSERT_TRUE(agent.send(prompt_request(3, session, "hello")));
    const auto done = agent.response(3);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ((*done)["result"]["stopReason"], "end_turn");
    EXPECT_TRUE(agent.wait_for([](const json& m) { return has_agent_text(m, "echo:hello"); }, 1s));

    // Cancel.
    ASSERT_TRUE(agent.send(prompt_request(4, session, "wait")));
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 1; }));
    ASSERT_TRUE(agent.send(cancel_notification(session)));
    const auto cancelled = agent.response(4);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ((*cancelled)["result"]["stopReason"], "cancelled");

    // A permission round trip: the request is a frame and so is the answer.
    // A fresh session: the cancelled turn above left its own state behind.
    ASSERT_TRUE(agent.send(new_session_request(7)));
    const auto asking = agent.response(7);
    ASSERT_TRUE(asking.has_value());
    const auto ask_session = (*asking)["result"].value("sessionId", std::string());
    ASSERT_TRUE(agent.send(prompt_request(5, ask_session, "ask")));
    const auto ask = agent.agent_request("session/request_permission");
    ASSERT_TRUE(ask.has_value());
    ASSERT_TRUE(agent.send(response_to(*ask, selected("approve"))));
    const auto answered = agent.response(5);
    ASSERT_TRUE(answered.has_value());
    EXPECT_EQ((*answered)["result"]["stopReason"], "end_turn");
    EXPECT_TRUE(agent.wait_for([](const json& m) { return has_agent_text(m, "resumed:approve"); }, 1s))
        << agent.dump();

    // fs/* through the shared client.
    ASSERT_TRUE(agent.send(prompt_request(6, session, "read:/work/a.txt")));
    const auto read = agent.agent_request("fs/read_text_file");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ((*read)["params"]["path"], "/work/a.txt");
    ASSERT_TRUE(agent.send(response_to(*read, {{"content", "ws file"}})));
    ASSERT_TRUE(agent.response(6).has_value());
    EXPECT_TRUE(agent.wait_for([](const json& m) { return has_agent_text(m, "read:ws file"); }, 1s));

    agent.close();
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, TwoWebSocketConnectionsAreIndependent) {
    Fixture fixture;
    WsAgent first(fixture.port());
    WsAgent second(fixture.port());
    ASSERT_TRUE(first.connect());
    ASSERT_TRUE(second.connect());
    const auto first_session  = first.initialize_and_new_session();
    const auto second_session = second.initialize_and_new_session();
    ASSERT_NE(first_session, second_session);
    EXPECT_EQ(fixture.server->connection_count(), 2u);

    ASSERT_TRUE(first.send(prompt_request(3, first_session, "wait")));
    ASSERT_TRUE(second.send(prompt_request(3, second_session, "wait")));
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 2; }));
    ASSERT_TRUE(first.send(cancel_notification(first_session)));
    const auto cancelled = first.response(3);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ((*cancelled)["result"]["stopReason"], "cancelled");
    EXPECT_FALSE(second.wait_for([](const json& m) { return is_response_to(m, 3); }, 300ms).has_value());
    fixture.probe->release.store(true);
    const auto released = second.response(3);
    ASSERT_TRUE(released.has_value());
    EXPECT_EQ((*released)["result"]["stopReason"], "end_turn");

    // A session of one connection cannot be taken by the other.
    ASSERT_TRUE(second.send(request(9, "session/resume", {{"sessionId", first_session}, {"cwd", "/work"}})));
    const auto refused = second.response(9);
    ASSERT_TRUE(refused.has_value());
    ASSERT_TRUE(refused->contains("error")) << refused->dump();
    EXPECT_EQ((*refused)["error"]["data"]["reason"], "session_busy");
}

TEST(ACPHttpServer, ADroppedWebSocketCancelsItsPrompt) {
    Fixture fixture;
    WsAgent agent(fixture.port());
    ASSERT_TRUE(agent.connect());
    const auto session = agent.initialize_and_new_session();
    ASSERT_TRUE(agent.send(prompt_request(3, session, "wait")));
    ASSERT_TRUE(fixture.wait_for([&] { return fixture.probe->entered.load() == 1; }));
    agent.close();
    EXPECT_TRUE(fixture.wait_for([&] { return fixture.probe->cancelled.load() == 1; }));
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, TheUpgradeIsCheckedLikeEveryOtherRequest) {
    Fixture fixture;
    {
        WsAgent foreign_origin(fixture.port(), with_origin("https://attacker.example"));
        EXPECT_FALSE(foreign_origin.connect());
        EXPECT_EQ(foreign_origin.status(), 403);
    }
    {
        WsAgent foreign_host(fixture.port(), with_host("evil.example"));
        EXPECT_FALSE(foreign_host.connect());
        EXPECT_EQ(foreign_host.status(), 403);
    }
    {
        WsAgent own_origin(fixture.port(), with_origin("http://127.0.0.1:" + std::to_string(fixture.port())));
        testing::internal::CaptureStderr();
        const bool connected = own_origin.connect();
        (void)testing::internal::GetCapturedStderr();
        EXPECT_TRUE(connected);
    }
    EXPECT_TRUE(fixture.wait_connections(0));
    // A listed origin passes.
    auto config            = test_config();
    config.allowed_origins = {"https://app.example"};
    Fixture listed(config);
    WsAgent app(listed.port(), with_origin("https://app.example"));
    EXPECT_TRUE(app.connect());
}

TEST(ACPHttpServer, AWebSocketMessageOverTheLimitClosesTheConnection) {
    auto config              = test_config();
    config.max_payload_bytes = 4096;
    Fixture fixture(config);
    WsAgent agent(fixture.port());
    ASSERT_TRUE(agent.connect());
    const auto session = agent.initialize_and_new_session();
    ASSERT_FALSE(session.empty());

    json big = prompt_request(3, session, std::string(8192, 'x'));
    ASSERT_TRUE(agent.send(big));
    EXPECT_TRUE(agent.wait_closed());
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, AnUnmaskedClientFrameClosesTheConnection) {
    Fixture fixture;
    RawWs   raw(fixture.port());
    ASSERT_EQ(raw.handshake(), 101);
    EXPECT_TRUE(fixture.wait_connections(1));
    raw.send_frame(0x1, initialize_request(1).dump(), true, /*mask=*/false);
    raw.read_frames(5s);
    EXPECT_TRUE(raw.closed());
    EXPECT_TRUE(fixture.wait_connections(0));
}

TEST(ACPHttpServer, FragmentedMessagesPingsAndBinaryFramesFollowRfc6455) {
    Fixture fixture;
    RawWs   raw(fixture.port());
    ASSERT_EQ(raw.handshake(), 101);

    // initialize in two fragments with a ping between them.
    const auto text  = initialize_request(1).dump();
    const auto split = text.size() / 2;
    raw.send_frame(0x1, text.substr(0, split), /*fin=*/false);
    raw.send_frame(0x9, "ping!");
    raw.send_frame(0x0, text.substr(split), /*fin=*/true);
    // A binary frame is ignored.
    raw.send_frame(0x2, "ignored");
    raw.send_frame(0x1, new_session_request(2).dump());

    const auto frames = raw.read_frames(5s, 3);
    ASSERT_GE(frames.size(), 3u);
    bool saw_pong = false;
    int  answers  = 0;
    for (const auto& frame : frames) {
        if (frame.opcode == 0xA) {
            saw_pong = true;
            EXPECT_EQ(frame.payload, "ping!");
        }
        if (frame.opcode == 0x1) {
            const auto message = json::parse(frame.payload);
            if (is_response_to(message, 1) || is_response_to(message, 2)) ++answers;
        }
    }
    EXPECT_TRUE(saw_pong);
    EXPECT_EQ(answers, 2);

    // A text frame that is not JSON is a JSON-RPC parse error, not a close.
    raw.send_frame(0x1, "not json");
    const auto errors = raw.read_frames(5s, 1);
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(json::parse(errors[0].payload)["error"]["code"], -32700);
    // And a batch is refused.
    raw.send_frame(0x1, "[]");
    const auto batch = raw.read_frames(5s, 1);
    ASSERT_FALSE(batch.empty());
    EXPECT_EQ(json::parse(batch[0].payload)["error"]["code"], -32600);
}

TEST(ACPHttpServer, ConnectionsOverTheLimitAreRefusedOnBothTransports) {
    auto config            = test_config();
    config.max_connections = 1;
    Fixture fixture(config);
    WsAgent first(fixture.port());
    ASSERT_TRUE(first.connect());
    WsAgent second(fixture.port());
    EXPECT_FALSE(second.connect());
    EXPECT_EQ(second.status(), 503);
    EXPECT_EQ(post_json(fixture.port(), initialize_request()).status, 503);
}

TEST(ACPHttpServer, EachTransportCanBeLeftOut) {
    auto config            = test_config();
    config.websocket       = false;
    Fixture http_only(config);
    EXPECT_EQ(post_json(http_only.port(), initialize_request()).status, 200);
    WsAgent refused(http_only.port());
    EXPECT_FALSE(refused.connect());

    config                 = test_config();
    config.streamable_http = false;
    Fixture ws_only(config);
    EXPECT_EQ(post_json(ws_only.port(), initialize_request()).status, 404);
    WsAgent accepted(ws_only.port());
    EXPECT_TRUE(accepted.connect());
}

TEST(ACPHttpServer, StopEndsEveryConnectionAndCancelsItsPrompts) {
    auto fixture = std::make_unique<Fixture>();
    HttpAgent http(fixture->port());
    ASSERT_EQ(http.initialize().status, 200);
    auto       connection_stream = http.stream();
    const auto http_session      = http.new_session(*connection_stream, 2);
    WsAgent    ws(fixture->port());
    ASSERT_TRUE(ws.connect());
    const auto ws_session = ws.initialize_and_new_session();
    ASSERT_EQ(http.post(prompt_request(3, http_session, "wait"), http_session).status, 202);
    ASSERT_TRUE(ws.send(prompt_request(3, ws_session, "wait")));
    ASSERT_TRUE(fixture->wait_for([&] { return fixture->probe->entered.load() == 2; }));

    const auto probe = fixture->probe;
    fixture->server->stop();
    EXPECT_EQ(probe->cancelled.load(), 2);
    EXPECT_EQ(fixture->server->connection_count(), 0u);
    EXPECT_FALSE(fixture->server->is_running());
    EXPECT_TRUE(connection_stream->wait_ended());
    EXPECT_TRUE(ws.wait_closed());
}
