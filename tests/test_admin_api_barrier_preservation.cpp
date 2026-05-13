// Admin APIs (`update_state`, `fork`) wrote a new Checkpoint directly to
// `checkpoint_store_` and — silently — dropped the `barrier_state` field
// of the source cp. A user who called update_state or fork mid-AND-join
// would lose every upstream arrival the barrier had already accumulated,
// so on resume the join node waited forever for signals that had
// already been consumed.
//
// These tests lock in the fix: both admin APIs must carry barrier_state
// over from the source checkpoint to the one they produce.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

// Minimal no-op node — we only exercise the admin APIs, not the engine
// step loop, so the graph structure just has to compile.
class Noop : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return "noop"; }
};

std::shared_ptr<CheckpointStore> seed_cp_with_barrier(
    const std::string& thread_id,
    BarrierState partial_arrivals) {

    auto store = std::make_shared<InMemoryCheckpointStore>();

    Checkpoint cp;
    cp.id = Checkpoint::generate_id();
    cp.thread_id = thread_id;
    cp.current_node = "join";
    cp.next_nodes = {"join"};
    cp.interrupt_phase = CheckpointPhase::Before;
    cp.step = 3;
    cp.barrier_state = std::move(partial_arrivals);
    // Minimal channel_values that restore() will accept.
    cp.channel_values = {
        {"a_done",       {{"value", true},  {"version", 1}}},
        {"b_step1_done", {{"value", true},  {"version", 1}}}
    };
    cp.timestamp = 1;
    store->save(cp);
    return store;
}

std::unique_ptr<GraphEngine> compile_minimal_engine(
    std::shared_ptr<CheckpointStore> store) {

    NodeFactory::instance().register_type("__aab_noop",
        [](const std::string&, const json&, const NodeContext&)
            -> std::unique_ptr<GraphNode> {
            return std::make_unique<Noop>();
        });

    json graph = {
        {"name", "minimal"},
        {"channels", {
            {"a_done",       {{"reducer", "overwrite"}}},
            {"b_step1_done", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"join", {{"type", "__aab_noop"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "join"}},
            {{"from", "join"},      {"to", "__end__"}}
        })}
    };
    return GraphEngine::compile(graph, NodeContext{}, std::move(store));
}

} // namespace

TEST(AdminApiBarrierPreservation, UpdateStateCarriesBarrierStateForward) {
    BarrierState arrivals;
    arrivals["join"] = {"a"};  // `a` already signalled; `b_final` still pending

    auto store = seed_cp_with_barrier("admin-update-t1", arrivals);
    auto engine = compile_minimal_engine(store);

    // Write a new value through update_state. Must carry arrivals forward.
    json writes = {{"a_done", true}};
    engine->update_state("admin-update-t1", writes);

    auto all = store->list("admin-update-t1");
    ASSERT_GE(all.size(), 2u) << "update_state should have appended a cp";

    // Latest cp is the one update_state just produced.
    auto latest = store->load_latest("admin-update-t1");
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(CheckpointPhase::Updated, latest->interrupt_phase);

    ASSERT_TRUE(latest->barrier_state.count("join"))
        << "update_state dropped barrier_state — pre-fix regression";
    EXPECT_EQ(1u, latest->barrier_state["join"].size());
    EXPECT_EQ(1u, latest->barrier_state["join"].count("a"));
}

TEST(AdminApiBarrierPreservation, ForkCarriesBarrierStateToNewThread) {
    BarrierState arrivals;
    arrivals["join"] = {"a", "c"};  // arbitrary partial set

    auto store = seed_cp_with_barrier("admin-fork-src", arrivals);
    auto engine = compile_minimal_engine(store);

    std::string forked_id = engine->fork("admin-fork-src", "admin-fork-dst");
    ASSERT_FALSE(forked_id.empty());

    auto forked = store->load_latest("admin-fork-dst");
    ASSERT_TRUE(forked.has_value());
    EXPECT_EQ(forked_id, forked->id);

    ASSERT_TRUE(forked->barrier_state.count("join"))
        << "fork dropped barrier_state — pre-fix regression";
    EXPECT_EQ(2u, forked->barrier_state["join"].size());
    EXPECT_EQ(1u, forked->barrier_state["join"].count("a"));
    EXPECT_EQ(1u, forked->barrier_state["join"].count("c"));
}
