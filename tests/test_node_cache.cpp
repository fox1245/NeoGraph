#include <gtest/gtest.h>

#include <neograph/graph/node_cache.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Test node that bumps a shared counter every time it executes.
// We rely on counter monotonicity to detect cache hits (no bump).
class CountingNode : public GraphNode {
public:
    CountingNode(std::string name, std::shared_ptr<std::atomic<int>> counter)
        : name_(std::move(name)), counter_(std::move(counter)) {}

    std::string get_name() const override { return name_; }

    asio::awaitable<NodeOutput> run(NodeInput in) override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        std::string value = "hit";
        if (in.ctx.store) {
            if (auto item = in.ctx.store->get({"cache"}, "value")) {
                value = item->value.get<std::string>();
            }
        }
        if (in.ctx.tool_gate) {
            auto decision = co_await in.ctx.tool_gate(ToolCall{}, ToolGateContext{});
            value = decision.kind == ToolDecision::Kind::Allow ? "allow" : "deny";
        }
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"out", json(value)});
        co_return out;
    }

private:
    std::string name_;
    std::shared_ptr<std::atomic<int>> counter_;
};

std::unique_ptr<GraphEngine> build_engine_with_node(
    std::shared_ptr<std::atomic<int>> counter) {

    auto cls_name = std::string("counting_test_node");
    NodeFactory::instance().register_type(
        cls_name,
        [counter](const std::string& name, const json&,
                  const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<CountingNode>(name, counter);
        });

    json defn = {
        {"name", "cache_test"},
        {"channels", {{"out", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"work", {{"type", "counting_test_node"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "work"}},
            {{"from", "work"},      {"to", "__end__"}},
        })},
    };
    auto e = GraphEngine::compile(defn, NodeContext());
    e->set_checkpoint_store(std::make_shared<InMemoryCheckpointStore>());
    return e;
}

} // namespace

namespace {

NodeResult cache_result(int value) {
    return NodeResult{{ChannelWrite{"out", json(value)}}};
}

int cached_value(const std::optional<NodeResult>& result) {
    return result->writes[0].value.get<int>();
}

} // namespace

TEST(NodeCache, ZeroCapacityPreservesUnboundedBehavior) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(0);

    for (int i = 0; i < 100; ++i) {
        cache.store("node", std::to_string(i), cache_result(i));
    }

    EXPECT_EQ(cache.size(), 100u);
    EXPECT_EQ(cache.max_entries(), 0u);
    EXPECT_EQ(cache.eviction_count(), 0u);
}

TEST(NodeCache, BoundedCacheEvictsLeastRecentlyUsedEntry) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(2);
    cache.store("node", "a", cache_result(1));
    cache.store("node", "b", cache_result(2));
    ASSERT_TRUE(cache.lookup("node", "a"));  // b is now least-recently used
    cache.store("node", "c", cache_result(3));

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.eviction_count(), 1u);
    EXPECT_FALSE(cache.lookup("node", "b"));
    EXPECT_EQ(cached_value(cache.lookup("node", "a")), 1);
    EXPECT_EQ(cached_value(cache.lookup("node", "c")), 3);
}

TEST(NodeCache, OverwriteRefreshesRecencyWithoutCountingEviction) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(2);
    cache.store("node", "a", cache_result(1));
    cache.store("node", "b", cache_result(2));
    cache.store("node", "a", cache_result(10));
    EXPECT_EQ(cache.eviction_count(), 0u);

    cache.store("node", "c", cache_result(3));
    EXPECT_FALSE(cache.lookup("node", "b"));
    EXPECT_EQ(cached_value(cache.lookup("node", "a")), 10);
    EXPECT_EQ(cache.eviction_count(), 1u);
}

TEST(NodeCache, LoweringCapacityEvictsImmediatelyWithStableTieBreak) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.store("node", "c", cache_result(3));
    cache.store("node", "a", cache_result(1));
    cache.store("node", "b", cache_result(2));

    cache.set_max_entries(2);
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_FALSE(cache.lookup("node", "a"));
    EXPECT_EQ(cache.eviction_count(), 1u);

    cache.set_max_entries(1);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.lookup("node", "b"));
    EXPECT_EQ(cached_value(cache.lookup("node", "c")), 3);
    EXPECT_EQ(cache.eviction_count(), 2u);
}

