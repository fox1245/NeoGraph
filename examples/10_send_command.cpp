// NeoGraph Example 10: Send & Command Engine Integration
//
// Demonstrates Send (dynamic fan-out) and Command (routing override)
// working in the engine.
//
// Scenario: Research agent
//   1. planner node analyzes the user's question and uses Send for dynamic fan-out
//   2. researcher node investigates each topic in parallel
//   3. evaluator node assesses results and branches via Command
//      - sufficient → summarizer → END
//      - insufficient → back to planner for additional research
//
// No API key required (custom nodes)
//
// Usage: ./example_send_command

#include <neograph/neograph.h>

#include <iostream>
#include <thread>
#include <chrono>

using namespace neograph;
using namespace neograph::graph;

// =========================================================================
// PlannerNode: extracts topics from user question and uses Send for dynamic fan-out
// =========================================================================
class PlannerNode : public GraphNode {
    static int round_;
public:
    // Override execute_full to return Sends
    NodeResult execute_full(const GraphState& state) override {
        round_++;
        auto query = state.get("query");
        std::string q = query.is_string() ? query.get<std::string>() : "general research";

        // Round 1: 3 topics, Round 2: 2 additional topics
        std::vector<std::string> topics;
        if (round_ == 1) {
            topics = {"market_size", "key_players", "technology_trends"};
        } else {
            topics = {"risk_factors", "future_outlook"};
        }

        NodeResult nr;
        nr.writes.push_back(ChannelWrite{"plan", json({
            {"round", round_},
            {"query", q},
            {"topics", topics}
        })});

        // Send: dynamically execute researcher node once per topic
        // Send.input is a channel-name→value mapping (applied to state via apply_input)
        for (const auto& topic : topics) {
            nr.sends.push_back(Send{"researcher", json({
                {"topic", topic}   // Inject topic string into "topic" channel
            })});
        }

        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "planner"; }
};
int PlannerNode::round_ = 0;

// =========================================================================
// ResearcherNode: investigates a single topic (invoked via Send)
// =========================================================================
class ResearcherNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto topic_json = state.get("topic");
        std::string topic = topic_json.is_string() ? topic_json.get<std::string>() : "unknown";

        // Simulation: research takes 50-100ms
        int delay = 50 + (std::hash<std::string>{}(topic) % 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

        json finding = {
            {"topic", topic},
            {"result", "Analysis of " + topic + " completed."},
            {"confidence", 0.7 + (std::hash<std::string>{}(topic) % 30) / 100.0},
            {"elapsed_ms", delay}
        };

        return {ChannelWrite{"findings", json::array({finding})}};
    }

    std::string name() const override { return "researcher"; }
};

// =========================================================================
// EvaluatorNode: evaluates results and decides branching via Command
// =========================================================================
class EvaluatorNode : public GraphNode {
    static int eval_count_;
public:
    NodeResult execute_full(const GraphState& state) override {
        eval_count_++;
        auto findings = state.get("findings");
        int count = findings.is_array() ? (int)findings.size() : 0;

        NodeResult nr;

        if (count >= 5 || eval_count_ >= 2) {
            // Sufficient data → go to summarizer
            nr.command = Command{
                "summarizer",
                {ChannelWrite{"eval_status", json("sufficient: " + std::to_string(count) + " findings")}}
            };
        } else {
            // Insufficient → go back to planner for more research
            nr.command = Command{
                "planner",
                {ChannelWrite{"eval_status", json("insufficient: need more research (have " + std::to_string(count) + ")")}}
            };
        }

        return nr;
    }

    std::vector<ChannelWrite> execute(const GraphState& state) override {
        return execute_full(state).writes;
    }

    std::string name() const override { return "evaluator"; }
};
int EvaluatorNode::eval_count_ = 0;

// =========================================================================
// SummarizerNode: aggregates all results
// =========================================================================
class SummarizerNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto findings = state.get("findings");
        auto eval = state.get("eval_status");

        std::string summary = "=== Research Summary ===\n";
        summary += "Status: " + (eval.is_string() ? eval.get<std::string>() : "n/a") + "\n\n";

        if (findings.is_array()) {
            for (const auto& f : findings) {
                summary += "- [" + f.value("topic", "?") + "] "
                        + f.value("result", "") + " (confidence: "
                        + std::to_string(f.value("confidence", 0.0)).substr(0, 4) + ")\n";
            }
            summary += "\nTotal findings: " + std::to_string(findings.size());
        }

        return {ChannelWrite{"summary", json(summary)}};
    }

    std::string name() const override { return "summarizer"; }
};

