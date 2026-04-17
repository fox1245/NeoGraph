// Engine-overhead benchmark for NeoGraph.
//
// Two workloads, both with NO sleep / NO I/O so the number reflects
// dispatch + state + checkpoint-disabled overhead, not simulated work:
//
//   * seq  — 3-node chain, each increments a counter channel.
//   * par  — fan-out 5 workers + summarizer. Each worker appends
//            its index; summarizer counts.
//
// Graph is compiled once; invoke() is the hot loop.

#include <neograph/neograph.h>
#include <chrono>
#include <iostream>
#include <memory>

using namespace neograph;
using namespace neograph::graph;

// ── Sequential workload: chain a → b → c ──────────────────────────────

class IncNode : public GraphNode {
public:
    explicit IncNode(std::string n) : n_(std::move(n)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int cur = 0;
        auto v = state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        return {ChannelWrite{"counter", json(cur + 1)}};
    }
    std::string name() const override { return n_; }
private:
    std::string n_;
};

// ── Parallel workload: 5 workers + summarizer ────────────────────────

class WorkerNode : public GraphNode {
public:
    WorkerNode(std::string n, int idx) : n_(std::move(n)), idx_(idx) {}
    std::vector<ChannelWrite> execute(const GraphState&) override {
        return {ChannelWrite{"results", json::array({idx_})}};
    }
    std::string name() const override { return n_; }
private:
    std::string n_;
    int idx_;
};

class SumNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto r = state.get("results");
        int n = r.is_array() ? static_cast<int>(r.size()) : 0;
        return {ChannelWrite{"count", json(n)}};
    }
    std::string name() const override { return "summarizer"; }
};

static void register_types() {
    NodeFactory::instance().register_type("inc",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncNode>(name);
        });
    NodeFactory::instance().register_type("worker",
        [](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<WorkerNode>(name, cfg.value("idx", 0));
        });
    NodeFactory::instance().register_type("summarizer",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SumNode>();
        });
}

static json seq_graph() {
    return {
        {"name", "seq"},
        {"channels", {{"counter", {{"reducer", "overwrite"}}}}},
        {"nodes", {
            {"a", {{"type", "inc"}}},
            {"b", {{"type", "inc"}}},
            {"c", {{"type", "inc"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"},         {"to", "b"}},
            {{"from", "b"},         {"to", "c"}},
            {{"from", "c"},         {"to", "__end__"}}
        })}
    };
}

static json par_graph() {
    return {
        {"name", "par"},
        {"channels", {
            {"results", {{"reducer", "append"}}},
            {"count",   {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"w1", {{"type", "worker"}, {"idx", 1}}},
            {"w2", {{"type", "worker"}, {"idx", 2}}},
            {"w3", {{"type", "worker"}, {"idx", 3}}},
            {"w4", {{"type", "worker"}, {"idx", 4}}},
            {"w5", {{"type", "worker"}, {"idx", 5}}},
            {"summarizer", {{"type", "summarizer"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "w1"}},
            {{"from", "__start__"}, {"to", "w2"}},
            {{"from", "__start__"}, {"to", "w3"}},
            {{"from", "__start__"}, {"to", "w4"}},
            {{"from", "__start__"}, {"to", "w5"}},
            {{"from", "w1"},        {"to", "summarizer"}},
            {{"from", "w2"},        {"to", "summarizer"}},
            {{"from", "w3"},        {"to", "summarizer"}},
            {{"from", "w4"},        {"to", "summarizer"}},
            {{"from", "w5"},        {"to", "summarizer"}},
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };
}

struct BenchResult {
    long total_ms;
    double per_iter_us;
};

static BenchResult bench(GraphEngine* engine, int iters) {
    RunConfig cfg;  // no thread_id → checkpoint coordinator disabled

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        (void)engine->run(cfg);
    }
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    return {total_ms, (total_ms * 1000.0) / iters};
}

int main(int argc, char** argv) {
    const int seq_iters = (argc > 1) ? std::atoi(argv[1]) : 10000;
    const int par_iters = (argc > 2) ? std::atoi(argv[2]) : 5000;

    register_types();

    // Warm-up so first iteration doesn't pay type-system init costs.
    auto warm = GraphEngine::compile(seq_graph(), NodeContext{});
    for (int i = 0; i < 10; ++i) (void)warm->run(RunConfig{});

    // --- Sequential ---
    auto seq_engine = GraphEngine::compile(seq_graph(), NodeContext{});
    auto seq = bench(seq_engine.get(), seq_iters);
    std::cout << "seq\t" << seq_iters << "\t" << seq.total_ms
              << "\t" << seq.per_iter_us << "\n";

    // --- Parallel ---
    auto par_engine = GraphEngine::compile(par_graph(), NodeContext{});
    auto par = bench(par_engine.get(), par_iters);
    std::cout << "par\t" << par_iters << "\t" << par.total_ms
              << "\t" << par.per_iter_us << "\n";

    return 0;
}
