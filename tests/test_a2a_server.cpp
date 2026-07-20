// A2AServer round-trip tests. Spin up a NeoGraph engine wrapping a
// trivial echo node, expose it over A2A, then drive it with our own
// A2AClient — that exercises both halves of the protocol bridge.
//
// Streaming is covered by SendsStatusUpdatesOverSSE: the server emits
// `Working` then `Completed` status updates and a final Task; the
// client reassembles them via send_message_stream.

#include <gtest/gtest.h>

#include <neograph/a2a/server.h>
#include <neograph/a2a/client.h>
#include <neograph/a2a/types.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::a2a;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace {

class EchoNode : public GraphNode {
  public:
    EchoNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string prompt = raw.is_string() ? raw.get<std::string>() : raw.dump();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("echo:" + prompt)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

class JsonSiteNode : public GraphNode {
  public:
    JsonSiteNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("SiteSpec ready")});
        out.writes.push_back(ChannelWrite{
            "site_spec", json{{"schemaVersion", 1}, {"name", "Demo"}}});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_echo_engine() {
    auto& factory = NodeFactory::instance();
    factory.register_type("echo",
        [](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<EchoNode>(name);
        });
    neograph::json def = {
        {"name", "echo-graph"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"echo", {{"type", "echo"}}},
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

AgentCard build_card(int port) {
    AgentCard c;
    c.name             = "test-echo";
    c.description      = "echo agent for tests";
    c.url              = "http://127.0.0.1:" + std::to_string(port) + "/";
    c.version          = "0.0.1";
    c.protocol_version = "0.3.0";
    c.preferred_transport = "JSONRPC";
    c.default_input_modes  = {"text/plain"};
    c.default_output_modes = {"text/plain"};
    c.skill_names = {"echo"};
    return c;
}

struct LiveServer {
    std::shared_ptr<GraphEngine> engine = build_echo_engine();
    AgentCard                     card  = build_card(0);
    std::unique_ptr<A2AServer>    server;
    int                           port = 0;

    LiveServer() {
        server = std::make_unique<A2AServer>(engine, card);
        ASSERT_TRUE_OR_THROW(server->start_async("127.0.0.1", 0));
        port = server->port();
    }
    ~LiveServer() { server->stop(); }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }

  private:
    static void ASSERT_TRUE_OR_THROW(bool cond) {
        if (!cond) throw std::runtime_error("server failed to start");
    }
};

TEST(A2AServer, AgentCardDiscovery) {
    LiveServer srv;
    A2AClient client(srv.url());
    auto card = client.fetch_agent_card();
    EXPECT_EQ(card.name,                "test-echo");
    EXPECT_EQ(card.preferred_transport, "JSONRPC");
}

TEST(A2AServer, MessageSendRoundTrip) {
    LiveServer srv;
    A2AClient client(srv.url());
    auto task = client.send_message_sync("hello world");
    EXPECT_EQ(task.status.state, TaskState::Completed);
    ASSERT_FALSE(task.history.empty());
    auto last = task.history.back();
    ASSERT_FALSE(last.parts.empty());
    EXPECT_EQ(last.parts[0].text, "echo:hello world");
}

TEST(A2AServer, StructuredOutputAdapterReturnsVersionedDataArtifact) {
    auto& factory = NodeFactory::instance();
    factory.register_type("json_site",
        [](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<JsonSiteNode>(name);
        });
    neograph::json def = {
        {"name", "site-spec-graph"},
        {"channels", {
            {"prompt", { {"reducer", "overwrite"} }},
            {"response", { {"reducer", "overwrite"} }},
            {"site_spec", { {"reducer", "overwrite"} }},
        }},
        {"nodes", {{"site", {{"type", "json_site"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "site"}},
            neograph::json{{"from", "site"}, {"to", "__end__"}},
        })},
    };
    NodeContext context;
    auto engine = std::shared_ptr<GraphEngine>(GraphEngine::compile(def, context));
    auto adapter = std::make_shared<StructuredOutputAdapter>(
        "neograph/site-spec", 1, "site_spec");
    A2AServer server(engine, build_card(0), adapter);
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    A2AClient client("http://127.0.0.1:" + std::to_string(server.port()));
    auto task = client.send_message_sync("compose");
    server.stop();

    ASSERT_EQ(task.artifacts.size(), 1u);
    ASSERT_FALSE(task.history.empty());
    EXPECT_EQ(task.history.back().parts[0].text, "SiteSpec ready");
    ASSERT_EQ(task.artifacts[0].parts.size(), 1u);
    const auto& part = task.artifacts[0].parts[0];
    EXPECT_EQ(part.kind, "data");
    EXPECT_EQ(part.data["contract"], "neograph/site-spec");
    EXPECT_EQ(part.data["version"], 1);
    EXPECT_EQ(part.data["value"]["name"], "Demo");
}

TEST(A2AServer, TaskGetReturnsLatestSnapshot) {
    LiveServer srv;
    A2AClient client(srv.url());
    auto task = client.send_message_sync("snapshot please");
    auto fetched = client.get_task(task.id);
    EXPECT_EQ(fetched.id, task.id);
    EXPECT_EQ(fetched.status.state, TaskState::Completed);
}

TEST(A2AServer, TaskGetMissingIdReturnsRpcError) {
    LiveServer srv;
    A2AClient client(srv.url());
    EXPECT_THROW({
        try {
            (void)client.get_task("does-not-exist");
        } catch (const std::runtime_error& e) {
            std::string what(e.what());
            EXPECT_NE(what.find("Task not found"), std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

TEST(A2AServer, SendsStatusUpdatesOverSSE) {
    LiveServer srv;
    A2AClient client(srv.url());

    std::vector<TaskState> states;
    bool                   saw_terminal_task = false;

    auto task = client.send_message_stream(
        "stream test",
        [&](const StreamEvent& ev) {
            if (ev.type == StreamEvent::Type::StatusUpdate && ev.status_update) {
                states.push_back(ev.status_update->status.state);
            }
            if (ev.type == StreamEvent::Type::Task) saw_terminal_task = true;
            return true;
        });

    EXPECT_TRUE(saw_terminal_task);
    EXPECT_EQ(task.status.state, TaskState::Completed);
    // We expect at least one Working frame and one terminal frame.
    ASSERT_GE(states.size(), 1u);
    EXPECT_EQ(states.front(), TaskState::Working);
    EXPECT_EQ(states.back(),  TaskState::Completed);
}

// Serve-mode contract: a server is expected to stay up and accept many
// independent client connections over its lifetime, not stop after the
// first round-trip. This mirrors example_a2a_server's "serve" mode where
// a separate process connects across the process boundary. Here we use
// fresh A2AClient objects (independent connections) against one server.
TEST(A2AServer, StaysUpAcrossIndependentClients) {
    LiveServer srv;
    for (int i = 0; i < 5; ++i) {
        A2AClient client(srv.url());  // fresh connection each iteration
        auto task = client.send_message_sync("ping " + std::to_string(i));
        ASSERT_EQ(task.status.state, TaskState::Completed);
        ASSERT_FALSE(task.history.empty());
        EXPECT_EQ(task.history.back().parts[0].text,
                  "echo:ping " + std::to_string(i));
        EXPECT_TRUE(srv.server->is_running());
    }
    // Server still healthy after all those round-trips — only an explicit
    // stop() (or a signal in the example) tears it down.
    EXPECT_TRUE(srv.server->is_running());
}

TEST(A2AServer, MethodNotFoundReturnsCorrectCode) {
    LiveServer srv;
    A2AClient client(srv.url());
    EXPECT_THROW({
        try {
            (void)client.rpc_call("nonsense/method", neograph::json::object());
        } catch (const std::runtime_error& e) {
            std::string what(e.what());
            EXPECT_NE(what.find("-32601"), std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

}  // namespace
