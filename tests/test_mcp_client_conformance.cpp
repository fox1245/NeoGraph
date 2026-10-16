// NeoGraph's MCP client against the MCP 2025-11-25 Streamable HTTP transport
// rules the official servers rely on (protocol audit, MCP-C-01 ... MCP-C-20).
// Every server here is an in-process cpp-httplib mock that records the wire.
#include <gtest/gtest.h>

#include <neograph/async/endpoint.h>
#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/mcp/client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>

#include "../src/mcp/http_redirect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace std::chrono_literals;

namespace {

struct Guard {
    explicit Guard(httplib::Server& server)
        : svr(server), port(svr.bind_to_any_port("127.0.0.1")),
          thread([this] { svr.listen_after_bind(); }) {
        for (int i = 0; i < 400 && !svr.is_running(); ++i) std::this_thread::sleep_for(5ms);
    }
    ~Guard() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }
    std::string url(const std::string& target = "/mcp") const {
        return "http://127.0.0.1:" + std::to_string(port) + target;
    }
    httplib::Server& svr;
    int port;
    std::thread thread;
};

// One recorded exchange.
struct Seen {
    std::string method;    ///< HTTP method
    std::string path;
    std::string query;     ///< the tenant parameter, when any
    std::string session;   ///< Mcp-Session-Id
    std::string version;   ///< MCP-Protocol-Version
    std::string last_event_id;
    json body;
};

struct Recorder {
    std::mutex mu;
    std::condition_variable changed;
    std::vector<Seen> seen;

    void add(const httplib::Request& req) {
        Seen item;
        item.method = req.method;
        item.path = req.path;
        item.query = req.get_param_value("tenant");
        item.session = req.get_header_value("Mcp-Session-Id");
        item.version = req.get_header_value("MCP-Protocol-Version");
        item.last_event_id = req.get_header_value("Last-Event-ID");
        try {
            item.body = req.body.empty() ? json() : json::parse(req.body);
        } catch (...) {
        }
        std::lock_guard lock(mu);
        seen.push_back(std::move(item));
        changed.notify_all();
    }
    std::vector<Seen> snapshot() {
        std::lock_guard lock(mu);
        return seen;
    }
    std::vector<Seen> with_method(const std::string& rpc_method) {
        std::vector<Seen> out;
        for (auto& item : snapshot())
            if (item.body.is_object() && item.body.value("method", "") == rpc_method)
                out.push_back(item);
        return out;
    }
    std::vector<Seen> http(const std::string& method) {
        std::vector<Seen> out;
        for (auto& item : snapshot())
            if (item.method == method) out.push_back(item);
        return out;
    }
};

json initialize_result(bool list_changed = false) {
    json capabilities = {{"tools", {{"listChanged", list_changed}}}};
    return {{"protocolVersion", "2025-11-25"},
            {"capabilities", capabilities},
            {"serverInfo", {{"name", "conformance"}, {"version", "1"}}}};
}

std::string envelope(const json& id, const json& result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}

json text_result(const std::string& text) {
    return {{"content", json::array({{{"type", "text"}, {"text", text}}})}};
}

// The handshake and the client's replies, common to every mock: initialize
// with an optional session id, 202 for notifications and responses.
bool common(const json& request, httplib::Response& res, const std::string& session = {}) {
    if (!request.contains("method")) {  // a response to a server request
        res.status = 202;
        return true;
    }
    const auto method = request.value("method", std::string{});
    if (!request.contains("id")) {
        res.status = 202;
        return true;
    }
    if (method == "initialize") {
        if (!session.empty()) res.set_header("Mcp-Session-Id", session);
        res.set_content(envelope(request["id"], initialize_result()), "application/json");
        return true;
    }
    return false;
}

}  // namespace

