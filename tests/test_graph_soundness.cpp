// The opt-in soundness check (Program API v2, WP6; X3, #363, #364, #365).
//
// Hand-written graphs pin each finding (S1-S5), the SOUNDNESS summary, and how
// failure edges, failure exits, max_visits, a declared Command.goto or Send and
// the state cap change what the check may claim. Through the Program compiler
// the findings are returned with help and never sealed.
//
// The agreement suite runs the check on every graph of the bounded-exhaustive
// join harness (tests/join_model.h, the same family and the same
// NEOGRAPH_JOIN_MODEL_BOUND switch as tests/test_join_exhaustive.cpp) and
// compares its verdicts with what the reference interpreter reaches: the
// outcomes, the state count, each finding, and every witness trace replayed
// step by step through the reference. A node that may return
// Command.goto(__end__) gets a node type that declares it, so the check must
// report that node's region as not analyzable instead of claiming soundness.

#if defined(NEOGRAPH_SOUNDNESS_HAVE_PROGRAM)
#include "program_v2_test_support.h"
#include <neograph/program/javascript_capabilities.h>
#endif

#include "../src/core/graph_soundness.h"
#include "join_model.h"

#include <neograph/graph/compiler.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/graph/registry.h>
#include <neograph/graph/state.h>
#include <neograph/graph/validator.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using neograph::json;
using neograph::graph::check_soundness;
using neograph::graph::GraphCompiler;
using neograph::graph::GraphRegistry;
using neograph::graph::GraphValidator;
using neograph::graph::SoundnessOptions;
using neograph::graph::SoundnessReport;
using neograph::graph::SoundnessStatus;
using neograph::graph::ValidationOptions;
using CoreDiagnostic = neograph::graph::Diagnostic;

class Idle final : public neograph::graph::GraphNode {
public:
    explicit Idle(std::string name) : name_(std::move(name)) {}
    asio::awaitable<neograph::graph::NodeOutput> run(neograph::graph::NodeInput) override {
        co_return neograph::graph::NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

std::unique_ptr<neograph::graph::GraphNode> idle(const std::string& name, const json&,
                                                  const neograph::graph::NodeContext&) {
    return std::make_unique<Idle>(name);
}

std::string dump(const json& value) { return value.dump(); }

void add_reducers(GraphRegistry& registry) {
    for (const auto* name : {"overwrite", "append"})
        registry.register_reducer(name, neograph::graph::ReducerRegistry::instance().get(name));
}

// s.task declares no effects of interest; s.writer writes the overwrite channel
// "out"; s.router and s.sender declare Command.goto and Send.
const GraphRegistry& unit_registry() {
    static const GraphRegistry registry = [] {
        GraphRegistry r;
        add_reducers(r);
        const json open{{"type", "object"}};
        r.register_type("s.task", idle, open, json::object());
        r.register_type("s.writer", idle, open, json{{"writes", json::array({"out"})}});
        r.register_type("s.router", idle, open, json{{"control", json::array({"goto"})}});
        r.register_type("s.sender", idle, open, json{{"control", json::array({"send"})}});
        const auto constant = [](std::string label) {
            return [label](const neograph::graph::GraphState&) { return label; };
        };
        r.register_condition("s.check", constant("pass"), neograph::graph::ConditionSpec{{"pass", "fail"}, false});
        r.register_condition("s.again", constant("done"), neograph::graph::ConditionSpec{{"again", "done"}, false});
        r.register_condition("s.decide", constant("retry"),
                             neograph::graph::ConditionSpec{{"retry", "abort"}, false});
        return r;
    }();
    return registry;
}

json node(const char* type = "s.task") { return json{{"type", type}}; }
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }
json conditional(const char* from, const char* condition, json routes) {
    return json{{"from", from}, {"condition", condition}, {"routes", std::move(routes)}};
}

json definition(json nodes, json edges, json conditionals = json::array()) {
    return json{{"schema_version", 1},
                {"name", "g"},
                {"nodes", std::move(nodes)},
                {"edges", std::move(edges)},
                {"conditional_edges", std::move(conditionals)}};
}

// A fans out to B and C; B routes pass -> D, fail -> FIX; FIX and C go to D.
json issue_357(json wait_for) {
    json nodes{{"A", node()}, {"B", node()}, {"C", node()}, {"FIX", node()}, {"D", node()}};
    nodes["D"]["barrier"] = json{{"wait_for", std::move(wait_for)}};
    return definition(std::move(nodes),
                      json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"), edge("C", "D"),
                                   edge("FIX", "D"), edge("D", "__end__")}),
                      json::array({conditional("B", "s.check", json{{"pass", "D"}, {"fail", "FIX"}})}));
}

SoundnessReport check(const json& graph, std::size_t cap = 100000) {
    const auto topology = GraphCompiler::parse_local(graph, unit_registry());
    return check_soundness(topology, unit_registry(), SoundnessOptions{cap, true});
}

std::vector<CoreDiagnostic> with(const SoundnessReport& report, const char* code) {
    std::vector<CoreDiagnostic> result;
    for (const auto& finding : report.findings)
        if (finding.code == code) result.push_back(finding);
    return result;
}

std::string codes(const SoundnessReport& report) {
    std::string result;
    for (const auto& finding : report.findings) result += " " + finding.code;
    return result;
}

json parsed(const char* text) { return json::parse(text); }

}  // namespace

// ---------------------------------------------------------------------------
// Findings on hand-written graphs
// ---------------------------------------------------------------------------

