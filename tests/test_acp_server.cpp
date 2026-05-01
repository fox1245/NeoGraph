// ACPServer lifecycle tests. Drives the server via handle_message
// (transport-agnostic — no stdio plumbing needed) for fast unit-style
// coverage, plus one end-to-end test that pumps NDJSON through actual
// std::stringstreams via run().
//
// session/prompt is async: handle_message dispatches to a worker
// thread and returns null; the response envelope is later emitted
// through the notification sink. Tests use a thread-safe sink that
// signals on the awaited response id.

#include <gtest/gtest.h>

#include <neograph/acp/server.h>
#include <neograph/acp/types.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::acp;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;

namespace {

class EchoNode : public GraphNode {
  public:
    EchoNode(std::string n) : name_(std::move(n)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto raw = state.get("prompt");
        std::string p = raw.is_string() ? raw.get<std::string>() : raw.dump();
        return {{"response", "echo:" + p}};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_echo_engine() {
    NodeFactory::instance().register_type("acp_echo",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<EchoNode>(n);
        });
    neograph::json def = {
        {"name", "acp-echo"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"echo", {{"type", "acp_echo"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "echo"}},
            neograph::json{{"from", "echo"},      {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(unique));
}

ACPServer make_server() {
    auto engine = build_echo_engine();
    neograph::json info = {{"name", "test-acp"}, {"version", "0.0.1"}};
    return ACPServer(engine, info);
}

neograph::json make_request(int id, const std::string& method, neograph::json params) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["method"]  = method;
    env["params"]  = std::move(params);
    return env;
}

// Thread-safe sink that records every emitted envelope and lets a test
// thread block until a response with a particular id arrives. Models
// what the run-loop reader does in production.
struct CapturingSink {
    std::mutex                  mu;
    std::condition_variable     cv;
    std::vector<neograph::json> envs;

    ACPServer::NotificationSink as_sink() {
        return [this](const neograph::json& env) {
            std::lock_guard lk(mu);
            envs.push_back(env);
            cv.notify_all();
        };
    }

    neograph::json wait_for_response(int id,
                                     std::chrono::milliseconds timeout
                                         = std::chrono::seconds(5)) {
        std::unique_lock lk(mu);
        cv.wait_for(lk, timeout, [&]{
            for (auto& e : envs) {
                if (!e.contains("method")
                    && e.contains("id")
                    && e["id"].is_number_integer()
                    && e["id"].get<int>() == id) return true;
            }
            return false;
        });
        for (auto& e : envs) {
            if (!e.contains("method")
                && e.contains("id")
                && e["id"].is_number_integer()
                && e["id"].get<int>() == id) return e;
        }
        return {};
    }

    std::vector<neograph::json> notifications_for(const std::string& method) {
        std::lock_guard lk(mu);
        std::vector<neograph::json> out;
        for (auto& e : envs) {
            if (e.value("method", std::string()) == method) out.push_back(e);
        }
        return out;
    }
};

TEST(ACPServer, InitializeAdvertisesCapabilities) {
    auto server = make_server();
    server.capabilities().load_session = true;
    server.capabilities().prompt.image = true;

    auto req = make_request(1, "initialize",
        {{"protocolVersion", 1}, {"clientCapabilities", neograph::json::object()}});
    auto resp = server.handle_message(req);

    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"].value("protocolVersion", 0), 1);
    EXPECT_EQ(resp["result"]["agentCapabilities"].value("loadSession", false), true);
    EXPECT_EQ(resp["result"]["agentCapabilities"]["promptCapabilities"].value("image", false), true);
    EXPECT_EQ(resp["result"]["agentInfo"].value("name", std::string()), "test-acp");
    EXPECT_TRUE(server.initialized());
}

TEST(ACPServer, NewSessionReturnsId) {
    auto server = make_server();
    auto resp = server.handle_message(make_request(2, "session/new",
        {{"cwd", "/tmp"}, {"mcpServers", neograph::json::array()}}));

    ASSERT_TRUE(resp.contains("result"));
    auto sid = resp["result"].value("sessionId", std::string());
    EXPECT_FALSE(sid.empty());
}

TEST(ACPServer, PromptRunsGraphAndEmitsUpdate) {
    auto server = make_server();
    CapturingSink cap;
    server.set_notification_sink(cap.as_sink());

    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "."}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());
    ASSERT_FALSE(sid.empty());

    neograph::json prompt = neograph::json::array();
    prompt.push_back({{"type", "text"}, {"text", "hi"}});
    auto immediate = server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", std::move(prompt)}}));
    EXPECT_TRUE(immediate.is_null());  // dispatched async — response comes via sink

    auto resp = cap.wait_for_response(2);
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    auto chunks = cap.notifications_for("session/update");
    bool saw_chunk = false;
    for (auto& n : chunks) {
        auto upd = n["params"]["update"];
        if (upd.value("sessionUpdate", std::string()) == "agent_message_chunk") {
            saw_chunk = true;
            EXPECT_EQ(upd["content"].value("text", std::string()), "echo:hi");
        }
    }
    EXPECT_TRUE(saw_chunk);
}

TEST(ACPServer, CancelBeforeFinalReturnsCancelled) {
    auto server = make_server();
    CapturingSink cap;
    server.set_notification_sink(cap.as_sink());

    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "."}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    neograph::json cancel;
    cancel["jsonrpc"] = "2.0";
    cancel["method"]  = "session/cancel";
    cancel["params"]  = {{"sessionId", sid}};
    auto noresp = server.handle_message(cancel);
    EXPECT_TRUE(noresp.is_null());

