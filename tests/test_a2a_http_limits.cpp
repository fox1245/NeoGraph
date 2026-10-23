// The Host and Origin checks and the request size limit of the A2A server.
//
// A page on another origin reaches a server bound to a loopback address when
// its host name is re-pointed at this machine (DNS rebinding), and a request
// body is otherwise read up to the HTTP library's own default of 100 MiB.
// The tests follow tests/test_mcp_http_server.cpp, which holds the same
// checks for the MCP HTTP server: the same names, the same defaults, and a
// test per route family of this server (the agent card, the JSON-RPC
// endpoint, its event streams and a path it does not serve).

#include <neograph/a2a/server.h>
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
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::a2a;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

using namespace std::chrono_literals;

class EchoNode final : public GraphNode {
  public:
    explicit EchoNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto raw = in.state.get("prompt");
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("echo:" + (raw.is_string() ? raw.get<std::string>() : raw.dump()))});
        co_return out;
    }
    std::string get_name() const override { return name_; }

  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> echo_engine() {
    NodeFactory::instance().register_type(
        "a2a_http_limits_echo",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EchoNode>(name);
        });
    const json def = {
        {"name", "echo-graph"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}}, {"response", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"echo", {{"type", "a2a_http_limits_echo"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "echo"}},
                               json{{"from", "echo"}, {"to", "__end__"}}})},
    };
    NodeContext ctx;
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, ctx));
}

AgentCard make_card() {
    AgentCard card;
    card.name                 = "limits-echo";
    card.description          = "echo agent for the HTTP limit tests";
    card.version              = "0.0.1";
    card.protocol_version     = "0.3.0";
    card.preferred_transport  = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names          = {"echo"};
    return card;
}

/// A server on `host`, port 0, stopped when it goes out of scope.
/// `configure` runs before it starts.
class RunningServer {
  public:
    explicit RunningServer(const std::string& host = "127.0.0.1",
                           const std::function<void(A2AServer&)>& configure = {})
        : server_(echo_engine(), make_card()) {
        if (configure) configure(server_);
        if (!server_.start_async(host, 0)) throw std::runtime_error("the server did not start");
    }
    ~RunningServer() { server_.stop(); }
    int port() const { return server_.port(); }

  private:
    A2AServer server_;
};

json message_params(const std::string& text) {
    return {{"message",
             {{"kind", "message"},
              {"messageId", "m-1"},
              {"role", "user"},
              {"parts", json::array({json{{"kind", "text"}, {"text", text}}})}}}};
}

std::string rpc_body(const std::string& method, const json& params) {
    return json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump();
}

std::string send_body(const std::string& text = "hello") {
    return rpc_body("message/send", message_params(text));
}

struct Reply {
    int         status = -1;  ///< -1: no response
    std::string body;
    std::string content_type;
};

Reply to_reply(const httplib::Result& result) {
    if (!result) return {};
    return {result->status, result->body, result->get_header_value("Content-Type")};
}

httplib::Headers with_host(const std::string& host) { return {{"Host", host}}; }
httplib::Headers with_origin(const std::string& origin) { return {{"Origin", origin}}; }

Reply get_card(httplib::Client& client, const httplib::Headers& headers = {}) {
    return to_reply(client.Get("/.well-known/agent-card.json", headers));
}

Reply post_rpc(httplib::Client& client, const std::string& body,
               const httplib::Headers& headers = {}) {
    return to_reply(client.Post("/", headers, body, "application/json"));
}

int card_status(httplib::Client& client, const httplib::Headers& headers) {
    return get_card(client, headers).status;
}

int send_status(httplib::Client& client, const httplib::Headers& headers) {
    return post_rpc(client, send_body(), headers).status;
}

// What a raw connection received before the server closed it or the time ran
// out.
std::string send_raw(int port, const std::string& message, std::chrono::milliseconds limit) {
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

A2AHttpConfig config_with_hosts(std::vector<std::string> hosts) {
    A2AHttpConfig config;
    config.allowed_hosts = std::move(hosts);
    return config;
}

}  // namespace

// --- Host ---------------------------------------------------------------------