TEST(Soundness, Issue357LegacyBarrierCannotCompleteOnTheFailRoute) {
    const auto report = check(issue_357(json::array({"B", "C"})));
    EXPECT_EQ(report.status, SoundnessStatus::Checked);
    EXPECT_TRUE(report.outcomes.completed);
    EXPECT_TRUE(report.outcomes.stalled);
    EXPECT_FALSE(report.outcomes.cyclic);
    EXPECT_EQ(codes(report), " S1 S2") << codes(report);

    const auto s1 = with(report, "S1");
    ASSERT_EQ(s1.size(), 1U);
    EXPECT_EQ(s1[0].severity, "warning");
    EXPECT_EQ(s1[0].json_pointer, "/conditional_edges/0");
    EXPECT_EQ(dump(s1[0].witness.at("trace")),
              dump(parsed(R"([{"ready":["A"],"routes":{}},{"ready":["B","C"],"routes":{"B":"fail"}}])")));
    EXPECT_EQ(dump(s1[0].witness.at("state")), dump(parsed(R"({"ready":["FIX"],"pending":{"D":["C"]}})")));
    EXPECT_EQ(dump(s1[0].witness.at("stalls_at")), dump(parsed(R"(["D"])")));
    EXPECT_EQ(dump(s1[0].witness.at("ends")), dump(parsed(R"(["stalled"])")));
    EXPECT_EQ(s1[0].message,
              "After A -> B ('fail'), C the run can no longer complete: every continuation ends "
              "barrier-stalled at 'D'");

    const auto s2 = with(report, "S2");
    ASSERT_EQ(s2.size(), 1U);
    EXPECT_EQ(s2[0].json_pointer, "/nodes/D/barrier");
    const auto& witness = s2[0].witness;
    EXPECT_EQ(witness.at("kind").get<std::string>(), "stalled");
    EXPECT_EQ(dump(witness.at("trace")),
              dump(parsed(R"([{"ready":["A"],"routes":{}},{"ready":["B","C"],"routes":{"B":"fail"}},
                              {"ready":["FIX"],"routes":{}}])")));
    EXPECT_EQ(dump(witness.at("received")), dump(parsed(R"(["C","FIX"])")));
    EXPECT_EQ(dump(witness.at("missing")), dump(parsed(R"(["B"])")));
    EXPECT_EQ(witness.at("substitute").get<std::string>(), "FIX");
    EXPECT_EQ(dump(witness.at("suggested_wait_for")), dump(parsed(R"([{"any_of":["B","FIX"]},"C"])")));
    EXPECT_EQ(dump(witness.at("choices")),
              dump(parsed(R"([{"node":"B","route":"fail","pointer":"/conditional_edges/0"}])")));

    EXPECT_EQ(report.summary.code, "SOUNDNESS");
    EXPECT_EQ(report.summary.severity, "note");
    EXPECT_EQ(report.summary.witness.at("status").get<std::string>(), "checked");
    EXPECT_EQ(dump(report.summary.witness.at("outcomes")), dump(parsed(R"(["completed","stalled"])")));
    EXPECT_EQ(dump(report.summary.witness.at("findings")), dump(parsed(R"({"S1":1,"S2":1})")));
}

TEST(Soundness, AnAnyOfSlotMakesIssue357Sound) {
    const auto report = check(issue_357(json::array({json{{"any_of", json::array({"B", "FIX"})}}, "C"})));
    EXPECT_EQ(report.status, SoundnessStatus::Checked);
    EXPECT_TRUE(report.findings.empty()) << codes(report);
    EXPECT_TRUE(report.outcomes.completed);
    EXPECT_FALSE(report.outcomes.stalled);
    EXPECT_EQ(report.summary.severity, "note");
    EXPECT_NE(report.summary.message.find("no findings"), std::string::npos) << report.summary.message;
}

TEST(Soundness, AnExplicitEndOverWaitingSignalsIsImproperCompletion) {
    // A -> {B, J}; J waits for A and B. B routes pass -> J, fail -> X, and X
    // exits: the run reports completed while J still holds A's signal.
    json nodes{{"A", node()}, {"B", node()}, {"X", node()}, {"J", node()}};
    nodes["J"]["barrier"] = json{{"wait_for", json::array({"A", "B"})}};
    const auto report = check(definition(
        std::move(nodes),
        json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "J"), edge("X", "__end__"),
                     edge("J", "__end__")}),
        json::array({conditional("B", "s.check", json{{"pass", "J"}, {"fail", "X"}})})));
    EXPECT_EQ(codes(report), " S2");
    EXPECT_TRUE(report.outcomes.completed_over_pending);
    const auto s2 = with(report, "S2");
    ASSERT_EQ(s2.size(), 1U);
    EXPECT_EQ(s2[0].witness.at("kind").get<std::string>(), "completed");
    EXPECT_EQ(dump(s2[0].witness.at("received")), dump(parsed(R"(["A"])")));
    EXPECT_FALSE(s2[0].witness.contains("suggested_wait_for"));
}

TEST(Soundness, AJoinOfExclusiveBranchesNeverFires) {
    // A routes pass -> B, fail -> C; J waits for both, so no run fills it and
    // no run can complete at all.
    json nodes{{"A", node()}, {"B", node()}, {"C", node()}, {"J", node()}};
    nodes["J"]["barrier"] = json{{"wait_for", json::array({"B", "C"})}};
    const auto report = check(definition(
        std::move(nodes),
        json::array({edge("__start__", "A"), edge("B", "J"), edge("C", "J"), edge("J", "__end__")}),
        json::array({conditional("A", "s.check", json{{"pass", "B"}, {"fail", "C"}})})));
    EXPECT_EQ(codes(report), " S1 S2 S3");
    const auto s1 = with(report, "S1");
    ASSERT_EQ(s1.size(), 1U);
    EXPECT_EQ(dump(s1[0].witness.at("trace")), "[]");
    EXPECT_EQ(s1[0].json_pointer, "");
    EXPECT_EQ(s1[0].message, "No run can complete: every run ends barrier-stalled at 'J'");
    const auto s3 = with(report, "S3");
    ASSERT_EQ(s3.size(), 1U);
    EXPECT_EQ(s3[0].witness.at("kind").get<std::string>(), "unfireable");
    EXPECT_EQ(dump(s3[0].witness.at("received")), dump(parsed(R"(["B","C"])")));
    EXPECT_EQ(dump(s3[0].witness.at("slots_never_filled")), "[]");
    EXPECT_EQ(s3[0].message, "Barrier 'J' never fires: no run fills all of its slots together");
}

