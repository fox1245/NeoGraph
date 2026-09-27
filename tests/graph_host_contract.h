#pragma once

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/store.h>
#include <neograph/tool_dispatch.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

// The same graph fixtures and assertions run through each real host adapter.
// A host adapter must invoke its public request/cancel/shutdown entry points;
// it must not invoke GraphEngine or RunInvocation directly.
namespace graph_host_contract {
using namespace neograph;
using namespace neograph::graph;
using namespace std::chrono_literals;

enum class Terminal { completed, interrupted, max_steps, cancelled, error, rejected };
struct Result {
    Terminal terminal = Terminal::error;
    std::string output;
    std::vector<std::string> events;
};
struct Probe {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> events;
    std::string thread_id;
    std::string prompt;
    json stored;
    bool gate_present = false;
    std::atomic<int> starts{0};
    std::atomic<int> finished{0};
    std::atomic<int> gate_calls{0};
    std::atomic<int> tool_calls{0};

    void record(std::string event) {
        std::lock_guard lock(mu);
        events.push_back(std::move(event));
        cv.notify_all();
    }
    bool await_starts(int count = 1) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, 2s, [&] { return starts.load() >= count; });
    }
    bool await_event(const std::string& event) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, 2s, [&] {
            return std::find(events.begin(), events.end(), event) != events.end();
        });
    }
    std::vector<std::string> snapshot() {
        std::lock_guard lock(mu);
        return events;
    }
};

class PolicyTool final : public Tool {
public:
    explicit PolicyTool(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    ChatTool get_definition() const override {
        return {"contract_tool", "must be denied by parent gate", json::object()};
    }
    std::string execute(const json&) override {
        probe_->tool_calls.fetch_add(1);
        return R"({"unexpected":"allowed"})";
    }
    std::string get_name() const override { return "contract_tool"; }
private:
    std::shared_ptr<Probe> probe_;
};
class DependencyProvider final : public Provider {
public:
    explicit DependencyProvider(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    asio::awaitable<ChatCompletion> complete_async(const CompletionParams& params) override {
        probe_->record("dependency-start");
        const auto until = std::chrono::steady_clock::now() + 1200ms;
        while (std::chrono::steady_clock::now() < until) {
            if (params.cancel_token && params.cancel_token->is_cancelled()) {
                probe_->record("dependency-cancelled");
                throw CancelledException();
            }
            std::this_thread::sleep_for(1ms);
        }
        probe_->record("dependency-complete");
        co_return ChatCompletion{};
    }
    ChatCompletion complete(const CompletionParams&) override {
        throw std::logic_error("provider fixture requires the async path");
    }
    std::string get_name() const override { return "contract-provider"; }
private:
    std::shared_ptr<Probe> probe_;
};

class DependencyTool final : public Tool, public ContextualAsyncTool {
public:
    explicit DependencyTool(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}
    ChatTool get_definition() const override {
        return {"contract_wait_tool", "cooperative cancellation fixture", json::object()};
    }
    std::string get_name() const override { return "contract_wait_tool"; }
    std::string execute(const json&) override {
        throw std::logic_error("tool fixture requires contextual dispatch");
    }
    asio::awaitable<std::string> execute_async(const json&,
                                                ToolExecutionContext execution) override {
        probe_->record("dependency-start");
        const auto until = std::chrono::steady_clock::now() + 1200ms;
        while (std::chrono::steady_clock::now() < until) {
            if (execution.cancel_token && execution.cancel_token->is_cancelled()) {
                probe_->record("dependency-cancelled");
                throw CancelledException();
            }
            std::this_thread::sleep_for(1ms);
        }
        probe_->record("dependency-complete");
        co_return "{}";
    }
private:
    std::shared_ptr<Probe> probe_;
};


class Node final : public GraphNode {
public:
    Node(std::string name, std::shared_ptr<Probe> probe, bool leaf,
         std::shared_ptr<DependencyProvider> provider = {})
        : name_(std::move(name)), probe_(std::move(probe)), leaf_(leaf),
          provider_(std::move(provider)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto raw = input.state.get("prompt");
        auto prompt = raw.is_string() ? raw.get<std::string>() : std::string();
        {
            std::lock_guard lock(probe_->mu);
            probe_->thread_id = input.ctx.thread_id;
            probe_->prompt = prompt;
            if (leaf_) {
                probe_->gate_present = static_cast<bool>(input.ctx.tool_gate);
                if (input.ctx.store) {
                    auto item = input.ctx.store->get(Namespace{"contract"}, "secret");
                    if (item) probe_->stored = item->value;
                }
            }
        }
        probe_->starts.fetch_add(1);
        probe_->record(leaf_ ? "leaf-start" : "node-start");
        if (prompt == "wait") {
            const auto until = std::chrono::steady_clock::now() + 1200ms;
            while (std::chrono::steady_clock::now() < until) {
                if (input.ctx.cancel_token && input.ctx.cancel_token->is_cancelled()) {
                    probe_->record("node-cancelled");
                    throw CancelledException();
                }
                std::this_thread::sleep_for(1ms);
            }
        }
        if (prompt == "provider") {
            CompletionParams params;
            params.model = "contract-local";
            params.cancel_token = input.ctx.cancel_token;
            (void)co_await provider_->complete_async(params);
        }
        if (prompt == "error") throw std::runtime_error("contract node failure");
        if (prompt == "interrupt") throw NodeInterrupt("contract approval required");
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"response", json("reply:" + prompt)});
        if (prompt == "policy" && leaf_) {
            if (!input.ctx.store || !input.ctx.tool_gate)
                throw std::runtime_error("nested policy missing");
            output.writes.push_back(ChannelWrite{"messages", json::array({{
                {"role", "assistant"}, {"content", ""},
                {"tool_calls", json::array({{
                    {"id", "contract-tool-call"},
                    {"name", "contract_tool"},
                    {"arguments", "{}"},
                }})},
            }})});
        }
        if (prompt == "tool_wait") {
            output.writes.push_back(ChannelWrite{"messages", json::array({{
                {"role", "assistant"}, {"content", ""},
                {"tool_calls", json::array({{
                    {"id", "contract-tool-wait"},
                    {"name", "contract_wait_tool"},
                    {"arguments", "{}"},
                }})},
            }})});
        }
        probe_->finished.fetch_add(1);
        probe_->record(leaf_ ? "leaf-end" : "node-end");
        co_return output;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::shared_ptr<Probe> probe_;
    bool leaf_;
    std::shared_ptr<DependencyProvider> provider_;
};

inline json definition(std::string name, std::string type) {
    return {{"name", std::move(name)},
            {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                          {"response", {{"reducer", "overwrite"}}},
                          {"_acp_session_id", {{"reducer", "overwrite"}}},
                          {"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"worker", {{"type", std::move(type)}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "worker"}},
                                   {{"from", "worker"}, {"to", "__end__"}}})}};
}

