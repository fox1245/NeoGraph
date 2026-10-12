#include <neograph/mcp/http_server.h>

#include <gtest/gtest.h>
#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/hook_outbox.h>
#include <neograph/mcp/client.h>
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using neograph::json;
using neograph::mcp::CallToolResult;
using neograph::mcp::MCPHttpServer;
using neograph::mcp::MCPHttpServerConfig;
using neograph::mcp::MCPHttpServerSession;
using neograph::mcp::MCPServer;
using neograph::mcp::MCPServerConfig;
using neograph::mcp::ToolDefinition;
#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
using neograph::mcp::MCPClient;
#endif

namespace {

using namespace std::chrono_literals;

std::unique_ptr<MCPServer> make_server() {
    MCPServerConfig config;
    config.server_info = {{"name", "http-test"}, {"version", "1"}};
    auto server        = std::make_unique<MCPServer>(std::move(config));

    ToolDefinition echo;
    echo.name         = "echo";
    echo.description  = "Echo a value";
    echo.input_schema = {
        {"type", "object"},
        {"required", json::array({"value"})},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    echo.output_schema = {
        {"type", "object"},
        {"required", json::array({"echoed"})},
        {"properties", {{"echoed", {{"type", "string"}}}}},
        {"additionalProperties", false},
    };
    server->register_tool(std::move(echo), [](const json& arguments, const auto&) {
        const auto     value = arguments.at("value").get<std::string>();
        CallToolResult result;
        result.content            = json::array({{{"type", "text"}, {"text", value}}});
        result.structured_content = {{"echoed", value}};
        return result;
    });
    return server;
}

MCPHttpServerSession make_http_session(std::string_view) {
    return {make_server(), {}};
}

json request(json id, std::string method, json params = json::object()) {
    return {
        {"jsonrpc", "2.0"},
        {"id", std::move(id)},
        {"method", std::move(method)},
        {"params", std::move(params)},
    };
}

httplib::Headers base_headers() {
    return {{"Accept", "application/json, text/event-stream"}};
}

httplib::Headers session_headers(const std::string& session_id, const std::string& bearer = {}) {
    auto headers = base_headers();
    headers.emplace("Mcp-Session-Id", session_id);
    headers.emplace("MCP-Protocol-Version", "2025-11-25");
    if (!bearer.empty()) headers.emplace("Authorization", "Bearer " + bearer);
    return headers;
}

class HttpServerGuard {
public:
    explicit HttpServerGuard(MCPHttpServer& server) : server_(server) {}
    ~HttpServerGuard() { server_.stop(); }

private:
    MCPHttpServer& server_;
};

// A tool that runs until its call is cancelled, then counts the cancellation.
struct BlockingTool {
    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};

    MCPHttpServer::ServerFactory factory() {
        return [this](std::string_view) {
            MCPServerConfig config;
            config.server_info = {{"name", "blocking"}, {"version", "1"}};
            config.max_concurrent_calls = 8;
            auto server = std::make_unique<MCPServer>(std::move(config));
            ToolDefinition wait;
            wait.name = "wait";
            server->register_tool(std::move(wait), [this](const json&, const auto& cancel) {
                started.fetch_add(1);
                const auto until = std::chrono::steady_clock::now() + 10s;
                while (std::chrono::steady_clock::now() < until) {
                    if (cancel->is_cancelled()) {
                        cancelled.fetch_add(1);
                        cancel->throw_if_cancelled("wait cancelled");
                    }
                    std::this_thread::sleep_for(5ms);
                }
                return CallToolResult{};
            });
            return MCPHttpServerSession{std::move(server), {}};
        };
    }
};

std::string open_session(httplib::Client& client) {
    auto initialize = client.Post(
        "/mcp", base_headers(),
        request(1, "initialize", {{"protocolVersion", "2025-11-25"},
                                  {"capabilities", json::object()},
                                  {"clientInfo", {{"name", "lifecycle"}, {"version", "1"}}}})
            .dump(),
        "application/json");
    if (!initialize || initialize->status != 200) return {};
    const auto id = initialize->get_header_value("Mcp-Session-Id");
    client.Post("/mcp", session_headers(id),
                json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump(),
                "application/json");
    return id;
}

int ping_status(httplib::Client& client, const std::string& session_id) {
    auto response =
        client.Post("/mcp", session_headers(session_id), request(7, "ping").dump(), "application/json");
    return response ? response->status : -1;
}

TEST(MCPHttpServerTest, DefaultsRejectUnauthenticatedRemoteBind) {
    MCPHttpServerConfig config;
    config.host = "0.0.0.0";
    EXPECT_THROW(MCPHttpServer(make_http_session, std::move(config)), std::invalid_argument);
}

TEST(MCPHttpServerTest, AnIdleSessionMakesRoomAtCapacity) {
    MCPHttpServerConfig config;
    config.max_sessions = 1;
    MCPHttpServer server(make_http_session, std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    const auto initialize_body =
        request(1, "initialize",
                {{"protocolVersion", "2025-11-25"},
                 {"capabilities", json::object()},
                 {"clientInfo", {{"name", "capacity-test"}, {"version", "1"}}}});
    auto first = client.Post("/mcp", base_headers(), initialize_body.dump(), "application/json");
    ASSERT_TRUE(first);
    ASSERT_EQ(first->status, 200);
    const auto first_id = first->get_header_value("Mcp-Session-Id");

    initialize_body["id"] = 2;
    auto second = client.Post("/mcp", base_headers(), initialize_body.dump(), "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 200);
    EXPECT_EQ(ping_status(client, first_id), 404);
    EXPECT_EQ(ping_status(client, second->get_header_value("Mcp-Session-Id")), 200);
}

TEST(MCPHttpServerTest, JsonPostLifecycleAndDeleteRoundTrip) {
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    auto invalid_accept = base_headers();
    invalid_accept.erase("Accept");
    invalid_accept.emplace("Accept", "application/json-ld, text/event-stream");
    auto rejected_accept =
        client.Post("/mcp", invalid_accept,
                    request(0, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "bad-accept"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(rejected_accept);
    EXPECT_EQ(rejected_accept->status, 406);

    auto initialize =
        client.Post("/mcp", base_headers(),
                    request(1, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "http-test"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(initialize);
    ASSERT_EQ(initialize->status, 200);
    const auto session_id = initialize->get_header_value("Mcp-Session-Id");
    ASSERT_FALSE(session_id.empty());
    EXPECT_EQ(json::parse(initialize->body)["result"]["protocolVersion"], "2025-11-25");

    // Without MCP-Protocol-Version the negotiated revision applies (MCP-S-08);
    // another revision than the negotiated one is 400.
    auto missing_version_headers = base_headers();
    missing_version_headers.emplace("Mcp-Session-Id", session_id);
    auto missing_version = client.Post("/mcp", missing_version_headers,
                                       request(9, "ping").dump(), "application/json");
    ASSERT_TRUE(missing_version);
    EXPECT_EQ(missing_version->status, 200);
    auto other_version_headers = missing_version_headers;
    other_version_headers.emplace("MCP-Protocol-Version", "2025-06-18");
    auto other_version = client.Post("/mcp", other_version_headers,
                                     request(19, "ping").dump(), "application/json");
    ASSERT_TRUE(other_version);
    EXPECT_EQ(other_version->status, 400);

    auto repeated_initialize =
        client.Post("/mcp", session_headers(session_id),
                    request(10, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "repeat"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(repeated_initialize);
    EXPECT_EQ(repeated_initialize->status, 400);

    json initialized = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    };
    auto accepted =
        client.Post("/mcp", session_headers(session_id), initialized.dump(), "application/json");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->status, 202);
    EXPECT_TRUE(accepted->body.empty());

    auto call = client.Post(
        "/mcp", session_headers(session_id),
        request("call", "tools/call", {{"name", "echo"}, {"arguments", {{"value", "hello"}}}})
            .dump(),
        "application/json");
    ASSERT_TRUE(call);
    ASSERT_EQ(call->status, 200);
    EXPECT_EQ(json::parse(call->body)["result"]["structuredContent"]["echoed"], "hello");

    auto get = client.Get("/mcp", base_headers());
    ASSERT_TRUE(get);
    EXPECT_EQ(get->status, 405);

    auto removed = client.Delete("/mcp", session_headers(session_id));
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->status, 204);

    auto after_delete = client.Post("/mcp", session_headers(session_id),
                                    request(2, "tools/list").dump(), "application/json");
    ASSERT_TRUE(after_delete);
    EXPECT_EQ(after_delete->status, 404);
}

TEST(MCPHttpServerTest, SessionsKeepTheRevisionTheyNegotiated) {
    // D2: a session that negotiated 2025-06-18 expects that revision in
    // MCP-Protocol-Version, or no header at all.
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    auto initialize = client.Post(
        "/mcp", base_headers(),
        request(1, "initialize", {{"protocolVersion", "2025-06-18"},
                                  {"capabilities", json::object()},
                                  {"clientInfo", {{"name", "older"}, {"version", "1"}}}})
            .dump(),
        "application/json");
    ASSERT_TRUE(initialize);
    ASSERT_EQ(initialize->status, 200);
    EXPECT_EQ(json::parse(initialize->body)["result"]["protocolVersion"], "2025-06-18");
    const auto session_id = initialize->get_header_value("Mcp-Session-Id");
    auto with_version = [&](const char* version) {
        auto headers = base_headers();
        headers.emplace("Mcp-Session-Id", session_id);
        if (version) headers.emplace("MCP-Protocol-Version", version);
        return client.Post("/mcp", headers, request(2, "ping").dump(), "application/json");
    };
    auto negotiated = with_version("2025-06-18");
    ASSERT_TRUE(negotiated);
    EXPECT_EQ(negotiated->status, 200);
    auto absent = with_version(nullptr);
    ASSERT_TRUE(absent);
    EXPECT_EQ(absent->status, 200);
    auto newest = with_version("2025-11-25");
    ASSERT_TRUE(newest);
    EXPECT_EQ(newest->status, 400);
}

TEST(MCPHttpServerTest, IdleSessionsExpireAndAnswer404) {
    // MCP-S-02: a session without requests for session_idle_timeout is closed,
    // and its id answers 404, which tells the client to initialize again.
    MCPHttpServerConfig config;
    config.session_idle_timeout = 300ms;
    MCPHttpServer server(make_http_session, std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    const auto active = open_session(client);
    const auto idle = open_session(client);
    ASSERT_FALSE(active.empty());
    ASSERT_FALSE(idle.empty());
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(ping_status(client, active), 200);
        std::this_thread::sleep_for(100ms);
    }
    EXPECT_EQ(ping_status(client, active), 200);
    EXPECT_EQ(ping_status(client, idle), 404);
}

TEST(MCPHttpServerTest, AbandonedSessionsDoNotExhaustTheServer) {
    // MCP-S-02: the official TypeScript clients never DELETE on close(). At
    // capacity the least recently used idle session makes room; only when
    // every session is busy is initialize refused, with Retry-After.
    BlockingTool tool;
    MCPHttpServerConfig config;
    config.max_sessions = 2;
    MCPHttpServer server(tool.factory(), std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    std::vector<std::string> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(open_session(client));
        ASSERT_FALSE(ids.back().empty()) << "initialize #" << i;
    }
    EXPECT_EQ(ping_status(client, ids[0]), 404);
    EXPECT_EQ(ping_status(client, ids[3]), 200);
    EXPECT_EQ(ping_status(client, ids[4]), 200);

    // Both sessions busy: nothing may be evicted.
    std::vector<std::future<httplib::Result>> calls;
    for (int i = 3; i < 5; ++i) {
        calls.push_back(std::async(std::launch::async, [port = server.port(), id = ids[i]] {
            httplib::Client caller("127.0.0.1", port);
            caller.set_read_timeout(15s);
            return caller.Post("/mcp", session_headers(id),
                               request("w", "tools/call", {{"name", "wait"},
                                                           {"arguments", json::object()}})
                                   .dump(),
                               "application/json");
        }));
    }
    for (int i = 0; i < 2000 && tool.started.load() < 2; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(tool.started.load(), 2);
    auto refused = client.Post(
        "/mcp", base_headers(),
        request(1, "initialize", {{"protocolVersion", "2025-11-25"},
                                  {"capabilities", json::object()},
                                  {"clientInfo", {{"name", "late"}, {"version", "1"}}}})
            .dump(),
        "application/json");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 503);
    EXPECT_EQ(refused->get_header_value("Retry-After"), "1");
    EXPECT_FALSE(json::parse(refused->body).contains("id"));
    for (int i = 3; i < 5; ++i) {
        httplib::Client canceller("127.0.0.1", server.port());
        canceller.Post("/mcp", session_headers(ids[i]),
                       json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                            {"params", {{"requestId", "w"}}}}.dump(),
                       "application/json");
    }
    for (auto& call : calls) call.get();
}

TEST(MCPHttpServerTest, ACancelledCallEndsWithAnEmptyEventStream) {
    // MCP: the receiver of notifications/cancelled SHOULD NOT answer the
    // request; the official clients report a response to an id they cancelled
    // as an error. The POST ends with an event stream that carries nothing.
    BlockingTool tool;
    MCPHttpServer server(tool.factory());
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    const auto id = open_session(client);
    ASSERT_FALSE(id.empty());

    auto call = std::async(std::launch::async, [port = server.port(), id] {
        httplib::Client caller("127.0.0.1", port);
        return caller.Post("/mcp", session_headers(id),
                           request(41, "tools/call", {{"name", "wait"}, {"arguments", json::object()}})
                               .dump(),
                           "application/json");
    });
    for (int i = 0; i < 400 && tool.started.load() < 1; ++i) std::this_thread::sleep_for(5ms);
    auto cancel = client.Post("/mcp", session_headers(id),
                              json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                                   {"params", {{"requestId", 41}, {"reason", "user"}}}}.dump(),
                              "application/json");
    ASSERT_TRUE(cancel);
    EXPECT_EQ(cancel->status, 202);
    auto answered = call.get();
    ASSERT_TRUE(answered);
    EXPECT_EQ(answered->status, 200);
    EXPECT_EQ(answered->get_header_value("Content-Type").rfind("text/event-stream", 0), 0u);
    EXPECT_TRUE(answered->body.empty()) << answered->body;
    EXPECT_EQ(tool.cancelled.load(), 1);
}

TEST(MCPHttpServerTest, AResponseTimeoutCancelsTheTool) {
    // MCP-S-16: when nobody waits for the answer any more, the tool stops.
    BlockingTool tool;
    MCPHttpServerConfig config;
    config.response_timeout = 200ms;
    MCPHttpServer server(tool.factory(), std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    const auto id = open_session(client);
    auto timed_out = client.Post(
        "/mcp", session_headers(id),
        request(5, "tools/call", {{"name", "wait"}, {"arguments", json::object()}}).dump(),
        "application/json");
    ASSERT_TRUE(timed_out);
    EXPECT_EQ(timed_out->status, 504);
    EXPECT_EQ(json::parse(timed_out->body)["id"], 5);
    for (int i = 0; i < 400 && tool.cancelled.load() < 1; ++i) std::this_thread::sleep_for(5ms);
    EXPECT_EQ(tool.cancelled.load(), 1);
}

TEST(MCPHttpServerTest, CancellationIsNotStarvedByCallsInFlight) {
    // MCP-S-10: with every kept HTTP thread held by a tool call, a
    // notifications/cancelled on a new connection is still handled at once.
    BlockingTool tool;
    MCPHttpServerConfig config;
    config.http_threads = 2;
    MCPHttpServer server(tool.factory(), std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    const auto id = open_session(client);
    std::vector<std::future<httplib::Result>> calls;
    for (int i = 0; i < 4; ++i) {
        calls.push_back(std::async(std::launch::async, [port = server.port(), id, i] {
            httplib::Client caller("127.0.0.1", port);
            caller.set_read_timeout(15s);
            return caller.Post("/mcp", session_headers(id),
                               request(100 + i, "tools/call",
                                       {{"name", "wait"}, {"arguments", json::object()}})
                                   .dump(),
                               "application/json");
        }));
    }
    for (int i = 0; i < 2000 && tool.started.load() < 4; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(tool.started.load(), 4);
    for (int i = 0; i < 4; ++i) {
        httplib::Client canceller("127.0.0.1", server.port());
        const auto started = std::chrono::steady_clock::now();
        auto cancel = canceller.Post("/mcp", session_headers(id),
                                     json{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
                                          {"params", {{"requestId", 100 + i}}}}.dump(),
                                     "application/json");
        ASSERT_TRUE(cancel);
        EXPECT_EQ(cancel->status, 202);
        EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    }
    for (auto& call : calls) call.get();
    EXPECT_EQ(tool.cancelled.load(), 4);
}

TEST(MCPHttpServerTest, DeleteClosesTheConnection) {
    // MCP-S-17: the official Python client's DELETE carries Content-Type and
    // no body; the server closes that connection after its 204.
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    const auto id = open_session(client);
    auto headers = session_headers(id);
    headers.emplace("Content-Type", "application/json");
    auto removed = client.Delete("/mcp", headers);
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->status, 204);
    EXPECT_EQ(removed->get_header_value("Connection"), "close");
    EXPECT_EQ(ping_status(client, id), 404);
}

TEST(MCPHttpServerTest, ErrorBodiesCarryNoNullId) {
    // MCP-S-07: an HTTP error body is a JSON-RPC error response without an id,
    // never "id": null, which the 2025-11-25 schema does not allow.
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    auto no_session = client.Post("/mcp", base_headers(), request(2, "tools/list").dump(),
                                  "application/json");
    ASSERT_TRUE(no_session);
    EXPECT_EQ(no_session->status, 400);
    EXPECT_FALSE(json::parse(no_session->body).contains("id")) << no_session->body;

    auto unknown = client.Post("/mcp", session_headers("ng_missing"),
                               request(3, "tools/list").dump(), "application/json");
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);
    EXPECT_FALSE(json::parse(unknown->body).contains("id")) << unknown->body;

    auto unparsable = client.Post("/mcp", base_headers(), "{not json", "application/json");
    ASSERT_TRUE(unparsable);
    EXPECT_EQ(unparsable->status, 400);
    const auto body = json::parse(unparsable->body);
    EXPECT_EQ(body["error"]["code"], -32700);
    EXPECT_FALSE(body.contains("id")) << unparsable->body;
}

// MCP 2025-03-26 Streamable HTTP: a POST body may be a batch, and a batch
// with requests is answered with an array. Sessions that negotiated
// 2025-03-26 or 2024-11-05 get that; later revisions removed batching, and a
// batch can never initialize a session.
TEST(MCPHttpServerTest, BatchesFollowTheSessionRevision) {
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    auto headers_for = [](const std::string& session, const std::string& revision) {
        auto headers = base_headers();
        headers.emplace("Mcp-Session-Id", session);
        headers.emplace("MCP-Protocol-Version", revision);
        return headers;
    };
    auto open_as = [&](const std::string& revision) {
        auto init = client.Post(
            "/mcp", base_headers(),
            request(1, "initialize", {{"protocolVersion", revision},
                                      {"capabilities", json::object()},
                                      {"clientInfo", {{"name", "batch"}, {"version", "1"}}}})
                .dump(),
            "application/json");
        EXPECT_TRUE(init && init->status == 200);
        if (!init) return std::string();
        EXPECT_EQ(json::parse(init->body)["result"]["protocolVersion"], revision);
        const auto id = init->get_header_value("Mcp-Session-Id");
        client.Post("/mcp", headers_for(id, revision),
                    json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump(),
                    "application/json");
        return id;
    };
    const json progress = {{"jsonrpc", "2.0"},
                           {"method", "notifications/progress"},
                           {"params", {{"progressToken", 1}, {"progress", 1}}}};
    for (const std::string revision : {"2025-03-26", "2024-11-05"}) {
        const auto session = open_as(revision);
        const auto batch = json::array(
            {request(2, "ping"),
             request(3, "tools/call", {{"name", "echo"}, {"arguments", {{"value", "b"}}}}),
             progress});
        auto answered =
            client.Post("/mcp", headers_for(session, revision), batch.dump(), "application/json");
        ASSERT_TRUE(answered);
        EXPECT_EQ(answered->status, 200) << answered->body;
        EXPECT_EQ(answered->get_header_value("Content-Type").rfind("application/json", 0), 0u);
        const auto body = json::parse(answered->body);
        ASSERT_TRUE(body.is_array()) << answered->body;
        ASSERT_EQ(body.size(), 2u) << answered->body;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const json item = body[i];
            if (item["id"] == 2) EXPECT_EQ(item["result"], json::object());
            else EXPECT_EQ(item["result"]["content"][0]["text"], "b") << item.dump();
        }
        auto notifications_only = client.Post("/mcp", headers_for(session, revision),
                                              json::array({progress}).dump(), "application/json");
        ASSERT_TRUE(notifications_only);
        EXPECT_EQ(notifications_only->status, 202);
    }

    const auto modern = open_as("2025-11-25");
    auto refused = client.Post("/mcp", headers_for(modern, "2025-11-25"),
                               json::array({request(4, "ping")}).dump(), "application/json");
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->status, 400);
    const auto error = json::parse(refused->body);
    EXPECT_EQ(error["error"]["code"], -32600);
    EXPECT_FALSE(error.contains("id")) << refused->body;

    auto batched_initialize = client.Post(
        "/mcp", base_headers(),
        json::array({request(5, "initialize",
                              {{"protocolVersion", "2025-03-26"},
                               {"capabilities", json::object()},
                               {"clientInfo", {{"name", "batch"}, {"version", "1"}}}})})
            .dump(),
        "application/json");
    ASSERT_TRUE(batched_initialize);
    EXPECT_EQ(batched_initialize->status, 400);
    EXPECT_TRUE(batched_initialize->get_header_value("Mcp-Session-Id").empty());
}

TEST(MCPHttpServerTest, RejectsUnlistedOrigin) {
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    auto            headers = base_headers();
    headers.emplace("Origin", "https://attacker.example");

    auto response =
        client.Post("/mcp", headers,
                    request(1, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "origin-test"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 403);
}

TEST(MCPHttpServerTest, DuplicateRequestDoesNotOrphanOriginalResponse) {
    struct BlockState {
        std::atomic<int>         calls{0};
        std::promise<void>       entered;
        std::promise<void>       release;
        std::shared_future<void> released = release.get_future().share();
    };
    auto          state = std::make_shared<BlockState>();
    MCPHttpServer server([state](std::string_view) {
        MCPServerConfig config;
        config.server_info     = {{"name", "duplicate-test"}, {"version", "1"}};
        auto           session = std::make_unique<MCPServer>(std::move(config));
        ToolDefinition block;
        block.name         = "block";
        block.input_schema = {{"type", "object"}};
        session->register_tool(std::move(block), [state](const json&, const auto&) {
            if (state->calls.fetch_add(1) == 0) state->entered.set_value();
            state->released.wait();
            CallToolResult result;
            result.content = json::array({{{"type", "text"}, {"text", "released"}}});
            return result;
        });
        return MCPHttpServerSession{std::move(session), state};
    });
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    auto initialize =
        client.Post("/mcp", base_headers(),
                    request(1, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "duplicate-test"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(initialize);
    const auto session_id  = initialize->get_header_value("Mcp-Session-Id");
    json       initialized = {
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", json::object()},
    };
    auto initialized_response =
        client.Post("/mcp", session_headers(session_id), initialized.dump(), "application/json");
    ASSERT_TRUE(initialized_response);
    ASSERT_EQ(initialized_response->status, 202);

    const auto call =
        request("same-id", "tools/call", {{"name", "block"}, {"arguments", json::object()}}).dump();
    auto       first   = std::async(std::launch::async, [port = server.port(), session_id, call] {
        httplib::Client first_client("127.0.0.1", port);
        return first_client.Post("/mcp", session_headers(session_id), call, "application/json");
    });
    const auto entered = state->entered.get_future().wait_for(2s);
    if (entered != std::future_status::ready) state->release.set_value();
    ASSERT_EQ(entered, std::future_status::ready);

    auto duplicate = client.Post("/mcp", session_headers(session_id), call, "application/json");
    state->release.set_value();
    EXPECT_TRUE(duplicate);
    if (duplicate) {
        EXPECT_EQ(duplicate->status, 200);
        EXPECT_EQ(json::parse(duplicate->body)["error"]["code"], -32600);
    }

    auto original = first.get();
    ASSERT_TRUE(original);
    ASSERT_EQ(original->status, 200);
    EXPECT_EQ(json::parse(original->body)["result"]["content"][0]["text"], "released");
    EXPECT_EQ(state->calls.load(), 1);
}

#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
TEST(MCPHttpServerTest, ExistingMcpClientUsesNegotiatedHttpSession) {
    MCPHttpServer server(make_http_session);
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);

    MCPClient client("http://127.0.0.1:" + std::to_string(server.port()) + "/mcp");
    ASSERT_TRUE(client.initialize("neograph-http-test"));
    auto definitions = client.get_tool_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions[0].name, "echo");
    auto result = client.call_tool_result("echo", {{"value", "client-roundtrip"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.structured_content["echoed"], "client-roundtrip");
}
#endif

#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
// A remote run control signal (the server's Hook blocked the call: -32010;
// the server's run needs input: -32011) is the remote's, not this run's:
// MCPTool reports it as an ordinary failure carrying the remote code
// (NG.HookBlocked, NG.Interrupted), and the local graph run goes on with a
// failed tool result. Native human-in-the-loop propagation across protocols is
// a later phase.
namespace {

std::unique_ptr<MCPServer> make_control_server() {
    MCPServerConfig config;
    config.server_info = {{"name", "control-test"}, {"version", "1"}};
    auto server = std::make_unique<MCPServer>(std::move(config));
    ToolDefinition blocked;
    blocked.name = "blocked";
    blocked.description = "A tool whose Hook blocks it";
    blocked.input_schema = {{"type", "object"}};
    server->register_tool(blocked, [](const json&, const auto&) -> CallToolResult {
        throw neograph::HookBoundaryBlocked("sha256:" + std::string(64, 'd'),
                                            neograph::HookExecutionState::Failed,
                                            "remote audit hook blocked the call");
    });
    ToolDefinition paused;
    paused.name = "paused";
    paused.description = "A tool that needs a human";
    paused.input_schema = {{"type", "object"}};
    server->register_tool(paused, [](const json&, const auto&) -> CallToolResult {
        throw neograph::graph::NodeInterrupt("remote approval needed", json{{"cmd", "deploy"}});
    });
    return server;
}

class RemoteControlCallingNode final : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeResult> run(neograph::graph::NodeInput) override {
        json assistant;
        assistant["role"] = "assistant";
        assistant["content"] = "";
        assistant["tool_calls"] = json::array({
            json{{"id", "call-blocked"}, {"name", "blocked"}, {"arguments", "{}"}},
            json{{"id", "call-paused"}, {"name", "paused"}, {"arguments", "{}"}},
        });
        neograph::graph::NodeResult out;
        out.writes.push_back(neograph::graph::ChannelWrite{"messages", json::array({assistant})});
        co_return out;
    }
    std::string get_name() const override { return "llm"; }
};

}  // namespace

TEST(MCPHttpServerTest, RemoteRunControlSignalsAreOrdinaryTypedToolFailures) {
    MCPHttpServer server([](std::string_view) {
        return MCPHttpServerSession{make_control_server(), {}};
    });
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    neograph::graph::NodeFactory::instance().register_type(
        "mcp_remote_control_llm",
        [](const std::string&, const json&, const neograph::graph::NodeContext&) {
            return std::make_unique<RemoteControlCallingNode>();
        });
    MCPClient client("http://127.0.0.1:" + std::to_string(server.port()) + "/mcp");
    ASSERT_TRUE(client.initialize("neograph-remote-control"));
    neograph::graph::NodeContext context;
    context.tools = neograph::ToolSet(client.get_tools());
    auto engine = neograph::graph::GraphEngine::compile(
        json{{"name", "mcp_remote_control"},
             {"channels", {{"messages", {{"reducer", "append"}}}}},
             {"nodes", {{"llm", {{"type", "mcp_remote_control_llm"}}},
                        {"tools", {{"type", "tool_dispatch"}}}}},
             {"edges", json::array({json{{"from", "__start__"}, {"to", "llm"}},
                                    json{{"from", "llm"}, {"to", "tools"}},
                                    json{{"from", "tools"}, {"to", "__end__"}}})}},
        context);
    neograph::graph::RunConfig config;
    config.thread_id = "mcp-remote-control";
    neograph::graph::RunResult result;
    ASSERT_NO_THROW(result = engine->run(config));
    EXPECT_FALSE(result.interrupted);
    std::map<std::string, std::pair<std::string, std::string>> tool_results;
    for (const auto& message : result.output["channels"]["messages"]["value"]) {
        if (message.value("role", "") != "tool") continue;
        tool_results[message.value("tool_call_id", "")] = {message.value("tool_status", ""),
                                                           message.value("content", "")};
    }
    ASSERT_EQ(tool_results.size(), 2u);
    for (const auto& [id, code] : std::vector<std::pair<std::string, std::string>>{
             {"call-blocked", "NG.HookBlocked"}, {"call-paused", "NG.Interrupted"}}) {
        SCOPED_TRACE(id);
        const auto& [status, content] = tool_results[id];
        EXPECT_NE(status, "succeeded");
        EXPECT_NE(content.find(code), std::string::npos) << content;
        EXPECT_EQ(content.find("remote audit hook"), std::string::npos)
            << "never the remote exception text: " << content;
    }
    // Called directly, the classified failure is ordinary, carries the code
    // and is not retried.
    for (auto& tool : client.get_tools()) {
        try {
            (void)neograph::async::run_sync(tool->execute_async(json::object()));
            ADD_FAILURE() << tool->get_name() << " must fail";
        } catch (...) {
            const auto verdict = neograph::graph::classify_failure(std::current_exception());
            EXPECT_EQ(verdict.failure_class, neograph::graph::NodeFailureClass::Ordinary);
            EXPECT_EQ(verdict.code, tool->get_name() == "blocked" ? "NG.HookBlocked"
                                                                    : "NG.Interrupted");
            EXPECT_FALSE(verdict.retryable);
        }
    }
}
#endif

TEST(MCPHttpServerTest, BindsSessionsToBearerAuthorizationScope) {
    MCPHttpServerConfig config;
    config.bearer_authorizer = [](std::string_view token) -> std::optional<std::string> {
        if (token == "token-a") return "principal-a";
        if (token == "token-b") return "principal-b";
        return std::nullopt;
    };
    MCPHttpServer server(make_http_session, std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    auto init_headers = base_headers();
    init_headers.emplace("Authorization", "Bearer token-a");
    auto initialize =
        client.Post("/mcp", init_headers,
                    request(1, "initialize",
                            {{"protocolVersion", "2025-11-25"},
                             {"capabilities", json::object()},
                             {"clientInfo", {{"name", "auth-test"}, {"version", "1"}}}})
                        .dump(),
                    "application/json");
    ASSERT_TRUE(initialize);
    ASSERT_EQ(initialize->status, 200);
    const auto session_id = initialize->get_header_value("Mcp-Session-Id");

    auto wrong_scope = client.Post("/mcp", session_headers(session_id, "token-b"),
                                   request(2, "tools/list").dump(), "application/json");
    ASSERT_TRUE(wrong_scope);
    EXPECT_EQ(wrong_scope->status, 403);

    auto missing_auth = client.Post("/mcp", session_headers(session_id),
                                    request(3, "tools/list").dump(), "application/json");
    ASSERT_TRUE(missing_auth);
    EXPECT_EQ(missing_auth->status, 401);

    auto correct_scope = client.Post("/mcp", session_headers(session_id, "token-a"),
                                     request(4, "tools/list").dump(), "application/json");
    ASSERT_TRUE(correct_scope);
    EXPECT_EQ(correct_scope->status, 200);

    auto wrong_scope_delete = client.Delete("/mcp", session_headers(session_id, "token-b"));
    ASSERT_TRUE(wrong_scope_delete);
    EXPECT_EQ(wrong_scope_delete->status, 403);

    auto correct_scope_delete = client.Delete("/mcp", session_headers(session_id, "token-a"));
    ASSERT_TRUE(correct_scope_delete);
    EXPECT_EQ(correct_scope_delete->status, 204);
}

TEST(MCPHttpServerTest, PublishesProtectedResourceMetadataForOAuthDeployments) {
    // MCP-S-09: with OAuth-issued bearer tokens, MCP authorization requires
    // RFC 9728 discovery, from the 401 challenge or a well-known URI.
    MCPHttpServerConfig config;
    config.bearer_authorizer = [](std::string_view token) -> std::optional<std::string> {
        if (token == "good") return "principal";
        return std::nullopt;
    };
    config.protected_resource_metadata = {
        {"resource", "https://mcp.example.com/mcp"},
        {"authorization_servers", json::array({"https://auth.example.com"})},
        {"scopes_supported", json::array({"mcp:tools", "mcp:read"})},
        {"bearer_methods_supported", json::array({"header"})},
    };
    const auto metadata = config.protected_resource_metadata;
    MCPHttpServer server(make_http_session, std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());

    for (const char* path : {"/.well-known/oauth-protected-resource/mcp",
                             "/.well-known/oauth-protected-resource"}) {
        auto published = client.Get(path);
        ASSERT_TRUE(published) << path;
        EXPECT_EQ(published->status, 200) << path;
        EXPECT_EQ(json::parse(published->body), metadata) << path;
    }
    const auto initialize = request(1, "initialize",
                                    {{"protocolVersion", "2025-11-25"},
                                     {"capabilities", json::object()},
                                     {"clientInfo", {{"name", "oauth"}, {"version", "1"}}}})
                                .dump();
    auto missing = client.Post("/mcp", base_headers(), initialize, "application/json");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 401);
    EXPECT_EQ(missing->get_header_value("WWW-Authenticate"),
              "Bearer resource_metadata=\"https://mcp.example.com/.well-known/"
              "oauth-protected-resource/mcp\", scope=\"mcp:tools mcp:read\"");
    auto headers = base_headers();
    headers.emplace("Authorization", "Bearer bad");
    auto invalid = client.Post("/mcp", headers, initialize, "application/json");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(invalid->status, 401);
    EXPECT_EQ(invalid->get_header_value("WWW-Authenticate").rfind(
                  "Bearer error=\"invalid_token\", resource_metadata=", 0),
              0u);

    MCPHttpServerConfig incomplete;
    incomplete.protected_resource_metadata = {{"resource", "https://mcp.example.com/mcp"}};
    EXPECT_THROW(MCPHttpServer(make_http_session, std::move(incomplete)), std::invalid_argument);
}

TEST(MCPHttpServerTest, StaticBearerTokensPublishNoMetadata) {
    MCPHttpServerConfig config;
    config.bearer_authorizer = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };
    MCPHttpServer server(make_http_session, std::move(config));
    ASSERT_TRUE(server.start_async());
    HttpServerGuard guard(server);
    httplib::Client client("127.0.0.1", server.port());
    auto published = client.Get("/.well-known/oauth-protected-resource");
    ASSERT_TRUE(published);
    EXPECT_EQ(published->status, 404);
    auto missing = client.Post("/mcp", base_headers(), request(1, "ping").dump(), "application/json");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->get_header_value("WWW-Authenticate"), "Bearer");
}

}  // namespace
