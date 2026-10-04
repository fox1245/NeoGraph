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
#include <neograph/a2a/a2a_caller_node.h>
#include <neograph/a2a/harness_backend.h>
#include <neograph/async/run_sync.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/state.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::a2a;

namespace {

struct MockA2AServer {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    std::atomic<int>           rpc_count{0};
    std::atomic<int>           card_count{0};
    std::mutex                 observed_mutex;
    std::vector<int>           request_ids;
    std::vector<std::string>   message_ids;
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
                    {
                        std::lock_guard<std::mutex> lock(observed_mutex);
                        last_card_request_path = req.path;
                    }
                    res.status = 200;
                    res.set_content(card.dump(), "application/json");
                });

        svr.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            rpc_count.fetch_add(1, std::memory_order_relaxed);
            int id = 0;
            json parsed;
            try {
                parsed = json::parse(req.body);
                if (parsed.is_object()) {
                    id           = parsed.value("id", 0);
                    std::lock_guard<std::mutex> lock(observed_mutex);
                    last_method  = parsed.value("method", std::string());
                    if (parsed.contains("params")) last_params = parsed["params"];
                }
            } catch (...) {}

            {
                std::lock_guard<std::mutex> lock(observed_mutex);
                request_ids.push_back(id);
                if (parsed.contains("params") && parsed["params"].contains("message"))
                    message_ids.push_back(parsed["params"]["message"].value("messageId", std::string()));
            }

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

#ifdef NEOGRAPH_TESTS_HAVE_HARNESS
TEST(A2AClient, HarnessAdapterCallsConfiguredAgent) {
    MockA2AServer srv;
    auto client = std::make_shared<A2AClient>(srv.url());
    auto executor = make_harness_capability_executor({{"research", client}});
    json tool = {
        {"id", "research.ask"},
        {"executor", {{"kind", "a2a"}, {"agent", "research"}}},
    };

    auto result = executor(tool, {{"question", "What changed?"}},
        std::make_shared<neograph::graph::CancelToken>());

    EXPECT_EQ(srv.last_method, "message/send");
    EXPECT_EQ(result["id"], "task-1");
    EXPECT_EQ(result["status"]["state"], "completed");
}
#endif

TEST(A2AClient, FetchAgentCardCachesByDefault) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    (void)client.fetch_agent_card();
    EXPECT_EQ(srv.card_count.load(), 1) << "second call should hit local cache";

    (void)client.fetch_agent_card(/*force=*/true);
    EXPECT_EQ(srv.card_count.load(), 2);
}

TEST(A2AClient, ConcurrentFirstCardFetchInitializesCacheOnce) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    constexpr int callers = 16;
    std::atomic<bool> start{false};
    std::vector<std::future<AgentCard>> futures;
    futures.reserve(callers);

    for (int i = 0; i < callers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            return client.fetch_agent_card();
        }));
    }
    start.store(true, std::memory_order_release);
    for (auto& future : futures) EXPECT_EQ(future.get().name, "mock-agent");
    EXPECT_EQ(srv.card_count.load(), 1);
}

TEST(A2AClient, ConcurrentCallsHaveUniqueRequestIdsWhileTimeoutChanges) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    constexpr int callers = 32;
    std::atomic<bool> start{false};
    std::vector<std::future<void>> futures;
    futures.reserve(callers);

    std::thread config_writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        // This exercises synchronized snapshotting, not transport-timeout
        // behavior. Keep every concurrent local request above the server's
        // worst-case accept backlog so a scheduling blip cannot turn an ID
        // uniqueness test into a timeout test.
        for (int i = 1; i <= 1000; ++i)
            client.set_timeout(std::chrono::seconds(10 + i % 3));
    });
    for (int i = 0; i < callers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)client.send_message_sync("concurrent");
        }));
    }
    start.store(true, std::memory_order_release);
    // Join before observing async worker exceptions. Otherwise a failed
    // future unwinds through a joinable std::thread and masks the actual
    // transport failure with std::terminate.
    config_writer.join();
    for (auto& future : futures) future.get();

    std::lock_guard<std::mutex> lock(srv.observed_mutex);
    std::set<int> ids(srv.request_ids.begin(), srv.request_ids.end());
    EXPECT_EQ(srv.request_ids.size(), static_cast<std::size_t>(callers));
    EXPECT_EQ(ids.size(), srv.request_ids.size());
}

