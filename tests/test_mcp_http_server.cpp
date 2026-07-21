#include <neograph/mcp/http_server.h>

#include <gtest/gtest.h>
#ifdef NEOGRAPH_TESTS_HAVE_MCP_CLIENT
#include <neograph/mcp/client.h>
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

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

TEST(MCPHttpServerTest, DefaultsRejectUnauthenticatedRemoteBind) {
    MCPHttpServerConfig config;
    config.host = "0.0.0.0";
    EXPECT_THROW(MCPHttpServer(make_http_session, std::move(config)), std::invalid_argument);
}

TEST(MCPHttpServerTest, RejectsInitializeWhenSessionCapacityIsExhausted) {
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

    initialize_body["id"] = 2;
    auto second = client.Post("/mcp", base_headers(), initialize_body.dump(), "application/json");
    ASSERT_TRUE(second);
    EXPECT_EQ(second->status, 503);
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

    auto missing_version_headers = base_headers();
    missing_version_headers.emplace("Mcp-Session-Id", session_id);
    auto missing_version = client.Post("/mcp", missing_version_headers,
                                       request(9, "tools/list").dump(), "application/json");
    ASSERT_TRUE(missing_version);
    EXPECT_EQ(missing_version->status, 400);

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

    MCPClient client("http://127.0.0.1:" + std::to_string(server.port()));
    ASSERT_TRUE(client.initialize("neograph-http-test"));
    auto definitions = client.get_tool_definitions();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_EQ(definitions[0].name, "echo");
    auto result = client.call_tool_result("echo", {{"value", "client-roundtrip"}});
    EXPECT_FALSE(result.is_error);
    EXPECT_EQ(result.structured_content["echoed"], "client-roundtrip");
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

}  // namespace
