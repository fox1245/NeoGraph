#include <neograph/neograph.h>
#include <neograph/graph/validator.h>

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

class OwnedProbeTool final : public Tool {
public:
    static std::atomic<int> destructions;

    ~OwnedProbeTool() override { ++destructions; }

    ChatTool get_definition() const override {
        return {"owned_probe", "ToolSet lifetime probe", json::object()};
    }
    std::string execute(const json&) override { return "tool-alive"; }
    std::string get_name() const override { return "owned_probe"; }
};

std::atomic<int> OwnedProbeTool::destructions{0};

class ToolProbeNode final : public GraphNode {
public:
    explicit ToolProbeNode(Tool* tool) : tool_(tool) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back({"output", tool_->execute(json::object())});
        co_return out;
    }

    std::string get_name() const override { return "work"; }

private:
    Tool* tool_;
};

class RegistryProbeNode final : public GraphNode {
public:
    explicit RegistryProbeNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        if (name_ == "seed") {
            out.writes.push_back({"value", 1});
        } else {
            out.writes.push_back({"output", name_});
        }
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

json registry_graph() {
    return {
        {"schema_version", 1},
        {"name", "registry_isolation"},
        {"channels",
         {
             {"value", {{"reducer", "engine_local_reducer"}, {"initial", 0}}},
             {"output", {{"reducer", "overwrite"}}},
         }},
        {"nodes",
         {
             {"seed", {{"type", "engine_local_node"}}},
             {"one", {{"type", "engine_local_node"}}},
             {"eleven", {{"type", "engine_local_node"}}},
             {"wrong", {{"type", "engine_local_node"}}},
         }},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "seed"}},
                      {{"from", "one"}, {"to", "__end__"}},
                      {{"from", "eleven"}, {"to", "__end__"}},
                      {{"from", "wrong"}, {"to", "__end__"}},
                  })},
        {"conditional_edges",
         json::array({
             {{"from", "seed"},
              {"condition", "engine_local_condition"},
              {"routes", {{"one", "one"}, {"eleven", "eleven"}, {"wrong", "wrong"}}}},
         })},
    };
}

std::shared_ptr<GraphRegistry> make_registry(int         reducer_bias,
                                             int         expected_value,
                                             std::string route) {
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type("engine_local_node",
                            [](const std::string& name, const json&, const NodeContext&) {
                                return std::make_unique<RegistryProbeNode>(name);
                            });
    registry->register_reducer("engine_local_reducer",
                               [reducer_bias](const json& current, const json& incoming) {
                                   return current.get<int>() + incoming.get<int>() + reducer_bias;
                               });
    registry->register_condition(
        "engine_local_condition",
        [expected_value, route = std::move(route)](const GraphState& state) {
            return state.get("value").get<int>() == expected_value ? route : "wrong";
        });
    return registry;
}

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

TEST(EngineConfigTest, StrictBuildRejectsUnknownKeysWithoutMutatingDefinition) {
    register_node("engine_config_strict",
                  [](const std::string&, const json&, const NodeContext&) {
                      return std::make_unique<EchoNode>();
                  });

    auto definition = one_node_graph("engine_config_strict");
    json legacy = json::object();
    for (const auto& [key, value] : definition.items()) {
        if (key != "schema_version") legacy[key] = value;
    }
    legacy["conditionnal_edges"] = json::array();

    EXPECT_NO_THROW((void)GraphEngine::build(legacy, EngineConfig{}));
    EXPECT_THROW((void)GraphEngine::build_strict(legacy, EngineConfig{}),
                 std::runtime_error);
    EXPECT_FALSE(legacy.contains("schema_version"));
}