TEST(A2AClient, ConcurrentSendAndGetUseUniqueRequestIds) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    constexpr int callers = 16;
    std::atomic<bool> start{false};
    std::vector<std::future<void>> futures;
    futures.reserve(callers * 2);

    for (int i = 0; i < callers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)client.send_message_sync("send");
        }));
        futures.push_back(std::async(std::launch::async, [&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            (void)client.get_task("task-concurrent");
        }));
    }
    start.store(true, std::memory_order_release);
    for (auto& future : futures) future.get();

    std::lock_guard<std::mutex> lock(srv.observed_mutex);
    std::set<int> ids(srv.request_ids.begin(), srv.request_ids.end());
    EXPECT_EQ(srv.request_ids.size(), static_cast<std::size_t>(callers * 2));
    EXPECT_EQ(ids.size(), srv.request_ids.size());
}

TEST(A2AClient, ConcurrentForcedCardRefreshesAreSerialized) {
    MockA2AServer srv;
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    constexpr int callers = 8;
    std::atomic<bool> start{false};
    std::vector<std::future<AgentCard>> futures;
    futures.reserve(callers);

    for (int i = 0; i < callers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            return client.fetch_agent_card(true);
        }));
    }
    start.store(true, std::memory_order_release);
    for (auto& future : futures) EXPECT_EQ(future.get().name, "mock-agent");
    EXPECT_EQ(srv.card_count.load(), callers + 1);
}

TEST(A2ACallerNode, RepeatedCallsUseUniqueCallScopedMessageIds) {
    MockA2AServer srv;
    auto client = std::make_shared<A2AClient>(srv.url());
    A2ACallerNode node("caller", client);
    neograph::graph::GraphState state;
    state.init_channel("prompt", neograph::graph::ReducerType::OVERWRITE,
        [](const json&, const json& incoming) { return incoming; });
    state.write("prompt", "hello");
    neograph::graph::RunContext ctx;

    for (int i = 0; i < 8; ++i)
        (void)neograph::async::run_sync(node.run({state, ctx}));

    std::lock_guard<std::mutex> lock(srv.observed_mutex);
    std::set<std::string> ids(srv.message_ids.begin(), srv.message_ids.end());
    EXPECT_EQ(srv.message_ids.size(), 8u);
    EXPECT_EQ(ids.size(), srv.message_ids.size());
    for (const auto& id : srv.message_ids) {
        EXPECT_EQ(id.rfind("ng-a2a-call-", 0), 0u);
        EXPECT_EQ(id.find("0x"), std::string::npos);
    }
}

TEST(A2ACallerNode, UsesInjectedMessageIdFactory) {
    MockA2AServer srv;
    auto client = std::make_shared<A2AClient>(srv.url());
    std::atomic<unsigned int> next_id{0};
    A2ACallerNode node(
        "caller", client, "prompt", "response",
        [&next_id] {
            return "fixture-message-" + std::to_string(
                next_id.fetch_add(1, std::memory_order_relaxed));
        });
    neograph::graph::GraphState state;
    state.init_channel("prompt", neograph::graph::ReducerType::OVERWRITE,
        [](const json&, const json& incoming) { return incoming; });
    state.write("prompt", "hello");
    neograph::graph::RunContext ctx;

    (void)neograph::async::run_sync(node.run({state, ctx}));
    (void)neograph::async::run_sync(node.run({state, ctx}));

    std::lock_guard<std::mutex> lock(srv.observed_mutex);
    ASSERT_EQ(srv.message_ids.size(), 2u);
    EXPECT_EQ(srv.message_ids[0], "fixture-message-0");
    EXPECT_EQ(srv.message_ids[1], "fixture-message-1");
}