struct GraphFixture {
    std::shared_ptr<Probe> probe = std::make_shared<Probe>();
    std::shared_ptr<PolicyTool> tool;
    std::shared_ptr<DependencyProvider> provider;
    std::shared_ptr<DependencyTool> wait_tool;
    json graph;
    NodeContext context;
    // Engine nodes retain raw Tool pointers; dependencies outlive engine.
    std::shared_ptr<GraphEngine> engine;
    explicit GraphFixture(bool nested = false, bool looping = false,
                          std::string dependency = {}) {
        if (dependency == "provider") provider = std::make_shared<DependencyProvider>(probe);
        NodeFactory::instance().register_type("contract_boundary_node",
            [p = probe, dep = provider](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<Node>(name, p, false, dep);
            });
        graph = definition("host-contract", "contract_boundary_node");
        if (looping) graph["edges"][1]["to"] = "worker";
        if (dependency == "tool_wait") {
            wait_tool = std::make_shared<DependencyTool>(probe);
            context.tools = {wait_tool.get()};
            graph["nodes"]["tool"] = {{"type", "tool_dispatch"}};
            graph["edges"][1]["to"] = "tool";
            graph["edges"].push_back({{"from", "tool"}, {"to", "__end__"}});
        }
        if (nested) {
            NodeFactory::instance().register_type("contract_boundary_leaf",
                [p = probe](const std::string& name, const json&, const NodeContext&) {
                    return std::make_unique<Node>(name, p, true);
                });
            tool = std::make_shared<PolicyTool>(probe);
            auto leaf_definition = definition("host-contract-leaf", "contract_boundary_leaf");
            leaf_definition["nodes"]["tool"] = {{"type", "tool_dispatch"}};
            leaf_definition["edges"][1]["to"] = "tool";
            leaf_definition["edges"].push_back({{"from", "tool"}, {"to", "__end__"}});
            NodeContext leaf_context;
            leaf_context.tools = {tool.get()};
            struct OwnedLeaf {
                std::shared_ptr<PolicyTool> tool;
                std::shared_ptr<GraphEngine> engine;
            };
            auto leaf = std::make_shared<OwnedLeaf>();
            leaf->tool = tool;
            leaf->engine = std::shared_ptr<GraphEngine>(
                GraphEngine::compile(leaf_definition, leaf_context).release());
            NodeFactory::instance().register_type("contract_boundary_subgraph",
                [leaf](const std::string& name, const json&, const NodeContext&) {
                    return std::make_unique<SubgraphNode>(name, leaf->engine);
                });
            graph = definition("host-contract-parent", "contract_boundary_subgraph");
        }
        engine = std::shared_ptr<GraphEngine>(GraphEngine::compile(graph, context).release());
        if (nested) {
            auto store = std::make_shared<InMemoryStore>();
            store->put(Namespace{"contract"}, "secret", json("host-policy"));
            engine->set_store(std::move(store));
            engine->set_tool_gate([p = probe](ToolCall, ToolGateContext)
                -> asio::awaitable<ToolDecision> {
                p->gate_calls.fetch_add(1);
                co_return ToolDecision::deny("host policy");
            });
        }
    }
};

