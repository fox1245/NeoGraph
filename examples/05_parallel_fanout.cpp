// NeoGraph Example 05: Parallel Fan-out / Fan-in
//
// Runs multiple nodes in parallel using Taskflow,
// collects all results (fan-in), then proceeds to the next step.
//
// Scenario: Parallel research agent
//   __start__ → [researcher_a, researcher_b, researcher_c] → summarizer → __end__
//   3 researchers work concurrently, and the summarizer aggregates the results.
//
// No API key required (uses custom nodes)
//
// Usage: ./example_parallel_fanout

#include <neograph/neograph.h>

#include <iostream>
#include <thread>
#include <chrono>

// Custom node: Researcher (simulation — 100ms delay)
class ResearcherNode : public neograph::graph::GraphNode {
    std::string name_;
    std::string topic_;
    int delay_ms_;

public:
    ResearcherNode(const std::string& name, const std::string& topic, int delay_ms)
        : name_(name), topic_(topic), delay_ms_(delay_ms) {}

    std::vector<neograph::graph::ChannelWrite> execute(
        const neograph::graph::GraphState& state) override {

        auto start = std::chrono::steady_clock::now();

        // In practice, this is where you'd call an LLM or web search
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        neograph::json result = {
            {"researcher", name_},
            {"topic", topic_},
            {"finding", "Research findings on " + topic_ + ". (" + std::to_string(elapsed) + "ms)"},
            {"elapsed_ms", elapsed}
        };

        // Append result to the "findings" channel
        return {neograph::graph::ChannelWrite{"findings", neograph::json::array({result})}};
    }

    std::string get_name() const override { return name_; }
};

// Custom node: Summarizer
class SummarizerNode : public neograph::graph::GraphNode {
public:
    std::vector<neograph::graph::ChannelWrite> execute(
        const neograph::graph::GraphState& state) override {

        auto findings = state.get("findings");
        std::string summary = "=== Research Summary ===\n";

        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "- [" + f.value("researcher", "?") + "] "
                        + f.value("finding", "") + "\n";
            }
            summary += "\nTotal " + std::to_string(findings.size()) + " research findings collected.";
        }

        return {neograph::graph::ChannelWrite{"summary", neograph::json(summary)}};
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

    // Execute
    std::cout << "=== Parallel Fan-out / Fan-in ===\n\n";

    auto total_start = std::chrono::steady_clock::now();

    neograph::graph::RunConfig config;
    auto result = engine->run_stream(config,
        [](const neograph::graph::GraphEvent& event) {
            if (event.type == neograph::graph::GraphEvent::Type::NODE_START)
                std::cout << "[start] " << event.node_name << "\n";
            if (event.type == neograph::graph::GraphEvent::Type::NODE_END)
                std::cout << "[done]  " << event.node_name << "\n";
        });

    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - total_start).count();

    std::cout << "\nExecution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n";

    // Print summary
    if (result.output.contains("channels") &&
        result.output["channels"].contains("summary")) {
        std::cout << "\n" << result.output["channels"]["summary"]["value"].get<std::string>() << "\n";
    }

    std::cout << "\nTotal elapsed: " << total_ms << "ms";
    std::cout << " (sequential would be ~370ms)\n";

    return 0;
}
