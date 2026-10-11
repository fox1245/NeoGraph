// X1 self-repair diagnostic contract (Program API v2, WP1; #365).
//
// Every diagnostic a compilation returns carries `help`: a documentation link,
// labelled related locations, suggestions as JSON Patch edits with an
// applicability, and did-you-mean candidates. Sealed diagnostics never carry
// it, so bundle ids do not change. Cascades about one unknown name collapse
// into one diagnostic, and at most 20 diagnostics are returned.

#include "program_v2_test_support.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#ifndef NEOGRAPH_SOURCE_DIR
#error "NEOGRAPH_SOURCE_DIR must name the repository root"
#endif

namespace {

using namespace program_v2_test;

json help_json(const Diagnostic& diagnostic) {
    if (!diagnostic.help) return nullptr;
    json value;
    to_json(value, *diagnostic.help);
    return value;
}

// Snapshot of what a self-repairing caller reads: code, severity, generated
// pointer and the full help object.
json snapshot(const Diagnostic& diagnostic) {
    return json{{"code", diagnostic.code},
                {"severity", std::string(to_string(diagnostic.severity))},
                {"json_pointer", diagnostic.primary.json_pointer},
                {"help", help_json(diagnostic)}};
}

void expect_snapshot(const Diagnostic& diagnostic, const char* expected) {
    EXPECT_EQ(canonical(snapshot(diagnostic)), canonical(json::parse(expected)))
        << "message: " << diagnostic.message;
}

const Diagnostic& only(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    const auto matches = with_code(diagnostics, code);
    if (matches.size() != 1) {
        ADD_FAILURE() << "expected one " << code << ", got" << codes(diagnostics);
        static const Diagnostic empty;
        return empty;
    }
    for (const auto& diagnostic : diagnostics)
        if (diagnostic.code == code) return diagnostic;
    static const Diagnostic empty;
    return empty;
}

json minimal_definition() {
    return json{{"schema_version", 1},
                {"name", "main"},
                {"nodes", json{{"work", json{{"type", "t.task"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                       json{{"from", "work"}, {"to", "__end__"}}})}};
}

}  // namespace

// ---------------------------------------------------------------------------
// Value contract
// ---------------------------------------------------------------------------

TEST(DiagnosticHelp, NameDistanceCountsTranspositionsAsOneEdit) {
    EXPECT_EQ(diagnostic_name_distance("Done", "Done"), 0U);
    EXPECT_EQ(diagnostic_name_distance("Dnoe", "Done"), 1U);
    EXPECT_EQ(diagnostic_name_distance("Don", "Done"), 1U);
    EXPECT_EQ(diagnostic_name_distance("Dxne", "Done"), 1U);
    EXPECT_EQ(diagnostic_name_distance("conditionnal_edges", "conditional_edges"), 1U);
    EXPECT_EQ(diagnostic_name_distance("abcdef", "uvwxyz"), 3U) << "stops at limit + 1";
    EXPECT_EQ(diagnostic_name_candidates("Dnoe", {"Work", "Done", "Dome", "Do", "Doer"}),
              (std::vector<std::string>{"Done", "Do", "Doer", "Dome"}));
    EXPECT_TRUE(diagnostic_name_candidates("x", {"x"}).empty()) << "an exact match is no candidate";
}

TEST(DiagnosticHelp, SerializesOnlyWhenPresentAndRoundTrips) {
    Diagnostic diagnostic;
    diagnostic.phase    = CompilePhase::CoreValidate;
    diagnostic.code     = "P_CORE_E3";
    diagnostic.primary  = {"main.js", "/root/definition/edges/1", SourceSpan{3, 9, 1, 4, 1, 10}};
    diagnostic.message  = "edge 'to' references unknown node 'Dnoe'";
    diagnostic.witness  = json{{"edge_to", "Dnoe"}};

    json without;
    to_json(without, diagnostic);
    EXPECT_FALSE(without.contains("help"));

    DiagnosticHelp help;
    help.doc        = "docs/diagnostics.md#p_core_e3";
    help.candidates = {"Done"};
    help.related.push_back({"also references 'Dnoe'", {"main.js", "/root/definition/interrupt_before", std::nullopt}});
    help.suggestions.push_back({"Replace 'Dnoe' with 'Done'",
                                {{"replace", "/root/definition/edges/1/to", "", json("Done")},
                                 {"move", "/root/definition/b", "/root/definition/a", nullptr}},
                                SuggestionApplicability::Maybe});
    diagnostic.help = help;

    json with;
    to_json(with, diagnostic);
    ASSERT_TRUE(with.contains("help"));
    EXPECT_EQ(canonical(with["help"]), canonical(json::parse(R"({
        "doc": "docs/diagnostics.md#p_core_e3",
        "related": [{"label": "also references 'Dnoe'",
                     "source_id": "main.js", "json_pointer": "/root/definition/interrupt_before",
                     "span": null}],
        "suggestions": [{"message": "Replace 'Dnoe' with 'Done'", "applicability": "maybe",
                         "edits": [{"op": "replace", "path": "/root/definition/edges/1/to", "value": "Done"},
                                   {"op": "move", "path": "/root/definition/b", "from": "/root/definition/a"}]}],
        "candidates": ["Done"]})")));

    Diagnostic parsed;
    from_json(with, parsed);
    json again;
    to_json(again, parsed);
    EXPECT_EQ(canonical(again), canonical(with));

    const auto sealed = sealed_diagnostic(diagnostic);
    EXPECT_FALSE(sealed.help.has_value());
    json sealed_json;
    to_json(sealed_json, sealed);
    EXPECT_EQ(canonical(sealed_json), canonical(without));

    for (const auto* bad : {R"({"op": "copy", "path": "/a", "from": "/b"})",
                            R"({"op": "move", "path": "/a"})",
                            R"({"op": "remove", "path": "/a", "value": 1})"}) {
        json broken = with;
        broken["help"]["suggestions"][0]["edits"] = json::array({json::parse(bad)});
        Diagnostic rejected;
        EXPECT_THROW(from_json(broken, rejected), std::invalid_argument) << bad;
    }
    EXPECT_EQ(suggestion_applicability_from_string("placeholder"),
              SuggestionApplicability::Placeholder);
    EXPECT_THROW(suggestion_applicability_from_string("always"), std::invalid_argument);
}

TEST(DiagnosticHelp, BundlesNeverSealHelp) {
    auto       outcome = compile(cpp_source(program_document(minimal_definition())));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    const auto bundle = *outcome.bundle;

    // A bundle built from data whose diagnostics carry help seals them without it.
    Diagnostic warning;
    warning.phase    = CompilePhase::CoreValidate;
    warning.code     = "P_CORE_E7";
    warning.severity = DiagnosticSeverity::Warning;
    warning.primary  = {"program-v2/test.json", "/root/definition", std::nullopt};
    warning.message  = "node(s) unreachable";
    warning.witness  = json::object();
    ProgramBundleData plain;
    plain.source_kind                    = bundle.source_kind();
    plain.source_hash                    = bundle.source_hash();
    plain.canonical_program_hash         = bundle.canonical_program_hash();
    plain.compiler_build_id              = bundle.compiler_build_id();
    plain.program_schema_version         = bundle.program_schema_version();
    plain.registry_snapshot_fingerprint  = bundle.registry_snapshot_fingerprint();
    plain.module_dependency_merkle_root  = bundle.module_dependency_merkle_root();
    plain.input_contract                 = bundle.input_contract();
    plain.output_contract                = bundle.output_contract();
    plain.orchestration_plan             = bundle.orchestration_plan();
    plain.sealed_core_definitions        = bundle.sealed_core_definitions();
    plain.core_plan_identities           = bundle.core_plan_identities();
    plain.capability_effect_closure      = bundle.capability_effect_closure();
    plain.execution_guarantee            = bundle.execution_guarantee();
    plain.executable_registry_identities = bundle.executable_registry_identities();
    plain.declared_budget_requirements   = bundle.declared_budget_requirements();
    plain.source_map                     = bundle.source_map();
    plain.diagnostics                    = {warning};
    auto helped                          = plain;
    warning.help                         = DiagnosticHelp{"docs/diagnostics.md#p_core_e7", {}, {}, {}};
    helped.diagnostics                   = {warning};

    const ProgramBundle without(plain);
    const ProgramBundle with(helped);
    EXPECT_EQ(with.id(), without.id());
    EXPECT_EQ(with.serialize_canonical(), without.serialize_canonical());
    EXPECT_FALSE(with.diagnostics().at(0).help.has_value());
    EXPECT_EQ(with.serialize_canonical().find("\"help\""), std::string::npos);
}

TEST(DiagnosticHelp, DocLinksResolveInTheDiagnosticsGuide) {
    const std::filesystem::path guide = std::filesystem::path(NEOGRAPH_SOURCE_DIR) / "docs" / "diagnostics.md";
    std::ifstream               input(guide);
    ASSERT_TRUE(input) << guide;
    std::stringstream text;
    text << input.rdbuf();
    const auto            body = text.str();
    std::set<std::string> anchors;
    const std::regex      anchor(R"re(<a id="([^"]+)"></a>)re");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), anchor); it != std::sregex_iterator();
         ++it)
        anchors.insert((*it)[1].str());
    // Headings (GitHub slugs) the family fallback uses.
    for (const auto* family : {"program-document", "javascript-frontend", "core-parser",
                               "core-validator", "registry", "runtime-and-admission"})
        EXPECT_NE(body.find("](#" + std::string(family) + ")"), std::string::npos)
            << "the guide's table of contents must link the family section " << family;

    // Every code the compiler emits has its own entry.
    for (const auto* code :
         {"P_CORE_E3",  "P_CORE_E4",  "P_CORE_E5",  "P_CORE_E6",  "P_CORE_E7",
          "P_CORE_E8",  "P_CORE_E9",  "P_CORE_E10", "P_CORE_E11", "P_CORE_E12",
          "P_CORE_E13", "P_CORE_E14", "P_CORE_E15", "P_CORE_E16",
          "P_CORE_GC_BARRIER", "P_CORE_GC_EDGE", "P_CORE_GC_FIELD_REQUIRED",
          "P_CORE_GC_FIELD_TYPE", "P_CORE_GC_FIELD_VALUE", "P_CORE_GC_NODE_CONFIG",
          "P_CORE_GC_NO_ENTRY", "P_CORE_GC_REGISTRY_MISS", "P_CORE_GC_RESERVED_NODE",
          "P_CORE_GC_SCHEMA_VERSION", "P_CORE_GC_UNKNOWN_FIELD", "P_CORE_ROUNDTRIP",
          "P_JS_GRAPH_ARGUMENT", "P_JS_CONTROL_CHARACTER", "P_JS_EVALUATION",
          "P_JS_TIMEOUT", "P_SCHEMA_UNKNOWN_FIELD", "P_SCHEMA_REQUIRED", "P_SCHEMA_TYPE",
          "P_SCHEMA_VERSION", "P_ROOT_NAME", "P_BUDGET_INVALID", "P_REGISTRY_MISSING",
          "P_IMPORT_UNRESOLVED", "P_COMPILER_INTERNAL", "P_DIAGNOSTICS_TRUNCATED",
          "P_CORE_BARRIER_STALLED"}) {
        const auto link = diagnostic_doc_link(code);
        ASSERT_EQ(link.rfind("docs/diagnostics.md#", 0), 0U) << link;
        const auto fragment = link.substr(std::string("docs/diagnostics.md#").size());
        EXPECT_TRUE(anchors.count(fragment)) << code << " -> " << link;
        std::string lower(code);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        EXPECT_EQ(fragment, lower) << code;
    }
    // Codes without an entry of their own link their family's section.
    EXPECT_EQ(diagnostic_doc_link("P_RUN_LEASE_HELD"), "docs/diagnostics.md#runtime-and-admission");
    EXPECT_EQ(diagnostic_doc_link("P_JS_SOMETHING_NEW"), "docs/diagnostics.md#javascript-frontend");
    EXPECT_EQ(diagnostic_doc_link("P_CORE_GC_SOMETHING_NEW"), "docs/diagnostics.md#core-parser");
    EXPECT_EQ(diagnostic_doc_link("P_CORE_E99"), "docs/diagnostics.md#core-validator");
}

// ---------------------------------------------------------------------------
// Ten representative diagnostics
// ---------------------------------------------------------------------------

#if defined(NEOGRAPH_V2_TESTS_HAVE_QUICKJS)

// 1. An unknown edge target: the declared name one transposition away is the
//    candidate, and replacing it is the suggestion.
TEST(DiagnosticHelpSnapshot, UnknownEdgeTargetSuggestsTheNearestNode) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("Work", {type: "t.task"}).node("Done", {type: "t.task"})
            .entry("Work").edge("Work", "Dnoe").exit("Done");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    expect_snapshot(only(outcome.returned, "P_CORE_E3"), R"({
        "code": "P_CORE_E3", "severity": "error", "json_pointer": "/root/definition/edges/1",
        "help": {"doc": "docs/diagnostics.md#p_core_e3", "related": [],
                 "candidates": ["Done"],
                 "suggestions": [{"message": "Replace 'Dnoe' with 'Done'", "applicability": "maybe",
                                  "edits": [{"op": "replace", "path": "/root/definition/edges/1/to",
                                             "value": "Done"}]}]}})");
}

// 2. One unknown name referenced from three places is one diagnostic; the
//    other sites become labelled related locations and every reference is in
//    the suggestion.
TEST(DiagnosticHelpSnapshot, CascadesAboutOneUnknownNameCollapse) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"}).node("Review", {type: "t.review"})
            .entry("A").edge("A", "Review").edge("Rveiew", "Review")
            .barrier("Review", ["A", "Rveiew"]).interruptBefore("Rveiew").exit("Review");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    const auto& e3 = only(outcome.returned, "P_CORE_E3");
    ASSERT_TRUE(e3.help.has_value());
    // Every call site is located; read the spans back rather than pin columns.
    ASSERT_EQ(e3.help->related.size(), 2U);
    for (const auto& related : e3.help->related) {
        EXPECT_EQ(related.label, "also references 'Rveiew'");
        EXPECT_TRUE(related.coordinate.span.has_value()) << related.coordinate.json_pointer;
    }
    auto expected = json::parse(R"({
        "code": "P_CORE_E3", "severity": "error", "json_pointer": "/root/definition/edges/2",
        "help": {"doc": "docs/diagnostics.md#p_core_e3",
                 "related": [
                   {"label": "also references 'Rveiew'", "source_id": "program-v2/test.js",
                    "json_pointer": "/root/definition/interrupt_before"},
                   {"label": "also references 'Rveiew'", "source_id": "program-v2/test.js",
                    "json_pointer": "/root/definition/nodes/Review/barrier"}],
                 "candidates": ["Review"],
                 "suggestions": [{"message": "Replace 'Rveiew' with 'Review'", "applicability": "maybe",
                                  "edits": [
                   {"op": "replace", "path": "/root/definition/edges/2/from", "value": "Review"},
                   {"op": "replace", "path": "/root/definition/interrupt_before/0", "value": "Review"},
                   {"op": "replace", "path": "/root/definition/nodes/Review/barrier/wait_for/1",
                    "value": "Review"}]}]}})");
    auto actual = snapshot(e3);
    for (std::size_t index = 0; index < 2; ++index)
        expected["help"]["related"][index]["span"] = actual["help"]["related"][index]["span"];
    EXPECT_EQ(canonical(actual), canonical(expected)) << e3.message;
}

