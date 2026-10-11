// #363 any_of slots in AND joins (Program API v2, WP4).
//
// barrier("D", [{any_of: ["B", "FIX"]}, "C"]) waits for one alternative of
// each slot. A slot is filled when any alternative signals the join; the join
// fires when every slot is filled and then clears all signals. A legacy spec
// (every slot a singleton) keeps today's bytes in barrier_specs; any other
// spec lives in join_specs. tests/test_join_exhaustive.cpp compares the real
// Scheduler and GraphEngine with a reference interpreter over every small
// graph; the cases here pin the contract by name.

#include "program_v2_test_support.h"

#include "../src/core/canonical_json.h"

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>
#include <neograph/graph/validator.h>

#include <atomic>

namespace {

using namespace program_v2_test;

json node(const char* type = "t.task") { return json{{"type", type}}; }

json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

// The #357 shape: A fans out to B and C; B routes pass -> D, fail -> FIX;
// FIX and C go to D; D exits.
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

std::vector<std::string> parse_codes(const json& definition) {
    GraphRegistry            registry;
    std::vector<std::string> result;
    for (const auto& diagnostic : GraphCompiler::parse_report(definition, registry).diagnostics)
        result.push_back(diagnostic.code + "@" + diagnostic.json_pointer);
    return result;
}

// Copies, so a temporary report can be passed in.
std::vector<neograph::graph::Diagnostic> codes_in(const ValidationReport& report,
                                                  std::string_view       code) {
    std::vector<neograph::graph::Diagnostic> result;
    for (const auto& diagnostic : report.diagnostics)
        if (diagnostic.code == code) result.push_back(diagnostic);
    return result;
}

json slots(std::initializer_list<std::initializer_list<const char*>> values) {
    json result = json::array();
    for (const auto& slot : values) {
        if (slot.size() == 1) {
            result.push_back(*slot.begin());
            continue;
        }
        json alternatives = json::array();
        for (const auto* name : slot) alternatives.push_back(name);
        result.push_back(json{{"any_of", alternatives}});
    }
    return result;
}

class Trace final : public GraphNode {
public:
    explicit Trace(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"trace", json::array({name_})});
        co_return out;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

void register_engine_types() {
    static const bool done = [] {
        NodeFactory::instance().register_type(
            "slot.trace",
            [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<Trace>(name);
            },
            json{{"type", "object"}});
        ConditionRegistry::instance().register_condition(
            "slot.pass", [](const GraphState&) { return std::string("pass"); });
        ConditionRegistry::instance().register_condition(
            "slot.fail", [](const GraphState&) { return std::string("fail"); });
        return true;
    }();
    (void)done;
}

json engine_graph(json wait_for, const char* condition) {
    auto graph = issue_357(std::move(wait_for), condition);
    for (const auto* name : {"A", "B", "C", "FIX", "D"}) graph["nodes"][name]["type"] = "slot.trace";
    graph["channels"] = json{{"trace", json{{"reducer", "append"}, {"initial", json::array()}}}};
    return graph;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing and the canonical form
// ---------------------------------------------------------------------------

TEST(AnyOfSlots, LegacyEquivalentSpecsKeepBarrierSpecsAndBytes) {
    const auto plain    = GraphCompiler::parse(issue_357(json::array({"B", "C"})));
    const auto wrapped  = GraphCompiler::parse(issue_357(slots({{"B"}, {"C"}})));
    const auto anyof_b  = GraphCompiler::parse(
        issue_357(json::array({json{{"any_of", json::array({"B"})}}, "C"})));
    for (const auto* topology : {&plain, &wrapped, &anyof_b}) {
        EXPECT_TRUE(topology->join_specs.empty());
        EXPECT_EQ(topology->barrier_specs.at("D"), (std::set<std::string>{"B", "C"}));
    }
    EXPECT_EQ(canonical(anyof_b.to_json()), canonical(plain.to_json()));
}

TEST(AnyOfSlots, SlotSpecsPrintOneCanonicalForm) {
    // Alternatives sorted inside a slot, slots ordered by their smallest source.
    const auto topology = GraphCompiler::parse(issue_357(json::array(
        {"C", json{{"any_of", json::array({"FIX", "B"})}}})));
    EXPECT_TRUE(topology.barrier_specs.empty());
    ASSERT_EQ(topology.join_specs.count("D"), 1U);
    EXPECT_EQ(topology.join_specs.at("D").slots,
              (std::vector<std::set<std::string>>{{"B", "FIX"}, {"C"}}));
    const auto printed = topology.to_json().at("nodes").at("D").at("barrier");
    EXPECT_EQ(canonical(printed), canonical(json::parse(R"({"wait_for":[{"any_of":["B","FIX"]},"C"]})")));
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(
        issue_357(json::array({"C", json{{"any_of", json::array({"FIX", "B"})}}})), topology));
    EXPECT_EQ(canonical(GraphCompiler::canon(issue_357(json::array(
                  {"C", json{{"any_of", json::array({"FIX", "B"})}}})))),
              canonical(GraphCompiler::canon(topology.to_json())));
}

TEST(AnyOfSlots, NamesOutsideTheBmpSortByUtf8Bytes) {
    // UTF-16 code units put U+1F600 before U+FF5A; UTF-8 bytes put it after.
    json definition{{"schema_version", 1},
                    {"name", "main"},
                    {"nodes", json{{"start", node()}, {"join", node()},
                                   {"\xF0\x9F\x98\x80", node()}, {"\xEF\xBD\x9A", node()}, {"z", node()}}},
                    {"edges", json::array({edge("__start__", "start"), edge("join", "__end__")})}};
    for (const auto* name : {"\xF0\x9F\x98\x80", "\xEF\xBD\x9A", "z"}) {
        definition["edges"].push_back(edge("start", name));
        definition["edges"].push_back(edge(name, "join"));
    }
    definition["nodes"]["join"]["barrier"] = json{
        {"wait_for", json::array({json{{"any_of", json::array({"\xF0\x9F\x98\x80", "\xEF\xBD\x9A"})}}, "z"})}};
    const auto printed = GraphCompiler::parse(definition).to_json()["nodes"]["join"]["barrier"]["wait_for"];
    EXPECT_EQ(canonical(printed),
              canonical(json::array({"z", json{{"any_of", json::array({"\xEF\xBD\x9A", "\xF0\x9F\x98\x80"})}}})));
}

TEST(AnyOfSlots, MalformedSlotsAreBarrierErrors) {
    const std::vector<json> malformed{
        json::array({json{{"any_of", json::array()}}, "C"}),                         // empty
        json::array({json{{"any_of", json::array({1})}}, "C"}),                      // not a string
        json::array({json{{"any_of", json::array({""})}}, "C"}),                     // empty name
        json::array({json{{"any_of", json::array({"B", "FIX"})}, {"mode", "any"}}, "C"}),  // extra key
        json::array({json{{"any_of", json::array({json{{"any_of", json::array({"B"})}}})}}, "C"}),
        json::array({json{{"any_of", json::array({"B", "B"})}}, "C"}),               // repeated in a slot
        json::array({json{{"any_of", json::array({"B", "FIX"})}}, "B"}),             // repeated across
        json::array({json{{"any_of", "B"}}, "C"}),                                   // not an array
        json::array({json::object(), "C"}),                                          // no any_of
    };
    for (const auto& wait_for : malformed) {
        const auto codes = parse_codes(issue_357(wait_for));
        ASSERT_FALSE(codes.empty()) << wait_for.dump();
        EXPECT_NE(std::find_if(codes.begin(), codes.end(),
                               [](const std::string& code) { return code.rfind("GC_BARRIER@/nodes/D/barrier", 0) == 0; }),
                  codes.end())
            << wait_for.dump();
    }
    // Plain duplicates keep their historical meaning (one member).
    EXPECT_TRUE(parse_codes(issue_357(json::array({"B", "B", "C"}))).empty());
    // Legacy (schema 0) documents do not read slot objects.
    auto legacy = issue_357(slots({{"B", "FIX"}, {"C"}}));
    legacy["schema_version"] = 0;
    EXPECT_THROW((void)GraphCompiler::parse(legacy), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

namespace {

struct SchedulerFixture {
    std::vector<Edge>            edges{{"B", "D"}, {"FIX", "D"}, {"C", "D"}, {"X", "D"}};
    std::vector<ConditionalEdge> conditional;
    JoinSpecs                    joins{{"D", JoinSpec{{{"B", "FIX"}, {"C"}}}}};
    Scheduler                    scheduler{edges, conditional, BarrierSpecs{}, joins, nullptr};
    GraphState                   state;
    BarrierState                 barriers;

    NextStepPlan step(std::initializer_list<const char*> ran) {
        std::vector<StepRouting> routings;
        for (const auto* name : ran) routings.push_back({name, std::nullopt});
        return scheduler.plan_next_step(routings, state, barriers);
    }
};

}  // namespace

TEST(AnyOfSlotsScheduler, EachAlternativeFillsItsSlot) {
    for (const auto* alternative : {"B", "FIX"}) {
        SchedulerFixture fixture;
        EXPECT_TRUE(fixture.step({"C"}).ready.empty());
        const auto plan = fixture.step({alternative});
        EXPECT_EQ(plan.ready, (std::vector<std::string>{"D"})) << alternative;
        EXPECT_TRUE(fixture.barriers["D"].empty()) << "signals clear when the join fires";
    }
}

TEST(AnyOfSlotsScheduler, BothAlternativesInOneStepFireOnce) {
    SchedulerFixture fixture;
    const auto       plan = fixture.step({"B", "FIX", "C"});
    EXPECT_EQ(plan.ready, (std::vector<std::string>{"D"}));
    EXPECT_TRUE(fixture.barriers["D"].empty());
}

TEST(AnyOfSlotsScheduler, ALateAlternativeOpensARoundThatNeverCompletes) {
    SchedulerFixture fixture;
    EXPECT_EQ(fixture.step({"B", "C"}).ready, (std::vector<std::string>{"D"}));
    EXPECT_TRUE(fixture.step({"FIX"}).ready.empty());
    EXPECT_EQ(fixture.barriers["D"], (std::set<std::string>{"FIX"}));
}

TEST(AnyOfSlotsScheduler, NonMembersAreRecordedButNeverCount) {
    SchedulerFixture fixture;
    EXPECT_TRUE(fixture.step({"X"}).ready.empty());
    EXPECT_TRUE(fixture.step({"C"}).ready.empty());
    EXPECT_EQ(fixture.barriers["D"], (std::set<std::string>{"C", "X"}));
    EXPECT_EQ(fixture.step({"FIX"}).ready, (std::vector<std::string>{"D"}));
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

TEST(AnyOfSlotsEngine, Issue357CompletesOnBothRoutes) {
    register_engine_types();
    for (const auto* condition : {"slot.pass", "slot.fail"}) {
        auto engine = GraphEngine::compile(engine_graph(slots({{"B", "FIX"}, {"C"}}), condition), NodeContext{});
        const auto result = engine->run(RunConfig{});
        EXPECT_EQ(result.status(), RunStatus::Completed) << condition;
        ASSERT_FALSE(result.execution_trace.empty());
        EXPECT_EQ(result.execution_trace.back(), "D") << condition;
        EXPECT_EQ(std::count(result.execution_trace.begin(), result.execution_trace.end(), "D"), 1) << condition;
    }
}

TEST(AnyOfSlotsEngine, AStalledSlotJoinPrintsSlotsInItsWitness) {
    register_engine_types();
    // B fails to FIX, but FIX is not an alternative here: the slots are
    // {B, X} and C, and X never runs.
    auto graph          = engine_graph(json::array({"C", json{{"any_of", json::array({"B", "X"})}}}), "slot.fail");
    graph["nodes"]["X"] = json{{"type", "slot.trace"}};
    graph["edges"].push_back(edge("X", "D"));
    auto engine = GraphEngine::compile(graph, NodeContext{});
    const auto result = engine->run(RunConfig{});
    ASSERT_EQ(result.status(), RunStatus::BarrierStalled);
    const auto stall = result.output["_neograph"]["barrier_stalled"][0];
    EXPECT_EQ(canonical(stall["wait_for"]), canonical(json::parse(R"([{"any_of":["B","X"]},"C"])")));
    EXPECT_EQ(canonical(stall["missing"]), canonical(json::parse(R"([{"any_of":["B","X"]}])")));
    EXPECT_EQ(canonical(stall["received"]), canonical(json::array({"C", "FIX"})));
}

TEST(AnyOfSlotsEngine, InterruptBeforeTheJoinResumesOnce) {
    register_engine_types();
    auto graph               = engine_graph(slots({{"B", "FIX"}, {"C"}}), "slot.fail");
    graph["interrupt_before"] = json::array({"D"});
    auto store               = std::make_shared<InMemoryCheckpointStore>();
    auto engine              = GraphEngine::compile(graph, NodeContext{}, store);
    RunConfig config;
    config.thread_id = "slot-interrupt";
    const auto first = engine->run(config);
    ASSERT_TRUE(first.interrupted);
    EXPECT_EQ(first.interrupt_node, "D");
    const auto resumed = engine->resume("slot-interrupt");
    EXPECT_EQ(resumed.status(), RunStatus::Completed);
    EXPECT_EQ(resumed.execution_trace.back(), "D");
}

// A partial slot round survives fork and update_state: C has signaled D, the
// slot {B, FIX} is still empty when the run pauses after C.
TEST(AnyOfSlotsEngine, ForkAndUpdateStateKeepAPartialSlotRound) {
    register_engine_types();
    auto graph              = engine_graph(slots({{"B", "FIX"}, {"C"}}), "slot.fail");
    graph["interrupt_after"] = json::array({"C"});
    auto store              = std::make_shared<InMemoryCheckpointStore>();
    auto engine             = GraphEngine::compile(graph, NodeContext{}, store);
    RunConfig config;
    config.thread_id = "slot-partial";
    const auto paused = engine->run(config);
    ASSERT_TRUE(paused.interrupted);
    const auto latest = store->load_latest("slot-partial");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->barrier_state.at("D"), (std::set<std::string>{"C"}));

    engine->update_state("slot-partial", json{{"trace", json::array({"edited"})}});
    const auto updated = store->load_latest("slot-partial");
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->barrier_state.at("D"), (std::set<std::string>{"C"}));

    engine->fork("slot-partial", "slot-partial-fork");
    const auto forked = engine->resume("slot-partial-fork");
    EXPECT_EQ(forked.status(), RunStatus::Completed);
    EXPECT_EQ(forked.execution_trace.back(), "D");
    const auto original = engine->resume("slot-partial");
    EXPECT_EQ(original.status(), RunStatus::Completed);
}

// ---------------------------------------------------------------------------
// Validator
// ---------------------------------------------------------------------------

TEST(AnyOfSlotsValidator, UnknownAndSelfAlternativesAreE3) {
    auto unknown = issue_357(slots({{"B", "NOPE"}, {"C"}}));
    auto e3      = codes_in(GraphValidator::validate(GraphCompiler::parse(unknown)), "E3");
    ASSERT_EQ(e3.size(), 1U);
    EXPECT_EQ(e3[0].witness.at("unknown").get<std::string>(), "NOPE");
    auto self = issue_357(slots({{"B", "D"}, {"C"}}));
    EXPECT_EQ(codes_in(GraphValidator::validate(GraphCompiler::parse(self)), "E3").size(), 1U);
}

TEST(AnyOfSlotsValidator, ASlotWithoutARouteIntoTheJoinIsE8) {
    // X has no edge into D, but its slot partner C does.
    auto graph            = issue_357(json::array({json{{"any_of", json::array({"C", "X"})}}, "B"}));
    graph["nodes"]["X"]   = node();
    graph["edges"].push_back(edge("A", "X"));
    EXPECT_TRUE(codes_in(GraphValidator::validate(GraphCompiler::parse(graph)), "E8").empty())
        << "one alternative with a route is enough";
    auto none           = issue_357(json::array({json{{"any_of", json::array({"A", "X"})}}, "C"}));
    none["nodes"]["X"]  = node();
    none["edges"].push_back(edge("A", "X"));
    const auto e8 = codes_in(GraphValidator::validate(GraphCompiler::parse(none)), "E8");
    ASSERT_EQ(e8.size(), 1U);
    EXPECT_EQ(canonical(e8[0].witness.at("slot")), canonical(json::array({"A", "X"})));
}

TEST(AnyOfSlotsValidator, ARouteToAnotherAlternativeIsNotABypass) {
    const auto legacy = GraphValidator::validate(GraphCompiler::parse(issue_357(json::array({"B", "C"}))));
    ASSERT_EQ(codes_in(legacy, "E15").size(), 1U) << "B -> FIX bypasses a legacy join";
    EXPECT_NE(codes_in(legacy, "E15")[0].message.find(R"({"fail":"FIX"})"), std::string::npos)
        << codes_in(legacy, "E15")[0].message;
    const auto slot =
        GraphValidator::validate(GraphCompiler::parse(issue_357(slots({{"B", "FIX"}, {"C"}}))));
    EXPECT_TRUE(codes_in(slot, "E15").empty()) << "FIX is an alternative of B's slot";
    EXPECT_TRUE(codes_in(slot, "E9").empty()) << "a slot join is a declared join";
    EXPECT_FALSE(slot.has_errors());
}

TEST(AnyOfSlotsValidator, CoActiveAlternativesAreLinted) {
    // A fans out to B and C, and the slot {B, C} waits for either: both arrive.
    json graph{{"schema_version", 1},
               {"name", "main"},
               {"nodes", json{{"A", node()}, {"B", node()}, {"C", node()}, {"D", node()}}},
               {"edges", json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"),
                                      edge("B", "D"), edge("C", "D"), edge("D", "__end__")})}};
    graph["nodes"]["D"]["barrier"] = json{{"wait_for", slots({{"B", "C"}})}};
    const auto report             = GraphValidator::validate(GraphCompiler::parse(graph));
    const auto lint               = codes_in(report, "E17");
    ASSERT_EQ(lint.size(), 1U);
    EXPECT_EQ(lint[0].severity, "warning");
    EXPECT_EQ(lint[0].witness.at("fan_out").get<std::string>(), "A");
    EXPECT_EQ(canonical(lint[0].witness.at("paths")),
              canonical(json::parse(R"([["A","B"],["A","C"]])")));
    EXPECT_NE(lint[0].message.find("barrier-stalled"), std::string::npos) << lint[0].message;

