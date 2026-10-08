// A node must see the identity of its own invocation, however it is scheduled.
// The engine binds a task id to the RunContext each node receives. These tests
// pin what a node can observe through the public API
// (make_tool_execution_context(ctx).effect_task_id): distinct per node, stable
// while the node is suspended and while sibling nodes run, deterministic from
// run to run, and not disturbed by a failed run or by overlapping calls into
// one NodeExecutor that share a RunContext.
#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

struct IdentityProbe {
    std::mutex mutex;
    // Node name -> [id at entry, id after one suspension].
    std::map<std::string, std::vector<std::string>> observed;
    bool fail_node_b = false;

    void add(const std::string& node, std::string id) {
        std::lock_guard<std::mutex> lock(mutex);
        observed[node].push_back(std::move(id));
    }
    std::map<std::string, std::vector<std::string>> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(observed, {});
    }
};

class IdentityNode final : public GraphNode {
public:
    IdentityNode(std::string name, std::shared_ptr<IdentityProbe> probe)
        : name_(std::move(name)), probe_(std::move(probe)) {}

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        probe_->add(name_, make_tool_execution_context(in.ctx).effect_task_id);
        // Let sibling branches run before the node looks again.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        probe_->add(name_, make_tool_execution_context(in.ctx).effect_task_id);
        if (name_ == "b" && probe_->fail_node_b)
            throw std::runtime_error("node b failed on purpose");
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<IdentityProbe> probe_;
};

void register_identity_node(const std::shared_ptr<IdentityProbe>& probe) {
    NodeFactory::instance().register_type(
        "node_invocation_identity_probe",
        [probe](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IdentityNode>(name, probe);
        });
}

json graph_of(const std::vector<std::string>& nodes,
              const std::vector<std::pair<std::string, std::string>>& edges) {
    json node_defs = json::object();
    for (const auto& name : nodes)
        node_defs[name] = json{{"type", "node_invocation_identity_probe"}};
    json edge_defs = json::array();
    for (const auto& [from, to] : edges)
        edge_defs.push_back(json{{"from", from}, {"to", to}});
    return json{{"name", "node_invocation_identity"},
                {"channels", {{"unused", {{"reducer", "overwrite"}}}}},
                {"nodes", node_defs},
                {"edges", edge_defs}};
}

// Every node saw the same id before and after suspending, and no id is empty.
void expect_each_node_stable(const std::map<std::string, std::vector<std::string>>& observed) {
    for (const auto& [node, ids] : observed) {
        ASSERT_EQ(ids.size(), 2u) << node;
        EXPECT_FALSE(ids[0].empty()) << node;
        EXPECT_EQ(ids[0], ids[1]) << node << " changed identity across a suspension";
    }
}

std::set<std::string> distinct_ids(const std::map<std::string, std::vector<std::string>>& observed) {
    std::set<std::string> ids;
    for (const auto& entry : observed) ids.insert(entry.second.front());
    return ids;
}

}  // namespace

TEST(NodeInvocationIdentity, ChainedNodesEachSeeTheirOwnIdAndRepeatItNextRun) {
    auto probe = std::make_shared<IdentityProbe>();
    register_identity_node(probe);
    auto engine = GraphEngine::compile(
        graph_of({"a", "b", "c"}, {{"__start__", "a"}, {"a", "b"}, {"b", "c"}, {"c", "__end__"}}),
        NodeContext{});

    engine->run(RunConfig{});
    const auto first = probe->take();
    ASSERT_EQ(first.size(), 3u);
    expect_each_node_stable(first);
    EXPECT_EQ(distinct_ids(first).size(), 3u) << "nodes of one run must not share an id";

    engine->run(RunConfig{});
    EXPECT_EQ(probe->take(), first) << "ids must be reproducible from run to run";
}

class NodeInvocationIdentityFanOut : public ::testing::TestWithParam<std::size_t> {};