// 3. A misspelled top-level Core key in a JSON source: the candidate is the
//    real key and the edit moves the value there. JSON sources keep Core spelling.
TEST(DiagnosticHelpSnapshot, UnknownCoreKeySuggestsTheKnownKey) {
    auto definition                  = minimal_definition();
    definition["conditionnal_edges"] = json::array();
    const auto outcome               = compile(cpp_source(program_document(definition)));
    ASSERT_FALSE(outcome.ok());
    const auto& unknown = only(outcome.returned, "P_CORE_GC_UNKNOWN_FIELD");
    EXPECT_NE(unknown.message.find("conditionnal_edges"), std::string::npos) << unknown.message;
    expect_snapshot(unknown, R"({
        "code": "P_CORE_GC_UNKNOWN_FIELD", "severity": "error",
        "json_pointer": "/root/definition/conditionnal_edges",
        "help": {"doc": "docs/diagnostics.md#p_core_gc_unknown_field", "related": [],
                 "candidates": ["conditional_edges"],
                 "suggestions": [{"message": "Rename 'conditionnal_edges' to 'conditional_edges'",
                                  "applicability": "maybe",
                                  "edits": [{"op": "move", "from": "/root/definition/conditionnal_edges",
                                             "path": "/root/definition/conditional_edges"}]}]}})");
}