    // An alternative that routes both into the join and on to another one.
    json onward = graph;
    onward["edges"] = json::array({edge("__start__", "A"), edge("A", "B"), edge("B", "D"), edge("B", "C"),
                                   edge("C", "D"), edge("D", "__end__")});
    const auto both = codes_in(GraphValidator::validate(GraphCompiler::parse(onward)), "E17");
    ASSERT_EQ(both.size(), 1U);
    EXPECT_EQ(both[0].witness.at("fan_out").get<std::string>(), "B");

    // The #357 slot is route-exclusive: no lint.
    EXPECT_TRUE(codes_in(GraphValidator::validate(GraphCompiler::parse(issue_357(slots({{"B", "FIX"}, {"C"}})))),
                         "E17")
                    .empty());
    // Original rules never report the new lint.
    ValidationOptions original;
    original.original_rules = true;
    GraphRegistry registry;
    EXPECT_TRUE(codes_in(GraphValidator::validate(GraphCompiler::parse(graph), registry, original), "E17").empty());
}

// ---------------------------------------------------------------------------
// Program identity, sealing and admission
// ---------------------------------------------------------------------------

TEST(AnyOfSlotsProgram, SingletonSlotsHashLikeTheirLegacyForm) {
    const auto legacy  = compile(cpp_source(program_document(issue_357(json::array({"B", "C"})))));
    const auto wrapped = compile(cpp_source(program_document(
        issue_357(json::array({json{{"any_of", json::array({"B"})}}, "C"})))));
    ASSERT_TRUE(legacy.ok()) << codes(legacy.returned);
    ASSERT_TRUE(wrapped.ok()) << codes(wrapped.returned);
    EXPECT_EQ(wrapped.bundle->sealed_core_definitions()[0].definition_hash,
              legacy.bundle->sealed_core_definitions()[0].definition_hash);
    EXPECT_EQ(canonical(wrapped.bundle->sealed_core_definitions()[0].definition),
              canonical(legacy.bundle->sealed_core_definitions()[0].definition));
    EXPECT_EQ(wrapped.bundle->core_plan_identities()[0].compiled_plan_identity,
              legacy.bundle->core_plan_identities()[0].compiled_plan_identity);
    // The bundle id also covers the source bytes, which differ.
}

