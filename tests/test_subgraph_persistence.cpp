#include <gtest/gtest.h>
#include <neograph/neograph.h>
#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#endif
#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {
json one_node(const std::string& name, const std::string& node_type);

class AppendHistoryNode final : public GraphNode {
public:
    explicit AppendHistoryNode(std::string name, std::string* observed_owner = nullptr)
        : name_(std::move(name)), observed_owner_(observed_owner) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        if (observed_owner_) *observed_owner_ = input.ctx.tool_execution_identity.owner_scope;
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"history", json::array({"tick"})});
        co_return output;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::string* observed_owner_;
};

class BlockingHistoryNode final : public GraphNode {
public:
    static inline std::atomic<bool> entered{false};
    static inline std::atomic<bool> release{false};

    explicit BlockingHistoryNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        NodeOutput output;
        output.writes.push_back(ChannelWrite{"history", json::array({"tick"})});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

std::shared_ptr<GraphEngine> shared_engine(
    const json& definition, std::shared_ptr<CheckpointStore> store = {}) {
    return std::shared_ptr<GraphEngine>(
        GraphEngine::compile(definition, NodeContext{}, std::move(store)).release());
}

std::unique_ptr<GraphEngine> make_nested(
    const std::shared_ptr<CheckpointStore>& root_store) {
    NodeFactory::instance().register_type("subgraph_leaf_238",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<AppendHistoryNode>(name);
        });
    const auto leaf = shared_engine(one_node("nested_leaf", "subgraph_leaf_238"));
    NodeFactory::instance().register_type("subgraph_inner_238",
        [leaf](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, leaf, std::map<std::string, std::string>{{"prompt", "prompt"}},
                std::map<std::string, std::string>{{"history", "history"}},
                SubgraphPersistence::PerThread);
        });
    const auto middle = shared_engine(one_node("nested_middle", "subgraph_inner_238"));
    NodeFactory::instance().register_type("subgraph_outer_238",
        [middle](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, middle, std::map<std::string, std::string>{{"prompt", "prompt"}},
                std::map<std::string, std::string>{{"history", "history"}},
                SubgraphPersistence::PerThread);
        });
    return GraphEngine::compile(
        one_node("nested_root", "subgraph_outer_238"), NodeContext{}, root_store);
}

json one_node(const std::string& name, const std::string& node_type) {
    return {{"name", name},
            {"channels", {{"prompt", {{"reducer", "overwrite"}}},
                          {"history", {{"reducer", "append"}}}}},
            {"nodes", {{"child", {{"type", node_type}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "child"}},
                                   {{"from", "child"}, {"to", "__end__"}}})}};
}

std::unique_ptr<GraphEngine> make_parent(
    std::shared_ptr<CheckpointStore> root_store,
    SubgraphPersistence policy,
    const std::string& type = "subgraph_policy_238") {
    NodeFactory::instance().register_type("subgraph_append_238",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<AppendHistoryNode>(name);
        });
    auto child = std::shared_ptr<GraphEngine>(
        GraphEngine::compile(one_node("policy_child", "subgraph_append_238"), NodeContext{}).release());
    NodeFactory::instance().register_type(type,
        [weak = std::weak_ptr<GraphEngine>(child), policy](
            const std::string& name, const json&, const NodeContext&) {
            auto locked = weak.lock();
            if (!locked) throw std::runtime_error("child engine expired during compilation");
            return std::make_unique<SubgraphNode>(
                name, std::move(locked),
                std::map<std::string, std::string>{{"prompt", "prompt"}},
                std::map<std::string, std::string>{{"history", "history"}}, policy);
        });
    return GraphEngine::compile(one_node("policy_parent", type), NodeContext{},
                                std::move(root_store));
}

RunConfig run_on(const std::string& thread) {
    RunConfig config;
    config.thread_id = thread;
    config.input = {{"prompt", "input"}};
    return config;
}

SubgraphPathStep child_path(const std::string& parent_cp) {
    return {"child", 0, "s0:child", parent_cp};
}

std::size_t history_size(const NestedCheckpoint& cp) {
    return cp.checkpoint.channel_values.at("channels").at("history").at("value").size();
}

