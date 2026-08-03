#include <neograph/neograph.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

json invocation_graph() {
    return {
        {"name", "invocation_boundary"},
        {"channels", {{"result", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"writer", {{"type", "invocation_test_writer"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "writer"}},
          {{"from", "writer"}, {"to", "__end__"}}}},
    };
}

class InvocationWriter final : public GraphNode {
public:
    explicit InvocationWriter(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"result", json("completed")});
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class RunInvocationTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        NodeFactory::instance().register_type(
            "invocation_test_writer",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<InvocationWriter>(name);
            });
    }
};

std::shared_ptr<GraphEngine> make_engine() {
    return std::shared_ptr<GraphEngine>(
        GraphEngine::compile(invocation_graph(), NodeContext{}).release());
}

TEST_F(RunInvocationTest, RunsThroughOwnedBoundaryAndClassifiesResult) {
    std::vector<GraphEvent::Type> events;
    RunInvocationRequest request;
    request.config.thread_id = "invocation-complete";
    request.config.input = json{{"message", "hello"}};
    request.metadata.run_id = "durable-invocation";
    request.metadata.trace_id = "trace-invocation";
    request.on_event = [&events](const GraphEvent& event) {
        events.push_back(event.type);
    };

    RunInvocation invocation(make_engine(), std::move(request));
    auto result = invocation.run();

    ASSERT_EQ(result.status, InvocationStatus::Completed);
    ASSERT_TRUE(result.run_result.has_value());
    EXPECT_EQ(result.run_result->output["channels"]["result"]["value"],
              "completed");
    EXPECT_EQ(invocation.cancel_token()->is_cancelled(), false);
    ASSERT_GE(events.size(), 4U);
    EXPECT_EQ(events.front(), GraphEvent::Type::NODE_START);
    EXPECT_NE(std::find(events.begin(), events.end(), GraphEvent::Type::NODE_END),
              events.end());
}

TEST_F(RunInvocationTest, PreCancelledTokenProducesCancelledOutcome) {
    auto token = std::make_shared<CancelToken>();
    token->cancel();

    RunInvocationRequest request;
    request.config.thread_id = "invocation-cancelled";
    request.config.cancel_token = token;

    RunInvocation invocation(make_engine(), std::move(request));
    auto result = invocation.run();

    EXPECT_EQ(result.status, InvocationStatus::Cancelled);
    EXPECT_TRUE(result.cancelled());
    EXPECT_FALSE(result.run_result.has_value());
}

TEST_F(RunInvocationTest, RejectsSecondExecutionWithoutTouchingEngine) {
    RunInvocationRequest request;
    request.config.thread_id = "invocation-single-use";

    RunInvocation invocation(make_engine(), std::move(request));
    ASSERT_TRUE(invocation.run().succeeded());

    auto second = invocation.run();
    EXPECT_EQ(second.status, InvocationStatus::Error);
    EXPECT_FALSE(second.succeeded());
    EXPECT_EQ(second.error, "RunInvocation is single-use");
}

}  // namespace
