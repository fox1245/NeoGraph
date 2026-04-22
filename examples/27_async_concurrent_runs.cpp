// NeoGraph Example 27: Async Concurrent Runs (Stage 3 — pattern preview)
//
// Drives multiple agent runs through a single shared asio::io_context
// using engine->run_async(). This is the wiring pattern users adopt
// when they want to host many concurrent agents without paying the
// thread-per-agent cost that the sync run() implies.
//
// **Current behaviour (3.0):** each engine->run_async() drives its own
// internal io_context to completion via run_sync_pool, so the three
// agents run *serially* on the caller's io_context. Wall time on the
// 50 ms-per-step workload below is ~150 ms (3 × 50 ms), not the ideal
// ~50 ms a fully coroutine-ified engine would achieve. Hosting
// thousands of independent runs is still the right shape — drive them
// with a shared asio::thread_pool (see CONCURRENT.md), or co_spawn
// each onto the io_context as shown here for the eventual upgrade.
//
// Scenario: three agents each run a tiny graph that simulates a 50 ms
// work step using asio::steady_timer (NOT std::this_thread::sleep_for).
// The work node yields cleanly to the timer, so once internal
// coroutine-ification lands the three runs will overlap on one
// thread; until then the example exists to lock in the *call shape*
// users should write today.
//
// Usage: ./example_async_concurrent_runs

#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace ng = neograph::graph;

// Async-native work node — issues asio::steady_timer.async_wait
// instead of std::this_thread::sleep_for. The sync execute() is
// inherited from GraphNode and routes through run_sync; the async
// path drives this directly, letting siblings on the same
// io_context interleave during the wait.
class WorkNode : public ng::GraphNode {
    std::string name_;
    int delay_ms_;
public:
    WorkNode(std::string name, int delay_ms)
        : name_(std::move(name)), delay_ms_(delay_ms) {}

    asio::awaitable<std::vector<ng::ChannelWrite>>
    execute_async(const ng::GraphState&) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await t.async_wait(asio::use_awaitable);
        co_return std::vector<ng::ChannelWrite>{
            ng::ChannelWrite{"result", neograph::json("done by " + name_)}};
    }
    std::string get_name() const override { return name_; }
};

static neograph::json one_step_graph(const std::string& node_name) {
    return {
        {"name", "single_work"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"result",   {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {node_name, {{"type", "work"}}},
        }},
        {"edges", neograph::json::array({
            {{"from", "__start__"}, {"to", node_name}},
            {{"from", node_name},   {"to", "__end__"}},
        })},
    };
}

int main() {
    auto& factory = ng::NodeFactory::instance();
    factory.register_type("work",
        [](const std::string& name, const neograph::json& cfg,
           const ng::NodeContext&) {
            return std::make_unique<WorkNode>(
                name, cfg.value("delay_ms", 50));
        });

    auto engine = ng::GraphEngine::compile(one_step_graph("worker"),
                                           ng::NodeContext{});

    asio::io_context io;
    constexpr int N = 3;
    std::atomic<int> completed{0};

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < N; ++i) {
        // co_spawn each agent run as an independent coroutine on the
        // shared io_context. The io_context worker runs them serially
        // today (each run_async drives its own internal io_context to
        // completion); future coroutine-ification of the engine will
        // make them overlap on this io_context without code changes.
        asio::co_spawn(
            io,
            [&engine, &completed, i]() -> asio::awaitable<void> {
                ng::RunConfig cfg;
                cfg.thread_id = "agent-" + std::to_string(i);
                auto result = co_await engine->run_async(cfg);
                completed.fetch_add(1, std::memory_order_relaxed);
                std::cout << "[agent " << i << "] done — result="
                          << result.output["channels"]["result"]["value"]
                                .get<std::string>() << "\n";
            },
            asio::detached);
    }

    io.run();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "\n" << completed.load() << "/" << N
              << " agents completed in " << elapsed_ms << "ms.\n"
              << "(Each engine->run_async() drives its own internal io_context to\n"
              << " completion in 3.0, so the three runs are still serial here —\n"
              << " expected ~" << (50 * N) << "ms. The call shape is what locks in;\n"
              << " for true overlap today, dispatch each run() onto a shared\n"
              << " asio::thread_pool — see benchmarks/concurrent/CONCURRENT.md.)\n";
    return 0;
}
