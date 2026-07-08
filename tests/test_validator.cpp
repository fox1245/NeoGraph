// M2 static-semantics tests (issue #75): GraphValidator over
// CompiledGraph — dangling references (E3), reachability (E7), barrier
// liveness (E8), unsynchronized fan-in (E9), route completeness (E10),
// trapped cycles (E11), and channel effects (E4/E5/E6).
//
// Fixture style: build the definition JSON, compile leniently (the
// checks under test are the validator's, not the parser's), validate,
// then assert on diagnostic codes + witnesses. Engine-integration
// tests at the bottom assert the strict-mode throw path.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

class VNoopNode : public GraphNode {
public:
    explicit VNoopNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void ensure_vtypes_registered() {
    // vnoop: no declared effects (disables the E4/E5/E6 family).
    NodeFactory::instance().register_type(
        "vnoop",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<VNoopNode>(name);
        });
    // vfx_*: declared effects, for the effect-analysis tests.
    NodeFactory::instance().register_type(
        "vfx_writer",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<VNoopNode>(name);
        },
        json::parse(R"({"type":"object"})"),
        json::parse(R"({"reads":[],"writes":["out"]})"));
    NodeFactory::instance().register_type(
        "vfx_reader",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<VNoopNode>(name);
        },
        json::parse(R"({"type":"object"})"),
        json::parse(R"({"reads":["out"],"writes":[]})"));
    // closed two-label condition for E10 tests.
    ConditionRegistry::instance().register_condition(
        "vcond_binary",
        [](const GraphState&) -> std::string { return "yes"; },
        ConditionSpec{{"no", "yes"}, /*open=*/false});
}

ValidationReport validate_def(const json& def) {
    ensure_vtypes_registered();
    auto cg = GraphCompiler::compile(def, NodeContext{});
    return GraphValidator::validate(cg);
}

// All diagnostics with the given code.
std::vector<const Diagnostic*> by_code(const ValidationReport& r, const std::string& code) {
    std::vector<const Diagnostic*> v;
    for (const auto& d : r.diagnostics) if (d.code == code) v.push_back(&d);
    return v;
}

json two_node_base() {
    return json{
        {"nodes", {{"a", {{"type", "vnoop"}}}, {"b", {{"type", "vnoop"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "a"}, {"to", "b"}},
                                {{"from", "b"}, {"to", "__end__"}} })},
    };
}

} // namespace

// =========================================================================
// E3: dangling references (errors)
// =========================================================================

TEST(Validator, CleanGraphHasNoDiagnostics) {
    auto r = validate_def(two_node_base());
    EXPECT_TRUE(r.diagnostics.empty()) << r.summary();
}

TEST(Validator, E3_DanglingEdgeTarget) {
    auto def = two_node_base();
    def["edges"].push_back({{"from", "a"}, {"to", "ghost"}});
    auto r = validate_def(def);
    auto e3 = by_code(r, "E3");
    ASSERT_EQ(e3.size(), 1u) << r.summary();
    EXPECT_EQ(e3[0]->severity, "error");
    EXPECT_EQ(e3[0]->witness["edge_to"].get<std::string>(), "ghost");
}

TEST(Validator, E3_DanglingRouteTargetAndInterrupt) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "route_channel"},
         {"routes", {{"x", "ghost"}, {"default", "__end__"}}}} });
    def["interrupt_before"] = json::array({"phantom"});
    auto r = validate_def(def);
    auto e3 = by_code(r, "E3");
    ASSERT_EQ(e3.size(), 2u) << r.summary();
}

TEST(Validator, E3_BarrierSelfWaitAndUnknownMember) {
    auto def = two_node_base();
    def["nodes"]["b"]["barrier"] = {{"wait_for", json::array({"b", "ghost"})}};
    auto r = validate_def(def);
    auto e3 = by_code(r, "E3");
    ASSERT_EQ(e3.size(), 2u) << r.summary();   // self-wait + unknown member
}

// =========================================================================
// E7 / E11: reachability (warnings — Command.goto/Send caveat)
// =========================================================================

TEST(Validator, E7_UnreachableNodeWarns) {
    auto def = two_node_base();
    def["nodes"]["island"] = {{"type", "vnoop"}};
    auto r = validate_def(def);
    auto e7 = by_code(r, "E7");
    ASSERT_EQ(e7.size(), 1u) << r.summary();
    EXPECT_EQ(e7[0]->severity, "warning");
    EXPECT_EQ(e7[0]->witness["unreachable"][0].get<std::string>(), "island");
    EXPECT_FALSE(r.has_errors());
}

