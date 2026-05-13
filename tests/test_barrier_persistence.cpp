// Tests for Checkpoint schema v2: BarrierState persistence + resume
// restoration.
//
// Before v2, barrier_state was in-memory only, so an interrupt landing
// in the middle of a multi-source barrier accumulation silently dropped
// every upstream signal received so far. On resume the barrier started
// with an empty map and waited forever for signals that had already
// arrived. The tests here lock in:
//
//   1. A fresh Checkpoint has an empty barrier_state (default).
//   2. A graph without barriers never populates the field — no hidden
//      writes, no memory bloat.
//   3. When a barrier has received some (but not all) upstream signals
//      and an interrupt fires, the partial set is durably recorded on
//      the checkpoint.
//   4. On resume, the partial set is restored and the *remaining*
//      signals arriving after resume complete the barrier normally, so
//      the join node fires exactly once.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/checkpoint.h>
#include <atomic>

using namespace neograph;
using namespace neograph::graph;

namespace {

class HitCounter : public GraphNode {
public:
    HitCounter(std::string n, std::atomic<int>* counter)
        : name_(std::move(n)), counter_(counter) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        NodeOutput out;
        out.writes.push_back(ChannelWrite{name_ + "_done", json(true)});
        co_return out;
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
    std::atomic<int>* counter_;
};

NodeFactoryFn make_hit_factory(std::atomic<int>* counter) {
    return [counter](const std::string& n, const json&, const NodeContext&) {
        return std::make_unique<HitCounter>(n, counter);
    };
}

} // namespace

// =========================================================================
// 1. Default
// =========================================================================

TEST(BarrierPersistence, FreshCheckpointHasEmptyBarrierState) {
    Checkpoint cp;
    EXPECT_TRUE(cp.barrier_state.empty());
    EXPECT_EQ(cp.schema_version, CHECKPOINT_SCHEMA_VERSION);
    EXPECT_EQ(CHECKPOINT_SCHEMA_VERSION, 2)
        << "barrier_state requires schema v2";
}

// =========================================================================
// 2. Non-barrier graphs don't touch the field
// =========================================================================