TEST(NodeCache, ClearDoesNotCountAsEvictionAndPreservesCapacity) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(2);
    cache.store("node", "a", cache_result(1));
    cache.store("node", "b", cache_result(2));
    cache.clear();

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.max_entries(), 2u);
    EXPECT_EQ(cache.eviction_count(), 0u);
}

TEST(NodeCache, DiverseInputStressRemainsCountBounded) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(64);

    for (int i = 0; i < 10000; ++i) {
        cache.store("node", std::to_string(i), cache_result(i));
    }

    EXPECT_EQ(cache.size(), 64u);
    EXPECT_EQ(cache.eviction_count(), 10000u - 64u);
}

TEST(NodeCache, ConcurrentLookupInsertAndEvictionRemainBounded) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(32);

    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&cache, worker] {
            for (int i = 0; i < 2000; ++i) {
                const auto key = std::to_string(worker) + ":" + std::to_string(i);
                cache.store("node", key, cache_result(i));
                (void)cache.lookup("node", key);
                (void)cache.lookup("node", std::to_string((i + 7) % 64));
            }
        });
    }
    for (auto& worker : workers) worker.join();

    EXPECT_LE(cache.size(), 32u);
    EXPECT_GT(cache.eviction_count(), 0u);
}

TEST(NodeCache, ConcurrentCapacityChangesRemainRaceFreeAndApplyImmediately) {
    NodeCache cache;
    cache.set_enabled("node", true);
    cache.set_max_entries(32);

    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&cache, &start, worker] {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < 1000; ++i) {
                const auto key = std::to_string(worker) + ":" + std::to_string(i);
                cache.store("node", key, cache_result(i));
                (void)cache.lookup("node", key);
            }
        });
    }
    std::thread reconfigure([&cache, &start] {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < 500; ++i) {
            cache.set_max_entries(1u + static_cast<std::size_t>(i % 32));
        }
    });
    for (auto& worker : workers) worker.join();
    reconfigure.join();

    cache.set_max_entries(16);
    EXPECT_LE(cache.size(), 16u);
    EXPECT_GT(cache.eviction_count(), 0u);
}

TEST(NodeCache, DisabledByDefault) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);

    RunConfig cfg;
    cfg.thread_id = "t1";
    cfg.input     = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    cfg.thread_id = "t2";
    engine->run(cfg);

    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().hit_count(), 0u);
    EXPECT_EQ(engine->node_cache().miss_count(), 0u);
}

TEST(NodeCache, DefaultScopePartitionsTenants) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);
    engine->set_node_cache_enabled("work", true);

    RunConfig cfg;
    cfg.input = json::object();
    cfg.max_steps = 5;
    cfg.thread_id = "tenant-a";
    engine->run(cfg);
    cfg.thread_id = "tenant-b";
    engine->run(cfg);

    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().hit_count(), 0u);
    EXPECT_EQ(engine->node_cache().miss_count(), 2u);
    EXPECT_EQ(engine->node_cache().size(), 2u);
}

TEST(NodeCache, EnabledNodeReplaysCachedResult) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);

    engine->set_node_cache_enabled(
        "work", true, CacheKeyPolicy{CacheScope::Reusable, {}});

    RunConfig cfg;
    cfg.thread_id = "t1";
    cfg.input     = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    cfg.thread_id = "t2";
    engine->run(cfg);
    cfg.thread_id = "t3";
    engine->run(cfg);

    // First run misses → counter=1. Subsequent runs hit on identical
    // state hash → counter stays at 1.
    EXPECT_EQ(counter->load(), 1);
    EXPECT_EQ(engine->node_cache().hit_count(), 2u);
    EXPECT_EQ(engine->node_cache().miss_count(), 1u);
    EXPECT_EQ(engine->node_cache().size(), 1u);
}

TEST(NodeCache, ClearDropsEntriesButKeepsEnableState) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);

    engine->set_node_cache_enabled("work", true);

    RunConfig cfg;
    cfg.thread_id = "t1";
    cfg.input     = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    EXPECT_EQ(counter->load(), 1);
    EXPECT_EQ(engine->node_cache().size(), 1u);

    engine->clear_node_cache();
    EXPECT_EQ(engine->node_cache().size(), 0u);

    cfg.thread_id = "t2";
    engine->run(cfg);

    // Cache was cleared → second run executes again, populating cache.
    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().size(), 1u);
}

