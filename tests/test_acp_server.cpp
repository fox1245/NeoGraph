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
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;

namespace neograph::acp {

struct ACPServerTestAccess {
    static void fail_next_worker_launch(ACPServer& server) {
        server.fail_next_worker_launch_for_testing();
    }

    static void fail_next_handle_message(ACPServer& server) {
        server.fail_next_handle_message_for_testing();
    }
};

}  // namespace neograph::acp

namespace {

class EchoNode : public GraphNode {
  public:
    EchoNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string p = raw.is_string() ? raw.get<std::string>() : raw.dump();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json("echo:" + p)});
        co_return out;
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

class InterruptNode : public GraphNode {
  public:
    InterruptNode(std::string n,
                  std::atomic<int>* visits,
                  std::atomic<int>* side_effects)
        : name_(std::move(n)), visits_(visits), side_effects_(side_effects) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        visits_->fetch_add(1, std::memory_order_relaxed);
        if (!in.ctx.resume_value) {
            throw neograph::graph::NodeInterrupt(
                "external knowledge required",
                neograph::json{{"need", "web_search"}, {"query", "ACP resume"}});
        }

        side_effects_->fetch_add(1, std::memory_order_relaxed);
        const auto& answer = *in.ctx.resume_value;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{
            "response",
            neograph::json("resumed:" + (answer.is_string()
                                            ? answer.get<std::string>()
                                            : answer.dump()))});
        co_return out;
    }

    std::string get_name() const override { return name_; }

  private:
    std::string       name_;
    std::atomic<int>* visits_;
    std::atomic<int>* side_effects_;
};