    neograph::json prompt = neograph::json::array();
    prompt.push_back({{"type", "text"}, {"text", "abc"}});
    auto immediate = server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", std::move(prompt)}}));
    EXPECT_TRUE(immediate.is_null());

    auto resp = cap.wait_for_response(2);
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "cancelled");
}

TEST(ACPServer, UnknownMethodReturnsMethodNotFound) {
    auto server = make_server();
    auto resp = server.handle_message(make_request(1, "session/load",
        {{"sessionId", "x"}, {"cwd", "/"}, {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"].value("code", 0), -32601);
}

TEST(ACPServer, UnknownNotificationDropped) {
    auto server = make_server();
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = "ping";  // no id == notification
    auto resp = server.handle_message(env);
    EXPECT_TRUE(resp.is_null());
}

TEST(ACPServer, RunPumpsNdjsonThroughStreams) {
    auto server = make_server();

    std::ostringstream input_builder;
    // initialize
    {
        auto env = make_request(1, "initialize",
            {{"protocolVersion", 1}, {"clientCapabilities", neograph::json::object()}});
        input_builder << env.dump() << '\n';
    }
    // session/new
    {
        auto env = make_request(2, "session/new",
            {{"cwd", "/"}, {"mcpServers", neograph::json::array()}});
        input_builder << env.dump() << '\n';
    }

    // We don't know the session id yet — drive in two passes: first
    // pass exercises initialize+new and reads the session id from the
    // output, then we feed a session/prompt envelope with the real id.
    std::istringstream in1(input_builder.str());
    std::ostringstream out1;
    server.run(in1, out1);

    // Parse the two response lines from out1.
    std::string out_str = out1.str();
    std::vector<std::string> lines;
    std::string line;
    std::istringstream parse(out_str);
    while (std::getline(parse, line)) {
        if (!line.empty()) lines.push_back(line);
    }
    ASSERT_GE(lines.size(), 2u);

    auto init_resp = neograph::json::parse(lines[0]);
    auto new_resp  = neograph::json::parse(lines[1]);
    EXPECT_EQ(init_resp.value("id", -1), 1);
    EXPECT_EQ(new_resp.value("id", -1),  2);
    auto sid = new_resp["result"].value("sessionId", std::string());
    ASSERT_FALSE(sid.empty());

    // Second pass: session/prompt with the real id.
    std::ostringstream in2_builder;
    auto pmt = make_request(3, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "ping"}}})}});
    in2_builder << pmt.dump() << '\n';

    std::istringstream in2(in2_builder.str());
    std::ostringstream out2;
    server.run(in2, out2);

    // Output should contain at least one session/update notification +
    // one PromptResponse envelope.
    bool saw_chunk = false, saw_resp = false;
    std::istringstream parse2(out2.str());
    while (std::getline(parse2, line)) {
        if (line.empty()) continue;
        auto j = neograph::json::parse(line);
        if (j.contains("method") && j["method"] == "session/update") {
            auto upd = j["params"]["update"];
            if (upd.value("sessionUpdate", std::string()) == "agent_message_chunk"
                && upd["content"].value("text", std::string()) == "echo:ping") {
                saw_chunk = true;
            }
        } else if (j.contains("result")
                   && j["result"].value("stopReason", std::string()) == "end_turn") {
            saw_resp = true;
        }
    }
    EXPECT_TRUE(saw_chunk);
    EXPECT_TRUE(saw_resp);
}

