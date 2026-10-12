// NeoGraph Example 61: SyncGraphNode, a short node without coroutines
//
// GraphNode::run() is a coroutine, which is what a node needs when it waits on I/O. A node
// that only computes does not need that: derive from SyncGraphNode and implement a plain
// run_sync(). Here two such nodes form a pipeline: `trim` removes the blanks around the
// text and `upper` puts it in capitals.
//
// Blocking I/O (a network call, a sleep) still belongs in GraphNode::run() with co_await,
// because a blocking run_sync() holds its worker thread until it returns.
//
// No API key required.
//
// Usage: ./example_sync_node
//
// smoke-expect-line: HELLO
// smoke-expect-line: nodes run: trim upper
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <algorithm>
#include <cctype>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

const ChannelKey<std::string> text_channel{"text"};

class TrimNode : public SyncGraphNode {
public:
    explicit TrimNode(std::string name) : SyncGraphNode(std::move(name)) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        std::string text = in.state.get(text_channel);
        auto is_blank    = [](unsigned char c) { return std::isspace(c) != 0; };
        text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), is_blank));
        text.erase(std::find_if_not(text.rbegin(), text.rend(), is_blank).base(), text.end());
        return NodeOutput{{ChannelWrite{"text", json(text)}}};
    }
};

class UpperNode : public SyncGraphNode {
public:
    explicit UpperNode(std::string name) : SyncGraphNode(std::move(name)) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        std::string text = in.state.get(text_channel);
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return NodeOutput{{ChannelWrite{"text", json(text)}}};
    }
};

int main() {
    auto& factory = NodeFactory::instance();
    factory.register_type("trim", [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<TrimNode>(name);
    });
    factory.register_type("upper", [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<UpperNode>(name);
    });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "sync_node"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"trim", {{"type", "trim"}}}, {"upper", {{"type", "upper"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "trim"}},
            {{"from", "trim"}, {"to", "upper"}},
            {{"from", "upper"}, {"to", "__end__"}}
        })}
    };
    auto engine = GraphEngine::build_strict(definition, EngineConfig{});

    RunConfig config;
    config.input = {{"text", "  hello  "}};
    const auto result = engine->run(config);

    std::cout << result.channel(text_channel) << "\n";
    std::cout << "nodes run:";
    for (const auto& node : result.execution_trace) std::cout << " " << node;
    std::cout << "\n";
    return 0;
}