class InputHistoryNode final : public GraphNode {
public:
    InputHistoryNode(std::string name, std::shared_ptr<std::atomic<int>> calls)
        : name_(std::move(name)), calls_(std::move(calls)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++*calls_;
        NodeOutput output;
        output.writes.push_back({"history", json::array({input.state.get("prompt")})});
        co_return output;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::shared_ptr<std::atomic<int>> calls_;
};

std::unique_ptr<GraphEngine> interrupt_parent(
    const std::shared_ptr<CheckpointStore>& store, SubgraphPersistence policy,
    const std::shared_ptr<std::atomic<int>>& calls,
    const std::string& child_interrupt = {}, bool parent_interrupt = false) {
    NodeFactory::instance().register_type("subgraph_input_regression",
        [calls](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<InputHistoryNode>(name, calls);
        });
    auto definition = one_node("interrupt_child", "subgraph_input_regression");
    if (!child_interrupt.empty()) definition[child_interrupt] = json::array({"child"});
    auto child = shared_engine(definition);
    NodeFactory::instance().register_type("subgraph_interrupt_regression",
        [child, policy](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child, std::map<std::string, std::string>{{"prompt", "prompt"}},
                std::map<std::string, std::string>{{"history", "history"}}, policy);
        });
    auto parent = one_node("interrupt_parent", "subgraph_interrupt_regression");
    if (parent_interrupt) parent["interrupt_before"] = json::array({"child"});
    return GraphEngine::compile(parent, NodeContext{}, store);
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
struct TempDatabase {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("neograph-subgraph-" + Checkpoint::generate_id() + ".sqlite");
    ~TempDatabase() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
    }
};
#endif

}  // namespace

TEST(SubgraphPersistence, PerInvocationUsesFreshNamespacesAcrossRunsAndResumes) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_parent(store, SubgraphPersistence::PerInvocation);
    const auto config = run_on("per-invocation-thread");
    auto first = engine->run(config);
    auto first_cp = engine->inspect_nested_checkpoint(config.thread_id,
                                                      {child_path(first.checkpoint_id)});
    ASSERT_TRUE(first_cp);
    EXPECT_EQ(history_size(*first_cp), 1u);
    auto second = engine->run(config);
    auto second_cp = engine->inspect_nested_checkpoint(config.thread_id,
                                                       {child_path(second.checkpoint_id)});
    ASSERT_TRUE(second_cp);
    EXPECT_EQ(history_size(*second_cp), 1u);
    EXPECT_NE(first_cp->thread_id, second_cp->thread_id);
    EXPECT_EQ(history_size(*engine->inspect_nested_checkpoint(
        config.thread_id, {child_path(first.checkpoint_id)})), 1u);
    EXPECT_EQ(store->load_latest(first_cp->thread_id)->id, first_cp->checkpoint.id);
}

TEST(SubgraphPersistence, PerThreadRetainsChannelsAndDefaultLegacyIdentityUnchanged) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto threaded = make_parent(store, SubgraphPersistence::PerThread);
    const auto config = run_on("per-thread-state");
    auto first = threaded->run(config);
    const auto one = threaded->inspect_nested_checkpoint(config.thread_id,
                                                          {child_path(first.checkpoint_id)});
    ASSERT_TRUE(one);
    EXPECT_EQ(history_size(*one), 1u);
    auto second = threaded->run(config);
    const auto two = threaded->inspect_nested_checkpoint(config.thread_id,
                                                          {child_path(second.checkpoint_id)});
    ASSERT_TRUE(two);
    EXPECT_EQ(one->thread_id, two->thread_id);
    EXPECT_EQ(history_size(*two), 2u);

    auto legacy = make_parent(store, SubgraphPersistence::Legacy, "subgraph_legacy_238");
    const auto root = run_on("legacy-thread");
    auto original = legacy->run(root);
    const auto saved = legacy->inspect_nested_checkpoint(root.thread_id,
                                                          {child_path(original.checkpoint_id)});
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->thread_id, "subgraph/13:legacy-thread5:child1:08:s0:child");
    EXPECT_EQ(history_size(*saved), 1u);
}

