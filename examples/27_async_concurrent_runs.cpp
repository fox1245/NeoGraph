// NeoGraph Example 27: Async Concurrent Runs
//
// Drives multiple agent runs through a single shared asio::io_context
// using engine->run_async(). This is the wiring pattern users adopt
// when they want to host many concurrent agents without paying the
// thread-per-agent cost that the sync run() implies.
//
// **Current behaviour (Stage 4):** run_async stays on the caller's
// executor end-to-end — every super-step suspension point (node
// dispatch, checkpoint I/O, parallel fan-out, retry backoff) is a
// real co_await. The three 50 ms steps below therefore overlap on
// one io_context thread and the wall time lands at ~50 ms, not
// 3×50 ms. One thread, N concurrent agents — no thread-per-agent cost.
//
// Scenario: three agents each run a tiny graph that simulates a 50 ms
// work step using asio::steady_timer (NOT std::this_thread::sleep_for).
// The work node yields cleanly to the timer, so the three agent runs
// interleave on one thread during the wait.
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

    asio::awaitable<ng::NodeOutput> run(ng::NodeInput) override {
        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer t(ex);
        t.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await t.async_wait(asio::use_awaitable);
        ng::NodeOutput out;
        out.writes.push_back(
            ng::ChannelWrite{"result", neograph::json("done by " + name_)});
        co_return out;
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
        // shared io_context. With the Stage-4 engine, run_async never
        // spawns an inner io_context — the three agents' timer waits
        // overlap on this single io_context thread and finish in ~50 ms
        // wall-clock, not 3×50 ms.
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
              << "(Stage 4: run_async stays on the caller's executor end-to-end,\n"
              << " so the three 50 ms timer waits overlap on one io_context\n"
              << " thread — expected ~50ms. For CPU-bound fan-out, switch the\n"
              << " driver to a shared asio::thread_pool — see CONCURRENT.md.)\n";
    return 0;
}
