// NeoGraph Example 42: Per-node result cache (NodeCache)
//
// Demonstrates GraphEngine::set_node_cache_enabled — replays a node's
// outcome when the input state hash is unchanged. Useful for expensive
// pure nodes (embedding, deterministic LLM, heavy compute).
//
// We instrument a counter node so we can prove the cache short-circuits
// the second run; hit_count must be 1, miss_count must be 1.
//
// Usage: ./example_node_cache

#include <neograph/neograph.h>

#include <atomic>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// "Expensive" node — bumps a global counter every time its body runs.
// If the cache works, the counter stays at 1 after two runs.
std::atomic<int> g_exec_count{0};

class ExpensiveNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        g_exec_count.fetch_add(1, std::memory_order_relaxed);
        int x = 0;
        auto v = in.state.get("x");
        if (v.is_number()) x = v.get<int>();
        // Pretend this is an expensive LLM/embedding/etc.
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(x * x)});
        co_return out;
    }
    std::string get_name() const override { return "expensive"; }
};

int main() {
    NodeFactory::instance().register_type("expensive",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ExpensiveNode>();
        });

    json def = {
        {"name", "cache_demo"},
        {"channels", {
            {"x", {{"reducer", "overwrite"}}},
            {"y", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {{"expensive", {{"type", "expensive"}}}}},
        {"edges", {
            {{"from", "__start__"}, {"to", "expensive"}},
            {{"from", "expensive"}, {"to", "__end__"}},
        }},
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    engine->set_node_cache_enabled("expensive", true);

    // First run — miss.
    RunConfig cfg;
    cfg.input = {{"x", 5}};
    auto r1 = engine->run(cfg);
    int exec_after_first = g_exec_count.load();
    std::cout << "run1 output=" << r1.output.dump()
              << " exec_count=" << exec_after_first
              << " cache_size=" << engine->node_cache().size()
              << " hits=" << engine->node_cache().hit_count()
              << " misses=" << engine->node_cache().miss_count() << "\n";

    // Second run with identical input — should hit cache, body must NOT run again.
    auto r2 = engine->run(cfg);
    int exec_after_second = g_exec_count.load();
    std::cout << "run2 output=" << r2.output.dump()
              << " exec_count=" << exec_after_second
              << " cache_size=" << engine->node_cache().size()
              << " hits=" << engine->node_cache().hit_count()
              << " misses=" << engine->node_cache().miss_count() << "\n";

    // Third run with different input — should miss again.
    RunConfig cfg2;
    cfg2.input = {{"x", 7}};
    auto r3 = engine->run(cfg2);
    int exec_after_third = g_exec_count.load();
    std::cout << "run3 output=" << r3.output.dump()
              << " exec_count=" << exec_after_third
              << " hits=" << engine->node_cache().hit_count()
              << " misses=" << engine->node_cache().miss_count() << "\n";

    // Gates.
    bool ok = true;
    if (exec_after_first != 1) {
        std::cerr << "FAIL: first run should execute once (got " << exec_after_first << ")\n"; ok = false;
    }
    if (exec_after_second != 1) {
        std::cerr << "FAIL: second run should NOT re-execute (got " << exec_after_second << ")\n"; ok = false;
    }
    if (exec_after_third != 2) {
        std::cerr << "FAIL: third run with new state should re-execute (got " << exec_after_third << ")\n"; ok = false;
    }
    if (engine->node_cache().hit_count() != 1) {
        std::cerr << "FAIL: expected 1 cache hit, got " << engine->node_cache().hit_count() << "\n"; ok = false;
    }
    if (engine->node_cache().miss_count() != 2) {
        std::cerr << "FAIL: expected 2 misses, got " << engine->node_cache().miss_count() << "\n"; ok = false;
    }

    if (!ok) return 1;
    std::cout << "PASS\n";
    return 0;
}