// =========================================================================
// Main
// =========================================================================
int main() {
    // Register custom nodes
    auto& factory = NodeFactory::instance();

    factory.register_type("planner",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PlannerNode>(); });

    factory.register_type("researcher",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ResearcherNode>(); });

    factory.register_type("evaluator",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<EvaluatorNode>(); });

    factory.register_type("summarizer_node",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<SummarizerNode>(); });

    // Graph definition
    // planner → (dynamic researcher execution via Send) → evaluator → branching via Command
    json definition = {
        {"name", "research_agent"},
        {"channels", {
            {"query",       {{"reducer", "overwrite"}}},
            {"plan",        {{"reducer", "overwrite"}}},
            {"topic",       {{"reducer", "overwrite"}}},  // Channel injected by Send
            {"findings",    {{"reducer", "append"}}},
            {"eval_status", {{"reducer", "overwrite"}}},
            {"summary",     {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",    {{"type", "planner"}}},
            {"researcher", {{"type", "researcher"}}},
            {"evaluator",  {{"type", "evaluator"}}},
            {"summarizer", {{"type", "summarizer_node"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"},   {"to", "evaluator"}},
            // No edge needed here since evaluator routes via Command
            // (Command.goto directly specifies "summarizer" or "planner")
            {{"from", "summarizer"}, {"to", "__end__"}}
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(definition, ctx);

    // Execute
    std::cout << "=== Send & Command Integration Demo ===\n\n";

    auto start = std::chrono::steady_clock::now();

    RunConfig config;
    config.input = {{"query", "AI semiconductor market analysis"}};
    config.max_steps = 20;
    config.stream_mode = StreamMode::EVENTS | StreamMode::DEBUG;

    auto result = engine->run_stream(config,
        [](const GraphEvent& event) {
            switch (event.type) {
                case GraphEvent::Type::NODE_START:
                    if (event.node_name == "__send__") {
                        auto& sends = event.data["sends"];
                        std::cout << "  [send] Dynamic fan-out: " << sends.size() << " tasks\n";
                        for (const auto& s : sends)
                            std::cout << "         → " << s["target"] << "(" << s["input"]["topic"] << ")\n";
                    } else if (event.node_name == "__routing__") {
                        if (event.data.contains("command_goto"))
                            std::cout << "  [cmd]  Command goto → " << event.data["command_goto"] << "\n";
                        else if (event.data.contains("next_nodes"))
                            std::cout << "  [route] → " << event.data["next_nodes"].dump() << "\n";
                    } else {
                        std::cout << "  [start] " << event.node_name << "\n";
                    }
                    break;
                case GraphEvent::Type::NODE_END: {
                    std::string extra;
                    if (event.data.contains("command_goto"))
                        extra = " (cmd→" + event.data["command_goto"].get<std::string>() + ")";
                    if (event.data.contains("sends"))
                        extra = " (sends=" + std::to_string(event.data["sends"].get<int>()) + ")";
                    std::cout << "  [done]  " << event.node_name << extra << "\n";
                    break;
                }
                default: break;
            }
        });

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Print results
    std::cout << "\n";
    std::cout << "Execution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i) {
        std::cout << result.execution_trace[i];
        if (i + 1 < result.execution_trace.size()) std::cout << " → ";
    }
    std::cout << " → END\n\n";

    if (result.output.contains("channels") &&
        result.output["channels"].contains("summary") &&
        result.output["channels"]["summary"]["value"].is_string()) {
        std::cout << result.output["channels"]["summary"]["value"].get<std::string>() << "\n";
    } else {
        std::cout << "(No summary generated — check execution trace)\n";
    }

    std::cout << "\nTotal time: " << elapsed << "ms\n";
    std::cout << "(Researchers ran in parallel via Send + Taskflow)\n";

    return 0;
}
