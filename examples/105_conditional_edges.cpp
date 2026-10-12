// NeoGraph Example 105: conditional edges and the default route
//
// A conditional edge picks the next node at run time. `route_channel` is the built-in
// condition that reads a label from the `__route__` channel and looks it up in the edge's
// `routes`. Here a triage node writes the label and the edge sends the request to a desk.
//
// The reserved route key `default` catches every label the map does not list, so a label the
// graph has never seen (here "sales") still reaches a node instead of stopping the run with
// an error. An explicit default is also what keeps the graph's lint check quiet.
//
// No API key required.
//
// Usage: ./example_conditional_edges
//
// smoke-expect: "my invoice is wrong" -> label billing -> billing_desk
// smoke-expect: "the app crashes on start" -> label technical -> technical_desk
// smoke-expect: "can I get a quote?" -> label sales -> general_desk
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Reads the request and writes a routing label for the conditional edge.
class TriageNode : public SyncGraphNode {
public:
    TriageNode() : SyncGraphNode("triage") {}
protected:
    NodeOutput run_sync(NodeInput in) override {
        const std::string text = in.state.get("text").get<std::string>();
        auto has = [&](const char* word) { return text.find(word) != std::string::npos; };
        const char* label = has("invoice") ? "billing" : has("crash") ? "technical" : "sales";
        return NodeOutput{{ChannelWrite{"__route__", json(label)}}};
    }
};

// A desk answers the request. It only needs a name to show which one was reached.
class DeskNode : public SyncGraphNode {
public:
    explicit DeskNode(std::string name) : SyncGraphNode(std::move(name)) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"answer", json("handled by " + get_name())}}};
    }
};

int main() {
    auto& factory = NodeFactory::instance();
    factory.register_type("triage", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<TriageNode>();
    });
    factory.register_type("desk", [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<DeskNode>(name);
    });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "conditional_edges"},
        {"channels", {
            {"text",      {{"reducer", "overwrite"}}},
            {"__route__", {{"reducer", "overwrite"}}},
            {"answer",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"triage",         {{"type", "triage"}}},
            {"billing_desk",   {{"type", "desk"}}},
            {"technical_desk", {{"type", "desk"}}},
            {"general_desk",   {{"type", "desk"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "triage"}},
            {{"from", "triage"}, {"condition", "route_channel"}, {"routes", {
                {"billing",   "billing_desk"},
                {"technical", "technical_desk"},
                {"default",   "general_desk"}      // any other label
            }}},
            {{"from", "billing_desk"}, {"to", "__end__"}},
            {{"from", "technical_desk"}, {"to", "__end__"}},
            {{"from", "general_desk"}, {"to", "__end__"}}
        })}
    };
    auto engine = GraphEngine::build(definition, EngineConfig{});

    for (const char* text : {"my invoice is wrong", "the app crashes on start", "can I get a quote?"}) {
        RunConfig run;
        run.input = {{"text", text}};
        const auto result = engine->run(run);
        std::cout << "\"" << text << "\" -> label " << result.channel<std::string>("__route__")
                  << " -> " << result.execution_trace.back() << "\n";
    }
    return 0;
}
