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
//                       [--rows=seq,par,...] [--record] [--all-workers]
// Each timing row reports the median of `samples` after `warmup` single-run
// warmups; par_workers is 1 (default), auto, or an explicit positive integer.
//
// Every timed sample is checked: the last run of the sample and one extra run
// after the clock has stopped must end in the expected state (the counter is 3
// after the chain; the five workers' indices are present, count is 5 and the
// trace has six entries after the fan-out). A run that throws or ends in the
// wrong state makes the process print the first error on stderr and exit
// non-zero, so a timing is never printed for a cell that did not work.
//
// The options are for the cross-framework record (benchmarks/compare):
//   --rows=...      run only these rows (seq, seq_sync, par, seq_stream_idle, seq_events)
//   --record        also print one `record<TAB>{json}` line with the raw samples,
//                   the completions, the checks, the process memory readings and the
//                   build facts (hardening macros, compiler, hardware threads)
//   --all-workers   apply the worker mode to every engine, not only to par
// Without the options the output is exactly the rows the CI gates read.

#include <neograph/neograph.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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

// ── Checks: a timing is only printed for a cell that produced the right answer ──

struct Check {
    bool ok = true;
    std::string detail;
};

static json channel_value(const RunResult& result, const char* name) {
    const json& out = result.output;
    if (!out.is_object() || !out.contains("channels")) return json();
    const json channels = out["channels"];
    if (!channels.is_object() || !channels.contains(name)) return json();
    const json channel = channels[name];
    return channel.is_object() && channel.contains("value") ? channel["value"] : json();
}

static Check check_seq(const RunResult& result) {
    const json counter = channel_value(result, "counter");
    if (!counter.is_number() || counter.get<int>() != 3)
        return {false, "seq: expected counter == 3, got " + counter.dump()};
    if (result.execution_trace != std::vector<std::string>{"a", "b", "c"})
        return {false, "seq: expected the trace a,b,c, got " +
                       std::to_string(result.execution_trace.size()) + " entries"};
    if (result.interrupted) return {false, "seq: the run was interrupted"};
    return {};
}

static Check check_par(const RunResult& result) {
    const json results = channel_value(result, "results");
    if (!results.is_array() || results.size() != 5)
        return {false, "par: expected five results, got " + results.dump()};
    std::vector<int> indices;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const json item = results[i];
        // The append reducer concatenates the arrays each worker wrote, so an entry is [idx].
        const json value = item.is_array() && item.size() == 1 ? item[static_cast<std::size_t>(0)] : item;
        if (!value.is_number()) return {false, "par: a result is not a number: " + results.dump()};
        indices.push_back(value.get<int>());
    }
    std::sort(indices.begin(), indices.end());
    if (indices != std::vector<int>{1, 2, 3, 4, 5})
        return {false, "par: expected the indices 1..5, got " + results.dump()};
    const json count = channel_value(result, "count");
    if (!count.is_number() || count.get<int>() != 5)
        return {false, "par: expected count == 5, got " + count.dump()};
    if (result.execution_trace.size() != 6 || result.execution_trace.back() != "summarizer")
        return {false, "par: expected six trace entries ending in summarizer, got " +
                       std::to_string(result.execution_trace.size())};
    if (result.interrupted) return {false, "par: the run was interrupted"};
    return {};
}

struct RowResult {
    std::string name;
    int iters = 0;
    std::vector<double> sample_ms;
    double total_ms = 0;
    double per_iter_us = 0;
    long completions = 0;
    long failures = 0;
    long checked_runs = 0;
    std::string first_error;
};

static std::string g_first_error;

// Runs `samples` timed samples of `iters` runs. `operation(n)` performs n runs and returns the last
// result; `check` judges a result. The clock stops before any check, and one extra run is checked per
// sample, so the checks cost the timing nothing.
template <class Operation, class Verify>
static RowResult bench(const std::string& name, int iters, int samples,
                       Operation&& operation, Verify&& verify) {
    RowResult row;
    row.name = name;
    row.iters = iters;
    row.sample_ms.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        RunResult last;
        const auto started = std::chrono::steady_clock::now();
        try {
            last = operation(iters);
        } catch (const std::exception& error) {
            row.failures += 1;
            if (row.first_error.empty()) row.first_error = name + ": " + error.what();
            break;
        }
        const double total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        row.sample_ms.push_back(total_ms);
        row.completions += iters;
        Check check = verify(last);
        ++row.checked_runs;
        if (check.ok) {
            try {
                check = verify(operation(1));
                ++row.checked_runs;
            } catch (const std::exception& error) {
                check = {false, name + ": the verification run threw: " + error.what()};
            }
        }
        if (!check.ok) {
            row.failures += 1;
            if (row.first_error.empty()) row.first_error = check.detail;
            break;
        }
    }
    if (row.failures == 0 && !row.sample_ms.empty()) {
        std::vector<double> sorted = row.sample_ms;
        std::sort(sorted.begin(), sorted.end());
        row.total_ms = sorted[sorted.size() / 2];
        row.per_iter_us = row.total_ms * 1000.0 / static_cast<double>(iters);
    } else if (row.sample_ms.empty() && row.failures == 0) {
        row.failures = 1;
        row.first_error = name + ": no sample was taken";
    }
    if (row.failures != 0 && g_first_error.empty()) g_first_error = row.first_error;
    return row;
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

