// A2AClient wire-protocol tests.
//
// Stand up a local httplib::Server pretending to be an A2A agent. The
// server speaks two endpoints:
//   - GET  /.well-known/agent-card.json     → AgentCard JSON
//   - POST /                                  → JSON-RPC endpoint
//
// We check the canonical shape of method names, params, and the
// task/message coercion the client performs on the result.

#include <gtest/gtest.h>
#include <neograph/a2a/client.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace neograph;
using namespace neograph::a2a;

namespace {

struct MockA2AServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    std::atomic<int>           rpc_count{0};
    std::atomic<int>           card_count{0};
    std::string                last_method;
    json                       last_params;
    std::string                last_card_request_path;

    /// Canned `result` for the next RPC. Defaults to a finished Task
    /// echoing the inbound text back as an Agent message.
    json next_result = json::parse(R"({
        "kind": "task",
        "id": "task-1",
        "contextId": "ctx-1",
        "status": {"state": "completed"},
        "history": [
            {
                "kind": "message",
                "messageId": "agent-1",
                "role": "agent",
                "parts": [{"kind": "text", "text": "hello back"}]
            }
        ]
    })");

    /// When set, the server returns this JSON-RPC error instead of
    /// `result`. Object shape: {"code": -32600, "message": "..."}.
    json forced_error = json();

    /// Card returned from /.well-known/agent-card.json.
    json card = json::parse(R"({
        "name": "mock-agent",
        "description": "test agent",
        "url": "http://127.0.0.1",
        "version": "1.0",
        "protocolVersion": "0.3.0",
        "preferredTransport": "JSONRPC",
        "capabilities": {"streaming": false, "pushNotifications": false},
        "defaultInputModes": ["text/plain"],
        "defaultOutputModes": ["text/plain"],
        "skills": []
    })");

    MockA2AServer() {
        svr.Get("/.well-known/agent-card.json",
                [this](const httplib::Request& req, httplib::Response& res) {
                    card_count.fetch_add(1, std::memory_order_relaxed);
                    last_card_request_path = req.path;
                    res.status = 200;
                    res.set_content(card.dump(), "application/json");
                });

        svr.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            rpc_count.fetch_add(1, std::memory_order_relaxed);
            int id = 0;
            try {
                auto parsed = json::parse(req.body);
                if (parsed.is_object()) {
                    id           = parsed.value("id", 0);
                    last_method  = parsed.value("method", std::string());
                    if (parsed.contains("params")) last_params = parsed["params"];
                }
            } catch (...) {}

            json envelope;
            envelope["jsonrpc"] = "2.0";
            envelope["id"]      = id;
            if (!forced_error.is_null()) {
                envelope["error"] = forced_error;
            } else {
                envelope["result"] = next_result;
            }
            res.status = 200;
            res.set_content(envelope.dump(), "application/json");
        });

        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~MockA2AServer() {
        svr.stop();
        if (t.joinable()) t.join();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

TEST(A2AClient, FetchAgentCardReadsWellKnownPath) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    auto card = client.fetch_agent_card();

    EXPECT_EQ(srv.card_count.load(), 1);
    EXPECT_EQ(srv.last_card_request_path, "/.well-known/agent-card.json");
    EXPECT_EQ(card.name,             "mock-agent");
    EXPECT_EQ(card.protocol_version, "0.3.0");
    EXPECT_EQ(card.preferred_transport, "JSONRPC");
}

TEST(A2AClient, FetchAgentCardCachesByDefault) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    (void)client.fetch_agent_card();
    EXPECT_EQ(srv.card_count.load(), 1) << "second call should hit local cache";

    (void)client.fetch_agent_card(/*force=*/true);
    EXPECT_EQ(srv.card_count.load(), 2);
}

TEST(A2AClient, SendMessageUsesCanonicalMethodName) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    auto task = client.send_message_sync("hi there");

    // Spec form (a2a-js canonical, also accepted by a2a-sdk Python ≥1.0.0
    // with `enable_v0_3_compat=True`) is slash-form. PascalCase is the
    // fallback for v1-only deployments — see
    // RpcFallsBackToPascalCaseMethodName below.
    EXPECT_EQ(srv.last_method, "message/send");
    ASSERT_TRUE(srv.last_params.contains("message"));
    auto msg = srv.last_params["message"];
    EXPECT_EQ(msg.value("kind", std::string()), "message");
    EXPECT_EQ(msg.value("role", std::string()), "user");
    auto parts = msg["parts"];
    ASSERT_TRUE(parts.is_array());
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0].value("text", std::string()), "hi there");

    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_EQ(task.history.size(), 1u);
    EXPECT_EQ(task.history[0].parts[0].text, "hello back");
}

