// NeoGraph Example 51: channels, the state a graph runs on
//
// No model, no tool, no key: one node and two channels. A channel is a named slot of state;
// the node reads one, writes both, and the caller reads both back with typed keys.
//
// Steps:
//   1) describe the graph as JSON: two channels, one node, two edges
//   2) register the node type under the name the JSON uses
//   3) build, run, and read the result with `result.channel(key)`
//
// A ChannelKey<T> ties a channel name to its C++ type, so a read is checked where it is
// written. `channel<T>(name)` is the untyped form; `try_channel` returns nullopt for a channel
// the graph does not have, where `channel` would throw.
//
// Usage: ./example_minimal     prints: HELLO, length: 5, missing: not in the graph
//
// smoke-expect-line: HELLO
// smoke-expect-line: length: 5
// smoke-expect-line: missing: not in the graph
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <cctype>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

const ChannelKey<std::string> text_key{"text"};
const ChannelKey<int>         length_key{"length"};
const ChannelKey<int>         missing_key{"nothing_here"};

// Reads `text`, writes it back in capitals and writes its length to `length`.
class UpperNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        std::string text = in.state.get(text_key);
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"length", json(static_cast<int>(text.size()))});
        out.writes.push_back(ChannelWrite{"text", json(std::move(text))});
        co_return out;
    }
    std::string get_name() const override { return "upper"; }
};

int main() {
    // (2) A node type is a name plus a function that makes the node.
    NodeFactory::instance().register_type("upper",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        });

    // (1) Two channels (overwrite: a write replaces the value), one node, two edges.
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "minimal"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}, {"length", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "upper"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "upper"}},
            {{"from", "upper"}, {"to", "__end__"}}
        })}
    };

    // (3) Build, run with an input for `text`, read the channels back.
    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);

    std::cout << result.channel(text_key) << "\n";
    std::cout << "length: " << result.channel(length_key) << "\n";
    std::cout << "missing: " << (result.try_channel(missing_key) ? "found" : "not in the graph") << "\n";
    return 0;
}
