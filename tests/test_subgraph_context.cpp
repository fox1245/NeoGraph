#include <gtest/gtest.h>

#include <neograph/neograph.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

struct ContextSnapshot {
    std::shared_ptr<CancelToken> cancel_token;
    std::shared_ptr<UsageAccumulator> usage;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string trace_id;
    std::string thread_id;
    StreamMode stream_mode = StreamMode::ALL;
    std::shared_ptr<Store> store;
    std::optional<json> store_value;
    bool has_tool_gate = false;
};

struct ContextCapture {
    std::mutex mutex;
    std::vector<ContextSnapshot> snapshots;
    std::optional<Namespace> store_namespace;
    std::string store_key;

    void add(const RunContext& ctx) {
        std::lock_guard<std::mutex> lock(mutex);
        ContextSnapshot snapshot{ctx.cancel_token, ctx.usage, ctx.deadline,
                                 ctx.trace_id, ctx.thread_id, ctx.stream_mode,
                                 ctx.store, std::nullopt,
                                 static_cast<bool>(ctx.tool_gate)};
        if (store_namespace && ctx.store) {
            if (auto item = ctx.store->get(*store_namespace, store_key)) {
                snapshot.store_value = item->value;
            }
        }
        snapshots.push_back(std::move(snapshot));
    }
};

class CaptureContextNode final : public GraphNode {
public:
    CaptureContextNode(std::string name, std::shared_ptr<ContextCapture> capture)
        : name_(std::move(name)), capture_(std::move(capture)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        capture_->add(in.ctx);
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<ContextCapture> capture_;
};

class FanoutNode final : public GraphNode {
public:
    explicit FanoutNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.sends.push_back(Send{"child", json{{"slot", "zero"}}});
        out.sends.push_back(Send{"child", json{{"slot", "one"}}});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class WaitForParentCancelNode final : public GraphNode {
public:
    WaitForParentCancelNode(std::string name, std::atomic<bool>* entered,
                            std::atomic<bool>* observed_cancel)
        : name_(std::move(name)), entered_(entered), observed_cancel_(observed_cancel) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        entered_->store(true, std::memory_order_release);
        const auto timeout = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
        while (!in.ctx.cancel_token || !in.ctx.cancel_token->is_cancelled()) {
            if (std::chrono::steady_clock::now() >= timeout) {
                throw std::runtime_error("parent cancellation did not reach grandchild");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        observed_cancel_->store(true, std::memory_order_release);
        in.ctx.cancel_token->throw_if_cancelled("grandchild observed parent cancel");
        co_return NodeOutput{};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<bool>* entered_;
    std::atomic<bool>* observed_cancel_;
};

class SpyTool final : public Tool {
public:
    explicit SpyTool(std::atomic<int>* calls, json* observed_args = nullptr)
        : calls_(calls), observed_args_(observed_args) {}

    ChatTool get_definition() const override {
        ChatTool definition;
        definition.name = "nested_tool";
        return definition;
    }

    std::string execute(const json& arguments) override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        if (observed_args_) *observed_args_ = arguments;
        return R"({"ok":true})";
    }

    std::string get_name() const override { return "nested_tool"; }

private:
    std::atomic<int>* calls_;
    json* observed_args_;
};

class CountingToolSeedNode final : public GraphNode {
public:
    CountingToolSeedNode(std::string name, std::atomic<int>* calls)
        : name_(std::move(name)), calls_(calls) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        calls_->fetch_add(1, std::memory_order_relaxed);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"messages", json::array({{
            {"role", "assistant"},
            {"content", ""},
            {"tool_calls", json::array({{
                {"id", "nested"},
                {"name", "nested_tool"},
                {"arguments", "{}"},
            }})},
        }})});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* calls_;
};

json one_node_graph(const std::string& name, const std::string& node_type,
                    const std::string& node_name = "node") {
    return {
        {"name", name},
        {"channels", json::object()},
        {"nodes", {{node_name, {{"type", node_type}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name}, {"to", "__end__"}},
        })},
    };
}