TEST(Validator, E11_TrappedCycleWarns) {
    // a <-> b cycle with no exit; both have outgoing edges, so the
    // scheduler's implicit-__end__ rule does not apply.
    json def = {
        {"nodes", {{"a", {{"type", "vnoop"}}}, {"b", {{"type", "vnoop"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "a"}, {"to", "b"}},
                                {{"from", "b"}, {"to", "a"}} })},
    };
    auto r = validate_def(def);
    auto e11 = by_code(r, "E11");
    ASSERT_EQ(e11.size(), 1u) << r.summary();
    EXPECT_EQ(e11[0]->witness["trapped"].size(), 2u);
}

TEST(Validator, E11_NoOutgoingMeansImplicitEnd) {
    // Node with zero outgoing edges auto-routes to __end__ — no warning.
    json def = {
        {"nodes", {{"a", {{"type", "vnoop"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}} })},
    };
    auto r = validate_def(def);
    EXPECT_TRUE(by_code(r, "E11").empty()) << r.summary();
}

// =========================================================================
// E8: barrier liveness (error)
// =========================================================================

TEST(Validator, E8_BarrierMemberWithoutSignalPath) {
    auto def = two_node_base();
    // b waits for a — ok (edge a->b exists). Add c waiting on b, but b
    // routes to __end__, never to c... build explicitly:
    def["nodes"]["c"] = {{"type", "vnoop"},
                         {"barrier", {{"wait_for", json::array({"a", "b"})}}}};
    def["edges"].push_back({{"from", "a"}, {"to", "c"}});
    // b -> c edge deliberately missing: b can never signal c.
    auto r = validate_def(def);
    auto e8 = by_code(r, "E8");
    ASSERT_EQ(e8.size(), 1u) << r.summary();
    EXPECT_EQ(e8[0]->severity, "error");
    EXPECT_EQ(e8[0]->witness["waits_for"].get<std::string>(), "b");
}

TEST(Validator, E8_SatisfiedBarrierIsClean) {
    json def = {
        {"nodes", {{"a", {{"type", "vnoop"}}}, {"b", {{"type", "vnoop"}}},
                   {"j", {{"type", "vnoop"},
                          {"barrier", {{"wait_for", json::array({"a", "b"})}}}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "__start__"}, {"to", "b"}},
                                {{"from", "a"}, {"to", "j"}},
                                {{"from", "b"}, {"to", "j"}},
                                {{"from", "j"}, {"to", "__end__"}} })},
    };
    auto r = validate_def(def);
    EXPECT_TRUE(by_code(r, "E8").empty()) << r.summary();
    EXPECT_TRUE(by_code(r, "E9").empty()) << r.summary();   // barrier present
}

// =========================================================================
// E9: unsynchronized plain fan-in (warning)
// =========================================================================

TEST(Validator, E9_FanInWithoutBarrierWarns) {
    json def = {
        {"nodes", {{"a", {{"type", "vnoop"}}}, {"b", {{"type", "vnoop"}}},
                   {"j", {{"type", "vnoop"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "__start__"}, {"to", "b"}},
                                {{"from", "a"}, {"to", "j"}},
                                {{"from", "b"}, {"to", "j"}},
                                {{"from", "j"}, {"to", "__end__"}} })},
    };
    auto r = validate_def(def);
    auto e9 = by_code(r, "E9");
    ASSERT_EQ(e9.size(), 1u) << r.summary();
    EXPECT_EQ(e9[0]->severity, "warning");
    EXPECT_EQ(e9[0]->witness["node"].get<std::string>(), "j");
}

// =========================================================================
// E10: route completeness
// =========================================================================

TEST(Validator, E10_EmptyRoutesIsError) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "route_channel"}} });
    auto r = validate_def(def);
    auto e10 = by_code(r, "E10");
    ASSERT_EQ(e10.size(), 1u) << r.summary();
    EXPECT_EQ(e10[0]->severity, "error");
}

TEST(Validator, E10_ClosedConditionDeadRouteIsError) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "vcond_binary"},
         {"routes", {{"yes", "b"}, {"no", "__end__"}, {"maybe", "b"}}}} });
    auto r = validate_def(def);
    auto e10 = by_code(r, "E10");
    ASSERT_EQ(e10.size(), 1u) << r.summary();
    EXPECT_EQ(e10[0]->severity, "error");
    EXPECT_EQ(e10[0]->witness["dead_keys"][0].get<std::string>(), "maybe");
}

TEST(Validator, E10_ClosedConditionUncoveredLabelIsError) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "vcond_binary"},
         {"routes", {{"yes", "b"}}}} });   // "no" uncovered
    auto r = validate_def(def);
    auto e10 = by_code(r, "E10");
    ASSERT_EQ(e10.size(), 1u) << r.summary();
    EXPECT_EQ(e10[0]->severity, "error");
    EXPECT_EQ(e10[0]->witness["uncovered"][0].get<std::string>(), "no");
}