TEST(A2ACallerNode, ConcurrentCallsUseUniqueCallScopedMessageIds) {
    MockA2AServer srv;
    auto client = std::make_shared<A2AClient>(srv.url());
    A2ACallerNode node("caller", client);
    neograph::graph::GraphState state;
    state.init_channel("prompt", neograph::graph::ReducerType::OVERWRITE,
        [](const json&, const json& incoming) { return incoming; });
    state.write("prompt", "hello");
    neograph::graph::RunContext ctx;
    constexpr int callers = 32;
    std::vector<std::future<void>> futures;
    for (int i = 0; i < callers; ++i) {
        futures.push_back(std::async(std::launch::async, [&] {
            (void)neograph::async::run_sync(node.run({state, ctx}));
        }));
    }
    for (auto& future : futures) future.get();

    std::lock_guard<std::mutex> lock(srv.observed_mutex);
    std::set<std::string> ids(srv.message_ids.begin(), srv.message_ids.end());
    EXPECT_EQ(srv.message_ids.size(), static_cast<std::size_t>(callers));
    EXPECT_EQ(ids.size(), srv.message_ids.size());
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


// ---------------------------------------------------------------------------
// A2A 1.0 wire dialect (protobuf-JSON) — modelled on a2a-sdk >= 1.0, which
// parses SendMessageRequest with protobuf `ParseDict` (so a stray `kind`
// is -32602), only knows PascalCase methods (slash-form is -32601) and
// requires the `A2A-Version: 1.0` header.
// ---------------------------------------------------------------------------
namespace {

bool has_key_deep(const json& j, const std::string& key) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == key || has_key_deep(it.value(), key)) return true;
        }
    } else if (j.is_array()) {
        for (const auto& v : j) if (has_key_deep(v, key)) return true;
    }
    return false;
}

struct MockV1Server {
    httplib::Server svr;
    std::thread     t;
    int             port = 0;

    std::mutex               mu;
    std::vector<std::string> methods;
    std::vector<std::string> versions;   ///< A2A-Version header per RPC
    std::vector<json>        bodies;     ///< params per RPC
    std::atomic<int>         rpc_count{0};

    json card = json::object();
    /// Returned as `result` for unary methods.
    json unary_result = json::parse(R"({
        "task": {
            "id": "t-1", "contextId": "c-1",
            "status": {"state": "TASK_STATE_COMPLETED",
                       "message": {"messageId": "a-1", "role": "ROLE_AGENT",
                                   "parts": [{"text": "hello from v1", "mediaType": "text/plain"}]}},
            "artifacts": [{"artifactId": "art-1",
                           "parts": [{"text": "artifact text"}]}]
        }
    })");
    /// Raw SSE body returned for SendStreamingMessage.
    std::string sse_body;
    std::string probe_sse_body;
    int stream_status = 200;

    MockV1Server() {
        card = json::parse(R"({
            "name": "v1-agent", "description": "d", "version": "1.0.0",
            "supportedInterfaces": [
                {"url": "http://127.0.0.1/", "protocolBinding": "JSONRPC",
                 "protocolVersion": "1.0"}],
            "capabilities": {"streaming": true},
            "defaultInputModes": ["text/plain"],
            "defaultOutputModes": ["text/plain"],
            "skills": []
        })");
        svr.Get("/.well-known/agent-card.json",
                [this](const httplib::Request&, httplib::Response& res) {
                    res.set_content(card.dump(), "application/json");
                });
        svr.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            rpc_count.fetch_add(1);
            auto parsed = json::parse(req.body);
            auto method = parsed.value("method", std::string());
            json params = parsed.value("params", json::object());
            {
                std::lock_guard<std::mutex> lock(mu);
                methods.push_back(method);
                versions.push_back(req.get_header_value("A2A-Version"));
                bodies.push_back(params);
            }
            json env = {{"jsonrpc", "2.0"}, {"id", parsed["id"]}};
            const bool v1_method = method == "SendMessage" || method == "GetTask"
                                || method == "CancelTask"
                                || method == "SendStreamingMessage";
            if (method == "message/stream" && !probe_sse_body.empty()) {
                res.status = stream_status;
                res.set_content(probe_sse_body, "text/event-stream");
                return;
            }
            if (!v1_method) {
                env["error"] = {{"code", -32601}, {"message", "Method not found"}};
            } else if (req.get_header_value("A2A-Version") != "1.0") {
                env["error"] = {{"code", -32009}, {"message", "version not supported"}};
            } else if (has_key_deep(params, "kind")) {
                env["error"] = {{"code", -32602}, {"message", "Invalid parameters"},
                                {"data", "Message has no field named \"kind\""}};
            } else if (method == "SendStreamingMessage") {
                res.status = stream_status;
                res.set_content(sse_body, "text/event-stream");
                return;
            } else if (method == "GetTask" || method == "CancelTask") {
                env["result"] = unary_result["task"];
            } else {
                env["result"] = unary_result;
            }
            res.set_content(env.dump(), "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        t = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 200 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~MockV1Server() { svr.stop(); if (t.joinable()) t.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

}  // namespace

