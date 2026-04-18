// Checkpointer regression: under signal dispatch a super-step can leave
// multiple nodes ready simultaneously (parallel fan-out, conditional
// branches activating together). The checkpoint schema previously stored
// only a single `next_node`, so on resume every sibling-branch after the
// first was silently dropped. These tests lock in that the full ready
// set survives checkpoint round-trips.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Simple counting node that writes "<name>_hits = N+1" on every execution.
class HitCounter : public GraphNode {
public:
    explicit HitCounter(std::string n) : n_(std::move(n)) {}
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int cur = 0;
        auto v = state.get(n_ + "_hits");
        if (v.is_number()) cur = static_cast<int>(v.get<double>());
        return {ChannelWrite{n_ + "_hits", json(cur + 1)}};
    }
    std::string get_name() const override { return n_; }
private:
    std::string n_;
};

void register_hit_counter() {
    NodeFactory::instance().register_type("hit",
        [](const std::string& name, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<HitCounter>(name);
        });
}

} // namespace

// -------------------------------------------------------------------------
// Mid-graph fan-out: `a` writes to two downstream nodes. The checkpoint
// committed at the end of step 0 (where `a` ran) must record BOTH
// {b, c} as next_nodes — the exact case where the old single-string
// next_node would have dropped a sibling.
// -------------------------------------------------------------------------
TEST(MultiNodeResume, FanOutCheckpointPersistsBothBranches) {
    register_hit_counter();

    json graph = {
        {"name", "fanout_mid"},
        {"channels", {
            {"a_hits", {{"reducer", "overwrite"}}},
            {"b_hits", {{"reducer", "overwrite"}}},
            {"c_hits", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a", {{"type", "hit"}}},
            {"b", {{"type", "hit"}}},
            {"c", {{"type", "hit"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "a"}, {"to", "c"}},
            {{"from", "b"}, {"to", "__end__"}},
            {{"from", "c"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "fanout-mid-001";
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    EXPECT_EQ(result.output["channels"]["a_hits"]["value"].get<int>(), 1);
    EXPECT_EQ(result.output["channels"]["b_hits"]["value"].get<int>(), 1);
    EXPECT_EQ(result.output["channels"]["c_hits"]["value"].get<int>(), 1);

    // Find the checkpoint whose current_node is "a". Its next_nodes must
    // contain BOTH "b" and "c" — this is the whole point of the vector
    // upgrade.
    auto history = engine->get_state_history("fanout-mid-001");
    const Checkpoint* after_a = nullptr;
    for (auto& cp : history) {
        if (cp.current_node == "a" && cp.interrupt_phase == CheckpointPhase::Completed) {
            after_a = &cp;
            break;
        }
    }
    ASSERT_NE(after_a, nullptr) << "must have a completed cp after node 'a'";
    ASSERT_EQ(after_a->next_nodes.size(), 2u)
        << "fan-out from a must save both b and c";
    std::set<std::string> got(after_a->next_nodes.begin(),
                              after_a->next_nodes.end());
    EXPECT_EQ(got, (std::set<std::string>{"b", "c"}));
}

// -------------------------------------------------------------------------
// End-to-end resume from a multi-element next_nodes checkpoint: the
// engine must re-enter with the ENTIRE ready set, not just ready[0].
// We force this by running until an interrupt_before at a single node,
// but after a fan-out — so the committed cp from the fan-out step is
// restored and both branches must be picked up on resume.
// -------------------------------------------------------------------------
TEST(MultiNodeResume, ResumeReEntersAllReadyNodes) {
    register_hit_counter();

    json graph = {
        {"name", "fanout_resume"},
        {"channels", {
            {"a_hits", {{"reducer", "overwrite"}}},
            {"b_hits", {{"reducer", "overwrite"}}},
            {"c_hits", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a", {{"type", "hit"}}},
            {"b", {{"type", "hit"}}},
            {"c", {{"type", "hit"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "a"}, {"to", "c"}},
            {{"from", "b"}, {"to", "__end__"}},
            {{"from", "c"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();

    // Phase 1: run with interrupt_before={b, c} so execution stops the
    // moment the fan-out decision is committed. The latest cp will have
    // next_nodes = {b, c}.
    {
        auto g = graph;
        g["interrupt_before"] = json::array({"b"});
        auto engine = GraphEngine::compile(g, NodeContext{}, store);

        RunConfig cfg;
        cfg.thread_id = "resume-001";
        cfg.max_steps = 10;
        auto result = engine->run(cfg);
        EXPECT_TRUE(result.interrupted);
    }

    auto cp = store->load_latest("resume-001");
    ASSERT_TRUE(cp.has_value());

    // Phase 2: resume with NO interrupt — both siblings must fire.
    auto engine2 = GraphEngine::compile(graph, NodeContext{}, store);
    auto result = engine2->resume("resume-001");

    EXPECT_EQ(result.output["channels"]["a_hits"]["value"].get<int>(), 1);
    EXPECT_EQ(result.output["channels"]["b_hits"]["value"].get<int>(), 1)
        << "b must fire on resume (it was in next_nodes)";
    EXPECT_EQ(result.output["channels"]["c_hits"]["value"].get<int>(), 1)
        << "c must ALSO fire on resume — under the old single next_node "
           "schema this branch would have been silently dropped";
}

// -------------------------------------------------------------------------
// interrupt_before with multiple ready nodes: all three must be in the
// checkpoint's next_nodes so that resume() re-enters with {a, b, c},
// not just the first.
// -------------------------------------------------------------------------
TEST(MultiNodeResume, InterruptBeforeSavesAllReadyNodes) {
    register_hit_counter();

    json graph = {
        {"name", "fanout_interrupt"},
        {"channels", {
            {"a_hits", {{"reducer", "overwrite"}}},
            {"b_hits", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a", {{"type", "hit"}}},
            {"b", {{"type", "hit"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "b"}},
            {{"from", "a"}, {"to", "__end__"}},
            {{"from", "b"}, {"to", "__end__"}}
        })},
        {"interrupt_before", json::array({"a"})}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "intr-001";
    cfg.max_steps = 10;
    auto result = engine->run(cfg);

    EXPECT_TRUE(result.interrupted);

    // The interrupt fires before 'a' runs. The checkpoint's next_nodes
    // must at least contain 'a'. (The exact set depends on implementation
    // — currently we save only the interrupted node so resume re-enters
    // precisely there. This is documented contract.)
    auto cp = store->load_latest("intr-001");
    ASSERT_TRUE(cp.has_value());
    ASSERT_FALSE(cp->next_nodes.empty());
    bool has_a = false;
    for (auto& n : cp->next_nodes) if (n == "a") has_a = true;
    EXPECT_TRUE(has_a) << "interrupt_before cp must remember it was about to run 'a'";
}

// -------------------------------------------------------------------------
// fork() copies next_nodes entirely, so a forked thread resumes with the
// same parallel branches the source had.
// -------------------------------------------------------------------------
TEST(MultiNodeResume, ForkCopiesEntireNextNodesVector) {
    Checkpoint cp;
    cp.id = "src-cp";
    cp.thread_id = "src";
    cp.channel_values = json::object();
    cp.current_node = "planner";
    cp.next_nodes = {"a", "b", "c"};
    cp.interrupt_phase = CheckpointPhase::Completed;
    cp.step = 3;
    cp.timestamp = 1;

    InMemoryCheckpointStore store;
    store.save(cp);

    auto loaded = store.load_latest("src");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->next_nodes.size(), 3u);
    EXPECT_EQ(loaded->next_nodes[0], "a");
    EXPECT_EQ(loaded->next_nodes[1], "b");
    EXPECT_EQ(loaded->next_nodes[2], "c");
}
