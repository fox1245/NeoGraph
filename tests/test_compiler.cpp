// Pure parsing tests for GraphCompiler. These assertions do not construct
// a GraphEngine — they exercise the JSON → CompiledGraph stage in
// isolation so malformed-input behavior, reducer/edge parsing rules, and
// retry_policy optionality are locked in without runtime coupling.
//
// Parsing failures should surface here (before the engine is built) with
// clear errors — that is the entire point of splitting the compilation
// stage out.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// A minimal stateless node used across these tests so parsing hits the
// NodeFactory path without pulling in LLM or tool dependencies.
class NoopNode : public GraphNode {
public:
    explicit NoopNode(std::string n) : name_(std::move(n)) {}
    std::vector<ChannelWrite> execute(const GraphState&) override { return {}; }
    std::string name() const override { return name_; }
private:
    std::string name_;
};

void ensure_noop_registered() {
    // Idempotent: NodeFactory::register_type overwrites the entry so
    // repeated calls across test cases are safe.
    NodeFactory::instance().register_type(
        "noop",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<NoopNode>(name);
        });
}

} // namespace

// =========================================================================
// Defaults
// =========================================================================

TEST(GraphCompiler, DefaultNameWhenMissing) {
    auto cg = GraphCompiler::compile(json::object(), NodeContext{});
    EXPECT_EQ(cg.name, "unnamed_graph");
    EXPECT_TRUE(cg.channel_defs.empty());
    EXPECT_TRUE(cg.nodes.empty());
    EXPECT_TRUE(cg.edges.empty());
    EXPECT_TRUE(cg.conditional_edges.empty());
    EXPECT_TRUE(cg.barrier_specs.empty());
    EXPECT_TRUE(cg.interrupt_before.empty());
    EXPECT_TRUE(cg.interrupt_after.empty());
    EXPECT_FALSE(cg.retry_policy.has_value());
}

TEST(GraphCompiler, CustomName) {
    auto cg = GraphCompiler::compile(json{{"name", "my_graph"}}, NodeContext{});
    EXPECT_EQ(cg.name, "my_graph");
}

// =========================================================================
// Channels
// =========================================================================

