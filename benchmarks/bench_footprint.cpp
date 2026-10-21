// Footprint measurements of the NeoGraph engine, for the cross-framework record (benchmarks/compare).
//
//   bench_footprint idle              nothing run: memory of the process right after start
//   bench_footprint cold              from process start to the first completed `seq` run
//   bench_footprint inflight <N> [seq|par]   memory with N runs parked in flight
//
// Each mode prints one line, `record<TAB>{json}`, and exits 0; any wrong result makes it exit non-zero with the
// first error on stderr.
//
// cold: the `seq` chain is the same 3-node counter chain bench_neograph times. The process registers the node
// type, compiles the graph and runs it once, checks the answer (counter == 3) and prints CLOCK_MONOTONIC
// nanoseconds at the moment the first run completed. A parent that read CLOCK_MONOTONIC just before it spawned
// the process takes the difference, so the figure covers the loader, static initialisation, the compile and the
// first run, and nothing after it. The same program is the "minimal app" whose stripped size the size table
// reports.
//
// inflight: the middle node of the `seq` chain, or the summarizer of `par` (after the five workers have
// written their results), parks on a timer until the harness has counted N parked runs, so N runs are alive
// at once on the one io_context thread (the engine's default worker_count of 1: every run executes on the
// caller's executor). The memory is read at that moment from /proc/self/status (VmRSS, VmHWM)
// and /proc/self/smaps_rollup (Pss), after one run has completed (the warmup) and the allocator has been asked
// to return what it can, so the first-use allocations are not charged to the parked runs. The runs are then
// released and every one of them must end with counter == 3.

#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <malloc.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

long long monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// A kB value from /proc/self/status or /proc/self/smaps_rollup; 0 where it is missing.
long proc_kb(const char* file, const char* key) {
    std::ifstream in(file);
    std::string line;
    const std::string prefix = std::string(key) + ":";
    while (std::getline(in, line)) {
        if (line.rfind(prefix, 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str() + prefix.size(), " %ld", &kb);
            return kb;
        }
    }
    return 0;
}

json memory_now() {
    json memory = json::object();
    memory["vm_rss_kb"] = static_cast<long long>(proc_kb("/proc/self/status", "VmRSS"));
    memory["vm_hwm_kb"] = static_cast<long long>(proc_kb("/proc/self/status", "VmHWM"));
    memory["pss_kb"] = static_cast<long long>(proc_kb("/proc/self/smaps_rollup", "Pss"));
    return memory;
}

// Drives the io_context until `done()` holds; false when `timeout_ms` passes first.
template <class Done>
bool pump(asio::io_context& io, Done done, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done()) {
        if (io.stopped()) io.restart();
        io.run_for(std::chrono::milliseconds(20));
        if (std::chrono::steady_clock::now() > deadline) return false;
    }
    return true;
}

struct Gate {
    int parked = 0;
    std::vector<asio::steady_timer*> timers;  // only touched on the io_context thread
    bool hold = true;                         // false: nodes pass straight through
};

// Parks the calling run on a timer until the harness opens the gate (cancels every parked timer).
asio::awaitable<void> park(Gate* gate) {
    if (gate == nullptr || !gate->hold) co_return;
    auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer(executor);
    timer.expires_after(std::chrono::hours(1));
    gate->timers.push_back(&timer);
    ++gate->parked;
    try {
        co_await timer.async_wait(asio::use_awaitable);
    } catch (const std::system_error&) {
        // cancelled by the harness: the gate is open
    }
    gate->timers.erase(std::remove(gate->timers.begin(), gate->timers.end(), &timer), gate->timers.end());
}

class IncNode : public GraphNode {
public:
    IncNode(std::string name, Gate* gate) : name_(std::move(name)), gate_(gate) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        if (name_ == "b") co_await park(gate_);  // the middle node of the chain
        int cur = 0;
        auto v = in.state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"counter", json(cur + 1)});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    Gate* gate_;
};

class WorkerNode : public GraphNode {
public:
    WorkerNode(std::string name, int idx) : name_(std::move(name)), idx_(idx) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({idx_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    int idx_;
};

class SumNode : public GraphNode {
public:
    explicit SumNode(Gate* gate) : gate_(gate) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        co_await park(gate_);  // parked after the fan-out has been collected
        auto r = in.state.get("results");
        int n = r.is_array() ? static_cast<int>(r.size()) : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"count", json(n)});
        co_return out;
    }
    std::string get_name() const override { return "summarizer"; }

