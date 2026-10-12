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
#include <neograph/graph/cancel.h>
#include <neograph/graph/node.h>

#include "fixtures/typed_provider.h"
#include <neograph/hook_outbox.h>
#include <neograph/tool.h>
#include <neograph/types.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
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

struct CancelProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
};

class CancelAwareNode : public GraphNode {
  public:
    CancelAwareNode(std::string n, std::shared_ptr<CancelProbe> probe)
        : name_(std::move(n)), probe_(std::move(probe)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline) {
            if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
                throw neograph::graph::CancelledException();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        probe_->completed.store(true, std::memory_order_release);
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
    std::shared_ptr<CancelProbe> probe_;
};

struct DependencyProbe {
    std::atomic<bool> provider_started{false};
    std::atomic<bool> provider_release{false};
    std::atomic<bool> provider_cancelled{false};
    std::atomic<bool> tool_started{false};
    std::atomic<bool> tool_cancelled{false};
    std::shared_ptr<neograph::graph::CancelToken> token;
};

class CancellableProvider final : public neograph::test::LocalProvider {
  public:
    explicit CancellableProvider(std::shared_ptr<DependencyProbe> probe)
        : LocalProvider([probe = std::move(probe)](
              neograph::ProviderRequest request, const neograph::PreparedProviderRequest&,
              const EventCallback&) -> asio::awaitable<sp::runtime::Result> {
            probe->provider_started.store(true, std::memory_order_release);
            while (true) {
                if (probe->provider_release.load(std::memory_order_acquire))
                    co_return neograph::test::success("");
                if (request.cancel_token && request.cancel_token->is_cancelled()) {
                    probe->provider_cancelled.store(true, std::memory_order_release);
                    throw neograph::graph::CancelledException();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, "cancellable") {}
};

class CancellableTool final : public neograph::AsyncTool {
  public:
    explicit CancellableTool(std::shared_ptr<DependencyProbe> probe)
        : probe_(std::move(probe)) {}
    neograph::ChatTool get_definition() const override {
        return {"mock_tool", "mock", neograph::json::object()};
    }
    std::string get_name() const override { return "mock_tool"; }
    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        probe_->tool_started.store(true, std::memory_order_release);
        while (true) {
            if (probe_->token && probe_->token->is_cancelled()) {
                probe_->tool_cancelled.store(true, std::memory_order_release);
                throw neograph::graph::CancelledException();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
  private:
    std::shared_ptr<DependencyProbe> probe_;
};

class DependencyNode final : public GraphNode {
  public:
    DependencyNode(std::string name, std::shared_ptr<neograph::Provider> provider,
                   std::shared_ptr<CancellableTool> tool,
                   std::shared_ptr<DependencyProbe> probe)
        : name_(std::move(name)), provider_(std::move(provider)),
          tool_(std::move(tool)), probe_(std::move(probe)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->token = in.ctx.cancel_token;
        auto request = neograph::test::request("mock");
        request.cancel_token = in.ctx.cancel_token;
        (void)co_await provider_->invoke_async(std::move(request));
        (void)co_await tool_->execute_async({});
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
    std::shared_ptr<neograph::Provider> provider_;
    std::shared_ptr<CancellableTool> tool_;
    std::shared_ptr<DependencyProbe> probe_;
};

std::shared_ptr<GraphEngine> build_dependency_engine(
    const std::shared_ptr<neograph::Provider>& provider,
    const std::shared_ptr<CancellableTool>& tool,
    const std::shared_ptr<DependencyProbe>& probe) {
    NodeFactory::instance().register_type("acp_dependency",
        [provider, tool, probe](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<DependencyNode>(name, provider, tool, probe);
        });
    neograph::json def = {
        {"name", "acp-dependency"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"worker", {{"type", "acp_dependency"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
}

struct ToolDispatchProbe {
    std::atomic<bool> entered{false};
    std::atomic<bool> cancelled{false};
};

class CancellableToolDispatchTool final : public Tool,
                                          public ContextualAsyncTool {
  public:
    explicit CancellableToolDispatchTool(std::shared_ptr<ToolDispatchProbe> probe)
        : probe_(std::move(probe)) {}

    ChatTool get_definition() const override {
        return {"cancel_tool", "waits for cancellation", neograph::json::object()};
    }

    std::string execute(const neograph::json&) override {
        throw std::logic_error("context-aware tool overload was not used");
    }

    asio::awaitable<std::string> execute_async(
        const neograph::json&, ToolExecutionContext execution) override {
        if (!execution.cancel_token) {
            throw std::logic_error("missing tool cancellation token");
        }
        probe_->entered.store(true, std::memory_order_release);
        while (!execution.cancel_token->is_cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        probe_->cancelled.store(true, std::memory_order_release);
        throw neograph::graph::CancelledException("tool observed cancellation");
        co_return "";
    }

    std::string get_name() const override { return "cancel_tool"; }

  private:
    std::shared_ptr<ToolDispatchProbe> probe_;
};

class ToolCallSeedNode final : public GraphNode {
  public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        neograph::json assistant = {
            {"role", "assistant"},
            {"content", ""},
            {"tool_calls", neograph::json::array({{
                {"id", "cancel-tool-call"},
                {"name", "cancel_tool"},
                {"arguments", "{}"},
            }})},
        };
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", neograph::json::array({assistant})});
        co_return out;
    }

    std::string get_name() const override { return "seed"; }
};

std::shared_ptr<GraphEngine> build_tool_dispatch_engine(std::shared_ptr<Tool> tool) {
    NodeFactory::instance().register_type("acp_tool_call_seed",
        [](const std::string&, const neograph::json&, const NodeContext&) {
            return std::make_unique<ToolCallSeedNode>();
        });
    neograph::json def = {
        {"name", "acp-tool-dispatch"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"seed", {{"type", "acp_tool_call_seed"}}},
            {"tools", {{"type", "tool_dispatch"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "seed"}},
            neograph::json{{"from", "seed"}, {"to", "tools"}},
            neograph::json{{"from", "tools"}, {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    ctx.tools = neograph::ToolSet(std::vector<std::shared_ptr<Tool>>{std::move(tool)});
    auto unique = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(unique));
}

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

std::shared_ptr<GraphEngine> build_cancel_aware_engine(
    const std::shared_ptr<CancelProbe>& probe) {
    NodeFactory::instance().register_type("acp_cancel_probe",
        [probe](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<CancelAwareNode>(n, probe);
        });
    neograph::json def = {
        {"name", "acp-cancel"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"worker", {{"type", "acp_cancel_probe"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
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
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false, released = false;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
  public:
    std::vector<neograph::graph::Checkpoint> list(
        const std::string& thread_id, int limit) override {
        auto state = state_;
        {
            std::unique_lock lock(state->mutex);
            state->entered = true;
            state->condition.notify_all();
            state->condition.wait(lock, [state] { return state->released; });
        }
        return InMemoryCheckpointStore::list(thread_id, limit);
    }

    void wait_for_list() {
        auto state = state_;
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->condition.wait_for(lock, std::chrono::seconds(2),
                                              [state] { return state->entered; }));
    }

    void release_list() {
        std::lock_guard lock(state_->mutex);
        state_->released = true;
        state_->condition.notify_all();
    }
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

void initialize_server(ACPServer& server, bool fs_read = false,
                       bool fs_write = false) {
    neograph::json caps = neograph::json::object();
    if (fs_read || fs_write) {
        caps["fs"] = neograph::json::object();
        caps["fs"]["readTextFile"] = fs_read;
        caps["fs"]["writeTextFile"] = fs_write;
    }
    (void)server.handle_message({
        {"jsonrpc", "2.0"}, {"id", "init"}, {"method", "initialize"},
        {"params", {{"protocolVersion", 1}, {"clientCapabilities", caps}}}
    });
}

ACPServer make_server() {
    return ACPServer(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
}

neograph::json make_request(int id, const std::string& method, neograph::json params) {
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["id"]      = id;
    env["method"]  = method;
    env["params"]  = std::move(params);
    return env;
}
std::string new_session(ACPServer& server, const std::string& cwd = "/work") {
    auto response = server.handle_message(make_request(
        1000, "session/new",
        {{"cwd", cwd}, {"mcpServers", neograph::json::array()}}));
    return response["result"].value("sessionId", std::string());
}

// Thread-safe sink that records every emitted envelope and lets a test
// thread block until a response with a particular id arrives. Models
// what the run-loop reader does in production.
struct CapturingSink {
  private:
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<neograph::json> envelopes;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
  public:
    CapturingSink() : mu(state_->mutex), cv(state_->condition), envs(state_->envelopes) {}
    std::mutex& mu;
    std::condition_variable& cv;
    std::vector<neograph::json>& envs;

    ACPServer::NotificationSink as_sink() {
        return [state = state_](const neograph::json& env) {
            std::lock_guard lock(state->mutex);
            state->envelopes.push_back(env);
            state->condition.notify_all();
        };
    }

    neograph::json wait_for_response(int id,
                                     std::chrono::milliseconds timeout
                                         = std::chrono::seconds(5)) {
        std::unique_lock lk(mu);
        cv.wait_for(lk, timeout, [state = state_, id] {
            for (const auto& e : state->envelopes) {
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

    /// Wait until an envelope whose method is @p method was emitted; returns
    /// the first one, or null on timeout.
    neograph::json wait_for_method(const std::string& method,
                                   std::chrono::milliseconds timeout
                                       = std::chrono::seconds(5)) {
        std::unique_lock lk(mu);
        cv.wait_for(lk, timeout, [state = state_, &method] {
            for (const auto& e : state->envelopes) {
                if (e.value("method", std::string()) == method) return true;
            }
            return false;
        });
        for (auto& e : envs) {
            if (e.value("method", std::string()) == method) return e;
        }
        return {};
    }

    /// Every envelope emitted so far, serialized — for "never on the wire"
    /// assertions.
    std::string wire() {
        std::lock_guard lk(mu);
        std::string out;
        for (const auto& e : envs) out += e.dump() + "\n";
        return out;
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
    auto server = ACPServer(build_echo_engine(),
                            {{"name", "test-acp"}, {"version", "0.0.1"}});
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
    initialize_server(server);
    auto resp = server.handle_message(make_request(2, "session/new",
        {{"cwd", "/tmp"}, {"mcpServers", neograph::json::array()}}));

    ASSERT_TRUE(resp.contains("result"));
    auto sid = resp["result"].value("sessionId", std::string());
    EXPECT_FALSE(sid.empty());
}

TEST(ACPServer, PromptRunsGraphAndEmitsUpdate) {
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());

    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
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

/// A sink that answers session/request_permission through handle_message
/// with @p reply_for(request) (an ACP RequestPermissionResponse body).
ACPServer::NotificationSink answering_sink(
    CapturingSink& cap, std::weak_ptr<ACPServer> weak_server,
    std::function<neograph::json(const neograph::json&)> reply_for) {
    auto capture = cap.as_sink();
    return [capture, weak_server, reply_for](const neograph::json& env) {
        capture(env);
        if (env.value("method", std::string()) == "session/request_permission"
            && env.contains("id")) {
            auto reply_id = env["id"];
            auto body = reply_for(env);
            std::thread([weak_server, reply_id, body]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = body;
                if (auto server = weak_server.lock()) server->handle_message(reply);
            }).detach();
        }
    };
}

neograph::json select_option(const std::string& option_id) {
    return neograph::json{{"outcome", {{"outcome", "selected"}, {"optionId", option_id}}}};
}

TEST(ACPServer, InterruptAsksPermissionAndResumesInTheSameTurn) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    CapturingSink cap;
    auto server = std::make_shared<ACPServer>(
        build_interrupt_engine(store, &visits, &side_effects),
        neograph::json{{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(*server);
    std::atomic<int> answers{0};
    server->set_notification_sink(answering_sink(
        cap, server, [&answers](const neograph::json&) {
            return select_option(answers.fetch_add(1) == 0 ? "approve" : "reject");
        }));

    const auto sid = new_session(*server);
    auto prompt = [](std::string text) {
        return neograph::json::array({
            neograph::json{{"type", "text"}, {"text", std::move(text)}}});
    };
    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("find it")}}));

    auto done = cap.wait_for_response(2);
    ASSERT_TRUE(done.contains("result")) << done.dump();
    EXPECT_EQ(done["result"].value("stopReason", std::string()), "end_turn");
    EXPECT_EQ(visits.load(), 2);
    EXPECT_EQ(side_effects.load(), 1);

    auto asks = cap.notifications_for("session/request_permission");
    ASSERT_EQ(asks.size(), 1u);
    const auto params = asks[0]["params"];
    EXPECT_EQ(params.value("sessionId", std::string()), sid);
    const auto call = params["toolCall"];
    EXPECT_EQ(call.value("title", std::string()), "external knowledge required");
    EXPECT_EQ(call.value("kind", std::string()), "other");
    EXPECT_EQ(call.value("status", std::string()), "pending");
    EXPECT_EQ(call["rawInput"].value("need", std::string()), "web_search");
    const auto interrupt = call["_meta"]["neograph/interrupt"];
    EXPECT_EQ(interrupt.value("node", std::string()), "knowledge");
    EXPECT_FALSE(interrupt.value("checkpointId", std::string()).empty());
    ASSERT_EQ(params["options"].size(), 2u);
    EXPECT_EQ(params["options"][0].value("kind", std::string()), "allow_once");
    EXPECT_EQ(params["options"][1].value("kind", std::string()), "reject_once");

    bool saw_resumed_text = false;
    for (const auto& notification : cap.notifications_for("session/update")) {
        const auto update = notification["params"]["update"];
        EXPECT_FALSE(update.contains("_meta"));
        if (update.value("sessionUpdate", std::string()) == "agent_message_chunk" &&
            update["content"].value("text", std::string()) == "resumed:approve") {
            saw_resumed_text = true;
        }
    }
    EXPECT_TRUE(saw_resumed_text);

    // The next prompt is a new turn: the node pauses again and the second
    // answer resumes that run.
    server->handle_message(make_request(3, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("again")}}));
    auto second = cap.wait_for_response(3);
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(visits.load(), 4);
    EXPECT_EQ(side_effects.load(), 2);
    EXPECT_NE(cap.wire().find("resumed:reject"), std::string::npos);
}

TEST(ACPServer, InterruptPermissionCancelledEndsTheTurnCancelled) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    CapturingSink cap;
    auto server = std::make_shared<ACPServer>(
        build_interrupt_engine(store, &visits, &side_effects),
        neograph::json{{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(*server);
    server->set_notification_sink(answering_sink(cap, server, [](const neograph::json&) {
        return neograph::json{{"outcome", {{"outcome", "cancelled"}}}};
    }));
    const auto sid = new_session(*server);
    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "find it"}}})}}));
    auto done = cap.wait_for_response(2);
    ASSERT_TRUE(done.contains("result")) << done.dump();
    EXPECT_EQ(done["result"].value("stopReason", std::string()), "cancelled");
    EXPECT_EQ(side_effects.load(), 0);
}

class RefusingInterruptAdapter final : public ACPGraphAdapter {
  public:
    std::optional<ACPInterruptPermission> interrupt_permission(
        const ACPInterrupt&) const override {
        return std::nullopt;
    }
};

TEST(ACPServer, InterruptTheAdapterCannotPresentIsAJsonRpcError) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    CapturingSink cap;
    ACPServer server(build_interrupt_engine(store, &visits, &side_effects),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     std::make_shared<RefusingInterruptAdapter>());
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "find it"}}})}}));
    auto failed = cap.wait_for_response(2);
    ASSERT_TRUE(failed.contains("error")) << failed.dump();
    EXPECT_EQ(failed["error"].value("code", 0), -32603);
    EXPECT_EQ(failed["error"]["data"].value("code", std::string()), "NG.Interrupted");
    EXPECT_TRUE(cap.notifications_for("session/request_permission").empty());

    // The session is not left paused: the next prompt starts a new run.
    server.handle_message(make_request(3, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "answer"}}})}}));
    auto next = cap.wait_for_response(3);
    ASSERT_TRUE(next.contains("error")) << next.dump();
    EXPECT_EQ(visits.load(), 2);
    EXPECT_EQ(side_effects.load(), 0);
}