// 4. The #364 repro: an unknown retry-policy field. A JavaScript caller reads
//    the builder call, not the Core key path.
TEST(DiagnosticHelpSnapshot, JavaScriptMessagesNameTheBuilderCall) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"}).node("B", {type: "t.task"})
            .entry("A").edge("A", "B").exit("B")
            .retryPolicy({max_retries: 2, initial_delay_ms: 1, backoff_multiplier: 1,
                          max_delay_ms: 1, on_exhausted: "B"});
    })JS"));
    ASSERT_FALSE(outcome.ok());
    const auto& unknown = only(outcome.returned, "P_CORE_GC_UNKNOWN_FIELD");
    EXPECT_EQ(unknown.message.rfind("graph.retryPolicy({...}) has an unknown field 'on_exhausted'", 0), 0U)
        << unknown.message;
    EXPECT_EQ(unknown.message.find("unconsumed"), std::string::npos) << unknown.message;
    expect_snapshot(unknown, R"({
        "code": "P_CORE_GC_UNKNOWN_FIELD", "severity": "error",
        "json_pointer": "/root/definition/retry_policy/on_exhausted",
        "help": {"doc": "docs/diagnostics.md#p_core_gc_unknown_field", "related": [],
                 "candidates": [], "suggestions": []}})");
}

