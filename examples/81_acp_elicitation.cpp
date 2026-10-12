// NeoGraph Example 81: ask the user a question in the middle of a turn
//
// ACP lets an agent request structured input: `elicitation/create` carries a message and a small
// JSON schema, and the editor shows a form. The agent may only send it when the client declared
// the `elicitation` capability in `initialize`. A node asks with ACPServer::call_client; with
// the capability the editor's answer reaches the node, without it the call throws before
// anything is sent, and the node falls back to a default. The example runs the same agent
// against two scripted editors (scripted_editor.h), one with the capability and one without.
//
// smoke-expect: elicitation asked: Which environment should I deploy to?
// smoke-expect: <- update agent_message_chunk: deploying to production
// smoke-expect: <- update agent_message_chunk: deploying to staging (default: the client cannot be asked)
// smoke-forbid: [neograph] lint

#include "scripted_editor.h"

#include <neograph/acp/server.h>
#include <neograph/neograph.h>

#include <iostream>
#include <memory>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

class AskNode final : public GraphNode {
public:
    explicit AskNode(std::shared_ptr<acp::ACPServer*> server) : server_(std::move(server)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const json request = {
            {"sessionId", input.state.get(ChannelKey<std::string>{"_acp_session_id"})},
            {"mode", "form"},
            {"message", "Which environment should I deploy to?"},
            {"requestedSchema",
             {{"type", "object"},
              {"properties", {{"environment", {{"type", "string"}, {"enum", json::array({"staging", "production"})}}}}},
              {"required", json::array({"environment"})}}},
        };
        std::string text;
        try {
            // Throws std::runtime_error when the client did not declare the capability.
            const auto answer = (*server_)->call_client("elicitation/create", request);
            const auto action = answer.value("action", std::string());
            text = action == "accept" ? "deploying to " + answer.at("content").at("environment").get<std::string>()
                                      : "nothing deployed (" + action + ")";
        } catch (const std::runtime_error&) {
            text = "deploying to staging (default: the client cannot be asked)";
        }
        co_return NodeOutput{{ChannelWrite{"response", json(text)}}};
    }
    std::string get_name() const override { return "ask"; }

private:
    std::shared_ptr<acp::ACPServer*> server_;  // set once the server exists
};

// One agent, one scripted editor with the given capabilities, one turn.
void run_turn(const std::string& label, const json& capabilities) {
    auto server_slot = std::make_shared<acp::ACPServer*>(nullptr);
    const std::string type = "example.ask." + label;
    NodeFactory::instance().register_type(type, [server_slot](const std::string&, const json&, const NodeContext&) {
        return std::make_unique<AskNode>(server_slot);
    });
    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "deploy"},
        {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"response", {{"reducer", "overwrite"}, {"initial", ""}}},
                      {"_acp_session_id", {{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"ask", {{"type", type}}}}},
        {"edges", {{{"from", "__start__"}, {"to", "ask"}}, {{"from", "ask"}, {"to", "__end__"}}}},
    };
    std::shared_ptr<GraphEngine> engine(GraphEngine::build_strict(definition, EngineConfig{}));
    acp::ACPServer server(engine, json{{"name", "deploy-agent"}, {"version", "1.0.0"}});
    *server_slot = &server;

    // The user's answer to the form.
    examples::ScriptedEditor editor(server, capabilities, [](const json& request) {
        std::cout << "elicitation asked: " << request.at("params").at("message").get<std::string>() << "\n";
        return json{{"action", "accept"}, {"content", {{"environment", "production"}}}};
    });
    std::cout << label << ":\n";
    editor.prompt(editor.new_session(), "deploy");
}

}  // namespace

int main() {
    try {
        run_turn("with form support", json{{"elicitation", {{"form", json::object()}}}});
        run_turn("without it", json::object());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
