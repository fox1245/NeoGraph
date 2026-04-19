// bench_async_fanout — does asio coroutine-based concurrency actually
// scale better than thread-per-agent for the LLM-wait workload shape?
//
// Simulates the dominant shape of an agent workload: N "agents" each
// doing M "LLM calls", where each call blocks for `--latency-ms`
// (typically 100-1000ms for a real Anthropic call). CPU work per
// between-calls is negligible for this first cut — we're measuring the
// asymptote of "what does concurrency cost".
//
// Two modes, same workload, different implementation:
//
//   --mode sync   : N OS threads. Each thread calls ::sleep synchronously
//                   M times. Memory = N × thread_stack_size (~2-8 MB).
//                   Wall = M × latency-ms (perfect parallelism across
//                   threads, bounded by OS thread scheduling).
//
//   --mode async  : K worker threads running an asio io_context + N
//                   coroutines. Each coroutine awaits a steady_timer M
//                   times. Memory = K × thread_stack + N × coroutine_frame
//                   (~200 B each). Wall = same asymptote as sync (still
//                   bounded by timer firing), but memory is flat in N.
//
// Output:
//   mode=sync  concur=  1000 rounds=5  lat_ms=50  wall=0.253s  ops/s=197.8  rss_mb=2450.3
//   mode=async concur=  1000 rounds=5  lat_ms=50  wall=0.253s  ops/s=197.8  rss_mb=12.8
//
// The interesting axis is rss_mb — the whole point of async is that it
// doesn't blow up per-agent. Wall-clock should be close between the two
// once the thread scheduler catches up.

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Config {
    std::string mode      = "async";  // sync | async
    int         concur    = 1000;     // how many parallel "agents"
    int         rounds    = 5;        // LLM calls per agent
    int         latency_ms = 50;      // simulated per-call wait
    int         io_threads = 0;       // async only: 0 = std::thread::hardware_concurrency
};

Config parse(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if      (a == "--mode")       c.mode       = next();
        else if (a == "--concur")     c.concur     = std::stoi(next());
        else if (a == "--rounds")     c.rounds     = std::stoi(next());
        else if (a == "--latency-ms") c.latency_ms = std::stoi(next());
        else if (a == "--io-threads") c.io_threads = std::stoi(next());
    }
    if (c.io_threads == 0)
        c.io_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (c.io_threads == 0) c.io_threads = 4;
    return c;
}

// Peak RSS from /proc/self/status (VmHWM = high-water mark). Linux-only
// but that's every environment we care about for this PoC. Return -1
// on any parse/open failure so the bench still prints.
double peak_rss_mb() {
    std::ifstream f("/proc/self/status");
    if (!f) return -1.0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            auto pos = line.find_first_of("0123456789");
            if (pos == std::string::npos) return -1.0;
            long kb = std::stol(line.substr(pos));
            return kb / 1024.0;
        }
    }
    return -1.0;
}

// Sync worker: thread-per-agent. Each agent does `rounds` sleeps of
// `latency_ms`. Nothing async, nothing shared.
void run_sync(const Config& cfg) {
    std::vector<std::thread> ts;
    ts.reserve(cfg.concur);
    for (int i = 0; i < cfg.concur; ++i) {
        ts.emplace_back([&] {
            for (int r = 0; r < cfg.rounds; ++r) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg.latency_ms));
            }
        });
    }
    for (auto& t : ts) t.join();
}

// Async worker coroutine: `rounds` timer awaits on the current executor.
// Each instance holds only its frame — no OS thread per coroutine.
asio::awaitable<void> agent_coro(int rounds, int latency_ms) {
    auto ex = co_await asio::this_coro::executor;
    asio::steady_timer t{ex};
    for (int r = 0; r < rounds; ++r) {
        t.expires_after(std::chrono::milliseconds(latency_ms));
        co_await t.async_wait(asio::use_awaitable);
    }
}

void run_async(const Config& cfg) {
    asio::io_context io;
    // Keep the io_context alive even if all coroutines finish before
    // io.run() starts on worker threads. work_guard is released in the
    // main thread after join so run() actually returns.
    auto work = asio::make_work_guard(io);

    // Spawn all N coroutines up front. asio's scheduler picks them up
    // as the worker threads enter run().
    std::atomic<int> remaining{cfg.concur};
    for (int i = 0; i < cfg.concur; ++i) {
        asio::co_spawn(io,
            [rounds = cfg.rounds, latency = cfg.latency_ms]() -> asio::awaitable<void> {
                co_await agent_coro(rounds, latency);
            },
            [&remaining](std::exception_ptr e) {
                if (e) std::rethrow_exception(e);
                if (--remaining == 0) {
                    // last coroutine done — we could release work here,
                    // but we do it explicitly from main after join for
                    // clearer lifetime.
                }
            });
    }

    std::vector<std::thread> ts;
    ts.reserve(cfg.io_threads);
    for (int i = 0; i < cfg.io_threads; ++i) {
        ts.emplace_back([&io] { io.run(); });
    }

    // Watch completion, then release work so io.run() can return.
    while (remaining.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    work.reset();
    io.stop();
    for (auto& t : ts) t.join();
}

} // namespace

int main(int argc, char** argv) {
    Config cfg = parse(argc, argv);

    auto t0 = std::chrono::steady_clock::now();
    if      (cfg.mode == "sync")  run_sync(cfg);
    else if (cfg.mode == "async") run_async(cfg);
    else { std::cerr << "unknown mode: " << cfg.mode << "\n"; return 2; }
    auto wall = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    double total_ops = static_cast<double>(cfg.concur) * cfg.rounds;
    double rss = peak_rss_mb();

    std::printf("mode=%-5s concur=%6d rounds=%d lat_ms=%d  "
                "wall=%6.3fs  ops/s=%9.1f  rss_mb=%7.1f"
                "  io_threads=%d\n",
                cfg.mode.c_str(), cfg.concur, cfg.rounds, cfg.latency_ms,
                wall, total_ops / wall, rss,
                cfg.mode == "async" ? cfg.io_threads : cfg.concur);
    return 0;
}
