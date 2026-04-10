#include <gtest/gtest.h>
#include <neograph/neograph.h>

using namespace neograph;
using namespace neograph::graph;

// ── Helper: minimal graph JSON ──

static json make_linear_graph(const std::string& node_name = "worker") {
    return {
        {"name", "test_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {node_name, {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}}
        }}
    };
}

static json make_conditional_graph() {
    return {
        {"name", "cond_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"router",  {{"type", "custom"}}},
            {"path_a",  {{"type", "custom"}}},
            {"path_b",  {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "router"}},
            {{"from", "router"}, {"condition", "route_channel"},
             {"routes", {{"a", "path_a"}, {"b", "path_b"}}}},
            {{"from", "path_a"}, {"to", "__end__"}},
            {{"from", "path_b"}, {"to", "__end__"}}
        }}
    };
}

// ── Custom node that writes to result ──

class EchoNode : public GraphNode {
public:
    EchoNode(const std::string& name, const std::string& value)
        : name_(name), value_(value) {}

    std::vector<ChannelWrite> execute(const GraphState& /*state*/) override {
        return {ChannelWrite{"result", json(value_)}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
    std::string value_;
};

// ── Router node that writes to __route__ ──

class RouterNode : public GraphNode {
public:
    RouterNode(const std::string& name, const std::string& route)
        : name_(name), route_(route) {}

    std::vector<ChannelWrite> execute(const GraphState& /*state*/) override {
        return {ChannelWrite{"__route__", json(route_)}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
    std::string route_;
};

// ── Test fixture ──

class GraphEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register custom node type for tests
        NodeFactory::instance().register_type("custom",
            [](const std::string& name, const json& /*config*/, const NodeContext& /*ctx*/) {
                return std::make_unique<EchoNode>(name, "done_by_" + name);
            });
    }
};

// ── Linear execution ──

TEST_F(GraphEngineTest, LinearExecution) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 1);
    EXPECT_EQ(result.execution_trace[0], "worker");
}

// ── Result channel written ──

TEST_F(GraphEngineTest, ResultChannelWritten) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_TRUE(result.output.contains("channels"));
    auto& channels = result.output["channels"];
    ASSERT_TRUE(channels.contains("result"));
    EXPECT_EQ(channels["result"]["value"], "done_by_worker");
}

// ── Conditional routing ──

TEST_F(GraphEngineTest, ConditionalRoutingA) {
    // Override to route to "a"
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "a");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[0], "router");
    EXPECT_EQ(result.execution_trace[1], "path_a");
}

TEST_F(GraphEngineTest, ConditionalRoutingB) {
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "b");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[1], "path_b");
}

// ── Max steps safety ──

TEST_F(GraphEngineTest, MaxStepsLimit) {
    // Create a cycle: worker -> worker (via condition always routing back)
    json cycle_graph = {
        {"name", "cycle"},
        {"channels", {
            {"result", {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"looper", {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "looper"}},
            {{"from", "looper"}, {"to", "looper"}}
        }}
    };

    auto engine = GraphEngine::compile(cycle_graph, NodeContext{});
    RunConfig config;
    config.max_steps = 5;
    auto result = engine->run(config);

    // Should not run forever — capped by max_steps
    EXPECT_LE(result.execution_trace.size(), 5u);
}

// ── Streaming callback invoked ──

TEST_F(GraphEngineTest, StreamCallbackFires) {
    // Restore default custom type
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EchoNode>(name, "done");
        });

    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;

    int event_count = 0;
    auto result = engine->run_stream(config, [&](const GraphEvent& /*ev*/) {
        ++event_count;
    });

    EXPECT_GT(event_count, 0);
    EXPECT_FALSE(result.interrupted);
}

// ── Empty input ──

TEST_F(GraphEngineTest, EmptyInput) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    // No input — should still execute
    auto result = engine->run(config);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(result.execution_trace.size(), 1);
}

// ── Graph name ──

TEST_F(GraphEngineTest, GraphName) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    EXPECT_EQ(engine->graph_name(), "test_graph");
}