// 5. An unsynchronized fan-in warning suggests the barrier; the sealed copy of
//    the same warning (original rules) carries no help.
TEST(DiagnosticHelpSnapshot, FanInWarningSuggestsABarrier) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"}).node("B", {type: "t.task"})
            .node("C", {type: "t.task"}).node("D", {type: "t.task"})
            .entry("A").edge("A", "B").edge("A", "C").edge("B", "D").edge("C", "D").exit("D");
    })JS"));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    const auto& e9 = only(outcome.returned, "P_CORE_E9");
    EXPECT_NE(e9.message.find("graph.barrier(\"D\", [...])"), std::string::npos) << e9.message;
    expect_snapshot(e9, R"({
        "code": "P_CORE_E9", "severity": "warning", "json_pointer": "/root/definition/nodes/D",
        "help": {"doc": "docs/diagnostics.md#p_core_e9", "related": [], "candidates": [],
                 "suggestions": [{"message": "Join the sources with graph.barrier(\"D\", [\"B\", \"C\"]) if both run",
                                  "applicability": "maybe",
                                  "edits": [{"op": "add", "path": "/root/definition/nodes/D/barrier",
                                             "value": {"wait_for": ["B", "C"]}}]}]}})");
    const auto sealed = outcome.bundle->diagnostics();
    ASSERT_FALSE(sealed.empty());
    for (const auto& diagnostic : sealed) EXPECT_FALSE(diagnostic.help.has_value()) << diagnostic.code;
}