class Host {
public:
    virtual ~Host() = default;
    virtual std::future<Result> start(const std::string& identity,
                                       const std::string& prompt,
                                       int max_steps = 0) = 0;
    virtual void cancel(const std::string& identity) = 0;
    virtual std::string effective_identity(const std::string& identity) const {
        return identity;
    }
    virtual void shutdown() = 0;
};
using Factory = std::function<std::unique_ptr<Host>(GraphFixture&, std::size_t limit)>;

inline Result receive(std::future<Result>& future) {
    EXPECT_EQ(future.wait_for(3s), std::future_status::ready)
        << "host failed to terminate bounded graph request";
    return future.get();
}

struct CancellationEvidence {
    Terminal rejected = Terminal::error;
    Terminal running = Terminal::error;
    int starts = 0;
    int finished = 0;
    std::vector<std::string> node_events;
};
inline bool cancellation_passes(const CancellationEvidence& evidence) {
    return evidence.rejected == Terminal::rejected &&
           evidence.running == Terminal::cancelled &&
           evidence.starts == 1 && evidence.finished == 0 &&
           evidence.node_events ==
               std::vector<std::string>{"node-start", "node-cancelled"};
}


inline CancellationEvidence observe_cancellation(const Factory& make) {
    GraphFixture fixture;
    auto host = make(fixture, 1);
    auto running = host->start("contract-cancel", "wait");
    if (!fixture.probe->await_starts()) {
        host->shutdown();
        throw std::runtime_error("contract node never entered its in-flight cancellation point");
    }
    auto rejected = host->start("contract-overloaded", "hello");
    CancellationEvidence evidence;
    evidence.rejected = receive(rejected).terminal;
    host->cancel("contract-cancel");
    evidence.running = receive(running).terminal;
    // A transport can terminate its client stream before the graph worker
    // observes cancellation. Wait for the backend evidence, not just the RPC.
    (void)fixture.probe->await_event("node-cancelled");
    evidence.starts = fixture.probe->starts.load();
    evidence.finished = fixture.probe->finished.load();
    evidence.node_events = fixture.probe->snapshot();
    host->shutdown();
    return evidence;
}

inline void expect_terminal_event_once(const Result& result) {
    ASSERT_FALSE(result.events.empty());
    EXPECT_EQ(result.events.back(), "terminal");
    EXPECT_EQ(std::count(result.events.begin(), result.events.end(), "terminal"), 1);
}