TEST(A2AHttpLimits, ALoopbackBindAnswersOnlyLoopbackHosts) {
    // A DNS-rebinding page sends its own name in Host, with or without an
    // Origin. A loopback bind answers the loopback names on any port.
    RunningServer    server;
    httplib::Client  client("127.0.0.1", server.port());
    const auto       port = std::to_string(server.port());

    for (const auto& host : {"127.0.0.1:" + port, "localhost:" + port, "[::1]:" + port,
                             "LOCALHOST:" + port, std::string("localhost"),
                             std::string("127.0.0.1:1")}) {
        EXPECT_EQ(card_status(client, with_host(host)), 200) << host;
        EXPECT_EQ(send_status(client, with_host(host)), 200) << host;
    }
    for (const auto& host : {"evil.example:" + port, std::string("evil.example"),
                             "127.0.0.2:" + port, "localhost.:" + port, "[::2]:" + port}) {
        EXPECT_EQ(card_status(client, with_host(host)), 403) << host;
        EXPECT_EQ(send_status(client, with_host(host)), 403) << host;
    }
    for (const auto& host : {std::string("localhost:http"), std::string("[::1"),
                             std::string("[::1]x"), std::string(""), ":" + port}) {
        EXPECT_EQ(card_status(client, with_host(host)), 400) << host;
        EXPECT_EQ(send_status(client, with_host(host)), 400) << host;
    }

    // A request without Host at all.
    const auto body  = send_body();
    const auto reply = send_raw(server.port(),
                                "POST / HTTP/1.1\r\nContent-Type: application/json\r\n"
                                "Content-Length: " + std::to_string(body.size()) +
                                    "\r\nConnection: close\r\n\r\n" + body,
                                3s);
    EXPECT_EQ(reply.rfind("HTTP/1.1 400", 0), 0u) << reply;
}

TEST(A2AHttpLimits, ARefusedRequestSaysWhyInTheShapeOfItsRoute) {
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());

    // The JSON-RPC endpoint answers with a JSON-RPC error, like the other
    // refusals of a peer.
    const auto rpc = post_rpc(client, send_body(), with_host("evil.example"));
    ASSERT_EQ(rpc.status, 403);
    EXPECT_EQ(rpc.content_type, "application/json");
    const auto rpc_json = json::parse(rpc.body);
    EXPECT_EQ(rpc_json["error"]["code"], -32600);
    EXPECT_EQ(rpc_json["error"]["message"], "Host is not allowed");
    EXPECT_EQ(rpc_json["error"]["data"][0]["reason"], "PERMISSION_DENIED");
    EXPECT_TRUE(rpc_json["id"].is_null());
    // Nothing ran: the task store is empty.
    const auto listed = json::parse(post_rpc(client, rpc_body("ListTasks", json::object()),
                                             {{"A2A-Version", "1.0"}}).body);
    EXPECT_EQ(listed["result"]["totalSize"], 0) << listed.dump();

    // The card route answers with a plain error object.
    const auto card = get_card(client, with_host("evil.example"));
    ASSERT_EQ(card.status, 403);
    EXPECT_EQ(json::parse(card.body)["error"], "Host is not allowed");
    const auto origin = get_card(client, with_origin("https://attacker.example"));
    ASSERT_EQ(origin.status, 403);
    EXPECT_EQ(json::parse(origin.body)["error"], "Origin is not allowed");

    const auto malformed = post_rpc(client, send_body(), with_host("[::1"));
    ASSERT_EQ(malformed.status, 400);
    EXPECT_EQ(json::parse(malformed.body)["error"]["message"], "Host header is missing or invalid");
}

TEST(A2AHttpLimits, EveryRouteIsCheckedIncludingThoseTheServerDoesNotServe) {
    // The check runs before routing, so a path with no handler answers 403 to
    // a foreign Host and Origin, and 404 only to a request that passes.
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());
    const auto      foreign = with_host("evil.example");

    EXPECT_EQ(to_reply(client.Get("/nowhere", foreign)).status, 403);
    EXPECT_EQ(to_reply(client.Get("/nowhere")).status, 404);
    EXPECT_EQ(to_reply(client.Delete("/", foreign)).status, 403);
    EXPECT_EQ(to_reply(client.Delete("/")).status, 404);
    EXPECT_EQ(to_reply(client.Put("/", foreign, "{}", "application/json")).status, 403);
    EXPECT_EQ(to_reply(client.Options("/", foreign)).status, 403);
    EXPECT_EQ(to_reply(client.Head("/.well-known/agent-card.json", foreign)).status, 403);
    EXPECT_EQ(to_reply(client.Get("/nowhere", with_origin("https://attacker.example"))).status, 403);
    EXPECT_EQ(to_reply(client.Delete("/", with_origin("https://attacker.example"))).status, 403);
}

