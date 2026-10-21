// Concurrent-load bench for NeoGraph (record schema 2).
//
// One closed burst: N runs of the 3-node counter chain are posted to a shared
// asio::thread_pool at one instant, and the bench waits until all of them
// finish. For every run it records
//
//   service_us   from the moment the run started executing on a pool thread
//                to its completion
//   response_us  from the ONE common submit instant (taken before the first
//                post) to its completion
//
// and reports, per kind, p50/p90/p95/p99/max by the nearest-rank method (p95
// from 100 and p99 from 1000 samples, otherwise null). total_wall_us is the
// common submit instant to the last completion, in microseconds.
//
// Every run result is checked after its service clock stopped: the chain must
// end with counter == 3 and a 3-entry execution trace, and must not be
// interrupted. A thrown exception or a wrong result is a failure. A cell with
// no success or with any failure has status "error", null percentiles, the
// first error text in first_error, and the exit status is 2.
//
// Memory: peak_rss_kb is VmHWM of this process (rss_scope "process").
//
// Output is a single JSON line on stdout so the runner script can append it to
// a .jsonl results file without parsing.
//
// Usage: bench_ng <concurrency>   (a positive integer)
//        BURST_INJECT_FAILURE=neograph (or all) makes the first node throw, to
//        show the failure path.

#include <neograph/neograph.h>

#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

constexpr int kExpected = 3;  // the chain a -> b -> c adds one per node, from 0
constexpr std::size_t kFirstErrorLimit = 400;
constexpr std::size_t kP95MinSamples = 100;
constexpr std::size_t kP99MinSamples = 1000;
constexpr long kMaxConcurrency = 100000000;

bool injected_failure() {
    const char* value = std::getenv("BURST_INJECT_FAILURE");
    if (value == nullptr) return false;
    const std::string text = value;
    return text == "neograph" || text == "all";
}

const bool g_inject = injected_failure();

// ── 3-node sequential counter chain (matched to bench_neograph.cpp) ──
class IncNode : public GraphNode {
public:
    explicit IncNode(std::string n) : n_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        if (g_inject) throw std::runtime_error("injected failure in neograph");
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

// A kB field of /proc/self/status (VmRSS, VmHWM); 0 where unavailable.
long vm_kb(const char* field) {
    std::ifstream f("/proc/self/status");
    std::string line;
    const std::string prefix = std::string(field) + ":";
    while (std::getline(f, line)) {
        if (line.rfind(prefix, 0) == 0) {
            long kb = 0;
            std::sscanf(line.c_str() + prefix.size(), " %ld kB", &kb);
            return kb;
        }
    }
    return 0;
}

// Empty string when the result is right, otherwise what is wrong.
std::string verify(const RunResult& result) {
    if (result.interrupted) return "run was interrupted";
    if (result.execution_trace.size() != 3) {
        return "expected 3 executed nodes, got " + std::to_string(result.execution_trace.size());
    }
    const auto& value = result.output.at("channels").at("counter").at("value");
    if (!value.is_number_integer() || value.get<int>() != kExpected) {
        return "counter: expected " + std::to_string(kExpected) + ", got " + value.dump();
    }
    return {};
}

std::string truncate(std::string text) {
    if (text.size() > kFirstErrorLimit) text = text.substr(0, kFirstErrorLimit - 3) + "...";
    return text;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", ch);
                    out += buffer;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

// Nearest rank: the sample at index ceil(p * n) - 1 of the sorted values.
double nearest_rank(const std::vector<double>& sorted, double p) {
    const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[rank == 0 ? 0 : rank - 1];
}

void append_number(std::ostringstream& out, const char* key, double value, bool present) {
    out << ",\"" << key << "\":";
    if (!present) {
        out << "null";
    } else {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.1f", value);
        out << buffer;
    }
}

void append_percentiles(std::ostringstream& out, const char* prefix,
                        std::vector<double> values_us, bool valid) {
    std::sort(values_us.begin(), values_us.end());
    const bool any = valid && !values_us.empty();
    const std::size_t n = values_us.size();
    const std::string p = prefix;
    append_number(out, (p + "_p50_us").c_str(), any ? nearest_rank(values_us, 0.50) : 0, any);
    append_number(out, (p + "_p90_us").c_str(), any ? nearest_rank(values_us, 0.90) : 0, any);
    append_number(out, (p + "_p95_us").c_str(), any ? nearest_rank(values_us, 0.95) : 0,
                  any && n >= kP95MinSamples);
    append_number(out, (p + "_p99_us").c_str(), any ? nearest_rank(values_us, 0.99) : 0,
                  any && n >= kP99MinSamples);
    append_number(out, (p + "_max_us").c_str(), any ? values_us.back() : 0, any);
}

}  // namespace

int main(int argc, char** argv) {
    long parsed = 0;
    if (argc == 2) {
        char* end = nullptr;
        parsed = std::strtol(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') parsed = 0;
    }
    if (argc != 2 || parsed < 1 || parsed > kMaxConcurrency) {
        std::cerr << "usage: bench_ng <concurrency>   (a positive integer up to "
                  << kMaxConcurrency << ")\n";
        return 1;
    }
    const int concurrency = static_cast<int>(parsed);
    const long rss_after_import_kb = vm_kb("VmRSS");

    NodeFactory::instance().register_type("inc",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncNode>(name);
        });

    auto engine = GraphEngine::compile(seq_graph(), NodeContext{});

    const std::size_t num_workers =
        std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
    asio::thread_pool caller_pool(num_workers);

    // Warm-up: prime the engine state AND wake every caller-side worker so the
    // first-burst scheduling latency is not part of the small-N rows. A warm-up
    // run that fails ends the cell.
    std::string warm_error;
    std::mutex warm_mutex;
    try {
        for (int i = 0; i < 10; ++i) {
            const auto error = verify(engine->run(RunConfig{}));
            if (!error.empty()) throw std::runtime_error(error);
        }
        const std::size_t W = std::max<std::size_t>(num_workers, 8);
        std::atomic<std::size_t> warm_remaining{W * 4};
        std::promise<void> warm_done;
        auto warm_fut = warm_done.get_future();
        for (std::size_t i = 0; i < W * 4; ++i) {
            asio::post(caller_pool, [&]() {
                try {
                    const auto error = verify(engine->run(RunConfig{}));
                    if (!error.empty()) throw std::runtime_error(error);
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(warm_mutex);
                    if (warm_error.empty()) warm_error = e.what();
                }
                if (warm_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                    warm_done.set_value();
            });
        }
        warm_fut.wait();
    } catch (const std::exception& e) {
        warm_error = e.what();
    }
    const long rss_before_burst_kb = vm_kb("VmRSS");

    using clock = std::chrono::steady_clock;
    std::vector<std::int64_t> service_ns(concurrency, -1);
    std::vector<std::int64_t> response_ns(concurrency, -1);
    std::atomic<int> ok_count{0};
    std::atomic<int> err_count{0};
    std::atomic<int> done_count{0};
    std::mutex error_mutex;
    std::string first_error;
    std::promise<void> all_done;
    auto all_done_fut = all_done.get_future();
    std::int64_t wall_ns = 0;

    if (warm_error.empty()) {
        const auto t_submit = clock::now();  // the one common submit instant
        for (int i = 0; i < concurrency; ++i) {
            asio::post(caller_pool, [&, i, t_submit]() {
                std::string failure;
                clock::time_point t0{}, t1{};
                try {
                    RunConfig cfg;  // no thread_id — the checkpoint coordinator is a no-op
                    t0 = clock::now();
                    auto result = engine->run(cfg);
                    t1 = clock::now();
                    failure = verify(result);  // after the service clock stopped
                } catch (const std::exception& e) {
                    failure = e.what();
                } catch (...) {
                    failure = "non-standard exception";
                }
                if (failure.empty()) {
                    service_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                    response_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t_submit).count();
                    ok_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    {
                        std::lock_guard<std::mutex> lock(error_mutex);
                        if (first_error.empty()) first_error = truncate(failure);
                    }
                    err_count.fetch_add(1, std::memory_order_relaxed);
                }
                if (done_count.fetch_add(1, std::memory_order_acq_rel) + 1 == concurrency)
                    all_done.set_value();
            });
        }
        all_done_fut.wait();
        wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t_submit).count();
    } else {
        first_error = truncate("warm-up failed: " + warm_error);
        err_count = concurrency;
    }