TEST(AnyOfSlotsProgram, SlotBundlesSealNoValidatorWarningsAndRoundTrip) {
    auto definition               = issue_357(slots({{"B", "FIX"}, {"C"}}));
    definition["nodes"]["orphan"] = node();
    const auto outcome            = compile(cpp_source(program_document(definition)));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    EXPECT_FALSE(with_code(outcome.returned, "P_CORE_E7").empty());
    EXPECT_TRUE(outcome.bundle->diagnostics().empty()) << codes(outcome.bundle->diagnostics());
    EXPECT_EQ(ProgramBundle::parse(outcome.bundle->serialize_canonical()).id(), outcome.bundle->id());

    // A sealed slot must be in canonical form.
    auto sealed = outcome.bundle->sealed_core_definitions().at(0).definition;
    sealed["nodes"]["D"]["barrier"]["wait_for"] = json::parse(R"(["C",{"any_of":["FIX","B"]}])");
    SealedCoreDefinition reordered{"main", sealed_core_definition_hash(sealed), sealed};
    EXPECT_THROW((void)core_compiled_plan_identity(reordered, "build",
                                                   outcome.bundle->registry_snapshot_fingerprint(),
                                                   outcome.bundle->executable_registry_identities()),
                 std::invalid_argument);
}

TEST(AnyOfSlotsProgram, ProgramDiagnosticsUseSlotCodes) {
    auto definition = issue_357(slots({{"B", "NOPE"}, {"C"}}));
    const auto outcome = compile(cpp_source(program_document(definition)));
    ASSERT_FALSE(outcome.ok());
    const auto e3 = with_code(outcome.returned, "P_CORE_E3");
    ASSERT_EQ(e3.size(), 1U) << codes(outcome.returned);
    ASSERT_TRUE(e3[0].help.has_value());
    ASSERT_EQ(e3[0].help->suggestions.size(), 0U) << "NOPE is not near a declared node";
    EXPECT_EQ(diagnostic_doc_link("P_CORE_E17"), "docs/diagnostics.md#p_core_e17");
}