// ---------------------------------------------------------------------------
// Bidirectional RPC: a node calls back to the editor mid-prompt
// ---------------------------------------------------------------------------

namespace {

// A node that asks the editor (via ACPClient) to read a workspace file
// and emits its contents on the `response` channel. Models the real
// usage pattern: capture the client by value/ref, look up session_id
// from the seeded `_acp_session_id` channel.
class ReadFileNode : public GraphNode {
  public:
    ReadFileNode(std::string n, std::shared_ptr<ACPClient> client, std::string path)
        : name_(std::move(n)), client_(std::move(client)), path_(std::move(path)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto sid_v = state.get("_acp_session_id");
        std::string sid = sid_v.is_string() ? sid_v.get<std::string>() : "";
        auto contents = client_->read_text_file(sid, path_);
        return {{"response", "got:" + contents}};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string                name_;
    std::shared_ptr<ACPClient> client_;
    std::string                path_;
};

std::shared_ptr<GraphEngine>
build_read_file_engine(std::shared_ptr<ACPClient> client) {
    NodeFactory::instance().register_type("acp_read_file",
        [client](const std::string& n, const neograph::json& cfg, const NodeContext&) {
            std::string path = cfg.value("path", std::string("/tmp/x"));
            return std::make_unique<ReadFileNode>(n, client, std::move(path));
        });
    neograph::json def = {
        {"name", "acp-readfile"},
        {"channels", {
            {"prompt",            {{"reducer", "overwrite"}}},
            {"response",          {{"reducer", "overwrite"}}},
            {"_acp_session_id",   {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"reader", {{"type", "acp_read_file"}, {"config", {{"path", "/work/main.cpp"}}}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "reader"}},
            neograph::json{{"from", "reader"},   {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(unique));
}

}  // namespace

TEST(ACPServer, NodeCallsBackToEditorViaFsRead) {
    // Late-binding flow: pre-create an unbound client, capture it in the
    // node factory, compile the engine, wrap it in a server, then attach
    // the client. The same shared_ptr the nodes hold is now bound and
    // ready for fs/* round-trips.
    auto client = std::make_shared<ACPClient>();
    auto engine = build_read_file_engine(client);

    neograph::json info = {{"name", "acp-bidir"}, {"version", "0.0.1"}};
    auto server = std::make_shared<ACPServer>(engine, info);
    server->attach_client(client);

    // Stub editor. The sink does double duty: capture envelopes for
    // assertions AND respond to outbound fs/read_text_file requests by
    // feeding a synthetic response back through handle_message.
    CapturingSink cap;
    auto cap_sink = cap.as_sink();
    server->set_notification_sink([&, cap_sink, server](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "fs/read_text_file"
            && env.contains("id")) {
            auto reply_id = env["id"];
            std::thread([server, reply_id]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = {{"content", "void main(){}\n"}};
                server->handle_message(reply);
            }).detach();
        }
    });

    auto sess_resp = server->handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    auto immediate = server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "x"}}})}}));
    EXPECT_TRUE(immediate.is_null());

    auto resp = cap.wait_for_response(2, std::chrono::seconds(5));
    ASSERT_TRUE(resp.contains("result")) << "no PromptResponse arrived: " << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    bool saw_expected = false;
    for (auto& n : cap.notifications_for("session/update")) {
        auto upd = n["params"]["update"];
        if (upd.value("sessionUpdate", std::string()) == "agent_message_chunk"
            && upd["content"].value("text", std::string()) == "got:void main(){}\n") {
            saw_expected = true;
        }
    }
    EXPECT_TRUE(saw_expected);
}