TEST(Soundness, AJoinNoRunSignalsIsDead) {
    json nodes{{"A", node()}, {"X", node()}, {"J", node()}};
    nodes["J"]["barrier"] = json{{"wait_for", json::array({"X"})}};
    const auto report = check(definition(
        std::move(nodes),
        json::array({edge("__start__", "A"), edge("A", "__end__"), edge("X", "J"), edge("J", "__end__")})));
    EXPECT_EQ(codes(report), " S3");
    const auto s3 = with(report, "S3");
    ASSERT_EQ(s3.size(), 1U);
    EXPECT_EQ(s3[0].witness.at("kind").get<std::string>(), "dead");
    EXPECT_EQ(dump(s3[0].witness.at("slots_never_filled")), dump(parsed(R"(["X"])")));
}

TEST(Soundness, ALoopWithoutAnExitIsReportedOnceAsS4) {
    const auto report = check(definition(
        json{{"A", node()}, {"B", node()}, {"C", node()}},
        json::array({edge("__start__", "A"), edge("A", "B"), edge("B", "C"), edge("C", "B")})));
    EXPECT_EQ(codes(report), " S4");  // a loop alone is not also S1
    EXPECT_TRUE(report.outcomes.cyclic);
    EXPECT_FALSE(report.outcomes.completed);
    const auto s4 = with(report, "S4");
    ASSERT_EQ(s4.size(), 1U);
    EXPECT_EQ(s4[0].json_pointer, "/nodes/B");
    EXPECT_EQ(dump(s4[0].witness.at("nodes")), dump(parsed(R"(["B","C"])")));
    EXPECT_EQ(dump(s4[0].witness.at("trace")), dump(parsed(R"([{"ready":["A"],"routes":{}}])")));
    EXPECT_EQ(dump(s4[0].witness.at("cycle")),
              dump(parsed(R"([{"ready":["B"],"routes":{}},{"ready":["C"],"routes":{}}])")));
    EXPECT_EQ(s4[0].message,
              "After A the run enters a loop through 'B' and 'C' that it never leaves; it stops only "
              "at the step limit");

    // A conditional exit makes the loop sound: it may repeat, but can end.
    const auto exits = check(definition(
        json{{"A", node()}, {"B", node()}, {"C", node()}},
        json::array({edge("__start__", "A"), edge("A", "B"), edge("C", "B")}),
        json::array({conditional("B", "s.again", json{{"again", "C"}, {"done", "__end__"}})})));
    EXPECT_TRUE(exits.findings.empty()) << codes(exits);
    EXPECT_TRUE(exits.outcomes.cyclic);
    EXPECT_TRUE(exits.outcomes.completed);
}

TEST(Soundness, OverwriteWritersReadyTogetherBeyondFanOutAreS5) {
    // A -> {B, X}; B -> D; X -> C: C and D run in the same super-step and both
    // write "out", though they are not fan-out siblings (E5 misses them).
    auto graph = definition(json{{"A", node()}, {"B", node()}, {"X", node()}, {"C", node("s.writer")},
                                 {"D", node("s.writer")}},
                            json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "X"),
                                         edge("B", "D"), edge("X", "C"), edge("C", "__end__"),
                                         edge("D", "__end__")}));
    graph["channels"] = json{{"out", json{{"reducer", "overwrite"}}}};
    const auto report = check(graph);
    EXPECT_EQ(codes(report), " S5");
    const auto s5 = with(report, "S5");
    ASSERT_EQ(s5.size(), 1U);
    EXPECT_EQ(s5[0].json_pointer, "/channels/out");
    EXPECT_EQ(dump(s5[0].witness.at("writers")), dump(parsed(R"(["C","D"])")));
    EXPECT_EQ(dump(s5[0].witness.at("state")), dump(parsed(R"({"ready":["C","D"]})")));

    // Direct fan-out siblings are E5's finding and are not repeated.
    auto siblings = definition(json{{"A", node()}, {"C", node("s.writer")}, {"D", node("s.writer")}},
                               json::array({edge("__start__", "A"), edge("A", "C"), edge("A", "D"),
                                            edge("C", "__end__"), edge("D", "__end__")}));
    siblings["channels"] = json{{"out", json{{"reducer", "overwrite"}}}};
    EXPECT_TRUE(check(siblings).findings.empty());
    // An append channel is commutative enough: no finding.
    graph["channels"] = json{{"out", json{{"reducer", "append"}}}};
    EXPECT_TRUE(check(graph).findings.empty());
}

TEST(Soundness, FailureEdgesAreRouteChoicesAndFailureExitsEndTheRun) {
    // The #364 fix loop: TEST fails over to ASK_USER (at most three visits),
    // which retries IMPLEMENT or aborts through a failure exit.
    auto graph = definition(
        json{{"IMPLEMENT", node()}, {"TEST", node()}, {"ASK_USER", node()}, {"ABORT", node()}},
        json::array({edge("__start__", "IMPLEMENT"), edge("IMPLEMENT", "TEST"), edge("TEST", "__end__")}),
        json::array({conditional("ASK_USER", "s.decide", json{{"retry", "IMPLEMENT"}, {"abort", "ABORT"}})}));
    graph["node_retry_policies"] = json{{"TEST", json{{"max_retries", 2}, {"max_visits", 3}}}};
    graph["failure_edges"]       = json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}}});
    graph["failure_exits"]       = json{{"ABORT", json::object()}};
    const auto report            = check(graph);
    EXPECT_EQ(report.status, SoundnessStatus::Checked);
    EXPECT_TRUE(report.findings.empty()) << codes(report);
    EXPECT_TRUE(report.outcomes.completed);
    EXPECT_TRUE(report.outcomes.failure_exit);
    EXPECT_TRUE(report.outcomes.cyclic);
    EXPECT_FALSE(report.outcomes.failed);
    EXPECT_FALSE(report.outcomes.stalled);
}

