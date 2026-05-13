// NeoGraph Example 44: Fixed-worker pool with backpressure (RequestQueue)
//
// The README's "Using the bundled RequestQueue" section advertises this
// shape for multi-tenant servers that want shed-load semantics instead
// of unbounded memory growth — but there's no runnable example. Most
// existing examples use `std::async`, `engine->run_async()` on an
// io_context, or `set_worker_count(N)` for in-run fan-out; none show
// the RequestQueue API.
//
// What this example shows:
//
//   * A graph that simulates per-session work via asio::steady_timer
//     (no LLM, no API key — the WorkNode itself is the same shape as
//     in example 27).
//   * 50 incoming "user sessions" submitted to a tiny RequestQueue
//     (4 workers, max-queue=8) — far more than the pool can hold.
//   * Backpressure rejecting sessions when the queue is saturated.
//   * Stats showing pending / active / completed / rejected.
//
// The interesting bit is the `auto [accepted, future] = pool.submit(...)`
// return — `accepted=false` means the queue refused the task and the
// future is invalid. The README docstring on submit() warned about
// this but no example exercised the rejection path.
//
// Usage: ./example_request_queue_backpressure

#include <neograph/neograph.h>
#include <neograph/util/request_queue.h>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

// Same WorkNode shape as example 27 — sleep on a timer to simulate
// per-session work without actually blocking the worker thread.
class WorkNode : public GraphNode {
    int delay_ms_;
public:
    explicit WorkNode(int d) : delay_ms_(d) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await t.async_wait(asio::use_awaitable);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"result", json("ok")});
        co_return out;
    }
    std::string get_name() const override { return "work"; }
};

static json work_graph() {
    return {
        {"name", "single_work"},
        {"channels", {{"result", {{"reducer", "overwrite"}}}}},
        {"nodes",    {{"work",   {{"type", "work"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "work"}},
            {{"from", "work"},      {"to", "__end__"}},
        })},
    };
}

int main() {
    NodeFactory::instance().register_type("work",
        [](const std::string&, const json& cfg, const NodeContext&) {
            return std::make_unique<WorkNode>(cfg.value("delay_ms", 80));
        });

    NodeContext ctx;
    auto engine = GraphEngine::compile(work_graph(), ctx,
                                       std::make_shared<InMemoryCheckpointStore>());

    // ── Tiny pool: only 4 workers, only 8 pending allowed ────────────
    //
    // With 50 incoming sessions, the queue will saturate quickly and
    // a chunk of submissions will be rejected — the load-shedding
    // behaviour the README advertises.
    util::RequestQueue pool(/*num_workers=*/4, /*max_queue_size=*/8);

    constexpr int N = 50;
    std::atomic<int> accepted_count{0};
    std::atomic<int> rejected_count{0};
    std::vector<std::future<void>> futs;
    futs.reserve(N);

    auto t0 = std::chrono::steady_clock::now();

    // Fire all 50 submissions back-to-back. Most will be queued; the
    // ones submitted after the queue saturates get rejected.
    for (int i = 0; i < N; ++i) {
        auto [ok, fut] = pool.submit([engine = engine.get(), i]() {
            RunConfig cfg;
            cfg.thread_id = "session-" + std::to_string(i);
            (void) engine->run(cfg);
        });
        if (ok) {
            accepted_count.fetch_add(1, std::memory_order_relaxed);
            futs.push_back(std::move(fut));
        } else {
            rejected_count.fetch_add(1, std::memory_order_relaxed);
            // Real server would return 503 here.
        }
    }

    // Periodically print stats so the user sees the queue draining.
    for (int tick = 0; tick < 30; ++tick) {
        auto s = pool.stats();
        std::cout << "[t+" << tick * 50 << "ms]"
                  << " pending=" << s.pending
                  << " active="  << s.active
                  << " completed=" << s.completed
                  << " rejected=" << s.rejected
                  << "\n";
        if (s.completed >= static_cast<size_t>(accepted_count.load())
            && s.active == 0 && s.pending == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Wait on all the accepted futures so we propagate any node
    // exceptions before final stats.
    for (auto& f : futs) {
        try { f.get(); }
        catch (const std::exception& e) {
            std::cerr << "task threw: " << e.what() << "\n";
        }
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    auto s = pool.stats();
    std::cout << "\n── Final ──────────────────────────────────────\n"
              << "submitted    : " << N << "\n"
              << "accepted     : " << accepted_count.load() << "\n"
              << "rejected (BP): " << rejected_count.load() << "\n"
              << "completed    : " << s.completed << "\n"
              << "elapsed      : " << elapsed_ms << " ms (4 workers × 80ms/job)\n"
              << "\n"
              << "If accepted_count > workers, the queue absorbed the burst.\n"
              << "rejected_count > 0 proves backpressure kicked in — those\n"
              << "would be 503s in a real HTTP server. Sub-`workers` accept\n"
              << "would mean the test machine drained the queue faster than\n"
              << "the loop could submit, in which case re-run with a larger\n"
              << "N or smaller delay_ms.\n";

    // Sanity: at least *some* should have been accepted and processed.
    if (accepted_count.load() < 4) {
        std::cerr << "FAIL: expected at least pool size in accepted.\n";
        return 1;
    }
    if (s.completed != static_cast<size_t>(accepted_count.load())) {
        std::cerr << "FAIL: accepted != completed (worker dropped a task?)\n";
        return 1;
    }
    return 0;
}