// MCP-C-01: an SSE response with notifications before the response, CRLF
// line ends as the official Python servers write them.
TEST(MCPClientConformance, CrlfEventStreamWithNotificationsBeforeTheResponse) {
    httplib::Server svr;
    svr.Post("/mcp", [](const httplib::Request& req, httplib::Response& res) {
        const auto request = json::parse(req.body);
        if (common(request, res)) return;
        std::string body;
        for (int step = 0; step < 3; ++step) {
            body += "event: message\r\ndata: " +
                    json{{"jsonrpc", "2.0"}, {"method", "notifications/message"},
                         {"params", {{"level", "info"}, {"data", {{"step", step}}}}}}
                        .dump() +
                    "\r\n\r\n";
        }
        body += "event: message\r\ndata: " + envelope(request["id"], text_result("answered")) +
                "\r\n\r\n";
        res.set_content(body, "text/event-stream");
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    auto result = client.call_tool_result("log_then_answer", json::object());
    EXPECT_EQ(result.content[0]["text"], "answered");
}

// MCP-C-02, MCP-C-06: server requests on a response stream are answered while
// it stays open, and a server request that reuses the pending call's id is
// still a request, not that call's response.
TEST(MCPClientConformance, ServerRequestsOnAResponseStreamAreAnswered) {
    Recorder recorder;
    std::mutex mu;
    std::condition_variable replied;
    std::vector<json> replies;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (!request.contains("method")) {
            {
                std::lock_guard lock(mu);
                replies.push_back(request);
            }
            replied.notify_all();
            res.status = 202;
            return;
        }
        if (common(request, res)) return;
        const json call_id = request["id"];
        auto step = std::make_shared<int>(0);
        res.set_chunked_content_provider(
            "text/event-stream",
            [&, call_id, step](std::size_t, httplib::DataSink& sink) {
                if (*step == 0) {
                    *step = 1;
                    const std::string events =
                        "data: " + json{{"jsonrpc", "2.0"}, {"id", call_id}, {"method", "ping"}}.dump() +
                        "\n\ndata: " +
                        json{{"jsonrpc", "2.0"}, {"id", "r-1"}, {"method", "roots/list"}}.dump() +
                        "\n\n";
                    sink.write(events.data(), events.size());
                    return true;
                }
                std::unique_lock lock(mu);
                replied.wait_for(lock, 5s, [&] { return replies.size() >= 2; });
                const std::string answer =
                    "data: " + envelope(call_id, text_result("after " + std::to_string(replies.size()) +
                                                             " replies")) +
                    "\n\n";
                lock.unlock();
                sink.write(answer.data(), answer.size());
                sink.done();
                return true;
            });
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    auto result = client.call_tool_result("ping_client", json::object());
    EXPECT_EQ(result.content[0]["text"], "after 2 replies");
    std::lock_guard lock(mu);
    ASSERT_EQ(replies.size(), 2u);
    for (const auto& reply : replies) {
        if (reply["id"] == json("r-1")) {
            EXPECT_EQ(reply["error"]["code"], -32601) << reply.dump();
        } else {
            EXPECT_EQ(reply["result"], json::object()) << reply.dump();
        }
    }
}

// MCP-C-04: the configured endpoint URL is used as given, path and query.
TEST(MCPClientConformance, EndpointUrlIsUsedAsGiven) {
    for (const std::string target : {"/", "/mcp/", "/v1/rpc?tenant=a"}) {
        Recorder recorder;
        httplib::Server svr;
        const auto path = target.substr(0, target.find('?'));
        svr.Post(path, [&](const httplib::Request& req, httplib::Response& res) {
            recorder.add(req);
            const auto request = json::parse(req.body);
            if (common(request, res)) return;
            res.set_content(envelope(request["id"], text_result("ok")), "application/json");
        });
        Guard server(svr);
        mcp::MCPClient client(server.url(target));
        EXPECT_EQ(client.call_tool_result("echo", json::object()).content[0]["text"], "ok") << target;
        for (const auto& item : recorder.snapshot()) {
            EXPECT_EQ(item.path, path) << target;
            if (target.find('?') != std::string::npos) EXPECT_EQ(item.query, "a");
        }
        EXPECT_EQ(recorder.snapshot().size(), 3u) << target;
    }
    EXPECT_THROW(mcp::MCPClient("http://127.0.0.1:1/mcp#fragment"), std::invalid_argument);
}

// MCP-C-03: HTTP 404 for a request with Mcp-Session-Id starts a new session
// (initialize without a session id) and sends the request again, once.
TEST(MCPClientConformance, AnExpiredSessionIsReplacedAndTheRequestSentAgain) {
    Recorder recorder;
    std::atomic<int> sessions{0};
    std::atomic<bool> forget_all{false};
    std::mutex mu;
    std::string live;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        const auto session = req.get_header_value("Mcp-Session-Id");
        if (request.value("method", "") == "initialize") {
            const auto id = "s" + std::to_string(++sessions);
            {
                std::lock_guard lock(mu);
                live = id;
            }
            common(request, res, id);
            return;
        }
        bool known;
        {
            std::lock_guard lock(mu);
            known = !forget_all && session == live;
        }
        if (!known) {
            res.status = 404;
            res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32600,"message":"Session not found"}})",
                            "application/json");
            return;
        }
        if (common(request, res)) return;
        res.set_content(envelope(request["id"], text_result("in " + session)), "application/json");
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    EXPECT_EQ(client.call_tool_result("echo", json::object()).content[0]["text"], "in s1");
    {
        std::lock_guard lock(mu);
        live = "restarted";  // the server lost s1, as after a restart or idle expiry
    }
    EXPECT_EQ(client.call_tool_result("echo", json::object()).content[0]["text"], "in s2");
    const auto initializes = recorder.with_method("initialize");
    ASSERT_EQ(initializes.size(), 2u);
    EXPECT_TRUE(initializes[1].session.empty());
    EXPECT_EQ(recorder.with_method("tools/call").size(), 3u);

    // A server that refuses the new session too: one retry, then the error.
    forget_all = true;
    try {
        client.call_tool_result("echo", json::object());
        FAIL() << "a server that keeps answering 404 was accepted";
    } catch (const mcp::MCPTransportError& error) {
        EXPECT_EQ(error.http_status(), 404);
    }
    EXPECT_EQ(recorder.with_method("initialize").size(), 3u);
}

// MCP-C-07: a stream that ends before the response is resumed with GET and
// Last-Event-ID after the stream's retry interval.
TEST(MCPClientConformance, AStreamTheServerClosedEarlyIsResumed) {
    Recorder recorder;
    std::mutex mu;
    json pending_id;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (common(request, res, "resumable")) return;
        {
            std::lock_guard lock(mu);
            pending_id = request["id"];
        }
        // A priming event with an id and a retry, then the stream closes.
        res.set_content("id: e1\nretry: 120\ndata:\n\n", "text/event-stream");
    });
    svr.Get("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        if (req.get_header_value("Last-Event-ID").empty()) {
            res.status = 405;  // no stream for messages between calls
            return;
        }
        recorder.add(req);
        json id;
        {
            std::lock_guard lock(mu);
            id = pending_id;
        }
        res.set_content("id: e2\ndata: " + envelope(id, text_result("resumed")) + "\n\n",
                        "text/event-stream");
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    client.initialize();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(client.call_tool_result("poll_then_answer", json::object()).content[0]["text"],
              "resumed");
    EXPECT_GE(std::chrono::steady_clock::now() - started, 100ms);  // retry respected
    const auto gets = recorder.http("GET");
    ASSERT_EQ(gets.size(), 1u);
    EXPECT_EQ(gets[0].last_event_id, "e1");
    EXPECT_EQ(gets[0].session, "resumable");
}

