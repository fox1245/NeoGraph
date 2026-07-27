// NeoGraph Example 61: synchronous custom node
//
// SyncGraphNode removes coroutine boilerplate for short, non-blocking work.
// Blocking I/O should still use GraphNode::run() and co_await.

#include <neograph/neograph.h>

#include <cctype>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

const ChannelKey<std::string> text_channel{"text"};

class UpperNode : public SyncGraphNode {
public:
    explicit UpperNode(std::string name) : SyncGraphNode(std::move(name)) {}

protected:
    NodeOutput run_sync(NodeInput in) override {
        std::string text = in.state.get(text_channel);
        for (auto& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return NodeOutput{{ChannelWrite{"text", json(text)}}};
    }
};

int main() {
    NodeFactory::instance().register_type(
        "upper", [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>(name);
        });

    json def = {
        {"name", "sync_node"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(
        def, EngineConfig{.node_context = NodeContext{}});
    RunConfig config;
    config.input = {{"text", "hello"}};

    std::cout << engine->run(config).channel(text_channel) << '\n';
    return 0;
}
