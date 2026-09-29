// fork() copies exactly one checkpoint into a new thread and resume()
// continues from that checkpoint's pending nodes. Forking a thread whose
// latest checkpoint is terminal therefore yields a branch with nothing left
// to run, while forking the checkpoint the run paused at yields a branch that
// really executes against the edited conversation (examples/08).

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

class CityProvider : public Provider {
public:
    ChatCompletion complete(const CompletionParams& params) override {
        ChatCompletion result;
        result.message.role = "assistant";
        std::string last_user;
        for (auto it = params.messages.rbegin(); it != params.messages.rend(); ++it) {
            if (it->role == "user") { last_user = it->content; break; }
        }
        result.message.content = "answer:" + last_user;
        return result;
    }
    ChatCompletion complete_stream(const CompletionParams& p, const StreamCallback&) override {
        return complete(p);
    }
    std::string get_name() const override { return "fork_city"; }
};

struct Fixture {
    std::shared_ptr<InMemoryCheckpointStore> store =
        std::make_shared<InMemoryCheckpointStore>();
    std::unique_ptr<GraphEngine> engine;

    Fixture() {
        json definition = {
            {"schema_version", TOPOLOGY_SCHEMA_VERSION},
            {"name", "fork_semantics"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"llm", {{"type", "llm_call"}}},
                       {"reviewer", {{"type", "llm_call"}}}}},
            {"edges", json::array({
                {{"from", "__start__"}, {"to", "llm"}},
                {{"from", "llm"}, {"to", "reviewer"}},
                {{"from", "reviewer"}, {"to", "__end__"}}})},
            {"interrupt_before", json::array({"reviewer"})}};
        NodeContext ctx;
        ctx.provider = std::make_shared<CityProvider>();
        engine = GraphEngine::build(
            definition, EngineConfig{.node_context = ctx, .checkpoint_store = store});

        RunConfig cfg;
        cfg.thread_id = "src";
        cfg.input = {{"messages", json::array({{{"role", "user"}, {"content", "Seoul"}}})}};
        EXPECT_TRUE(engine->run(cfg).interrupted);
        engine->resume("src");  // run to __end__: latest checkpoint is now terminal
    }

    static json last_message(const std::optional<json>& state) {
        return (*state)["channels"]["messages"]["value"].back();
    }
};

const Checkpoint* pause_checkpoint(const std::vector<Checkpoint>& history) {
    for (const auto& cp : history)
        if (cp.interrupt_phase == CheckpointPhase::Before && !cp.next_nodes.empty()) return &cp;
    return nullptr;
}

}  // namespace

TEST(ForkResumeSemantics, ForkOfTerminalCheckpointHasNothingToExecute) {
    Fixture f;
    f.engine->fork("src", "terminal-fork");
    f.engine->update_state("terminal-fork",
        {{"messages", json::array({{{"role", "user"}, {"content", "Tokyo"}}})}});

    auto result = f.engine->resume("terminal-fork");
    EXPECT_TRUE(result.execution_trace.empty());
    // The unanswered question is still the last message.
    EXPECT_EQ(Fixture::last_message(f.engine->get_state("terminal-fork")).value("content", ""),
              "Tokyo");
}

TEST(ForkResumeSemantics, ForkOfPausedCheckpointExecutesPendingNodeOnEditedState) {
    Fixture f;
    const auto history = f.engine->get_state_history("src");
    const Checkpoint* pause = pause_checkpoint(history);
    ASSERT_NE(pause, nullptr);
    ASSERT_EQ(pause->next_nodes, std::vector<std::string>{"reviewer"});

    f.engine->fork("src", "paused-fork", pause->id);
    f.engine->update_state("paused-fork",
        {{"messages", json::array({{{"role", "user"}, {"content", "Tokyo"}}})}});

    auto result = f.engine->resume("paused-fork");
    EXPECT_EQ(result.execution_trace, std::vector<std::string>{"reviewer"});
    auto last = Fixture::last_message(f.engine->get_state("paused-fork"));
    EXPECT_EQ(last.value("role", ""), "assistant");
    EXPECT_EQ(last.value("content", ""), "answer:Tokyo");

    // The source thread is untouched by the branch.
    EXPECT_EQ(Fixture::last_message(f.engine->get_state("src")).value("content", ""),
              "answer:Seoul");
}