/// Leave a persisted pause behind: prompt, wait until the turn asks for
/// permission, then destroy the server while the request is unanswered.
std::string pause_session_in_store(
    const std::shared_ptr<neograph::graph::CheckpointStore>& store,
    std::atomic<int>* visits, std::atomic<int>* side_effects) {
    CapturingSink cap;
    auto first = std::make_unique<ACPServer>(
        build_interrupt_engine(store, visits, side_effects),
        neograph::json{{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(*first);
    first->set_notification_sink(cap.as_sink());
    const auto sid = new_session(*first);
    first->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "find it"}}})}}));
    EXPECT_FALSE(cap.wait_for_method("session/request_permission").is_null());
    first.reset();
    return sid;
}

TEST(ACPServer, SessionResumeRestoresPersistedInterruptWithoutReplay) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<neograph::graph::InMemoryCheckpointStore>();
    const std::string sid = pause_session_in_store(store, &visits, &side_effects);
    ASSERT_EQ(visits.load(), 1);

    CapturingSink resumed_cap;
    ACPServer resumed_server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(resumed_server);
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
    // A pause restored by session/resume is answered by the next prompt.
    auto answer = resumed_cap.wait_for_response(4);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    EXPECT_EQ(visits.load(), 2);
    EXPECT_EQ(side_effects.load(), 1);
    EXPECT_NE(resumed_cap.wire().find("resumed:approved"), std::string::npos);
    EXPECT_TRUE(resumed_cap.notifications_for("session/request_permission").empty());
}