// MCP-C-10: the client ends its session with DELETE on shutdown() and when
// its last reference goes.
TEST(MCPClientConformance, TheSessionEndsWithDelete) {
    Recorder recorder;
    std::atomic<int> sessions{0};
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        common(request, res, "d" + std::to_string(++sessions));
    });
    svr.Delete("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 204;
    });
    Guard server(svr);
    {
        mcp::MCPClient released(server.url());
        released.initialize();
    }
    mcp::MCPClient closed(server.url());
    closed.initialize();
    closed.shutdown();
    const auto deletes = recorder.http("DELETE");
    ASSERT_EQ(deletes.size(), 2u);
    EXPECT_EQ(deletes[0].session, "d1");
    EXPECT_EQ(deletes[1].session, "d3");
    EXPECT_EQ(deletes[0].version, "2025-11-25");
}

// After initialize the client opens the optional GET event stream, as the
// official TypeScript client does. A server without one answers 405 (MCP
// transports, "Listening for Messages"): that means no stream, not a
// failure. The client asks once, keeps its session and its calls work.
TEST(MCPClientConformance, AServerWithoutTheGetStreamIsNotAnError) {
    Recorder recorder;
    std::atomic<int> sessions{0};
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (common(request, res, "g" + std::to_string(++sessions))) return;
        if (request.value("method", "") == "tools/list") {
            res.set_content(envelope(request["id"],
                                     {{"tools", json::array({{{"name", "echo"},
                                                              {"inputSchema", {{"type", "object"}}}}})}}),
                            "application/json");
            return;
        }
        res.set_content(envelope(request["id"], text_result("ok")), "application/json");
    });
    svr.Get("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 405;
        res.set_header("Allow", "POST, DELETE");
    });
    svr.Delete("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 204;
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    std::atomic<int> list_changes{0};
    client.set_tool_list_changed_handler([&] { ++list_changes; });
    ASSERT_TRUE(client.initialize("no-stream"));
    {
        std::unique_lock lock(recorder.mu);
        ASSERT_TRUE(recorder.changed.wait_for(lock, 5s, [&] {
            for (const auto& item : recorder.seen)
                if (item.method == "GET") return true;
            return false;
        }));
    }
    std::this_thread::sleep_for(300ms);  // a retry would come within this
    EXPECT_EQ(client.get_tool_definitions().size(), 1u);
    for (int i = 0; i < 2; ++i) {
        const auto result = client.call_tool_result("echo", json::object());
        EXPECT_FALSE(result.is_error);
        EXPECT_EQ(result.content[0]["text"], "ok");
    }
    EXPECT_TRUE(client.is_initialized());
    const auto gets = recorder.http("GET");
    ASSERT_EQ(gets.size(), 1u) << "the client asked again after 405";
    EXPECT_EQ(gets[0].session, "g1");
    EXPECT_EQ(recorder.with_method("initialize").size(), 1u);
    for (const auto& call : recorder.with_method("tools/call")) EXPECT_EQ(call.session, "g1");
    EXPECT_EQ(list_changes.load(), 0);
}

