// NeoGraph Example 77: one A2A agent, two wire dialects
//
// A2A 1.0 is the primary version and 0.3 stays as a compatibility dialect with the same
// meaning. The server lists both as JSON-RPC interfaces in its agent card. A client does not
// guess: it reads the card and takes the first interface whose version it speaks (1.x or 0.x)
// and sends the matching `A2A-Version` header and method spelling. A client that meets an
// agent whose card lists only 0.3 follows it just as well.
//
// Two servers on free ports (port 0) run in this process: the first one with the default
// card (1.0 and 0.3), the second one whose card is restricted to 0.3.
//
// smoke-expect: card interfaces: JSONRPC 1.0, JSONRPC 0.3
// smoke-expect: dialect: 1.0
// smoke-expect: dialect: 0.3
// smoke-expect: reply: echo: hello
// smoke-forbid: [neograph] lint

#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>
#include <neograph/neograph.h>

#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

// The agent: answer "echo: <prompt>".
class EchoNode final : public SyncGraphNode {
public:
    EchoNode() : SyncGraphNode("echo") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto prompt = input.state.get(ChannelKey<std::string>{"prompt"});
        return NodeOutput{{ChannelWrite{"response", json("echo: " + prompt)}}};
    }
};

std::shared_ptr<GraphEngine> build_engine() {
    NodeFactory::instance().register_type("example.echo", [](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<EchoNode>();
    });
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "echo-agent"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"echo", {{"type", "example.echo"}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "echo"}}, {{"from", "echo"}, {"to", "__end__"}}}},
    };
    return std::shared_ptr<GraphEngine>(GraphEngine::build_strict(definition, EngineConfig{}));
}

// Fetch the card, call the agent once, and report what the client decided.
void call_agent(const std::string& label, int port) {
    a2a::A2AClient client("http://127.0.0.1:" + std::to_string(port));
    const auto card = client.fetch_agent_card();
    std::cout << label << ": card interfaces:";
    for (size_t i = 0; i < card.supported_interfaces.size(); ++i)
        std::cout << (i ? ", " : " ") << card.supported_interfaces[i].protocol_binding << " "
                  << card.supported_interfaces[i].protocol_version;
    std::cout << "\n";

    const auto task = client.send_message_sync("hello");
    const auto dialect = client.wire_dialect();
    std::cout << label << ": dialect: " << (dialect == a2a::WireDialect::V1_0 ? "1.0" : "0.3") << "\n";
    std::cout << label << ": reply: " << task.status.message->parts[0].text << "\n";
}

}  // namespace

int main() {
    try {
        auto engine = build_engine();

        // Server 1: the default card lists JSON-RPC 1.0 first and 0.3 second.
        a2a::AgentCard both;
        both.name = "echo-both";
        both.description = "Speaks A2A 1.0 and 0.3.";
        both.version = "1.0.0";
        a2a::A2AServer server_both(engine, both);

        // Server 2: the same agent, with a card that offers only 0.3.
        a2a::AgentCard legacy = both;
        legacy.name = "echo-legacy";
        legacy.description = "Advertises A2A 0.3 only.";
        legacy.supported_interfaces = {a2a::AgentInterface{"", "JSONRPC", "0.3", ""}};
        a2a::A2AServer server_legacy(engine, legacy);

        if (!server_both.start_async("127.0.0.1", 0) || !server_legacy.start_async("127.0.0.1", 0)) {
            std::cerr << "could not start the A2A servers\n";
            return 1;
        }
        call_agent("both", server_both.port());
        call_agent("legacy", server_legacy.port());
        server_legacy.stop();
        server_both.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
