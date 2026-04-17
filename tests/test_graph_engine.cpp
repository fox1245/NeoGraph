#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <thread>
#include <future>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

// ── Helper: minimal graph JSON ──

static json make_linear_graph(const std::string& node_name = "worker") {
    return {
        {"name", "test_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {node_name, {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}}
        }}
    };
}

static json make_conditional_graph() {
    return {
        {"name", "cond_graph"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"router",  {{"type", "custom"}}},
            {"path_a",  {{"type", "custom"}}},
            {"path_b",  {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "router"}},
            {{"from", "router"}, {"condition", "route_channel"},
             {"routes", {{"a", "path_a"}, {"b", "path_b"}}}},
            {{"from", "path_a"}, {"to", "__end__"}},
            {{"from", "path_b"}, {"to", "__end__"}}
        }}
    };
}

// ── Custom node that writes to result ──

class EchoNode : public GraphNode {
public:
    EchoNode(const std::string& name, const std::string& value)
        : name_(name), value_(value) {}

    std::vector<ChannelWrite> execute(const GraphState& /*state*/) override {
        return {ChannelWrite{"result", json(value_)}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
    std::string value_;
};

// ── Router node that writes to __route__ ──

class RouterNode : public GraphNode {
public:
    RouterNode(const std::string& name, const std::string& route)
        : name_(name), route_(route) {}

    std::vector<ChannelWrite> execute(const GraphState& /*state*/) override {
        return {ChannelWrite{"__route__", json(route_)}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
    std::string route_;
};

// ── Test fixture ──

class GraphEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register custom node type for tests
        NodeFactory::instance().register_type("custom",
            [](const std::string& name, const json& /*config*/, const NodeContext& /*ctx*/) {
                return std::make_unique<EchoNode>(name, "done_by_" + name);
            });
    }
};

// ── Linear execution ──

TEST_F(GraphEngineTest, LinearExecution) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 1);
    EXPECT_EQ(result.execution_trace[0], "worker");
}

// ── Result channel written ──

TEST_F(GraphEngineTest, ResultChannelWritten) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_TRUE(result.output.contains("channels"));
    auto channels = result.output["channels"];
    ASSERT_TRUE(channels.contains("result"));
    EXPECT_EQ(channels["result"]["value"], "done_by_worker");
}

// ── Conditional routing ──

TEST_F(GraphEngineTest, ConditionalRoutingA) {
    // Override to route to "a"
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "a");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    EXPECT_FALSE(result.interrupted);
    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[0], "router");
    EXPECT_EQ(result.execution_trace[1], "path_a");
}

TEST_F(GraphEngineTest, ConditionalRoutingB) {
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            if (name == "router") return std::make_unique<RouterNode>(name, "b");
            return std::make_unique<EchoNode>(name, "done_by_" + name);
        });

    auto engine = GraphEngine::compile(make_conditional_graph(), NodeContext{});
    RunConfig config;
    auto result = engine->run(config);

    ASSERT_EQ(result.execution_trace.size(), 2);
    EXPECT_EQ(result.execution_trace[1], "path_b");
}

// ── Max steps safety ──

TEST_F(GraphEngineTest, MaxStepsLimit) {
    // Create a cycle: worker -> worker (via condition always routing back)
    json cycle_graph = {
        {"name", "cycle"},
        {"channels", {
            {"result", {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"looper", {{"type", "custom"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "looper"}},
            {{"from", "looper"}, {"to", "looper"}}
        }}
    };

    auto engine = GraphEngine::compile(cycle_graph, NodeContext{});
    RunConfig config;
    config.max_steps = 5;
    auto result = engine->run(config);

    // Should not run forever — capped by max_steps
    EXPECT_LE(result.execution_trace.size(), 5u);
}

// ── Streaming callback invoked ──

TEST_F(GraphEngineTest, StreamCallbackFires) {
    // Restore default custom type
    NodeFactory::instance().register_type("custom",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<EchoNode>(name, "done");
        });

    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;

    int event_count = 0;
    auto result = engine->run_stream(config, [&](const GraphEvent& /*ev*/) {
        ++event_count;
    });

    EXPECT_GT(event_count, 0);
    EXPECT_FALSE(result.interrupted);
}

// ── Empty input ──

TEST_F(GraphEngineTest, EmptyInput) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    RunConfig config;
    // No input — should still execute
    auto result = engine->run(config);
    EXPECT_FALSE(result.interrupted);
    EXPECT_EQ(result.execution_trace.size(), 1);
}

// ── Graph name ──

TEST_F(GraphEngineTest, GraphName) {
    auto engine = GraphEngine::compile(make_linear_graph(), NodeContext{});
    EXPECT_EQ(engine->graph_name(), "test_graph");
}

