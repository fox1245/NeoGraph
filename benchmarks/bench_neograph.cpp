// Engine-overhead benchmark for NeoGraph.
//
// Three workloads, all with NO sleep / NO I/O so the number reflects
// dispatch + state + checkpoint-disabled overhead, not simulated work:
//
//   * seq       — 3-node coroutine chain, each increments a counter channel.
//   * seq_sync  — the same chain implemented with SyncGraphNode.
//   * par  — fan-out 5 workers + summarizer. Each worker appends
//            its index; summarizer counts.
//
// Graph is compiled once; invoke() is the hot loop.
// Usage: bench_neograph [seq_iters] [par_iters] [par_workers] [warmup] [samples]
// Each timing row reports the median of `samples` after `warmup` single-run
// warmups; par_workers is 1 (default), auto, or an explicit positive integer.

#include <neograph/neograph.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── Sequential workload: chain a → b → c ──────────────────────────────

class IncNode : public GraphNode {
public:
    explicit IncNode(std::string n) : n_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int cur = 0;
        auto v = in.state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"counter", json(cur + 1)});
        co_return out;
    }
    std::string get_name() const override { return n_; }
private:
    std::string n_;
};

class SyncIncNode : public SyncGraphNode {
public:
    explicit SyncIncNode(std::string n) : SyncGraphNode(std::move(n)) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        int cur = 0;
        auto v = in.state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        return NodeOutput{{ChannelWrite{"counter", json(cur + 1)}}};
    }
};

// ── Parallel workload: 5 workers + summarizer ────────────────────────

class WorkerNode : public GraphNode {
public:
    WorkerNode(std::string n, int idx) : n_(std::move(n)), idx_(idx) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({idx_})});
        co_return out;
    }
    std::string get_name() const override { return n_; }
private:
    std::string n_;
    int idx_;
};

class SumNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto r = in.state.get("results");
        int n = r.is_array() ? static_cast<int>(r.size()) : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"count", json(n)});
        co_return out;
    }
    std::string get_name() const override { return "summarizer"; }
};

