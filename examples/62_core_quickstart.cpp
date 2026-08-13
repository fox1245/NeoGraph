// NeoGraph Core quickstart.
//
// Build with the installed `neograph::core` target, define one node, compile a
// strict topology, run it, and read a typed channel.  This deliberately uses
// no LLM, MCP, Program, or transport component.
//
// Usage: ./example_core_quickstart     prints: HELLO

#include <neograph/neograph.h>

#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

const ChannelKey<std::string> text_channel{"text"};

class UpperNode final : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get(text_channel);
        for (auto& character : text)
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        co_return NodeOutput{{ChannelWrite{"text", json(std::move(text))}}};
    }

    std::string get_name() const override { return "upper"; }
};

}  // namespace

int main() {
    NodeFactory::instance().register_type(
        "quickstart.upper", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>();
        },
        json::object(),
        json{{"reads", json::array({"text"})},
             {"writes", json::array({"text"})},
             {"exports", json::array({"text"})}});

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "core-quickstart"},
        {"channels", {{"text", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"upper", {{"type", "quickstart.upper"}}}}},
        {"edges",
         {{{"from", "__start__"}, {"to", "upper"}},
          {{"from", "upper"}, {"to", "__end__"}}}},
    };

    auto engine = GraphEngine::build_strict(definition, EngineConfig{});
    RunConfig config;
    config.input = {{"text", "hello"}};
    const auto result = engine->run(config);
    std::cout << result.channel(text_channel) << '\n';
    return 0;
}