TEST(ACPServer, SessionResumeRejectsUnknownSession) {
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    auto response = server.handle_message(make_request(1, "session/resume",
        {{"sessionId", "missing"},
         {"cwd", "/work"},
         {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    // ACP ErrorCode -32002 "Resource not found", the code session/prompt uses.
    EXPECT_EQ(response["error"].value("code", 0), -32002);
}

TEST(ACPServer, SessionResumeOfALiveSessionWithoutCheckpointSucceeds) {
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    const auto sid = new_session(server, "/work");
    auto response = server.handle_message(make_request(1, "session/resume",
        {{"sessionId", sid},
         {"cwd", "/work"},
         {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_TRUE(response["result"].is_object());
}

TEST(ACPServer, PromptCannotRaceSessionResumeStateRestore) {
    std::atomic<int> visits{0};
    std::atomic<int> side_effects{0};
    auto store = std::make_shared<BlockingListCheckpointStore>();
    const std::string sid = pause_session_in_store(store, &visits, &side_effects);

    CapturingSink cap;
    ACPServer server(
        build_interrupt_engine(store, &visits, &side_effects),
        {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
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
    // Not -32000: ACP reserves it for "Authentication required".
    EXPECT_EQ(rejected["error"].value("code", 0), -32603);
    EXPECT_EQ(rejected["error"]["data"].value("reason", std::string()), "session_busy");
    ASSERT_TRUE(resume_response.contains("result")) << resume_response.dump();
    EXPECT_EQ(visits.load(), 1);
    EXPECT_EQ(side_effects.load(), 0);
}

TEST(ACPServer, DestructorJoinsPromptThreadLocalCleanup) {
    struct ExitState {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
    };
    struct ExitGuard {
        std::shared_ptr<ExitState> state;
        ~ExitGuard() {
            if (!state) return;
            state->entered.set_value();
            state->released.wait();
        }
    };
    class ExitAdapter final : public ACPGraphAdapter {
    public:
        explicit ExitAdapter(std::shared_ptr<ExitState> state) : state_(std::move(state)) {}
        neograph::json build_initial_state(const std::vector<ContentBlock>& blocks,
                                          const std::string& session_id) const override {
            thread_local ExitGuard guard;
            guard.state = state_;
            return ACPGraphAdapter::build_initial_state(blocks, session_id);
        }
    private:
        std::shared_ptr<ExitState> state_;
    };

    auto state = std::make_shared<ExitState>();
    auto entered = state->entered.get_future();
    CapturingSink cap;
    auto server = std::make_unique<ACPServer>(
        build_echo_engine(), neograph::json{{"name", "test-acp"}, {"version", "0.0.1"}},
        std::make_shared<ExitAdapter>(state));
    initialize_server(*server);
    server->set_notification_sink(cap.as_sink());
    const auto sid = new_session(*server);
    server->handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "hi"}}})}}));
    EXPECT_TRUE(cap.wait_for_response(2).contains("result"));
    const auto exit_started = entered.wait_for(std::chrono::seconds(5));
    if (exit_started != std::future_status::ready) {
        state->release.set_value();
        FAIL() << "Prompt worker did not reach thread-local cleanup";
    }
    auto destroyed = std::async(std::launch::async, [&] { server.reset(); });
    const auto before_release = destroyed.wait_for(std::chrono::milliseconds(50));
    state->release.set_value();
    destroyed.get();
    EXPECT_EQ(before_release, std::future_status::timeout);
}

TEST(ACPServer, BuildInitialStateExceptionIsContained) {
    CapturingSink cap;
    auto adapter = std::make_shared<ThrowingAdapter>(
        ThrowingAdapter::Stage::BuildInitialState);
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     adapter);
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    // A failed turn is a JSON-RPC error, and the adapter's exception text
    // stays local.
    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"].value("code", 0), -32603);
    EXPECT_EQ(resp["error"]["data"].value("code", std::string()), "NG.TaskFailed");
    EXPECT_TRUE(cap.notifications_for("session/update").empty());
    EXPECT_EQ(cap.wire().find("build_initial_state failed"), std::string::npos);
}

TEST(ACPServer, NonStdExtractAgentTextExceptionIsContained) {
    CapturingSink cap;
    auto adapter = std::make_shared<ThrowingAdapter>(
        ThrowingAdapter::Stage::ExtractAgentText);
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}},
                     adapter);
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"].value("code", 0), -32603);
    EXPECT_EQ(resp["error"]["data"].value("code", std::string()), "NG.UnknownFailure");
    EXPECT_TRUE(cap.notifications_for("session/update").empty());
}

TEST(ACPServer, NotificationSinkExceptionIsContained) {
    CapturingSink cap;
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    auto capture = cap.as_sink();
    std::atomic<bool> throw_once{true};
    server.set_notification_sink([&](const neograph::json& env) {
        if (throw_once.exchange(false, std::memory_order_acq_rel)) {
            throw std::runtime_error("sink failed");
        }
        capture(env);
    });
    const auto sid = new_session(server);

    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({
             neograph::json{{"type", "text"}, {"text", "hi"}}})}}));

    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ(resp["error"].value("code", 0), -32603);
    // The host's own exception text is not sent to the client.
    EXPECT_EQ(resp["error"].value("message", std::string()), "Internal error");
    EXPECT_EQ(cap.wire().find("sink failed"), std::string::npos);
}

TEST(ACPServer, WorkerLaunchFailureRollsBackReservation) {
    CapturingSink cap;
    ACPServer server(build_echo_engine(),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    ACPServerTestAccess::fail_next_worker_launch(server);

    auto prompt = [] {
        return neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "hi"}}});
    };
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt()}}));

    auto failed = cap.wait_for_response(1);
    ASSERT_TRUE(failed.contains("error")) << failed.dump();
    EXPECT_EQ(failed["error"].value("code", 0), -32603);

    // The same session must be immediately reusable. If launch failure left
    // its reservation behind, this would return the single-flight rejection.
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt()}}));
    auto retried = cap.wait_for_response(2);
    ASSERT_TRUE(retried.contains("result")) << retried.dump();
    EXPECT_EQ(retried["result"].value("stopReason", std::string()), "end_turn");
}