static RunResult run_graph(GraphEngine* engine, int iters) {
    RunConfig cfg;  // no thread_id → checkpoint coordinator disabled
    RunResult last;
    for (int i = 0; i < iters; ++i) last = engine->run(cfg);
    return last;
}

static RunResult run_stream(GraphEngine* engine, int iters, StreamMode mode) {
    RunConfig cfg;
    cfg.stream_mode = mode;
    const GraphStreamCallback discard = [](const GraphEvent&) {};
    RunResult last;
    for (int i = 0; i < iters; ++i) last = engine->run_stream(cfg, discard);
    return last;
}


static std::string configure_workers(GraphEngine& engine,
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

// /proc/self/status readings in kB (0 where the file or key is missing).
static long proc_status_kb(const char* key) {
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(status, line)) {
        if (line.rfind(prefix, 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str() + prefix.size(), " %ld", &kb);
            return kb;
        }
    }
    return 0;
}

struct Options {
    std::vector<std::string> rows;
    bool record = false;
    bool all_workers = false;

    bool wants(const std::string& row) const {
        return rows.empty() || std::find(rows.begin(), rows.end(), row) != rows.end();
    }
};

static std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const auto comma = text.find(',');
        out.emplace_back(text.substr(0, comma));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

static json build_facts() {
    json build = json::object();
    build["compiler"] = std::string(__VERSION__);
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
    build["build_type"] = std::string(NEOGRAPH_BENCH_BUILD_TYPE);
#else
    build["build_type"] = std::string("unspecified");
#endif
#if defined(NDEBUG)
    build["ndebug"] = true;
#else
    build["ndebug"] = false;
#endif
    // What the translation unit of this benchmark was compiled with, read from the compiler's own macros, so
    // the record states the real hardening state and not what the build option was meant to do.
#if defined(_GLIBCXX_ASSERTIONS)
    build["glibcxx_assertions"] = true;
#else
    build["glibcxx_assertions"] = false;
#endif
#if defined(_FORTIFY_SOURCE)
    build["fortify_source"] = static_cast<int>(_FORTIFY_SOURCE + 0);
#else
    build["fortify_source"] = 0;
#endif
#if defined(__SSP_ALL__)
    build["stack_protector"] = std::string("all");
#elif defined(__SSP_STRONG__)
    build["stack_protector"] = std::string("strong");
#elif defined(__SSP__)
    build["stack_protector"] = std::string("basic");
#else
    build["stack_protector"] = std::string("none");
#endif
#if defined(__PIE__)
    build["pie"] = true;
#else
    build["pie"] = false;
#endif
#if defined(__x86_64__)
    build["arch"] = std::string("x86_64");
#elif defined(__aarch64__)
    build["arch"] = std::string("aarch64");
#else
    build["arch"] = std::string("unknown");
#endif
    build["hardware_concurrency"] = static_cast<int>(std::max(std::thread::hardware_concurrency(), 1u));
    return build;
}

static json row_json(const RowResult& row, const std::string& workers) {
    json out = json::object();
    out["row"] = row.name;
    out["iters"] = row.iters;
    out["workers"] = workers;
    json samples = json::array();
    for (const double ms : row.sample_ms) samples.push_back(json(ms));
    out["sample_ms"] = samples;
    out["median_ms"] = row.total_ms;
    out["per_iter_us"] = row.per_iter_us;
    out["completions"] = static_cast<long long>(row.completions);
    out["failures"] = static_cast<long long>(row.failures);
    out["checked_runs"] = static_cast<long long>(row.checked_runs);
    out["status"] = std::string(row.failures == 0 ? "ok" : "error");
    out["first_error"] = row.first_error;
    return out;
}