// MCP 2025-03-26 lets a server send a JSON-RPC batch: in an event of a
// response stream, or as the JSON body. Each message in it is handled as if
// it came alone, and the replies to its requests go back as one array.
TEST(MCPClientConformance, BatchesFromTheServerAreHandledMessageByMessage) {
    Recorder recorder;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (request.is_array() || !request.contains("method") || !request.contains("id")) {
            res.status = 202;
            return;
        }
        const auto method = request.value("method", std::string{});
        if (method == "initialize") {
            res.set_header("Mcp-Session-Id", "b1");
            auto result = initialize_result();
            result["protocolVersion"] = "2025-03-26";
            res.set_content(envelope(request["id"], result), "application/json");
            return;
        }
        const json answer = json::parse(envelope(request["id"], text_result("ok")));
        if (request["params"]["arguments"].value("mode", "") == "sse") {
            const json ping = {{"jsonrpc", "2.0"}, {"id", "s1"}, {"method", "ping"}};
            res.set_content("data: " + json::array({ping, answer}).dump() + "\n\n",
                            "text/event-stream");
        } else {
            res.set_content(json::array({answer}).dump(), "application/json");
        }
    });
    svr.Get("/mcp", [&](const httplib::Request&, httplib::Response& res) { res.status = 405; });
    svr.Delete("/mcp", [&](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    ASSERT_TRUE(client.initialize("batch"));
    EXPECT_EQ(client.call_tool_result("echo", {{"mode", "sse"}}).content[0]["text"], "ok");
    EXPECT_EQ(client.call_tool_result("echo", {{"mode", "json"}}).content[0]["text"], "ok");
    // The ping in the batch was answered, in an array.
    std::unique_lock lock(recorder.mu);
    const bool answered = recorder.changed.wait_for(lock, 5s, [&] {
        for (const auto& item : recorder.seen) {
            if (!item.body.is_array()) continue;
            for (std::size_t i = 0; i < item.body.size(); ++i) {
                const json reply = item.body[i];
                if (reply.value("id", json()) == json("s1") && reply.contains("result")) return true;
            }
        }
        return false;
    });
    EXPECT_TRUE(answered);
}

// A request that runs out of time while its response is still arriving fails
// as a timeout, and the server is told with notifications/cancelled, as on
// every other timeout path. It never fails as a malformed response built from
// the part that arrived. The mock server writes the first half of the
// response and then holds the rest back, in a JSON body and in an event
// stream, with and without a session, under the client's request timeout and
// under the call's own deadline.
namespace {

// The second half of a response waits here until the test lets it go.
struct HeldBack {
    std::atomic<bool> release{false};
    std::atomic<bool> first_half_sent{false};
    void wait() const {
        for (int i = 0; i < 600 && !release.load(); ++i) std::this_thread::sleep_for(5ms);
    }
};

// Answers with `full`, cut in the middle: a JSON body of the right length or
// a chunked event stream, as the mock for the stalled call.
void respond_in_halves(httplib::Response& res, std::string full, bool event_stream,
                       const std::shared_ptr<HeldBack>& held) {
    const std::size_t half = full.size() / 2;
    if (event_stream) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [full = std::move(full), half, held, step = 0](std::size_t,
                                                           httplib::DataSink& sink) mutable {
                if (step++ == 0) {
                    sink.write(full.data(), half);
                    held->first_half_sent = true;
                    return true;
                }
                held->wait();
                sink.write(full.data() + half, full.size() - half);
                sink.done();
                return true;
            });
        return;
    }
    const std::size_t size = full.size();
    res.set_content_provider(
        size, "application/json",
        [full = std::move(full), half, held](std::size_t offset, std::size_t,
                                             httplib::DataSink& sink) {
            if (offset == 0) {
                sink.write(full.data(), half);
                held->first_half_sent = true;
                return true;
            }
            held->wait();
            sink.write(full.data() + half, full.size() - half);
            return true;
        });
}

}  // namespace

TEST(MCPClientConformance, ATimeoutWhileTheResponseArrivesIsATimeout) {
    struct Mode {
        const char* name;
        bool event_stream;
        bool stateful;
        bool call_deadline;  ///< the call's own deadline, not request_timeout
    };
    for (const Mode mode : {Mode{"json", false, true, false}, Mode{"sse", true, true, false},
                            Mode{"stateless-sse", true, false, false},
                            Mode{"stateless-json", false, false, false},
                            Mode{"json-deadline", false, true, true},
                            Mode{"sse-deadline", true, true, true}}) {
        SCOPED_TRACE(mode.name);
        Recorder recorder;
        auto held = std::make_shared<HeldBack>();
        httplib::Server svr;
        svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
            recorder.add(req);
            const auto request = json::parse(req.body);
            if (common(request, res, mode.stateful ? "t1" : "")) return;
            const json id = request["id"];
            const std::string text =
                request["params"]["arguments"].value("mode", "") == "stall" ? "late" : "fine";
            if (text == "fine") {
                res.set_content(envelope(id, text_result(text)), "application/json");
                return;
            }
            const auto body = envelope(id, text_result(text));
            respond_in_halves(res, mode.event_stream ? "data: " + body + "\n\n" : body,
                              mode.event_stream, held);
        });
        svr.Get("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 405; });
        svr.Delete("/mcp",
                   [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
        Guard server(svr);
        mcp::MCPClientConfig config;
        config.request_timeout = mode.call_deadline ? 30s : 300ms;
        mcp::MCPClient client(server.url(), config);
        ASSERT_TRUE(client.initialize("timeout"));
        const auto started = std::chrono::steady_clock::now();
        try {
            if (mode.call_deadline) {
                async::run_sync(client.call_tool_result_async(
                    "slow", {{"mode", "stall"}}, std::chrono::steady_clock::now() + 300ms,
                    nullptr));
            } else {
                client.call_tool_result("slow", {{"mode", "stall"}});
            }
            ADD_FAILURE() << "the stalled call returned";
        } catch (const mcp::MCPTransportError& error) {
            EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout) << error.what();
        } catch (const std::exception& error) {
            ADD_FAILURE() << "not a transport timeout: " << error.what();
        }
        // It gave up at its own bound, long before the held-back half could arrive.
        EXPECT_LT(std::chrono::steady_clock::now() - started, 2500ms);
        // The server did start the response, so the client stopped waiting in
        // the middle of it, not before it.
        EXPECT_TRUE(held->first_half_sent.load());
        held->release = true;
        // The server is told, and the session goes on.
        int stalled_id = -1;
        for (const auto& call : recorder.with_method("tools/call"))
            if (call.body["params"]["arguments"].value("mode", "") == "stall")
                stalled_id = call.body["id"].get<int>();
        {
            std::unique_lock lock(recorder.mu);
            EXPECT_TRUE(recorder.changed.wait_for(lock, 10s, [&] {
                for (const auto& item : recorder.seen)
                    if (item.body.is_object() &&
                        item.body.value("method", "") == "notifications/cancelled" &&
                        item.body["params"]["requestId"] == stalled_id)
                        return true;
                return false;
            })) << "no notifications/cancelled for request " << stalled_id;
        }
        EXPECT_EQ(client.call_tool_result("echo", json::object()).content[0]["text"], "fine");
    }
}

