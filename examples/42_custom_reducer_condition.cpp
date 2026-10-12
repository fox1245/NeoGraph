// NeoGraph Example 42: your own reducer and your own condition
//
// The built-in reducers are `overwrite` and `append`; the built-in conditions are
// `has_tool_calls` and `route_channel`. Anything else you register by name and then use in the
// graph definition:
//
//   * ReducerRegistry::register_reducer("sum", fn)
//       a reducer that adds each incoming number to the running total of a channel
//       (token counters, rolling cost, score sums);
//   * ConditionRegistry::register_condition("score_above_5", fn)
//       a condition that reads the total and picks a branch, like a feature flag driven by a
//       channel value instead of a tool call.
//
// Three steps add 2, 3 and 2. The total, 7, is above the threshold, so the condition routes
// to `warn`. An unregistered name fails build(): change "sum" to "summ" below to see it.
//
// No API key required.
//
// Usage: ./example_custom_reducer_condition
//
// smoke-expect: Final score: 7
// smoke-expect: All assertions passed.
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/state.h>

#include <algorithm>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Writes a fixed number to the `score` channel; the `sum` reducer adds it to the total.
class IncrementNode : public SyncGraphNode {
public:
    IncrementNode(std::string name, int delta) : SyncGraphNode(std::move(name)), delta_(delta) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"score", json(delta_)}}};
    }
private:
    int delta_;
};

// One terminal node per route; it records which route was taken and the total it saw.
class VerdictNode : public SyncGraphNode {
public:
    VerdictNode(std::string name, std::string label)
        : SyncGraphNode(std::move(name)), label_(std::move(label)) {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const std::string text = label_ + ": running total " + std::to_string(in.state.get("score").get<int>());
        return NodeOutput{{ChannelWrite{"verdict", json(text)}}};
    }
private:
    std::string label_;
};

int main() {
    // The reducer receives the channel's current value (null before the first write) and the
    // incoming write, and returns the new value.
    ReducerRegistry::instance().register_reducer("sum", [](const json& current, const json& incoming) {
        return json((current.is_number() ? current.get<int>() : 0) + incoming.get<int>());
    });

    // The condition returns a label; the edge's `routes` map turns the label into a node.
    ConditionRegistry::instance().register_condition("score_above_5", [](const GraphState& state) {
        return state.get("score").get<int>() > 5 ? "above" : "below";
    });

    auto& factory = NodeFactory::instance();
    factory.register_type("increment", [](const std::string& name, const json& config, const NodeContext&) {
        return std::make_unique<IncrementNode>(name, config.value("delta", 1));
    });
    factory.register_type("verdict", [](const std::string& name, const json& config, const NodeContext&) {
        return std::make_unique<VerdictNode>(name, config.value("label", "done"));
    });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "custom_reducer_demo"},
        {"channels", {
            {"score",   {{"reducer", "sum"}}},          // the custom reducer, by name
            {"verdict", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"step1", {{"type", "increment"}, {"delta", 2}}},
            {"step2", {{"type", "increment"}, {"delta", 3}}},
            {"step3", {{"type", "increment"}, {"delta", 2}}},
            {"warn",  {{"type", "verdict"}, {"label", "WARN"}}},
            {"cont",  {{"type", "verdict"}, {"label", "OK"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "step1"}},
            {{"from", "step1"}, {"to", "step2"}},
            {{"from", "step2"}, {"to", "step3"}},
            {{"from", "step3"}, {"condition", "score_above_5"},      // the custom condition, by name
             {"routes", {{"above", "warn"}, {"below", "cont"}}}},
            {{"from", "warn"}, {"to", "__end__"}},
            {{"from", "cont"}, {"to", "__end__"}}
        })}
    };
    auto engine = GraphEngine::build(definition, EngineConfig{});
    auto result = engine->run(RunConfig{});

    const int score = result.channel<int>("score");
    std::cout << "Execution trace: ";
    for (const auto& node : result.execution_trace) std::cout << node << " -> ";
    std::cout << "END\n";
    std::cout << "Final score: " << score << "  (2+3+2 via the custom `sum` reducer)\n";
    std::cout << "Final verdict: " << result.channel<std::string>("verdict") << "\n";

    const auto& trace = result.execution_trace;
    if (score != 7 || std::find(trace.begin(), trace.end(), "warn") == trace.end()) {
        std::cerr << "FAIL: expected score 7 and the route through `warn`\n";
        return 1;
    }
    std::cout << "\nAll assertions passed.\n";
    return 0;
}
