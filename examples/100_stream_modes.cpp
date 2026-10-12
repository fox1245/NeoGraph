// NeoGraph Example 100: stream modes
//
// run_stream() calls you back while the graph runs. RunConfig::stream_mode is a bit mask
// that decides which events arrive. This example runs the same two-node graph once per
// mode and counts what each mode delivers:
//
//   VALUES   a full state snapshot after every super-step
//   UPDATES  one event per channel write
//   EVENTS   NODE_START and NODE_END for every node
//   DEBUG    the engine's routing decisions (which nodes run next); one per hop between nodes
//
// Modes combine with `|`. TOKENS (LLM tokens, needs a model) and ALL are not shown here.
//
// No API key required.
//
// Usage: ./example_stream_modes
//
// smoke-expect-line: VALUES   node_events=0 snapshots=2 updates=0 routing=0
// smoke-expect-line: UPDATES  node_events=0 snapshots=0 updates=3 routing=0
// smoke-expect-line: EVENTS   node_events=4 snapshots=0 updates=0 routing=0
// smoke-expect-line: DEBUG    node_events=0 snapshots=0 updates=0 routing=1
// smoke-expect-line: EVENTS|UPDATES node_events=4 snapshots=0 updates=3 routing=0
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <iostream>

using namespace neograph;
using namespace neograph::graph;

class PlanNode : public SyncGraphNode {
public:
    PlanNode() : SyncGraphNode("plan") {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"steps", json::array({"plan"})}}};   // 1 write
    }
};

class WorkNode : public SyncGraphNode {
public:
    WorkNode() : SyncGraphNode("work") {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"steps", json::array({"work"})},      // 2 writes
                           ChannelWrite{"total", json(42)}}};
    }
};

// What one run delivered, sorted by the kind of event.
struct Tally {
    int node_events = 0, snapshots = 0, updates = 0, routing = 0;
};

Tally observe(GraphEngine& engine, StreamMode mode) {
    Tally     tally;
    RunConfig config;
    config.stream_mode = mode;
    engine.run_stream(config, [&](const GraphEvent& event) {
        const bool engine_event = event.node_name == "__routing__" || event.node_name == "__state__";
        switch (event.type) {
            case GraphEvent::Type::NODE_START:
            case GraphEvent::Type::NODE_END:
                (event.node_name == "__routing__" ? tally.routing : tally.node_events)++;
                break;
            case GraphEvent::Type::CHANNEL_WRITE:
                (engine_event ? tally.snapshots : tally.updates)++;
                break;
            default: break;
        }
    });
    return tally;
}

void show(const char* label, const Tally& t) {
    std::cout << label << " node_events=" << t.node_events << " snapshots=" << t.snapshots
              << " updates=" << t.updates << " routing=" << t.routing << "\n";
}

int main() {
    auto& factory = NodeFactory::instance();
    factory.register_type("plan", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<PlanNode>();
    });
    factory.register_type("work", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<WorkNode>();
    });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "stream_modes"},
        {"channels", {{"steps", {{"reducer", "append"}}}, {"total", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"plan", {{"type", "plan"}}}, {"work", {{"type", "work"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "plan"}},
            {{"from", "plan"}, {"to", "work"}},
            {{"from", "work"}, {"to", "__end__"}}
        })}
    };
    auto engine = GraphEngine::build(definition, EngineConfig{});

    show("VALUES  ", observe(*engine, StreamMode::VALUES));
    show("UPDATES ", observe(*engine, StreamMode::UPDATES));
    show("EVENTS  ", observe(*engine, StreamMode::EVENTS));
    show("DEBUG   ", observe(*engine, StreamMode::DEBUG));
    show("EVENTS|UPDATES", observe(*engine, StreamMode::EVENTS | StreamMode::UPDATES));
    return 0;
}