private:
    Gate* gate_;
};

json seq_graph() {
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

json par_graph() {
    json nodes = json::object();
    json edges = json::array();
    for (int i = 1; i <= 5; ++i) {
        const std::string name = "w" + std::to_string(i);
        nodes[name] = json{{"type", "worker"}, {"idx", i}};
        edges.push_back(json{{"from", "__start__"}, {"to", name}});
        edges.push_back(json{{"from", name}, {"to", "summarizer"}});
    }
    nodes["summarizer"] = json{{"type", "summarizer"}};
    edges.push_back(json{{"from", "summarizer"}, {"to", "__end__"}});
    return {
        {"name", "par"},
        {"channels", {
            {"results", {{"reducer", "append"}}},
            {"count",   {{"reducer", "overwrite"}}}
        }},
        {"nodes", nodes},
        {"edges", edges}
    };
}

void register_nodes(Gate* gate) {
    NodeFactory::instance().register_type("inc",
        [gate](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncNode>(name, gate);
        });
    NodeFactory::instance().register_type("worker",
        [](const std::string& name, const json& cfg, const NodeContext&) {
            return std::make_unique<WorkerNode>(name, cfg.value("idx", 0));
        });
    NodeFactory::instance().register_type("summarizer",
        [gate](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SumNode>(gate);
        });
}

std::string check_seq(const RunResult& result) {
    const json out = result.output;
    if (!out.is_object() || !out.contains("channels")) return "the result has no channels";
    const json channels = out["channels"];
    if (!channels.is_object() || !channels.contains("counter")) return "the result has no counter channel";
    const json counter = channels["counter"]["value"];
    if (!counter.is_number() || counter.get<int>() != 3)
        return "expected counter == 3, got " + counter.dump();
    if (result.execution_trace != std::vector<std::string>{"a", "b", "c"})
        return "unexpected trace of " + std::to_string(result.execution_trace.size()) + " entries";
    return {};
}

std::string check_par(const RunResult& result) {
    const json out = result.output;
    if (!out.is_object() || !out.contains("channels")) return "the result has no channels";
    const json channels = out["channels"];
    if (!channels.is_object() || !channels.contains("results") || !channels.contains("count"))
        return "the result lacks the results or count channel";
    const json results = channels["results"]["value"];
    const json count = channels["count"]["value"];
    if (!count.is_number() || count.get<int>() != 5) return "expected count == 5, got " + count.dump();
    if (!results.is_array() || results.size() != 5) return "expected five results, got " + results.dump();
    std::vector<int> indices;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const json item = results[i];
        const json value = item.is_array() && item.size() == 1 ? item[static_cast<std::size_t>(0)] : item;
        if (!value.is_number()) return "a result is not a number: " + results.dump();
        indices.push_back(value.get<int>());
    }
    std::sort(indices.begin(), indices.end());
    if (indices != std::vector<int>{1, 2, 3, 4, 5}) return "expected the indices 1..5, got " + results.dump();
    if (result.execution_trace.size() != 6 || result.execution_trace.back() != "summarizer")
        return "unexpected trace of " + std::to_string(result.execution_trace.size()) + " entries";
    return {};
}

json build_facts() {
    json build = json::object();
    build["compiler"] = std::string(__VERSION__);
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
    build["build_type"] = std::string(NEOGRAPH_BENCH_BUILD_TYPE);
#else
    build["build_type"] = std::string("unspecified");
#endif
#if defined(_GLIBCXX_ASSERTIONS)
    build["glibcxx_assertions"] = true;
#else
    build["glibcxx_assertions"] = false;
#endif
    return build;
}

int mode_idle() {
    json record = json::object();
    record["schema"] = 1;
    record["tool"] = std::string("bench_footprint");
    record["mode"] = std::string("idle");
    record["at_start"] = memory_now();
    Gate gate;
    gate.hold = false;
    register_nodes(&gate);
    auto engine = GraphEngine::compile(seq_graph(), NodeContext{});
    record["after_compile"] = memory_now();
    record["build"] = build_facts();
    std::cout << "record\t" << record.dump() << "\n";
    return engine ? 0 : 1;
}