TEST(Soundness, AFailureRouteCanLeaveAJoinWaiting) {
    // X and Y join at J; X's failure edge leads to H, which exits.
    json nodes{{"A", node()}, {"X", node()}, {"Y", node()}, {"H", node()}, {"J", node()}};
    nodes["J"]["barrier"] = json{{"wait_for", json::array({"X", "Y"})}};
    auto graph = definition(std::move(nodes),
                            json::array({edge("__start__", "A"), edge("A", "X"), edge("A", "Y"),
                                         edge("X", "J"), edge("Y", "J"), edge("H", "__end__"),
                                         edge("J", "__end__")}));
    graph["failure_edges"] = json::array({json{{"from", "X"}, {"to", "H"}}});
    const auto report      = check(graph);
    EXPECT_EQ(codes(report), " S2");
    const auto s2 = with(report, "S2");
    ASSERT_EQ(s2.size(), 1U);
    EXPECT_EQ(s2[0].witness.at("kind").get<std::string>(), "completed");
    EXPECT_EQ(dump(s2[0].witness.at("choices")),
              dump(parsed(R"([{"node":"X","route":"on_failure","pointer":"/failure_edges/0"}])")));
}

TEST(Soundness, AVisitLimitWithoutAFailureEdgeFailsTheRun) {
    // A -> B; B routes again -> A or done -> __end__; A may run twice.
    auto graph = definition(json{{"A", node()}, {"B", node()}},
                            json::array({edge("__start__", "A"), edge("A", "B")}),
                            json::array({conditional("B", "s.again", json{{"again", "A"}, {"done", "__end__"}})}));
    graph["node_retry_policies"] = json{{"A", json{{"max_visits", 2}}}};
    const auto report            = check(graph);
    EXPECT_TRUE(report.outcomes.failed);
    EXPECT_TRUE(report.outcomes.completed);
    EXPECT_FALSE(report.outcomes.cyclic);
    EXPECT_EQ(codes(report), " S1");
    const auto s1 = with(report, "S1");
    ASSERT_EQ(s1.size(), 1U);
    EXPECT_EQ(dump(s1[0].witness.at("state")), dump(parsed(R"({"ready":["A"],"visits":{"A":2}})")));
    EXPECT_EQ(dump(s1[0].witness.at("ends")), dump(parsed(R"(["failed"])")));
    EXPECT_NE(s1[0].message.find("fails at a visit limit"), std::string::npos) << s1[0].message;
}

TEST(Soundness, DeclaredGotoOrSendLeavesARegionNotAnalyzable) {
    for (const auto* type : {"s.router", "s.sender"}) {
        const auto report = check(definition(
            json{{"A", node()}, {"R", node(type)}, {"B", node()}},
            json::array({edge("__start__", "A"), edge("A", "R"), edge("R", "B"), edge("B", "__end__")})));
        EXPECT_EQ(report.status, SoundnessStatus::Partial) << type;
        EXPECT_TRUE(report.findings.empty()) << type << codes(report);
        EXPECT_EQ(report.not_analyzable, std::vector<std::string>{"R"}) << type;
        EXPECT_EQ(report.summary.severity, "warning") << type;
        const auto& entry = report.summary.witness.at("not_analyzable")[0];
        EXPECT_EQ(entry.at("type").get<std::string>(), type);
        EXPECT_EQ(dump(entry.at("trace")), dump(parsed(R"([{"ready":["A"],"routes":{}}])")));
    }
    // A finding reached before the region still stands; a dead-join claim,
    // which needs every run, is not made.
    auto graph = issue_357(json::array({"B", "C"}));
    graph["nodes"]["R"] = node("s.router");
    graph["edges"]      = json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"), edge("C", "D"),
                                       edge("FIX", "D"), edge("D", "R"), edge("R", "__end__")});
    const auto report   = check(graph);
    EXPECT_EQ(report.status, SoundnessStatus::Partial);
    EXPECT_EQ(codes(report), " S1 S2");
}

TEST(Soundness, TheStateCapReportsNotChecked) {
    const auto report = check(issue_357(json::array({"B", "C"})), 2);
    EXPECT_EQ(report.status, SoundnessStatus::NotChecked);
    EXPECT_EQ(report.summary.severity, "warning");
    EXPECT_EQ(report.summary.witness.at("status").get<std::string>(), "not_checked");
    EXPECT_EQ(report.summary.witness.at("state_cap").get<std::size_t>(), 2U);
    EXPECT_TRUE(with(report, "S1").empty());
    EXPECT_TRUE(with(report, "S3").empty());
    EXPECT_TRUE(with(report, "S4").empty());
    EXPECT_NE(report.summary.message.find("state cap of 2"), std::string::npos) << report.summary.message;
}

TEST(Soundness, TheValidatorRunsTheCheckOnlyWhenAsked) {
    const auto topology = GraphCompiler::parse_local(issue_357(json::array({"B", "C"})), unit_registry());
    const auto count    = [](const neograph::graph::ValidationReport& report) {
        std::string result;
        for (const auto& d : report.diagnostics)
            if (d.code == "SOUNDNESS" || d.code[0] == 'S') result += " " + d.code + "/" + d.severity;
        return result;
    };
    ValidationOptions options;
    EXPECT_EQ(count(GraphValidator::validate_local(topology, unit_registry(), options)), "");
    options.soundness_check = true;
    EXPECT_EQ(count(GraphValidator::validate_local(topology, unit_registry(), options)),
              " S1/warning S2/warning SOUNDNESS/note");
    EXPECT_EQ(count(GraphValidator::validate(topology, unit_registry(), options)),
              " S1/warning S2/warning SOUNDNESS/note");
    options.original_rules = true;
    EXPECT_EQ(count(GraphValidator::validate_local(topology, unit_registry(), options)), "");

    // A graph with errors is not explored.
    auto broken = issue_357(json::array({"B", "C"}));
    broken["edges"].push_back(edge("C", "NOWHERE"));
    const auto with_error = GraphCompiler::parse_local(broken, unit_registry());
    ValidationOptions checked;
    checked.soundness_check = true;
    const auto report       = GraphValidator::validate_local(with_error, unit_registry(), checked);
    EXPECT_TRUE(report.has_errors());
    EXPECT_EQ(count(report), "");
}

// ---------------------------------------------------------------------------
// Through the Program compiler: returned with help, never sealed
// ---------------------------------------------------------------------------

#if defined(NEOGRAPH_SOUNDNESS_HAVE_PROGRAM)

