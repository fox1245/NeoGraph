// NeoGraph Example 05: Parallel Fan-out / Fan-in (async edition)
//
// Runs multiple nodes in parallel on a single asio::io_context using
// engine->run_stream_async(). The parallel fan-out goes through
// NodeExecutor::run_parallel_async (Stage 3 / Sem 3.7), which uses
// asio::experimental::make_parallel_group on co_spawn-deferred
// workers. Each researcher's "work" is an asio::steady_timer wait
// (stand-in for a real async LLM or HTTP call), so the io_context's
// worker thread stays free for siblings while any one researcher is
// waiting — true overlap on a single OS thread.
//
// Scenario: Parallel research agent
//   __start__ → [researcher_a, researcher_b, researcher_c] → summarizer → __end__
//
// Both run() and run_async() now share the same coroutine machinery
// (asio::experimental::make_parallel_group, no Taskflow). The async
// variant overlaps the I/O on ONE thread, so scaling to hundreds of
// concurrent research runs doesn't pay per-run thread costs; sync
// run() bridges to the same path through neograph::async::run_sync.
//
// No API key required (uses custom nodes).
//
// Usage: ./example_parallel_fanout

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

// Async-native researcher — execute_async co_awaits an asio
// steady_timer instead of std::this_thread::sleep_for, so the
// io_context isn't frozen during the "work".
class ResearcherNode : public neograph::graph::GraphNode {
    std::string name_;
    std::string topic_;
    int delay_ms_;

public:
    ResearcherNode(const std::string& name, const std::string& topic, int delay_ms)
        : name_(name), topic_(topic), delay_ms_(delay_ms) {}

    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput /*in*/) override {
        auto start = std::chrono::steady_clock::now();

        auto ex = co_await asio::this_coro::executor;
        asio::steady_timer timer(ex);
        timer.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await timer.async_wait(asio::use_awaitable);

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        // Build result without nested brace-init in the coroutine body
        // (GCC 13 ICE workaround, same shape as Sem 2.7 / 3.6 step 3).
        neograph::json result;
        result["researcher"] = name_;
        result["topic"]      = topic_;
        result["finding"]    = "Research findings on " + topic_
            + ". (" + std::to_string(elapsed) + "ms)";
        result["elapsed_ms"] = elapsed;

        neograph::graph::NodeOutput out;
        out.writes.push_back(neograph::graph::ChannelWrite{
            "findings", neograph::json::array({result})});
        co_return out;
    }

    // Note: v0.4 unified `run()` replaces the old 8-virtual cross-product.
    // The previous version of this node overrode `execute_full_stream_async`
    // to avoid double-running its already-async body; with `run(NodeInput)`
    // that's no longer needed — engine dispatches `run()` exactly once per
    // super-step regardless of stream mode (the stream_cb just lives in
    // `in.stream_cb`, which we don't use here).

    std::string get_name() const override { return name_; }
};

// Custom node: Summarizer
class SummarizerNode : public neograph::graph::GraphNode {
public:
    asio::awaitable<neograph::graph::NodeOutput>
    run(neograph::graph::NodeInput in) override {

        auto findings = in.state.get("findings");
        std::string summary = "=== Research Summary ===\n";

        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "- [" + f.value("researcher", "?") + "] "
                        + f.value("finding", "") + "\n";
            }
            summary += "\nTotal " + std::to_string(findings.size()) + " research findings collected.";
        }

        neograph::graph::NodeOutput out;
        out.writes.push_back(
            neograph::graph::ChannelWrite{"summary", neograph::json(summary)});
        co_return out;
    }

    std::string get_name() const override { return "summarizer"; }
};

int main() {
    // Register custom node types
    auto& factory = neograph::graph::NodeFactory::instance();

    factory.register_type("researcher",
        [](const std::string& name, const neograph::json& config,
           const neograph::graph::NodeContext&) -> std::unique_ptr<neograph::graph::GraphNode> {
            return std::make_unique<ResearcherNode>(
                name,
                config.value("topic", "unknown"),
                config.value("delay_ms", 100)
            );
        });

    factory.register_type("summarizer",
        [](const std::string& name, const neograph::json&,
           const neograph::graph::NodeContext&) -> std::unique_ptr<neograph::graph::GraphNode> {
            return std::make_unique<SummarizerNode>();
        });

    // JSON-based graph definition
    neograph::json definition = {
        {"name", "parallel_research"},
        {"channels", {
            {"findings", {{"reducer", "append"}}},
            {"summary",  {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"researcher_a", {{"type", "researcher"}, {"topic", "AI semiconductor market"}, {"delay_ms", 150}}},
            {"researcher_b", {{"type", "researcher"}, {"topic", "Quantum computing trends"}, {"delay_ms", 100}}},
            {"researcher_c", {{"type", "researcher"}, {"topic", "Autonomous driving technology"}, {"delay_ms", 120}}},
            {"summarizer",   {{"type", "summarizer"}}}
        }},
        {"edges", neograph::json::array({
            // fan-out: __start__ → 3 researchers (parallel execution)
            {{"from", "__start__"}, {"to", "researcher_a"}},
            {{"from", "__start__"}, {"to", "researcher_b"}},
            {{"from", "__start__"}, {"to", "researcher_c"}},
            // fan-in: 3 researchers → summarizer (after all complete)
            {{"from", "researcher_a"}, {"to", "summarizer"}},
            {{"from", "researcher_b"}, {"to", "summarizer"}},
            {{"from", "researcher_c"}, {"to", "summarizer"}},
            // Summarization complete
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    neograph::graph::NodeContext ctx;  // Custom nodes, no Provider/Tool needed
    auto engine = neograph::graph::GraphEngine::compile(definition, ctx);

    std::cout << "=== Parallel Fan-out / Fan-in (async edition) ===\n\n";

    auto total_start = std::chrono::steady_clock::now();

    neograph::graph::RunConfig config;

    auto event_cb = [](const neograph::graph::GraphEvent& event) {
        if (event.type == neograph::graph::GraphEvent::Type::NODE_START)
            std::cout << "[start] " << event.node_name << "\n";
        if (event.type == neograph::graph::GraphEvent::Type::NODE_END)
            std::cout << "[done]  " << event.node_name << "\n";
    };

    // Drive the graph on a single io_context. The three researcher
    // nodes co_await their timers in parallel via run_parallel_async
    // inside execute_graph_async, so all three 100-150ms delays
    // overlap on one thread.
    asio::io_context io;
    neograph::graph::RunResult result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            result = co_await engine->run_stream_async(config, event_cb);
        },
        asio::detached);
    io.run();

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - total_start).count();

    std::cout << "\nExecution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    if (result.output.contains("channels") &&
        result.output["channels"].contains("summary")) {
        std::cout << "\n" << result.output["channels"]["summary"]["value"].get<std::string>() << "\n";
    }

    std::cout << "\nTotal elapsed: " << total_ms << "ms"
              << " (sequential would be ~370ms; single-thread async "
              << "overlap: ~longest-branch = ~150ms.)\n";

    return 0;
}