TEST(MCPClientConformance, InitializeThatTimesOutInTheMiddleOfItsResponseIsATimeout) {
    // initialize itself, cut off in the middle of its response, fails as a
    // timeout; MCP never cancels initialize, so nothing is sent.
    for (const bool event_stream : {false, true}) {
        SCOPED_TRACE(event_stream ? "sse" : "json");
        Recorder recorder;
        auto held = std::make_shared<HeldBack>();
        httplib::Server svr;
        svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
            recorder.add(req);
            const auto request = json::parse(req.body);
            if (!request.contains("id")) {
                res.status = 202;
                return;
            }
            const auto body = envelope(request["id"], initialize_result());
            respond_in_halves(res, event_stream ? "data: " + body + "\n\n" : body,
                              event_stream, held);
        });
        Guard server(svr);
        mcp::MCPClientConfig config;
        config.request_timeout = 300ms;
        mcp::MCPClient client(server.url(), config);
        try {
            client.initialize("timeout");
            ADD_FAILURE() << "initialize returned";
        } catch (const mcp::MCPTransportError& error) {
            EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout) << error.what();
        }
        EXPECT_TRUE(held->first_half_sent.load());
        held->release = true;
        EXPECT_TRUE(recorder.with_method("notifications/cancelled").empty());
        EXPECT_FALSE(client.is_initialized());
    }
}

// MCP-C-08: the URL tool constructor keeps one session for all its calls and
// ends it with the tool.
TEST(MCPClientConformance, TheUrlToolKeepsOneSession) {
    Recorder recorder;
    std::atomic<int> sessions{0};
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (common(request, res, "t" + std::to_string(++sessions))) return;
        res.set_content(envelope(request["id"], text_result("ok")), "application/json");
    });
    svr.Delete("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 204;
    });
    Guard server(svr);
    {
        mcp::MCPTool tool(server.url(), "echo", "Echo", {{"type", "object"}});
        for (int i = 0; i < 3; ++i) EXPECT_EQ(tool.execute_result(json::object()).content[0]["text"], "ok");
    }
    EXPECT_EQ(recorder.with_method("initialize").size(), 1u);
    for (const auto& call : recorder.with_method("tools/call")) EXPECT_EQ(call.session, "t1");
    const auto deletes = recorder.http("DELETE");
    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes[0].session, "t1");
}

// MCP-C-13: a JSON-RPC error in an HTTP error body keeps its code and data.
TEST(MCPClientConformance, AJsonRpcErrorInAnHttpErrorBodyKeepsItsCode) {
    httplib::Server svr;
    svr.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
        res.status = 400;
        res.set_content(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32022,
            "message":"Unsupported protocol version: 2025-11-25",
            "data":{"supported":["2026-07-28"],"requested":"2025-11-25"}}})",
                        "application/json");
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    try {
        client.initialize();
        FAIL() << "accepted an error";
    } catch (const mcp::MCPError& error) {
        EXPECT_EQ(error.code(), -32022);
        EXPECT_EQ(error.http_status(), 400);
        EXPECT_EQ(error.data()["supported"][0], "2026-07-28");
    }
}

// MCP-C-15, MCP-C-20: a request sent with rpc_call_async on a fresh client
// shakes hands first, and clientInfo names the library's version.
TEST(MCPClientConformance, RequestsComeAfterTheHandshake) {
    Recorder recorder;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (common(request, res)) return;
        res.set_content(envelope(request["id"], json::object()), "application/json");
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    async::run_sync(client.rpc_call_async("hooks/invoke", json::object()));
    const auto seen = recorder.snapshot();
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0].body["method"], "initialize");
    EXPECT_EQ(seen[1].body["method"], "notifications/initialized");
    EXPECT_EQ(seen[2].body["method"], "hooks/invoke");
    const auto version = seen[0].body["params"]["clientInfo"]["version"].get<std::string>();
    EXPECT_FALSE(version.empty());
    EXPECT_NE(version, "0.1.0");
}

// A bounded handshake stops waiting; MCP never cancels initialize on the wire.
TEST(MCPClientConformance, ABoundedInitializeStopsWaiting) {
    Recorder recorder;
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        std::this_thread::sleep_for(800ms);
        common(json::parse(req.body), res);
    });
    Guard server(svr);
    {
        mcp::MCPClient client(server.url());
        const auto started = std::chrono::steady_clock::now();
        try {
            client.initialize("bounded", std::chrono::steady_clock::now() + 100ms, {});
            FAIL() << "the deadline was ignored";
        } catch (const mcp::MCPTransportError& error) {
            EXPECT_EQ(error.failure(), mcp::MCPFailure::timeout);
        }
        EXPECT_LT(std::chrono::steady_clock::now() - started, 600ms);
        EXPECT_FALSE(client.is_initialized());
    }
    {
        mcp::MCPClient client(server.url());
        auto cancel = std::make_shared<graph::CancelToken>();
        std::thread stop([cancel] {
            std::this_thread::sleep_for(50ms);
            cancel->cancel();
        });
        try {
            client.initialize("bounded", std::chrono::steady_clock::time_point::max(), cancel);
            FAIL() << "the cancellation was ignored";
        } catch (const mcp::MCPTransportError& error) {
            EXPECT_EQ(error.failure(), mcp::MCPFailure::cancelled);
        }
        stop.join();
    }
    std::this_thread::sleep_for(900ms);
    EXPECT_TRUE(recorder.with_method("notifications/cancelled").empty());
}

