// Load benchmark for CheckpointStore implementations.
//
// Compares InMemoryCheckpointStore vs SqliteCheckpointStore vs
// PostgresCheckpointStore under the same synthetic workload:
//
//   N parallel threads, each running M iterations.
//   Each iteration saves one Checkpoint with K channels of ~payload_bytes
//   each. Channels rotate so dedup is realistic (most steady, one changes).
//
// Reports per-backend:
//   - Total wall time
//   - Throughput (saves/sec)
//   - p50, p95, p99 save latency
//   - Final blob_count (proxy for storage efficiency)
//
// Usage:
//   ./bench_checkpoint_store [--threads N] [--iters M] [--channels K]
//                            [--payload BYTES] [--backends LIST]
//                            [--pg-url URL] [--sqlite-path PATH]
//
// Default: 8 threads × 200 iters × 6 channels × 256 byte payload.
// Backends default to "memory,sqlite,postgres" if PG URL is set, else
// "memory,sqlite".

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/sqlite_checkpoint.h>
#ifdef NEOGRAPH_HAVE_POSTGRES
#include <neograph/graph/postgres_checkpoint.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;
using clk = std::chrono::steady_clock;

namespace {

struct Config {
    int  threads          = 8;
    int  iters_per_thread = 200;
    int  channels         = 6;
    int  payload_bytes    = 256;
    std::string backends  = "auto";      // resolved to memory,sqlite[,postgres]
    std::string pg_url;
    std::string sqlite_path = "/tmp/neograph_bench.db";
};

Config parse_args(int argc, char** argv) {
    Config c;
    if (const char* env = std::getenv("NEOGRAPH_BENCH_PG_URL")) c.pg_url = env;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--threads")     c.threads          = std::stoi(next());
        else if (a == "--iters")       c.iters_per_thread = std::stoi(next());
        else if (a == "--channels")    c.channels         = std::stoi(next());
        else if (a == "--payload")     c.payload_bytes    = std::stoi(next());
        else if (a == "--backends")    c.backends         = next();
        else if (a == "--pg-url")      c.pg_url           = next();
        else if (a == "--sqlite-path") c.sqlite_path      = next();
    }
    if (c.backends == "auto") {
        c.backends = c.pg_url.empty() ? "memory,sqlite"
                                      : "memory,sqlite,postgres";
    }
    return c;
}

// Build a payload string of the given size. Deterministic per-version
// so dedup is honest — same version → same content.
std::string make_payload(int bytes, uint64_t version) {
    std::string s;
    s.reserve(bytes);
    while (static_cast<int>(s.size()) < bytes) {
        s += "v" + std::to_string(version) + "_";
    }
    s.resize(bytes);
    return s;
}

// One synthetic super-step cp. `step` rotates which channel is "fresh"
// — modelling the realistic case where most channels carry over and
// one or two get updated per super-step.
Checkpoint synth_cp(const std::string& thread_id, int step,
                    int channels, int payload_bytes) {
    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.step = step;
    cp.timestamp = step * 1000 + 1;
    cp.next_nodes = {"__end__"};
    cp.interrupt_phase = CheckpointPhase::Completed;
    cp.current_node = "node";

    json chs = json::object();
    for (int c = 0; c < channels; ++c) {
        // Channel `step % channels` advances every step; others stay at
        // version 1 forever — exactly the scenario dedup is designed for.
        uint64_t ver = (c == step % channels) ? (1 + step / channels) : 1;
        json entry = json::object();
        entry["value"] = make_payload(payload_bytes, ver);
        entry["version"] = ver;
        chs["ch" + std::to_string(c)] = entry;
    }
    cp.channel_values = json::object();
    cp.channel_values["channels"] = chs;
    cp.channel_values["global_version"] = static_cast<uint64_t>(step + 1);
    return cp;
}

struct Result {
    std::string backend;
    double wall_seconds   = 0.0;
    double throughput_qps = 0.0;
    double p50_us         = 0.0;
    double p95_us         = 0.0;
    double p99_us         = 0.0;
    size_t blob_count     = 0;
};