namespace {

json program_357(json wait_for) {
    json nodes{{"A", json{{"type", "t.task"}}}, {"B", json{{"type", "t.task"}}}, {"C", json{{"type", "t.task"}}},
               {"FIX", json{{"type", "t.task"}}}, {"D", json{{"type", "t.task"}}}};
    nodes["D"]["barrier"] = json{{"wait_for", std::move(wait_for)}};
    return program_v2_test::program_document(json{
        {"schema_version", 1},
        {"name", "main"},
        {"nodes", std::move(nodes)},
        {"edges", json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"), edge("C", "D"),
                               edge("FIX", "D"), edge("D", "__end__")})},
        {"conditional_edges",
         json::array({conditional("B", "t.check", json{{"pass", "D"}, {"fail", "FIX"}})})}});
}

neograph::program::ProgramCompilerConfig soundness_config(std::size_t cap = 100000) {
    neograph::program::ProgramCompilerConfig config{"program-v2-test/v1"};
    config.soundness_check     = true;
    config.soundness_state_cap = cap;
    return config;
}

}  // namespace

TEST(SoundnessProgram, FindingsAreReturnedWithHelpAndNeverSealed) {
    using namespace program_v2_test;
    const auto source = cpp_source(program_357(json::array({"B", "C"})));
    const auto plain  = compile(source);
    const auto sound  = compile(source, compiler(soundness_config()));
    ASSERT_TRUE(plain.ok()) << codes(plain.returned);
    ASSERT_TRUE(sound.ok()) << codes(sound.returned);
    EXPECT_TRUE(with_code(plain.returned, "P_CORE_SOUNDNESS").empty());

    // The bundle, its id and its sealed diagnostics do not depend on the check.
    EXPECT_EQ(sound.bundle->id(), plain.bundle->id());
    EXPECT_EQ(sound.bundle->serialize_canonical(), plain.bundle->serialize_canonical());
    for (const auto& sealed : sound.bundle->diagnostics())
        EXPECT_EQ(sealed.code.rfind("P_CORE_S", 0), std::string::npos) << sealed.code;

    const auto s1 = with_code(sound.returned, "P_CORE_S1");
    ASSERT_EQ(s1.size(), 1U);
    EXPECT_EQ(s1[0].severity, DiagnosticSeverity::Warning);
    ASSERT_TRUE(s1[0].help.has_value());
    EXPECT_EQ(s1[0].help->doc, "docs/diagnostics.md#p_core_s1");
    EXPECT_EQ(s1[0].witness.at("core_code").get<std::string>(), "S1");

    const auto s2 = with_code(sound.returned, "P_CORE_S2");
    ASSERT_EQ(s2.size(), 1U);
    EXPECT_EQ(s2[0].primary.json_pointer, "/root/definition/nodes/D/barrier");
    ASSERT_TRUE(s2[0].help.has_value());
    EXPECT_EQ(s2[0].help->doc, "docs/diagnostics.md#p_core_s2");
    ASSERT_EQ(s2[0].help->suggestions.size(), 1U);
    const auto& suggestion = s2[0].help->suggestions[0];
    EXPECT_EQ(suggestion.applicability, SuggestionApplicability::Maybe);
    ASSERT_EQ(suggestion.edits.size(), 1U);
    EXPECT_EQ(suggestion.edits[0].op, "replace");
    EXPECT_EQ(suggestion.edits[0].path, "/root/definition/nodes/D/barrier/wait_for");
    EXPECT_EQ(canonical(suggestion.edits[0].value), canonical(json::parse(R"([{"any_of":["B","FIX"]},"C"])")));
    ASSERT_FALSE(s2[0].help->related.empty());
    EXPECT_EQ(s2[0].help->related[0].label, "'B' takes route 'fail' here");

    const auto summary = with_code(sound.returned, "P_CORE_SOUNDNESS");
    ASSERT_EQ(summary.size(), 1U);
    EXPECT_EQ(summary[0].severity, DiagnosticSeverity::Note);
    EXPECT_EQ(summary[0].help->doc, "docs/diagnostics.md#p_core_soundness");
    EXPECT_EQ(summary[0].witness.at("status").get<std::string>(), "checked");
}

TEST(SoundnessProgram, AV2DefinitionStaysUnsealedAndSoundWithTheSlot) {
    using namespace program_v2_test;
    const auto source = cpp_source(program_357(json::array({json{{"any_of", json::array({"B", "FIX"})}}, "C"})));
    const auto plain  = compile(source);
    const auto sound  = compile(source, compiler(soundness_config()));
    ASSERT_TRUE(sound.ok()) << codes(sound.returned);
    EXPECT_EQ(sound.bundle->serialize_canonical(), plain.bundle->serialize_canonical());
    EXPECT_TRUE(sound.bundle->diagnostics().empty());
    for (const auto* code : {"P_CORE_S1", "P_CORE_S2", "P_CORE_S3", "P_CORE_S4", "P_CORE_S5"})
        EXPECT_TRUE(with_code(sound.returned, code).empty()) << code;
    EXPECT_EQ(with_code(sound.returned, "P_CORE_SOUNDNESS").size(), 1U);
}