// tools/list_changed reaches the handler, on a response stream and on the
// optional GET stream the client opens when the server declares listChanged.
TEST(MCPClientConformance, ToolListChangedReachesTheHandler) {
    std::atomic<int> gets{0};
    httplib::Server svr;
    const std::string changed =
        "data: " + json{{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}}.dump() +
        "\n\n";
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        const auto request = json::parse(req.body);
        if (request.value("method", "") == "initialize") {
            res.set_header("Mcp-Session-Id", "listening");
            res.set_content(envelope(request["id"], initialize_result(true)), "application/json");
            return;
        }
        if (common(request, res)) return;
        res.set_content(changed + "data: " + envelope(request["id"], text_result("ok")) + "\n\n",
                        "text/event-stream");
    });
    svr.Get("/mcp", [&](const httplib::Request&, httplib::Response& res) {
        if (gets.fetch_add(1) == 0) {
            res.set_content(changed, "text/event-stream");
        } else {
            res.status = 405;  // no more streams
        }
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    std::atomic<int> notified{0};
    client.set_tool_list_changed_handler([&] { notified.fetch_add(1); });
    client.initialize();
    for (int i = 0; i < 400 && notified.load() < 1; ++i) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(notified.load(), 1);  // from the GET stream
    EXPECT_GE(gets.load(), 1);
    client.call_tool_result("echo", json::object());
    EXPECT_EQ(notified.load(), 2);  // from the call's response stream
    client.set_tool_list_changed_handler({});
    client.call_tool_result("echo", json::object());
    EXPECT_EQ(notified.load(), 2);
}

// async_request_stream reports the status and headers of any method before
// its body.
namespace {
struct StreamProbe {
    std::vector<std::string> order;
    int status = 0;
    std::string accept;
    int delete_status = 0;
    std::string deleted;
};

asio::awaitable<void> probe_streamed_requests(int port, StreamProbe& probe) {
    // Named locals rather than braced temporaries in the co_await calls,
    // which GCC 13 cannot compile inside a coroutine.
    auto ex = co_await asio::this_coro::executor;
    const std::string host = "127.0.0.1";
    const std::string port_text = std::to_string(port);
    std::vector<std::pair<std::string, std::string>> accept_events;
    accept_events.emplace_back("Accept", "text/event-stream");
    std::function<void(const async::HttpResponseHead&)> on_get_head =
        [&probe](const async::HttpResponseHead& head) {
            probe.order.push_back("head");
            probe.status = head.status;
            probe.accept = std::string(head.get_header("x-seen-accept"));
        };
    std::function<void(std::string_view)> on_chunk = [&probe](std::string_view chunk) {
        probe.order.push_back("body:" + std::string(chunk));
    };
    auto response = co_await async::async_request_stream(
        ex, "GET", host, port_text, "/events", "", std::move(accept_events), false,
        std::move(on_get_head), std::move(on_chunk));
    probe.status = response.status;
    std::function<void(const async::HttpResponseHead&)> on_delete_head =
        [&probe](const async::HttpResponseHead& head) {
            probe.deleted = std::string(head.get_header("X-Deleted"));
        };
    std::function<void(std::string_view)> ignore_body;
    std::vector<std::pair<std::string, std::string>> no_headers;
    auto removed = co_await async::async_request_stream(
        ex, "DELETE", host, port_text, "/events", "", std::move(no_headers), false,
        std::move(on_delete_head), std::move(ignore_body));
    probe.delete_status = removed.status;
}
}  // namespace

TEST(MCPClientConformance, AStreamedRequestReportsItsHeadFirst) {
    httplib::Server svr;
    svr.Get("/events", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("X-Seen-Accept", req.get_header_value("Accept"));
        res.set_content("data: one\n\n", "text/event-stream");
    });
    svr.Delete("/events", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
        res.set_header("X-Deleted", "yes");
    });
    Guard server(svr);
    StreamProbe probe;
    async::run_sync(probe_streamed_requests(server.port, probe));
    ASSERT_EQ(probe.order.size(), 2u);
    EXPECT_EQ(probe.order[0], "head");
    EXPECT_EQ(probe.order[1], "body:data: one\n\n");
    EXPECT_EQ(probe.status, 200);
    EXPECT_EQ(probe.accept, "text/event-stream");
    EXPECT_EQ(probe.delete_status, 204);
    EXPECT_EQ(probe.deleted, "yes");
}

