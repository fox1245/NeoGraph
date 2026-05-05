// example_a2a_server — expose a NeoGraph as an Agent-to-Agent endpoint.
//
// Builds a tiny ReAct-style graph (mock provider, no API key needed)
// and serves it over A2A on 127.0.0.1:8087. Then connects a local
// A2AClient to its own server and round-trips a message — proving
// the loop works end-to-end without external deps.
//
// Real deployment: pass the same engine you use for direct in-process
// calls into A2AServer; the agent is reachable from any A2A client
// (a2a-js, a2a-python, our own A2AClient, etc.) at the bound URL.
//
// Usage:
//   ./build-pybind/example_a2a_server [port]    (default 8087, 0=auto)

#include <neograph/neograph.h>
#include <neograph/a2a/server.h>
#include <neograph/a2a/client.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using neograph::graph::ChannelWrite;
using neograph::graph::GraphEngine;
using neograph::graph::GraphNode;
using neograph::graph::GraphState;
using neograph::graph::NodeContext;
using neograph::graph::NodeFactory;
using neograph::graph::NodeInput;
using neograph::graph::NodeOutput;
using namespace neograph::a2a;

namespace {

// Trivial deterministic agent — uppercases the prompt and prepends a tag.
class UppercaseNode : public GraphNode {
  public:
    explicit UppercaseNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto raw = in.state.get("prompt");
        std::string p = raw.is_string() ? raw.get<std::string>() : raw.dump();
        std::string upper = "[neo-agent] ";
        for (char c : p)
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        NodeOutput out;
        out.writes.push_back({"response", std::move(upper)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
  private:
    std::string name_;
};

std::shared_ptr<GraphEngine> build_demo_engine() {
    NodeFactory::instance().register_type("uppercase",
        [](const std::string& n, const neograph::json&, const NodeContext&) {
            return std::make_unique<UppercaseNode>(n);
        });
    neograph::json def = {
        {"name", "uppercase-agent"},
        {"channels", {
            {"prompt",   {{"reducer", "overwrite"}}},
            {"response", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"uppercase", {{"type", "uppercase"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"}, {"to", "uppercase"}},
            neograph::json{{"from", "uppercase"}, {"to", "__end__"}},
        })},
    };
    NodeContext ctx;
    auto engine = GraphEngine::compile(def, ctx);
    return std::shared_ptr<GraphEngine>(std::move(engine));
}

}  // namespace

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 8087;

    auto engine = build_demo_engine();

    AgentCard card;
    card.name             = "neo-uppercase-agent";
    card.description      = "Demo NeoGraph agent that uppercases its input.";
    card.version          = "0.1.0";
    card.protocol_version = "0.3.0";
    card.preferred_transport = "JSONRPC";
    card.default_input_modes  = {"text/plain"};
    card.default_output_modes = {"text/plain"};
    card.skill_names = {"uppercase"};

    A2AServer server(engine, card);
    if (!server.start_async("127.0.0.1", port)) {
        std::cerr << "[!] failed to bind on 127.0.0.1:" << port << "\n";
        return 1;
    }
    int bound = server.port();
    std::string url = "http://127.0.0.1:" + std::to_string(bound);
    std::cout << "[*] A2A server listening at " << url << "\n";
    std::cout << "    GET  " << url << "/.well-known/agent-card.json\n";
    std::cout << "    POST " << url << "/   (JSON-RPC: message/send, message/stream, tasks/get, tasks/cancel)\n";

    // Drive our own server with our own client to prove the loop works.
    A2AClient client(url);
    client.set_timeout(std::chrono::seconds(5));

    auto fetched_card = client.fetch_agent_card();
    std::cout << "\n[*] Discovery OK — agent name: " << fetched_card.name << "\n";

    auto task = client.send_message_sync("hello from main()");
    std::cout << "[*] message/send round-trip:\n";
    std::cout << "    state:    " << task_state_to_string(task.status.state) << "\n";
    if (!task.history.empty() && !task.history.back().parts.empty()) {
        std::cout << "    response: " << task.history.back().parts[0].text << "\n";
    }

    std::cout << "\n[*] message/stream — collecting SSE frames...\n";
    int events_seen = 0;
    auto streamed = client.send_message_stream(
        "stream me",
        [&](const StreamEvent& ev) {
            ++events_seen;
            if (ev.type == StreamEvent::Type::StatusUpdate && ev.status_update) {
                std::cout << "    [event " << events_seen << "] status="
                          << task_state_to_string(ev.status_update->status.state)
                          << (ev.status_update->final ? " (final)" : "") << "\n";
            } else if (ev.type == StreamEvent::Type::Task) {
                std::cout << "    [event " << events_seen << "] terminal task\n";
            }
            return true;
        });
    std::cout << "    streamed task state: "
              << task_state_to_string(streamed.status.state) << "\n";

    // Server keeps running until we tear it down — for an actual
    // deployment, replace this with a wait-for-signal loop.
    server.stop();
    std::cout << "\n[*] server stopped, exiting cleanly\n";
    return 0;
}
