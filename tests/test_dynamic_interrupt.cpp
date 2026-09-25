// Dynamic interrupt: a node decides at runtime to pause, and the pause
// carries information in both directions (issue #94).
//
// The static form of this (interrupt_before / interrupt_after in the graph
// definition) has always worked and is covered elsewhere. The *dynamic* form
// — `throw NodeInterrupt(...)` from inside a node body — is the one the
// canonical approval prompt needs, because you cannot know at compile time
// which node will want a human:
//
//     "The agent wants to run `rm -rf build/`. Allow?"
//
// For that to work at all, three things must hold. Before this file, none of
// them did, and no test in the repository asserted any of them:
//
//   1. The node can say WHY, and attach WHAT needs approving.
//   2. The caller can read both, and learn WHICH node paused.
//   3. The caller can answer, and the resumed node can read the answer.
//
// (1) and (2) were broken in opposite ways on the two executor paths: the
// single-node path threw away the node's reason and replaced it with the node
// name; the parallel path kept the reason but lost the node name, so
// RunResult::interrupt_node — a field whose name promises a node — held a
// sentence. (3) had no general mechanism: resume_value was written into a
// "messages" chat channel, so a graph without one could not receive it.

#include <gtest/gtest.h>

#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>
#include <neograph/graph/checkpoint.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <memory>
#include <set>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

constexpr const char* kReason = "High-value payment: 2500000 KRW. Approval required.";

// Pauses on the first visit with a reason + a structured payload; on the
// second visit (the resume) it reads the human's answer off the RunContext
// and either completes or refuses.
class ApprovalNode : public GraphNode {
public:
    explicit ApprovalNode(std::string name, std::atomic<int>* visits = nullptr)
        : name_(std::move(name)), visits_(visits) {}

    asio::awaitable<NodeResult> run(NodeInput in) override {
        if (visits_) visits_->fetch_add(1);

        const auto& answer = in.ctx.resume_value;
        if (!answer) {
            json payload;
            payload["tool"] = "shell";
            payload["cmd"]  = "rm -rf build/";
            throw NodeInterrupt(kReason, payload);
        }

        const bool approved = answer->value("approved", false);
        NodeResult out;
        out.writes.push_back(ChannelWrite{
            "result", json(approved ? "paid" : "refused")});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string       name_;
    std::atomic<int>* visits_;
};

// Plain worker, used as the sibling that makes a super-step parallel.
class QuietNode : public GraphNode {
public:
    explicit QuietNode(std::string name, std::atomic<int>* visits = nullptr)
        : name_(std::move(name)), visits_(visits) {}

    asio::awaitable<NodeResult> run(NodeInput) override {
        if (visits_) visits_->fetch_add(1);
        NodeResult out;
        out.writes.push_back(ChannelWrite{"sibling", json("done")});
        co_return out;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
    std::atomic<int>* visits_;
};

class AnchorNode : public GraphNode {
public:
    asio::awaitable<NodeResult> run(NodeInput) override {
        NodeResult out;
        out.writes.push_back(ChannelWrite{"anchor", json(true)});
        co_return out;
    }
    std::string get_name() const override { return "anchor"; }
};

class RepeatApprovalNode : public GraphNode {
public:
    explicit RepeatApprovalNode(std::atomic<int>* visits) : visits_(visits) {}

    asio::awaitable<NodeResult> run(NodeInput) override {
        const int visit = visits_->fetch_add(1) + 1;
        if (visit < 3) throw NodeInterrupt("approval still pending");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"result", json("approved")});
        co_return out;
    }

    std::string get_name() const override { return "risky"; }

private:
    std::atomic<int>* visits_;
};

json single_node_graph() {
    return json{
        {"name", "di_single"},
        {"channels", {
            {"result",  {{"reducer", "overwrite"}}},
            {"sibling", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {{"risky", {{"type", "di_approval"}}}}},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "risky"}},
            json{{"from", "risky"},     {"to", "__end__"}}
        })}
    };
}

// Two nodes ready in the same super-step → the engine takes run_parallel_async.
json parallel_graph() {
    return json{
        {"name", "di_parallel"},
        {"channels", {
            {"result",  {{"reducer", "overwrite"}}},
            {"sibling", {{"reducer", "overwrite"}}}
        }},
        {"nodes", {
            {"risky", {{"type", "di_approval"}}},
            {"calm",  {{"type", "di_quiet"}}}
        }},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "risky"}},
            json{{"from", "__start__"}, {"to", "calm"}},
            json{{"from", "risky"},     {"to", "__end__"}},
            json{{"from", "calm"},      {"to", "__end__"}}
        })}
    };
}

