// NeoGraph Example 27: Async Concurrent Runs (Stage 3 / Sem 3.6 + 4.1)
//
// Drives multiple agent runs through a single shared asio::io_context
// using engine->run_async(). This is the canonical pattern users adopt
// when they want to host many concurrent agents without paying the
// thread-per-agent cost that the sync run() implies.
//
// As of Sem 3.6 internal step 3, GraphEngine::run_async dispatches
// single-node super-steps through the coroutine path:
// run_one_async → execute_node_with_retry_async → node->execute_full_async.
// A node whose execute_async issues a real co_await (timer, HTTP,
// MCP, db) yields the io_context's worker thread for siblings to run.
//
// Scenario: three agents each run a tiny graph that simulates a
// 50ms work step using asio::steady_timer (NOT std::this_thread::
// sleep_for). With a sync work node the io_context would still be
// pinned for the full 50ms; with the async work node below all
// three agents make progress in parallel on a single thread —
// total elapsed converges on ~50ms, not 150ms.
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
              << "(Sync sequential would be ~" << (50 * N)
              << "ms; the async work node collapses this toward the "
              << "longest single run — ~50ms with full overlap.)\n";
    return 0;
}