void register_subgraph_factory(const std::string& type,
                               std::shared_ptr<GraphEngine> child) {
    NodeFactory::instance().register_type(
        type, [child = std::move(child)](const std::string& name, const json&,
                                         const NodeContext&) {
            return std::make_unique<SubgraphNode>(name, child);
        });
}

json add_messages_channel(json definition) {
    definition["channels"] = {{"messages", {{"reducer", "append"}}}};
    return definition;
}

json seed_then_tool_graph(const std::string& name, const std::string& seed_type) {
    return {
        {"name", name},
        {"channels", {{"messages", {{"reducer", "append"}}}}},
        {"nodes", {
            {"seed", {{"type", seed_type}}},
            {"tool", {{"type", "tool_dispatch"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "seed"}},
            {{"from", "seed"}, {"to", "tool"}},
            {{"from", "tool"}, {"to", "__end__"}},
        })},
    };
}

}  // namespace

TEST(SubgraphContext, MultiLevelRunInheritsRuntimeResourcesAndIdentity) {
    auto capture = std::make_shared<ContextCapture>();
    capture->store_namespace = Namespace{"tenant", "root/with:separators"};
    capture->store_key = "setting";
    NodeFactory::instance().register_type(
        "subgraph_context_capture_196",
        [capture](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CaptureContextNode>(name, capture);
        });

    auto leaf = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("leaf", "subgraph_context_capture_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_middle_196", leaf);
    auto middle = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("middle", "subgraph_context_middle_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_outer_196", middle);

    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto store = std::make_shared<InMemoryStore>();
    store->put(*capture->store_namespace, capture->store_key,
               json{{"inherited", true}});
    auto engine = GraphEngine::compile(
        one_node_graph("root", "subgraph_context_outer_196"), NodeContext{}, checkpoints);
    engine->set_store(store);

    auto cancel = std::make_shared<CancelToken>();
    auto usage = std::make_shared<UsageAccumulator>();
    RunConfig config;
    config.thread_id = "root/with:separators";
    config.cancel_token = cancel;
    config.usage = usage;
    config.stream_mode = StreamMode::TOKENS;

    RunMetadata metadata;
    metadata.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    metadata.trace_id = "trace-196";

    std::vector<GraphEvent> events;
    auto callback = [&events](GraphEvent event) { events.push_back(std::move(event)); };

    ASSERT_NO_THROW(engine->run_stream(config, callback, metadata));
    ASSERT_NO_THROW(engine->run_stream(config, callback, metadata));

    std::lock_guard<std::mutex> lock(capture->mutex);
    ASSERT_EQ(capture->snapshots.size(), 2u);
    const auto& first = capture->snapshots[0];
    const auto& second = capture->snapshots[1];
    EXPECT_EQ(first.thread_id, second.thread_id);
    EXPECT_NE(first.thread_id, config.thread_id);
    EXPECT_FALSE(first.thread_id.empty());
    EXPECT_EQ(first.usage, usage);
    EXPECT_EQ(first.deadline, metadata.deadline);
    EXPECT_EQ(first.trace_id, metadata.trace_id);
    EXPECT_EQ(first.stream_mode, config.stream_mode);
    EXPECT_EQ(first.store, store);
    ASSERT_TRUE(first.store_value);
    EXPECT_EQ(*first.store_value, json({{"inherited", true}}));
    EXPECT_TRUE(first.cancel_token);
    EXPECT_FALSE(checkpoints->list(first.thread_id).empty());
    EXPECT_TRUE(events.empty())
        << "a TOKENS-only parent must not let a child widen its stream mode";
}