TEST(SoundnessProgram, ACappedCheckIsAWarningThatClaimsNothing) {
    using namespace program_v2_test;
    const auto sound = compile(cpp_source(program_357(json::array({"B", "C"}))), compiler(soundness_config(2)));
    ASSERT_TRUE(sound.ok()) << codes(sound.returned);
    const auto summary = with_code(sound.returned, "P_CORE_SOUNDNESS");
    ASSERT_EQ(summary.size(), 1U);
    EXPECT_EQ(summary[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(summary[0].witness.at("status").get<std::string>(), "not_checked");
    EXPECT_TRUE(with_code(sound.returned, "P_CORE_S1").empty());
}

TEST(SoundnessProgram, TheCapabilityManifestPublishesTheCheck) {
    const auto manifest = neograph::program::javascript_authoring_capability_manifest();
    EXPECT_TRUE(manifest.at("features").at("diagnostics.soundness_check").get<bool>());
}

#if defined(NEOGRAPH_SOUNDNESS_HAVE_QUICKJS)

TEST(SoundnessProgram, JavaScriptHelpSpellsTheBuilderCalls) {
    using namespace program_v2_test;
    const auto sound = compile(js_source(R"JS(export function define() {
        return ng.graph("main")
            .node("A", {type: "t.task"}).node("B", {type: "t.task"}).node("C", {type: "t.task"})
            .node("FIX", {type: "t.task"}).node("D", {type: "t.task"})
            .entry("A").edge("A", "B").edge("A", "C")
            .conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
            .edge("FIX", "D").edge("C", "D")
            .barrier("D", ["B", "C"]).exit("D");
    })JS"),
                               compiler(soundness_config()));
    ASSERT_TRUE(sound.ok()) << codes(sound.returned);
    const auto s2 = with_code(sound.returned, "P_CORE_S2");
    ASSERT_EQ(s2.size(), 1U);
    ASSERT_TRUE(s2[0].help.has_value());
    ASSERT_EQ(s2[0].help->suggestions.size(), 1U);
    EXPECT_EQ(s2[0].help->suggestions[0].message,
              "Let 'FIX' fill the missing slot: graph.barrier(\"D\", [{\"any_of\":[\"B\",\"FIX\"]},\"C\"])");
    // The route choice points at the builder call that declared it.
    ASSERT_FALSE(s2[0].help->related.empty());
    EXPECT_TRUE(s2[0].help->related[0].coordinate.span.has_value());
}

TEST(SoundnessProgram, ALoopSuggestsARouteOutOfIt) {
    using namespace program_v2_test;
    const auto sound = compile(js_source(R"JS(export function define() {
        return ng.graph("main")
            .node("A", {type: "t.task"}).node("B", {type: "t.task"}).node("C", {type: "t.task"})
            .entry("A").edge("A", "B")
            .conditionalEdge("B", "t.check", {pass: "C", fail: "A"})
            .edge("C", "B");
    })JS"),
                               compiler(soundness_config()));
    ASSERT_TRUE(sound.ok()) << codes(sound.returned);
    const auto s4 = with_code(sound.returned, "P_CORE_S4");
    ASSERT_EQ(s4.size(), 1U) << codes(sound.returned);
    ASSERT_TRUE(s4[0].help.has_value());
    ASSERT_EQ(s4[0].help->suggestions.size(), 1U);
    const auto& suggestion = s4[0].help->suggestions[0];
    EXPECT_EQ(suggestion.applicability, SuggestionApplicability::Placeholder);
    ASSERT_EQ(suggestion.edits.size(), 1U);
    EXPECT_EQ(suggestion.edits[0].op, "add");
    EXPECT_NE(suggestion.edits[0].path.find("/routes/<label>"), std::string::npos) << suggestion.edits[0].path;
    EXPECT_EQ(suggestion.edits[0].value.get<std::string>(), "__end__");
}

#endif  // NEOGRAPH_SOUNDNESS_HAVE_QUICKJS
#endif  // NEOGRAPH_SOUNDNESS_HAVE_PROGRAM

// ---------------------------------------------------------------------------
// Agreement with the reference interpreter of the join harness
// ---------------------------------------------------------------------------

namespace {

using namespace join_model;

// Every model node writes the overwrite channel "shared", so S5 is the
// question whether two nodes can run in one super-step. The node that may
// return Command.goto(__end__) has a type that declares it.
const GraphRegistry& model_registry() {
    static const GraphRegistry registry = [] {
        GraphRegistry r;
        add_reducers(r);
        const json open{{"type", "object"}};
        r.register_type("join_model.node", idle, open, json{{"writes", json::array({"shared"})}});
        r.register_type("join_model.ender", idle, open,
                        json{{"writes", json::array({"shared"})}, {"control", json::array({"goto"})}});
        for (int u = 0; u < kMaxNodes; ++u) {
            const auto channel = "route_" + node_name(u);
            r.register_condition(
                "join_model.route." + node_name(u),
                [channel](const neograph::graph::GraphState& state) { return state.get(channel).get<std::string>(); },
                neograph::graph::ConditionSpec{{"a", "b"}, false});
        }
        return r;
    }();
    return registry;
}

json model_definition(const Graph& graph) {
    auto result = join_model::definition(graph, 1, false);
    result["channels"]["shared"] = json{{"reducer", "overwrite"}};
    if (graph.ender >= 0) result["nodes"][node_name(graph.ender)]["type"] = "join_model.ender";
    return result;
}

// What the reference interpreter reaches, under the abstraction the check
// promises: a state in which the ender is ready is not explored.
struct Reference {
    std::vector<std::pair<std::uint32_t, std::uint32_t>>      states;  // (ready, pending)
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> index;
    std::vector<std::vector<std::size_t>>                     successors;
    std::vector<char> complete_exit, any_exit, stall_exit, opaque;
    std::vector<char> can_complete, can_end, can_stall;
    bool completed = false, over_pending = false, stalled = false, cyclic = false;
    bool dead_end = false, livelock = false, fires = false, signalled = false;
    bool co_ready = false, partial = false;