double percentile(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

Result run_one(const std::string& name, CheckpointStore& store,
               const Config& cfg) {
    std::vector<std::vector<double>> per_thread_lat(cfg.threads);
    auto wall_start = clk::now();

    std::vector<std::thread> ts;
    ts.reserve(cfg.threads);
    for (int t = 0; t < cfg.threads; ++t) {
        ts.emplace_back([&, t] {
            std::string tid = "bench-" + std::to_string(t);
            per_thread_lat[t].reserve(cfg.iters_per_thread);
            for (int i = 0; i < cfg.iters_per_thread; ++i) {
                auto cp = synth_cp(tid, i, cfg.channels, cfg.payload_bytes);
                auto t0 = clk::now();
                store.save(cp);
                auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                              clk::now() - t0).count();
                per_thread_lat[t].push_back(static_cast<double>(dt));
            }
        });
    }
    for (auto& th : ts) th.join();

    auto wall = std::chrono::duration<double>(clk::now() - wall_start).count();
    size_t total_ops = static_cast<size_t>(cfg.threads) * cfg.iters_per_thread;

    std::vector<double> all_lat;
    all_lat.reserve(total_ops);
    for (auto& v : per_thread_lat)
        all_lat.insert(all_lat.end(), v.begin(), v.end());

    Result r;
    r.backend        = name;
    r.wall_seconds   = wall;
    r.throughput_qps = total_ops / wall;
    r.p50_us         = percentile(all_lat, 0.50);
    r.p95_us         = percentile(all_lat, 0.95);
    r.p99_us         = percentile(all_lat, 0.99);

    // blob_count via the per-impl helper. Dispatching by name keeps the
    // benchmark dependency-light — no virtual blob_count on the base
    // CheckpointStore.
    if (auto* m = dynamic_cast<InMemoryCheckpointStore*>(&store)) {
        r.blob_count = m->blob_count();
    } else if (auto* s = dynamic_cast<SqliteCheckpointStore*>(&store)) {
        r.blob_count = s->blob_count();
    }
#ifdef NEOGRAPH_HAVE_POSTGRES
    else if (auto* p = dynamic_cast<PostgresCheckpointStore*>(&store)) {
        r.blob_count = p->blob_count();
    }
#endif
    return r;
}

// Wipe per-thread state so a second run isn't polluted by the first's
// blobs / dedup hits.
void clean_threads(CheckpointStore& store, const Config& cfg) {
    for (int t = 0; t < cfg.threads; ++t) {
        store.delete_thread("bench-" + std::to_string(t));
    }
}

void print_header(const Config& c) {
    std::cout << "# bench_checkpoint_store"
              << " threads=" << c.threads
              << " iters=" << c.iters_per_thread
              << " channels=" << c.channels
              << " payload=" << c.payload_bytes << "B\n";
    std::cout << std::left
              << std::setw(10) << "backend"
              << std::right
              << std::setw(10) << "wall(s)"
              << std::setw(12) << "ops/sec"
              << std::setw(12) << "p50(us)"
              << std::setw(12) << "p95(us)"
              << std::setw(12) << "p99(us)"
              << std::setw(10) << "blobs"
              << "\n";
}

void print_row(const Result& r) {
    std::cout << std::left << std::setw(10) << r.backend
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(10) << r.wall_seconds
              << std::setw(12) << std::setprecision(0) << r.throughput_qps
              << std::setw(12) << r.p50_us
              << std::setw(12) << r.p95_us
              << std::setw(12) << r.p99_us
              << std::setw(10) << r.blob_count
              << "\n";
}

bool wants(const std::string& list, const std::string& backend) {
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) if (item == backend) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    print_header(cfg);

    if (wants(cfg.backends, "memory")) {
        InMemoryCheckpointStore store;
        // Warmup once so the OS allocator doesn't bias the first backend's
        // numbers vs the others.
        auto warm = synth_cp("warm", 0, cfg.channels, cfg.payload_bytes);
        store.save(warm);
        store.delete_thread("warm");

        auto r = run_one("memory", store, cfg);
        print_row(r);
    }

    if (wants(cfg.backends, "sqlite")) {
        // Wipe stale file from a previous run.
        std::remove(cfg.sqlite_path.c_str());
        std::remove((cfg.sqlite_path + "-wal").c_str());
        std::remove((cfg.sqlite_path + "-shm").c_str());
        SqliteCheckpointStore store(cfg.sqlite_path);
        auto r = run_one("sqlite", store, cfg);
        print_row(r);
    }

#ifdef NEOGRAPH_HAVE_POSTGRES
    if (wants(cfg.backends, "postgres")) {
        if (cfg.pg_url.empty()) {
            std::cerr << "skipping postgres: --pg-url not set "
                         "(or NEOGRAPH_BENCH_PG_URL env)\n";
        } else {
            PostgresCheckpointStore store(cfg.pg_url);
            // Drop and recreate so the bench starts clean.
            store.drop_schema();
            auto r = run_one("postgres", store, cfg);
            print_row(r);
        }
    }
#endif

    return 0;
}
