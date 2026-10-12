// NeoGraph Example 10: Send and Command
//
// Send creates work at run time: a node returns one Send per item and the engine runs the
// target node once per Send, in parallel, each on its own copy of the state.
// Command routes at run time: a node returns the next node together with state updates, and
// that overrides the node's ordinary edges.
//
// Scenario: a research loop.
//   planner     picks topics (3 in round 1, 2 in round 2) and Sends one `researcher` per topic
//   researcher  records one finding per Send
//   evaluator   Commands back to `planner` until 5 findings exist, then on to `summarizer`
//
//   __start__ -> planner -> evaluator -> (Command) planner | summarizer -> __end__
//
// No API key required.
//
// Usage: ./example_send_command
//
// smoke-expect: Status: sufficient: 5 findings
// smoke-expect: Total findings: 5
// smoke-expect: [command] evaluator -> summarizer
// smoke-expect: Execution trace: planner → researcher[send] → researcher[send] → researcher[send] → evaluator → planner
// smoke-forbid: [neograph] lint
// smoke-forbid: [neograph] warning

#include <neograph/neograph.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Picks the topics for this round and starts one researcher per topic.
class PlannerNode : public SyncGraphNode {
public:
    PlannerNode() : SyncGraphNode("planner") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const auto previous = in.state.get("round");
        const int  round    = (previous.is_number() ? previous.get<int>() : 0) + 1;
        const std::vector<std::string> topics =
            round == 1 ? std::vector<std::string>{"market_size", "key_players", "technology_trends"}
                       : std::vector<std::string>{"risk_factors", "future_outlook"};

        NodeOutput out;
        out.writes.push_back(ChannelWrite{"round", json(round)});
        for (const auto& topic : topics)
            out.sends.push_back(Send{"researcher", json({{"topic", topic}})});   // one run per Send
        return out;
    }
};

// Runs once per Send. The Send payload (`topic`) is visible only to that run.
class ResearcherNode : public SyncGraphNode {
public:
    ResearcherNode() : SyncGraphNode("researcher") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const std::string topic = in.state.get("topic").get<std::string>();
        json finding = {{"topic", topic}, {"result", "Analysis of " + topic + " completed."}};
        return NodeOutput{{ChannelWrite{"findings", json::array({finding})}}};
    }
};

// Decides where to go next and records why, in one return value.
class EvaluatorNode : public SyncGraphNode {
public:
    EvaluatorNode() : SyncGraphNode("evaluator") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const int count = static_cast<int>(in.state.get("findings").size());
        NodeOutput out;
        if (count >= 5)
            out.command = Command{"summarizer",
                {ChannelWrite{"eval_status", json("sufficient: " + std::to_string(count) + " findings")}}};
        else
            out.command = Command{"planner",
                {ChannelWrite{"eval_status", json("insufficient: have " + std::to_string(count))}}};
        return out;
    }
};

class SummarizerNode : public SyncGraphNode {
public:
    SummarizerNode() : SyncGraphNode("summarizer") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const auto  findings = in.state.get("findings");
        std::string summary  = "=== Research Summary ===\nStatus: " +
                               in.state.get("eval_status").get<std::string>() + "\n\n";
        for (const auto& f : findings)
            summary += "- [" + f.value("topic", "?") + "] " + f.value("result", "") + "\n";
        summary += "\nTotal findings: " + std::to_string(findings.size());
        return NodeOutput{{ChannelWrite{"summary", json(summary)}}};
    }
};

int main() {
    auto& factory = NodeFactory::instance();
    factory.register_type("planner", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<PlannerNode>(); });
    factory.register_type("researcher", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<ResearcherNode>(); });
    factory.register_type("evaluator", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<EvaluatorNode>(); });
    factory.register_type("summarizer", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<SummarizerNode>(); });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "research_agent"},
        {"channels", {
            {"round",       {{"reducer", "overwrite"}}},
            {"topic",       {{"reducer", "overwrite"}}},   // filled by each Send
            {"findings",    {{"reducer", "append"}}},
            {"eval_status", {{"reducer", "overwrite"}}},
            {"summary",     {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"planner",    {{"type", "planner"}}},
            {"researcher", {{"type", "researcher"}}},
            {"evaluator",  {{"type", "evaluator"}}},
            {"summarizer", {{"type", "summarizer"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "planner"}},
            {{"from", "planner"}, {"to", "evaluator"}},    // runs after the Sends have finished
            {{"from", "evaluator"}, {"to", "summarizer"}}, // the default; a Command can choose "planner" instead
            {{"from", "summarizer"}, {"to", "__end__"}}
            // No edge leads to `researcher`: only Send starts it.
        })}
    };

    EngineConfig config;
    config.worker_count   = 3;      // as wide as the widest Send batch, so the three run in parallel
    config.stderr_notices = false;  // `researcher` is reached only by Send, which the static
                                    // reachability lint (E7) cannot see; the notice is expected
    auto engine = GraphEngine::build(definition, std::move(config));

    RunConfig run;
    run.input       = {{"round", 0}};
    run.max_steps   = 20;
    run.stream_mode = StreamMode::EVENTS;
    auto result = engine->run_stream(run, [](const GraphEvent& event) {
        if (event.type != GraphEvent::Type::NODE_END) return;
        if (event.data.contains("sends"))
            std::cout << "[send]    " << event.node_name << " started "
                      << event.data["sends"].get<int>() << " researchers\n";
        if (event.data.contains("command_goto"))
            std::cout << "[command] " << event.node_name << " -> "
                      << event.data["command_goto"].get<std::string>() << "\n";
    });

    std::cout << "\nExecution trace: ";
    for (size_t i = 0; i < result.execution_trace.size(); ++i)
        std::cout << (i ? " → " : "") << result.execution_trace[i];
    std::cout << "\n\n" << result.channel<std::string>("summary") << "\n";
    return 0;
}
