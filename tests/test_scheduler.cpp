// Pure unit tests for the Scheduler — routing rules in isolation, no
// GraphEngine, no real nodes, no Taskflow. The point of extracting
// Scheduler from GraphEngine was that today's class of bug (XOR/AND
// conflation, dropped siblings, Command vs edge override) is a pure
// routing question that doesn't need node execution to verify; these
// tests catch regressions on the planning layer directly.

#include <gtest/gtest.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/loader.h>  // ConditionRegistry

using namespace neograph::graph;
using neograph::json;

namespace {

// Conditions under test are registered once per process. Guard against
// double-registration so multiple test cases can share the same names.
void register_cond_once(const std::string& name, ConditionFn fn) {
    static std::set<std::string> done;
    if (done.insert(name).second) {
        ConditionRegistry::instance().register_condition(name, std::move(fn));
    }
}

// GraphState holds a shared_mutex and is neither copyable nor movable,
// so we can't return it by value from a helper. Build it in place.
void fill_route(GraphState& s, const std::string& channel, const std::string& value) {
    s.init_channel(channel, ReducerType::OVERWRITE, nullptr, json(value));
}

} // namespace

// -------------------------------------------------------------------------
// resolve_next_nodes
// -------------------------------------------------------------------------
TEST(SchedulerResolve, RegularEdgeReturnsAllSuccessors) {
    std::vector<Edge> edges = {{"a", "b"}, {"a", "c"}, {"x", "y"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto nexts = sch.resolve_next_nodes("a", s_empty);
    EXPECT_EQ(nexts.size(), 2u);
    EXPECT_EQ(nexts[0], "b");
    EXPECT_EQ(nexts[1], "c");
}

TEST(SchedulerResolve, NoEdgeReturnsEndSentinel) {
    std::vector<Edge> edges = {{"a", "b"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto nexts = sch.resolve_next_nodes("orphan", s_empty);
    ASSERT_EQ(nexts.size(), 1u);
    EXPECT_EQ(nexts[0], std::string(END_NODE));
}

TEST(SchedulerResolve, ConditionalEdgePicksMatchingRoute) {
    register_cond_once("route_by_mode",
        [](const GraphState& s) { return s.get("mode").get<std::string>(); });

    std::vector<Edge> edges;
    std::vector<ConditionalEdge> cedges = {{
        "decider", "route_by_mode",
        {{"fast", "quick_path"}, {"slow", "careful_path"}}
    }};
    Scheduler sch(edges, cedges);

    GraphState s_fast; fill_route(s_fast, "mode", "fast");
    auto fast = sch.resolve_next_nodes("decider", s_fast);
    ASSERT_EQ(fast.size(), 1u);
    EXPECT_EQ(fast[0], "quick_path");

    GraphState s_slow; fill_route(s_slow, "mode", "slow");
    auto slow = sch.resolve_next_nodes("decider", s_slow);
    ASSERT_EQ(slow.size(), 1u);
    EXPECT_EQ(slow[0], "careful_path");
}

TEST(SchedulerResolve, ConditionalFallsBackToLastRouteOnMismatch) {
    register_cond_once("route_by_mode_fb",
        [](const GraphState& s) { return s.get("mode").get<std::string>(); });

    std::vector<Edge> edges;
    std::vector<ConditionalEdge> cedges = {{
        "decider", "route_by_mode_fb",
        {{"fast", "quick"}, {"slow", "careful"}, {"zzz_default", "fallback"}}
    }};
    Scheduler sch(edges, cedges);

    // "nonsense" doesn't match any route key; we expect the LAST entry
    // (by std::map ordering = lexicographic) — here "zzz_default" —
    // so the route target is "fallback". This is the documented
    // contract (last-map-entry fallback) carried over from the engine.
    GraphState s_nonsense; fill_route(s_nonsense, "mode", "nonsense");
    auto out = sch.resolve_next_nodes("decider", s_nonsense);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "fallback");
}

// -------------------------------------------------------------------------
// plan_start_step
// -------------------------------------------------------------------------
TEST(SchedulerStart, ExpandsStartEdgesSkippingEnd) {
    std::vector<Edge> edges = {
        {std::string(START_NODE), "a"},
        {std::string(START_NODE), "b"},
        {std::string(START_NODE), std::string(END_NODE)},  // pathological but allowed
        {"a", "c"}
    };
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    auto start = sch.plan_start_step();
    ASSERT_EQ(start.size(), 2u);
    EXPECT_EQ(start[0], "a");
    EXPECT_EQ(start[1], "b");
}

// -------------------------------------------------------------------------
// plan_next_step — core signal-dispatch rules
// -------------------------------------------------------------------------
TEST(SchedulerPlan, SingleNodeUnionSuccessors) {
    std::vector<Edge> edges = {{"a", "b"}, {"a", "c"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step({{"a", std::nullopt}}, s_empty);
    EXPECT_FALSE(plan.hit_end);
    EXPECT_FALSE(plan.winning_command_goto.has_value());
    EXPECT_EQ(plan.ready.size(), 2u);
    std::set<std::string> got(plan.ready.begin(), plan.ready.end());
    EXPECT_EQ(got, (std::set<std::string>{"b", "c"}));
}

TEST(SchedulerPlan, ParallelFanInDedupsToSingleDownstream) {
    // Two just-run nodes both route to `join`. Must be deduped to one.
    std::vector<Edge> edges = {{"a", "join"}, {"b", "join"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step(
        {{"a", std::nullopt}, {"b", std::nullopt}}, s_empty);
    EXPECT_FALSE(plan.hit_end);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "join");
}

TEST(SchedulerPlan, EndNodeTripsHitEndAndIsExcluded) {
    std::vector<Edge> edges = {{"a", "b"}, {"a", std::string(END_NODE)}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step({{"a", std::nullopt}}, s_empty);
    EXPECT_TRUE(plan.hit_end);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "b")
        << "ready must not contain END_NODE — downstream code assumes "
           "ready entries are actual graph nodes";
}

TEST(SchedulerPlan, CommandGotoOverridesAllEdgeRouting) {
    // `a` has regular edges to {wrong1, wrong2} but emits Command.goto=right.
    // Scheduler must ignore edges entirely.
    std::vector<Edge> edges = {{"a", "wrong1"}, {"a", "wrong2"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step(
        {{"a", std::optional<std::string>{"right"}}}, s_empty);
    EXPECT_FALSE(plan.hit_end);
    ASSERT_TRUE(plan.winning_command_goto.has_value());
    EXPECT_EQ(*plan.winning_command_goto, "right");
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "right");
}

TEST(SchedulerPlan, CommandGotoToEndTerminates) {
    std::vector<Edge> edges = {{"a", "keep_going"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step(
        {{"a", std::optional<std::string>{std::string(END_NODE)}}},
        s_empty);
    EXPECT_TRUE(plan.hit_end);
    EXPECT_TRUE(plan.ready.empty())
        << "Command goto=END must terminate with empty ready set";
}

TEST(SchedulerPlan, MultipleCommandGotosLastWins) {
    // Preserves existing engine semantics: parallel Taskflow order is
    // already non-deterministic, so whichever iteration runs LAST in
    // the aggregation loop wins. We assert the rule itself (not the
    // order), to lock it in as intentional.
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step({
            {"a", std::optional<std::string>{"first"}},
            {"b", std::optional<std::string>{"second"}}
        }, s_empty);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "second");
    EXPECT_EQ(*plan.winning_command_goto, "second");
}

TEST(SchedulerPlan, NoRoutingsProducesEmptyPlan) {
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step({}, s_empty);
    EXPECT_FALSE(plan.hit_end);
    EXPECT_TRUE(plan.ready.empty());
}

// -------------------------------------------------------------------------
// Cross-cutting: this is the scheduler-level restatement of the signal
// dispatch contract that test_signal_dispatch validates end-to-end.
// Having it here as a pure test means the rule is verifiable without
// spinning up the engine.
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// NodeResult-taking overload: the engine-facing ergonomic path. The
// scheduler should do the ready[i] ↔ results[i] pairing + Command
// extraction internally.
// -------------------------------------------------------------------------
TEST(SchedulerPlan, NodeResultOverloadExtractsCommandGoto) {
    std::vector<Edge> edges = {{"a", "wrong"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    NodeResult a_result;
    a_result.command = Command{"right", {}};
    std::vector<NodeResult> results = {a_result};
    std::vector<std::string> ready = {"a"};

    GraphState s_empty;
    auto plan = sch.plan_next_step(ready, results, s_empty);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "right")
        << "Command.goto must preempt the edge to 'wrong'";
}

TEST(SchedulerPlan, NodeResultOverloadIgnoresEmptyCommandGoto) {
    std::vector<Edge> edges = {{"a", "b"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    NodeResult a_result;
    a_result.command = Command{"", {}};  // empty goto_node: no override
    std::vector<NodeResult> results = {a_result};
    std::vector<std::string> ready = {"a"};

    GraphState s_empty;
    auto plan = sch.plan_next_step(ready, results, s_empty);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "b")
        << "Command with empty goto_node must NOT override edge routing "
           "— Command is carrying state updates only";
}

TEST(SchedulerPlan, NodeResultOverloadThrowsOnSizeMismatch) {
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    std::vector<std::string> ready = {"a", "b"};
    std::vector<NodeResult> results = {NodeResult{}};  // one short

    GraphState s_empty;
    EXPECT_THROW(sch.plan_next_step(ready, results, s_empty),
                 std::invalid_argument)
        << "caller must not silently truncate — pairing invariant is a "
           "contract, not a hint";
}

TEST(SchedulerPlan, NoImplicitJoinBarrierAcrossSuperSteps) {
    // Graph: a -> join, s2 -> join. Both `a` and `s2` route to `join`
    // in the same super-step → join runs once. This is dedup, not a
    // join barrier; the barrier question is "what if a and s2 run in
    // DIFFERENT super-steps" — that's tested end-to-end in
    // test_signal_dispatch.AsymmetricSerialFanInFiresJoinPerPath. At
    // the scheduler level, a single invocation = a single super-step,
    // so we just assert the dedup rule.
    std::vector<Edge> edges = {{"a", "join"}, {"s2", "join"}};
    std::vector<ConditionalEdge> cedges;
    Scheduler sch(edges, cedges);

    GraphState s_empty;
    auto plan = sch.plan_next_step(
        {{"a", std::nullopt}, {"s2", std::nullopt}}, s_empty);
    ASSERT_EQ(plan.ready.size(), 1u)
        << "parallel fan-in into the same super-step dedups to one ready entry";
}

// -------------------------------------------------------------------------
// Barrier (AND-join) semantics — opt-in, declared per-node at compile
// time. Covers the full lifecycle: partial signals accumulate across
// super-step boundaries, the barrier fires only when its wait_for set
// is fully satisfied, then its state resets for the next round.
// -------------------------------------------------------------------------
TEST(SchedulerBarrier, DefersUntilAllUpstreamsHaveSignaled) {
    std::vector<Edge> edges = {{"a", "join"}, {"s2", "join"}};
    std::vector<ConditionalEdge> cedges;
    BarrierSpecs specs = {{"join", {"a", "s2"}}};
    Scheduler sch(edges, cedges, specs);

    GraphState s_empty;
    BarrierState bstate;

    // Super-step 1: only `a` signals → join NOT ready yet.
    auto plan1 = sch.plan_next_step({{"a", std::nullopt}}, s_empty, bstate);
    EXPECT_TRUE(plan1.ready.empty())
        << "barrier must defer until every wait_for entry has signaled";
    ASSERT_EQ(bstate["join"].size(), 1u);
    EXPECT_TRUE(bstate["join"].count("a"));

    // Super-step 2: `s2` signals → join fires, state resets.
    auto plan2 = sch.plan_next_step({{"s2", std::nullopt}}, s_empty, bstate);
    ASSERT_EQ(plan2.ready.size(), 1u);
    EXPECT_EQ(plan2.ready[0], "join");
    EXPECT_TRUE(bstate["join"].empty())
        << "barrier state must clear on fire so a subsequent loop "
           "through the barrier collects fresh signals";
}

TEST(SchedulerBarrier, FiresImmediatelyWhenAllSignalsArriveInOneStep) {
    std::vector<Edge> edges = {{"a", "join"}, {"s2", "join"}};
    std::vector<ConditionalEdge> cedges;
    BarrierSpecs specs = {{"join", {"a", "s2"}}};
    Scheduler sch(edges, cedges, specs);

    GraphState s_empty;
    BarrierState bstate;

    // Both signal in the same super-step → fire that super-step.
    auto plan = sch.plan_next_step(
        {{"a", std::nullopt}, {"s2", std::nullopt}}, s_empty, bstate);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "join");
    EXPECT_TRUE(bstate["join"].empty());
}

TEST(SchedulerBarrier, TwoArgOverloadIgnoresBarrierSpecs) {
    // Document the contract: the barrierless overload has no caller-
    // owned state to remember partial signals in, so it treats every
    // candidate (barrier or not) as ready-on-first-signal. Callers
    // that declared barriers MUST use the BarrierState overload.
    std::vector<Edge> edges = {{"a", "join"}};
    std::vector<ConditionalEdge> cedges;
    BarrierSpecs specs = {{"join", {"a", "s2"}}};  // s2 never signals
    Scheduler sch(edges, cedges, specs);

    GraphState s_empty;
    auto plan = sch.plan_next_step({{"a", std::nullopt}}, s_empty);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "join")
        << "two-arg overload must fire normally — it cannot honor "
           "barriers because it has no state to accumulate into";
}

TEST(SchedulerBarrier, NonBarrierTargetsUnaffected) {
    // Only `join` is a barrier; `other` isn't. `other` fires on first
    // signal regardless of barrier_state contents.
    std::vector<Edge> edges = {{"a", "join"}, {"a", "other"}};
    std::vector<ConditionalEdge> cedges;
    BarrierSpecs specs = {{"join", {"a", "b"}}};
    Scheduler sch(edges, cedges, specs);

    GraphState s_empty;
    BarrierState bstate;

    auto plan = sch.plan_next_step({{"a", std::nullopt}}, s_empty, bstate);
    // `join` is deferred (only 1/2 signals). `other` fires immediately.
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "other");
}

TEST(SchedulerBarrier, CommandGotoPreemptsBarrier) {
    // If any node emits Command.goto, edge routing (and therefore
    // barrier accumulation) is entirely skipped. Preserves existing
    // "Command overrides everything" contract.
    std::vector<Edge> edges = {{"a", "join"}};
    std::vector<ConditionalEdge> cedges;
    BarrierSpecs specs = {{"join", {"a", "s2"}}};
    Scheduler sch(edges, cedges, specs);

    GraphState s_empty;
    BarrierState bstate;

    auto plan = sch.plan_next_step(
        {{"a", std::optional<std::string>{"redirect"}}}, s_empty, bstate);
    ASSERT_EQ(plan.ready.size(), 1u);
    EXPECT_EQ(plan.ready[0], "redirect");
    EXPECT_TRUE(bstate.empty())
        << "Command preempts signal accumulation — no partial barrier "
           "state should be recorded when edges aren't consulted";
}
