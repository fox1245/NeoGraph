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
#include <neograph/graph/cancel.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>

#include <atomic>
#include <chrono>
#include <future>
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

using namespace std::chrono_literals;

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

struct CancelProbe {
    std::atomic<int> starts{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> normal_completion{false};
};

class CancelAwareNode : public GraphNode {
  public:
    CancelAwareNode(std::string name, std::shared_ptr<CancelProbe> probe)
        : name_(std::move(name)), probe_(std::move(probe)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->starts.fetch_add(1, std::memory_order_relaxed);
        probe_->entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
                throw neograph::graph::CancelledException();
            }
            std::this_thread::sleep_for(1ms);
        }
        probe_->normal_completion.store(true, std::memory_order_release);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("completed")});
        co_return out;
    }

    std::string get_name() const override { return name_; }

  private:
    std::string                  name_;
    std::shared_ptr<CancelProbe> probe_;
};

class BlockingAdapter final : public GraphAgentAdapter {
  public:
    std::promise<void> entered;
    std::shared_future<void> release;

    neograph::json build_initial_state(const std::string& text) const override {
        const_cast<BlockingAdapter*>(this)->entered.set_value();
        release.wait();
        return GraphAgentAdapter::build_initial_state(text);
    }
};

struct OverlapProbe {
    std::atomic<int> active{0};
    std::atomic<int> max_active{0};
    std::atomic<bool> release{false};
};

class OverlapNode final : public GraphNode {
  public:
    OverlapNode(std::string name, std::shared_ptr<OverlapProbe> probe)
        : name_(std::move(name)), probe_(std::move(probe)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto active = probe_->active.fetch_add(1) + 1;
        auto max = probe_->max_active.load();
        while (active > max &&
               !probe_->max_active.compare_exchange_weak(max, active)) {}
        while (!probe_->release.load(std::memory_order_acquire)) {
            if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
                probe_->active.fetch_sub(1);
                throw neograph::graph::CancelledException();
            }
            std::this_thread::sleep_for(1ms);
        }
        probe_->active.fetch_sub(1);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("overlap")});
        co_return out;
    }

    std::string get_name() const override { return name_; }

  private:
    std::string name_;
    std::shared_ptr<OverlapProbe> probe_;
};

