// Concurrent stress test: many engine.run_async() calls overlap on
// a single io_context. The shared engine + scheduler + checkpoint
// store + thread-pool worker count machinery must be safe under
// genuine concurrent dispatch — not just "ran 10000 sequentially
// without leaking".
//
// What this catches:
//   - Data races on shared engine state (worker pool, checkpoint
//     store, node factory) — surfaces under TSan; this test under
//     ASan still catches use-after-free if the engine were to
//     destruct shared resources mid-run.
//   - Heap corruption from parallel Send fan-out (executor's
//     parallel_group + pending_writes machinery).
//   - Per-run state leaks — RSS check between iter 10 and iter 200
//     of 200 concurrent runs.

#include <gtest/gtest.h>

#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Read RSS in KB from /proc/self/status (Linux only — skip on macOS CI).
long read_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            std::istringstream is(line.substr(6));
            long kb; std::string unit;
            is >> kb >> unit;
            return kb;
        }
    }
    return -1;
}

// Worker node — reads `i` from Send payload, produces a single-element
// "results" list. Runs ~1 ms (asio::steady_timer) so the executor
// genuinely interleaves siblings.
class Worker : public GraphNode {
public:
    std::string get_name() const override { return "worker"; }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int i = in.state.get("i").get<int>();
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(1));
        co_await t.async_wait(asio::use_awaitable);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"results", json::array({i * i})});
        co_return out;
    }
};

// Planner — emits 3 Sends. Sync execute_full only (since the engine
// fix in commit 6bd9632, the async path picks this up via the
// execute_full_async default that routes to sync execute_full).
class Planner : public GraphNode {
public:
    std::string get_name() const override { return "planner"; }
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        for (int i = 0; i < 3; ++i) {
            out.sends.emplace_back(Send{"worker", json{{"i", i}}});
        }
        co_return out;
    }
};

std::shared_ptr<GraphEngine> make_stress_engine() {
    auto& factory = NodeFactory::instance();
    factory.register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<Planner>();
        });
    factory.register_type("worker",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<Worker>();
        });

    json def = {
        {"name", "stress"},
        {"channels", {
            {"i",       {{"reducer", "overwrite"}}},
            {"results", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"planner", {{"type", "planner"}}},
            {"worker",  {{"type", "worker"}}}
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "planner"}},
            // Planner's outgoing edge is __end__ — workers reach it
            // independently via Send. The barrier shape isn't needed
            // here because we don't aggregate worker outputs through
            // a summarizer; we read the appended results channel
            // directly from RunResult.output.
            json{{"from", "planner"}, {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto unique = GraphEngine::compile(def, ctx);
    auto engine = std::shared_ptr<GraphEngine>(unique.release());
    // Match worker count to fan-out width.
    engine->set_worker_count(4);
    return engine;
}

} // namespace

// ---------------------------------------------------------------- //
// Concurrent run_async fan-out — 200 simultaneous engine runs on
// one io_context, each doing a 3-way Send fan-out.
// ---------------------------------------------------------------- //
TEST(ConcurrentStress, TwoHundredOverlappingRunsAllSucceed) {
    constexpr int N = 200;
    auto engine = make_stress_engine();

    asio::io_context io;
    std::vector<std::future<RunResult>> futures;
    futures.reserve(N);

    for (int k = 0; k < N; ++k) {
        RunConfig cfg;
        cfg.thread_id = "t" + std::to_string(k);
        cfg.input["i"] = k;
        // engine->run_async() returns asio::awaitable<RunResult>; we
        // co_spawn it onto the io_context with use_future for the
        // sync wait at the end.
        futures.emplace_back(asio::co_spawn(
            io.get_executor(), engine->run_async(std::move(cfg)),
            asio::use_future));
    }

    // Drive the io_context until all futures resolve.
    std::thread driver([&] { io.run(); });

    for (auto& f : futures) {
        auto r = f.get();
        EXPECT_FALSE(r.interrupted)
            << "run completed without interrupt";
        // Each run produced 3 worker outputs (i² for i in {0,1,2}).
        auto results = r.output.value("channels", json::object())
                               .value("results", json::object())
                               .value("value", json::array());
        EXPECT_EQ(results.size(), 3u);
        // Order is non-deterministic under parallel_group; check the
        // set instead.
        std::set<int> got;
        for (const auto& v : results) got.insert(v.get<int>());
        std::set<int> expected{0, 1, 4};
        EXPECT_EQ(got, expected);
    }

    driver.join();
}

// ---------------------------------------------------------------- //
// RSS doesn't grow under sustained concurrent load.
// ---------------------------------------------------------------- //
//
// Skipped under AddressSanitizer because ASan's shadow memory + redzone
// metadata grow with every allocation it observes, so RSS measurements
// are dominated by the sanitizer's bookkeeping rather than NeoGraph's
// allocation behaviour. ASan/LSan have their own per-allocation leak
// detection (run via the sanitizer-test CI gate); this test exists for
// the non-sanitizer Debug/Release runs.
TEST(ConcurrentStress, RssBoundedOverHundredsOfConcurrentRuns) {
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
    GTEST_SKIP() << "RSS check unreliable under ASan (shadow-memory growth swamps signal); "
                    "LSan exit-time check covers the leak path independently";
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
    GTEST_SKIP() << "RSS check unreliable under ASan (GCC __SANITIZE_ADDRESS__)";
#endif

    if (read_rss_kb() < 0) {
        GTEST_SKIP() << "/proc/self/status not available (non-Linux)";
    }

    constexpr int N = 200;
    auto engine = make_stress_engine();

    long rss_at_warmup = -1;
    long rss_at_end    = -1;

    auto run_burst = [&](int burst_id) {
        asio::io_context io;
        std::vector<std::future<RunResult>> futures;
        for (int k = 0; k < N; ++k) {
            RunConfig cfg;
            cfg.thread_id = "burst" + std::to_string(burst_id) + "-t" + std::to_string(k);
            cfg.input["i"] = k;
            futures.emplace_back(asio::co_spawn(
                io.get_executor(), engine->run_async(std::move(cfg)),
                asio::use_future));
        }
        std::thread driver([&] { io.run(); });
        for (auto& f : futures) f.get();
        driver.join();
    };

    // Warmup pass — fills allocator pools, JIT-ed paths, etc.
    run_burst(0);
    rss_at_warmup = read_rss_kb();

    // Five more bursts of 200 — 1000 more concurrent runs total.
    for (int b = 1; b <= 5; ++b) run_burst(b);
    rss_at_end = read_rss_kb();

    long growth = rss_at_end - rss_at_warmup;
    std::printf("[stress] RSS: warmup=%ldkB  end=%ldkB  Δ=%ldkB (over 1000 concurrent runs)\n",
        rss_at_warmup, rss_at_end, growth);

    // Allocator returns blocks to its pool; we tolerate up to 10 MB
    // of fragmentation noise but a real leak would grow much more
    // than that across 1000 runs.
    EXPECT_LT(growth, 10L * 1024)
        << "RSS grew " << growth << " kB over 5 bursts of 200 concurrent runs — "
        << "suggests a per-run leak. warmup=" << rss_at_warmup
        << " end=" << rss_at_end;
}