// 6. A node type missing from the registry: candidates are registered types.
TEST(DiagnosticHelpSnapshot, RegistryMissSuggestsARegisteredName) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.tsak"}).entry("A").exit("A");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    expect_snapshot(only(outcome.returned, "P_CORE_GC_REGISTRY_MISS"), R"({
        "code": "P_CORE_GC_REGISTRY_MISS", "severity": "error",
        "json_pointer": "/root/definition/nodes/A/type",
        "help": {"doc": "docs/diagnostics.md#p_core_gc_registry_miss", "related": [],
                 "candidates": ["t.task"],
                 "suggestions": [{"message": "Replace 't.tsak' with 't.task'", "applicability": "maybe",
                                  "edits": [{"op": "replace", "path": "/root/definition/nodes/A/type",
                                             "value": "t.task"}]}]}})");
}

// 7. A builder argument error links its documentation.
TEST(DiagnosticHelpSnapshot, BuilderArgumentErrorLinksItsEntry) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"}).barrier("A", "B").entry("A").exit("A");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    expect_snapshot(only(outcome.returned, "P_JS_GRAPH_ARGUMENT"), R"({
        "code": "P_JS_GRAPH_ARGUMENT", "severity": "error", "json_pointer": "",
        "help": {"doc": "docs/diagnostics.md#p_js_graph_argument", "related": [],
                 "candidates": [], "suggestions": []}})");
}