TEST(A2AClientV1, CardWithV1InterfaceSendsV1RequestBody) {
    MockV1Server srv;
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    auto task = client.send_message_sync("hi v1", "task-x", "ctx-x");

    ASSERT_EQ(srv.rpc_count.load(), 1) << "no slash-form probe when the card says 1.0";
    EXPECT_EQ(srv.methods[0],  "SendMessage");
    EXPECT_EQ(srv.versions[0], "1.0");
    const auto& msg = srv.bodies[0]["message"];
    EXPECT_FALSE(has_key_deep(srv.bodies[0], "kind"));
    EXPECT_EQ(msg.value("role", std::string()), "ROLE_USER");
    EXPECT_EQ(msg.value("taskId", std::string()), "task-x");
    EXPECT_EQ(msg.value("contextId", std::string()), "ctx-x");
    ASSERT_EQ(msg["parts"].size(), 1u);
    EXPECT_EQ(msg["parts"][0].value("text", std::string()), "hi v1");
    EXPECT_EQ(client.wire_dialect(), WireDialect::V1_0);

    // {"task": ...} wrapper, TASK_STATE_* and flat Parts are decoded.
    EXPECT_EQ(task.id, "t-1");
    EXPECT_EQ(task.context_id, "c-1");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_TRUE(task.status.message.has_value());
    EXPECT_EQ(task.status.message->role, Role::Agent);
    EXPECT_EQ(task.status.message->parts[0].text, "hello from v1");
    ASSERT_EQ(task.artifacts.size(), 1u);
    EXPECT_EQ(task.artifacts[0].parts[0].text, "artifact text");
}

TEST(A2AClientV1, GetAndCancelUsePascalCaseWithVersionHeader) {
    MockV1Server srv;
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    auto got = client.get_task("t-1", 3);
    auto cancelled = client.cancel_task("t-1");

    ASSERT_EQ(srv.methods.size(), 2u);
    EXPECT_EQ(srv.methods[0], "GetTask");
    EXPECT_EQ(srv.methods[1], "CancelTask");
    EXPECT_EQ(srv.versions[0], "1.0");
    EXPECT_EQ(srv.versions[1], "1.0");
    EXPECT_EQ(srv.bodies[0].value("id", std::string()), "t-1");
    EXPECT_EQ(srv.bodies[0].value("historyLength", 0), 3);
    EXPECT_EQ(got.status.state, TaskState::Completed);   // bare Task result
    EXPECT_EQ(cancelled.id, "t-1");
}

TEST(A2AClientV1, MessageOnlyResultBecomesCompletedTask) {
    MockV1Server srv;
    srv.unary_result = json::parse(R"({
        "message": {"messageId": "m-9", "role": "ROLE_AGENT", "contextId": "c-9",
                    "parts": [{"text": "just a message"}]}
    })");
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    auto task = client.send_message_sync("hi");
    EXPECT_EQ(task.context_id, "c-9");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_EQ(task.history.size(), 1u);
    EXPECT_EQ(task.history[0].parts[0].text, "just a message");
}

TEST(A2AClientV1, WithoutCardProbeFallsBackToV1BodyAndIsRemembered) {
    // The regression: the fallback used to retry PascalCase `SendMessage`
    // with the 0.3 body (`kind` fields), which a v1 server rejects with
    // -32602 "Invalid params".
    MockV1Server srv;
    A2AClient client(srv.url());
    EXPECT_FALSE(client.wire_dialect().has_value());
    auto task = client.send_message_sync("probe");

    ASSERT_EQ(srv.rpc_count.load(), 2);
    EXPECT_EQ(srv.methods[0], "message/send");
    EXPECT_EQ(srv.methods[1], "SendMessage");
    EXPECT_EQ(srv.versions[1], "1.0");
    EXPECT_FALSE(has_key_deep(srv.bodies[1], "kind"));
    EXPECT_EQ(srv.bodies[1]["message"].value("role", std::string()), "ROLE_USER");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    EXPECT_EQ(client.wire_dialect(), WireDialect::V1_0);

    (void)client.send_message_sync("again");
    ASSERT_EQ(srv.rpc_count.load(), 3) << "probe outcome is remembered";
    EXPECT_EQ(srv.methods[2], "SendMessage");
}

