// Wire-protocol coverage for MCPClient HTTP transport after Stage 3 /
// Semester 2.6.
//
// MCPClient now drives the HTTP path through neograph::async::async_post
// behind both the existing sync rpc_call() and the new rpc_call_async().
// httplib::Client is gone from this file. These tests stand up a local
// httplib::Server speaking minimal MCP JSON-RPC and check that:
//   1. rpc_call_async() resolves to the parsed `result` field.
//   2. The sync rpc_call() returns the same shape via run_sync.
//   3. SSE-framed responses (event: message\ndata: {...}) are parsed.
//   4. JSON-RPC errors surface as runtime_error with the server's message.
//
// The MCP repo had no automated MCP tests prior to this — the examples
// covered the happy path manually. The async migration is the natural
// moment to add a regression net.

#include <gtest/gtest.h>
#include <neograph/mcp/client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace neograph;

namespace {

struct MockMcpServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;
    std::atomic<int> request_count{0};

    // Per-request control surface.
    std::string     content_type = "application/json";
    int             status = 200;
    std::string     body_template = R"({"jsonrpc":"2.0","id":%ID%,"result":{"echo":"%METHOD%"}})";

    MockMcpServer() {
        svr.Post("/mcp", [this](const httplib::Request& req,
                                httplib::Response& res) {
            request_count.fetch_add(1, std::memory_order_relaxed);
            res.status = status;
            // Pull id + method out of the request body and substitute
            // them into the response template — keeps the mock generic
            // across rpc_call_async/rpc_call without per-test handlers.
            int id = 0;
            std::string method;
            try {
                auto parsed = json::parse(req.body);
                if (parsed.is_object()) {
                    id = parsed.value("id", 0);
                    method = parsed.value("method", std::string{});
                }
            } catch (...) {
                // unparseable body -> id stays 0, method stays empty
            }
            std::string body = body_template;
            auto replace_all = [&](const std::string& from, const std::string& to) {
                size_t p = 0;
                while ((p = body.find(from, p)) != std::string::npos) {
                    body.replace(p, from.size(), to);
                    p += to.size();
                }
            };
            replace_all("%ID%", std::to_string(id));
            replace_all("%METHOD%", method);
            res.set_content(body, content_type.c_str());
        });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~MockMcpServer() {
        svr.stop();
        if (t.joinable()) t.join();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

} // namespace

TEST(MCPClientAsync, RpcCallAsyncReturnsResultField) {
    MockMcpServer mock;
    ASSERT_GT(mock.port, 0);

    mcp::MCPClient client(mock.url());

    asio::io_context io;
    json result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            result = co_await client.rpc_call_async("ping", json::object());
        },
        asio::detached);
    io.run();

    EXPECT_TRUE(result.is_object());
    EXPECT_EQ(result.value("echo", ""), "ping");
    EXPECT_EQ(mock.request_count.load(), 1);
}

TEST(MCPClientAsync, SyncRpcCallBridgesThroughAsync) {
    MockMcpServer mock;
    mcp::MCPClient client(mock.url());

    auto result = client.call_tool("greet", json{{"name", "world"}});

    EXPECT_TRUE(result.is_object());
    EXPECT_EQ(result.value("echo", ""), "tools/call");
    EXPECT_EQ(mock.request_count.load(), 1);
}

TEST(MCPClientAsync, ParsesSseFramedResponse) {
    MockMcpServer mock;
    mock.content_type = "text/event-stream";
    mock.body_template =
        "event: message\n"
        "data: {\"jsonrpc\":\"2.0\",\"id\":%ID%,\"result\":{\"sse\":true}}\n"
        "\n";

    mcp::MCPClient client(mock.url());

    auto result = client.call_tool("any", json::object());
    EXPECT_TRUE(result.is_object());
    EXPECT_EQ(result.value("sse", false), true);
}

TEST(MCPClientAsync, JsonRpcErrorSurfacesAsRuntimeError) {
    MockMcpServer mock;
    mock.body_template =
        R"({"jsonrpc":"2.0","id":%ID%,"error":{"code":-32601,"message":"method not found"}})";

    mcp::MCPClient client(mock.url());

    try {
        client.call_tool("missing", json::object());
        FAIL() << "expected runtime_error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("method not found"),
                  std::string::npos);
    }
}

TEST(MCPClientAsync, NonOkHttpStatusSurfacesAsRuntimeError) {
    MockMcpServer mock;
    mock.status = 500;
    mock.body_template = R"({"error":"server boom"})";

    mcp::MCPClient client(mock.url());

    EXPECT_THROW(client.call_tool("x", json::object()), std::runtime_error);
}

TEST(MCPClientAsync, SessionIdHeaderRoundTrips) {
    // Regression for the Mcp-Session-Id tracking that was lost in the
    // Sem 2.6 httplib → async_post migration and restored in the
    // follow-up commit. The server sends Mcp-Session-Id on the first
    // response; subsequent requests must echo it back verbatim, so
    // server-side session state stays routable.
    std::atomic<int> seen_session_echoes{0};
    std::string assigned_sid = "sess-abc123";

    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        // If the client already carries a session id, it must be the
        // one we issued. Count those echoes.
        auto client_sid = req.get_header_value("Mcp-Session-Id");
        if (!client_sid.empty()) {
            EXPECT_EQ(client_sid, assigned_sid);
            seen_session_echoes.fetch_add(1, std::memory_order_relaxed);
        }

        // Always reply with the assigned session id so the client's
        // tracking stays refreshed.
        res.set_header("Mcp-Session-Id", assigned_sid);
        int id = 0;
        try {
            auto parsed = json::parse(req.body);
            if (parsed.is_object()) id = parsed.value("id", 0);
        } catch (...) { }
        std::string body = R"({"jsonrpc":"2.0","id":)"
            + std::to_string(id) + R"(,"result":{"ok":true}})";
        res.set_content(body, "application/json");
    });

    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    for (int i = 0; i < 200 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(svr.is_running());

    mcp::MCPClient client("http://127.0.0.1:" + std::to_string(port));
    // First call: no session id yet — server assigns one.
    client.call_tool("first", json::object());
    // Subsequent calls: must carry the assigned id back.
    client.call_tool("second", json::object());
    client.call_tool("third",  json::object());

    svr.stop();
    if (t.joinable()) t.join();

    // 3 requests total, 2 of which should have echoed the session id
    // (the very first didn't have one yet).
    EXPECT_EQ(seen_session_echoes.load(), 2);
}