TEST(NodeCache, DisablingNodeStopsLookups) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);

    engine->set_node_cache_enabled("work", true);
    RunConfig cfg;
    cfg.thread_id = "t1";
    cfg.input     = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    EXPECT_EQ(counter->load(), 1);

    engine->set_node_cache_enabled("work", false);
    cfg.thread_id = "t2";
    engine->run(cfg);
    cfg.thread_id = "t3";
    engine->run(cfg);

    // Cache disabled → both subsequent runs execute the node.
    EXPECT_EQ(counter->load(), 3);
}

TEST(NodeCache, DifferentInputsProduceDifferentEntries) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);

    engine->set_node_cache_enabled("work", true);

    RunConfig cfg1;
    cfg1.thread_id = "t1";
    cfg1.input     = json{{"out", json("seed_a")}};
    cfg1.max_steps = 5;
    engine->run(cfg1);

    RunConfig cfg2;
    cfg2.thread_id = "t2";
    cfg2.input     = json{{"out", json("seed_b")}};
    cfg2.max_steps = 5;
    engine->run(cfg2);

    // Different seed inputs → different state hashes → two misses.
    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().size(), 2u);
}
TEST(NodeCache, ReusableScopePartitionsDeclaredContext) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine  = build_engine_with_node(counter);
    engine->set_node_cache_enabled(
        "work", true, CacheKeyPolicy{
            CacheScope::Reusable,
            [](const RunContext& ctx) { return ctx.thread_id; }});

    RunConfig cfg;
    cfg.input = json::object();
    cfg.max_steps = 5;
    cfg.thread_id = "tenant-a";
    engine->run(cfg);
    engine->run(cfg);
    cfg.thread_id = "tenant-b";
    engine->run(cfg);

    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().hit_count(), 1u);
    EXPECT_EQ(engine->node_cache().miss_count(), 2u);
}
std::string cached_out(const RunResult& result) {
    return result.output["channels"]["out"]["value"].get<std::string>();
}

TEST(NodeCache, DefaultScopePartitionsStoreAndToolGate) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine = build_engine_with_node(counter);
    engine->set_node_cache_enabled("work", true);

    auto first_store = std::make_shared<InMemoryStore>();
    first_store->put({"cache"}, "value", "store-a");
    engine->set_store(first_store);
    RunConfig cfg;
    cfg.thread_id = "same-thread";
    cfg.input = json::object();
    cfg.max_steps = 5;
    EXPECT_EQ(cached_out(engine->run(cfg)), "store-a");

    auto second_store = std::make_shared<InMemoryStore>();
    second_store->put({"cache"}, "value", "store-b");
    engine->set_store(second_store);
    EXPECT_EQ(cached_out(engine->run(cfg)), "store-b");

    engine->set_store({});
    engine->set_tool_gate([](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        co_return ToolDecision::allow();
    });
    EXPECT_EQ(cached_out(engine->run(cfg)), "allow");
    engine->set_tool_gate([](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
        co_return ToolDecision::deny("policy-b");
    });
    EXPECT_EQ(cached_out(engine->run(cfg)), "deny");
    EXPECT_EQ(counter->load(), 4);
    EXPECT_EQ(engine->node_cache().hit_count(), 0u);
}

TEST(NodeCache, LegacyEnableResetsReusablePolicy) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine = build_engine_with_node(counter);
    engine->set_node_cache_enabled("work", true, CacheKeyPolicy{CacheScope::Reusable, {}});
    RunConfig cfg;
    cfg.input = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    engine->set_node_cache_enabled("work", false);
    engine->set_node_cache_enabled("work", true);
    engine->run(cfg);
    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().hit_count(), 0u);
}
TEST(NodeCache, DefaultScopePartitionsAutoResume) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    auto engine = build_engine_with_node(counter);
    engine->set_node_cache_enabled("work", true);

    RunConfig cfg;
    cfg.thread_id = "resume-thread";
    cfg.input = json::object();
    cfg.max_steps = 5;
    engine->run(cfg);
    cfg.resume_if_exists = true;
    engine->run(cfg);

    EXPECT_EQ(counter->load(), 2);
    EXPECT_EQ(engine->node_cache().hit_count(), 0u);
    EXPECT_EQ(engine->node_cache().miss_count(), 2u);
}
