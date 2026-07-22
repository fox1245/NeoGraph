#include <neograph/graph/node.h>

#include <gtest/gtest.h>

using namespace neograph::graph;

namespace {

class HeaderOnlyContextNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        observed_thread_id = in.ctx.thread_id;
        if (in.ctx.store) {
            (void)in.ctx.store->get({"header-test"}, "value");
        }
        co_return NodeOutput{};
    }

    std::string get_name() const override { return "header-only-context"; }

    std::string observed_thread_id;
};

}  // namespace

TEST(NodeHeader, ExposesCompleteRunContextWithoutEngineHeader) {
    HeaderOnlyContextNode node;
    EXPECT_EQ(node.get_name(), "header-only-context");
}