int main(int argc, char** argv) {
    try {
        Options options;
        std::vector<char*> positional;
        positional.push_back(argv[0]);
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg.rfind("--rows=", 0) == 0) {
                options.rows = split_commas(arg.substr(7));
                for (const auto& row : options.rows) {
                    if (row != "seq" && row != "seq_sync" && row != "par" &&
                        row != "seq_stream_idle" && row != "seq_events")
                        throw std::invalid_argument("unknown row: " + row);
                }
            } else if (arg == "--record") {
                options.record = true;
            } else if (arg == "--all-workers") {
                options.all_workers = true;
            } else if (arg.rfind("--", 0) == 0) {
                throw std::invalid_argument("unknown option: " + std::string(arg));
            } else {
                positional.push_back(argv[i]);
            }
        }
        const int pargc = static_cast<int>(positional.size());
        char** pargv = positional.data();
        if (pargc > 6) {
            throw std::invalid_argument(
                "usage: bench_neograph [seq_iters] [par_iters] [par_workers] "
                "[warmup] [samples] [--rows=...] [--record] [--all-workers]");
        }
        const int seq_iters = pargc > 1
            ? static_cast<int>(parse_positive(pargv[1], "seq_iters")) : 10000;
        const int par_iters = pargc > 2
            ? static_cast<int>(parse_positive(pargv[2], "par_iters")) : 5000;
        const std::string par_worker_mode = pargc > 3 ? pargv[3] : "1";
        const int warmup = pargc > 4
            ? static_cast<int>(parse_positive(pargv[4], "warmup")) : 10;
        const int samples = pargc > 5
            ? static_cast<int>(parse_positive(pargv[5], "samples")) : 5;

        const long rss_kb_start = proc_status_kb("VmRSS");
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

        json rows_json = json::array();
        long rss_kb_after_compile = 0;
        const auto seq_workers_label = options.all_workers ? par_worker_mode : std::string("1");
        auto emit = [&](const RowResult& row, const std::string& workers) {
            if (row.failures == 0)
                std::cout << row.name << "\t" << row.iters << "\t" << row.total_ms
                          << "\t" << row.per_iter_us << "\n";
            rows_json.push_back(row_json(row, workers));
        };
        auto compile_seq = [&](const char* name, const char* node_type) {
            auto engine = GraphEngine::compile(seq_graph(name, node_type), NodeContext{});
            if (options.all_workers) configure_workers(*engine, par_worker_mode);
            return engine;
        };

        // --- Sequential ---
        std::unique_ptr<GraphEngine> seq_engine;
        if (options.wants("seq") || options.wants("seq_stream_idle") || options.wants("seq_events")) {
            seq_engine = compile_seq("seq", "inc");
            if (rss_kb_after_compile == 0) rss_kb_after_compile = proc_status_kb("VmRSS");
        }
        if (options.wants("seq")) {
            for (int i = 0; i < warmup; ++i) (void)run_graph(seq_engine.get(), 1);
            emit(bench("seq", seq_iters, samples,
                       [&](int n) { return run_graph(seq_engine.get(), n); }, check_seq),
                 seq_workers_label);
        }

        if (options.wants("seq_sync")) {
            auto sync_engine = compile_seq("seq_sync", "sync_inc");
            for (int i = 0; i < warmup; ++i) (void)run_graph(sync_engine.get(), 1);
            emit(bench("seq_sync", seq_iters, samples,
                       [&](int n) { return run_graph(sync_engine.get(), n); }, check_seq),
                 seq_workers_label);
        }

        // --- Parallel ---
        if (options.wants("par")) {
            auto par_engine = GraphEngine::compile(par_graph(), NodeContext{});
            const auto par_workers = configure_workers(*par_engine, par_worker_mode);
            if (rss_kb_after_compile == 0) rss_kb_after_compile = proc_status_kb("VmRSS");
            std::cout << "config\tpar_workers\t" << par_workers << "\n";
            for (int i = 0; i < warmup; ++i) (void)run_graph(par_engine.get(), 1);
            emit(bench("par", par_iters, samples,
                       [&](int n) { return run_graph(par_engine.get(), n); }, check_par),
                 par_workers);
        } else {
            std::cout << "config\tpar_workers\t" << (par_worker_mode == "auto"
                ? "auto(" + std::to_string(std::max(std::thread::hardware_concurrency(), 1u)) + ")"
                : par_worker_mode) << "\n";
        }

        // A callback with TOKENS emits nothing for this graph. Comparing it
        // with EVENTS isolates lifecycle-event construction and callback
        // dispatch.
        if (options.wants("seq_stream_idle")) {
            for (int i = 0; i < warmup; ++i) (void)run_stream(seq_engine.get(), 1, StreamMode::TOKENS);
            emit(bench("seq_stream_idle", seq_iters, samples,
                       [&](int n) { return run_stream(seq_engine.get(), n, StreamMode::TOKENS); },
                       check_seq),
                 seq_workers_label);
        }

        if (options.wants("seq_events")) {
            for (int i = 0; i < warmup; ++i) (void)run_stream(seq_engine.get(), 1, StreamMode::EVENTS);
            emit(bench("seq_events", seq_iters, samples,
                       [&](int n) { return run_stream(seq_engine.get(), n, StreamMode::EVENTS); },
                       check_seq),
                 seq_workers_label);
        }

        if (options.record) {
            json record = json::object();
            record["schema"] = 1;
            record["tool"] = std::string("bench_neograph");
            record["rows"] = rows_json;
            json process = json::object();
            process["rss_kb_start"] = static_cast<long long>(rss_kb_start);
            process["rss_kb_after_compile"] = static_cast<long long>(rss_kb_after_compile);
            process["hwm_kb_end"] = static_cast<long long>(proc_status_kb("VmHWM"));
            record["process"] = process;
            record["build"] = build_facts();
            record["par_worker_mode"] = par_worker_mode;
            std::cout << "record\t" << record.dump() << "\n";
        }
        if (!g_first_error.empty()) {
            std::cerr << "error: " << g_first_error << '\n';
            return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