TEST(A2AClientV1, LegacyCardKeepsSlashMethodsAndKindBody) {
    MockA2AServer srv;   // 0.3 card, protocolVersion "0.3.0"
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    (void)client.send_message_sync("legacy");
    EXPECT_EQ(srv.last_method, "message/send");
    EXPECT_EQ(srv.last_params["message"].value("kind", std::string()), "message");
    EXPECT_EQ(srv.last_params["message"].value("role", std::string()), "user");
    EXPECT_EQ(client.wire_dialect(), WireDialect::V0_3);
}

TEST(A2AClientV1, NoCompatibleInterfaceFailsClearlyWithoutSending) {
    MockV1Server srv;
    srv.card["supportedInterfaces"] = json::parse(R"([
        {"url": "grpc.example:443", "protocolBinding": "GRPC", "protocolVersion": "1.0"},
        {"url": "http://x/", "protocolBinding": "JSONRPC", "protocolVersion": "2.0"}
    ])");
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    try {
        (void)client.send_message_sync("hi");
        FAIL() << "expected an incompatible-interface error";
    } catch (const std::runtime_error& e) {
        std::string what = e.what();
        EXPECT_NE(what.find("no compatible interface"), std::string::npos) << what;
        EXPECT_NE(what.find("GRPC 1.0"), std::string::npos) << what;
        EXPECT_NE(what.find("JSONRPC 2.0"), std::string::npos) << what;
    }
    EXPECT_EQ(srv.rpc_count.load(), 0);
}

TEST(A2AClientV1, SelectionPrefersMatchingUrlAndPropagatesTenant) {
    MockV1Server srv;
    srv.card["supportedInterfaces"] = json::parse(
        R"([{"url": "http://elsewhere.example/a2a/", "protocolBinding": "JSONRPC",
             "protocolVersion": "0.3"},
            {"url": ")" + srv.url() + R"(/", "protocolBinding": "jsonrpc",
             "protocolVersion": "1.0", "tenant": "acme"}])");
    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    (void)client.send_message_sync("hi");
    ASSERT_EQ(srv.methods.size(), 1u);
    EXPECT_EQ(srv.methods[0], "SendMessage");   // not the earlier 0.3 entry
    EXPECT_EQ(srv.bodies[0].value("tenant", std::string()), "acme");
    (void)client.get_task("t-1");
    (void)client.cancel_task("t-1");
    srv.sse_body = "data: " + json({{"result", srv.unary_result}}).dump() + "\n\n";
    auto streamed = client.send_message_stream("tenant stream", nullptr);
    EXPECT_EQ(streamed.status.state, TaskState::Completed);
    ASSERT_EQ(srv.bodies.size(), 4u);
    for (const auto& body : srv.bodies)
        EXPECT_EQ(body.value("tenant", std::string()), "acme");
}