// 8. A closed condition with an unrouted label: the suggestion adds the route
//    with a placeholder target the author must choose.
TEST(DiagnosticHelpSnapshot, UnroutedClosedLabelSuggestsAPlaceholderRoute) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("main").node("A", {type: "t.task"}).node("B", {type: "t.task"})
            .entry("A").conditionalEdge("A", "t.check", {pass: "B"}).exit("B");
    })JS"));
    ASSERT_FALSE(outcome.ok());
    expect_snapshot(only(outcome.returned, "P_CORE_E10"), R"({
        "code": "P_CORE_E10", "severity": "error", "json_pointer": "/root/definition/conditional_edges/0",
        "help": {"doc": "docs/diagnostics.md#p_core_e10", "related": [], "candidates": [],
                 "suggestions": [{"message": "Route label 'fail' to a node or __end__",
                                  "applicability": "placeholder",
                                  "edits": [{"op": "add",
                                             "path": "/root/definition/conditional_edges/0/routes/fail",
                                             "value": "<node>"}]}]}})");
}

#endif  // NEOGRAPH_V2_TESTS_HAVE_QUICKJS

// 9. An unknown Program document field: the closed field list supplies the
//    candidate.
TEST(DiagnosticHelpSnapshot, UnknownProgramFieldSuggestsTheKnownField) {
    auto document           = program_document(minimal_definition());
    document["input_contrct"] = document["input_contract"];
    const auto outcome      = compile(cpp_source(document));
    ASSERT_FALSE(outcome.ok());
    expect_snapshot(only(outcome.returned, "P_SCHEMA_UNKNOWN_FIELD"), R"({
        "code": "P_SCHEMA_UNKNOWN_FIELD", "severity": "error", "json_pointer": "/input_contrct",
        "help": {"doc": "docs/diagnostics.md#p_schema_unknown_field", "related": [],
                 "candidates": ["input_contract"],
                 "suggestions": [{"message": "Rename 'input_contrct' to 'input_contract'",
                                  "applicability": "maybe",
                                  "edits": [{"op": "move", "from": "/input_contrct",
                                             "path": "/input_contract"}]}]}})");
}

// 10. Past 20 returned diagnostics the rest are counted in one note; errors
//     are kept before warnings.
TEST(DiagnosticHelpSnapshot, ReturnedDiagnosticsAreCappedAtTwenty) {
    auto definition = minimal_definition();
    for (int index = 0; index < 25; ++index)
        definition["edges"].push_back(
            json{{"from", "work"}, {"to", "missing_" + std::to_string(100 + index)}});
    const auto outcome = compile(cpp_source(program_document(definition)));
    ASSERT_FALSE(outcome.ok());
    ASSERT_EQ(outcome.returned.size(), MAX_RETURNED_DIAGNOSTICS + 1) << codes(outcome.returned);
    for (std::size_t index = 0; index < MAX_RETURNED_DIAGNOSTICS; ++index)
        EXPECT_EQ(outcome.returned[index].severity, DiagnosticSeverity::Error) << index;
    const auto& note = outcome.returned.back();
    EXPECT_EQ(note.code, DIAGNOSTICS_TRUNCATED_CODE);
    EXPECT_EQ(note.severity, DiagnosticSeverity::Note);
    EXPECT_EQ(note.witness.at("omitted").get<std::uint64_t>(), 5U) << note.witness.dump();
    ASSERT_TRUE(note.help.has_value());
    EXPECT_EQ(note.help->doc, "docs/diagnostics.md#p_diagnostics_truncated");
}

// Every returned diagnostic carries help with at least its documentation link.
TEST(DiagnosticHelp, EveryReturnedDiagnosticCarriesHelp) {
    auto definition = minimal_definition();
    definition["nodes"]["orphan"] = json{{"type", "t.task"}};
    const auto outcome = compile(cpp_source(program_document(definition)));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    ASSERT_FALSE(outcome.returned.empty());
    for (const auto& diagnostic : outcome.returned) {
        ASSERT_TRUE(diagnostic.help.has_value()) << diagnostic.code;
        EXPECT_EQ(diagnostic.help->doc, diagnostic_doc_link(diagnostic.code));
    }
    for (const auto& diagnostic : outcome.bundle->diagnostics())
        EXPECT_FALSE(diagnostic.help.has_value()) << diagnostic.code;
}