inline void check_contract(const Factory& make) {
    {
        GraphFixture fixture;
        auto host = make(fixture, 1);
        auto run = host->start("contract-identity", "hello");
        auto result = receive(run);
        EXPECT_EQ(result.terminal, Terminal::completed);
        EXPECT_EQ(result.output, "reply:hello");
        EXPECT_EQ(fixture.probe->thread_id, host->effective_identity("contract-identity"));
        expect_terminal_event_once(result);
        ASSERT_GE(result.events.size(), 2U);
        EXPECT_NE(result.events.front(), "terminal");
        if (result.events.front() == "node-start") {
            const auto end = std::find(result.events.begin(), result.events.end(), "node-end");
            ASSERT_NE(end, result.events.end());
            EXPECT_LT(std::distance(result.events.begin(), end),
                      std::distance(result.events.begin(), result.events.end()) - 1);
        }
        EXPECT_EQ(fixture.probe->snapshot(),
                  (std::vector<std::string>{"node-start", "node-end"}));
        EXPECT_EQ(fixture.probe->starts, 1);
        EXPECT_EQ(fixture.probe->finished, 1);
        host->shutdown();
    }
    {
        const auto evidence = observe_cancellation(make);
        EXPECT_TRUE(cancellation_passes(evidence))
            << "in-flight cancellation and admission contract failed";
        EXPECT_EQ(evidence.rejected, Terminal::rejected);
        EXPECT_EQ(evidence.starts, 1) << "rejected request started a node";
        EXPECT_EQ(evidence.running, Terminal::cancelled)
            << "cancellation must reach a node already executing";
        EXPECT_EQ(evidence.finished, 0);
        EXPECT_EQ(evidence.node_events,
                  (std::vector<std::string>{"node-start", "node-cancelled"}));
    }
    for (const auto& [prompt, max_steps, expected] :
         std::vector<std::tuple<std::string, int, Terminal>>{
             {"error", 0, Terminal::error},
             {"interrupt", 0, Terminal::interrupted}}) {
        GraphFixture fixture;
        auto host = make(fixture, 1);
        auto run = host->start("contract-" + prompt, prompt, max_steps);
        auto result = receive(run);
        EXPECT_EQ(result.terminal, expected) << prompt;
        expect_terminal_event_once(result);
        EXPECT_EQ(fixture.probe->starts, 1) << prompt;
        host->shutdown();
    }
    for (const auto& dependency : {"provider", "tool_wait"}) {
        GraphFixture fixture(false, false, dependency);
        auto host = make(fixture, 1);
        auto run = host->start(std::string("contract-") + dependency, dependency);
        ASSERT_TRUE(fixture.probe->await_event("dependency-start")) << dependency;
        host->cancel(std::string("contract-") + dependency);
        auto result = receive(run);
        (void)fixture.probe->await_event("dependency-cancelled");
        EXPECT_EQ(result.terminal, Terminal::cancelled) << dependency;
        expect_terminal_event_once(result);
        auto events = fixture.probe->snapshot();
        EXPECT_NE(std::find(events.begin(), events.end(), "dependency-cancelled"),
                  events.end()) << dependency;
        EXPECT_EQ(std::find(events.begin(), events.end(), "dependency-complete"),
                  events.end()) << dependency;
        host->shutdown();
    }
    {
        GraphFixture fixture(false, true);
        auto host = make(fixture, 1);
        auto run = host->start("contract-limit", "hello");
        auto result = receive(run);
        EXPECT_EQ(result.terminal, Terminal::max_steps);
        expect_terminal_event_once(result);
        EXPECT_GT(fixture.probe->starts, 1);
        host->shutdown();
    }
    {
        GraphFixture fixture(true);
        auto host = make(fixture, 1);
        auto run = host->start("contract-nested", "policy");
        auto result = receive(run);
        EXPECT_EQ(result.terminal, Terminal::completed);
        expect_terminal_event_once(result);
        EXPECT_EQ(fixture.probe->thread_id.empty(), false);
        EXPECT_EQ(fixture.probe->stored, json("host-policy"));
        EXPECT_TRUE(fixture.probe->gate_present);
        EXPECT_EQ(fixture.probe->snapshot(),
                  (std::vector<std::string>{"leaf-start", "leaf-end"}));
        EXPECT_EQ(fixture.probe->gate_calls, 1);
        EXPECT_EQ(fixture.probe->tool_calls, 0);
        host->shutdown();
    }
    {
        GraphFixture fixture;
        auto host = make(fixture, 1);
        auto running = host->start("contract-shutdown", "wait");
        ASSERT_TRUE(fixture.probe->await_starts());
        // Hosts can drain or cancel, but must not abandon in-flight work.
        host->shutdown();
        const auto result = receive(running);
        EXPECT_TRUE(result.terminal == Terminal::completed ||
                    result.terminal == Terminal::cancelled);
        EXPECT_EQ(fixture.probe->starts, 1);
    }
}
} // namespace graph_host_contract