TEST(A2AClientV1, StreamingUsesSendStreamingMessageAndAssemblesTask) {
    MockV1Server srv;
    // sse-starlette (a2a-sdk) frames with CRLF line endings and interleaves
    // `: ping` comments; 1.0 has no `final` flag and no trailing Task.
    auto frame = [](const json& result) {
        return "data: " + json({{"jsonrpc", "2.0"}, {"id", 1}, {"result", result}}).dump()
             + "\r\n\r\n";
    };
    srv.sse_body =
        frame(json::parse(R"({"task": {"id": "t-s", "contextId": "c-s",
              "status": {"state": "TASK_STATE_SUBMITTED"}}})"))
        + ": ping\r\n\r\n"
        + frame(json::parse(R"({"statusUpdate": {"taskId": "t-s", "contextId": "c-s",
              "status": {"state": "TASK_STATE_WORKING"}}})"))
        + frame(json::parse(R"({"artifactUpdate": {"taskId": "t-s", "contextId": "c-s",
              "artifact": {"artifactId": "a", "parts": [{"text": "Hel"}]},
              "append": false, "lastChunk": false}})"))
        + frame(json::parse(R"({"artifactUpdate": {"taskId": "t-s", "contextId": "c-s",
              "artifact": {"artifactId": "a", "parts": [{"text": "lo"}]},
              "append": true, "lastChunk": true}})"))
        + frame(json::parse(R"({"statusUpdate": {"taskId": "t-s", "contextId": "c-s",
              "status": {"state": "TASK_STATE_COMPLETED"}}})"));

    A2AClient client(srv.url());
    (void)client.fetch_agent_card();
    std::vector<StreamEvent::Type> types;
    bool last_final = false;
    auto task = client.send_message_stream("stream", [&](const StreamEvent& ev) {
        types.push_back(ev.type);
        last_final = ev.is_final();
        return true;
    });

    ASSERT_EQ(srv.methods.size(), 1u);
    EXPECT_EQ(srv.methods[0], "SendStreamingMessage");
    EXPECT_EQ(srv.versions[0], "1.0");
    EXPECT_FALSE(has_key_deep(srv.bodies[0], "kind"));
    ASSERT_EQ(types.size(), 5u);
    EXPECT_EQ(types[0], StreamEvent::Type::Task);
    EXPECT_EQ(types[1], StreamEvent::Type::StatusUpdate);
    EXPECT_EQ(types[2], StreamEvent::Type::ArtifactUpdate);
    EXPECT_TRUE(last_final) << "terminal 1.0 state ends the stream";
    EXPECT_EQ(task.id, "t-s");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_EQ(task.artifacts.size(), 1u);
    ASSERT_EQ(task.artifacts[0].parts.size(), 2u);
    EXPECT_EQ(task.artifacts[0].parts[0].text + task.artifacts[0].parts[1].text, "Hello");
}

TEST(A2AClientV1, StreamingProbeSurfacesJsonRpcErrorBodyInsteadOfEmptyTask) {
    // Non-SSE JSON-RPC error replies used to be swallowed (empty Task).
    MockV1Server srv;
    srv.sse_body = "data: " + json({{"result", srv.unary_result}}).dump() + "\n\n";
    A2AClient client(srv.url());
    // No card: the 0.3 probe (`message/stream`) gets -32601, the client
    // retries as SendStreamingMessage with an actual completed task.
    auto task = client.send_message_stream("hi", nullptr);
    ASSERT_EQ(srv.methods.size(), 2u);
    EXPECT_EQ(srv.methods[0], "message/stream");
    EXPECT_EQ(srv.methods[1], "SendStreamingMessage");
    EXPECT_EQ(client.wire_dialect(), WireDialect::V1_0);
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_TRUE(task.status.message);
    EXPECT_EQ(task.status.message->parts[0].text, "hello from v1");

    // A genuine RPC error is reported, not hidden.
    MockA2AServer failing;
    failing.forced_error = json::parse(R"({"code": -32602, "message": "Invalid params"})");
    A2AClient c2(failing.url());
    try {
        (void)c2.send_message_stream("hi", nullptr);
        FAIL() << "expected A2ARpcError";
    } catch (const A2ARpcError& e) {
        EXPECT_EQ(e.code(), -32602);
    }
}


TEST(A2AClientV1, CallerNodeReturnsAnswerNotInterimHistoryNote) {
    // a2a-sdk style final Task: the user's turn and an interim "working"
    // note sit in `history`, the answer is an artifact and the terminal
    // status carries no message.
    MockV1Server srv;
    srv.unary_result = json::parse(R"({"task": {
        "id": "t-2", "contextId": "c-2",
        "status": {"state": "TASK_STATE_COMPLETED"},
        "artifacts": [{"artifactId": "a", "parts": [{"text": "final answer"}]}],
        "history": [
            {"messageId": "u", "role": "ROLE_USER",  "parts": [{"text": "question"}]},
            {"messageId": "w", "role": "ROLE_AGENT", "parts": [{"text": "working on it"}]}]
    }})");
    auto client = std::make_shared<A2AClient>(srv.url());
    (void)client->fetch_agent_card();
    A2ACallerNode node("caller", client);
    neograph::graph::GraphState state;
    state.init_channel("prompt", neograph::graph::ReducerType::OVERWRITE,
        [](const json&, const json& incoming) { return incoming; });
    state.write("prompt", "question");
    neograph::graph::RunContext ctx;

    auto out = neograph::async::run_sync(node.run({state, ctx}));
    ASSERT_FALSE(out.writes.empty());
    EXPECT_EQ(out.writes[0].value, json("final answer"));
}