TEST(EngineConfigTest, StrictBuildPreservesInvalidDeclaredSchemaVersion) {
    auto definition = one_node_graph("unused");
    definition["schema_version"] = "1";

    EXPECT_THROW((void)GraphEngine::build_strict(definition, EngineConfig{}),
                 std::runtime_error);

    definition["schema_version"] = 1.5;
    EXPECT_THROW((void)GraphEngine::build_strict(definition, EngineConfig{}),
                 std::runtime_error);
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

TEST(ValidatedTopologyLinkTest, ParsesAndValidatesBeforeCreatingRuntimeNodes) {
    link_factory_calls = 0;
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type(
        "validated_topology_echo",
        [](const std::string&, const json&, const NodeContext&) {
            ++link_factory_calls;
            return std::make_unique<EchoNode>();
        });

    const auto definition = one_node_graph("validated_topology_echo");
    auto topology = GraphCompiler::parse(definition, *registry);
    EXPECT_EQ(link_factory_calls.load(), 0);
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(definition, topology));

    auto validated = GraphValidator::require_valid(std::move(topology), *registry);
    EXPECT_EQ(link_factory_calls.load(), 0);
    EXPECT_FALSE(validated.report().has_errors());

    EngineResources resources;
    resources.registry = registry;
    auto engine = GraphEngine::link(std::move(validated), EngineConfig{},
                                    std::move(resources));
    EXPECT_EQ(link_factory_calls.load(), 1);

    RunConfig run;
    run.input = {{"input", "validated"}};
    EXPECT_EQ(engine->run(run).channel<std::string>("output"), "validated");
}

TEST(ValidatedTopologyLinkTest, RejectsSemanticErrorsBeforeCreatingNodes) {
    link_factory_calls = 0;
    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type(
        "invalid_validated_topology",
        [](const std::string&, const json&, const NodeContext&) {
            ++link_factory_calls;
            return std::make_unique<EchoNode>();
        });

    auto definition = one_node_graph("invalid_validated_topology");
    definition["edges"].push_back({{"from", "work"}, {"to", "missing"}});
    auto topology = GraphCompiler::parse(definition, *registry);

    EXPECT_THROW((void)GraphValidator::require_valid(std::move(topology), *registry),
                 std::runtime_error);
    EXPECT_EQ(link_factory_calls.load(), 0);
}

TEST(EngineResourcesTest, KeepsOwnedToolSetAliveForEngineLifetime) {
    OwnedProbeTool::destructions = 0;

    auto registry = std::make_shared<GraphRegistry>();
    registry->register_type(
        "owned_tool_probe", [](const std::string&, const json&, const NodeContext& context) {
            if (context.tools.size() != 1) {
                throw std::runtime_error("owned ToolSet was not bound to NodeContext");
            }
            return std::make_unique<ToolProbeNode>(context.tools.front());
        });

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<OwnedProbeTool>());

    EngineResources resources;
    resources.tools    = ToolSet(std::move(tools));
    resources.registry = registry;
    auto engine        = GraphEngine::build(one_node_graph("owned_tool_probe"), EngineConfig{},
                                            std::move(resources));

    EXPECT_EQ(OwnedProbeTool::destructions.load(), 0);
    RunConfig run;
    EXPECT_EQ(engine->run(run).channel<std::string>("output"), "tool-alive");

    engine.reset();
    EXPECT_EQ(OwnedProbeTool::destructions.load(), 1);
}

TEST(EngineResourcesTest, IsolatesNodeReducerAndConditionRegistrationsPerEngine) {
    EngineResources first_resources;
    first_resources.registry = make_registry(0, 1, "one");
    auto first = GraphEngine::build(registry_graph(), EngineConfig{}, std::move(first_resources));

    EngineResources second_resources;
    second_resources.registry = make_registry(10, 11, "eleven");
    auto second = GraphEngine::build(registry_graph(), EngineConfig{}, std::move(second_resources));

    RunConfig run;
    EXPECT_EQ(first->run(run).channel<std::string>("output"), "one");
    EXPECT_EQ(second->run(run).channel<std::string>("output"), "eleven");
    EXPECT_THROW((void)GraphEngine::build(registry_graph(), EngineConfig{}), std::runtime_error);
}

TEST(EngineResourcesTest, RejectsAmbiguousOwnedAndRawToolBindings) {
    EngineConfig   config;
    OwnedProbeTool raw_tool;
    config.node_context.tools = {&raw_tool};

    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<OwnedProbeTool>());
    EngineResources resources;
    resources.tools = ToolSet(std::move(tools));

    EXPECT_THROW(
        (void)GraphEngine::build(one_node_graph("unused"), std::move(config), std::move(resources)),
        std::invalid_argument);
}