// The GET event stream may stay open as long as the session does (MCP
// transports, "Listening for Messages from the Server"). shutdown() and the
// destructor end it whether or not the server closes it on DELETE, and
// whatever the DELETE answer is (#448).
TEST(MCPClientConformance, ShutdownEndsAGetStreamTheServerKeepsOpen) {
    for (const int delete_status : {200, 405}) {
        for (const bool by_destructor : {false, true}) {
            SCOPED_TRACE("DELETE " + std::to_string(delete_status) +
                         (by_destructor ? ", destructor" : ", shutdown()"));
            std::atomic<bool> released{false};
            std::atomic<int> streams{0};
            httplib::Server svr;
            svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
                const auto request = json::parse(req.body);
                if (common(request, res, "held")) return;
                res.set_content(envelope(request["id"], text_result("ok")), "application/json");
            });
            svr.Get("/mcp", [&](const httplib::Request&, httplib::Response& res) {
                streams.fetch_add(1);
                const auto opened = std::chrono::steady_clock::now();
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [&released, opened](std::size_t, httplib::DataSink& sink) {
                        // Open until the test ends, with a comment now and
                        // then; bounded, so a client that hangs fails the test.
                        if (released.load() || std::chrono::steady_clock::now() - opened > 8s) {
                            sink.done();
                            return true;
                        }
                        std::this_thread::sleep_for(50ms);
                        const std::string comment = ": open\n\n";
                        return sink.write(comment.data(), comment.size());
                    });
            });
            svr.Delete("/mcp", [&](const httplib::Request&, httplib::Response& res) {
                res.status = delete_status;  // the GET stream stays open either way
            });
            Guard server(svr);
            auto client = std::make_unique<mcp::MCPClient>(server.url());
            ASSERT_TRUE(client->initialize("held-stream"));
            for (int i = 0; i < 400 && streams.load() < 1; ++i) std::this_thread::sleep_for(5ms);
            ASSERT_EQ(streams.load(), 1);
            const auto started = std::chrono::steady_clock::now();
            if (by_destructor) {
                client.reset();
            } else {
                client->shutdown();
            }
            EXPECT_LT(std::chrono::steady_clock::now() - started, 4s);
            client.reset();
            released.store(true);
        }
    }
}

// An ended GET stream is reopened after the `retry` it named. shutdown() does
// not wait for that pause (#448).
TEST(MCPClientConformance, ShutdownDoesNotWaitForTheReconnectPause) {
    std::atomic<int> streams{0};
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        const auto request = json::parse(req.body);
        if (common(request, res, "paused")) return;
        res.set_content(envelope(request["id"], text_result("ok")), "application/json");
    });
    svr.Get("/mcp", [&](const httplib::Request&, httplib::Response& res) {
        streams.fetch_add(1);
        // A priming event that asks for a long pause, then the stream ends.
        res.set_content("id: p1\nretry: 6000\ndata: \n\n", "text/event-stream");
    });
    svr.Delete("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    ASSERT_TRUE(client.initialize("paused-stream"));
    for (int i = 0; i < 400 && streams.load() < 1; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(streams.load(), 1);
    std::this_thread::sleep_for(200ms);  // the stream has ended: the client pauses
    const auto started = std::chrono::steady_clock::now();
    client.shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
    EXPECT_EQ(streams.load(), 1);
}

// Node and uvicorn write each event of a response stream as one HTTP chunk.
// An event over 1 MiB is read like a smaller one, up to the event limit (#449).
TEST(MCPClientConformance, AnEventOverOneMebibyteInOneChunk) {
    const std::string large(2 * 1024 * 1024, 'x');
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        const auto request = json::parse(req.body);
        if (common(request, res)) return;
        const json call_id = request["id"];
        res.set_chunked_content_provider(
            "text/event-stream", [&large, call_id](std::size_t, httplib::DataSink& sink) {
                const auto event = "data: " + envelope(call_id, text_result(large)) + "\n\n";
                sink.write(event.data(), event.size());  // one HTTP chunk
                sink.done();
                return true;
            });
    });
    Guard server(svr);
    mcp::MCPClient client(server.url());
    const auto result = client.call_tool_result("large", json::object());
    ASSERT_EQ(result.content.size(), 1u);
    EXPECT_EQ(result.content[0]["text"].get<std::string>(), large);
}

// FastMCP servers on mcp 1.x answer POST /mcp with 307 to /mcp/. The client
// follows 307 and 308 on the same origin, absolute or relative, with the
// method, body and MCP headers of the request (RFC 9110 15.4.8, 15.4.9;
// #449).
TEST(MCPClientConformance, RedirectsOnTheSameOriginAreFollowed) {
    Recorder recorder;
    std::atomic<int> port{0};
    httplib::Server svr;
    const auto moved = [&](int status, bool absolute) {
        return [&recorder, &port, status, absolute](const httplib::Request& req,
                                                    httplib::Response& res) {
            recorder.add(req);
            res.status = status;
            res.set_header("Location",
                           absolute ? "http://127.0.0.1:" + std::to_string(port.load()) + "/mcp/"
                                    : std::string("/mcp/"));
        };
    };
    svr.Post("/mcp", moved(307, true));  // as Starlette writes it
    svr.Get("/mcp", moved(308, false));
    svr.Delete("/mcp", moved(307, false));
    svr.Post("/mcp/", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        const auto request = json::parse(req.body);
        if (common(request, res, "moved")) return;
        res.set_content(envelope(request["id"], text_result("ok")), "application/json");
    });
    svr.Get("/mcp/", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 405;
    });
    svr.Delete("/mcp/", [&](const httplib::Request& req, httplib::Response& res) {
        recorder.add(req);
        res.status = 204;
    });
    Guard server(svr);
    port = server.port;
    {
        mcp::MCPClient client(server.url());
        EXPECT_EQ(client.call_tool_result("echo", json::object()).content[0]["text"], "ok");
        std::unique_lock lock(recorder.mu);
        EXPECT_TRUE(recorder.changed.wait_for(lock, 5s, [&] {
            for (const auto& item : recorder.seen)
                if (item.method == "GET" && item.path == "/mcp/") return true;
            return false;
        }));
    }
    std::vector<std::string> redirected;
    std::vector<std::string> answered;
    for (const auto& item : recorder.snapshot()) {
        if (item.method != "POST") continue;
        (item.path == "/mcp" ? redirected : answered).push_back(item.body.dump());
    }
    EXPECT_EQ(redirected.size(), 3u);  // initialize, initialized, tools/call
    EXPECT_EQ(answered, redirected);   // each sent again, body unchanged
    const auto calls = recorder.with_method("tools/call");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].path, "/mcp/");
    EXPECT_EQ(calls[1].session, "moved");
    EXPECT_EQ(calls[1].version, "2025-11-25");
    const auto deletes = recorder.http("DELETE");
    ASSERT_EQ(deletes.size(), 2u);
    EXPECT_EQ(deletes[1].path, "/mcp/");
    EXPECT_EQ(deletes[1].session, "moved");
}