    std::optional<std::size_t> find(std::uint32_t ready, std::uint32_t pending) const {
        const auto found = index.find({ready, pending});
        if (found == index.end()) return std::nullopt;
        return found->second;
    }
};

std::vector<char> closure(const Reference& ref, const std::vector<char>& seeds) {
    std::vector<std::vector<std::size_t>> predecessors(ref.states.size());
    for (std::size_t s = 0; s < ref.states.size(); ++s)
        for (const auto t : ref.successors[s]) predecessors[t].push_back(s);
    std::vector<char>        reached = seeds;
    std::vector<std::size_t> queue;
    for (std::size_t s = 0; s < reached.size(); ++s)
        if (reached[s]) queue.push_back(s);
    while (!queue.empty()) {
        const auto s = queue.back();
        queue.pop_back();
        for (const auto p : predecessors[s])
            if (!reached[p]) {
                reached[p] = 1;
                queue.push_back(p);
            }
    }
    return reached;
}

Reference explore_reference(const Graph& graph) {
    Reference  ref;
    const auto intern = [&](std::uint32_t ready, std::uint32_t pending) {
        const auto [found, inserted] = ref.index.emplace(std::pair{ready, pending}, ref.states.size());
        if (inserted) {
            ref.states.push_back({ready, pending});
            ref.successors.emplace_back();
            for (auto* flags : {&ref.complete_exit, &ref.any_exit, &ref.stall_exit, &ref.opaque})
                flags->push_back(0);
        }
        return found->second;
    };
    // Pairs E5 reports: static fan-out siblings.
    std::set<std::pair<int, int>> siblings;
    for (int u = 0; u < graph.n; ++u) {
        if (graph.routes[u].conditional) continue;
        for (const auto a : graph.routes[u].targets)
            for (const auto b : graph.routes[u].targets)
                if (a != kEnd && b != kEnd && a < b) siblings.insert({a, b});
    }
    intern(bit(0), 0);
    for (std::size_t i = 0; i < ref.states.size(); ++i) {
        const auto [ready, pending] = ref.states[i];
        for (int a = 0; a < graph.n; ++a)
            for (int b = a + 1; b < graph.n; ++b)
                if ((ready & bit(a)) && (ready & bit(b)) && !siblings.count({a, b})) ref.co_ready = true;
        if (graph.ender >= 0 && (ready & bit(graph.ender))) {
            ref.opaque[i] = 1;
            ref.partial   = true;
            continue;
        }
        for_each_choice(graph, ready, [&](Choice choice) {
            const auto next = reference_step(graph, ready, pending, choice);
            if (next.arrivals) ref.signalled = true;
            if (next.fired) ref.fires = true;
            if (next.ready == 0) {
                ref.any_exit[i] = 1;
                if (next.pending == 0) {
                    ref.completed        = true;
                    ref.complete_exit[i] = 1;
                } else if (next.explicit_end) {
                    ref.over_pending     = true;
                    ref.complete_exit[i] = 1;
                } else {
                    ref.stalled       = true;
                    ref.stall_exit[i] = 1;
                }
                return;
            }
            const auto j = intern(next.ready, next.pending);
            ref.successors[i].push_back(j);
        });
    }
    // A cycle exists when peeling states without predecessors leaves some.
    std::vector<std::size_t> in(ref.states.size(), 0);
    for (const auto& successors : ref.successors)
        for (const auto t : successors) ++in[t];
    std::vector<std::size_t> queue;
    for (std::size_t s = 0; s < in.size(); ++s)
        if (in[s] == 0) queue.push_back(s);
    std::size_t peeled = 0;
    while (!queue.empty()) {
        const auto s = queue.back();
        queue.pop_back();
        ++peeled;
        for (const auto t : ref.successors[s])
            if (--in[t] == 0) queue.push_back(t);
    }
    ref.cyclic = peeled < ref.states.size();
    std::vector<char> complete_seeds(ref.states.size()), end_seeds(ref.states.size());
    for (std::size_t s = 0; s < ref.states.size(); ++s) {
        complete_seeds[s] = ref.complete_exit[s] || ref.opaque[s];
        end_seeds[s]      = ref.any_exit[s] || ref.opaque[s];
    }
    ref.can_complete = closure(ref, complete_seeds);
    ref.can_end      = closure(ref, end_seeds);
    ref.can_stall    = closure(ref, ref.stall_exit);
    for (std::size_t s = 0; s < ref.states.size(); ++s) {
        if (!ref.can_complete[s] && ref.can_stall[s]) ref.dead_end = true;
        if (!ref.can_end[s]) ref.livelock = true;
    }
    return ref;
}

std::uint32_t mask_of(const json& names) {
    std::uint32_t result = 0;
    for (const auto& name : names) result |= bit(std::stoi(name.get<std::string>().substr(1)));
    return result;
}

struct Replayed {
    std::uint32_t ready = 0, pending = 0;
    Step          last;
    bool          ended = false;  // the last step left nothing ready
};

// Replays witness steps from (ready, pending) through the reference.
std::optional<Replayed> replay(const Graph& graph, const json& steps, std::uint32_t ready,
                               std::uint32_t pending, std::string& problem) {
    Replayed out{ready, pending, {}, false};
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        if (out.ended) {
            problem = "the trace continues after the run ended";
            return std::nullopt;
        }
        if (mask_of(step["ready"]) != out.ready) {
            problem = "step " + std::to_string(i) + " ready " + step["ready"].dump() + ", reference " +
                      mask_names(out.ready, graph.n);
            return std::nullopt;
        }
        Choice choice;
        for (const auto& [name, label] : step["routes"].items()) {
            const int  u     = std::stoi(name.substr(1));
            const auto value = label.get<std::string>();
            if (!graph.routes[u].conditional || (value != "a" && value != "b")) {
                problem = "step " + std::to_string(i) + " routes " + step["routes"].dump();
                return std::nullopt;
            }
            if (value == "b") choice.label_b |= bit(u);
        }
        out.last    = reference_step(graph, out.ready, out.pending, choice);
        out.ready   = out.last.ready;
        out.pending = out.last.pending;
        out.ended   = out.last.ready == 0;
    }
    return out;
}

struct AgreementTotals {
    std::uint64_t graphs = 0, any_of_graphs = 0, partial_graphs = 0, states = 0, transitions = 0;
    std::uint64_t clean = 0, s1 = 0, s2_stalled = 0, s2_completed = 0, s3_dead = 0, s3_unfireable = 0;
    std::uint64_t s4 = 0, s5 = 0, cyclic = 0, witnesses = 0, failure_count = 0;
    std::vector<std::string> failures;

    void fail(std::string message) {
        ++failure_count;
        if (failures.size() < 8) failures.push_back(std::move(message));
    }
};

