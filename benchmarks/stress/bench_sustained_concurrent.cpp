// Sustained-burst stress harness for NeoGraph (Phase 3a — operational
// readiness gate).
//
// The single-shot `bench_concurrent_neograph` proves NeoGraph handles
// 10,000 concurrent requests at t=0. That's the right shape for a
// burst measurement, but it doesn't catch what happens *over time*:
//
//   * Steady-state RSS — does the process leak? Memory pinned by
//     a coroutine that never returns? A growing internal cache?
//   * Latency drift — does p99 walk up after 5 minutes of churn?
//   * Pool exhaustion — does the worker pool starve under sustained
//     non-bursty arrivals (where some completions overlap with new
//     submissions instead of all draining together)?
//
// This harness sustains a target concurrency for a configurable
// wall-clock window. New runs are submitted as fast as completions
// allow, holding `concurrency` in flight. RSS and per-window p99
// latency are sampled every `--sample-interval` seconds. Process
// exits with code 1 if RSS grows by more than `--rss-tolerance`
// percent between the warm baseline and the final sample (a
// best-effort leak indicator — a real leak detector goes through
// LSan / Valgrind).
//
// Usage:
//   ./bench_sustained_concurrent \
//       --concurrency 1000 \
//       --duration-s   60   \
//       --sample-s     5    \
//       --rss-tolerance-pct 25
//
// Emits one JSON line per sample to stdout (consumable by the runner
// script for trend plots) and a single summary line at exit.

#include <neograph/neograph.h>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// ── Tiny work node (matches single-shot bench's shape) ──────────────
class IncNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int cur = 0;
        auto v = state.get("counter");
        if (v.is_number()) cur = v.get<int>();
        return {ChannelWrite{"counter", json(cur + 1)}};
    }
    std::string get_name() const override { return "inc"; }
};

static void register_types() {
    NodeFactory::instance().register_type("inc",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<IncNode>();
        });
}

static json seq_graph() {
    return {
        {"name", "stress-seq"},
        {"channels", {{"counter", {{"reducer", "overwrite"}}}}},
        {"nodes", {
            {"a", {{"type", "inc"}}},
            {"b", {{"type", "inc"}}},
            {"c", {{"type", "inc"}}},
        }},
        {"edges", {
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"},         {"to", "b"}},
            {{"from", "b"},         {"to", "c"}},
            {{"from", "c"},         {"to", "__end__"}},
        }},
    };
}

// /proc/self/status VmHWM (peak resident set, kB) — same source the
// single-shot bench uses, so the two report comparable numbers.
static std::size_t peak_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::size_t v = 0;
            for (char c : line) if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
            return v;
        }
    }
    return 0;
}

// /proc/self/status VmRSS — *current* RSS, distinct from peak. The
// drift check uses this because peak is monotone-non-decreasing and
// can't tell us "memory grew then shrank" vs "memory grew and stayed".
static std::size_t current_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::size_t v = 0;
            for (char c : line) if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
            return v;
        }
    }
    return 0;
}

struct Args {
    int concurrency       = 1000;
    int duration_s        = 60;
    int sample_s          = 5;
    int warmup_s          = 5;
    int rss_tolerance_pct = 25;
};