// A redirect to another origin is not followed, as the request may carry
// credentials: nothing reaches the other server, not even the request line.
// Another port, another host name and another scheme are each another origin.
// A redirect loop ends after a few hops, and 301, 302 and 303 are not followed
// for a POST (#449).
TEST(MCPClientConformance, RedirectsToAnotherOriginOrInALoopAreNotFollowed) {
    std::atomic<int> reached_other{0};
    httplib::Server other;
    other.set_pre_routing_handler([&](const httplib::Request&, httplib::Response& res) {
        reached_other.fetch_add(1);
        res.status = 500;
        return httplib::Server::HandlerResponse::Handled;
    });
    Guard other_server(other);

    std::atomic<int> port{0};
    std::atomic<int> answered{0};
    std::atomic<int> loops{0};
    httplib::Server svr;
    const auto to = [&](int status, std::function<std::string()> location) {
        return [status, location](const httplib::Request&, httplib::Response& res) {
            res.status = status;
            res.set_header("Location", location());
        };
    };
    svr.Post("/other-port", to(307, [&] { return other_server.url(); }));
    svr.Post("/other-host",
             to(307, [&] { return "http://localhost:" + std::to_string(port.load()) + "/mcp"; }));
    svr.Post("/other-scheme",
             to(308, [&] { return "https://127.0.0.1:" + std::to_string(port.load()) + "/mcp"; }));
    svr.Post("/see-other", to(303, [] { return std::string("/mcp"); }));
    svr.Post("/found", to(302, [] { return std::string("/mcp"); }));
    svr.Post("/moved", to(301, [] { return std::string("/mcp"); }));
    svr.Post("/loop", [&](const httplib::Request&, httplib::Response& res) {
        loops.fetch_add(1);
        res.status = 308;
        res.set_header("Location", "loop");
    });
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        answered.fetch_add(1);
        common(json::parse(req.body), res);
    });
    Guard server(svr);
    port = server.port;
    const std::vector<std::pair<std::string, int>> cases = {
        {"/other-port", 307}, {"/other-host", 307}, {"/other-scheme", 308}, {"/see-other", 303},
        {"/found", 302},      {"/moved", 301},      {"/loop", 308}};
    for (const auto& [target, status] : cases) {
        SCOPED_TRACE(target);
        mcp::MCPClientConfig config;
        config.headers.emplace_back("Authorization", "Bearer must-not-leave-the-origin");
        mcp::MCPClient client(server.url(target), config);
        try {
            client.initialize("redirected");
            ADD_FAILURE() << "initialize succeeded";
        } catch (const mcp::MCPTransportError& error) {
            EXPECT_EQ(error.failure(), mcp::MCPFailure::http_status);
            EXPECT_EQ(error.http_status(), status);
            EXPECT_EQ(std::string(error.what()).find("must-not-leave"), std::string::npos);
        }
    }
    EXPECT_EQ(reached_other.load(), 0);
    EXPECT_EQ(answered.load(), 0);
    EXPECT_GE(loops.load(), 2);
    EXPECT_LE(loops.load(), 10);
}

// The origin check itself (RFC 6454): scheme, host and effective port. An
// https endpoint never follows a redirect to http, whatever the host (#449).
TEST(MCPClientConformance, RedirectTargetsStayOnTheEndpointOrigin) {
    using mcp::detail::same_origin_redirect_target;
    const auto https = async::split_async_endpoint("https://mcp.example.com/mcp");
    const auto http = async::split_async_endpoint("http://127.0.0.1:8000/mcp");

    EXPECT_FALSE(same_origin_redirect_target(https, "http://mcp.example.com/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "http://mcp.example.com:443/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(http, "https://127.0.0.1:8000/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "https://other.example.com/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "https://mcp.example.com:8443/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "//other.example.com/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "https://user@mcp.example.com/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "ftp://mcp.example.com/mcp/", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "/mcp/\r\nX-Injected: 1", "/mcp"));
    EXPECT_FALSE(same_origin_redirect_target(https, "", "/mcp"));

    EXPECT_EQ(same_origin_redirect_target(https, "https://MCP.example.com/mcp/?t=1#f", "/mcp"),
              "/mcp/?t=1");
    EXPECT_EQ(same_origin_redirect_target(https, "https://mcp.example.com:443/", "/mcp"), "/");
    EXPECT_EQ(same_origin_redirect_target(http, "http://127.0.0.1:8000", "/mcp"), "/");
    EXPECT_EQ(same_origin_redirect_target(http, "/mcp/", "/mcp?t=1"), "/mcp/");
    EXPECT_EQ(same_origin_redirect_target(http, "next", "/a/b?t=1"), "/a/next");
    EXPECT_EQ(same_origin_redirect_target(http, "?t=2", "/a/b?t=1"), "/a/b?t=2");
}