std::shared_ptr<GraphEngine> build_interrupt_engine(
    const std::shared_ptr<neograph::graph::CheckpointStore>& store,
    std::atomic<int>* visits,
    std::atomic<int>* side_effects) {
    NodeFactory::instance().register_type(
        "acp_interrupt",
        [visits, side_effects](const std::string& n,
                               const neograph::json&,
                               const NodeContext&) {
            return std::make_unique<InterruptNode>(n, visits, side_effects);
        });
    neograph::json def = {
        {"name", "acp-interrupt"},
        {"channels", {
            {"prompt",          {{"reducer", "overwrite"}}},
            {"response",        {{"reducer", "overwrite"}}},
            {"_acp_session_id", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"knowledge", {{"type", "acp_interrupt"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"},  {"to", "knowledge"}},
            neograph::json{{"from", "knowledge"}, {"to", "__end__"}},
        })},
    };
    auto unique = GraphEngine::compile(def, NodeContext{}, store);
    return std::shared_ptr<GraphEngine>(std::move(unique));
}

class BlockingListCheckpointStore : public neograph::graph::InMemoryCheckpointStore {
  public:
    std::vector<neograph::graph::Checkpoint> list(
        const std::string& thread_id, int limit) override {
        {
            std::unique_lock lk(mu_);
            list_entered_ = true;
            cv_.notify_all();
            cv_.wait(lk, [this] { return release_list_; });
        }
        return InMemoryCheckpointStore::list(thread_id, limit);
    }

    void wait_for_list() {
        std::unique_lock lk(mu_);
        ASSERT_TRUE(cv_.wait_for(lk, std::chrono::seconds(2),
                                 [this] { return list_entered_; }));
    }

    void release_list() {
        std::lock_guard lk(mu_);
        release_list_ = true;
        cv_.notify_all();
    }

  private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    list_entered_ = false;
    bool                    release_list_ = false;
};

class ThrowingAdapter : public ACPGraphAdapter {
  public:
    enum class Stage { BuildInitialState, ExtractAgentText };

    explicit ThrowingAdapter(Stage stage) : stage_(stage) {}

    neograph::json build_initial_state(
        const std::vector<ContentBlock>& blocks,
        const std::string& session_id) const override {
        if (stage_ == Stage::BuildInitialState) {
            throw std::runtime_error("build_initial_state failed");
        }
        return ACPGraphAdapter::build_initial_state(blocks, session_id);
    }

    std::string extract_agent_text(
        const neograph::json& output) const override {
        if (stage_ == Stage::ExtractAgentText) {
            throw 42;
        }
        return ACPGraphAdapter::extract_agent_text(output);
    }

  private:
    Stage stage_;
};

class BlockingAdapter : public ACPGraphAdapter {
  public:
    explicit BlockingAdapter(std::shared_future<void> release)
        : release_(std::move(release)) {}

    std::future<void> entered_future() { return entered_.get_future(); }

    neograph::json build_initial_state(
        const std::vector<ContentBlock>& blocks,
        const std::string& session_id) const override {
        entered_.set_value();
        release_.wait();
        return ACPGraphAdapter::build_initial_state(blocks, session_id);
    }

  private:
    mutable std::promise<void> entered_;
    std::shared_future<void>   release_;
};

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
    EXPECT_TRUE(resp["result"]["agentCapabilities"]["sessionCapabilities"]
                    .contains("resume"));
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

TEST(ACPServer, InterruptMetadataAndAnswerResumeCrossProtocolBoundary) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    CapturingSink cap;
    ACPServer server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});
    server.set_notification_sink(cap.as_sink());

    auto session = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    const auto sid = session["result"].value("sessionId", std::string());

    auto prompt = [](std::string text) {
        return neograph::json::array({
            neograph::json{{"type", "text"}, {"text", std::move(text)}}});
    };
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("find it")}}));

    auto paused = cap.wait_for_response(2);
    ASSERT_TRUE(paused.contains("result")) << paused.dump();
    EXPECT_EQ(paused["result"].value("stopReason", std::string()), "end_turn");
    EXPECT_EQ(visits.load(), 1);
    EXPECT_EQ(side_effects.load(), 0);

    bool saw_interrupt = false;
    std::string checkpoint_id;
    for (const auto& notification : cap.notifications_for("session/update")) {
        const auto& update = notification["params"]["update"];
        if (!update.contains("_meta") ||
            !update["_meta"].contains("neograph/interrupt")) {
            continue;
        }
        const auto& interrupt = update["_meta"]["neograph/interrupt"];
        EXPECT_EQ(update.value("sessionUpdate", std::string()),
                  "agent_message_chunk");
        EXPECT_EQ(update["content"].value("text", std::string()),
                  "external knowledge required");
        EXPECT_EQ(interrupt.value("node", std::string()), "knowledge");
        EXPECT_EQ(interrupt.value("reason", std::string()),
                  "external knowledge required");
        EXPECT_EQ(interrupt["payload"].value("need", std::string()),
                  "web_search");
        checkpoint_id = interrupt.value("checkpointId", std::string());
        saw_interrupt = true;
    }
    EXPECT_TRUE(saw_interrupt);
    EXPECT_FALSE(checkpoint_id.empty());

    server.handle_message(make_request(3, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("approved")}}));
    auto resumed = cap.wait_for_response(3);
    ASSERT_TRUE(resumed.contains("result")) << resumed.dump();
    EXPECT_EQ(resumed["result"].value("stopReason", std::string()), "end_turn");
    EXPECT_EQ(visits.load(), 2);
    EXPECT_EQ(side_effects.load(), 1);

    bool saw_resumed_text = false;
    for (const auto& notification : cap.notifications_for("session/update")) {
        const auto& update = notification["params"]["update"];
        if (update.value("sessionUpdate", std::string()) == "agent_message_chunk" &&
            update.contains("content") &&
            update["content"].value("text", std::string()) == "resumed:approved") {
            saw_resumed_text = true;
        }
    }
    EXPECT_TRUE(saw_resumed_text);

    // A duplicate answer is a new turn, not a second resume of the consumed
    // checkpoint. The node pauses again before its side effect.
    server.handle_message(make_request(4, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("approved")}}));
    auto duplicate = cap.wait_for_response(4);
    ASSERT_TRUE(duplicate.contains("result")) << duplicate.dump();
    EXPECT_EQ(visits.load(), 3);
    EXPECT_EQ(side_effects.load(), 1);
}

TEST(ACPServer, SessionResumeRestoresPersistedInterruptWithoutReplay) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    std::string sid;

    {
        CapturingSink first_cap;
        ACPServer first(
            build_interrupt_engine(store, &visits, &side_effects),
            {{"name", "test-acp"}, {"version", "0.0.1"}});
        first.set_notification_sink(first_cap.as_sink());
        auto session = first.handle_message(make_request(1, "session/new",
            {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
        sid = session["result"].value("sessionId", std::string());
        first.handle_message(make_request(2, "session/prompt",
            {{"sessionId", sid},
             {"prompt", neograph::json::array({
                 neograph::json{{"type", "text"}, {"text", "find it"}}})}}));
        ASSERT_TRUE(first_cap.wait_for_response(2).contains("result"));
    }

    CapturingSink resumed_cap;
    ACPServer resumed_server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});
    resumed_server.set_notification_sink(resumed_cap.as_sink());
    EXPECT_TRUE(resumed_server.capabilities().session.resume);

    auto resumed_session = resumed_server.handle_message(make_request(
        3, "session/resume",
        {{"sessionId", sid},
         {"cwd", "/work"},
         {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(resumed_session.contains("result")) << resumed_session.dump();
    EXPECT_TRUE(resumed_session["result"].is_object());
    EXPECT_TRUE(resumed_cap.notifications_for("session/update").empty());

    resumed_server.handle_message(make_request(4, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "approved"}}})}}));
    auto answer = resumed_cap.wait_for_response(4);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(visits.load(), 2);
    EXPECT_EQ(side_effects.load(), 1);
    for (const auto& notification :
         resumed_cap.notifications_for("session/update")) {
        const auto& update = notification["params"]["update"];
        EXPECT_FALSE(update.contains("_meta") &&
                     update["_meta"].contains("neograph/interrupt"));
    }
}