TEST(GraphCompiler, ChannelReducerMapping) {
    json def = {{"channels", {
        {"a", {{"reducer", "append"}}},
        {"b", {{"reducer", "overwrite"}}},
        {"c", {{"reducer", "custom_xyz"}}}  // unknown → CUSTOM
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.channel_defs.size(), 3u);

    std::map<std::string, ReducerType> by_name;
    for (const auto& cd : cg.channel_defs) by_name[cd.name] = cd.type;

    EXPECT_EQ(by_name["a"], ReducerType::APPEND);
    EXPECT_EQ(by_name["b"], ReducerType::OVERWRITE);
    EXPECT_EQ(by_name["c"], ReducerType::CUSTOM);
}

TEST(GraphCompiler, ChannelDefaultReducerIsOverwrite) {
    json def = {{"channels", {{"a", json::object()}}}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.channel_defs.size(), 1u);
    EXPECT_EQ(cg.channel_defs[0].type, ReducerType::OVERWRITE);
    EXPECT_EQ(cg.channel_defs[0].reducer_name, "overwrite");
}

TEST(GraphCompiler, ChannelInitialValuePreserved) {
    json def = {{"channels", {
        {"counter", {{"reducer", "overwrite"}, {"initial", 42}}}
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.channel_defs.size(), 1u);
    EXPECT_EQ(cg.channel_defs[0].initial_value.get<int>(), 42);
}

// =========================================================================
// Nodes + barriers
// =========================================================================

TEST(GraphCompiler, UnknownNodeTypeThrows) {
    json def = {{"nodes", {{"x", {{"type", "definitely_not_registered"}}}}}};
    EXPECT_THROW(GraphCompiler::compile(def, NodeContext{}), std::runtime_error);
}

TEST(GraphCompiler, NodesInstantiatedViaFactory) {
    ensure_noop_registered();
    json def = {{"nodes", {
        {"a", {{"type", "noop"}}},
        {"b", {{"type", "noop"}}}
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_EQ(cg.nodes.size(), 2u);
    ASSERT_NE(cg.nodes.find("a"), cg.nodes.end());
    EXPECT_EQ(cg.nodes["a"]->name(), "a");
    EXPECT_EQ(cg.nodes["b"]->name(), "b");
}

TEST(GraphCompiler, BarrierSpecsCollected) {
    ensure_noop_registered();
    json def = {{"nodes", {
        {"a", {{"type", "noop"}}},
        {"b", {{"type", "noop"}}},
        {"join", {
            {"type", "noop"},
            {"barrier", {{"wait_for", {"a", "b"}}}}
        }}
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.barrier_specs.size(), 1u);
    auto it = cg.barrier_specs.find("join");
    ASSERT_NE(it, cg.barrier_specs.end());
    EXPECT_EQ(it->second, (std::set<std::string>{"a", "b"}));
}

TEST(GraphCompiler, EmptyBarrierWaitForIsIgnored) {
    ensure_noop_registered();
    json def = {{"nodes", {
        {"x", {
            {"type", "noop"},
            {"barrier", {{"wait_for", json::array()}}}
        }}
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    // An empty wait_for is not a barrier — don't pollute barrier_specs
    // with entries that would never fire.
    EXPECT_TRUE(cg.barrier_specs.empty());
}

// =========================================================================
// Edges (regular + conditional)
// =========================================================================

TEST(GraphCompiler, RegularEdgesParsed) {
    json def = {{"edges", json::array({
        {{"from", "a"}, {"to", "b"}},
        {{"from", "b"}, {"to", "c"}}
    })}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.edges.size(), 2u);
    EXPECT_EQ(cg.edges[0].from, "a");
    EXPECT_EQ(cg.edges[0].to, "b");
    EXPECT_EQ(cg.edges[1].from, "b");
    EXPECT_EQ(cg.edges[1].to, "c");
    EXPECT_TRUE(cg.conditional_edges.empty());
}

TEST(GraphCompiler, ConditionalEdgeViaConditionField) {
    json def = {{"edges", json::array({
        {{"from", "router"},
         {"condition", "state.topic"},
         {"routes", {{"ai", "ai_node"}, {"ml", "ml_node"}}}}
    })}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_TRUE(cg.edges.empty());
    ASSERT_EQ(cg.conditional_edges.size(), 1u);
    EXPECT_EQ(cg.conditional_edges[0].from, "router");
    EXPECT_EQ(cg.conditional_edges[0].condition, "state.topic");
    EXPECT_EQ(cg.conditional_edges[0].routes.size(), 2u);
    EXPECT_EQ(cg.conditional_edges[0].routes.at("ai"), "ai_node");
    EXPECT_EQ(cg.conditional_edges[0].routes.at("ml"), "ml_node");
}

TEST(GraphCompiler, ConditionalEdgeViaTypeField) {
    // Alternative form: {"type": "conditional"} without routes map.
    json def = {{"edges", json::array({
        {{"from", "r"},
         {"type", "conditional"},
         {"condition", "state.flag"}}
    })}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_EQ(cg.conditional_edges.size(), 1u);
    EXPECT_EQ(cg.conditional_edges[0].condition, "state.flag");
    EXPECT_TRUE(cg.conditional_edges[0].routes.empty());
}

// =========================================================================
// Interrupts
// =========================================================================

TEST(GraphCompiler, InterruptSetsParsed) {
    json def = {
        {"interrupt_before", {"a", "b"}},
        {"interrupt_after", {"c"}}
    };
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_EQ(cg.interrupt_before, (std::set<std::string>{"a", "b"}));
    EXPECT_EQ(cg.interrupt_after, (std::set<std::string>{"c"}));
}

// =========================================================================
// Retry policy (optional → propagation)
// =========================================================================

TEST(GraphCompiler, RetryPolicyAbsentMeansNullopt) {
    auto cg = GraphCompiler::compile(json::object(), NodeContext{});
    EXPECT_FALSE(cg.retry_policy.has_value());
}

TEST(GraphCompiler, RetryPolicyAllFieldsParsed) {
    json def = {{"retry_policy", {
        {"max_retries", 4},
        {"initial_delay_ms", 250},
        {"backoff_multiplier", 3.5},
        {"max_delay_ms", 10000}
    }}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_TRUE(cg.retry_policy.has_value());
    EXPECT_EQ(cg.retry_policy->max_retries, 4);
    EXPECT_EQ(cg.retry_policy->initial_delay_ms, 250);
    EXPECT_FLOAT_EQ(cg.retry_policy->backoff_multiplier, 3.5f);
    EXPECT_EQ(cg.retry_policy->max_delay_ms, 10000);
}

TEST(GraphCompiler, RetryPolicyDefaultsForMissingFields) {
    json def = {{"retry_policy", {{"max_retries", 2}}}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    ASSERT_TRUE(cg.retry_policy.has_value());
    EXPECT_EQ(cg.retry_policy->max_retries, 2);
    EXPECT_EQ(cg.retry_policy->initial_delay_ms, 100);
    EXPECT_FLOAT_EQ(cg.retry_policy->backoff_multiplier, 2.0f);
    EXPECT_EQ(cg.retry_policy->max_delay_ms, 5000);
}
