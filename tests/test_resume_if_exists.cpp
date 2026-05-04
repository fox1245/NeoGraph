// v0.3.1: RunConfig::resume_if_exists — multi-turn-chat semantics.
// When set, the engine seeds GraphState from the latest checkpoint
// for thread_id (if any) and applies input on top via the same
// reducer pipeline as a fresh run. Default-off keeps historical
// fresh-start behaviour for callers that thread state through input
// themselves. These tests lock both branches in.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Echoes the last user message into an APPEND-reduced "messages"
// channel. One run = one assistant turn appended.
class EchoNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        auto msgs = state.get("messages");
        std::string last_user = "";
        if (msgs.is_array()) {
            for (const auto& m : msgs) {
                if (m.is_object() && m.value("role", "") == "user") {
                    last_user = m.value("content", "");
                }
            }
        }
        json reply = {{"role", "assistant"},
                      {"content", "echo: " + last_user}};
        return {ChannelWrite{"messages", json::array({reply})}};
    }
    std::string get_name() const override { return "echo"; }
};

void register_echo_once() {
    static bool done = false;
    if (done) return;
    done = true;
    NodeFactory::instance().register_type("echo",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<EchoNode>();
        });
}

json make_chat_graph() {
    return json{
        {"name", "chat"},
        {"channels", {
            {"messages", {{"reducer", "append"}}}
        }},
        {"nodes", {
            {"echo", {{"type", "echo"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "echo"}},
            {{"from", "echo"}, {"to", "__end__"}}
        })}
    };
}

json user_msg(const std::string& content) {
    return json::array({
        {{"role", "user"}, {"content", content}}
    });
}

} // namespace

// =========================================================================
// Default OFF (back-compat): two runs on same thread_id, no resume flag.
// Each run starts fresh — second run's state contains only its own input
// turn and reply, the first run's contributions are gone.
// =========================================================================
TEST(ResumeIfExists, DefaultOffStartsFresh) {
    register_echo_once();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, store);

    RunConfig c1;
    c1.thread_id = "t-default-off";
    c1.input["messages"] = user_msg("hi");
    auto r1 = engine->run(c1);
    ASSERT_TRUE(r1.output["channels"]["messages"]["value"].is_array());
    EXPECT_EQ(r1.output["channels"]["messages"]["value"].size(), 2u);

    RunConfig c2;
    c2.thread_id = "t-default-off";
    c2.input["messages"] = user_msg("again");
    // resume_if_exists default is false.
    auto r2 = engine->run(c2);
    auto msgs = r2.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    EXPECT_EQ(msgs.size(), 2u)
        << "default-off must NOT carry prior turn forward — got " << msgs.dump();
    EXPECT_EQ(msgs[0]["content"], "again");
}

// =========================================================================
// Opt-in ON: second run loads the prior checkpoint, appends new input via
// the channel reducer, then re-enters the entry node. Final state has
// BOTH turns' messages.
// =========================================================================
TEST(ResumeIfExists, OptInCarriesPriorState) {
    register_echo_once();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, store);

    RunConfig c1;
    c1.thread_id = "t-opt-in";
    c1.input["messages"] = user_msg("hi");
    auto r1 = engine->run(c1);
    EXPECT_EQ(r1.output["channels"]["messages"]["value"].size(), 2u);

    RunConfig c2;
    c2.thread_id        = "t-opt-in";
    c2.input["messages"] = user_msg("how are you");
    c2.resume_if_exists  = true;
    auto r2 = engine->run(c2);
    auto msgs = r2.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    ASSERT_EQ(msgs.size(), 4u)
        << "opt-in must carry prior + new turn — got " << msgs.dump();
    EXPECT_EQ(msgs[0]["role"],    "user");
    EXPECT_EQ(msgs[0]["content"], "hi");
    EXPECT_EQ(msgs[1]["role"],    "assistant");
    EXPECT_EQ(msgs[1]["content"], "echo: hi");
    EXPECT_EQ(msgs[2]["role"],    "user");
    EXPECT_EQ(msgs[2]["content"], "how are you");
    EXPECT_EQ(msgs[3]["role"],    "assistant");
    EXPECT_EQ(msgs[3]["content"], "echo: how are you");
}