TEST(SubgraphContext, ParentToolGateCannotBeWeakenedByNestedEngine) {
    std::atomic<int> tool_calls{0};
    std::atomic<int> parent_gate_calls{0};
    std::atomic<int> child_gate_calls{0};
    SpyTool tool(&tool_calls);

    NodeContext leaf_context;
    leaf_context.tools = {&tool};
    auto leaf = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(
            add_messages_channel(one_node_graph("leaf", "tool_dispatch", "tool")),
            leaf_context).release());
    leaf->set_tool_gate([&child_gate_calls](ToolCall, ToolGateContext)
                            -> asio::awaitable<ToolDecision> {
        child_gate_calls.fetch_add(1, std::memory_order_relaxed);
        co_return ToolDecision::allow();
    });
    register_subgraph_factory("subgraph_context_gated_child_196", leaf);
    auto middle = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(add_messages_channel(
                                 one_node_graph("middle", "subgraph_context_gated_child_196")),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_gated_outer_196", middle);

    auto engine = GraphEngine::compile(
        add_messages_channel(one_node_graph("root", "subgraph_context_gated_outer_196")),
        NodeContext{});
    engine->set_tool_gate([&parent_gate_calls](ToolCall, ToolGateContext)
                              -> asio::awaitable<ToolDecision> {
        parent_gate_calls.fetch_add(1, std::memory_order_relaxed);
        co_return ToolDecision::deny("parent policy denies nested tool");
    });

    RunConfig config;
    config.thread_id = "tool-gate-root";
    config.input = {
        {"messages", json::array({{
            {"role", "assistant"},
            {"content", ""},
            {"tool_calls", json::array({{
                {"id", "nested"},
                {"name", "nested_tool"},
                {"arguments", "{}"},
            }})},
        }})},
    };
    ASSERT_NO_THROW(engine->run(config));
    EXPECT_EQ(parent_gate_calls.load(), 1);
    EXPECT_EQ(child_gate_calls.load(), 0);
    EXPECT_EQ(tool_calls.load(), 0);
}

TEST(SubgraphContext, NestedToolGatesApplyRewritesInParentThenChildOrder) {
    std::atomic<int> tool_calls{0};
    json observed_args;
    SpyTool tool(&tool_calls, &observed_args);

    NodeContext leaf_context;
    leaf_context.tools = {&tool};
    auto leaf = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(
            add_messages_channel(one_node_graph("leaf", "tool_dispatch", "tool")),
            leaf_context).release());
    leaf->set_tool_gate([](ToolCall call, ToolGateContext)
                            -> asio::awaitable<ToolDecision> {
        auto args = json::parse(call.arguments);
        if (!args.value("parent", false)) {
            co_return ToolDecision::deny("child did not receive parent rewrite");
        }
        args["child"] = true;
        co_return ToolDecision::allow(std::move(args));
    });
    register_subgraph_factory("subgraph_context_rewrite_child_196", leaf);
    auto middle = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(add_messages_channel(
                                 one_node_graph("middle", "subgraph_context_rewrite_child_196")),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_rewrite_outer_196", middle);

    auto engine = GraphEngine::compile(
        add_messages_channel(one_node_graph("root", "subgraph_context_rewrite_outer_196")),
        NodeContext{});
    engine->set_tool_gate([](ToolCall call, ToolGateContext)
                              -> asio::awaitable<ToolDecision> {
        auto args = json::parse(call.arguments);
        args["parent"] = true;
        co_return ToolDecision::allow(std::move(args));
    });

    RunConfig config;
    config.thread_id = "tool-gate-rewrite-root";
    config.input = {
        {"messages", json::array({{
            {"role", "assistant"},
            {"content", ""},
            {"tool_calls", json::array({{
                {"id", "nested"},
                {"name", "nested_tool"},
                {"arguments", "{\"original\":true}"},
            }})},
        }})},
    };

    ASSERT_NO_THROW(engine->run(config));
    EXPECT_EQ(tool_calls.load(), 1);
    EXPECT_TRUE(observed_args.value("original", false));
    EXPECT_TRUE(observed_args.value("parent", false));
    EXPECT_TRUE(observed_args.value("child", false));
}

