// NeoGraph Example 27: Async Concurrent Runs (Stage 3 / Sem 3.6 + 4.1)
//
// Drives multiple agent runs through a single shared asio::io_context
// using engine->run_async(). This is the canonical pattern users adopt
// when they want to host many concurrent agents without paying the
// thread-per-agent cost that the sync run() implies.
//
// **Today's behaviour (Stage 3 / Sem 3.6 API surface)**: the engine
// internals (super-step loop, Taskflow fan-out, checkpoint writes)
// are still synchronous, so the wrapper effectively serializes runs
// on the io_context's worker thread. The example still exercises the
// awaitable surface end-to-end and the API contract is the one users
// will see once the engine internals get coroutinized.
//
// **Tomorrow's behaviour (Sem 3.6 internal)**: real interleaving
// across runs — when one agent's LLM call is in flight (already
// non-blocking via Provider::complete_async), other agents make
// forward progress on the same thread.
//
// Scenario: three agents each run a tiny graph that simulates a
// 50ms work step. Sync would take ~150ms (3 × 50). Async on one
// io_context takes ~50ms once the engine internals are coroutine-
// native; today's wrapper still takes ~150ms. The point is the API
// shape — switch your call sites now, gain the speed-up later.
//
// Usage: ./example_async_concurrent_runs

#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace ng = neograph::graph;

class WorkNode : public ng::GraphNode {
    std::string name_;
    int delay_ms_;
public:
    WorkNode(std::string name, int delay_ms)
        : name_(std::move(name)), delay_ms_(delay_ms) {}

    std::vector<ng::ChannelWrite> execute(const ng::GraphState&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        return {ng::ChannelWrite{"result",
                                 neograph::json("done by " + name_)}};
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
        // shared io_context. With the current Sem 3.6 thin wrapper the
        // io_context's worker thread runs them serially; with the
        // future internal coroutine-ification they will overlap.
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
              << "(Sync sequential would be ~" << (50 * N) << "ms; future "
              << "internal coroutines will collapse this toward the longest "
              << "single run.)\n";
    return 0;
}