json parallel_graph_with_anchor() {
    auto graph = parallel_graph();
    graph["name"] = "di_parallel_anchored";
    graph["channels"]["anchor"] = {{"reducer", "overwrite"}};
    graph["nodes"]["anchor"] = {{"type", "di_anchor"}};
    graph["edges"] = json::array({
        json{{"from", "__start__"}, {"to", "anchor"}},
        json{{"from", "anchor"}, {"to", "risky"}},
        json{{"from", "anchor"}, {"to", "calm"}},
        json{{"from", "risky"}, {"to", "__end__"}},
        json{{"from", "calm"}, {"to", "__end__"}}
    });
    return graph;
}

void register_types(std::atomic<int>* visits = nullptr,
                    std::atomic<int>* calm_visits = nullptr) {
    NodeFactory::instance().register_type("di_approval",
        [visits](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<ApprovalNode>("risky", visits);
        });
    NodeFactory::instance().register_type("di_quiet",
        [calm_visits](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<QuietNode>("calm", calm_visits);
        });
    NodeFactory::instance().register_type("di_anchor",
        [](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<AnchorNode>();
        });
}

} // namespace

// ── 1. The reason survives (single-node path) ─────────────────────────────
//
// RED: interrupt_value["reason"] == "risky" — the node's sentence was
// overwritten with the node's name at graph_executor.cpp:397.
TEST(DynamicInterrupt, ReasonSurvivesTheSingleNodePath) {
    register_types();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(single_node_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-reason";
    auto result = engine->run(cfg);

    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_value.value("reason", ""), kReason)
        << "the node's reason was replaced by something else";
}

// ── 2. The node name survives (parallel path) ─────────────────────────────
//
// RED: interrupt_node holds the reason sentence, not "risky". The field is
// named for a node; a caller routing on it (or logging "paused at: {node}")
// gets a paragraph.
TEST(DynamicInterrupt, NodeNameSurvivesTheParallelPath) {
    register_types();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(parallel_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-parallel";
    auto result = engine->run(cfg);

    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_node, "risky")
        << "interrupt_node must name the node that paused, on every path";
    EXPECT_EQ(result.interrupt_value.value("reason", ""), kReason);
}