TEST(SubgraphContext, ParentResumeResumesInterruptedGrandchild) {
    std::atomic<int> tool_calls{0};
    std::atomic<int> gate_calls{0};
    std::atomic<int> seed_calls{0};
    SpyTool tool(&tool_calls);

    NodeFactory::instance().register_type(
        "subgraph_context_resume_seed_196",
        [&seed_calls](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CountingToolSeedNode>(name, &seed_calls);
        });

    NodeContext leaf_context;
    leaf_context.tools = {&tool};
    auto leaf = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(
            seed_then_tool_graph("leaf", "subgraph_context_resume_seed_196"),
            leaf_context).release());
    register_subgraph_factory("subgraph_context_resume_child_196", leaf);
    auto middle = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(add_messages_channel(
                                 one_node_graph("middle", "subgraph_context_resume_child_196")),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_resume_outer_196", middle);

    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        add_messages_channel(one_node_graph("root", "subgraph_context_resume_outer_196")),
        NodeContext{}, checkpoints);
    engine->set_tool_gate([&gate_calls](ToolCall, ToolGateContext context)
                              -> asio::awaitable<ToolDecision> {
        gate_calls.fetch_add(1, std::memory_order_relaxed);
        if (!context.resume_value) {
            co_return ToolDecision::interrupt("approval required");
        }
        if (context.resume_value->value("approved", false)) {
            co_return ToolDecision::allow();
        }
        co_return ToolDecision::deny("approval refused");
    });

    RunConfig config;
    config.thread_id = "nested-resume-root";

    auto interrupted = engine->run(config);
    ASSERT_TRUE(interrupted.interrupted);
    EXPECT_EQ(tool_calls.load(), 0);
    EXPECT_EQ(seed_calls.load(), 1);

    auto reinterrupted = engine->resume(config.thread_id);
    EXPECT_TRUE(reinterrupted.interrupted);
    EXPECT_EQ(tool_calls.load(), 0);
    EXPECT_EQ(gate_calls.load(), 2);
    EXPECT_EQ(seed_calls.load(), 1)
        << "a null-payload parent resume must resume, not restart, the grandchild";

    auto resumed = engine->resume(config.thread_id, json{{"approved", true}});
    EXPECT_FALSE(resumed.interrupted);
    EXPECT_EQ(tool_calls.load(), 1);
    EXPECT_EQ(gate_calls.load(), 3);
    EXPECT_EQ(seed_calls.load(), 1)
        << "a fresh grandchild run would execute the seed node twice";
}

TEST(SubgraphContext, SendSiblingsReceiveDistinctCheckpointIdentities) {
    auto capture = std::make_shared<ContextCapture>();
    NodeFactory::instance().register_type(
        "subgraph_context_sibling_capture_196",
        [capture](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CaptureContextNode>(name, capture);
        });
    NodeFactory::instance().register_type(
        "subgraph_context_fanout_196",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FanoutNode>(name);
        });

    auto child = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("child", "subgraph_context_sibling_capture_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_send_child_196", child);

    json root = {
        {"name", "root"},
        {"channels", json::object()},
        {"nodes", {
            {"fanout", {{"type", "subgraph_context_fanout_196"}}},
            {"child", {{"type", "subgraph_context_send_child_196"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "fanout"}},
            {{"from", "child"}, {"to", "__end__"}},
        })},
    };
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(root, NodeContext{}, checkpoints);
    engine->set_worker_count(2);

    RunConfig config;
    config.thread_id = "send-root";
    ASSERT_NO_THROW(engine->run(config));

    std::lock_guard<std::mutex> lock(capture->mutex);
    ASSERT_EQ(capture->snapshots.size(), 2u);
    const auto& first = capture->snapshots[0];
    const auto& second = capture->snapshots[1];
    EXPECT_NE(first.thread_id, second.thread_id);
    EXPECT_FALSE(checkpoints->list(first.thread_id).empty());
    EXPECT_FALSE(checkpoints->list(second.thread_id).empty());
}