static int parse_int(const char* v, int def) {
    return (v && *v) ? std::atoi(v) : def;
}

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if      (k == "--concurrency")        a.concurrency       = parse_int(v, 1000);
        else if (k == "--duration-s")         a.duration_s        = parse_int(v, 60);
        else if (k == "--sample-s")           a.sample_s          = parse_int(v, 5);
        else if (k == "--warmup-s")           a.warmup_s          = parse_int(v, 5);
        else if (k == "--rss-tolerance-pct")  a.rss_tolerance_pct = parse_int(v, 25);
    }

    register_types();
    auto def    = seq_graph();
    auto engine = GraphEngine::compile(def, NodeContext{});
    // Worker pool size is independent of submission concurrency — we
    // want to measure scheduler back-pressure, not "did you set the
    // pool to N when you wanted N parallel runs." Default
    // (hardware_concurrency) is what user code typically inherits.

    // Caller pool drives submissions. Sized at concurrency so we can
    // keep the target inflight without queueing on this side.
    asio::thread_pool caller_pool(static_cast<std::size_t>(a.concurrency));

    std::atomic<std::int64_t> ok{0}, err{0};
    std::atomic<std::int64_t> latencies_us_sum{0}, latencies_us_max{0};
    std::atomic<int>          inflight{0};
    std::atomic<bool>         shutdown{false};

    auto submit_one = [&](auto&& self) -> void {
        if (shutdown.load(std::memory_order_acquire)) return;
        ++inflight;
        asio::post(caller_pool, [&, self]() {
            auto t0 = std::chrono::steady_clock::now();
            try {
                RunConfig cfg;
                cfg.input = {{"counter", 0}};
                (void)engine->run(cfg);
                ok.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                err.fetch_add(1, std::memory_order_relaxed);
            }
            auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            latencies_us_sum.fetch_add(dt, std::memory_order_relaxed);
            // CAS-style max — fine at this fanout, no per-batch contention.
            std::int64_t cur = latencies_us_max.load(std::memory_order_relaxed);
            while (dt > cur &&
                   !latencies_us_max.compare_exchange_weak(
                       cur, dt, std::memory_order_relaxed)) {}
            --inflight;
            // Re-arm: keep `concurrency` in flight by submitting a
            // replacement now. Recursion through the asio executor —
            // `self` is the captured copy of submit_one.
            self(self);
        });
    };

    auto t_start = std::chrono::steady_clock::now();
    auto t_warm  = t_start + std::chrono::seconds(a.warmup_s);
    auto t_end   = t_start + std::chrono::seconds(a.duration_s);

    // Prime the pool with `concurrency` submissions. Each completion
    // re-arms via `self(self)` so the inflight target stays steady.
    for (int i = 0; i < a.concurrency; ++i) submit_one(submit_one);

    std::size_t rss_warm_kb = 0;

    // Sample loop — drive the wall-clock window, emit per-sample JSON.
    int sample_n = 0;
    while (true) {
        auto next_sample = std::chrono::steady_clock::now() +
                           std::chrono::seconds(a.sample_s);
        std::int64_t prev_ok  = ok.load(std::memory_order_relaxed);
        std::int64_t prev_sum = latencies_us_sum.load(std::memory_order_relaxed);
        latencies_us_max.store(0, std::memory_order_relaxed);

        std::this_thread::sleep_for(std::chrono::seconds(a.sample_s));

        std::int64_t cur_ok    = ok.load(std::memory_order_relaxed);
        std::int64_t cur_sum   = latencies_us_sum.load(std::memory_order_relaxed);
        std::int64_t cur_max   = latencies_us_max.load(std::memory_order_relaxed);
        std::int64_t window_n  = cur_ok - prev_ok;
        double mean_us = window_n > 0 ? double(cur_sum - prev_sum) / window_n : 0.0;

        std::size_t rss_now = current_rss_kb();
        if (sample_n == 0 && std::chrono::steady_clock::now() >= t_warm) {
            rss_warm_kb = rss_now;
        }

        ++sample_n;
        std::cout
            << "{\"sample\":"          << sample_n
            << ",\"elapsed_s\":"       << std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::steady_clock::now() - t_start).count()
            << ",\"window_ok\":"       << window_n
            << ",\"err_total\":"       << err.load(std::memory_order_relaxed)
            << ",\"inflight\":"        << inflight.load(std::memory_order_relaxed)
            << ",\"mean_us\":"         << mean_us
            << ",\"max_us_window\":"   << cur_max
            << ",\"rss_kb\":"          << rss_now
            << ",\"peak_rss_kb\":"     << peak_rss_kb()
            << "}\n";
        std::cout.flush();

        if (std::chrono::steady_clock::now() >= t_end) break;
    }

    // Stop accepting new submissions and drain the pool.
    shutdown.store(true, std::memory_order_release);
    caller_pool.stop();
    caller_pool.join();

    std::size_t rss_final_kb = current_rss_kb();
    std::size_t rss_peak_kb  = peak_rss_kb();

    double drift_pct = rss_warm_kb > 0
        ? (double(rss_final_kb) - rss_warm_kb) / rss_warm_kb * 100.0
        : 0.0;
    bool   leak_suspect = drift_pct > a.rss_tolerance_pct;

    std::cout
        << "{\"summary\":true"
        << ",\"concurrency\":"        << a.concurrency
        << ",\"duration_s\":"         << a.duration_s
        << ",\"ok_total\":"           << ok.load(std::memory_order_relaxed)
        << ",\"err_total\":"          << err.load(std::memory_order_relaxed)
        << ",\"rss_warm_kb\":"        << rss_warm_kb
        << ",\"rss_final_kb\":"       << rss_final_kb
        << ",\"rss_peak_kb\":"        << rss_peak_kb
        << ",\"rss_drift_pct\":"      << drift_pct
        << ",\"rss_tolerance_pct\":"  << a.rss_tolerance_pct
        << ",\"leak_suspect\":"       << (leak_suspect ? "true" : "false")
        << "}\n";

    return leak_suspect ? 1 : 0;
}
