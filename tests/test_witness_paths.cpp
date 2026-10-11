// X2 witness paths, the any_of suggestion and the route table (Program API
// v2, WP5; #363, #365).
//
// The E15 witness names the route sequence from the barrier member through
// its bypass. When that sequence re-enters the join, the last node before the
// join can stand in for the member, and the witness and the returned
// diagnostic suggest the computed any_of slot. A stalled run's witness derives
// the same route from the trace and the routed successor map.
// ProgramCompilerConfig::echo_routes returns the routed successor table.

#include "program_v2_test_support.h"

#include <neograph/graph/engine.h>
#include <neograph/graph/validator.h>

namespace {

using namespace program_v2_test;

json node(const char* type = "t.task") { return json{{"type", type}}; }
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

json issue_357(json wait_for, const char* condition = "t.check") {
    json nodes{{"A", node()}, {"B", node()}, {"C", node()}, {"FIX", node()}, {"D", node()}};
    nodes["D"]["barrier"] = json{{"wait_for", std::move(wait_for)}};
    return json{{"schema_version", 1},
                {"name", "main"},
                {"nodes", std::move(nodes)},
                {"edges", json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"),
                                       edge("C", "D"), edge("FIX", "D"), edge("D", "__end__")})},
                {"conditional_edges",
                 json::array({json{{"from", "B"},
                                   {"condition", condition},
                                   {"routes", json{{"pass", "D"}, {"fail", "FIX"}}}}})}};
}

std::vector<neograph::graph::Diagnostic> codes_in(const ValidationReport& report, std::string_view code) {
    std::vector<neograph::graph::Diagnostic> result;
    for (const auto& diagnostic : report.diagnostics)
        if (diagnostic.code == code) result.push_back(diagnostic);
    return result;
}

class Trace final : public GraphNode {
public:
    explicit Trace(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

void register_engine_types() {
    static const bool done = [] {
        NodeFactory::instance().register_type(
            "witness.node",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<Trace>(name);
            },
            json{{"type", "object"}});
        ConditionRegistry::instance().register_condition(
            "witness.fail", [](const GraphState&) { return std::string("fail"); });
        return true;
    }();
    (void)done;
}

}  // namespace

