// example_a2a_client — drive a remote A2A (Agent-to-Agent) agent.
//
// Two parts:
//   1. Standalone use of A2AClient — discover the agent's card, send a
//      message, print the response.
//   2. A2ACallerNode wired into a NeoGraph — the remote agent becomes
//      an executable node in the graph, callable via a node "type"
//      registered with NodeFactory.
//
// Usage:
//   ./build-pybind/example_a2a_client [base-url] [prompt]
//
// Defaults: base-url=http://127.0.0.1:8080, prompt="Say hi from NeoGraph"
// Start an A2A server first (e.g. a2a-python's `agent_helloworld.py`)
// so AgentCard discovery succeeds; otherwise the example reports the
// connect failure and exits 0 (env-skip semantics).

#include <neograph/neograph.h>
#include <neograph/a2a/client.h>
#include <neograph/a2a/a2a_caller_node.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using neograph::graph::NodeFactory;
using neograph::graph::GraphEngine;
using neograph::graph::NodeContext;
using neograph::graph::RunConfig;
using namespace neograph::a2a;

namespace {

void print_card(const AgentCard& c) {
    std::cout << "=== AgentCard ===\n"
              << "  name:                 " << c.name             << "\n"
              << "  description:          " << c.description      << "\n"
              << "  url:                  " << c.url              << "\n"
              << "  protocolVersion:      " << c.protocol_version << "\n"
              << "  preferredTransport:   " << c.preferred_transport << "\n"
              << "  capabilities.streaming: " << (c.streaming ? "yes" : "no") << "\n"
              << "  skills:               " << c.skill_names.size() << "\n";
    for (auto& s : c.skill_names) std::cout << "    - " << s << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string base_url = (argc >= 2) ? argv[1] : "http://127.0.0.1:8080";
    std::string prompt   = (argc >= 3) ? argv[2] : "Say hi from NeoGraph.";

    std::cout << "[*] Connecting to A2A agent at " << base_url << "\n\n";

    auto client = std::make_shared<A2AClient>(base_url);
    // Tight timeout — when no demo server is running we want fast skip,
    // not a 30 s hang.
    client->set_timeout(std::chrono::seconds(5));

    // Phase 1 — discovery.
    AgentCard card;
    try {
        card = client->fetch_agent_card();
        print_card(card);
    } catch (const std::exception& e) {
        std::cerr << "Skipping (AgentCard discovery failed): " << e.what() << "\n";
        std::cerr << "  hint: start an A2A server on " << base_url << " first\n";
        return 0;  // env-skip per run_all_cpp_examples.sh classifier
    }

    // Phase 2 — direct send.
    std::cout << "\n[*] Sending message: " << prompt << "\n";
    Task task;
    try {
        task = client->send_message_sync(prompt);
    } catch (const std::exception& e) {
        std::cerr << "send_message failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n=== Response ===\n"
              << "  taskId:    " << task.id           << "\n"
              << "  contextId: " << task.context_id   << "\n"
              << "  state:     " << task_state_to_string(task.status.state) << "\n";
    for (auto& m : task.history) {
        if (m.role == Role::Agent) {
            for (auto& p : m.parts) {
                if (p.kind == "text") std::cout << "  agent text: " << p.text << "\n";
            }
        }
    }
    for (auto& a : task.artifacts) {
        for (auto& p : a.parts) {
            if (p.kind == "text") std::cout << "  artifact: " << p.text << "\n";
        }
    }

    // Phase 3 — embed the agent in a NeoGraph node. The remote agent
    // becomes an addressable node "remote_agent" with reads from
    // "prompt" and writes to "reply".
    std::cout << "\n[*] Building a graph that delegates to the remote agent\n";

    NodeFactory::instance().register_type(
        "a2a_caller",
        [client](const std::string& name, const neograph::json&, const NodeContext&) {
            return std::make_unique<A2ACallerNode>(
                name, client, /*input_key=*/"prompt", /*output_key=*/"reply");
        });

    neograph::json definition = {
        {"name", "a2a_demo"},
        {"channels", {
            {"prompt",            {{"reducer", "overwrite"}}},
            {"reply",             {{"reducer", "overwrite"}}},
            {"reply_task_id",     {{"reducer", "overwrite"}}},
            {"reply_context_id",  {{"reducer", "overwrite"}}},
        }},
        {"nodes", {
            {"remote_agent", {{"type", "a2a_caller"}}},
        }},
        {"edges", neograph::json::array({
            neograph::json{{"from", "__start__"},    {"to", "remote_agent"}},
            neograph::json{{"from", "remote_agent"}, {"to", "__end__"}},
        })},
    };

    NodeContext ctx;  // remote agent doesn't need a Provider/Tool
    auto engine = GraphEngine::compile(definition, ctx);

    RunConfig cfg;
    cfg.thread_id     = "a2a-graph-1";
    cfg.input["prompt"] = prompt + " (via NeoGraph node)";

    auto res = engine->run(cfg);
    std::cout << "  graph reply:         "
              << res.output.value("reply", std::string("(empty)")) << "\n"
              << "  graph reply_task_id: "
              << res.output.value("reply_task_id", std::string("(none)")) << "\n";

    return 0;
}