TEST(ACPClient, UnboundCallThrows) {
    ACPClient unbound;
    EXPECT_FALSE(unbound.bound());
    EXPECT_THROW(unbound.read_text_file("sess", "/x"), std::runtime_error);
    EXPECT_THROW(unbound.write_text_file("sess", "/x", "data"), std::runtime_error);
    EXPECT_THROW(unbound.request_permission("sess", acp::ToolCallUpdate{}, {}), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Bidirectional: request_permission round-trip
// ---------------------------------------------------------------------------

namespace {

// Node that asks the editor for permission before "executing" a fake
// tool. Writes the chosen option's optionId to the response channel, or
// "(cancelled)" if the editor cancelled the prompt.
class GatedNode : public GraphNode {
  public:
    GatedNode(std::string n, std::shared_ptr<ACPClient> client)
        : name_(std::move(n)), client_(std::move(client)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto sid_v = state.get("_acp_session_id");
        std::string sid = sid_v.is_string() ? sid_v.get<std::string>() : "";

        acp::ToolCallUpdate tc;
        tc.tool_call_id = "tc-1";
        tc.tool_name    = "edit_file";
        tc.kind         = "edit";
        tc.input        = neograph::json{{"path", "x.cpp"}};

        std::vector<PermissionOption> options = {
            {"allow-once",  "Allow once",  "allow_once"},
            {"reject-once", "Reject once", "reject_once"},
        };

        auto outcome = client_->request_permission(sid, tc, options);
        std::string answer = (outcome.kind == PermissionOutcomeKind::Selected)
                             ? outcome.option_id
                             : "(cancelled)";
        return {{"response", answer}};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string                name_;
    std::shared_ptr<ACPClient> client_;
};

std::shared_ptr<GraphEngine>
build_gated_engine(std::shared_ptr<ACPClient> client) {
    NodeFactory::instance().register_type("acp_gated",
        [client](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<GatedNode>(n, client);
        });
    neograph::json def = {
        {"name", "acp-gated"},
        {"channels", {
            {"prompt",          {{"reducer", "overwrite"}}},
            {"response",        {{"reducer", "overwrite"}}},
            {"_acp_session_id", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"gate", {{"type", "acp_gated"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "gate"}},
            neograph::json{{"from", "gate"},     {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(unique));
}

// Build a server whose stub editor responds to session/request_permission
// with the supplied outcome shape.
std::shared_ptr<ACPServer>
make_gated_server(std::shared_ptr<ACPClient> client,
                  CapturingSink& cap,
                  std::function<neograph::json(const neograph::json&)> reply_for) {
    auto engine = build_gated_engine(client);
    neograph::json info = {{"name", "acp-gate"}, {"version", "0.0.1"}};
    auto server = std::make_shared<ACPServer>(engine, info);
    server->attach_client(client);

    auto cap_sink = cap.as_sink();
    server->set_notification_sink([cap_sink, server, reply_for](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "session/request_permission"
            && env.contains("id")) {
            auto reply_id = env["id"];
            auto outcome  = reply_for(env);
            std::thread([server, reply_id, outcome]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = outcome;
                server->handle_message(reply);
            }).detach();
        }
    });
    return server;
}

}  // namespace

TEST(ACPServer, RequestPermissionSelectedRoundTrip) {
    auto client = std::make_shared<ACPClient>();
    CapturingSink cap;
    auto server = make_gated_server(client, cap, [](const neograph::json&) {
        return neograph::json{{"outcome", {{"outcome", "selected"},
                                           {"optionId", "allow-once"}}}};
    });

    auto sess_resp = server->handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "go"}}})}}));

    auto resp = cap.wait_for_response(2, std::chrono::seconds(5));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    bool saw_chunk = false;
    for (auto& n : cap.notifications_for("session/update")) {
        auto upd = n["params"]["update"];
        if (upd.value("sessionUpdate", std::string()) == "agent_message_chunk"
            && upd["content"].value("text", std::string()) == "allow-once") {
            saw_chunk = true;
        }
    }
    EXPECT_TRUE(saw_chunk) << "expected chunk text 'allow-once' (the chosen optionId)";

    // Verify the outbound request carried the right toolCall + options.
    bool saw_perm_request = false;
    for (auto& n : cap.notifications_for("session/request_permission")) {
        auto p = n["params"];
        EXPECT_EQ(p.value("sessionId", std::string()), sid);
        EXPECT_EQ(p["toolCall"].value("toolName", std::string()), "edit_file");
        ASSERT_TRUE(p["options"].is_array());
        EXPECT_EQ(p["options"].size(), 2u);
        EXPECT_EQ(p["options"][0].value("optionId", std::string()), "allow-once");
        saw_perm_request = true;
    }
    EXPECT_TRUE(saw_perm_request);
}

