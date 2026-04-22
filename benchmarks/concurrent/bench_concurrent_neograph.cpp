// Concurrent-load bench for NeoGraph.
//
// Submits N simultaneous graph invocations to a shared
// asio::thread_pool, waits for all to finish, and reports:
//
//   total_wall_ms  — from first submit to last completion
//   p50/p95/p99_us — per-request latency distribution
//   peak_rss_kb    — self-reported via /proc/self/status VmHWM
//   ok_count       — successful invocations
//   err_count      — exceptions caught per worker
//
// Output is a single JSON line on stdout so the runner script can
// append it to a .jsonl results file without parsing.

#include <neograph/neograph.h>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── 3-node sequential counter chain (matched to bench_neograph.cpp) ──
class IncNode : public GraphNode {
public:
    explicit IncNode(std::string n) : n_(std::move(n)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int cur = 0;
        auto v = state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        return {ChannelWrite{"counter", json(cur + 1)}};
    }
    std::string get_name() const override { return n_; }
private:
    std::string n_;
};

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

// Peak RSS (kB) — Linux only. Returns 0 on other platforms.
static long read_vmhwm_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str(), "VmHWM: %ld kB", &kb);
            return kb;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    const int concurrency = (argc > 1) ? std::atoi(argv[1]) : 100;

    NodeFactory::instance().register_type("inc",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncNode>(name);
        });

    auto engine = GraphEngine::compile(seq_graph(), NodeContext{});

    const std::size_t num_workers =
        std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
    asio::thread_pool caller_pool(num_workers);

    // Warm-up: prime the engine state AND wake every caller-side
    // worker so first-burst scheduling latency doesn't bias N=10 P99.
    for (int i = 0; i < 10; ++i) (void)engine->run(RunConfig{});
    {
        const std::size_t W = std::max<std::size_t>(num_workers, 8);
        std::atomic<std::size_t> warm_remaining{W * 4};
        std::promise<void> warm_done;
        auto warm_fut = warm_done.get_future();
        for (std::size_t i = 0; i < W * 4; ++i) {
            asio::post(caller_pool, [&engine, &warm_remaining, &warm_done]() {
                (void)engine->run(RunConfig{});
                if (warm_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    warm_done.set_value();
            });
        }
        warm_fut.wait();
    }

    std::vector<long> latencies_us(concurrency);
    std::atomic<int> ok_count{0};
    std::atomic<int> err_count{0};
    std::atomic<int> done_count{0};
    std::promise<void> all_done;
    auto all_done_fut = all_done.get_future();

    auto t_start = std::chrono::steady_clock::now();
    for (int i = 0; i < concurrency; ++i) {
        asio::post(caller_pool,
            [i, concurrency, &engine, &latencies_us,
             &ok_count, &err_count, &done_count, &all_done]() {
            auto t0 = std::chrono::steady_clock::now();
            try {
                RunConfig cfg;  // no thread_id — coordinator is a no-op
                (void)engine->run(cfg);
                auto t1 = std::chrono::steady_clock::now();
                latencies_us[i] = std::chrono::duration_cast<std::chrono::microseconds>(
                    t1 - t0).count();
                ok_count.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                latencies_us[i] = -1;
                err_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (done_count.fetch_add(1, std::memory_order_acq_rel) + 1 == concurrency)
                all_done.set_value();
        });
    }
    all_done_fut.wait();
    auto t_end = std::chrono::steady_clock::now();
    long total_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    // Percentiles over successful latencies only.
    std::vector<long> ok;
    ok.reserve(ok_count.load());
    for (auto v : latencies_us) if (v >= 0) ok.push_back(v);
    std::sort(ok.begin(), ok.end());

    auto pct = [&](double p) -> long {
        if (ok.empty()) return 0;
        size_t idx = std::min(ok.size() - 1, static_cast<size_t>(ok.size() * p));
        return ok[idx];
    };

    long peak_rss_kb = read_vmhwm_kb();

    // Single-line JSON, stable field order.
    std::cout
        << "{\"engine\":\"neograph\",\"mode\":\"threadpool\",\"concurrency\":"
        << concurrency
        << ",\"total_wall_ms\":" << total_wall_ms
        << ",\"p50_us\":" << pct(0.50)
        << ",\"p95_us\":" << pct(0.95)
        << ",\"p99_us\":" << pct(0.99)
        << ",\"ok\":" << ok_count.load()
        << ",\"err\":" << err_count.load()
        << ",\"peak_rss_kb\":" << peak_rss_kb
        << "}" << std::endl;

    return err_count.load() > 0 ? 2 : 0;
}
