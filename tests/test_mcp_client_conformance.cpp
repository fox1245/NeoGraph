// NeoGraph's MCP client against the MCP 2025-11-25 Streamable HTTP transport
// rules the official servers rely on (protocol audit, MCP-C-01 ... MCP-C-20).
// Every server here is an in-process cpp-httplib mock that records the wire.
#include <gtest/gtest.h>

#include <neograph/async/http_client.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/cancel.h>
#include <neograph/mcp/client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
