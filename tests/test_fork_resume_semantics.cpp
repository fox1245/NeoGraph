#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>
#include "fixtures/typed_provider.h"

using namespace neograph;
using namespace neograph::graph;
namespace {
struct Fixture {
    std::shared_ptr<InMemoryCheckpointStore> store = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<int> calls = std::make_shared<int>(0);
    std::unique_ptr<GraphEngine> engine;
    Fixture() {
        auto provider = std::make_shared<test::LocalProvider>([count = calls](ProviderRequest request, const auto&, const auto&)
            -> asio::awaitable<sp::runtime::Result> {
            ++*count;
            const auto& messages = std::get<sp::chat::Request>(request.payload).canonical_messages;
            std::string last_user;
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role != sp::Role::User) continue;
                for (const auto& part : it->parts)
                    if (const auto* text = std::get_if<sp::Text>(&part)) last_user += text->value;
                break;
            }
            co_return test::success("answer:" + last_user);
        });
        const json definition{
            {"schema_version", TOPOLOGY_SCHEMA_VERSION}, {"name", "fork_semantics"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"llm", {{"type", "llm_call"}}}, {"reviewer", {{"type", "llm_call"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "llm"}},
                {{"from", "llm"}, {"to", "reviewer"}}, {{"from", "reviewer"}, {"to", "__end__"}}})},
            {"interrupt_before", json::array({"reviewer"})}};
        NodeContext context;
        context.provider = std::move(provider);
        context.model = "test-model";
        engine = GraphEngine::build(definition, EngineConfig{.node_context = context, .checkpoint_store = store});
        RunConfig config;
        config.thread_id = "source";
        config.input = {{"messages", json::array({{{"role", "user"}, {"content", "Seoul"}}})}};
        EXPECT_TRUE(engine->run(config).interrupted);
        engine->resume("source");
    }
    json last_message(const std::string& thread) const {
        const auto state = engine->get_state(thread);
        if (!state) throw std::logic_error("missing fork state");
        return state->at("channels").at("messages").at("value").back();
    }
};
}

TEST(ForkResumeSemantics, TerminalForkDoesNotScheduleEditedUserTurn) {
    Fixture fixture;
    ASSERT_EQ(*fixture.calls, 2);
    fixture.engine->fork("source", "terminal-fork");
    fixture.engine->update_state("terminal-fork", {{"messages", json::array({{{"role", "user"}, {"content", "Tokyo"}}})}});
    const auto result = fixture.engine->resume("terminal-fork");
    EXPECT_TRUE(result.execution_trace.empty());
    EXPECT_EQ(*fixture.calls, 2);
    EXPECT_EQ(fixture.last_message("terminal-fork").value("role", ""), "user");
    EXPECT_EQ(fixture.last_message("terminal-fork").value("content", ""), "Tokyo");
    EXPECT_EQ(fixture.last_message("source").value("content", ""), "answer:Seoul");
}

TEST(ForkResumeSemantics, PausedExactCheckpointExecutesPendingNodeOnEditedState) {
    Fixture fixture;
    const auto history = fixture.engine->get_state_history("source");
    const Checkpoint* paused = nullptr;
    for (const auto& checkpoint : history)
        if (checkpoint.interrupt_phase == CheckpointPhase::Before && !checkpoint.next_nodes.empty()) {
            paused = &checkpoint;
            break;
        }
    ASSERT_NE(paused, nullptr);
    ASSERT_EQ(paused->next_nodes, (std::vector<std::string>{"reviewer"}));
    fixture.engine->fork("source", "paused-fork", paused->id);
    fixture.engine->update_state("paused-fork", {{"messages", json::array({{{"role", "user"}, {"content", "Tokyo"}}})}});
    const auto result = fixture.engine->resume("paused-fork");
    EXPECT_EQ(result.execution_trace, (std::vector<std::string>{"reviewer"}));
    EXPECT_EQ(*fixture.calls, 3);
    EXPECT_EQ(fixture.last_message("paused-fork").value("role", ""), "assistant");
    EXPECT_EQ(fixture.last_message("paused-fork").value("content", ""), "answer:Tokyo");
    EXPECT_EQ(fixture.last_message("source").value("content", ""), "answer:Seoul");
}