// =========================================================================
// Opt-in but no prior checkpoint for thread_id — must NOT error, behaves
// like a fresh run. This is what makes the flag safe to flip to True at
// the top of a chat handler without checking history first.
// =========================================================================
TEST(ResumeIfExists, OptInNoCheckpointStartsFresh) {
    register_echo_once();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id        = "t-fresh-thread";
    cfg.input["messages"] = user_msg("first");
    cfg.resume_if_exists  = true;
    auto r = engine->run(cfg);
    auto msgs = r.output["channels"]["messages"]["value"];
    ASSERT_TRUE(msgs.is_array());
    EXPECT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0]["content"], "first");
    EXPECT_EQ(msgs[1]["content"], "echo: first");
}

// =========================================================================
// Opt-in with a checkpoint store but the engine never ran for this
// thread — load_latest returns nullopt, and the run proceeds fresh.
// Same effective behaviour as the previous test, but explicitly with
// the store populated for a *different* thread to make sure load_latest
// is keyed correctly.
// =========================================================================
TEST(ResumeIfExists, OptInDoesNotCrossThreadIds) {
    register_echo_once();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, store);

    RunConfig c1;
    c1.thread_id        = "thread-A";
    c1.input["messages"] = user_msg("private to A");
    engine->run(c1);

    RunConfig c2;
    c2.thread_id        = "thread-B";
    c2.input["messages"] = user_msg("hello B");
    c2.resume_if_exists  = true;
    auto r2 = engine->run(c2);
    auto msgs = r2.output["channels"]["messages"]["value"];
    ASSERT_EQ(msgs.size(), 2u)
        << "thread-B must not pick up thread-A's history — got " << msgs.dump();
    EXPECT_EQ(msgs[0]["content"], "hello B");
}

// =========================================================================
// Opt-in without a configured checkpoint store: silent no-op. The flag
// being set must not cause the engine to throw when there's nowhere
// to load from — important because the Python pybind binding makes
// the store optional.
// =========================================================================
TEST(ResumeIfExists, OptInWithoutStoreIsNoOp) {
    register_echo_once();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, nullptr);

    RunConfig cfg;
    cfg.thread_id        = "t-no-store";
    cfg.input["messages"] = user_msg("orphan");
    cfg.resume_if_exists  = true;
    auto r = engine->run(cfg);
    auto msgs = r.output["channels"]["messages"]["value"];
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0]["content"], "orphan");
}

// =========================================================================
// Opt-in chains: 3 turns. Verifies the flag is durable across more than
// just one resume — each new turn loads the previous turn's checkpoint
// (which itself was generated from a resumed run), so step numbering
// keeps incrementing without the loop count getting clipped by max_steps.
// =========================================================================
TEST(ResumeIfExists, ThreeTurnConversation) {
    register_echo_once();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_chat_graph(), NodeContext{}, store);

    auto turn = [&](const std::string& content) {
        RunConfig c;
        c.thread_id        = "t-three";
        c.input["messages"] = user_msg(content);
        c.resume_if_exists  = true;
        return engine->run(c);
    };

    turn("turn one");
    turn("turn two");
    auto r3 = turn("turn three");
    auto msgs = r3.output["channels"]["messages"]["value"];
    ASSERT_EQ(msgs.size(), 6u);
    EXPECT_EQ(msgs[0]["content"], "turn one");
    EXPECT_EQ(msgs[2]["content"], "turn two");
    EXPECT_EQ(msgs[4]["content"], "turn three");
    EXPECT_EQ(msgs[5]["content"], "echo: turn three");
}