void compare(const Graph& graph, AgreementTotals& totals) {
    const auto topology = GraphCompiler::parse_local(model_definition(graph), model_registry());
    const auto report   = check_soundness(topology, model_registry(), SoundnessOptions{1000000, true});
    const auto ref      = explore_reference(graph);
    ++totals.graphs;
    if (graph.any_of()) ++totals.any_of_graphs;
    if (ref.partial) ++totals.partial_graphs;
    totals.states += report.states;
    totals.transitions += report.transitions;
    if (ref.cyclic) ++totals.cyclic;

    std::vector<std::string> problems;
    const auto expect = [&](bool ok, const std::string& what) {
        if (!ok) problems.push_back(what);
    };
    const auto has = [&](const char* code, const char* kind = nullptr) {
        return std::any_of(report.findings.begin(), report.findings.end(), [&](const CoreDiagnostic& d) {
            return d.code == code && (!kind || d.witness.at("kind").get<std::string>() == kind);
        });
    };
    expect(report.status == (ref.partial ? SoundnessStatus::Partial : SoundnessStatus::Checked), "status");
    expect(report.states == ref.states.size(),
           "states " + std::to_string(report.states) + " vs " + std::to_string(ref.states.size()));
    expect(report.outcomes.completed == ref.completed, "completed");
    expect(report.outcomes.completed_over_pending == ref.over_pending, "completed over pending");
    expect(report.outcomes.stalled == ref.stalled, "stalled");
    expect(report.outcomes.cyclic == ref.cyclic, "cyclic");
    expect(!report.outcomes.failure_exit && !report.outcomes.failed, "failure outcomes");
    expect(has("S1") == ref.dead_end, "S1");
    expect(has("S2", "stalled") == ref.stalled, "S2 stalled");
    expect(has("S2", "completed") == ref.over_pending, "S2 completed");
    expect(has("S3") == (!ref.partial && !ref.fires), "S3");
    if (has("S3")) expect(has("S3", ref.signalled ? "unfireable" : "dead"), "S3 kind");
    expect(has("S4") == ref.livelock, "S4");
    expect(has("S5") == ref.co_ready, "S5");

    // Every witness trace is a run of the reference.
    const std::uint32_t entry = bit(0);
    for (const auto& finding : report.findings) {
        ++totals.witnesses;
        std::string problem;
        const auto& witness = finding.witness;
        if (finding.code == "S3" && witness.at("kind").get<std::string>() == "dead") continue;
        const auto run = replay(graph, witness.at("trace"), entry, 0, problem);
        if (!run) {
            problems.push_back(finding.code + " trace: " + problem);
            continue;
        }
        if (finding.code == "S2") {
            const bool completed = witness.at("kind").get<std::string>() == "completed";
            expect(run->ended && run->pending != 0 && run->last.explicit_end == completed &&
                       mask_of(witness.at("received")) == run->pending,
                   "S2 witness end");
        } else if (finding.code == "S3") {
            expect(run->last.arrivals != 0, "S3 witness arrival");
        } else {
            const auto state = ref.find(run->ready, run->pending);
            expect(!run->ended && state.has_value() && mask_of(witness.at("state").at("ready")) == run->ready,
                   finding.code + " witness state");
            if (!state) continue;
            if (finding.code == "S1") expect(!ref.can_complete[*state] && ref.can_stall[*state], "S1 dead end");
            if (finding.code == "S5") expect(popcount(run->ready) >= 2, "S5 co-ready");
            if (finding.code == "S4") {
                expect(!ref.can_end[*state], "S4 never ends");
                const auto cycle = replay(graph, witness.at("cycle"), run->ready, run->pending, problem);
                expect(cycle && !cycle->ended && cycle->ready == run->ready && cycle->pending == run->pending,
                       "S4 cycle " + problem);
            }
        }
    }
    if (report.summary.witness.contains("not_analyzable")) {
        for (const auto& region : report.summary.witness.at("not_analyzable")) {
            std::string problem;
            const auto  run = replay(graph, region.at("trace"), entry, 0, problem);
            expect(run && !run->ended && (run->ready & bit(graph.ender)), "not analyzable trace " + problem);
        }
    }

    const bool clean = report.findings.empty();
    if (clean) ++totals.clean;
    if (has("S1")) ++totals.s1;
    if (has("S2", "stalled")) ++totals.s2_stalled;
    if (has("S2", "completed")) ++totals.s2_completed;
    if (has("S3", "dead")) ++totals.s3_dead;
    if (has("S3", "unfireable")) ++totals.s3_unfireable;
    if (has("S4")) ++totals.s4;
    if (has("S5")) ++totals.s5;
    if (problems.empty()) return;
    std::ostringstream out;
    out << "soundness check differs from the reference on " << describe(graph) << " (ender "
        << graph.ender << "):";
    for (const auto& problem : problems) out << "\n  " << problem;
    for (const auto& finding : report.findings) out << "\n  " << finding.code << ": " << finding.message;
    totals.fail(out.str());
}

}  // namespace

TEST(SoundnessAgreement, VerdictsMatchTheReferenceOnEveryHarnessGraph) {
    const auto limits = bounds();
    const auto start  = std::chrono::steady_clock::now();
    AgreementTotals totals;
    for_each_graph(limits, [&](const Graph& graph) { compare(graph, totals); });
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[ soundness ] check vs reference (bound " << limits.name << ": nodes<=" << limits.max_nodes
              << ", conditionals<=" << limits.max_conditionals << ", fan-out<=" << limits.max_fanout
              << ", back edges<=" << limits.max_back_edges << ", enders<=" << (limits.enders ? 1 : 0)
              << ")\n  graphs " << totals.graphs << " (any_of " << totals.any_of_graphs
              << ", with a goto node, partial " << totals.partial_graphs << "), states " << totals.states
              << ", transitions " << totals.transitions << ", cyclic " << totals.cyclic
              << "\n  graphs with findings: S1 " << totals.s1 << ", S2 stalled " << totals.s2_stalled
              << ", S2 completed " << totals.s2_completed << ", S3 dead " << totals.s3_dead
              << ", S3 unfireable " << totals.s3_unfireable << ", S4 " << totals.s4 << ", S5 " << totals.s5
              << "; without findings " << totals.clean << "\n  witnesses replayed " << totals.witnesses
              << "\n  mismatches " << totals.failure_count << ", " << seconds << " s\n";
    EXPECT_GT(totals.graphs, 1000U);
    EXPECT_GT(totals.partial_graphs, 0U);
    for (const auto* count : {&totals.s1, &totals.s2_stalled, &totals.s2_completed, &totals.s3_unfireable,
                              &totals.s4, &totals.s5})
        EXPECT_GT(*count, 0U) << "the family must reach every finding";
    EXPECT_EQ(totals.failure_count, 0U);
    for (const auto& failure : totals.failures) ADD_FAILURE() << failure;
}