TEST(ACPServer, StrayCancelBeforeTheFirstPromptIsIgnored) {
    // session/cancel cancels an ongoing turn; with none running it is stale
    // and must not cancel a prompt the client sends later.
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "abc"}}})}}));
    auto resp = cap.wait_for_response(2);
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");
}

TEST(ACPServer, PreArmedCancelIsAnEmbedderOptIn) {
    CapturingSink cap;
    auto server = make_server();
    server.set_pre_armed_cancel(true);
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());

    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
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

TEST(ACPServer, CancelAbortsInflightPrompt) {
    auto probe = std::make_shared<CancelProbe>();
    CapturingSink cap;
    ACPServer server(build_cancel_aware_engine(probe),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const std::string sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "cancel"}}})}}));
    for (int i = 0; i < 200 && !probe->entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(probe->entered.load(std::memory_order_acquire));
    neograph::json cancel = {{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                              {"params", {{"sessionId", sid}}}};
    server.handle_message(cancel);
    server.handle_message(cancel);
    auto response = cap.wait_for_response(1, std::chrono::milliseconds(500));
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_EQ(response["result"].value("stopReason", std::string()), "cancelled");
    EXPECT_FALSE(probe->completed.load(std::memory_order_acquire));
    int response_count = 0;
    for (const auto& env : cap.envs) {
        if (env.value("id", 0) == 1 && env.contains("result")) ++response_count;
    }
    EXPECT_EQ(response_count, 1);
}

TEST(ACPServer, CancelReachesProviderAndToolFixtures) {
    CapturingSink cap;
    auto probe = std::make_shared<DependencyProbe>();
    auto provider = std::make_shared<CancellableProvider>(probe);
    auto tool = std::make_shared<CancellableTool>(probe);
    ACPServer server(build_dependency_engine(provider, tool, probe),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const std::string sid = new_session(server);
    const std::string tool_sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "provider"}}})}}));
    for (int i = 0; i < 200 && !probe->provider_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(probe->provider_started.load());
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    auto canceled_provider = cap.wait_for_response(1);
    ASSERT_TRUE(canceled_provider.contains("result"));
    EXPECT_EQ(canceled_provider["result"].value("stopReason", std::string()),
              "cancelled");
    EXPECT_TRUE(probe->provider_cancelled.load());

    probe->provider_release.store(true, std::memory_order_release);
    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", tool_sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "tool"}}})}}));
    for (int i = 0; i < 200 && !probe->tool_started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(probe->tool_started.load());
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", tool_sid}}}});
    auto canceled_tool = cap.wait_for_response(2);
    ASSERT_TRUE(canceled_tool.contains("result"));
    EXPECT_EQ(canceled_tool["result"].value("stopReason", std::string()),
              "cancelled");
    EXPECT_TRUE(probe->tool_cancelled.load());
}

TEST(ACPServer, CancelReachesToolDispatchAndEmitsOneTerminalResponse) {
    auto probe = std::make_shared<ToolDispatchProbe>();
    auto tool = std::make_shared<CancellableToolDispatchTool>(probe);
    CapturingSink cap;
    ACPServer server(build_tool_dispatch_engine(tool),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const std::string sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "tool"}}})}}));

    for (int i = 0; i < 200 && !probe->entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(probe->entered.load(std::memory_order_acquire));
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});

    auto response = cap.wait_for_response(1, std::chrono::milliseconds(500));
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_EQ(response["result"].value("stopReason", std::string()), "cancelled");
    EXPECT_TRUE(probe->cancelled.load(std::memory_order_acquire));
    // The tool call was announced while it ran; it never completed and the
    // cancelled turn sends no agent text.
    for (const auto& n : cap.notifications_for("session/update")) {
        const auto update = n["params"]["update"];
        EXPECT_EQ(update.value("sessionUpdate", std::string()), "tool_call") << n.dump();
        EXPECT_EQ(update.value("status", std::string()), "in_progress") << n.dump();
    }

    int responses = 0;
    {
        std::lock_guard lk(cap.mu);
        for (const auto& env : cap.envs) {
            if (!env.contains("method") && env.value("id", 0) == 1
                && env.contains("result")) {
                ++responses;
            }
        }
    }
    EXPECT_EQ(responses, 1);
}

TEST(ACPServer, CancelDuringFinalUpdateWinsTerminalResponse) {
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    auto cap_sink = cap.as_sink();
    std::promise<void> update_entered;
    auto update_ready = update_entered.get_future();
    std::promise<void> release_update;
    auto release = release_update.get_future().share();
    std::atomic<bool> blocked{false};
    server.set_notification_sink([&](const neograph::json& env) {
        if (env.value("method", std::string()) == "session/update" &&
            !blocked.exchange(true, std::memory_order_acq_rel)) {
            update_entered.set_value();
            release.wait();
        }
        cap_sink(env);
    });
    const std::string sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "done"}}})}}));
    ASSERT_EQ(update_ready.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    release_update.set_value();

    auto response = cap.wait_for_response(1, std::chrono::seconds(1));
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_EQ(response["result"].value("stopReason", std::string()), "cancelled");
    EXPECT_EQ(cap.notifications_for("session/update").size(), 1u);

    std::lock_guard lock(cap.mu);
    int responses = 0;
    for (const auto& env : cap.envs) {
        if (!env.contains("method") && env.value("id", 0) == 1 &&
            env.contains("result")) {
            ++responses;
        }
    }
    EXPECT_EQ(responses, 1);
}

TEST(ACPServer, CancelAfterTerminalCommitCannotReachLaterPrompt) {
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    auto cap_sink = cap.as_sink();
    std::promise<void> response_entered;
    auto response_ready = response_entered.get_future();
    std::promise<void> release_response;
    auto release = release_response.get_future().share();
    std::atomic<bool> blocked{false};
    server.set_notification_sink([&](const neograph::json& env) {
        if (!env.contains("method") && env.value("id", 0) == 1 &&
            env.contains("result") &&
            !blocked.exchange(true, std::memory_order_acq_rel)) {
            response_entered.set_value();
            release.wait();
        }
        cap_sink(env);
    });
    const std::string sid = new_session(server);
    auto prompt = [](const char* text) {
        return neograph::json::array({
            neograph::json{{"type", "text"}, {"text", text}}});
    };
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("first")}}));
    ASSERT_EQ(response_ready.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);

    // The first response has committed its terminal state but is still blocked
    // in the sink. With no newer prompt active, this cancel is stale rather
    // than a pre-cancel for a later turn.
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    release_response.set_value();

    auto first = cap.wait_for_response(1, std::chrono::seconds(1));
    ASSERT_TRUE(first.contains("result")) << first.dump();
    EXPECT_EQ(first["result"].value("stopReason", std::string()), "end_turn");

    server.handle_message(make_request(2, "session/prompt",
        {{"sessionId", sid}, {"prompt", prompt("second")}}));
    auto second = cap.wait_for_response(2, std::chrono::seconds(1));
    ASSERT_TRUE(second.contains("result")) << second.dump();
    EXPECT_EQ(second["result"].value("stopReason", std::string()), "end_turn");
}

TEST(ACPServer, CancelAndDestructorDrainWithoutDuplicateResponse) {
    auto probe = std::make_shared<CancelProbe>();
    CapturingSink cap;
    {
        auto server = std::make_unique<ACPServer>(
            build_cancel_aware_engine(probe),
            neograph::json{{"name", "test-acp"}, {"version", "0.0.1"}});
        server->set_notification_sink(cap.as_sink());
        initialize_server(*server);
        const std::string sid = new_session(*server);
        server->handle_message(make_request(7, "session/prompt",
            {{"sessionId", sid}, {"prompt", neograph::json::array({
                neograph::json{{"type", "text"}, {"text", "dtor"}}})}}));
        for (int i = 0; i < 200 && !probe->entered.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_TRUE(probe->entered.load());
        server->handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                                {"params", {{"sessionId", sid}}}});
        auto response = cap.wait_for_response(7, std::chrono::milliseconds(500));
        ASSERT_TRUE(response.contains("result")) << response.dump();
        EXPECT_EQ(response["result"].value("stopReason", std::string()), "cancelled");
    }
    int responses = 0;
    for (const auto& env : cap.envs) {
        if (env.value("id", 0) == 7 && env.contains("result")) ++responses;
    }
    EXPECT_EQ(responses, 1);
    EXPECT_FALSE(probe->completed.load());
}

