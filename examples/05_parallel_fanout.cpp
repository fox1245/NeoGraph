// NeoGraph Example 05: parallel fan-out and fan-in
//
// Three researcher nodes run in the same super-step and a summarizer joins them:
//
//   __start__ -> [researcher_a, researcher_b, researcher_c] -> summarizer -> __end__
//
// Each researcher waits on an asio timer (a stand-in for an HTTP or model call), so all
// three are in flight at once; the example counts them to prove it. Two settings make the
// graph correct and quiet:
//   * a `barrier` on the summarizer, so it runs once, after all three researchers;
//   * `worker_count = 3`, so the engine owns a pool as wide as the fan-out. With the default,
//     1, branches that block or compute run one after the other, and the engine prints a
//     warning about it.
// The run is driven with run_async on one io_context.
//
// No API key required.
//
// Usage: ./example_parallel_fanout
//
// smoke-expect: Execution trace: researcher_a → researcher_b → researcher_c → summarizer → END
// smoke-expect: Total 3 research findings collected.
// smoke-expect: Peak researchers in flight: 3
// smoke-forbid: [neograph] lint
// smoke-forbid: [neograph] warning

#include <neograph/neograph.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Counts how many researchers are working at the same moment.
struct Gauge {
    std::atomic<int> now{0};
    std::atomic<int> peak{0};
    void enter() {
        const int value = ++now;
        int       seen  = peak.load();
        while (value > seen && !peak.compare_exchange_weak(seen, value)) {}
    }
    void leave() { --now; }
};

// Waits on a timer instead of sleeping, so its worker is free while the "request" is out.
class ResearcherNode : public GraphNode {
public:
    ResearcherNode(std::string name, std::string topic, int delay_ms, std::shared_ptr<Gauge> gauge)
        : name_(std::move(name)), topic_(std::move(topic)), delay_ms_(delay_ms),
          gauge_(std::move(gauge)) {}

    asio::awaitable<NodeOutput> run(NodeInput) override {
        gauge_->enter();
        auto               executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        timer.expires_after(std::chrono::milliseconds(delay_ms_));
        co_await timer.async_wait(asio::use_awaitable);
        gauge_->leave();

        json finding;
        finding["researcher"] = name_;
        finding["finding"]    = "Research findings on " + topic_ + ".";
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"findings", json::array({finding})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string            name_, topic_;
    int                    delay_ms_;
    std::shared_ptr<Gauge> gauge_;
};

// Runs once, after the barrier has seen all three researchers.
class SummarizerNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        const auto  findings = in.state.get("findings");
        std::string summary  = "=== Research Summary ===\n";
        for (const auto& f : findings)
            summary += "- [" + f.value("researcher", "?") + "] " + f.value("finding", "") + "\n";
        summary += "\nTotal " + std::to_string(findings.size()) + " research findings collected.";
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"summary", json(summary)});
        co_return out;
    }
    std::string get_name() const override { return "summarizer"; }
};

int main() {
    auto  gauge   = std::make_shared<Gauge>();
    auto& factory = NodeFactory::instance();
    factory.register_type("researcher",
        [gauge](const std::string& name, const json& config, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<ResearcherNode>(
                name, config.value("topic", "unknown"), config.value("delay_ms", 100), gauge);
        });
    factory.register_type("summarizer",
        [](const std::string&, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
            return std::make_unique<SummarizerNode>();
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "parallel_research"},
        {"channels", {
            {"findings", {{"reducer", "append"}}},
            {"summary",  {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"researcher_a", {{"type", "researcher"}, {"topic", "AI semiconductor market"}, {"delay_ms", 300}}},
            {"researcher_b", {{"type", "researcher"}, {"topic", "Quantum computing trends"}, {"delay_ms", 200}}},
            {"researcher_c", {{"type", "researcher"}, {"topic", "Autonomous driving technology"}, {"delay_ms", 250}}},
            // The barrier makes the join explicit: summarizer waits for all three.
            {"summarizer", {{"type", "summarizer"},
                            {"barrier", {{"wait_for", json::array({"researcher_a", "researcher_b", "researcher_c"})}}}}}
        }},
        {"edges", json::array({
            // Fan-out: one edge per researcher; they all become ready in the same super-step.
            {{"from", "__start__"}, {"to", "researcher_a"}},
            {{"from", "__start__"}, {"to", "researcher_b"}},
            {{"from", "__start__"}, {"to", "researcher_c"}},
            // Fan-in: each researcher signals the barrier.
            {{"from", "researcher_a"}, {"to", "summarizer"}},
            {{"from", "researcher_b"}, {"to", "summarizer"}},
            {{"from", "researcher_c"}, {"to", "summarizer"}},
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    EngineConfig config;
    config.worker_count = 3;
    auto engine         = GraphEngine::build(definition, std::move(config));

    asio::io_context io;
    RunResult        result;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> { result = co_await engine->run_async(RunConfig{}); },
        asio::detached);
    io.run();

    std::cout << "Execution trace: ";
    for (const auto& node : result.execution_trace) std::cout << node << " → ";
    std::cout << "END\n\n" << result.channel<std::string>("summary") << "\n\n";

    std::cout << "Peak researchers in flight: " << gauge->peak.load() << " (sequential would be 1)\n";
    return gauge->peak.load() == 3 ? 0 : 1;
}
