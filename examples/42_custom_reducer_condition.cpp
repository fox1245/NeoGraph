// NeoGraph Example 42: Custom reducers and conditions from C++
//
// The README mentions custom reducers and conditions (and the Python
// binding shows them), but no existing C++ example actually registers
// one via `ReducerRegistry::instance().register_reducer(...)` or
// `ConditionRegistry::instance().register_condition(...)`. The two
// built-in reducers are `overwrite` and `append`; the two built-in
// conditions are `has_tool_calls` and `route_channel`.
//
// This example demonstrates both:
//
//   * A custom "sum" reducer that adds incoming numeric values to the
//     running channel total — useful for accumulator-style channels
//     (token counters, rolling cost, score sums).
//
//   * A custom "running_total_above_threshold" condition that reads
//     the accumulator and routes to either a "warn" or "continue"
//     branch — i.e. a feature flag triggered by a channel value
//     rather than a tool call.
//
// No API key required.
//
// Usage: ./example_custom_reducer_condition

#include <neograph/neograph.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/state.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Custom node: writes a fixed numeric delta to the "score" channel.
// Demonstrates the "sum" reducer accumulating across super-steps.
class IncrementNode : public GraphNode {
    std::string name_;
    int         delta_;
public:
    IncrementNode(std::string n, int d) : name_(std::move(n)), delta_(d) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"score", json(delta_)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
};

// Custom terminal nodes — one for each route the condition can take.
class WarnNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int s = in.state.get("score").is_number()
                    ? in.state.get("score").get<int>() : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"verdict",
            json("WARN: running total " + std::to_string(s) + " exceeded threshold")});
        co_return out;
    }
    std::string get_name() const override { return "warn"; }
};

class ContinueNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int s = in.state.get("score").is_number()
                    ? in.state.get("score").get<int>() : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"verdict",
            json("OK: running total " + std::to_string(s) + " under threshold")});
        co_return out;
    }
    std::string get_name() const override { return "continue"; }
};

int main() {
    // ── Register custom reducer ──────────────────────────────────────
    //
    // The `sum` reducer treats `current` as the running total and
    // adds `incoming` (numeric) to it. Initial value is 0 the first
    // time anything writes (the engine seeds channels via reducer with
    // a null `current`, so we explicitly normalise to 0).
    ReducerRegistry::instance().register_reducer("sum",
        [](const json& current, const json& incoming) -> json {
            int cur = current.is_number() ? current.get<int>() : 0;
            int inc = incoming.is_number() ? incoming.get<int>() : 0;
            return json(cur + inc);
        });

    // ── Register custom condition ────────────────────────────────────
    //
    // The condition returns a route key matched against the
    // `routes` map in the JSON edge definition.
    ConditionRegistry::instance().register_condition(
        "score_above_5",
        [](const GraphState& state) -> std::string {
            int s = state.get("score").is_number()
                        ? state.get("score").get<int>() : 0;
            return s > 5 ? "above" : "below";
        });

    // ── Register custom node types ──────────────────────────────────
    NodeFactory::instance().register_type("increment",
        [](const std::string& name, const json& cfg, const NodeContext&) {
            int delta = cfg.value("delta", 1);
            return std::make_unique<IncrementNode>(name, delta);
        });
    NodeFactory::instance().register_type("warn",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<WarnNode>();
        });
    NodeFactory::instance().register_type("continue_node",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ContinueNode>();
        });

    // ── Graph: __start__ -> step1 -> step2 -> step3 -> router -> {warn|continue}
    //   Each step adds to `score` via the `sum` reducer. After three
    //   steps the running total is 2+3+2 = 7, which crosses the 5
    //   threshold, so the condition routes to `warn`.
    //
    // Channel definition uses `"reducer": "sum"` — the literal string
    // is what gets passed through to ReducerRegistry::get("sum"). If
    // the registry name is misspelled, compile() throws — try changing
    // "sum" to "summ" below to see the failure mode.
    json def = {
        {"name", "custom_reducer_demo"},
        {"channels", {
            {"score",   {{"reducer", "sum"}}},
            {"verdict", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"step1",  {{"type", "increment"}, {"delta", 2}}},
            {"step2",  {{"type", "increment"}, {"delta", 3}}},
            {"step3",  {{"type", "increment"}, {"delta", 2}}},
            {"warn",   {{"type", "warn"}}},
            {"cont",   {{"type", "continue_node"}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "step1"}},
            {{"from", "step1"},     {"to", "step2"}},
            {{"from", "step2"},     {"to", "step3"}},
            // After step3 — branch via the custom condition.
            {{"from", "step3"},
             {"condition", "score_above_5"},
             {"routes", {{"above", "warn"}, {"below", "cont"}}}},
            {{"from", "warn"}, {"to", "__end__"}},
            {{"from", "cont"}, {"to", "__end__"}},
        })}
    };

    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);

    RunConfig cfg;
    cfg.thread_id = "t1";
    cfg.input     = {};   // score starts at 0 (sum reducer normalises null)

    auto r = engine->run(cfg);

    std::cout << "Execution trace: ";
    for (const auto& n : r.execution_trace) std::cout << n << " -> ";
    std::cout << "END\n";

    std::cout << "Final score: " << r.output["channels"]["score"]["value"]
              << "  (2+3+2 = 7 via custom `sum` reducer)\n";
    std::cout << "Final verdict: "
              << r.output["channels"]["verdict"]["value"].get<std::string>()
              << "\n";

    // Sanity assertions:
    int score = r.output["channels"]["score"]["value"].get<int>();
    if (score != 7) {
        std::cerr << "FAIL: expected score=7 but got " << score << "\n";
        return 1;
    }
    auto trace = r.execution_trace;
    bool went_warn = false;
    for (const auto& n : trace) if (n == "warn") went_warn = true;
    if (!went_warn) {
        std::cerr << "FAIL: expected route via `warn` (score>5)\n";
        return 1;
    }

    std::cout << "\nAll assertions passed.\n";
    return 0;
}
