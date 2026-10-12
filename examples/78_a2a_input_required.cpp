// NeoGraph Example 78: a paused graph is an A2A task that waits for input
//
// A graph that stops at interrupt_before becomes an A2A task in the `input-required` state. The
// stream closes there. The client reads the question from the status message, answers by
// sending a second message with the same task id, and the server resumes the run from its
// checkpoint. The text of the second message is the value the paused node sees.
//
// Server and client share one process: the server listens on a free port (port 0).
//
// smoke-expect: states: submitted > working > input-required
// smoke-expect: question: Interrupt before node: publish
// smoke-expect: states: working > completed
// smoke-expect: result: Published: Release notes: version 2.0
// smoke-forbid: [neograph] lint

#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>
#include <neograph/neograph.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

// draft: write a draft from the prompt.
class DraftNode final : public SyncGraphNode {
public:
    DraftNode() : SyncGraphNode("draft") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto topic = input.state.get(ChannelKey<std::string>{"prompt"});
        return NodeOutput{{ChannelWrite{"draft", json("Release notes: " + topic)}}};
    }
};

// publish: runs only after a person answered. The answer is the resume value.
class PublishNode final : public SyncGraphNode {
public:
    PublishNode() : SyncGraphNode("publish") {}

protected:
    NodeOutput run_sync(NodeInput input) override {
        const auto draft = input.state.get(ChannelKey<std::string>{"draft"});
        const auto answer = input.ctx.resume_value ? input.ctx.resume_value->get<std::string>() : "";
        const bool approved = answer.rfind("approve", 0) == 0;
        return NodeOutput{{ChannelWrite{
            "response", json(approved ? "Published: " + draft : "Not published (" + answer + "): " + draft)}}};
    }
};

// Promote the "response" channel to a typed A2A artifact, so the client receives the result as
// an artifact of the task rather than only as status text.
class ResultArtifactAdapter final : public a2a::GraphAgentAdapter {
public:
    std::optional<a2a::Artifact> build_output_artifact(const json& output,
                                                       const std::string& task_id) const override {
        a2a::Artifact artifact;
        artifact.artifact_id = task_id + "-result";
        artifact.name = "result";
        artifact.parts = {a2a::Part::text_part(output["channels"]["response"]["value"].get<std::string>())};
        return artifact;
    }
};

// Send one message, print the state sequence the stream delivered, return the task.
a2a::Task send(a2a::A2AClient& client, const std::string& text, const std::string& task_id = "") {
    std::vector<std::string> states;
    auto note = [&states](a2a::TaskState state) {
        const auto name = a2a::task_state_to_string(state);
        if (states.empty() || states.back() != name) states.push_back(name);
    };
    const auto task = client.send_message_stream(text, [&](const a2a::StreamEvent& event) {
        if (event.task) note(event.task->status.state);
        if (event.status_update) note(event.status_update->status.state);
        return true;
    }, task_id);
    std::cout << "states:";
    for (size_t i = 0; i < states.size(); ++i) std::cout << (i ? " > " : " ") << states[i];
    std::cout << "\n";
    return task;
}

}  // namespace

int main() {
    try {
        NodeFactory::instance().register_type("example.draft", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<DraftNode>();
        });
        NodeFactory::instance().register_type("example.publish", [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<PublishNode>();
        });
        const json definition = {
            {"schema_version", TOPOLOGY_SCHEMA_VERSION},
            {"name", "release-notes"},
            {"channels", {{"prompt", {{"reducer", "overwrite"}, {"initial", ""}}},
                          {"draft", {{"reducer", "overwrite"}, {"initial", ""}}},
                          {"response", {{"reducer", "overwrite"}, {"initial", ""}}}}},
            {"nodes", {{"draft", {{"type", "example.draft"}}}, {"publish", {{"type", "example.publish"}}}}},
            {"edges", {{{"from", "__start__"}, {"to", "draft"}},
                       {{"from", "draft"}, {"to", "publish"}},
                       {{"from", "publish"}, {"to", "__end__"}}}},
            {"interrupt_before", json::array({"publish"})},
        };
        // The checkpoint store is what lets the task wait and resume.
        EngineConfig config;
        config.checkpoint_store = std::make_shared<InMemoryCheckpointStore>();
        std::shared_ptr<GraphEngine> engine = GraphEngine::build_strict(definition, std::move(config));

        a2a::AgentCard card;
        card.name = "release-notes";
        card.description = "Drafts release notes and publishes them after approval.";
        card.version = "1.0.0";
        card.default_input_modes = {"text/plain"};
        card.default_output_modes = {"text/plain"};
        a2a::A2AServer server(engine, card, std::make_shared<ResultArtifactAdapter>());
        if (!server.start_async("127.0.0.1", 0)) {
            std::cerr << "could not start the A2A server\n";
            return 1;
        }
        a2a::A2AClient client("http://127.0.0.1:" + std::to_string(server.port()));

        // Message 1: the run stops before "publish" and the task asks for input.
        auto task = send(client, "version 2.0");
        std::cout << "question: " << task.status.message->parts[0].text << "\n";

        // Message 2: the answer goes to the same task and resumes the run.
        task = send(client, "approved", task.id);
        std::cout << "result: " << task.artifacts.at(0).parts.at(0).text << "\n";
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