TEST_P(NodeInvocationIdentityFanOut, ParallelBranchesKeepDistinctStableIds) {
    const std::size_t workers = GetParam();
    auto probe = std::make_shared<IdentityProbe>();
    register_identity_node(probe);

    std::vector<std::string> nodes{"join"};
    std::vector<std::pair<std::string, std::string>> edges{{"join", "__end__"}};
    // More branches than the engine's registry holds without growing.
    constexpr std::size_t kBranches = 300;
    for (std::size_t i = 0; i < kBranches; ++i) {
        const std::string name = "branch" + std::to_string(i);
        nodes.push_back(name);
        edges.emplace_back("__start__", name);
        edges.emplace_back(name, "join");
    }
    auto engine = GraphEngine::compile(graph_of(nodes, edges), NodeContext{});
    if (workers > 1) engine->set_worker_count(workers);

    engine->run(RunConfig{});
    const auto first = probe->take();
    ASSERT_EQ(first.size(), kBranches + 1);
    expect_each_node_stable(first);
    EXPECT_EQ(distinct_ids(first).size(), kBranches + 1)
        << "branches that ran side by side must not see each other's id";

    engine->run(RunConfig{});
    EXPECT_EQ(probe->take(), first);
}

INSTANTIATE_TEST_SUITE_P(Workers, NodeInvocationIdentityFanOut,
                         ::testing::Values(std::size_t{1}, std::size_t{4}));

TEST(NodeInvocationIdentity, FailedRunDoesNotDisturbTheNextRun) {
    auto probe = std::make_shared<IdentityProbe>();
    register_identity_node(probe);
    auto engine = GraphEngine::compile(
        graph_of({"a", "b", "c"}, {{"__start__", "a"}, {"a", "b"}, {"b", "c"}, {"c", "__end__"}}),
        NodeContext{});

    probe->fail_node_b = true;
    EXPECT_THROW(engine->run(RunConfig{}), std::runtime_error);
    const auto failed = probe->take();
    ASSERT_EQ(failed.size(), 2u) << "only a and b ran before the failure";

    probe->fail_node_b = false;
    engine->run(RunConfig{});
    const auto healthy = probe->take();
    ASSERT_EQ(healthy.size(), 3u);
    expect_each_node_stable(healthy);
    EXPECT_EQ(healthy.at("a"), failed.at("a"));
    EXPECT_EQ(healthy.at("b"), failed.at("b"));
}

TEST(NodeInvocationIdentity, OverlappingExecutorCallsSharingOneContextKeepTheirOwnIds) {
    // NodeExecutor is documented as a stateless dispatcher: callers may drive
    // several invocations at once against one const RunContext. Interleave two
    // such calls on one io_context and check neither sees the other's id.
    auto probe = std::make_shared<IdentityProbe>();
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    nodes.emplace("left", std::make_unique<IdentityNode>("left", probe));
    nodes.emplace("right", std::make_unique<IdentityNode>("right", probe));
    const std::vector<ChannelDef> no_channels;
    NodeExecutor executor(nodes, no_channels, [](const std::string&) { return RetryPolicy{}; });

    const RunContext ctx;
    const std::unordered_map<std::string, NodeResult> no_replay;
    const BarrierState barrier;
    const GraphStreamCallback no_callback;

    std::map<std::string, std::vector<std::string>> first;
    for (int round = 0; round < 2; ++round) {
        asio::io_context io;
        GraphState left_state, right_state;
        CheckpointCoordinator left_coord(nullptr, ""), right_coord(nullptr, "");
        std::vector<std::string> left_trace, right_trace;
        auto call = [&](std::string node, GraphState& state, CheckpointCoordinator& coord,
                        std::vector<std::string>& trace) -> asio::awaitable<void> {
            try {
                co_await executor.run_one_async(node, 0, state, no_replay, coord, "", barrier, trace,
                                                no_callback, StreamMode::ALL, ctx);
            } catch (const std::exception& error) {
                ADD_FAILURE() << node << " threw: " << error.what();
            }
        };
        asio::co_spawn(io, call("left", left_state, left_coord, left_trace), asio::detached);
        asio::co_spawn(io, call("right", right_state, right_coord, right_trace), asio::detached);
        io.run();

        const auto observed = probe->take();
        ASSERT_EQ(observed.size(), 2u);
        expect_each_node_stable(observed);
        EXPECT_EQ(distinct_ids(observed).size(), 2u);
        if (round == 0) first = observed;
        else EXPECT_EQ(observed, first);
    }
}