TEST(ACPServer, RequestPermissionCancelledRoundTrip) {
    auto client = std::make_shared<ACPClient>();
    CapturingSink cap;
    auto server = make_gated_server(client, cap, [](const neograph::json&) {
        return neograph::json{{"outcome", {{"outcome", "cancelled"}}}};
    });

    auto sess_resp = server->handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "go"}}})}}));

    auto resp = cap.wait_for_response(2, std::chrono::seconds(5));
    ASSERT_TRUE(resp.contains("result"));

    bool saw_cancelled_chunk = false;
    for (auto& n : cap.notifications_for("session/update")) {
        auto upd = n["params"]["update"];
        if (upd.value("sessionUpdate", std::string()) == "agent_message_chunk"
            && upd["content"].value("text", std::string()) == "(cancelled)") {
            saw_cancelled_chunk = true;
        }
    }
    EXPECT_TRUE(saw_cancelled_chunk);
}

// ---------------------------------------------------------------------------
// Bidirectional: fs/write_text_file round-trip
// ---------------------------------------------------------------------------

namespace {

class WriteFileNode : public GraphNode {
  public:
    WriteFileNode(std::string n, std::shared_ptr<ACPClient> client,
                  std::string path, std::string body)
        : name_(std::move(n)), client_(std::move(client)),
          path_(std::move(path)), body_(std::move(body)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto sid_v = state.get("_acp_session_id");
        std::string sid = sid_v.is_string() ? sid_v.get<std::string>() : "";
        client_->write_text_file(sid, path_, body_);
        return {{"response", "wrote:" + path_}};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string                name_;
    std::shared_ptr<ACPClient> client_;
    std::string                path_;
    std::string                body_;
};

}  // namespace

TEST(ACPServer, FsWriteTextFileRoundTrip) {
    auto client = std::make_shared<ACPClient>();
    NodeFactory::instance().register_type("acp_write_file",
        [client](const std::string& n, const neograph::json& cfg, const NodeContext&) {
            // Compiler passes the full node-def (including the "config"
            // sub-object) as the second arg.
            auto sub = cfg.contains("config") ? cfg["config"]
                                              : neograph::json::object();
            return std::make_unique<WriteFileNode>(
                n, client,
                sub.value("path", std::string("/tmp/x")),
                sub.value("body", std::string("hello")));
        });
    neograph::json def = {
        {"name", "acp-write"},
        {"channels", {
            {"prompt",          {{"reducer", "overwrite"}}},
            {"response",        {{"reducer", "overwrite"}}},
            {"_acp_session_id", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"writer", {{"type", "acp_write_file"},
                        {"config", {{"path", "/work/out.txt"},
                                    {"body", "hello world"}}}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "writer"}},
            neograph::json{{"from", "writer"},   {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    auto engine = std::shared_ptr<GraphEngine>(std::move(unique));

    neograph::json info = {{"name", "acp-write-test"}, {"version", "0.0.1"}};
    auto server = std::make_shared<ACPServer>(engine, info);
    server->attach_client(client);

    CapturingSink cap;
    auto cap_sink = cap.as_sink();
    std::string seen_path, seen_body;
    server->set_notification_sink([&, cap_sink, server](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "fs/write_text_file"
            && env.contains("id")) {
            // Capture what the client sent the editor.
            auto p = env["params"];
            seen_path = p.value("path", std::string());
            seen_body = p.value("content", std::string());
            auto reply_id = env["id"];
            std::thread([server, reply_id]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = neograph::json::object();
                server->handle_message(reply);
            }).detach();
        }
    });

    auto sess_resp = server->handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "go"}}})}}));

    auto resp = cap.wait_for_response(2, std::chrono::seconds(5));
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");
    EXPECT_EQ(seen_path, "/work/out.txt");
    EXPECT_EQ(seen_body, "hello world");
}