TEST(A2AClient, SendMessagePropagatesTaskAndContextIds) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.send_message_sync("continue", "task-existing", "ctx-existing");

    auto msg = srv.last_params["message"];
    EXPECT_EQ(msg.value("taskId", std::string()),    "task-existing");
    EXPECT_EQ(msg.value("contextId", std::string()), "ctx-existing");
}

TEST(A2AClient, SendMessageHandlesMessageResult) {
    MockA2AServer srv;
    srv.next_result = json::parse(R"({
        "kind": "message",
        "messageId": "agent-only-1",
        "role": "agent",
        "taskId": "T-msg",
        "contextId": "C-msg",
        "parts": [{"kind": "text", "text": "raw msg result"}]
    })");
    A2AClient client(srv.url());
    auto task = client.send_message_sync("hi");

    // Client coerces a Message-shaped result into a Task with the
    // message in `history` so callers see one shape.
    EXPECT_EQ(task.id,         "T-msg");
    EXPECT_EQ(task.context_id, "C-msg");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_EQ(task.history.size(), 1u);
    EXPECT_EQ(task.history[0].parts[0].text, "raw msg result");
}

TEST(A2AClient, RpcErrorSurfacesAsRuntimeError) {
    MockA2AServer srv;
    srv.forced_error = json::parse(R"({"code": -32601, "message": "method not found"})");
    A2AClient client(srv.url());

    EXPECT_THROW({
        try {
            (void)client.send_message_sync("hi");
        } catch (const std::runtime_error& e) {
            // Sanity check the message contains the server's text.
            std::string what(e.what());
            EXPECT_NE(what.find("method not found"), std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

TEST(A2AClient, GetTaskUsesCanonicalMethod) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.get_task("task-x", /*history_length=*/5);

    EXPECT_EQ(srv.last_method, "tasks/get");
    EXPECT_EQ(srv.last_params.value("id", std::string()), "task-x");
    EXPECT_EQ(srv.last_params.value("historyLength", 0), 5);
}

TEST(A2AClient, CancelTaskUsesCanonicalMethod) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.cancel_task("task-y");

    EXPECT_EQ(srv.last_method, "tasks/cancel");
    EXPECT_EQ(srv.last_params.value("id", std::string()), "task-y");
}

// Standalone v1-only mock: rejects slash-form methods with -32601,
// accepts the PascalCase form. Mirrors an a2a-sdk v1 deployment without
// `enable_v0_3_compat`.
struct MockV03OnlyServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;
    std::string     last_method;

    json result_for_v1 = json::parse(R"({
        "kind": "task",
        "id": "t-v1",
        "contextId": "c-v1",
        "status": {"state": "completed"},
        "history": [{
            "kind": "message", "messageId": "agent-1", "role": "agent",
            "parts": [{"kind":"text","text":"v1 ok"}]
        }]
    })");

    MockV03OnlyServer() {
        svr.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            int id = 0; std::string method;
            try {
                auto parsed = json::parse(req.body);
                if (parsed.is_object()) {
                    id     = parsed.value("id", 0);
                    method = parsed.value("method", std::string());
                }
            } catch (...) {}
            last_method = method;

            json envelope = {{"jsonrpc", "2.0"}, {"id", id}};
            const bool is_v03 = method == "message/send" || method == "tasks/get"
                             || method == "tasks/cancel";
            if (is_v03) {
                envelope["error"] = {{"code", -32601},
                                     {"message", "Method not found"}};
            } else {
                envelope["result"] = result_for_v1;
            }
            res.status = 200;
            res.set_content(envelope.dump(), "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~MockV03OnlyServer() { svr.stop(); if (t.joinable()) t.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

TEST(A2AClient, RpcFallsBackToPascalCaseMethodName) {
    // v1-only server: rejects "message/send" → -32601; accepts "SendMessage".
    MockV03OnlyServer srv;
    A2AClient client(srv.url());
    auto task = client.send_message_sync("hi");
    EXPECT_EQ(srv.last_method, "SendMessage");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    EXPECT_EQ(task.id,           "t-v1");
}

TEST(A2AClient, NormalizeBaseUrlStripsWellKnownPath) {
    MockA2AServer srv;
    A2AClient client(srv.url() + "/.well-known/agent-card.json");
    // Discovery should still reach /.well-known/agent-card.json on root.
    auto card = client.fetch_agent_card();
    EXPECT_EQ(srv.last_card_request_path, "/.well-known/agent-card.json");
    EXPECT_EQ(card.name, "mock-agent");
}

}  // namespace