TEST(SubgraphPersistence, AdministrativeUpdatePreservesInterruptedInvocation) {
    for (bool ordered : {false, true}) {
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto engine = interrupt_parent(store, SubgraphPersistence::PerInvocation,
                                       calls, "interrupt_before");
        auto config = run_on("admin-invocation");
        ASSERT_TRUE(engine->run(config).interrupted);
        auto before = engine->inspect_nested_checkpoint(config.thread_id, {child_path("")});
        ASSERT_TRUE(before);
        for (int update = 0; update < 2; ++update) {
            if (ordered)
                engine->update_state_writes(config.thread_id, {{"prompt", "admin"}});
            else
                engine->update_state(config.thread_id, json{{"prompt", "admin"}});
        }
        auto result = engine->resume(config.thread_id);
        EXPECT_FALSE(result.interrupted);
        EXPECT_EQ(calls->load(), 1);
        EXPECT_EQ(result.channel<json>("history"), json::array({"input"}));
        auto after = engine->inspect_nested_checkpoint(config.thread_id, {child_path("")});
        ASSERT_TRUE(after);
        EXPECT_EQ(before->thread_id, after->thread_id);
    }
}

TEST(SubgraphPersistence, PerThreadFreshPausedParentExecutesNewChildInput) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto engine = interrupt_parent(store, SubgraphPersistence::PerThread, calls, {}, true);
    auto config = run_on("paused-thread-parent");
    ASSERT_TRUE(engine->run(config).interrupted);
    EXPECT_FALSE(engine->resume(config.thread_id).interrupted);
    auto first = engine->inspect_nested_checkpoint(config.thread_id, {child_path("")});
    ASSERT_TRUE(first);
    config.input = {{"prompt", "second"}};
    config.resume_if_exists = true;
    ASSERT_TRUE(engine->run(config).interrupted);
    auto result = engine->resume(config.thread_id);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(calls->load(), 2);
    EXPECT_EQ(result.channel<json>("history"), json::array({"input", "second"}));
    auto second = engine->inspect_nested_checkpoint(config.thread_id, {child_path("")});
    ASSERT_TRUE(second);
    EXPECT_EQ(first->thread_id, second->thread_id);
    EXPECT_EQ(second->checkpoint.channel_values.at("channels").at("history").at("value"),
              json::array({"input", "second"}));
}

TEST(SubgraphPersistence, PerThreadResumesSameInterruptedChildInvocation) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto engine = interrupt_parent(store, SubgraphPersistence::PerThread, calls,
                                   "interrupt_after");
    auto config = run_on("same-thread-parent");
    ASSERT_TRUE(engine->run(config).interrupted);
    auto result = engine->resume(config.thread_id);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(calls->load(), 1);
    EXPECT_EQ(result.channel<json>("history"), json::array({"input"}));
}

TEST(SubgraphPersistence, StatelessRejectsStaticInterruptBeforeEffects) {
    for (const auto& phase : {"interrupt_before", "interrupt_after"}) {
        auto store = std::make_shared<InMemoryCheckpointStore>();
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto engine = interrupt_parent(store, SubgraphPersistence::Stateless, calls, phase);
        EXPECT_THROW(engine->run(run_on("stateless-guard")), std::runtime_error);
        EXPECT_EQ(calls->load(), 0);
    }
}

TEST(SubgraphPersistence, StatelessChildDoesNotCreateAChildCheckpoint) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_parent(store, SubgraphPersistence::Stateless);
    const auto config = run_on("stateless-parent");
    auto result = engine->run(config);
    EXPECT_EQ(result.channel<json>("history"), json::array({"tick"}));
    EXPECT_THROW((void)engine->inspect_nested_checkpoint(
        config.thread_id, {child_path(result.checkpoint_id)}), std::invalid_argument);
}

TEST(SubgraphPersistence, NestedInspectionFollowsStableGraphPath) {
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = make_nested(store);
    const auto root_thread = "nested-thread";
    auto root_result = engine->run(run_on(root_thread));

    const auto middle = engine->inspect_nested_checkpoint(
        root_thread, {{"child", 0, "s0:child", root_result.checkpoint_id}});
    ASSERT_TRUE(middle);
    const auto leaf = engine->inspect_nested_checkpoint(
        root_thread,
        {{"child", 0, "s0:child", root_result.checkpoint_id},
         {"child", 0, "s0:child", middle->checkpoint.id}});
    ASSERT_TRUE(leaf);
    EXPECT_EQ(leaf->graph_path, std::vector<std::string>({"child", "child"}));
    EXPECT_EQ(history_size(*middle), 1u);
    EXPECT_EQ(history_size(*leaf), 1u);
    EXPECT_NE(middle->thread_id, leaf->thread_id);
}