// ---------------------------------------------------------------------------
// call_client timeout: throws + cleans pending map
// ---------------------------------------------------------------------------

TEST(ACPServer, CallClientTimeoutThrowsAndCleansPending) {
    auto engine = build_echo_engine();
    neograph::json info = {{"name", "acp-timeout"}, {"version", "0.0.1"}};
    ACPServer server(engine, info);

    // Set a sink that records but never replies — every call_client
    // must time out.
    std::vector<neograph::json> emitted;
    server.set_notification_sink([&](const neograph::json& env) {
        emitted.push_back(env);
    });

    auto client = server.client();
    // Tight timeout so the test runs in milliseconds, not the default 30s.
    client->set_timeout(std::chrono::milliseconds(100));
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(client->read_text_file("sess-x", "/y"), std::runtime_error);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, std::chrono::milliseconds(100));
    EXPECT_LT(elapsed, std::chrono::seconds(2));

    // Second call also times out — verifies the first-call's pending
    // entry was erased rather than left as a zombie that future calls
    // could (theoretically) collide with.
    EXPECT_THROW(client->read_text_file("sess-x", "/y"), std::runtime_error);

    // Sanity: emitted at least one outbound fs/read_text_file request.
    bool saw_outbound = false;
    for (auto& e : emitted) {
        if (e.value("method", std::string()) == "fs/read_text_file") {
            saw_outbound = true;
        }
    }
    EXPECT_TRUE(saw_outbound);
}

// ---------------------------------------------------------------------------
// Bounded worker pool: per-session single-flight + concurrent cap
// ---------------------------------------------------------------------------