// ── Concurrency: shared engine, distinct thread_ids ──
//
// Verifies that one GraphEngine instance can serve many concurrent run()
// calls with different thread_ids without cross-contamination, races, or
// checkpoint store corruption. This is the property we want to document
// for users who want to host multi-tenant agent workloads on top of
// NeoGraph without writing a separate async layer.

namespace {

// Stateless node that echoes the "input_val" channel * 2 into "result"
// with a small sleep to widen the race window.
class DoublerNode : public GraphNode {
public:
    DoublerNode(const std::string& name, std::atomic<int>* counter)
        : name_(name), counter_(counter) {}

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        json v = state.get("input_val");
        int input = v.is_number_integer() ? v.get<int>() : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (counter_) counter_->fetch_add(1, std::memory_order_relaxed);
        return {ChannelWrite{"result", json(input * 2)}};
    }
    std::string name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int>* counter_;
};

static json make_doubler_graph() {
    return {
        {"name", "doubler"},
        {"channels", {
            {"input_val", {{"reducer", "overwrite"}}},
            {"result",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"doubler", {{"type", "doubler"}}}
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "doubler"}},
            {{"from", "doubler"},   {"to", "__end__"}}
        }}
    };
}

} // namespace

TEST_F(GraphEngineTest, ConcurrentRunDifferentThreadIds) {
    static std::atomic<int> g_counter{0};
    g_counter = 0;

    NodeFactory::instance().register_type("doubler",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<DoublerNode>(name, &g_counter);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_doubler_graph(), NodeContext{}, store);

    constexpr int N_THREADS       = 16;
    constexpr int RUNS_PER_THREAD = 25;

    std::vector<std::future<std::string>> futures;
    futures.reserve(N_THREADS);

    for (int t = 0; t < N_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&engine, t]() -> std::string {
            for (int j = 0; j < RUNS_PER_THREAD; ++j) {
                int input_val = t * 1000 + j;
                RunConfig cfg;
                cfg.thread_id = "tid_" + std::to_string(t) + "_" + std::to_string(j);
                cfg.input = {{"input_val", input_val}};

                RunResult r;
                try {
                    r = engine->run(cfg);
                } catch (const std::exception& e) {
                    return std::string("threw: ") + e.what();
                }
                if (r.interrupted) return "unexpected interrupt";
                if (!r.output.contains("channels")) return "missing channels";
                auto ch = r.output["channels"];
                if (!ch.contains("result")) return "missing result channel";
                int got = ch["result"]["value"].get<int>();
                int want = input_val * 2;
                if (got != want) {
                    return "mismatch: got=" + std::to_string(got) +
                           " want=" + std::to_string(want);
                }
            }
            return "";
        }));
    }

    for (int t = 0; t < N_THREADS; ++t) {
        std::string err = futures[t].get();
        EXPECT_TRUE(err.empty()) << "thread " << t << ": " << err;
    }

    EXPECT_EQ(g_counter.load(), N_THREADS * RUNS_PER_THREAD);

    // Each unique thread_id should have its own checkpoint(s) in the store.
    for (int t = 0; t < N_THREADS; ++t) {
        for (int j = 0; j < RUNS_PER_THREAD; ++j) {
            std::string tid = "tid_" + std::to_string(t) + "_" + std::to_string(j);
            auto cps = store->list(tid, 100);
            EXPECT_FALSE(cps.empty()) << "no checkpoints for " << tid;
        }
    }
}

TEST_F(GraphEngineTest, ConcurrentRunSameThreadIdNoCrash) {
    // Hammer one thread_id from many threads simultaneously. We don't
    // require deterministic semantics here (LangGraph doesn't either),
    // only that the engine + checkpoint store don't corrupt or crash.
    static std::atomic<int> g_counter{0};
    g_counter = 0;

    NodeFactory::instance().register_type("doubler",
        [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<DoublerNode>(name, &g_counter);
        });

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_doubler_graph(), NodeContext{}, store);

    constexpr int N_THREADS = 8;
    constexpr int RUNS = 50;

    std::vector<std::future<bool>> futures;
    for (int t = 0; t < N_THREADS; ++t) {
        futures.push_back(std::async(std::launch::async, [&engine, t]() {
            for (int j = 0; j < RUNS; ++j) {
                RunConfig cfg;
                cfg.thread_id = "shared_tid";
                cfg.input = {{"input_val", t * 100 + j}};
                try {
                    auto r = engine->run(cfg);
                    if (r.interrupted) return false;
                } catch (...) {
                    return false;
                }
            }
            return true;
        }));
    }
    for (auto& f : futures) EXPECT_TRUE(f.get());
    EXPECT_EQ(g_counter.load(), N_THREADS * RUNS);
}