TEST(SubgraphPersistence, PerThreadRejectsOverlappingCallsToOneNamespace) {
    BlockingHistoryNode::entered.store(false, std::memory_order_release);
    BlockingHistoryNode::release.store(false, std::memory_order_release);
    NodeFactory::instance().register_type("subgraph_blocking_238",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<BlockingHistoryNode>(name);
        });
    auto child = shared_engine(one_node("blocking_child", "subgraph_blocking_238"));
    NodeFactory::instance().register_type("subgraph_blocking_parent_238",
        [child](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SubgraphNode>(
                name, child, std::map<std::string, std::string>{{"prompt", "prompt"}},
                std::map<std::string, std::string>{{"history", "history"}},
                SubgraphPersistence::PerThread);
        });
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(
        one_node("blocking_parent", "subgraph_blocking_parent_238"),
        NodeContext{}, store);
    const auto config = run_on("collision-thread");
    std::exception_ptr first_error;
    std::atomic<bool> second_rejected{false};
    std::thread first([&] {
        try {
            (void)engine->run(config);
        } catch (...) {
            first_error = std::current_exception();
        }
    });
    for (int i = 0; i != 200 &&
         !BlockingHistoryNode::entered.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(BlockingHistoryNode::entered.load(std::memory_order_acquire));
    std::thread second([&] {
        try {
            (void)engine->run(config);
        } catch (const std::exception&) {
            second_rejected.store(true, std::memory_order_release);
        }
    });
    for (int i = 0; i != 100 &&
         !second_rejected.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    BlockingHistoryNode::release.store(true, std::memory_order_release);
    second.join();
    first.join();
    EXPECT_FALSE(first_error);
    EXPECT_TRUE(second_rejected.load(std::memory_order_acquire));
}

#ifdef NEOGRAPH_TESTS_HAVE_SQLITE
TEST(SubgraphPersistence, SQLiteReopenPreservesPerThreadChildState) {
    TempDatabase db;
    const auto root_thread = "sqlite-" + Checkpoint::generate_id();
    std::string child_thread;
    {
        auto store = std::make_shared<SqliteCheckpointStore>(db.path.string());
        auto engine = make_parent(store, SubgraphPersistence::PerThread);
        auto result = engine->run(run_on(root_thread));
        auto inspected = engine->inspect_nested_checkpoint(root_thread,
                                                            {child_path(result.checkpoint_id)});
        ASSERT_TRUE(inspected);
        child_thread = inspected->thread_id;
        EXPECT_EQ(history_size(*inspected), 1u);
    }
    {
        auto store = std::make_shared<SqliteCheckpointStore>(db.path.string());
        auto engine = make_parent(store, SubgraphPersistence::PerThread);
        auto result = engine->run(run_on(root_thread));
        auto inspected = engine->inspect_nested_checkpoint(root_thread,
                                                            {child_path(result.checkpoint_id)});
        ASSERT_TRUE(inspected);
        EXPECT_EQ(inspected->thread_id, child_thread);
        EXPECT_EQ(history_size(*inspected), 2u);
    }
}
#endif

#ifdef NEOGRAPH_TESTS_HAVE_POSTGRES
TEST(SubgraphPersistence, PostgreSQLReopenPreservesPerThreadChildState) {
    const char* url = std::getenv("NEOGRAPH_TEST_POSTGRES_URL");
    if (!url || !*url) GTEST_SKIP() << "NEOGRAPH_TEST_POSTGRES_URL not set";
    const auto root_thread = "postgres-" + Checkpoint::generate_id();
    std::string child_thread;
    {
        auto store = std::make_shared<PostgresCheckpointStore>(url);
        auto engine = make_parent(store, SubgraphPersistence::PerThread);
        auto result = engine->run(run_on(root_thread));
        auto inspected = engine->inspect_nested_checkpoint(root_thread,
                                                            {child_path(result.checkpoint_id)});
        ASSERT_TRUE(inspected);
        child_thread = inspected->thread_id;
        EXPECT_EQ(history_size(*inspected), 1u);
    }
    {
        auto store = std::make_shared<PostgresCheckpointStore>(url);
        auto engine = make_parent(store, SubgraphPersistence::PerThread);
        auto result = engine->run(run_on(root_thread));
        auto inspected = engine->inspect_nested_checkpoint(root_thread,
                                                            {child_path(result.checkpoint_id)});
        ASSERT_TRUE(inspected);
        EXPECT_EQ(history_size(*inspected), 2u);
        store->delete_thread(root_thread);
        store->delete_thread(child_thread);
    }
}
#endif