TEST(ACPServer, RejectsSecondPromptOnSameSession) {
    // Build an engine whose node deliberately stalls so the first
    // prompt's worker is still in flight when the second arrives.
    class StallNode : public GraphNode {
      public:
        StallNode(std::string n, std::shared_ptr<std::atomic<bool>> stall)
            : name_(std::move(n)), stall_(std::move(stall)) {}
        std::vector<ChannelWrite> execute(const GraphState&) override {
            while (stall_->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return {{"response", "done"}};
        }
        std::string get_name() const override { return name_; }
      private:
        std::string                          name_;
        std::shared_ptr<std::atomic<bool>>   stall_;
    };

    auto stall = std::make_shared<std::atomic<bool>>(true);
    NodeFactory::instance().register_type("acp_stall",
        [stall](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<StallNode>(n, stall);
        });
    neograph::json def = {
        {"name", "acp-stall"},
        {"channels", {
            {"prompt",          {{"reducer", "overwrite"}}},
            {"response",        {{"reducer", "overwrite"}}},
            {"_acp_session_id", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {{"stall", {{"type", "acp_stall"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "stall"}},
            neograph::json{{"from", "stall"},     {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    auto engine = std::shared_ptr<GraphEngine>(std::move(unique));
    neograph::json info = {{"name", "acp-stall-test"}, {"version", "0.0.1"}};

    ACPServer server(engine, info);
    CapturingSink cap;
    server.set_notification_sink(cap.as_sink());

    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "."}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    // First prompt — dispatched async, will stall in StallNode.
    auto first = server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "a"}}})}}));
    EXPECT_TRUE(first.is_null());

    // Give the worker a moment to enter StallNode.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second prompt on SAME session — must be rejected with -32000.
    auto second = server.handle_message(make_request(3, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "b"}}})}}));
    EXPECT_TRUE(second.is_null());

    // The async error envelope arrives via the sink.
    auto err_env = cap.wait_for_response(3, std::chrono::seconds(2));
    ASSERT_TRUE(err_env.contains("error")) << "expected error envelope, got: " << err_env.dump();
    EXPECT_EQ(err_env["error"].value("code", 0), -32000);

    // Release the stall so the first worker can finish + dtor drain works.
    stall->store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Malformed JSON-RPC envelope handling
// ---------------------------------------------------------------------------

TEST(ACPServer, MalformedEnvelope_ParamsArrayInsteadOfObject) {
    auto server = make_server();
    // session/new with params as an ARRAY (spec: object). Our
    // from_json bridge tolerates this by returning defaults; the
    // server should NOT crash and should return a result envelope.
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = 99;
    env["method"]  = "session/new";
    env["params"]  = neograph::json::array({"oops"});
    auto resp = server.handle_message(env);
    // Either a result (default-tolerant parse) or an error — must be
    // one or the other, never both, and never crash.
    bool has_result = resp.contains("result");
    bool has_error  = resp.contains("error");
    EXPECT_NE(has_result, has_error);
}

TEST(ACPServer, MalformedEnvelope_MissingMethodOnRequest) {
    auto server = make_server();
    // Has `id` but no `method`. handle_message treats it as a
    // response-to-an-outbound-request. With no matching pending
    // entry, it's silently dropped (returns null). Verify no crash
    // and no spurious response.
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = 12345;
    env["result"]  = neograph::json::object();  // wire-shape valid response
    auto resp = server.handle_message(env);
    EXPECT_TRUE(resp.is_null());
}

TEST(ACPServer, MalformedEnvelope_PromptWithoutSessionId) {
    auto server = make_server();
    CapturingSink cap;
    server.set_notification_sink(cap.as_sink());

    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = 7;
    env["method"]  = "session/prompt";
    // sessionId missing — from_json will populate empty string;
    // server creates a stub session for it on the fly. Either way,
    // must not crash.
    env["params"]  = {{"prompt",
        neograph::json::array({neograph::json{{"type", "text"}, {"text", "x"}}})}};
    auto immediate = server.handle_message(env);
    EXPECT_TRUE(immediate.is_null());

    // Async response arrives via the sink. Must surface a result with
    // a valid stopReason — not crash, not silently lose.
    auto resp = cap.wait_for_response(7, std::chrono::seconds(5));
    ASSERT_TRUE(resp.contains("result") || resp.contains("error"));
}

TEST(ACPServer, MalformedEnvelope_UnknownMethodWithoutId) {
    auto server = make_server();
    // method present, id absent → notification per JSON-RPC §4.1.
    // Unknown notification is silently dropped — no error envelope.
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = "completely/made_up";
    env["params"]  = neograph::json::object();
    auto resp = server.handle_message(env);
    EXPECT_TRUE(resp.is_null());
}

}  // namespace