TEST(A2AClientV1, OnlyNumericMethodNotFoundAllowsProbeFallback) {
    for (const auto& error : {
            json{{"code", -32602}, {"message", "Method not found (-32601)"}},
            json{{"code", "-32601"}, {"message", "Method not found"}},
            json{{"code", -32009}, {"message", "version not supported"}}}) {
        MockA2AServer server;
        server.forced_error = error;
        A2AClient client(server.url());
        if (error["code"].is_number_integer()) {
            EXPECT_THROW(client.send_message_sync("no retry"), A2ARpcError);
        } else {
            EXPECT_THROW(client.send_message_sync("no retry"), std::runtime_error);
        }
        EXPECT_EQ(server.rpc_count.load(), 1);
        EXPECT_THROW(client.send_message_stream("no retry", nullptr), A2ARpcError);
        EXPECT_EQ(server.rpc_count.load(), 2);
        EXPECT_FALSE(client.wire_dialect());
    }
}

TEST(A2AClientV1, ObservedStreamEventProhibitsDialectRedispatch) {
    for (bool install_callback : {false, true}) {
        MockV1Server server;
        server.probe_sse_body =
            "data: " + json({{"result", {{"kind", "status-update"},
                {"taskId", "observed"}, {"status", {{"state", "working"}}}}}}).dump()
            + "\n\ndata: " + json({{"error", {{"code", -32601},
                {"message", "Method not found"}}}}).dump() + "\n\n";
        A2AClient client(server.url());
        int delivered = 0;
        A2AClient::EventCallback callback;
        if (install_callback) callback = [&](const StreamEvent&) { ++delivered; return true; };
        EXPECT_THROW(client.send_message_stream("once", callback), A2ARpcError);
        EXPECT_EQ(server.rpc_count.load(), 1);
        EXPECT_EQ(delivered, install_callback ? 1 : 0);
        EXPECT_FALSE(client.wire_dialect());
    }
}

TEST(A2AClientV1, CallerAbortDoesNotReceiveLaterFramesOrRedispatch) {
    MockV1Server server;
    server.probe_sse_body =
        "data: " + json({{"result", {{"kind", "status-update"},
            {"taskId", "aborted"}, {"status", {{"state", "working"}}}}}}).dump()
        + "\n\ndata: " + json({{"result", {{"kind", "task"},
            {"id", "aborted"}, {"status", {{"state", "completed"}}}}}}).dump() + "\n\n";
    A2AClient client(server.url());
    int delivered = 0;
    auto task = client.send_message_stream("abort", [&](const StreamEvent&) {
        ++delivered;
        return false;
    });
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(server.rpc_count.load(), 1);
    EXPECT_EQ(task.id, "aborted");
    EXPECT_EQ(task.status.state, TaskState::Working);
}

TEST(A2AClientV1, MultilineCrEventsAndUnterminatedTailAssembleReplacement) {
    MockV1Server server;
    server.sse_body =
        ": comment\r\r"
        "data: {\"result\":\r"
        "data: {\"task\":{\"id\":\"t\",\"status\":{\"state\":\"TASK_STATE_SUBMITTED\"}}}}\r\r"
        "data: {\"result\":{\"artifactUpdate\":{\"taskId\":\"t\",\"artifact\":"
        "{\"artifactId\":\"a\",\"parts\":[{\"text\":\"old\"}]}}}}\r\r"
        "data: {\"result\":{\"artifactUpdate\":{\"taskId\":\"t\",\"artifact\":"
        "{\"artifactId\":\"a\",\"parts\":[{\"text\":\"replacement\"}]},\"append\":false}}}\r\r"
        "data: {\"result\":{\"statusUpdate\":{\"taskId\":\"t\",\"status\":"
        "{\"state\":\"TASK_STATE_COMPLETED\"}}}}";
    A2AClient client(server.url());
    client.fetch_agent_card();
    std::vector<bool> finals;
    auto task = client.send_message_stream("assemble", [&](const StreamEvent& event) {
        finals.push_back(event.is_final());
        return true;
    });
    EXPECT_EQ(finals, (std::vector<bool>{false, false, false, true}));
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_EQ(task.artifacts.size(), 1u);
    ASSERT_EQ(task.artifacts[0].parts.size(), 1u);
    EXPECT_EQ(task.artifacts[0].parts[0].text, "replacement");
}