TEST(Validator, E10_OpenConditionUncoveredKnownLabelWarns) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "route_channel"},
         {"routes", {{"x", "b"}, {"y", "__end__"}}}} });   // "default" uncovered
    auto r = validate_def(def);
    auto e10 = by_code(r, "E10");
    ASSERT_EQ(e10.size(), 1u) << r.summary();
    EXPECT_EQ(e10[0]->severity, "warning");
    EXPECT_EQ(e10[0]->witness["uncovered"][0].get<std::string>(), "default");
    EXPECT_FALSE(r.has_errors());
}

TEST(Validator, E10_ExactCoverageIsClean) {
    auto def = two_node_base();
    def["conditional_edges"] = json::array({
        {{"from", "a"}, {"condition", "vcond_binary"},
         {"routes", {{"yes", "b"}, {"no", "__end__"}}}} });
    auto r = validate_def(def);
    EXPECT_TRUE(by_code(r, "E10").empty()) << r.summary();
}

// =========================================================================
// E4/E5/E6: channel effects (gated on full effect coverage)
// =========================================================================

TEST(Validator, E4_WriteToUndeclaredChannelIsError) {
    json def = {
        {"nodes", {{"w", {{"type", "vfx_writer"}}}}},   // writes "out"
        {"edges", json::array({ {{"from", "__start__"}, {"to", "w"}} })},
        // no channels block at all
    };
    auto r = validate_def(def);
    auto e4 = by_code(r, "E4");
    ASSERT_EQ(e4.size(), 1u) << r.summary();
    EXPECT_EQ(e4[0]->severity, "error");
    EXPECT_EQ(e4[0]->witness["channel"].get<std::string>(), "out");
}

TEST(Validator, EffectFamilySkippedWhenAnyTypeUndeclared) {
    json def = {
        {"nodes", {{"w", {{"type", "vfx_writer"}}},
                   {"mystery", {{"type", "vnoop"}}}}},   // no effect contract
        {"edges", json::array({ {{"from", "__start__"}, {"to", "w"}},
                                {{"from", "w"}, {"to", "mystery"}} })},
    };
    auto r = validate_def(def);
    EXPECT_TRUE(by_code(r, "E4").empty()) << r.summary();
    EXPECT_TRUE(by_code(r, "E6").empty()) << r.summary();
}

TEST(Validator, E6_DeadChannelWarns) {
    json def = {
        {"channels", {{"out", {{"reducer", "overwrite"}}},
                      {"unused", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"w", {{"type", "vfx_writer"}}},
                   {"rd", {{"type", "vfx_reader"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "w"}},
                                {{"from", "w"}, {"to", "rd"}} })},
    };
    auto r = validate_def(def);
    auto e6 = by_code(r, "E6");
    ASSERT_EQ(e6.size(), 1u) << r.summary();
    EXPECT_EQ(e6[0]->witness["channel"].get<std::string>(), "unused");
}

TEST(Validator, E5_OverwriteRaceBetweenFanOutSiblingsWarns) {
    json def = {
        {"channels", {{"out", {{"reducer", "overwrite"}}}}},
        {"nodes", {{"src", {{"type", "vfx_reader"}}},
                   {"w1", {{"type", "vfx_writer"}}},
                   {"w2", {{"type", "vfx_writer"}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "src"}},
                                {{"from", "src"}, {"to", "w1"}},
                                {{"from", "src"}, {"to", "w2"}} })},
    };
    auto r = validate_def(def);
    auto e5 = by_code(r, "E5");
    ASSERT_EQ(e5.size(), 1u) << r.summary();
    EXPECT_EQ(e5[0]->severity, "warning");
    EXPECT_EQ(e5[0]->witness["writers"].size(), 2u);
}

// =========================================================================
// Engine integration: strict throws on validator errors
// =========================================================================

TEST(ValidatorEngine, StrictCompileThrowsOnDanglingEdge) {
    ensure_vtypes_registered();
    auto def = two_node_base();
    def["schema_version"] = 1;
    def["edges"].push_back({{"from", "a"}, {"to", "ghost"}});
    try {
        GraphEngine::compile(def, NodeContext{});
        FAIL() << "expected validation error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("[E3/error]"), std::string::npos)
            << e.what();
    }
}

TEST(ValidatorEngine, LenientCompileSurvivesDanglingEdge) {
    ensure_vtypes_registered();
    auto def = two_node_base();
    def["edges"].push_back({{"from", "a"}, {"to", "ghost"}});
    EXPECT_NO_THROW(GraphEngine::compile(def, NodeContext{}));
}

TEST(ValidatorEngine, StrictCompileCleanGraphPasses) {
    ensure_vtypes_registered();
    auto def = two_node_base();
    def["schema_version"] = 1;
    EXPECT_NO_THROW(GraphEngine::compile(def, NodeContext{}));
}