TEST(ACPServer, UnknownMethodReturnsMethodNotFound) {
    auto server = make_server();
    initialize_server(server);
    auto resp = server.handle_message(make_request(1, "session/load",
        {{"sessionId", "x"}, {"cwd", "/"}, {"mcpServers", neograph::json::array()}}));
    ASSERT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"].value("code", 0), -32601);
}

TEST(ACPServer, UnknownNotificationDropped) {
    auto server = make_server();
    initialize_server(server);
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = "ping";  // no id == notification
    auto resp = server.handle_message(env);
    EXPECT_TRUE(resp.is_null());
}

TEST(ACPServer, RunPumpsNdjsonThroughStreams) {
    auto server = make_server();
    initialize_server(server);

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
    initialize_server(server);
    auto session = server.handle_message(make_request(
        3, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    const auto sid = session["result"].value("sessionId", std::string());

    std::ostringstream input;
    input << make_request(1, "session/prompt",
        {{"sessionId", sid},
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
    EXPECT_FALSE(saw_internal_error) << out.str();

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
    CapturingSink cap;
    auto server = std::make_shared<ACPServer>(engine, info);
    initialize_server(*server, true, false);
    server->attach_client(client);
    auto weak_server = std::weak_ptr<ACPServer>(server);

    // Stub editor. The sink does double duty: capture envelopes for
    // assertions AND respond to outbound fs/read_text_file requests by
    // feeding a synthetic response back through handle_message.
    auto cap_sink = cap.as_sink();
    server->set_notification_sink([&, cap_sink, weak_server](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "fs/read_text_file"
            && env.contains("id")) {
            auto reply_id = env["id"];
            std::thread([weak_server, reply_id]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = {{"content", "void main(){}\n"}};
                if (auto server = weak_server.lock()) {
                    server->handle_message(reply);
                }
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
    initialize_server(*server);
    server->attach_client(client);
    auto weak_server = std::weak_ptr<ACPServer>(server);

    auto cap_sink = cap.as_sink();
    server->set_notification_sink([cap_sink, weak_server, reply_for](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "session/request_permission"
            && env.contains("id")) {
            auto reply_id = env["id"];
            auto outcome  = reply_for(env);
            std::thread([weak_server, reply_id, outcome]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = outcome;
                if (auto server = weak_server.lock()) {
                    server->handle_message(reply);
                }
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
    CapturingSink cap;
    auto server = std::make_shared<ACPServer>(engine, info);
    initialize_server(*server, false, true);
    server->attach_client(client);
    auto weak_server = std::weak_ptr<ACPServer>(server);

    auto cap_sink = cap.as_sink();
    std::string seen_path, seen_body;
    server->set_notification_sink([&, cap_sink, weak_server](const neograph::json& env) {
        cap_sink(env);
        if (env.value("method", std::string()) == "fs/write_text_file"
            && env.contains("id")) {
            // Capture what the client sent the editor.
            auto p = env["params"];
            seen_path = p.value("path", std::string());
            seen_body = p.value("content", std::string());
            auto reply_id = env["id"];
            std::thread([weak_server, reply_id]() {
                neograph::json reply;
                reply["jsonrpc"] = "2.0";
                reply["id"]      = reply_id;
                reply["result"]  = neograph::json::object();
                if (auto server = weak_server.lock()) {
                    server->handle_message(reply);
                }
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
    initialize_server(server, true, true);

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
    initialize_server(server);
    auto sess_resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
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

    // Second prompt on SAME session — must be rejected.
    auto second = server.handle_message(make_request(3, "session/prompt",
        {{"sessionId", sid},
         {"prompt", neograph::json::array({neograph::json{{"type", "text"}, {"text", "b"}}})}}));
    EXPECT_TRUE(second.is_null());

    // The async error envelope arrives via the sink.
    auto err_env = cap.wait_for_response(3, std::chrono::seconds(2));
    ASSERT_TRUE(err_env.contains("error")) << "expected error envelope, got: " << err_env.dump();
    // -32000 is ACP's "Authentication required"; a busy session is an
    // internal error with a machine-readable reason.
    EXPECT_EQ(err_env["error"].value("code", 0), -32603);
    EXPECT_EQ(err_env["error"]["data"].value("reason", std::string()), "session_busy");
    auto nested_err = cap.wait_for_response(4, std::chrono::seconds(2));
    ASSERT_TRUE(nested_err.contains("error"));
    EXPECT_EQ(nested_err["error"].value("code", 0), -32603);

    // Release the stall so the first worker can finish + dtor drain works.
    stall->store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Malformed JSON-RPC envelope handling
// ---------------------------------------------------------------------------

TEST(ACPServer, MalformedEnvelope_ParamsArrayInsteadOfObject) {
    auto server = make_server();
    initialize_server(server);
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
    initialize_server(server);
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
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
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
    initialize_server(server);
    // method present, id absent → notification per JSON-RPC §4.1.
    // Unknown notification is silently dropped — no error envelope.
    neograph::json env;
    env["jsonrpc"] = "2.0";
    env["method"]  = "completely/made_up";
    env["params"]  = neograph::json::object();
    auto resp = server.handle_message(env);
    EXPECT_TRUE(resp.is_null());
}

// ---------------------------------------------------------------------------
// Failure classes -> ACP outcomes (D3/D6). A failed turn is a JSON-RPC error;
// exception text (the fake secret below) never reaches the wire.
// ---------------------------------------------------------------------------

constexpr const char* kFakeSecret = "db password=hunter2 at /srv/secret/agent.env";

std::shared_ptr<const sp::Outcome> failed_outcome(sp::ErrorKind kind, bool refusal) {
    sp::Failure failure;
    failure.error.kind = kind;
    failure.error.safe_message = std::string("provider said: ") + kFakeSecret;
    if (refusal) {
        sp::StopReason stop;
        stop.kind = sp::StopKind::Refusal;
        failure.partial.stop = stop;
    }
    return std::make_shared<const sp::Outcome>(std::move(failure));
}

class FailureNode : public GraphNode {
  public:
    explicit FailureNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        const std::string verb = raw.is_string() ? raw.get<std::string>() : raw.dump();
        NodeOutput out;
        if (verb == "fail") throw std::runtime_error(std::string("boom: ") + kFakeSecret);
        if (verb == "coded")
            throw neograph::graph::NodeFailureError("app.tool_error", "the tool failed", false);
        if (verb == "logic") throw std::invalid_argument(std::string("bad: ") + kFakeSecret);
        if (verb == "hook")
            throw HookBoundaryBlocked("hook-1", HookExecutionState::Failed,
                                      std::string("blocked: ") + kFakeSecret);
        if (verb == "budget")
            throw ProviderFailure(failed_outcome(sp::ErrorKind::QuotaExhausted, false));
        if (verb == "provider")
            throw ProviderFailure(failed_outcome(sp::ErrorKind::Transport, false));
        if (verb == "refusal")
            throw ProviderFailure(failed_outcome(sp::ErrorKind::RemoteFailure, true));
        if (verb == "aborted")
            throw neograph::graph::CancelledException(std::string("aborted: ") + kFakeSecret);
        if (verb == "loop") {
            out.command = neograph::graph::Command{name_, {}};
            co_return out;
        }
        if (verb == "token-budget") {
            if (in.ctx.budget_exhausted) in.ctx.budget_exhausted->store(true);
            out.command = neograph::graph::Command{name_, {}};
            co_return out;
        }
        out.writes.push_back(ChannelWrite{"response", neograph::json("ok:" + verb)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_failure_engine() {
    NodeFactory::instance().register_type("acp_failure",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<FailureNode>(n);
        });
    neograph::json def = {
        {"name", "acp-failure"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {{"worker", {{"type", "acp_failure"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{}));
}

neograph::json prompt_turn(ACPServer& server, CapturingSink& cap,
                           const std::string& sid, int id, const std::string& text) {
    server.handle_message(make_request(id, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", text}}})}}));
    return cap.wait_for_response(id);
}

struct FailureCase {
    const char* verb;
    const char* code;
    bool        retryable;
};

TEST(ACPServer, FailedTurnsAreJsonRpcErrorsWithoutExceptionText) {
    CapturingSink cap;
    ACPServer server(build_failure_engine(), {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    const FailureCase cases[] = {
        {"fail", "NG.TaskFailed", true},
        {"coded", "app.tool_error", false},
        {"logic", "NG.LogicError", false},
        {"hook", "NG.HookBlocked", false},
        {"budget", "NG.BudgetExhausted", false},
        {"provider", "NG.ProviderFailed", true},
    };
    int id = 10;
    for (const auto& c : cases) {
        auto resp = prompt_turn(server, cap, sid, ++id, c.verb);
        ASSERT_TRUE(resp.contains("error")) << c.verb << ": " << resp.dump();
        EXPECT_EQ(resp["error"].value("code", 0), -32603) << c.verb;
        const auto data = resp["error"]["data"];
        EXPECT_EQ(data.value("code", std::string()), c.code) << c.verb;
        EXPECT_EQ(data.value("retryable", !c.retryable), c.retryable) << c.verb;
        EXPECT_FALSE(resp["error"].value("message", std::string()).empty()) << c.verb;
    }
    // The author's public message is the only free text a failure may carry.
    auto coded = cap.wait_for_response(12);
    EXPECT_EQ(coded["error"].value("message", std::string()), "the tool failed");

    // Internal cancellation (a node aborted; the client did not cancel).
    auto aborted = prompt_turn(server, cap, sid, 30, "aborted");
    ASSERT_TRUE(aborted.contains("error")) << aborted.dump();
    EXPECT_EQ(aborted["error"].value("code", 0), -32800);

    // No failure ever produced an agent_message_chunk, and no exception text
    // reached the wire.
    EXPECT_TRUE(cap.notifications_for("session/update").empty());
    const auto wire = cap.wire();
    EXPECT_EQ(wire.find("hunter2"), std::string::npos) << wire;
    EXPECT_EQ(wire.find("/srv/secret"), std::string::npos) << wire;
}

TEST(ACPServer, ProviderRefusalIsTheRefusalStopReason) {
    CapturingSink cap;
    ACPServer server(build_failure_engine(), {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    auto resp = prompt_turn(server, cap, sid, 1, "refusal");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "refusal");
    EXPECT_TRUE(cap.notifications_for("session/update").empty());
    EXPECT_EQ(cap.wire().find("hunter2"), std::string::npos);
}

TEST(ACPServer, StepLimitIsMaxTurnRequestsWithoutAnEmptyChunk) {
    CapturingSink cap;
    ACPServer server(build_failure_engine(), {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    auto resp = prompt_turn(server, cap, sid, 1, "loop");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "max_turn_requests");
    EXPECT_TRUE(cap.notifications_for("session/update").empty());
}

TEST(ACPServer, ModelTokenBudgetIsMaxTokens) {
    CapturingSink cap;
    ACPServer server(build_failure_engine(), {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    auto resp = prompt_turn(server, cap, sid, 1, "token-budget");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "max_tokens");
}

// ---------------------------------------------------------------------------
// Protocol codes and validation
// ---------------------------------------------------------------------------

TEST(ACPServer, CallsBeforeInitializeAreInvalidRequests) {
    CapturingSink cap;
    auto server = make_server();
    server.set_notification_sink(cap.as_sink());
    auto created = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    EXPECT_EQ(created["error"].value("code", 0), -32600) << created.dump();
    auto resumed = server.handle_message(make_request(2, "session/resume",
        {{"sessionId", "x"}, {"cwd", "/work"}, {"mcpServers", neograph::json::array()}}));
    EXPECT_EQ(resumed["error"].value("code", 0), -32600) << resumed.dump();
    server.handle_message(make_request(3, "session/prompt",
        {{"sessionId", "x"}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "hi"}}})}}));
    auto prompted = cap.wait_for_response(3);
    EXPECT_EQ(prompted["error"].value("code", 0), -32600) << prompted.dump();
}

TEST(ACPServer, OverloadIsAnInternalErrorWithAReason) {
    auto probe = std::make_shared<CancelProbe>();
    CapturingSink cap;
    ACPServer server(build_cancel_aware_engine(probe),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    server.set_max_inflight_prompts(1);
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto first = new_session(server);
    const auto second = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", first}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "slow"}}})}}));
    for (int i = 0; i < 200 && !probe->entered.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto rejected = prompt_turn(server, cap, second, 2, "hi");
    ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
    EXPECT_EQ(rejected["error"].value("code", 0), -32603);
    EXPECT_EQ(rejected["error"]["data"].value("reason", std::string()), "overloaded");
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", first}}}});
    EXPECT_TRUE(cap.wait_for_response(1).contains("result"));
}

TEST(ACPServer, MalformedOrUnadvertisedContentBlocksAreInvalidParams) {
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    const neograph::json blocks[] = {
        {{"type", "text"}},
        {{"type", "resource_link"}, {"uri", "file:///x"}},
        {{"type", "nonsense"}},
        {{"type", "image"}, {"data", "aGVsbG8="}, {"mimeType", "image/png"}},
        {{"type", "resource"}, {"resource", {{"uri", "file:///x"}, {"text", "t"}}}},
    };
    int id = 0;
    for (const auto& block : blocks) {
        server.handle_message(make_request(++id, "session/prompt",
            {{"sessionId", sid}, {"prompt", neograph::json::array({block})}}));
        auto resp = cap.wait_for_response(id);
        EXPECT_EQ(resp["error"].value("code", 0), -32602) << block.dump() << " -> " << resp.dump();
    }
}

TEST(ACPServer, ResourceLinkAndAdvertisedEmbeddedResourceReachTheGraph) {
    CapturingSink cap;
    auto server = make_server();
    server.capabilities().prompt.embedded_context = true;
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "summarise"}},
            neograph::json{{"type", "resource_link"}, {"uri", "file:///tmp/spec.md"},
                           {"name", "spec.md"}},
            neograph::json{{"type", "resource"},
                           {"resource", {{"uri", "file:///tmp/a.txt"}, {"text", "inline"}}}},
        })}}));
    auto resp = cap.wait_for_response(1);
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_NE(cap.wire().find("echo:summarise [spec.md](file:///tmp/spec.md) inline"),
              std::string::npos) << cap.wire();
}

class SessionContextNode : public GraphNode {
  public:
    explicit SessionContextNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto cwd = in.state.get("_acp_cwd");
        auto dirs = in.state.get("_acp_additional_directories");
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json(
            (cwd.is_string() ? cwd.get<std::string>() : std::string("<none>")) + "|"
            + dirs.dump())});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

TEST(ACPServer, SessionCwdAndAdditionalDirectoriesReachTheGraph) {
    NodeFactory::instance().register_type("acp_session_context",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<SessionContextNode>(n);
        });
    neograph::json def = {
        {"name", "acp-session-context"},
        {"channels", {
            {"prompt", {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
            {"_acp_cwd", {{"reducer", "overwrite"}}},
            {"_acp_additional_directories", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {{"ctx", {{"type", "acp_session_context"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "ctx"}},
            neograph::json{{"from", "ctx"}, {"to", "__end__"}},
        })},
    };
    CapturingSink cap;
    ACPServer server(std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{})),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    auto created = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/work/project"}, {"additionalDirectories", neograph::json::array({"/work/lib"})},
         {"mcpServers", neograph::json::array()}}));
    const auto sid = created["result"].value("sessionId", std::string());
    auto resp = prompt_turn(server, cap, sid, 2, "where");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_NE(cap.wire().find("/work/project|[\\\"/work/lib\\\"]"), std::string::npos)
        << cap.wire();
}

TEST(ACPServer, AgentInfoIsAlwaysAnImplementation) {
    for (const auto& info : {neograph::json("free-form string"),
                             neograph::json{{"name", "only-name"}},
                             neograph::json()}) {
        ACPServer server(build_echo_engine(), info);
        auto resp = server.handle_message(make_request(1, "initialize",
            {{"protocolVersion", 1}, {"clientCapabilities", neograph::json::object()}}));
        const auto agent_info = resp["result"]["agentInfo"];
        ASSERT_TRUE(agent_info.is_object()) << resp.dump();
        EXPECT_TRUE(agent_info["name"].is_string());
        EXPECT_TRUE(agent_info["version"].is_string());
        EXPECT_FALSE(agent_info.value("version", std::string()).empty());
    }
    ACPServer named(build_echo_engine(), {{"name", "only-name"}});
    auto resp = named.handle_message(make_request(1, "initialize",
        {{"protocolVersion", 1}, {"clientCapabilities", neograph::json::object()}}));
    EXPECT_EQ(resp["result"]["agentInfo"].value("name", std::string()), "only-name");
}

TEST(ACPServer, TerminalMethodsRequireTheClientCapability) {
    std::vector<neograph::json> emitted;
    auto server = make_server();
    initialize_server(server);
    server.set_notification_sink([&](const neograph::json& env) { emitted.push_back(env); });
    EXPECT_THROW(server.call_client("terminal/create",
                                    {{"sessionId", "s"}, {"command", "true"}},
                                    std::chrono::milliseconds(20)),
                 std::runtime_error);
    EXPECT_THROW(server.call_client("elicitation/create", {{"sessionId", "s"}},
                                    std::chrono::milliseconds(20)),
                 std::runtime_error);
    EXPECT_TRUE(emitted.empty());

    auto with_terminal = make_server();
    (void)with_terminal.handle_message(make_request(1, "initialize",
        {{"protocolVersion", 1}, {"clientCapabilities", {{"terminal", true}}}}));
    std::vector<neograph::json> sent;
    with_terminal.set_notification_sink([&](const neograph::json& env) { sent.push_back(env); });
    EXPECT_THROW(with_terminal.call_client("terminal/create",
                                           {{"sessionId", "s"}, {"command", "true"}},
                                           std::chrono::milliseconds(20)),
                 std::runtime_error);  // times out, but it was sent
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].value("method", std::string()), "terminal/create");
}

TEST(ACPServer, CancelAbandonsAPendingClientRequest) {
    auto client = std::make_shared<ACPClient>();
    auto engine = build_read_file_engine(client);
    CapturingSink cap;
    ACPServer server(engine, {{"name", "acp-cancel-fs"}, {"version", "0.0.1"}});
    initialize_server(server, true, false);
    server.attach_client(client);
    server.set_notification_sink(cap.as_sink());  // never answers fs/*
    const auto sid = new_session(server);
    server.handle_message(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "read"}}})}}));
    auto outbound = cap.wait_for_method("fs/read_text_file");
    ASSERT_FALSE(outbound.is_null());
    const auto start = std::chrono::steady_clock::now();
    server.handle_message({{"jsonrpc", "2.0"}, {"method", "session/cancel"},
                           {"params", {{"sessionId", sid}}}});
    auto resp = cap.wait_for_response(1);
    const auto latency = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "cancelled");
    EXPECT_LT(latency, std::chrono::seconds(2));
    auto cancel_request = cap.notifications_for("$/cancel_request");
    ASSERT_EQ(cancel_request.size(), 1u);
    EXPECT_EQ(cancel_request[0]["params"]["requestId"], outbound["id"]);
}

class SlowCancelNode : public GraphNode {
  public:
    explicit SlowCancelNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled())
                throw neograph::graph::CancelledException();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

TEST(ACPServer, InputEofCancelsInflightPromptsAfterTheGrace) {
    NodeFactory::instance().register_type("acp_slow_cancel",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<SlowCancelNode>(n);
        });
    neograph::json def = {
        {"name", "acp-slow"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"worker", {{"type", "acp_slow_cancel"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "worker"}},
            neograph::json{{"from", "worker"}, {"to", "__end__"}},
        })},
    };
    ACPServer server(std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{})),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    server.set_eof_grace(std::chrono::milliseconds(50));
    initialize_server(server);
    const auto sid = new_session(server);
    std::istringstream in(make_request(1, "session/prompt",
        {{"sessionId", sid}, {"prompt", neograph::json::array({
            neograph::json{{"type", "text"}, {"text", "slow"}}})}}).dump() + "\n");
    std::ostringstream out;
    const auto start = std::chrono::steady_clock::now();
    server.run(in, out);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_NE(out.str().find("\"cancelled\""), std::string::npos) << out.str();
}

// ---------------------------------------------------------------------------
// Client-supplied MCP servers (session/new mcpServers)
// ---------------------------------------------------------------------------

TEST(ACPServer, McpServersOverUnadvertisedTransportsAreInvalidParams) {
    auto server = make_server();
    initialize_server(server);
    for (const char* type : {"http", "sse"}) {
        auto resp = server.handle_message(make_request(1, "session/new",
            {{"cwd", "/work"},
             {"mcpServers", neograph::json::array({neograph::json{
                 {"type", type}, {"name", "remote"}, {"url", "http://127.0.0.1:1/mcp"},
                 {"headers", neograph::json::array()}}})}}));
        ASSERT_TRUE(resp.contains("error")) << resp.dump();
        EXPECT_EQ(resp["error"].value("code", 0), -32602);
        EXPECT_NE(resp["error"].value("message", std::string()).find(type), std::string::npos);
    }
}

TEST(ACPServer, AnMcpServerThatCannotStartFailsSessionNewWithoutDetail) {
    auto server = make_server();
    initialize_server(server);
    auto resp = server.handle_message(make_request(1, "session/new",
        {{"cwd", "/tmp"},
         {"mcpServers", neograph::json::array({neograph::json{
             {"name", "missing"}, {"command", "/nonexistent/neograph-acp-mcp"},
             {"args", neograph::json::array()}, {"env", neograph::json::array()}}})}}));
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
#ifdef NEOGRAPH_ACP_MCP_TESTS
    EXPECT_EQ(resp["error"].value("code", 0), -32603);
    EXPECT_EQ(resp["error"]["data"].value("reason", std::string()), "mcp_server_unavailable");
#endif
    EXPECT_EQ(resp.dump().find("/nonexistent"), std::string::npos) << resp.dump();
}

#ifdef NEOGRAPH_ACP_MCP_TESTS
const char* acp_test_python() {
#ifdef _WIN32
    if (std::system("where python >nul 2>&1") == 0) return "python";
    return nullptr;
#else
    if (std::system("command -v python3 >/dev/null 2>&1") == 0) return "python3";
    return nullptr;
#endif
}

TEST(ACPServer, StdioMcpServerToolsAreAvailableToTheSession) {
    const char* python = acp_test_python();
    if (!python) GTEST_SKIP() << "python3 not available";
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures"
                         / "mcp_stdio_echo.py";
    CapturingSink cap;
    auto server = make_server();
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    auto created = server.handle_message(make_request(1, "session/new",
        {{"cwd", std::filesystem::temp_directory_path().string()},
         {"mcpServers", neograph::json::array({neograph::json{
             {"name", "echo-server"}, {"command", python},
             {"args", neograph::json::array({fixture.string()})},
             {"env", neograph::json::array({neograph::json{
                 {"name", "NEOGRAPH_ACP_TEST"}, {"value", "1"}}})}}})}}));
    ASSERT_TRUE(created.contains("result")) << created.dump();
    const auto sid = created["result"].value("sessionId", std::string());
    auto tools = server.client()->session_tools(sid);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools[0]->get_name(), "echo");
    const auto result = tools[0]->execute({{"msg", "hi"}});
    EXPECT_NE(result.find("\"msg\": \"hi\""), std::string::npos) << result;
    EXPECT_TRUE(server.session_tools("unknown").empty());
}

// The agent's environment may hold provider API keys: a client-specified MCP
// server gets PATH and the client's `env` entries, nothing else.
TEST(ACPServer, ClientMcpServersDoNotInheritTheAgentEnvironment) {
    const char* python = acp_test_python();
    if (!python) GTEST_SKIP() << "python3 not available";
    constexpr const char* kSecret = "NEOGRAPH_ACP_TEST_PROVIDER_KEY";
#ifdef _WIN32
    ASSERT_EQ(_putenv_s(kSecret, "sk-test-fake-0000"), 0);
#else
    ASSERT_EQ(setenv(kSecret, "sk-test-fake-0000", 1), 0);
#endif
    const auto fixture = std::filesystem::path(__FILE__).parent_path() / "fixtures"
                         / "mcp_stdio_env_probe.py";
    auto server = make_server();
    initialize_server(server);
    auto created = server.handle_message(make_request(1, "session/new",
        {{"cwd", std::filesystem::temp_directory_path().string()},
         {"mcpServers", neograph::json::array({neograph::json{
             {"name", "env-probe"}, {"command", python},
             {"args", neograph::json::array({fixture.string()})},
             {"env", neograph::json::array({neograph::json{
                 {"name", "NEOGRAPH_ACP_CLIENT_VALUE"}, {"value", "from-client"}}})}}})}}));
#ifdef _WIN32
    _putenv_s(kSecret, "");
#else
    unsetenv(kSecret);
#endif
    ASSERT_TRUE(created.contains("result")) << created.dump();
    const auto sid = created["result"].value("sessionId", std::string());
    auto tools = server.client()->session_tools(sid);
    ASSERT_EQ(tools.size(), 1u);
    const auto seen = neograph::json::parse(tools[0]->execute(
        {{"names", {kSecret, "NEOGRAPH_ACP_CLIENT_VALUE", "PATH", "HOME"}}}));
    EXPECT_TRUE(seen[kSecret].is_null()) << seen.dump();
    EXPECT_EQ(seen["NEOGRAPH_ACP_CLIENT_VALUE"], "from-client") << seen.dump();
    EXPECT_TRUE(seen["PATH"].is_string()) << seen.dump();
#ifndef _WIN32
    EXPECT_TRUE(seen["HOME"].is_null()) << seen.dump();
#endif
}
#endif

// ---------------------------------------------------------------------------
// Incremental updates (ACP-12): model tokens and tool calls
// ---------------------------------------------------------------------------

class TokenNode : public GraphNode {
  public:
    explicit TokenNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        if (in.stream_cb) {
            for (const char* token : {"Hel", "lo"}) {
                (*in.stream_cb)(neograph::graph::GraphEvent{
                    neograph::graph::GraphEvent::Type::LLM_TOKEN, name_, neograph::json(token)});
            }
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"response", neograph::json("Hello")});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

TEST(ACPServer, ModelTokensStreamAsMessageChunksWithoutARepeat) {
    NodeFactory::instance().register_type("acp_tokens",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<TokenNode>(n);
        });
    neograph::json def = {
        {"name", "acp-tokens"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                      {"response", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"model", {{"type", "acp_tokens"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "model"}},
            neograph::json{{"from", "model"}, {"to", "__end__"}},
        })},
    };
    CapturingSink cap;
    ACPServer server(std::shared_ptr<GraphEngine>(GraphEngine::compile(def, NodeContext{})),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    auto resp = prompt_turn(server, cap, sid, 1, "hi");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    std::vector<std::string> chunks;
    for (const auto& n : cap.notifications_for("session/update")) {
        chunks.push_back(n["params"]["update"]["content"].value("text", std::string()));
    }
    EXPECT_EQ(chunks, (std::vector<std::string>{"Hel", "lo"}));
}