TEST(A2AHttpLimits, EventStreamRoutesAreCheckedBeforeTheyStream) {
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());
    const auto      port = std::to_string(server.port());

    const json missing_task = {{"id", "no-such-task"}};
    const std::vector<std::pair<std::string, json>> streams = {
        {"message/stream", message_params("hello")},
        {"SendStreamingMessage", message_params("hello")},
        {"tasks/resubscribe", missing_task},
        {"SubscribeToTask", missing_task},
    };
    for (const auto& [method, params] : streams) {
        const auto body = rpc_body(method, params);
        for (const auto& refused : {with_host("evil.example:" + port),
                                    with_origin("https://attacker.example")}) {
            const auto reply = post_rpc(client, body, refused);
            EXPECT_EQ(reply.status, 403) << method;
            EXPECT_EQ(reply.content_type, "application/json") << method;
        }
        // A request that passes is served: a stream, or for a task that does
        // not exist the answer of the method itself.
        const auto own = post_rpc(client, body, with_origin("http://127.0.0.1:" + port));
        EXPECT_EQ(own.status, 200) << method;
    }

    const auto streamed = post_rpc(client, rpc_body("message/stream", message_params("hello")),
                                   with_origin("http://localhost:" + port));
    ASSERT_EQ(streamed.status, 200);
    EXPECT_EQ(streamed.content_type.rfind("text/event-stream", 0), 0u) << streamed.content_type;
    EXPECT_NE(streamed.body.find("echo:hello"), std::string::npos) << streamed.body;
}

// --- Origin -------------------------------------------------------------------

TEST(A2AHttpLimits, RejectsAnUnlistedOrigin) {
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());
    EXPECT_EQ(send_status(client, with_origin("https://attacker.example")), 403);
    EXPECT_EQ(card_status(client, with_origin("https://attacker.example")), 403);
}

TEST(A2AHttpLimits, ALoopbackBindAdmitsItsOwnLoopbackOrigin) {
    // A browser sends the origin of its page. A loopback bind admits its own
    // loopback origin without a listing, and refuses every other one.
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());
    const auto      port  = std::to_string(server.port());
    const auto      other = std::to_string(server.port() == 65535 ? 65534 : server.port() + 1);

    const std::vector<std::string> own = {"http://127.0.0.1:" + port, "http://localhost:" + port,
                                          "http://[::1]:" + port};
    testing::internal::CaptureStderr();
    for (const auto& origin : own) {
        EXPECT_EQ(send_status(client, with_origin(origin)), 200) << origin;
        EXPECT_EQ(card_status(client, with_origin(origin)), 200) << origin;
    }
    const auto notes = testing::internal::GetCapturedStderr();
    // One line for each origin, the first time it is admitted.
    for (const auto& origin : own) {
        const auto line = "A2A server admits Origin " + origin + ",";
        const auto at   = notes.find(line);
        EXPECT_NE(at, std::string::npos) << notes;
        EXPECT_EQ(notes.find(line, at + 1), std::string::npos) << notes;
    }

    for (const auto& origin : {"http://127.0.0.1:" + other, "https://127.0.0.1:" + port,
                               std::string("http://127.0.0.1"), "http://LOCALHOST:" + port,
                               "http://127.0.0.1:" + port + "/", "http://127.0.0.2:" + port,
                               "http://evil.example:" + port, std::string("null")}) {
        EXPECT_EQ(send_status(client, with_origin(origin)), 403) << origin;
        EXPECT_EQ(card_status(client, with_origin(origin)), 403) << origin;
    }
}

// --- Configuration ------------------------------------------------------------

TEST(A2AHttpLimits, ARemoteBindAdmitsOnlyListedOrigins) {
    // The own-origin rule is for loopback binds: a bind to every address
    // admits no loopback origin it does not list.
    RunningServer server("0.0.0.0", [](A2AServer& configured) {
        A2AHttpConfig config;
        config.allowed_origins = {"https://app.example"};
        configured.set_http_config(config);
    });
    httplib::Client client("127.0.0.1", server.port());
    const auto      port = std::to_string(server.port());

    EXPECT_EQ(send_status(client, with_origin("https://app.example")), 200);
    EXPECT_EQ(card_status(client, with_origin("https://app.example")), 200);
    EXPECT_EQ(send_status(client, with_origin("http://127.0.0.1:" + port)), 403);
    EXPECT_EQ(send_status(client, with_origin("http://localhost:" + port)), 403);
    EXPECT_EQ(send_status(client, with_origin("https://attacker.example")), 403);
    // A request without an Origin is not an Origin check.
    EXPECT_EQ(send_status(client, {}), 200);
}