static void register_types() {
    NodeFactory::instance().register_type("inc",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncNode>(name);
        });
    NodeFactory::instance().register_type("sync_inc",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SyncIncNode>(name);
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

static json seq_graph(const char* name = "seq", const char* node_type = "inc") {
    return {
        {"name", name},
        {"channels", {{"counter", {{"reducer", "overwrite"}}}}},
        {"nodes", {
            {"a", {{"type", node_type}}},
            {"b", {{"type", node_type}}},
            {"c", {{"type", node_type}}}
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
    double total_ms;
    double per_iter_us;
};

template <class Operation>
static BenchResult bench(int iters, int samples, Operation&& operation) {
    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        const auto started = std::chrono::steady_clock::now();
        operation(iters);
        const auto total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        timings.push_back(total_ms);
    }
    std::sort(timings.begin(), timings.end());
    const double median_ms = timings[timings.size() / 2];
    return {median_ms, median_ms * 1000.0 / static_cast<double>(iters)};
}

static std::size_t parse_positive(std::string_view text, const char* name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument(
            std::string(name) + " must be a positive integer");
    }
    return value;
}

static void run_graph(GraphEngine* engine, int iters) {
    RunConfig cfg;  // no thread_id → checkpoint coordinator disabled
    for (int i = 0; i < iters; ++i) (void)engine->run(cfg);
}

static void run_stream(GraphEngine* engine, int iters, StreamMode mode) {
    RunConfig cfg;
    cfg.stream_mode = mode;
    const GraphStreamCallback discard = [](const GraphEvent&) {};
    for (int i = 0; i < iters; ++i) (void)engine->run_stream(cfg, discard);
}


static std::string configure_par_workers(GraphEngine& engine,
                                         const std::string& mode) {
    if (mode == "auto") {
        engine.set_worker_count_auto();
        return "auto(" + std::to_string(
            std::max(std::thread::hardware_concurrency(), 1u)) + ")";
    }
    const auto workers = parse_positive(mode, "par_workers");
    engine.set_worker_count(workers);
    return std::to_string(workers);
}
int main(int argc, char** argv) {
    try {
        if (argc > 6) {
            throw std::invalid_argument(
                "usage: bench_neograph [seq_iters] [par_iters] [par_workers] "
                "[warmup] [samples]");
        }
        const int seq_iters = argc > 1
            ? static_cast<int>(parse_positive(argv[1], "seq_iters")) : 10000;
        const int par_iters = argc > 2
            ? static_cast<int>(parse_positive(argv[2], "par_iters")) : 5000;
        const std::string par_worker_mode = argc > 3 ? argv[3] : "1";
        const int warmup = argc > 4
            ? static_cast<int>(parse_positive(argv[4], "warmup")) : 10;
        const int samples = argc > 5
            ? static_cast<int>(parse_positive(argv[5], "samples")) : 5;

        register_types();

        std::cout << "config\tseq_iters\t" << seq_iters << '\n'
                  << "config\tpar_iters\t" << par_iters << '\n'
                  << "config\twarmup\t" << warmup << '\n'
                  << "config\tsamples\t" << samples << '\n'
                  << "runtime\tos\tLinux\n"
                  << "runtime\tarch\t"
#if defined(__x86_64__)
                  << "x86_64\n"
#elif defined(__aarch64__)
                  << "aarch64\n"
#else
                  << "unknown\n"
#endif
                  << "runtime\tcompiler\t" << __VERSION__ << '\n'
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
                  << "runtime\tbuild_type\t" << NEOGRAPH_BENCH_BUILD_TYPE << '\n'
#else
                  << "runtime\tbuild_type\tunspecified\n"
#endif
#if defined(NDEBUG)
                  << "runtime\toptimization\toptimized\n"
#else
                  << "runtime\toptimization\tunoptimized\n"
#endif
                  << "runtime\thardware_concurrency\t"
                  << std::max(std::thread::hardware_concurrency(), 1u) << '\n';

        // --- Sequential ---
        auto seq_engine = GraphEngine::compile(seq_graph(), NodeContext{});
        for (int i = 0; i < warmup; ++i) run_graph(seq_engine.get(), 1);
        const auto seq = bench(seq_iters, samples, [&](int n) {
            run_graph(seq_engine.get(), n);
        });
        std::cout << "seq\t" << seq_iters << "\t" << seq.total_ms
                  << "\t" << seq.per_iter_us << "\n";

        auto sync_engine = GraphEngine::compile(
            seq_graph("seq_sync", "sync_inc"), NodeContext{});
        for (int i = 0; i < warmup; ++i) run_graph(sync_engine.get(), 1);
        const auto seq_sync = bench(seq_iters, samples, [&](int n) {
            run_graph(sync_engine.get(), n);
        });
        std::cout << "seq_sync\t" << seq_iters << "\t" << seq_sync.total_ms
                  << "\t" << seq_sync.per_iter_us << "\n";

        // --- Parallel ---
        auto par_engine = GraphEngine::compile(par_graph(), NodeContext{});
        const auto par_workers = configure_par_workers(*par_engine, par_worker_mode);
        std::cout << "config\tpar_workers\t" << par_workers << "\n";
        for (int i = 0; i < warmup; ++i) run_graph(par_engine.get(), 1);
        const auto par = bench(par_iters, samples, [&](int n) {
            run_graph(par_engine.get(), n);
        });
        std::cout << "par\t" << par_iters << "\t" << par.total_ms
                  << "\t" << par.per_iter_us << "\n";

        // A callback with TOKENS emits nothing for this graph. Comparing it
        // with EVENTS isolates lifecycle-event construction and callback
        // dispatch.
        for (int i = 0; i < warmup; ++i)
            run_stream(seq_engine.get(), 1, StreamMode::TOKENS);
        const auto seq_stream_idle = bench(seq_iters, samples, [&](int n) {
            run_stream(seq_engine.get(), n, StreamMode::TOKENS);
        });
        std::cout << "seq_stream_idle\t" << seq_iters << "\t"
                  << seq_stream_idle.total_ms << "\t"
                  << seq_stream_idle.per_iter_us << "\n";

        for (int i = 0; i < warmup; ++i)
            run_stream(seq_engine.get(), 1, StreamMode::EVENTS);
        const auto seq_events = bench(seq_iters, samples, [&](int n) {
            run_stream(seq_engine.get(), n, StreamMode::EVENTS);
        });
        std::cout << "seq_events\t" << seq_iters << "\t"
                  << seq_events.total_ms << "\t" << seq_events.per_iter_us
                  << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