int mode_cold() {
    const long long t_main = monotonic_ns();
    Gate gate;
    gate.hold = false;
    register_nodes(&gate);
    auto engine = GraphEngine::compile(seq_graph(), NodeContext{});
    const long long t_compiled = monotonic_ns();
    RunConfig cfg;
    const RunResult result = engine->run(cfg);
    const long long t_done = monotonic_ns();
    const std::string problem = check_seq(result);
    if (!problem.empty()) {
        std::cerr << "error: " << problem << "\n";
        return 3;
    }
    json record = json::object();
    record["schema"] = 1;
    record["tool"] = std::string("bench_footprint");
    record["mode"] = std::string("cold");
    record["main_entered_ns"] = t_main;
    record["compiled_ns"] = t_compiled;
    record["first_run_done_ns"] = t_done;
    record["compile_us"] = static_cast<double>(t_compiled - t_main) / 1000.0;
    record["first_run_us"] = static_cast<double>(t_done - t_compiled) / 1000.0;
    record["build"] = build_facts();
    std::cout << "record\t" << record.dump() << "\n";
    return 0;
}

int mode_inflight(int n, const std::string& workload) {
    if (n < 1) throw std::invalid_argument("inflight needs N >= 1");
    Gate gate;
    register_nodes(&gate);
    const bool par = workload == "par";
    if (!par && workload != "seq") throw std::invalid_argument("workload must be seq or par");
    auto engine = GraphEngine::compile(par ? par_graph() : seq_graph(), NodeContext{});
    asio::io_context io;

    int finished = 0;
    int failed = 0;
    std::string first_error;
    auto launch = [&](int count) {
        for (int i = 0; i < count; ++i) {
            asio::co_spawn(io, engine->run_async(RunConfig{}),
                [&](std::exception_ptr error, RunResult result) {
                    ++finished;
                    std::string problem;
                    if (error) {
                        try {
                            std::rethrow_exception(error);
                        } catch (const std::exception& e) {
                            problem = e.what();
                        }
                    } else {
                        problem = par ? check_par(result) : check_seq(result);
                    }
                    if (!problem.empty()) {
                        ++failed;
                        if (first_error.empty()) first_error = problem;
                    }
                });
        }
    };

    // Warmup: one run end to end, with the gate open.
    gate.hold = false;
    launch(1);
    if (!pump(io, [&] { return finished == 1; }, 30000)) {
        std::cerr << "error: the warmup run did not finish\n";
        return 3;
    }
    gate.hold = true;
    finished = 0;
    gate.parked = 0;
    malloc_trim(0);

    json record = json::object();
    record["schema"] = 1;
    record["tool"] = std::string("bench_footprint");
    record["mode"] = std::string("inflight");
    record["n"] = n;
    record["workload"] = workload;
    record["before"] = memory_now();

    launch(n);
    if (!pump(io, [&] { return gate.parked >= n || failed != 0; }, 60000) && first_error.empty())
        first_error = "the runs did not all park in flight";
    record["parked"] = gate.parked;
    record["plateau"] = memory_now();

    for (asio::steady_timer* timer : std::vector<asio::steady_timer*>(gate.timers)) timer->cancel();
    if (!pump(io, [&] { return finished >= n; }, 60000) && first_error.empty())
        first_error = "the released runs did not all finish";
    record["finished"] = finished;
    record["failed"] = failed;
    record["after_release"] = memory_now();
    record["build"] = build_facts();
    std::cout << "record\t" << record.dump() << "\n";
    if (failed != 0 || gate.parked != n || finished != n) {
        std::cerr << "error: " << (first_error.empty() ? "not every run parked and finished" : first_error)
                  << "\n";
        return 3;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "idle") return mode_idle();
        if (mode == "cold") return mode_cold();
        if (mode == "inflight") {
            if (argc < 3) throw std::invalid_argument("usage: bench_footprint inflight <N> [seq|par]");
            return mode_inflight(std::atoi(argv[2]), argc > 3 ? argv[3] : "seq");
        }
        throw std::invalid_argument("usage: bench_footprint {idle|cold|inflight <N>}");
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}
