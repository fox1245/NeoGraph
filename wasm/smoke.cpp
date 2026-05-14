// WASM feasibility smoke — compile a JSON graph definition + run a
// trivial graph entirely inside the engine. No HTTP, no LLM, no
// network. If this builds + runs under em++, Phase 1 is feasible.

#include <neograph/graph/engine.h>
#include <neograph/graph/state.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

class DoubleNode : public GraphNode {
public:
    explicit DoubleNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto seed = in.state.get("seed");
        int v = seed.is_number() ? seed.get<int>() : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"doubled", json(v * 2)});
        co_return out;
    }
private:
    std::string name_;
};

int main() {
    NodeFactory::instance().register_type(
        "doubler",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<DoubleNode>(name);
        });

    json defn = {
        {"name", "wasm_smoke"},
        {"channels", {
            {"seed",    {{"reducer", "overwrite"}}},
            {"doubled", {{"reducer", "overwrite"}}},
        }},
        {"nodes",    {{"d", {{"type", "doubler"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "d"}},
            {{"from", "d"},         {"to", "__end__"}},
        })},
    };

    auto engine = GraphEngine::compile(defn, NodeContext());
    engine->set_checkpoint_store(std::make_shared<InMemoryCheckpointStore>());

    RunConfig cfg;
    cfg.thread_id = "wasm-1";
    cfg.input     = json{{"seed", 21}};
    cfg.max_steps = 5;

    auto result = engine->run(cfg);
    auto out = result.output["channels"]["doubled"]["value"];
    std::printf("doubled = %d\n", out.get<int>());
    std::printf("trace = ");
    for (const auto& step : result.execution_trace) std::printf("%s ", step.c_str());
    std::printf("\n");
    return out.get<int>() == 42 ? 0 : 1;
}