TEST(WitnessPaths, E15NamesTheBypassRouteAndTheComputedSlot) {
    const auto e15 = codes_in(GraphValidator::validate(GraphCompiler::parse(issue_357(json::array({"B", "C"})))),
                              "E15");
    ASSERT_EQ(e15.size(), 1U);
    EXPECT_EQ(canonical(e15[0].witness), canonical(json::parse(R"({
        "barrier": "D", "member": "B", "routes": {"fail": "FIX"},
        "paths": {"fail": ["B", "FIX", "D"]},
        "substitute": "FIX",
        "suggested_wait_for": [{"any_of": ["B", "FIX"]}, "C"]})")));
}

TEST(WitnessPaths, E15WithoutReentryHasAPathButNoSuggestion) {
    auto graph                      = issue_357(json::array({"B", "C"}));
    graph["nodes"]["LOG"]           = node();
    graph["conditional_edges"][0]["routes"]["fail"] = "LOG";  // LOG ends implicitly
    const auto e15 = codes_in(GraphValidator::validate(GraphCompiler::parse(graph)), "E15");
    ASSERT_EQ(e15.size(), 1U);
    EXPECT_EQ(canonical(e15[0].witness.at("paths")), canonical(json::parse(R"({"fail": ["B", "LOG"]})")));
    EXPECT_FALSE(e15[0].witness.contains("suggested_wait_for"));
}

TEST(WitnessPaths, E15OnASlotJoinExtendsThatSlot) {
    // B and Y share a slot; B's fail route goes through FIX, which is no
    // alternative, so FIX joins B's slot.
    auto graph            = issue_357(json::array({json{{"any_of", json::array({"B", "Y"})}}, "C"}));
    graph["nodes"]["Y"]   = node();
    graph["edges"].push_back(edge("A", "Y"));
    graph["edges"].push_back(edge("Y", "D"));
    const auto e15 = codes_in(GraphValidator::validate(GraphCompiler::parse(graph)), "E15");
    ASSERT_EQ(e15.size(), 1U);
    EXPECT_EQ(canonical(e15[0].witness.at("suggested_wait_for")),
              canonical(json::parse(R"([{"any_of": ["B", "FIX", "Y"]}, "C"])")));
}

TEST(WitnessPaths, ASubstituteThatIsAlreadyAMemberIsNotSuggested) {
    // B's fail route goes to C, which already has its own slot.
    auto graph = issue_357(json::array({"B", "C"}));
    graph["conditional_edges"][0]["routes"]["fail"] = "C";
    const auto e15 = codes_in(GraphValidator::validate(GraphCompiler::parse(graph)), "E15");
    ASSERT_EQ(e15.size(), 1U);
    EXPECT_EQ(canonical(e15[0].witness.at("paths")), canonical(json::parse(R"({"fail": ["B", "C", "D"]})")));
    EXPECT_FALSE(e15[0].witness.contains("suggested_wait_for"));
}

TEST(WitnessPaths, ReturnedE15SuggestsTheSlotAsAnEdit) {
    const auto outcome = compile(cpp_source(program_document(issue_357(json::array({"B", "C"})))));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    const auto e15 = with_code(outcome.returned, "P_CORE_E15");
    ASSERT_EQ(e15.size(), 1U);
    ASSERT_TRUE(e15[0].help.has_value());
    ASSERT_EQ(e15[0].help->suggestions.size(), 1U);
    const auto& suggestion = e15[0].help->suggestions[0];
    EXPECT_EQ(suggestion.applicability, SuggestionApplicability::Maybe);
    EXPECT_EQ(suggestion.message,
              R"(Let 'FIX' stand in for 'B': "wait_for": [{"any_of":["B","FIX"]},"C"])");
    ASSERT_EQ(suggestion.edits.size(), 1U);
    EXPECT_EQ(suggestion.edits[0].op, "replace");
    EXPECT_EQ(suggestion.edits[0].path, "/root/definition/nodes/D/barrier/wait_for");
    EXPECT_EQ(canonical(suggestion.edits[0].value), canonical(json::parse(R"([{"any_of":["B","FIX"]},"C"])")));
    // Applying the edit gives a program that compiles without E15.
    auto fixed = issue_357(suggestion.edits[0].value);
    const auto again = compile(cpp_source(program_document(fixed)));
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(with_code(again.returned, "P_CORE_E15").empty()) << codes(again.returned);
}

TEST(WitnessPaths, StalledRunsDeriveTheRouteFromTheTrace) {
    register_engine_types();
    auto graph = issue_357(json::array({"B", "C"}), "witness.fail");
    for (const auto* name : {"A", "B", "C", "FIX", "D"}) graph["nodes"][name]["type"] = "witness.node";
    auto engine       = GraphEngine::compile(graph, NodeContext{});
    const auto result = engine->run(RunConfig{});
    ASSERT_EQ(result.status(), RunStatus::BarrierStalled);
    const auto stall = result.output["_neograph"]["barrier_stalled"][0];
    // The fields #357 introduced keep their bytes; the route is added.
    EXPECT_EQ(canonical(stall["missing"]), canonical(json::array({"B"})));
    EXPECT_EQ(canonical(stall["received"]), canonical(json::array({"C", "FIX"})));
    EXPECT_EQ(canonical(stall["paths"]), canonical(json::parse(R"({"B": ["B", "FIX"]})")));
    EXPECT_EQ(canonical(stall["suggested_wait_for"]), canonical(json::parse(R"([{"any_of":["B","FIX"]},"C"])")));
}

TEST(WitnessPaths, StalledMembersWithoutARouteBackHaveNoPath) {
    register_engine_types();
    // B's fail route ends at LOG (an implicit end); nothing reaches D for B.
    auto graph            = issue_357(json::array({"B", "C"}), "witness.fail");
    graph["nodes"]["LOG"] = node();
    graph["conditional_edges"][0]["routes"]["fail"] = "LOG";
    for (const auto* name : {"A", "B", "C", "FIX", "D", "LOG"}) graph["nodes"][name]["type"] = "witness.node";
    auto engine       = GraphEngine::compile(graph, NodeContext{});
    const auto result = engine->run(RunConfig{});
    ASSERT_EQ(result.status(), RunStatus::BarrierStalled);
    const auto stall = result.output["_neograph"]["barrier_stalled"][0];
    EXPECT_FALSE(stall.contains("paths")) << stall.dump();
    EXPECT_FALSE(stall.contains("suggested_wait_for")) << stall.dump();
}

TEST(EchoRoutes, ReturnsTheRoutedSuccessorTableAsANote) {
    ProgramCompilerConfig config{"program-v2-test/v1"};
    config.echo_routes = true;
    const auto with    = compile(cpp_source(program_document(issue_357(json::array({"B", "C"})))), compiler(config));
    const auto without = compile(cpp_source(program_document(issue_357(json::array({"B", "C"})))));
    ASSERT_TRUE(with.ok()) << codes(with.returned);
    ASSERT_TRUE(without.ok());
    const auto notes = with_code(with.returned, "P_CORE_ROUTES");
    ASSERT_EQ(notes.size(), 1U);
    EXPECT_EQ(notes[0].severity, DiagnosticSeverity::Note);
    EXPECT_EQ(canonical(notes[0].witness.at("routes")), canonical(json::parse(R"({
        "__start__": ["A"], "A": ["B", "C"], "B": ["D", "FIX"], "C": ["D"], "D": ["__end__"],
        "FIX": ["D"]})")));
    EXPECT_TRUE(with_code(without.returned, "P_CORE_ROUTES").empty());
    EXPECT_EQ(with.bundle->id(), without.bundle->id()) << "the note is never sealed";
    EXPECT_TRUE(with_code(with.bundle->diagnostics(), "P_CORE_ROUTES").empty());
    EXPECT_EQ(diagnostic_doc_link("P_CORE_ROUTES"), "docs/diagnostics.md#p_core_routes");
    EXPECT_TRUE(javascript_authoring_capability_manifest().at("features").at("diagnostics.echo_routes").get<bool>());
}

TEST(EchoRoutes, ValidatorExposesTheRoutedSuccessors) {
    const auto routes = GraphValidator::routed_successors(GraphCompiler::parse(issue_357(json::array({"B", "C"}))));
    EXPECT_EQ(routes.at("B"), (std::set<std::string>{"D", "FIX"}));
    EXPECT_EQ(routes.at("__start__"), (std::set<std::string>{"A"}));
}