TEST(SubgraphContext, StaticParallelSiblingsReceiveDistinctCheckpointIdentities) {
    auto capture = std::make_shared<ContextCapture>();
    NodeFactory::instance().register_type(
        "subgraph_context_static_capture_196",
        [capture](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CaptureContextNode>(name, capture);
        });

    auto child = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("child", "subgraph_context_static_capture_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_static_child_196", child);

    json root = {
        {"name", "root"},
        {"channels", json::object()},
        {"nodes", {
            {"left", {{"type", "subgraph_context_static_child_196"}}},
            {"right", {{"type", "subgraph_context_static_child_196"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "left"}},
            {{"from", "__start__"}, {"to", "right"}},
            {{"from", "left"}, {"to", "__end__"}},
            {{"from", "right"}, {"to", "__end__"}},
        })},
    };
    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(root, NodeContext{}, checkpoints);
    engine->set_worker_count(2);

    RunConfig config;
    config.thread_id = "static-root";
    ASSERT_NO_THROW(engine->run(config));

    std::lock_guard<std::mutex> lock(capture->mutex);
    ASSERT_EQ(capture->snapshots.size(), 2u);
    const auto& first = capture->snapshots[0];
    const auto& second = capture->snapshots[1];
    EXPECT_NE(first.thread_id, second.thread_id);
    EXPECT_FALSE(first.thread_id.empty());
    EXPECT_FALSE(second.thread_id.empty());
    EXPECT_FALSE(checkpoints->list(first.thread_id).empty());
    EXPECT_FALSE(checkpoints->list(second.thread_id).empty());
}

TEST(SubgraphContext, AnonymousParentDisablesChildCheckpointing) {
    auto capture = std::make_shared<ContextCapture>();
    NodeFactory::instance().register_type(
        "subgraph_context_anonymous_capture_196",
        [capture](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<CaptureContextNode>(name, capture);
        });

    auto child = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("child", "subgraph_context_anonymous_capture_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_anonymous_child_196", child);

    auto checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        one_node_graph("root", "subgraph_context_anonymous_child_196"),
        NodeContext{}, checkpoints);

    ASSERT_NO_THROW(engine->run(RunConfig{}));

    std::lock_guard<std::mutex> lock(capture->mutex);
    ASSERT_EQ(capture->snapshots.size(), 1u);
    EXPECT_TRUE(capture->snapshots.front().thread_id.empty());
    EXPECT_TRUE(checkpoints->list("").empty());
}

TEST(SubgraphContext, ParentCancellationReachesGrandchild) {
    std::atomic<bool> entered{false};
    std::atomic<bool> observed_cancel{false};
    NodeFactory::instance().register_type(
        "subgraph_context_wait_cancel_196",
        [&entered, &observed_cancel](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WaitForParentCancelNode>(
                name, &entered, &observed_cancel);
        });

    auto leaf = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("leaf", "subgraph_context_wait_cancel_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_cancel_middle_196", leaf);
    auto middle = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node_graph("middle", "subgraph_context_cancel_middle_196"),
                             NodeContext{}).release());
    register_subgraph_factory("subgraph_context_cancel_outer_196", middle);
    auto engine = GraphEngine::compile(
        one_node_graph("root", "subgraph_context_cancel_outer_196"), NodeContext{});

    auto cancel = std::make_shared<CancelToken>();
    RunConfig config;
    config.thread_id = "cancel-root";
    config.cancel_token = cancel;

    std::exception_ptr result;
    std::thread runner([&] {
        try {
            engine->run(config);
        } catch (...) {
            result = std::current_exception();
        }
    });

    const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < timeout) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cancel->cancel();
    runner.join();

    EXPECT_TRUE(entered.load(std::memory_order_acquire));
    EXPECT_TRUE(observed_cancel.load(std::memory_order_acquire));
    ASSERT_TRUE(result);
    EXPECT_THROW(std::rethrow_exception(result), CancelledException);
}