class ScriptedTool final : public neograph::AsyncTool {
  public:
    ScriptedTool(std::string name, bool fail) : name_(std::move(name)), fail_(fail) {}
    neograph::ChatTool get_definition() const override {
        return {name_, "scripted", neograph::json{{"type", "object"}}};
    }
    std::string get_name() const override { return name_; }
    asio::awaitable<std::string> execute_async(const neograph::json&) override {
        if (fail_) throw std::runtime_error(std::string("tool crashed: ") + kFakeSecret);
        co_return std::string("ok-output");
    }
  private:
    std::string name_;
    bool fail_;
};

class TwoToolCallsNode final : public GraphNode {
  public:
    asio::awaitable<NodeOutput> run(NodeInput) override {
        neograph::json assistant = {
            {"role", "assistant"}, {"content", ""},
            {"tool_calls", neograph::json::array({
                {{"id", "call-ok"}, {"name", "ok_tool"}, {"arguments", "{\"q\":1}"}},
                {{"id", "call-bad"}, {"name", "bad_tool"}, {"arguments", "{}"}},
            })},
        };
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", neograph::json::array({assistant})});
        co_return out;
    }
    std::string get_name() const override { return "plan"; }
};

TEST(ACPServer, ToolCallsAreReportedWithoutToolErrorText) {
    NodeFactory::instance().register_type("acp_two_tool_calls",
        [](const std::string&, const neograph::json&, const NodeContext&) {
            return std::make_unique<TwoToolCallsNode>();
        });
    neograph::json def = {
        {"name", "acp-tool-updates"},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {{"plan", {{"type", "acp_two_tool_calls"}}},
                   {"tools", {{"type", "tool_dispatch"}}}}},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "plan"}},
            neograph::json{{"from", "plan"}, {"to", "tools"}},
            neograph::json{{"from", "tools"}, {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    ctx.tools = neograph::ToolSet(std::vector<std::shared_ptr<Tool>>{
        std::make_shared<ScriptedTool>("ok_tool", false),
        std::make_shared<ScriptedTool>("bad_tool", true)});
    CapturingSink cap;
    ACPServer server(std::shared_ptr<GraphEngine>(GraphEngine::compile(def, ctx)),
                     {{"name", "test-acp"}, {"version", "0.0.1"}});
    initialize_server(server);
    server.set_notification_sink(cap.as_sink());
    const auto sid = new_session(server);
    auto resp = prompt_turn(server, cap, sid, 1, "use tools");
    ASSERT_TRUE(resp.contains("result")) << resp.dump();
    EXPECT_EQ(resp["result"].value("stopReason", std::string()), "end_turn");

    std::map<std::string, std::vector<neograph::json>> by_call;
    for (const auto& n : cap.notifications_for("session/update")) {
        const auto update = n["params"]["update"];
        const auto kind = update.value("sessionUpdate", std::string());
        ASSERT_TRUE(kind == "tool_call" || kind == "tool_call_update") << n.dump();
        by_call[update.value("toolCallId", std::string())].push_back(update);
    }
    ASSERT_EQ(by_call["call-ok"].size(), 2u) << cap.wire();
    EXPECT_EQ(by_call["call-ok"][0].value("sessionUpdate", std::string()), "tool_call");
    EXPECT_EQ(by_call["call-ok"][0].value("title", std::string()), "ok_tool");
    EXPECT_EQ(by_call["call-ok"][0]["rawInput"].value("q", 0), 1);
    EXPECT_EQ(by_call["call-ok"][1].value("status", std::string()), "completed");
    EXPECT_EQ(by_call["call-ok"][1]["content"][0]["content"].value("text", std::string()),
              "ok-output");
    ASSERT_EQ(by_call["call-bad"].size(), 2u) << cap.wire();
    EXPECT_EQ(by_call["call-bad"][1].value("status", std::string()), "failed");
    EXPECT_FALSE(by_call["call-bad"][1].contains("content"));
    EXPECT_EQ(cap.wire().find("hunter2"), std::string::npos) << cap.wire();
}

}  // namespace