TEST(ACPServer, SessionResumeRejectsUnknownSession) {
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    ACPServer server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});

    auto response = server.handle_message(make_request(1, "session/resume",
        {{"sessionId", "missing"},
         {"cwd", "/work"},
         {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"].value("code", 0), -32001);
}

TEST(ACPServer, PromptCannotRaceSessionResumeStateRestore) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<BlockingListCheckpointStore>();
    std::string sid;

    {
        CapturingSink first_cap;
        ACPServer first(
            build_interrupt_engine(store, &visits, &side_effects),
            {{"name", "test-acp"}, {"version", "0.0.1"}});
        first.set_notification_sink(first_cap.as_sink());
        auto session = first.handle_message(make_request(1, "session/new",
            {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
        sid = session["result"].value("sessionId", std::string());
        first.handle_message(make_request(2, "session/prompt",
            {{"sessionId", sid},
             {"prompt", neograph::json::array({
                 neograph::json{{"type", "text"}, {"text", "find it"}}})}}));
        ASSERT_TRUE(first_cap.wait_for_response(2).contains("result"));
    }

    CapturingSink cap;
    ACPServer server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});
    server.set_notification_sink(cap.as_sink());

    neograph::json resume_response;
    std::thread resume_thread([&] {
        resume_response = server.handle_message(make_request(
            3, "session/resume",
            {{"sessionId", sid},
             {"cwd", "/work"},
             {"mcpServers", neograph::json::array()}}));
    });
    store->wait_for_list();

    server.handle_message(make_request(4, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "approved"}}})}}));
    auto rejected = cap.wait_for_response(4);
    store->release_list();
    resume_thread.join();

    ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
    EXPECT_EQ(rejected["error"].value("code", 0), -32000);
    ASSERT_TRUE(resume_response.contains("result")) << resume_response.dump();
    EXPECT_EQ(visits.load(), 1);
    EXPECT_EQ(side_effects.load(), 0);
}

TEST(ACPServer, BuildInitialStateExceptionIsContained) {
    CapturingSink cap;
    auto adapter = std::make_shared<ThrowingAdapter>(
        ThrowingAdapter::Stage::BuildInitialState);
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     adapter);
    server.set_notification_sink(cap.as_sink());

    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", "build-error-session"},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    auto updates = cap.notifications_for("session/update");
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_NE(updates[0]["params"]["update"]["content"]
                  .value("text", std::string()).find("build_initial_state failed"),
              std::string::npos);
}

TEST(ACPServer, NonStdExtractAgentTextExceptionIsContained) {
    CapturingSink cap;
    auto adapter = std::make_shared<ThrowingAdapter>(
        ThrowingAdapter::Stage::ExtractAgentText);
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     adapter);
    server.set_notification_sink(cap.as_sink());

    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", "extract-error-session"},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    auto updates = cap.notifications_for("session/update");
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_NE(updates[0]["params"]["update"]["content"]
                  .value("text", std::string()).find("unknown exception"),
              std::string::npos);
}

TEST(ACPServer, NotificationSinkExceptionIsContained) {
    CapturingSink cap;
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    auto capture = cap.as_sink();
    std::atomic<bool> throw_once{true};
    server.set_notification_sink([&](const neograph::json& env) {
        if (throw_once.exchange(false, std::memory_order_acq_rel)) {
            throw std::runtime_error("sink failed");
        }
        capture(env);
    });

    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", "sink-error-session"},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"].value("code", 0), -32603);
    EXPECT_NE(resp["error"].value("message", std::string()).find("sink failed"),
              std::string::npos);
}

TEST(ACPServer, WorkerLaunchFailureRollsBackReservation) {
    CapturingSink cap;
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    server.set_notification_sink(cap.as_sink());
    ACPServerTestAccess::fail_next_worker_launch(server);

    auto prompt = [] {
        return neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "hi"}}});
    };
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", "launch-error-session"}, {"prompt", prompt()}}));

    auto failed = cap.wait_for_response(1);
    ASSERT_TRUE(failed.contains("error")) << failed.dump();
    EXPECT_EQ(failed["error"].value("code", 0), -32603);

    // The same session must be immediately reusable. If launch failure left
    // its reservation behind, this would return the single-flight -32000.
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", "launch-error-session"}, {"prompt", prompt()}}));
    auto retried = cap.wait_for_response(2);
    ASSERT_TRUE(retried.contains("result")) << retried.dump();
    EXPECT_EQ(retried["result"].value("stopReason", std::string()), "end_turn");
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