TEST(A2AHttpLimits, ARemoteBindWithNoListsChecksNoHostButStillRefusesAnOrigin) {
    RunningServer   server("0.0.0.0");
    httplib::Client client("127.0.0.1", server.port());
    EXPECT_EQ(send_status(client, with_host("anything.example.test")), 200);
    EXPECT_EQ(card_status(client, with_host("anything.example.test")), 200);
    EXPECT_EQ(send_status(client, with_origin("https://attacker.example")), 403);
}

TEST(A2AHttpLimits, ListedHostsAreAnsweredAndAStarAnswersAny) {
    RunningServer   listed("127.0.0.1", [](A2AServer& configured) {
        configured.set_http_config(config_with_hosts({"A2A.example.test", "[2001:db8::1]"}));
    });
    httplib::Client client("127.0.0.1", listed.port());
    EXPECT_EQ(send_status(client, with_host("a2a.example.test:8443")), 200);
    EXPECT_EQ(card_status(client, with_host("[2001:DB8::1]")), 200);
    EXPECT_EQ(send_status(client, with_host("localhost:" + std::to_string(listed.port()))), 200);
    EXPECT_EQ(send_status(client, with_host("other.example.test")), 403);
    EXPECT_EQ(card_status(client, with_host("other.example.test")), 403);

    RunningServer   open("127.0.0.1", [](A2AServer& configured) {
        configured.set_http_config(config_with_hosts({"*"}));
    });
    httplib::Client open_client("127.0.0.1", open.port());
    EXPECT_EQ(send_status(open_client, with_host("tunnel.example.test")), 200);
    EXPECT_EQ(card_status(open_client, with_host("localhost:http")), 200);
}

TEST(A2AHttpLimits, ARemoteBindChecksHostOnlyWhenHostsAreListed) {
    RunningServer   checked("0.0.0.0", [](A2AServer& configured) {
        configured.set_http_config(config_with_hosts({"a2a.example.test"}));
    });
    httplib::Client client("127.0.0.1", checked.port());
    EXPECT_EQ(send_status(client, with_host("a2a.example.test")), 200);
    EXPECT_EQ(card_status(client, with_host("a2a.example.test")), 200);
    EXPECT_EQ(send_status(client, with_host("127.0.0.1:" + std::to_string(checked.port()))), 403);
    EXPECT_EQ(card_status(client, with_host("other.example.test")), 403);
}

TEST(A2AHttpLimits, AllowedHostsEntriesAreNamesWithoutAPort) {
    for (const auto& entry : {"localhost:8080", "::1", "", "[::1"}) {
        A2AServer server(echo_engine(), make_card());
        EXPECT_THROW(server.set_http_config(config_with_hosts({entry})), std::invalid_argument)
            << entry;
    }
    A2AServer server(echo_engine(), make_card());
    EXPECT_NO_THROW(server.set_http_config(config_with_hosts({"[::1]", "Example.Test", "*"})));

    A2AHttpConfig none;
    none.max_payload_bytes = 0;
    EXPECT_THROW(server.set_http_config(none), std::invalid_argument);
}

TEST(A2AHttpLimits, TheConfigurationIsSetBeforeTheServerStarts) {
    A2AServer server(echo_engine(), make_card());
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    EXPECT_THROW(server.set_http_config(config_with_hosts({"a2a.example.test"})), std::logic_error);
    httplib::Client client("127.0.0.1", server.port());
    // The refused call changed nothing.
    EXPECT_EQ(card_status(client, with_host("a2a.example.test")), 403);
    server.stop();
    // A server that was stopped can be configured and started again.
    EXPECT_NO_THROW(server.set_http_config(config_with_hosts({"a2a.example.test"})));
}

// --- Request size -------------------------------------------------------------

TEST(A2AHttpLimits, TheDefaultLimitIs4MiB) {
    EXPECT_EQ(A2AHttpConfig{}.max_payload_bytes, std::size_t{4} * 1024 * 1024);
}