TEST(AnyOfSlotsProgram, CapabilityManifestPublishesSlots) {
    const auto manifest = javascript_authoring_capability_manifest();
    EXPECT_TRUE(manifest.at("features").at("barrier.any_of").get<bool>());
}

#if defined(NEOGRAPH_V2_TESTS_HAVE_QUICKJS)

TEST(AnyOfSlotsProgram, JavaScriptBarrierAcceptsSlotsAndRejectsOtherShapes) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main")
            .node("A", {type: "t.task"}).node("B", {type: "t.task"}).node("C", {type: "t.task"})
            .node("FIX", {type: "t.task"}).node("D", {type: "t.task"})
            .entry("A").edge("A", "B").edge("A", "C")
            .conditionalEdge("B", "t.check", {pass: "D", fail: "FIX"})
            .edge("FIX", "D").edge("C", "D")
            .barrier("D", [{any_of: ["FIX", "B"]}, "C"]).exit("D");
    })JS"));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    EXPECT_EQ(canonical(outcome.bundle->sealed_core_definitions()[0].definition["nodes"]["D"]["barrier"]),
              canonical(json::parse(R"({"wait_for":[{"any_of":["B","FIX"]},"C"]})")));
    for (const auto* shape : {R"([{one_of: ["B"]}])", R"([["B"]])", R"([1])", R"("B")"}) {
        const auto failed = compile(js_source(std::string(R"JS(export function define() {
            return ng.graph("main").node("B", {type: "t.task"}).node("D", {type: "t.task"})
                .entry("B").edge("B", "D").barrier("D", )JS") + shape + R"JS().exit("D");
        })JS"));
        ASSERT_FALSE(failed.ok()) << shape;
        EXPECT_EQ(failed.returned.at(0).code, "P_JS_GRAPH_ARGUMENT") << shape << codes(failed.returned);
    }
}

#endif
