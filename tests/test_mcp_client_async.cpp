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
#include <mutex>
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
            if (status == 200 && method == "initialize") {
                res.set_content(
                    R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                        R"(,"result":{"protocolVersion":"2025-11-25","capabilities":{"tools":{}},"serverInfo":{"name":"mock","version":"1"}}})",
                    "application/json");
                return;
            }
            if (status == 200 && method == "notifications/initialized") {
                res.status = 204;
                return;
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

struct ServerGuard {
    explicit ServerGuard(httplib::Server& server)
      : svr(server)
      , port(svr.bind_to_any_port("127.0.0.1"))
      , thread([this] { svr.listen_after_bind(); })
    {
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~ServerGuard() {
        svr.stop();
        if (thread.joinable()) thread.join();
    }

    httplib::Server& svr;
    int port;
    std::thread thread;
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
    EXPECT_EQ(mock.request_count.load(), 3);
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

TEST(MCPClientAsync, RejectsMismatchedJsonRpcResponseId) {
    MockMcpServer mock;
    mock.body_template =
        R"({"jsonrpc":"2.0","id":999999,"result":{"ok":true}})";

    mcp::MCPClient client(mock.url());
    try {
        client.call_tool("mismatched", json::object());
        FAIL() << "expected mismatched response id to fail";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("id does not match"),
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

TEST(MCPClientAsync, NotFoundDoesNotTerminateAndThrowsRuntimeError) {
    // Regression for issue #65: a 404 from the MCP HTTP path used to escape
    // the sync call as an exception that callers could (and an example
    // didn't) catch. Here we assert the library surfaces a *catchable*
    // std::runtime_error rather than aborting the process — and that the
    // message carries the request URL so a misconfigured path is obvious
    // (issue #66 friendly-error half).
    httplib::Server svr;
    // Register nothing on the MCP path → httplib answers 404 for any POST.
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    for (int i = 0; i < 200 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(svr.is_running());

    mcp::MCPClient client("http://127.0.0.1:" + std::to_string(port));

    bool threw_runtime_error = false;
    try {
        client.call_tool("x", json::object());
    } catch (const std::runtime_error& e) {
        threw_runtime_error = true;
        std::string what = e.what();
        // HTTP status and the request URL must both appear.
        EXPECT_NE(what.find("404"), std::string::npos);
        EXPECT_NE(what.find("127.0.0.1"), std::string::npos);
    }
    EXPECT_TRUE(threw_runtime_error);

    svr.stop();
    if (t.joinable()) t.join();
}

TEST(MCPClientAsync, UrlWithMcpSuffixIsNotDoubled) {
    // Regression for issue #66: a user-supplied URL that already ends in
    // `/mcp` must resolve to `/mcp`, not `/mcp/mcp`. We register a handler
    // ONLY at `/mcp`; if the client doubled the suffix it would request
    // `/mcp/mcp` and httplib would 404 (which call_tool turns into a throw).
    std::atomic<int> hits_mcp{0};
    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        hits_mcp.fetch_add(1, std::memory_order_relaxed);
        int id = 0;
        std::string method;
        try {
            auto parsed = json::parse(req.body);
            if (parsed.is_object()) {
                id = parsed.value("id", 0);
                method = parsed.value("method", std::string{});
            }
        } catch (...) { }
        if (method == "notifications/initialized") {
            res.status = 204;
            return;
        }
        if (method == "initialize") {
            res.set_content(
                R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                    R"(,"result":{"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":{"name":"url-test","version":"1"}}})",
                "application/json");
            return;
        }
        res.set_content(
            R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                R"(,"result":{"ok":true}})",
            "application/json");
    });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    for (int i = 0; i < 200 && !svr.is_running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(svr.is_running());

    // URL already carries the `/mcp` path the spec endpoint uses.
    mcp::MCPClient client(
        "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    auto result = client.call_tool("ping", json::object());

    EXPECT_TRUE(result.is_object());
    EXPECT_EQ(result.value("ok", false), true);
    // initialize + initialized notification + tools/call all reached /mcp.
    EXPECT_EQ(hits_mcp.load(), 3);

    svr.stop();
    if (t.joinable()) t.join();
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
        std::string method;
        try {
            auto parsed = json::parse(req.body);
            if (parsed.is_object()) {
                id = parsed.value("id", 0);
                method = parsed.value("method", std::string{});
            }
        } catch (...) { }
        if (method == "notifications/initialized") {
            res.status = 204;
            return;
        }
        if (method == "initialize") {
            std::string body = R"({"jsonrpc":"2.0","id":)"
                + std::to_string(id)
                + R"(,"result":{"protocolVersion":"2025-11-25","capabilities":{},"serverInfo":{"name":"session-test","version":"1"}}})";
            res.set_content(body, "application/json");
            return;
        }
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

    // initialize assigns the id; the notification and three tool calls echo it.
    EXPECT_EQ(seen_session_echoes.load(), 4);
}

TEST(MCPClientAsync, StrictLifecyclePaginationAndRichResults) {
    std::atomic<int> initialize_count{0};
    std::atomic<int> notification_count{0};
    std::atomic<int> list_count{0};
    std::atomic<int> call_count{0};
    std::atomic<int> violations{0};
    std::atomic<bool> initialized{false};
    const std::string sid = "strict-session";

    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        json request = json::parse(req.body);
        const auto method = request.value("method", std::string{});
        const int id = request.value("id", 0);

        auto reply = [&](json result) {
            res.set_content(json{{"jsonrpc", "2.0"}, {"id", id},
                                 {"result", std::move(result)}}.dump(),
                            "application/json");
        };
        auto rpc_error = [&](int code, const std::string& message, json data) {
            res.set_content(json{{"jsonrpc", "2.0"}, {"id", id},
                                 {"error", {{"code", code},
                                            {"message", message},
                                            {"data", std::move(data)}}}}.dump(),
                            "application/json");
        };

        if (req.get_header_value("X-Static") != "static-value"
            || req.get_header_value("Authorization") != "Bearer dynamic-token") {
            violations.fetch_add(1, std::memory_order_relaxed);
        }

        if (method == "initialize") {
            if (initialize_count.fetch_add(1, std::memory_order_relaxed) != 0) {
                rpc_error(-32001, "repeated initialize", nullptr);
                return;
            }
            res.set_header("Mcp-Session-Id", sid);
            reply({{"protocolVersion", "2025-11-25"},
                   {"capabilities", {{"tools", {{"listChanged", true}}}}},
                   {"serverInfo", {{"name", "strict"}, {"version", "1.2.3"}}},
                   {"instructions", "Use typed results."}});
            return;
        }

        if (req.get_header_value("Mcp-Session-Id") != sid
            || req.get_header_value("MCP-Protocol-Version") != "2025-11-25") {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
        if (method == "notifications/initialized") {
            notification_count.fetch_add(1, std::memory_order_relaxed);
            initialized.store(true, std::memory_order_release);
            res.status = 204;
            return;
        }
        if (!initialized.load(std::memory_order_acquire)) {
            violations.fetch_add(1, std::memory_order_relaxed);
            rpc_error(-32002, "not initialized", nullptr);
            return;
        }

        if (method == "tools/list") {
            list_count.fetch_add(1, std::memory_order_relaxed);
            const auto& params = request.value("params", json::object());
            if (!params.contains("cursor")) {
                reply({
                    {"tools", json::array({{
                        {"name", "rich"},
                        {"title", "Rich result"},
                        {"description", "Returns every supported result shape"},
                        {"icons", json::array({{{"src", "data:image/png;base64,AA=="},
                                                {"mimeType", "image/png"}}})},
                        {"inputSchema", {{"type", "object"}}},
                        {"outputSchema", {{"type", "object"},
                                          {"required", json::array({"answer"})},
                                          {"properties", {{"answer", {{"type", "integer"}}}}}}},
                        {"annotations", {{"readOnlyHint", true}}},
                        {"execution", {{"taskSupport", "forbidden"}}},
                        {"_meta", {{"catalog", "first"}}},
                    }})},
                    {"nextCursor", "page-2"},
                    {"_meta", {{"page", 1}}},
                });
                return;
            }
            if (params.value("cursor", "") != "page-2") {
                rpc_error(-32602, "bad cursor", params);
                return;
            }
            reply({{"tools", json::array({
                       {{"name", "tool_error"},
                        {"description", "Returns isError"},
                        {"inputSchema", {{"type", "object"}}}},
                       {{"name", "bad_schema"},
                        {"description", "Violates its output schema"},
                        {"inputSchema", {{"type", "object"}}},
                        {"outputSchema", {{"type", "object"},
                                          {"required", json::array({"answer"})},
                                          {"properties", {{"answer", {{"type", "integer"}}}}}}}},
                   })},
                   {"_meta", {{"page", 2}}}});
            return;
        }

        if (method == "tools/call") {
            call_count.fetch_add(1, std::memory_order_relaxed);
            const auto& params = request.value("params", json::object());
            const auto name = params.value("name", std::string{});
            if (name == "rich") {
                reply({{"content", json::array({
                           {{"type", "text"}, {"text", "answer: 42"}},
                           {{"type", "image"}, {"data", "AA=="},
                            {"mimeType", "image/png"}},
                       })},
                       {"structuredContent", {{"answer", 42}}},
                       {"isError", false},
                       {"_meta", {{"trace", "trace-1"}}}});
                return;
            }
            if (name == "tool_error") {
                reply({{"content", json::array({{{"type", "text"},
                                                  {"text", "tool failed"}}})},
                       {"isError", true},
                       {"_meta", {{"retryable", false}}}});
                return;
            }
            if (name == "bad_schema") {
                reply({{"content", json::array()},
                       {"structuredContent", {{"answer", "not-an-integer"}}}});
                return;
            }
            rpc_error(-32602, "unknown tool", {{"name", name}});
            return;
        }

        rpc_error(-32601, "method not found", {{"method", method}});
    });

    ServerGuard server(svr);
    ASSERT_TRUE(svr.is_running());

    std::atomic<int> provider_calls{0};
    mcp::MCPClientConfig config;
    config.headers = {{"X-Static", "static-value"}};
    config.header_provider = [&] {
        provider_calls.fetch_add(1, std::memory_order_relaxed);
        return mcp::HeaderList{{"Authorization", "Bearer dynamic-token"}};
    };
    mcp::MCPClient client(
        "http://127.0.0.1:" + std::to_string(server.port) + "/mcp", config);

    EXPECT_TRUE(client.initialize("strict-test"));
    EXPECT_TRUE(client.initialize("ignored-after-first-init"));
    EXPECT_TRUE(client.is_initialized());
    const auto init = client.get_initialize_result();
    EXPECT_EQ(init.protocol_version, "2025-11-25");
    EXPECT_EQ(init.server_info.value("name", ""), "strict");
    EXPECT_EQ(init.instructions, "Use typed results.");

    auto tools = client.get_tools();
    ASSERT_EQ(tools.size(), 3u);
    EXPECT_EQ(initialize_count.load(), 1);
    EXPECT_EQ(notification_count.load(), 1);
    EXPECT_EQ(list_count.load(), 2);

    auto* rich = dynamic_cast<mcp::MCPTool*>(tools[0].get());
    ASSERT_NE(rich, nullptr);
    const auto& definition = rich->get_mcp_definition();
    EXPECT_EQ(definition.title, "Rich result");
    EXPECT_EQ(definition.annotations.value("readOnlyHint", false), true);
    EXPECT_EQ(definition.meta.value("catalog", ""), "first");

    const auto result = rich->execute_result(json::object());
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.content.size(), 2u);
    EXPECT_EQ(result.content[1].value("type", ""), "image");
    EXPECT_EQ(result.structured_content.value("answer", 0), 42);
    EXPECT_EQ(result.meta.value("trace", ""), "trace-1");

    auto* tool_error = dynamic_cast<mcp::MCPTool*>(tools[1].get());
    ASSERT_NE(tool_error, nullptr);
    const auto error_result = tool_error->execute_result(json::object());
    EXPECT_TRUE(error_result.is_error);
    EXPECT_THROW(tool_error->execute(json::object()), std::runtime_error);

    auto* bad_schema = dynamic_cast<mcp::MCPTool*>(tools[2].get());
    ASSERT_NE(bad_schema, nullptr);
    EXPECT_THROW(bad_schema->execute_result(json::object()), std::runtime_error);

    try {
        client.call_tool_result("missing", json::object());
        FAIL() << "expected MCPError";
    } catch (const mcp::MCPError& e) {
        EXPECT_EQ(e.code(), -32602);
        EXPECT_EQ(e.data().value("name", ""), "missing");
    }

    EXPECT_EQ(initialize_count.load(), 1);
    EXPECT_EQ(violations.load(), 0);
    EXPECT_GE(provider_calls.load(), 1);
    EXPECT_EQ(call_count.load(), 5);

}

TEST(MCPClientAsync, ConfiguredRequestTimeoutIsEnforced) {
    httplib::Server svr;
    svr.Post("/mcp", [](const httplib::Request&, httplib::Response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        res.set_content("{}", "application/json");
    });
    ServerGuard server(svr);
    ASSERT_TRUE(svr.is_running());

    mcp::MCPClientConfig config;
    config.request_timeout = std::chrono::milliseconds(20);
    mcp::MCPClient client(
        "http://127.0.0.1:" + std::to_string(server.port), config);
    EXPECT_THROW(client.initialize(), std::runtime_error);
    EXPECT_FALSE(client.is_initialized());

}

TEST(MCPClientAsync, FailedInitializationClearsNegotiatedHttpStateBeforeRetry) {
    std::atomic<int> initialize_count{0};
    std::atomic<int> notification_count{0};
    std::atomic<int> leaked_state{0};

    httplib::Server svr;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        const auto request = json::parse(req.body);
        const auto method = request.value("method", std::string{});
        const int id = request.value("id", 0);
        if (method == "initialize") {
            if (!req.get_header_value("Mcp-Session-Id").empty()
                || !req.get_header_value("MCP-Protocol-Version").empty()) {
                leaked_state.fetch_add(1, std::memory_order_relaxed);
            }
            const int attempt = initialize_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
            res.set_header("Mcp-Session-Id", "session-" + std::to_string(attempt));
            res.set_content(
                json{{"jsonrpc", "2.0"}, {"id", id},
                     {"result", {{"protocolVersion", "2025-11-25"},
                                 {"capabilities", json::object()},
                                 {"serverInfo", {{"name", "retry"},
                                                 {"version", "1"}}}}}}.dump(),
                "application/json");
            return;
        }
        if (method == "notifications/initialized") {
            const int attempt = notification_count.fetch_add(
                1, std::memory_order_relaxed) + 1;
            if (attempt == 1) {
                res.status = 500;
                res.set_content("first notification failed", "text/plain");
            } else {
                EXPECT_EQ(req.get_header_value("Mcp-Session-Id"), "session-2");
                EXPECT_EQ(req.get_header_value("MCP-Protocol-Version"),
                          "2025-11-25");
                res.status = 204;
            }
            return;
        }
        res.status = 400;
    });
    ServerGuard server(svr);
    ASSERT_TRUE(svr.is_running());

    mcp::MCPClient client(
        "http://127.0.0.1:" + std::to_string(server.port));
    EXPECT_THROW(client.initialize(), std::runtime_error);
    EXPECT_FALSE(client.is_initialized());
    EXPECT_TRUE(client.initialize());
    EXPECT_TRUE(client.is_initialized());
    EXPECT_EQ(initialize_count.load(), 2);
    EXPECT_EQ(notification_count.load(), 2);
    EXPECT_EQ(leaked_state.load(), 0);
}