TEST(ACPServer, RunExceptionDrainsWorkerAndClearsSink) {
    std::promise<void> release;
    auto adapter = std::make_shared<BlockingAdapter>(
        release.get_future().share());
    auto entered = adapter->entered_future();
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     adapter);
    ACPServerTestAccess::fail_next_handle_message(server);

    std::ostringstream input;
    input << make_request(2, "initialize",
        {{"protocolVersion", 1},
         {"clientCapabilities", neograph::json::object()}}).dump()
          << '\n';
    input << make_request(1, "session/prompt",
        {{"sessionId", "run-error-session"},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}).dump()
          << '\n';

    std::istringstream in(input.str());
    std::ostringstream out;
    auto run = std::async(std::launch::async, [&] { server.run(in, out); });

    auto entered_status = entered.wait_for(std::chrono::seconds(5));
    if (entered_status != std::future_status::ready) {
        release.set_value();
        FAIL() << "prompt worker did not enter the blocking adapter";
    }
    EXPECT_EQ(run.wait_for(std::chrono::milliseconds(100)),
              std::future_status::timeout)
        << "run() returned without draining the in-flight worker";

    release.set_value();
    ASSERT_EQ(run.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_NO_THROW(run.get());
    EXPECT_FALSE(server.is_running());

    std::istringstream responses(out.str());
    std::string line;
    bool saw_internal_error = false;
    while (std::getline(responses, line)) {
        auto response = neograph::json::parse(line);
        if (response.value("id", 0) == 2 && response.contains("error")) {
            saw_internal_error =
                response["error"].value("code", 0) == -32603;
        }
    }
    EXPECT_TRUE(saw_internal_error) << out.str();

    // run() owns and clears its stream sink after draining the worker.
    EXPECT_THROW(server.call_client("test/no_transport", {},
                                    std::chrono::milliseconds(1)),
                 std::runtime_error);
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
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto sid_v = in.state.get("_acp_session_id");
        std::string sid = sid_v.is_string() ? sid_v.get<std::string>() : "";
        auto contents = client_->read_text_file(sid, path_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json("got:" + contents)});
        co_return out;
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
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto sid_v = in.state.get("_acp_session_id");
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
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json(answer)});
        co_return out;
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
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto sid_v = in.state.get("_acp_session_id");
        std::string sid = sid_v.is_string() ? sid_v.get<std::string>() : "";
        client_->write_text_file(sid, path_, body_);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json("wrote:" + path_)});
        co_return out;
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
        asio::awaitable<NodeOutput> run(NodeInput) override {
            while (stall_->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            NodeOutput out;
            out.writes.push_back(ChannelWrite{"response", neograph::json("done")});
            co_return out;
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

    // `cap` MUST be declared before `server` — the sink lambda captures
    // `&cap` and the worker's final `emit(jsonrpc_result(...))` for the
    // first prompt fires during `~ACPServer`'s drain. If `cap` were
    // declared after `server`, it would be destroyed first (reverse-of-
    // construction), and the worker would lock a destroyed `cap.mu` →
    // macOS libc++ aborts with EINVAL ("mutex lock failed: Invalid
    // argument"). Linux pthread limps through silently. Same gotcha
    // applies to every test that wires a stack-local sink.
    CapturingSink cap;
    ACPServer server(engine, info);
    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "."}, {"mcpServers", neograph::json::array()}}));
    auto sid = sess_resp["result"].value("sessionId", std::string());

    // Re-enter prompt handling from a rejection callback. Before the fix,
    // rejection was emitted while workers_mu was held and this callback
    // deadlocked trying to acquire the same mutex. The one-shot flag avoids
    // recursively submitting another prompt for the nested rejection.
    auto capture = cap.as_sink();
    std::atomic<bool> reenter_once{true};
    server.set_notification_sink([&](const neograph::json& env) {
        if (env.value("id", -1) == 3
            && env.contains("error")
            && reenter_once.exchange(false, std::memory_order_acq_rel)) {
            server.handle_message(make_request(4, "session/prompt",
                {{"sessionId", sid},
                 {"prompt", neograph::json::array({
                     neograph::json{{"type", "text"}, {"text", "nested"}}})}}));
        }
        capture(env);
    });

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
    auto nested_err = cap.wait_for_response(4, std::chrono::seconds(2));
    ASSERT_TRUE(nested_err.contains("error"));
    EXPECT_EQ(nested_err["error"].value("code", 0), -32000);

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
