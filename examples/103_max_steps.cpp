// NeoGraph Example 103: max_steps stops a loop and says so
//
// A graph with a cycle can run for ever if its exit condition never holds. RunConfig::max_steps
// (default 50) caps the number of super-steps. When the cap stops a run that still has work
// ready, run() does not throw: it returns the state reached so far, with
// `max_steps_exhausted() == true` and status() == RunStatus::StepLimit. A run that finishes
// on its own, even on the last allowed step, is not flagged.
//
// The loop is `count -> count` while the `counter` channel is below the `target` input.
//
// No API key required.
//
// Usage: ./example_max_steps
//
// smoke-expect: target=1000 max_steps=5: steps run: 5, max_steps_exhausted: true, status: StepLimit, counter: 5
// smoke-expect: target=3 max_steps=5: steps run: 3, max_steps_exhausted: false, status: Completed, counter: 3
// smoke-expect: target=5 max_steps=5: steps run: 5, max_steps_exhausted: false, status: Completed, counter: 5
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

class CountNode : public SyncGraphNode {
public:
    CountNode() : SyncGraphNode("count") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const auto current = in.state.get("counter");
        const int  next    = (current.is_number() ? current.get<int>() : 0) + 1;
        return NodeOutput{{ChannelWrite{"counter", json(next)}}};
    }
};

const char* status_name(RunStatus status) {
    switch (status) {
        case RunStatus::Completed: return "Completed";
        case RunStatus::StepLimit: return "StepLimit";
        default: return "other";
    }
}

int main() {
    NodeFactory::instance().register_type("count",
        [](const std::string&, const json&, const NodeContext&) { return std::make_unique<CountNode>(); });

    // The condition decides between another lap and the exit.
    ConditionRegistry::instance().register_condition("below_target", [](const GraphState& state) {
        return state.get("counter").get<int>() < state.get("target").get<int>() ? "again" : "done";
    });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "max_steps"},
        {"channels", {{"counter", {{"reducer", "overwrite"}}}, {"target", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"count", {{"type", "count"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "count"}},
            {{"from", "count"}, {"condition", "below_target"},
             {"routes", {{"again", "count"}, {"done", "__end__"}}}}
        })}
    };
    auto engine = GraphEngine::build(definition, EngineConfig{});

    struct Case { int target; int max_steps; };
    for (const Case c : {Case{1000, 5}, Case{3, 5}, Case{5, 5}}) {
        RunConfig run;
        run.input     = {{"target", c.target}};
        run.max_steps = c.max_steps;
        const auto result = engine->run(run);
        std::cout << "target=" << c.target << " max_steps=" << c.max_steps
                  << ": steps run: " << result.execution_trace.size()
                  << ", max_steps_exhausted: " << (result.max_steps_exhausted() ? "true" : "false")
                  << ", status: " << status_name(result.status())
                  << ", counter: " << result.channel<int>("counter") << "\n";
    }
    return 0;
}
