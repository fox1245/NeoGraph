// NeoGraph Example 80: an ACP agent that asks before it deletes
//
// Before a node does something the user may not want, it asks the editor: ACPClient sends
// session/request_permission with a tool call and a list of options, and blocks until the
// editor answers. The editor shows the options; the one the user picks comes back as the
// outcome, or the outcome is `cancelled` when the turn is cancelled. Here a scripted editor
// (scripted_editor.h) plays the user: it allows the first deletion and rejects the second, so
// one turn deletes a file and the next one does not. Everything runs in this process.
//
// smoke-expect: permission asked: Delete draft.txt (allow-once, reject-once)
// smoke-expect: <- update agent_message_chunk: deleted draft.txt
// smoke-expect: permission asked: Delete notes.txt (allow-once, reject-once)
// smoke-expect: <- update agent_message_chunk: kept notes.txt (permission rejected)
// smoke-expect: turn 1 ended: end_turn
// smoke-expect: turn 2 ended: end_turn
// smoke-expect: files deleted: 1
// smoke-forbid: [neograph] lint

#include "scripted_editor.h"

#include <neograph/acp/server.h>
#include <neograph/neograph.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::atomic<int> files_deleted{0};

// The risky step: ask the editor, delete only when the user allowed it.
class DeleteNode final : public GraphNode {
public:
    explicit DeleteNode(std::shared_ptr<acp::ACPClient> client) : client_(std::move(client)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto path = input.state.get(ChannelKey<std::string>{"prompt"});
        const auto session = input.state.get(ChannelKey<std::string>{"_acp_session_id"});

        acp::ToolCallUpdate call;
        call.tool_call_id = "delete-1";
        call.title = "Delete " + path;
        call.kind = "delete";
        call.status = "pending";
        const std::vector<acp::PermissionOption> options = {
            {"allow-once", "Allow", "allow_once"}, {"reject-once", "Reject", "reject_once"}};

        // Blocks until the editor answers or the turn is cancelled.
        const auto outcome = client_->request_permission(session, call, options);
        const bool allowed = outcome.kind == acp::PermissionOutcomeKind::Selected &&
                             outcome.option_id == "allow-once";
        if (allowed) ++files_deleted;  // stands in for the real deletion
        co_return NodeOutput{{ChannelWrite{
            "response", json(allowed ? "deleted " + path : "kept " + path + " (permission rejected)")}}};
    }
    std::string get_name() const override { return "delete"; }

private:
    std::shared_ptr<acp::ACPClient> client_;
};

}  // namespace

int main() {
    try {
        auto client = std::make_shared<acp::ACPClient>();  // bound to the server below
        NodeFactory::instance().register_type("example.delete",
            [client](const std::string&, const json&, const NodeContext&) {
                return std::make_unique<DeleteNode>(client);
            });
        const json definition = {
            {"schema_version", TOPOLOGY_SCHEMA_VERSION},
            {"name", "cleanup"},
            {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                          {"response", {{"reducer", "overwrite"}, {"initial", ""}}},
                          {"_acp_session_id", {{"reducer", "overwrite"}, {"initial", ""}}}}},
            {"nodes", {{"delete", {{"type", "example.delete"}}}}},
            {"edges", {{{"from", "__start__"}, {"to", "delete"}}, {{"from", "delete"}, {"to", "__end__"}}}},
        };
        std::shared_ptr<GraphEngine> engine(GraphEngine::build_strict(definition, EngineConfig{}));

        acp::ACPServer server(engine, json{{"name", "cleanup-agent"}, {"version", "1.0.0"}});
        server.attach_client(client);

        // The user's choices: allow the first request, reject the second.
        int asked = 0;
        examples::ScriptedEditor editor(server, json::object(), [&asked](const json& request) {
            const auto& params = request.at("params");
            std::cout << "permission asked: " << params.at("toolCall").at("title").get<std::string>() << " (";
            for (size_t i = 0; i < params.at("options").size(); ++i)
                std::cout << (i ? ", " : "") << params["options"][i].at("optionId").get<std::string>();
            std::cout << ")\n";
            const char* choice = asked++ == 0 ? "allow-once" : "reject-once";
            return json{{"outcome", {{"outcome", "selected"}, {"optionId", choice}}}};
        });

        const auto session = editor.new_session();
        for (const char* file : {"draft.txt", "notes.txt"}) {
            static int turn = 0;
            const auto done = editor.prompt(session, file);
            std::cout << "turn " << ++turn << " ended: " << done.at("result").at("stopReason").get<std::string>() << "\n";
        }
        std::cout << "files deleted: " << files_deleted << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
