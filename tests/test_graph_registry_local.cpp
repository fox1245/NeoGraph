#include <neograph/graph/compiler.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/validator.h>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace neograph;
using namespace neograph::graph;

class LocalNode final : public GraphNode {
public:
    explicit LocalNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

NodeFactoryFn factory(std::atomic<int>* calls = nullptr) {
    return [calls](const std::string& name, const json&, const NodeContext&) {
        if (calls) ++*calls;
        return std::make_unique<LocalNode>(name);
    };
}

json node_definition(std::string type) {
    return json{{"schema_version", 1},
                {"nodes", json{{"work", json{{"type", std::move(type)}}}}},
                {"edges", json::array()},
                {"conditional_edges", json::array()}};
}

}  // namespace

TEST(GraphRegistryLocalTest, ExactResolversDoNotConsultProcessGlobalFallbacks) {
    std::atomic<int> calls{0};
    NodeFactory::instance().register_type("core-global-only-node", factory(&calls),
                                          json{{"type", "object"}});
    ReducerRegistry::instance().register_reducer(
        "core-global-only-reducer",
        [](const json&, const json& incoming) { return json(incoming); });
    ConditionRegistry::instance().register_condition(
        "core-global-only-condition", [](const GraphState&) { return std::string("go"); });

    GraphRegistry local;
    EXPECT_THROW(
        (void)local.create_local("core-global-only-node", "work", json::object(), NodeContext{}),
        std::out_of_range);
    EXPECT_THROW((void)local.local_reducer("core-global-only-reducer"), std::out_of_range);
    EXPECT_THROW((void)local.local_condition("core-global-only-condition"), std::out_of_range);
    EXPECT_EQ(calls.load(), 0);
}

TEST(GraphRegistryLocalTest, LocalCompileUsesOnlyInstanceOwnedCallables) {
    std::atomic<int> calls{0};
    GraphRegistry    local;
    local.register_type("core-local-node", factory(&calls), json{{"type", "object"}},
                        json::object());

    auto topology = GraphCompiler::parse_local(node_definition("core-local-node"), local);
    auto report   = GraphValidator::validate_local(topology, local);
    EXPECT_FALSE(report.has_errors()) << report.summary();
    auto graph           = GraphCompiler::link_local(std::move(topology), NodeContext{}, local);
    auto compiled_report = GraphValidator::validate_local(graph, local);
    EXPECT_FALSE(compiled_report.has_errors()) << compiled_report.summary();

    EXPECT_EQ(graph.nodes.size(), 1U);
    EXPECT_EQ(calls.load(), 1);
}

TEST(GraphRegistryLocalTest, LocalParseRejectsGlobalOnlyNodeBeforeFactoryDispatch) {
    std::atomic<int> calls{0};
    NodeFactory::instance().register_type("core-global-parse-node", factory(&calls),
                                          json{{"type", "object"}});

    GraphRegistry local;
    EXPECT_THROW((void)GraphCompiler::parse_local(node_definition("core-global-parse-node"), local),
                 std::out_of_range);
    EXPECT_EQ(calls.load(), 0);
    EXPECT_THROW((void)GraphCompiler::parse_local(node_definition(""), local), std::out_of_range);
}
