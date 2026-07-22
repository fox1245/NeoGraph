#include <neograph/neograph.h>

#include <gtest/gtest.h>

#include <atomic>

using namespace neograph;
using namespace neograph::graph;

namespace {

json one_node_graph(const std::string& type) {
    return {
        {"schema_version", 1},
        {"name", "engine_config"},
        {"channels",
         {
             {"input", {{"reducer", "overwrite"}}},
             {"output", {{"reducer", "overwrite"}}},
         }},
        {"nodes", {{"work", {{"type", type}}}}},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "work"}},
                      {{"from", "work"}, {"to", "__end__"}},
                  })},
    };
}

class EchoNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        NodeOutput out;
        out.writes.push_back({"output", in.state.get("input")});
        co_return out;
    }

    std::string get_name() const override { return "work"; }
};

class ConfigProbeNode final : public GraphNode {
public:
    static std::atomic<int> attempts;

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        if (++attempts == 1) {
            throw std::runtime_error("retry me");
        }

        std::string stored;
        if (in.ctx.store) {
            auto item = in.ctx.store->get({"tests"}, "value");
            if (item) stored = item->value.get<std::string>();
        }

        NodeOutput out;
        out.writes.push_back({"output", json{
                                            {"stored", stored},
                                            {"has_gate", static_cast<bool>(in.ctx.tool_gate)},
                                        }});
        co_return out;
    }

    std::string get_name() const override { return "work"; }
};

std::atomic<int> ConfigProbeNode::attempts{0};

class CountingNode final : public GraphNode {
public:
    static std::atomic<int> calls;

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        ++calls;
        NodeOutput out;
        out.writes.push_back({"output", in.state.get("input")});
        co_return out;
    }

    std::string get_name() const override { return "work"; }
};

std::atomic<int> CountingNode::calls{0};

std::atomic<int> link_factory_calls{0};

void register_node(const std::string& type, NodeFactoryFn factory) {
    NodeFactory::instance().register_type(
        type, std::move(factory), json::parse(R"({"type":"object","properties":{}})"),
        json::parse(R"({"reads":["input"],"writes":["output"]})"));
}

}  // namespace

TEST(EngineConfigTest, LegacyCompileDelegatesWithoutBehaviorChange) {
    register_node("engine_config_echo", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<EchoNode>();
    });

    const auto  definition = one_node_graph("engine_config_echo");
    NodeContext context;

    auto legacy = GraphEngine::compile(definition, context);

    EngineConfig config;
    config.node_context = context;
    auto configured     = GraphEngine::build(definition, std::move(config));

    RunConfig run;
    run.input = {{"input", "same"}};

    const auto legacy_result     = legacy->run(run);
    const auto configured_result = configured->run(run);

    EXPECT_EQ(legacy_result.output, configured_result.output);
    EXPECT_EQ(legacy_result.execution_trace, configured_result.execution_trace);
}

TEST(EngineConfigTest, AppliesRuntimeConfigurationBeforeFirstRun) {
    register_node("engine_config_probe", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<ConfigProbeNode>();
    });
    ConfigProbeNode::attempts = 0;

    auto checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
    auto store            = std::make_shared<InMemoryStore>();
    store->put({"tests"}, "value", "configured");

    RetryPolicy retry;
    retry.max_retries      = 1;
    retry.initial_delay_ms = 0;

    EngineConfig config;
    config.checkpoint_store = checkpoint_store;
    config.store            = store;
    config.retry_policy     = retry;
    config.worker_count     = 2;
    config.tool_gate        = [](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        co_return ToolDecision::allow();
    };

    auto engine = GraphEngine::build(one_node_graph("engine_config_probe"), std::move(config));

    RunConfig run;
    run.thread_id     = "configured-run";
    run.input         = {{"input", "unused"}};
    const auto result = engine->run(run);

    EXPECT_EQ(ConfigProbeNode::attempts.load(), 2);
    EXPECT_EQ(result.channel<json>("output")["stored"].get<std::string>(), "configured");
    EXPECT_TRUE(result.channel<json>("output")["has_gate"].get<bool>());
    EXPECT_TRUE(engine->get_state("configured-run").has_value());
    EXPECT_EQ(engine->get_store(), store);
}

TEST(EngineConfigTest, EnablesNodeCacheAtConstructionTime) {
    register_node("engine_config_counting",
                  [](const std::string&, const json&, const NodeContext&) {
                      return std::make_unique<CountingNode>();
                  });
    CountingNode::calls = 0;

    EngineConfig config;
    config.cached_nodes.insert("work");
    auto engine = GraphEngine::build(one_node_graph("engine_config_counting"), std::move(config));

    RunConfig run;
    run.input = {{"input", 7}};
    EXPECT_EQ(engine->run(run).channel<int>("output"), 7);
    EXPECT_EQ(engine->run(run).channel<int>("output"), 7);
    EXPECT_EQ(CountingNode::calls.load(), 1);
}

TEST(CompiledGraphLinkTest, ConsumesCompiledGraphWithoutRunningFactoriesAgain) {
    link_factory_calls = 0;
    register_node("compiled_graph_link", [](const std::string&, const json&, const NodeContext&) {
        ++link_factory_calls;
        return std::make_unique<EchoNode>();
    });

    const auto  definition = one_node_graph("compiled_graph_link");
    NodeContext context;
    auto        graph = GraphCompiler::compile(definition, context);
    GraphCompiler::verify_roundtrip(definition, graph);
    EXPECT_EQ(link_factory_calls.load(), 1);

    auto engine = GraphEngine::link(std::move(graph));
    EXPECT_EQ(link_factory_calls.load(), 1);

    RunConfig run;
    run.input = {{"input", "linked"}};
    EXPECT_EQ(engine->run(run).channel<std::string>("output"), "linked");
}

TEST(CompiledGraphLinkTest, AppliesSemanticValidationBeforeRuntimeConstruction) {
    register_node("compiled_graph_link_invalid",
                  [](const std::string&, const json&, const NodeContext&) {
                      return std::make_unique<EchoNode>();
                  });

    NodeContext context;
    auto graph = GraphCompiler::compile(one_node_graph("compiled_graph_link_invalid"), context);
    graph.edges.push_back({"missing", "work"});

    EXPECT_THROW((void)GraphEngine::link(std::move(graph)), std::runtime_error);
}