    std::vector<double> service_us, response_us;
    service_us.reserve(ok_count.load());
    response_us.reserve(ok_count.load());
    for (int i = 0; i < concurrency; ++i) {
        if (service_ns[i] >= 0) {
            service_us.push_back(static_cast<double>(service_ns[i]) / 1000.0);
            response_us.push_back(static_cast<double>(response_ns[i]) / 1000.0);
        }
    }

    const int ok = ok_count.load();
    const int err = err_count.load();
    const bool valid = ok > 0 && err == 0 && ok == concurrency;
    if (!valid && first_error.empty()) {
        first_error = std::to_string(ok) + " of " + std::to_string(concurrency) + " runs completed";
    }
    const long peak_rss_kb = vm_kb("VmHWM");
    const std::int64_t wall_us = wall_ns / 1000;

    std::ostringstream out;
    out << "{\"schema\":2,\"engine\":\"neograph\",\"mode\":\"threadpool\",\"concurrency\":" << concurrency
        << ",\"status\":\"" << (valid ? "ok" : "error") << "\""
        << ",\"ok\":" << ok << ",\"err\":" << err
        << ",\"first_error\":" << (valid ? std::string("null") : "\"" + json_escape(first_error) + "\"")
        << ",\"check\":\"counter == 3, 3 executed nodes and no interrupt on every run, checked after the service clock stopped\""
        << ",\"note\":\"" << num_workers << " caller-pool threads; engine at its default worker_count; no thread_id, so no checkpointing\""
        << ",\"clock\":\"CLOCK_MONOTONIC\",\"percentile_method\":\"nearest-rank\""
        << ",\"samples\":" << ok
        << ",\"total_wall_us\":" << wall_us;
    {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.3f", static_cast<double>(wall_us) / 1000.0);
        out << ",\"total_wall_ms\":" << buffer;
    }
    append_percentiles(out, "service", service_us, valid);
    append_percentiles(out, "response", response_us, valid);
    out << ",\"rss_scope\":\"process\""
        << ",\"peak_rss_kb\":" << peak_rss_kb
        << ",\"peak_rss_parent_kb\":" << peak_rss_kb
        << ",\"peak_rss_workers_kb\":[]"
        << ",\"rss_after_import_kb\":" << rss_after_import_kb
        << ",\"rss_before_burst_kb\":" << rss_before_burst_kb
        << ",\"worker_threads\":" << num_workers
        << "}";
    std::cout << out.str() << std::endl;

    if (!valid) {
        std::cerr << "neograph threadpool N=" << concurrency << ": " << err << " failed, " << ok
                  << " ok; first error: " << first_error << "\n";
        return 2;
    }
    return 0;
}