TEST(A2AHttpLimits, ABodyOverTheDefaultLimitGets413AndOneUnderItIsServed) {
    RunningServer   server;
    httplib::Client client("127.0.0.1", server.port());
    const auto      limit = A2AHttpConfig{}.max_payload_bytes;

    // A message that carries an inline file of 2 MB: its base64 text is
    // 2.7 MB, and "A" repeated is valid base64.
    json params = message_params("with a file");
    params["message"]["parts"].push_back(
        json{{"kind", "file"},
             {"file", {{"bytes", std::string(2'700'000, 'A')},
                       {"mimeType", "application/octet-stream"},
                       {"name", "data.bin"}}}});
    const auto with_file = post_rpc(client, rpc_body("message/send", params));
    EXPECT_EQ(with_file.status, 200) << with_file.body.substr(0, 200);
    EXPECT_NE(with_file.body.find("completed"), std::string::npos) << with_file.body.substr(0, 200);

    // A body of exactly the limit is read, and one byte more is refused.
    const auto padded = [&](std::size_t size) {
        auto       body = send_body("");
        const auto fill = size - body.size();
        return send_body(std::string(fill, 'x'));
    };
    EXPECT_EQ(padded(limit).size(), limit);
    EXPECT_EQ(post_rpc(client, padded(limit)).status, 200);
    EXPECT_EQ(post_rpc(client, padded(limit + 1)).status, 413);
}

TEST(A2AHttpLimits, ABodyOverAConfiguredLimitGets413OnEveryRoute) {
    constexpr std::size_t limit = 4096;
    RunningServer server("127.0.0.1", [](A2AServer& configured) {
        A2AHttpConfig config;
        config.max_payload_bytes = limit;
        configured.set_http_config(config);
    });
    httplib::Client client("127.0.0.1", server.port());

    const auto padded = [](const std::string& method, std::size_t size) {
        const auto empty = rpc_body(method, message_params(""));
        return rpc_body(method, message_params(std::string(size - empty.size(), 'x')));
    };
    for (const std::string method : {"message/send", "SendMessage", "message/stream",
                                     "SendStreamingMessage"}) {
        EXPECT_EQ(padded(method, limit).size(), limit);
        EXPECT_EQ(post_rpc(client, padded(method, limit)).status, 200) << method;
        EXPECT_EQ(post_rpc(client, padded(method, limit + 1)).status, 413) << method;
        EXPECT_EQ(post_rpc(client, padded(method, 64 * 1024)).status, 413) << method;
    }

    // A body without a Content-Length (chunked) is counted as it arrives.
    const std::string big(64 * 1024, 'x');
    std::size_t       sent = 0;
    const auto        chunked = client.Post(
        "/", httplib::Headers{},
        [&](size_t, httplib::DataSink& sink) {
            if (sent >= big.size()) {
                sink.done();
                return true;
            }
            const auto count = std::min<std::size_t>(1024, big.size() - sent);
            sink.write(big.data() + sent, count);
            sent += count;
            return true;
        },
        "application/json");
    ASSERT_TRUE(chunked);
    EXPECT_EQ(chunked->status, 413);

    // A Content-Length far over the limit with a part of the body: the
    // server answers once the limit is crossed, and does not wait for more.
    const auto reply = send_raw(server.port(),
                                "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                "Content-Length: 100000000\r\nConnection: close\r\n\r\n" +
                                    std::string(2 * limit, 'x'),
                                3s);
    EXPECT_EQ(reply.rfind("HTTP/1.1 413", 0), 0u) << reply;

    // The refusals left the server as it was.
    httplib::Client fresh("127.0.0.1", server.port());
    EXPECT_EQ(post_rpc(fresh, send_body()).status, 200);
    EXPECT_EQ(get_card(fresh).status, 200);
}

TEST(A2AHttpLimits, AForeignHostWithAHugeBodyIsRefusedBeforeTheBodyIsRead) {
    // The Host check comes first: the answer is 403, not the 413 that reading
    // the body would give.
    constexpr std::size_t limit = 4096;
    RunningServer server("127.0.0.1", [](A2AServer& configured) {
        A2AHttpConfig config;
        config.max_payload_bytes = limit;
        configured.set_http_config(config);
    });
    const auto reply = send_raw(server.port(),
                                "POST / HTTP/1.1\r\nHost: evil.example\r\nContent-Type: application/json\r\n"
                                "Content-Length: 100000000\r\nConnection: close\r\n\r\n" +
                                    std::string(2 * limit, 'x'),
                                3s);
    EXPECT_EQ(reply.rfind("HTTP/1.1 403", 0), 0u) << reply;
}