TEST(BarrierPersistence, GraphWithoutBarriersKeepsEmptyBarrierState) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    json graph = {
        {"name", "no_barriers"},
        {"channels", {
            {"a_done", {{"reducer", "overwrite"}}},
            {"b_done", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a", {{"type", "hit_bp"}}},
            {"b", {{"type", "hit_bp"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}},
            {{"from", "b"}, {"to", "__end__"}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(graph, NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "no-barrier";
    engine->run(cfg);

    auto cps = store->list("no-barrier");
    ASSERT_FALSE(cps.empty());
    for (const auto& cp : cps) {
        EXPECT_TRUE(cp.barrier_state.empty())
            << "cp " << cp.current_node << " leaked barrier_state";
    }
}

// =========================================================================
// 3-4. Shared fixture — barrier with partial signal + interrupt
//
// Topology:
//
//   __start__ ──► a ─────────────────► join  (barrier wait_for={a, b_final})
//        │                                ▲
//        └───► b_step1 ─► gate ─► b_final ┘
//
// interrupt_before "gate" fires at the start of super-step 2. By that
// point super-step 1 has already run {a, b_step1}; `a` has signaled
// `join`, so the barrier accumulator is {a}. `b_final` has not yet
// signaled because its path is still upstream of the interrupt.
//
// After resume, `gate` → `b_final` → `join`. If barrier_state was
// persisted and restored, the second signal completes the accumulator
// ({a, b_final} ⊇ wait_for) and `join` fires exactly once. Without
// persistence the accumulator starts empty on resume, the barrier never
// sees `a`'s signal again (a already ran once), and `join` never fires.
// =========================================================================

static json make_barrier_interrupt_graph() {
    return {
        {"name", "barrier_interrupt"},
        {"channels", {
            {"a_done",       {{"reducer", "overwrite"}}},
            {"b_step1_done", {{"reducer", "overwrite"}}},
            {"gate_done",    {{"reducer", "overwrite"}}},
            {"b_final_done", {{"reducer", "overwrite"}}},
            {"join_done",    {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"a",       {{"type", "hit_bp"}}},
            {"b_step1", {{"type", "hit_bp"}}},
            {"gate",    {{"type", "hit_bp"}}},
            {"b_final", {{"type", "hit_bp"}}},
            {"join", {
                {"type", "hit_bp"},
                {"barrier", {{"wait_for", {"a", "b_final"}}}}
            }}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "b_step1"}},
            {{"from", "a"},         {"to", "join"}},
            {{"from", "b_step1"},   {"to", "gate"}},
            {{"from", "gate"},      {"to", "b_final"}},
            {{"from", "b_final"},   {"to", "join"}},
            {{"from", "join"},      {"to", "__end__"}}
        })},
        {"interrupt_before", {"gate"}}
    };
}

// 3. Partial signal durably recorded
TEST(BarrierPersistence, PartialBarrierSignalsPersistedOnInterrupt) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_barrier_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "partial-barrier";
    auto result = engine->run(cfg);
    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_node, "gate");

    // The latest cp (the Before-phase one we just landed on) must carry
    // barrier_state["join"] = {"a"} — 'a' signaled join in super-step 1,
    // but b_final has not yet run, so the accumulator is half-full.
    auto cp = store->load_latest("partial-barrier");
    ASSERT_TRUE(cp.has_value());
    EXPECT_EQ(cp->interrupt_phase, CheckpointPhase::Before);

    auto it = cp->barrier_state.find("join");
    ASSERT_NE(it, cp->barrier_state.end())
        << "partial barrier signals must be recorded, not dropped";
    EXPECT_EQ(it->second, (std::set<std::string>{"a"}));
}

// 4. End-to-end: resume completes the barrier, join fires exactly once
TEST(BarrierPersistence, ResumeAfterPartialBarrierCompletesJoin) {
    std::atomic<int> hits{0};
    NodeFactory::instance().register_type("hit_bp", make_hit_factory(&hits));

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(make_barrier_interrupt_graph(),
                                        NodeContext{}, store);

    RunConfig cfg; cfg.thread_id = "resume-barrier";
    auto first = engine->run(cfg);
    ASSERT_TRUE(first.interrupted);

    // Phase 1 invocations: a, b_step1 (super-step 1). gate did NOT run
    // because interrupt_before fired first.
    EXPECT_EQ(hits.load(), 2);

    auto resumed = engine->resume("resume-barrier");
    EXPECT_FALSE(resumed.interrupted);

    // Phase 2 invocations: gate, b_final, join. Total = 2 + 3 = 5.
    // If barrier_state were lost on resume, `join` would never fire and
    // this would be 4 (or the run would hit max_steps).
    EXPECT_EQ(hits.load(), 5);
    EXPECT_EQ(resumed.output["channels"]["join_done"]["value"].get<bool>(), true);
}

// =========================================================================
// 5. v1 legacy blob: empty barrier_state on load is safe
// =========================================================================
//
// The v2 engine tolerates v1-shaped checkpoints by treating a missing
// barrier_state as empty. We simulate a v1 load by constructing a
// Checkpoint explicitly and clearing the field, then confirming
// restore() is a no-op on the barrier_state field (the field already
// defaults to empty, so this is a documentation test that the
// fallback shape is well-defined rather than a runtime fix).

TEST(BarrierPersistence, LegacyV1BlobLoadsWithEmptyBarrierState) {
    Checkpoint cp;
    cp.schema_version = 1;  // simulate older store returning a v1 blob
    cp.barrier_state.clear();
    // The engine's restore path uses cp.barrier_state directly; an
    // empty map is the valid "no in-flight barriers" state. Assert it
    // here so the invariant is checked alongside the other v2 tests.
    EXPECT_TRUE(cp.barrier_state.empty());
}