std::shared_ptr<GraphEngine> build_overlap_engine(
    const std::shared_ptr<OverlapProbe>& probe) {
    NodeFactory::instance().register_type("a2a_overlap",
        [probe](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<OverlapNode>(name, probe);
        });
    neograph::json def = {
        {"name", "a2a-overlap"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                      {"response", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"worker", {{"type", "a2a_overlap"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
}

struct DependencyProbe {
    std::atomic<bool> provider_started{false};
    std::atomic<bool> provider_cancelled{false};
    std::atomic<bool> tool_started{false};
    std::atomic<bool> tool_cancelled{false};
    std::shared_ptr<neograph::graph::CancelToken> token;
};

class CancellableProvider final : public neograph::Provider {
  public:
    explicit CancellableProvider(std::shared_ptr<DependencyProbe> probe)
        : probe_(std::move(probe)) {}
    asio::awaitable<neograph::ChatCompletion> complete_async(
        const neograph::CompletionParams& params) override {
        probe_->provider_started.store(true, std::memory_order_release);
        while (true) {
            if (params.cancel_token && params.cancel_token->is_cancelled()) {
                probe_->provider_cancelled.store(true, std::memory_order_release);
                throw neograph::graph::CancelledException();
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    neograph::ChatCompletion complete(const neograph::CompletionParams&) override {
        throw std::runtime_error("sync provider path not expected");
    }
    std::string get_name() const override { return "cancellable"; }
  private:
    std::shared_ptr<DependencyProbe> probe_;
};

class ProviderNode final : public GraphNode {
  public:
    ProviderNode(std::string name, std::shared_ptr<neograph::Provider> provider,
                 std::shared_ptr<DependencyProbe> probe)
        : name_(std::move(name)), provider_(std::move(provider)),
          probe_(std::move(probe)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->token = in.ctx.cancel_token;
        neograph::CompletionParams params;
        params.model = "mock";
        params.cancel_token = in.ctx.cancel_token;
        (void)co_await provider_->complete_async(params);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", json("provider")});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
    std::shared_ptr<neograph::Provider> provider_;
    std::shared_ptr<DependencyProbe> probe_;
};

std::shared_ptr<GraphEngine> build_provider_engine(
    const std::shared_ptr<neograph::Provider>& provider,
    const std::shared_ptr<DependencyProbe>& probe) {
    NodeFactory::instance().register_type("a2a_provider",
        [provider, probe](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<ProviderNode>(name, provider, probe);
        });
    neograph::json def = {
        {"name", "a2a-provider"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                      {"response", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"provider", {{"type", "a2a_provider"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "provider"}},
            neograph::json{{"from", "provider"}, {"to", "__end__"}},
        })},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
}

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

std::shared_ptr<GraphEngine> build_cancel_aware_engine(
    const std::shared_ptr<CancelProbe>& probe) {
    NodeFactory::instance().register_type("a2a_cancel_probe",
        [probe](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<CancelAwareNode>(name, probe);
        });
    neograph::json def = {
        {"name", "a2a-cancel-graph"},
        {"channels", {
            {"prompt", { {"reducer", "overwrite"} }},
            {"response", { {"reducer", "overwrite"} }},
        }},
        {"nodes", {{"worker", {{"type", "a2a_cancel_probe"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    auto unique = GraphEngine::compile(def, NodeContext{});
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

TEST(A2AServer, CancelTaskAbortsInflightGraphAndIsIdempotent) {
    auto probe = std::make_shared<CancelProbe>();
    A2AServer server(build_cancel_aware_engine(probe), build_card(0));
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string task_id = "a2a-cancel-inflight";

    std::promise<Task> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            A2AClient client(url);
            completion.set_value(client.send_message_sync("cancel me", task_id));
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });

    for (int i = 0; i < 200 && !probe->entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(probe->entered.load(std::memory_order_acquire));

    A2AClient canceller(url);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(canceller.cancel_task(task_id).status.state, TaskState::Canceled);
    EXPECT_EQ(canceller.cancel_task(task_id).status.state, TaskState::Canceled);
    ASSERT_EQ(done.wait_for(500ms), std::future_status::ready);
    const auto task = done.get();
    runner.join();
    server.stop();

    EXPECT_EQ(task.status.state, TaskState::Canceled);
    EXPECT_FALSE(probe->normal_completion.load(std::memory_order_acquire));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}

TEST(A2AServer, CancelUnknownBeforeStartIsDeterministic) {
    LiveServer srv;
    A2AClient client(srv.url());
    EXPECT_THROW({
        try {
            (void)client.cancel_task("not-started");
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("Task not found"),
                      std::string::npos);
            throw;
        }
    }, std::runtime_error);
}

TEST(A2AServer, DuplicateTaskIdIsRejectedWithoutReplacingOwner) {
    auto probe = std::make_shared<CancelProbe>();
    A2AServer server(build_cancel_aware_engine(probe), build_card(0));
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string task_id = "a2a-duplicate";

    std::promise<Task> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            A2AClient client(url);
            completion.set_value(client.send_message_sync("owner", task_id));
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });

    for (int i = 0; i < 200 && !probe->entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(probe->entered.load(std::memory_order_acquire));

    A2AClient duplicate(url);
    auto rejected = duplicate.send_message_sync("duplicate", task_id);
    EXPECT_EQ(rejected.status.state, TaskState::Rejected);
    ASSERT_FALSE(rejected.history.empty());
    EXPECT_EQ(rejected.history.back().parts[0].text,
              "task_id is already running");
    EXPECT_EQ(probe->starts.load(std::memory_order_relaxed), 1);

    A2AClient canceller(url);
    EXPECT_EQ(canceller.cancel_task(task_id).status.state, TaskState::Canceled);
    ASSERT_EQ(done.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(done.get().status.state, TaskState::Canceled);
    runner.join();
    server.stop();
}

TEST(A2AServer, CancelBeforeGraphStartStopsBeforeNodeRuns) {
    auto probe = std::make_shared<CancelProbe>();
    auto adapter = std::make_shared<BlockingAdapter>();
    std::promise<void> release;
    adapter->release = release.get_future().share();
    A2AServer server(build_cancel_aware_engine(probe), build_card(0), adapter);
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string task_id = "a2a-cancel-before-start";

    std::promise<Task> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            A2AClient client(url);
            completion.set_value(client.send_message_sync("blocked", task_id));
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });
    auto entered = adapter->entered.get_future();
    ASSERT_EQ(entered.wait_for(500ms), std::future_status::ready);

    A2AClient canceller(url);
    EXPECT_EQ(canceller.cancel_task(task_id).status.state, TaskState::Canceled);
    release.set_value();
    ASSERT_EQ(done.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(done.get().status.state, TaskState::Canceled);
    EXPECT_EQ(probe->starts.load(std::memory_order_relaxed), 0);
    runner.join();
    server.stop();
}

TEST(A2AServer, CancelAfterCompletionDoesNotChangeTask) {
    LiveServer srv;
    A2AClient client(srv.url());
    auto task = client.send_message_sync("complete", "a2a-after-complete");
    ASSERT_EQ(task.status.state, TaskState::Completed);
    auto canceled = client.cancel_task(task.id);
    EXPECT_EQ(canceled.status.state, TaskState::Completed);
    EXPECT_EQ(client.get_task(task.id).status.state, TaskState::Completed);
}

TEST(A2AServer, CancelledStreamEmitsOneTerminalStatusSequence) {
    auto probe = std::make_shared<CancelProbe>();
    A2AServer server(build_cancel_aware_engine(probe), build_card(0));
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string task_id = "a2a-stream-cancel";
    std::atomic<int> terminal_statuses{0};
    std::atomic<int> task_events{0};
    std::atomic<int> cancel_ack{0};
    std::promise<Task> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            A2AClient client(url);
            auto task = client.send_message_stream(
                "stream cancel",
                [&](const StreamEvent& ev) {
                    if (ev.type == StreamEvent::Type::StatusUpdate &&
                        ev.status_update) {
                        if (ev.status_update->status.state == TaskState::Working) {
                            A2AClient canceller(url);
                            cancel_ack.store(
                                canceller.cancel_task(task_id).status.state ==
                                    TaskState::Canceled ? 1 : -1);
                        }
                        if (ev.status_update->final) {
                            terminal_statuses.fetch_add(1);
                        }
                    }
                    if (ev.type == StreamEvent::Type::Task) task_events.fetch_add(1);
                    return true;
                }, task_id);
            completion.set_value(std::move(task));
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });
    ASSERT_EQ(done.wait_for(1s), std::future_status::ready);
    auto task = done.get();
    runner.join();
    server.stop();
    EXPECT_EQ(cancel_ack.load(), 1);
    EXPECT_EQ(task.status.state, TaskState::Canceled);
    EXPECT_EQ(terminal_statuses.load(), 1);
    EXPECT_EQ(task_events.load(), 1);
}

TEST(A2AServer, DifferentTaskRunsOverlapAndReconnectCanLookup) {
    auto probe = std::make_shared<OverlapProbe>();
    A2AServer server(build_overlap_engine(probe), build_card(0));
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    std::promise<Task> first_done, second_done;
    auto first = first_done.get_future();
    auto second = second_done.get_future();
    std::thread one([&] {
        try { first_done.set_value(A2AClient(url).send_message_sync("one", "a2a-one")); }
        catch (...) { first_done.set_exception(std::current_exception()); }
    });
    std::thread two([&] {
        try { second_done.set_value(A2AClient(url).send_message_sync("two", "a2a-two")); }
        catch (...) { second_done.set_exception(std::current_exception()); }
    });
    for (int i = 0; i < 200 && probe->active.load() < 2; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(probe->active.load(), 2);
    EXPECT_GE(probe->max_active.load(), 2);
    probe->release.store(true, std::memory_order_release);
    ASSERT_EQ(first.wait_for(500ms), std::future_status::ready);
    ASSERT_EQ(second.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(first.get().status.state, TaskState::Completed);
    EXPECT_EQ(second.get().status.state, TaskState::Completed);
    one.join();
    two.join();

    A2AClient reconnect(url);
    EXPECT_EQ(reconnect.get_task("a2a-one").status.state, TaskState::Completed);
    server.stop();
}

TEST(A2AServer, GlobalAdmissionRejectsDistinctTaskAtCap) {
    auto probe = std::make_shared<OverlapProbe>();
    A2AServer server(build_overlap_engine(probe), build_card(0));
    test::A2AServerTestAccess::set_max_inflight_runs(server, 1);
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    std::promise<Task> owner_done;
    auto owner_future = owner_done.get_future();
    std::thread owner([&] {
        try {
            owner_done.set_value(
                A2AClient(url).send_message_sync("owner", "a2a-admission-owner"));
        } catch (...) {
            owner_done.set_exception(std::current_exception());
        }
    });

    for (int i = 0; i < 200 && probe->active.load() < 1; ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(probe->active.load(), 1);

    auto rejected = A2AClient(url).send_message_sync(
        "second", "a2a-admission-second");
    EXPECT_EQ(rejected.status.state, TaskState::Rejected);
    EXPECT_EQ(rejected.status.message->parts.front().text,
              "A2A server is at its in-flight task limit");
    EXPECT_EQ(A2AClient(url).get_task("a2a-admission-owner").status.state,
              TaskState::Working);

    probe->release.store(true, std::memory_order_release);
    ASSERT_EQ(owner_future.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(owner_future.get().status.state, TaskState::Completed);
    owner.join();
    server.stop();
}

TEST(A2AServer, CancelReachesCooperativeProviderFixture) {
    auto probe = std::make_shared<DependencyProbe>();
    auto provider = std::make_shared<CancellableProvider>(probe);
    A2AServer server(build_provider_engine(provider, probe), build_card(0));
    ASSERT_TRUE(server.start_async("127.0.0.1", 0));
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port());
    const std::string task_id = "a2a-provider-cancel";
    std::promise<Task> completion;
    auto done = completion.get_future();
    std::thread runner([&] {
        try {
            completion.set_value(A2AClient(url).send_message_sync("provider", task_id));
        } catch (...) {
            completion.set_exception(std::current_exception());
        }
    });
    for (int i = 0; i < 200 && !probe->provider_started.load(); ++i) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(probe->provider_started.load());
    EXPECT_EQ(A2AClient(url).cancel_task(task_id).status.state, TaskState::Canceled);
    ASSERT_EQ(done.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(done.get().status.state, TaskState::Canceled);
    EXPECT_TRUE(probe->provider_cancelled.load());
    runner.join();
    server.stop();
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