TEST(A2AClientV1, StreamHttpErrorCannotBecomeSuccessfulTask) {
    MockV1Server server;
    server.stream_status = 500;
    server.sse_body = "data: " + json({{"result", server.unary_result}}).dump() + "\n\n";
    A2AClient client(server.url());
    client.fetch_agent_card();
    EXPECT_THROW(client.send_message_stream("failed", nullptr), std::runtime_error);
    EXPECT_EQ(server.rpc_count.load(), 1);
}

TEST(A2AClientV1, ForcedCardRefreshReplacesSelectedDialectAndTenant) {
    MockV1Server server;
    A2AClient client(server.url());
    client.fetch_agent_card();
    client.send_message_sync("select v1");
    server.card["supportedInterfaces"][0]["protocolVersion"] = "0.3";
    client.fetch_agent_card(true);
    EXPECT_FALSE(client.wire_dialect());
    EXPECT_THROW(client.send_message_sync("selected legacy, no fallback"), A2ARpcError);
    EXPECT_EQ(server.rpc_count.load(), 2);
    EXPECT_EQ(server.methods.back(), "message/send");
    EXPECT_EQ(client.wire_dialect(), WireDialect::V0_3);
}

TEST(A2AClientV1, CallerPrefersAgentStatusThenArtifactThenLastAgentHistory) {
    MockV1Server server;
    auto client = std::make_shared<A2AClient>(server.url());
    client->fetch_agent_card();
    A2ACallerNode node("caller", client);
    neograph::graph::GraphState state;
    state.init_channel("prompt", neograph::graph::ReducerType::OVERWRITE,
        [](const json&, const json& incoming) { return incoming; });
    state.write("prompt", "question");
    neograph::graph::RunContext context;
    auto task = server.unary_result["task"];
    task["history"] = json::array({
        {{"messageId", "agent"}, {"role", "ROLE_AGENT"}, {"parts", {{{"text", "history answer"}}}}},
        {{"messageId", "user"}, {"role", "ROLE_USER"}, {"parts", {{{"text", "user question"}}}}}});
    auto answer = [&]() {
        return neograph::async::run_sync(node.run({state, context})).writes[0].value;
    };
    EXPECT_EQ(answer(), json("hello from v1"));
    task["status"]["state"] = "TASK_STATE_WORKING";
    EXPECT_EQ(answer(), json("artifact text")); // progress is not answer authority
    task["status"]["state"] = "TASK_STATE_COMPLETED";
    task["status"]["message"]["role"] = "ROLE_USER";
    EXPECT_EQ(answer(), json("artifact text"));
    task["artifacts"] = json::array();
    EXPECT_EQ(answer(), json("history answer"));
    task["history"] = json::array({task["history"][1]});
    EXPECT_EQ(answer(), json(""));
}

TEST(A2AClientV1, SendConfigurationReachesSelectedDialectWire) {
    MessageSendParams params;
    params.message.message_id = "configured";
    params.message.parts.push_back(Part::text_part("configured request"));
    MessageSendConfiguration configuration;
    configuration.blocking = true;
    configuration.history_length = 7;
    configuration.accepted_output_modes = {"text/plain"};
    params.configuration = configuration;
    MockV1Server modern;
    A2AClient v1(modern.url());
    v1.fetch_agent_card();
    EXPECT_EQ(v1.send_message_sync(params).status.state, TaskState::Completed);
    ASSERT_EQ(modern.bodies.size(), 1u);
    EXPECT_EQ(modern.bodies[0]["configuration"],
        (json{{"returnImmediately", false}, {"historyLength", 7},
              {"acceptedOutputModes", {"text/plain"}}}));
    MockA2AServer legacy;
    A2AClient v03(legacy.url());
    v03.fetch_agent_card();
    EXPECT_EQ(v03.send_message_sync(params).status.state, TaskState::Completed);
    EXPECT_EQ(legacy.last_params["configuration"],
        (json{{"blocking", true}, {"historyLength", 7},
              {"acceptedOutputModes", {"text/plain"}}}));
}
}  // namespace