TEST(DynamicInterrupt, ParallelResumeReplaysSuccessfulSiblingWrites) {
    std::atomic<int> risky_visits{0};
    std::atomic<int> calm_visits{0};
    register_types(&risky_visits, &calm_visits);
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(parallel_graph_with_anchor(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-parallel-pending-replay";
    auto paused = engine->run(cfg);
    ASSERT_TRUE(paused.interrupted);
    EXPECT_EQ(risky_visits.load(), 1);
    EXPECT_EQ(calm_visits.load(), 1);
    auto interrupt_cp = store->load_latest(cfg.thread_id);
    ASSERT_TRUE(interrupt_cp.has_value());
    EXPECT_EQ(interrupt_cp->interrupt_phase, CheckpointPhase::NodeInterrupt);
    EXPECT_EQ(std::set<std::string>(interrupt_cp->next_nodes.begin(),
                                    interrupt_cp->next_nodes.end()),
              (std::set<std::string>{"risky", "calm"}));

    // Recompile against the same store and resume by the exact checkpoint ID.
    auto cold_engine = GraphEngine::compile(parallel_graph_with_anchor(), NodeContext{}, store);
    auto done = cold_engine->resume_from(cfg, interrupt_cp->id,
                                         json{{"approved", true}});
    EXPECT_FALSE(done.interrupted);
    EXPECT_EQ(risky_visits.load(), 2);
    EXPECT_EQ(calm_visits.load(), 1)
        << "the successful sibling must replay from its pending write";
    EXPECT_EQ(done.output["channels"]["sibling"]["value"], "done");
    EXPECT_EQ(done.output["channels"]["result"]["value"], "paid");
}

TEST(DynamicInterrupt, ExactResumeCarriesSiblingWritesAcrossRepeatedInterrupts) {
    std::atomic<int> risky_visits{0};
    std::atomic<int> calm_visits{0};
    register_types(nullptr, &calm_visits);
    NodeFactory::instance().register_type("di_repeat_approval",
        [&risky_visits](const std::string&, const json&, const NodeContext&) {
            return std::make_unique<RepeatApprovalNode>(&risky_visits);
        });
    auto graph = parallel_graph_with_anchor();
    graph["nodes"]["risky"]["type"] = "di_repeat_approval";

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto first_engine = GraphEngine::compile(graph, NodeContext{}, store);
    RunConfig cfg;
    cfg.thread_id = "di-parallel-repeated-interrupt";
    const auto first_pause = first_engine->run(cfg);
    ASSERT_TRUE(first_pause.interrupted);
    EXPECT_EQ(risky_visits.load(), 1);
    EXPECT_EQ(calm_visits.load(), 1);
    const auto first_cp = store->load_latest(cfg.thread_id);
    ASSERT_TRUE(first_cp.has_value());

    auto cold_engine = GraphEngine::compile(graph, NodeContext{}, store);
    const auto second_pause = cold_engine->resume_from(
        cfg, first_cp->id, json{{"approved", true}});
    ASSERT_TRUE(second_pause.interrupted);
    EXPECT_EQ(risky_visits.load(), 2);
    EXPECT_EQ(calm_visits.load(), 1)
        << "the sibling result must remain replayable after another interrupt";
    const auto second_cp = store->load_latest(cfg.thread_id);
    ASSERT_TRUE(second_cp.has_value());
    ASSERT_NE(second_cp->id, first_cp->id);
    EXPECT_EQ(second_cp->parent_id, first_cp->id);
    EXPECT_EQ(second_cp->step, first_cp->step);

    const auto done = cold_engine->resume_from(
        cfg, second_cp->id, json{{"approved", true}});
    EXPECT_FALSE(done.interrupted);
    EXPECT_EQ(risky_visits.load(), 3);
    EXPECT_EQ(calm_visits.load(), 1);
    EXPECT_EQ(done.output["channels"]["sibling"]["value"], "done");
    EXPECT_EQ(done.output["channels"]["result"]["value"], "approved");
}

TEST(DynamicInterrupt, ExactResumeRejectsCyclicInterruptAncestry) {
    std::atomic<int> risky_visits{0};
    std::atomic<int> calm_visits{0};
    register_types(&risky_visits, &calm_visits);
    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(parallel_graph_with_anchor(), NodeContext{}, store);
    RunConfig cfg;
    cfg.thread_id = "di-parallel-cyclic-ancestry";
    ASSERT_TRUE(engine->run(cfg).interrupted);
    auto interrupt_cp = store->load_latest(cfg.thread_id);
    ASSERT_TRUE(interrupt_cp.has_value());

    // Simulate corrupted ancestry through the store API. Exact resume must
    // stop before running any node instead of traversing or replaying it.
    interrupt_cp->parent_id = interrupt_cp->id;
    store->save(*interrupt_cp);
    auto cold_engine = GraphEngine::compile(parallel_graph_with_anchor(), NodeContext{}, store);
    EXPECT_THROW((void)cold_engine->resume_from(cfg, interrupt_cp->id,
                                                json{{"approved", true}}),
                 std::runtime_error);
    EXPECT_EQ(risky_visits.load(), 1);
    EXPECT_EQ(calm_visits.load(), 1);
}

// The single-node path must agree with the parallel one. Two paths that lose
// different halves of the same message is how this rotted in the first place.
TEST(DynamicInterrupt, BothPathsAgreeOnNodeAndReason) {
    register_types();
    auto store = std::make_shared<InMemoryCheckpointStore>();

    auto one = GraphEngine::compile(single_node_graph(), NodeContext{}, store);
    RunConfig c1; c1.thread_id = "di-agree-1";
    auto r1 = one->run(c1);

    auto many = GraphEngine::compile(parallel_graph(), NodeContext{}, store);
    RunConfig c2; c2.thread_id = "di-agree-2";
    auto r2 = many->run(c2);

    ASSERT_TRUE(r1.interrupted);
    ASSERT_TRUE(r2.interrupted);
    EXPECT_EQ(r1.interrupt_node, r2.interrupt_node);
    EXPECT_EQ(r1.interrupt_value.value("reason", ""),
              r2.interrupt_value.value("reason", ""));
}

// ── 3. The payload reaches the caller ─────────────────────────────────────
//
// RED: there is no API to attach one. A reason string cannot carry "which
// tool, which arguments" in a form the caller can branch on without parsing
// prose.
TEST(DynamicInterrupt, PayloadReachesTheCaller) {
    register_types();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(single_node_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-payload";
    auto result = engine->run(cfg);

    ASSERT_TRUE(result.interrupted);
    ASSERT_TRUE(result.interrupt_value.contains("value"))
        << "the structured payload the node attached is missing";
    EXPECT_EQ(result.interrupt_value["value"]["tool"], "shell");
    EXPECT_EQ(result.interrupt_value["value"]["cmd"],  "rm -rf build/");

    // The pre-existing keys stay put — callers reading them keep working.
    EXPECT_EQ(result.interrupt_value.value("type", ""), "NodeInterrupt");
    EXPECT_TRUE(result.interrupt_value.contains("reason"));
}

// ── 4. The answer reaches the node ────────────────────────────────────────
//
// RED: resume_value is written into a "messages" channel. This graph has no
// such channel, so the resumed node cannot see the decision, throws again,
// and the run pauses forever. (This is exactly what examples/09 does today:
// it prints "Result: null" after the "approval".)
TEST(DynamicInterrupt, ResumeValueReachesTheNode) {
    std::atomic<int> visits{0};
    register_types(&visits);
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(single_node_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-resume";
    auto paused = engine->run(cfg);
    ASSERT_TRUE(paused.interrupted);
    EXPECT_EQ(visits.load(), 1);

    json decision;
    decision["approved"] = true;
    auto done = engine->resume("di-resume", decision);

    EXPECT_FALSE(done.interrupted)
        << "the resumed node could not read the human's answer, so it paused again";
    EXPECT_EQ(done.output["channels"]["result"]["value"], "paid");
    EXPECT_EQ(visits.load(), 2) << "the node should have run exactly twice";
}

// A refusal is an answer too — the node must be able to act on the content of
// the decision, not merely on the fact that one arrived.
TEST(DynamicInterrupt, TheNodeCanActOnTheContentOfTheAnswer) {
    register_types();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(single_node_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-refuse";
    ASSERT_TRUE(engine->run(cfg).interrupted);

    json decision;
    decision["approved"] = false;
    auto done = engine->resume("di-refuse", decision);

    EXPECT_FALSE(done.interrupted);
    EXPECT_EQ(done.output["channels"]["result"]["value"], "refused");
}

// ── 5. A fresh run does not see a stale answer ────────────────────────────
//
// ctx.resume_value must be null on a normal run, or a node would take the
// approved branch without anyone approving anything.
TEST(DynamicInterrupt, AFreshRunCarriesNoAnswer) {
    register_types();
    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(single_node_graph(), NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-fresh";
    auto result = engine->run(cfg);

    EXPECT_TRUE(result.interrupted)
        << "with no resume_value the node must pause — it must not see a decision";
}

// ── 6. The reason-only constructor keeps working ──────────────────────────
//
// Every existing caller (examples, deep research, the cookbook) uses the
// one-argument form. It must keep meaning what it meant, with an absent —
// not empty-string — payload.
TEST(DynamicInterrupt, TheReasonOnlyConstructorStillWorks) {
    NodeFactory::instance().register_type("di_legacy",
        [](const std::string&, const json&, const NodeContext&) {
            class Legacy : public GraphNode {
            public:
                asio::awaitable<NodeResult> run(NodeInput) override {
                    throw NodeInterrupt("plain old reason");
                    co_return NodeResult{};
                }
                std::string get_name() const override { return "legacy"; }
            };
            return std::make_unique<Legacy>();
        });

    json def{
        {"name", "di_legacy_graph"},
        {"channels", {{"result", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"legacy", {{"type", "di_legacy"}}}}},
        {"edges", json::array({
            json{{"from", "__start__"}, {"to", "legacy"}},
            json{{"from", "legacy"},    {"to", "__end__"}}
        })}
    };

    auto store  = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(def, NodeContext{}, store);

    RunConfig cfg;
    cfg.thread_id = "di-legacy";
    auto result = engine->run(cfg);

    ASSERT_TRUE(result.interrupted);
    EXPECT_EQ(result.interrupt_node, "legacy");
    EXPECT_EQ(result.interrupt_value.value("reason", ""), "plain old reason");
    EXPECT_FALSE(result.interrupt_value.contains("value"))
        << "no payload was attached; the key must be absent, not null or empty";
}
