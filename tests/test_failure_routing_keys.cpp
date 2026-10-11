// #364 Core keys, JavaScript sugar, validation and migration shape (Program
// API v2, WP10). The runtime that acts on them is WP11; these tests pin what
// the compiler accepts, how it canonicalizes it, and every error.
//
//   "node_retry_policies": {"TEST": {"max_retries": 2, "max_visits": 3, ...}},
//   "failure_edges": [{"from": "TEST", "to": "ASK_USER", "error_channel": "failures"}],
//   "failure_exits": {"ABORT": {"code": "plan.user_abort"}}

#include "program_v2_test_support.h"

#include <neograph/graph/validator.h>

namespace {

using namespace program_v2_test;

json node(const char* type = "t.task") { return json{{"type", type}}; }
json edge(const char* from, const char* to) { return json{{"from", from}, {"to", to}}; }

// IMPLEMENT -> TEST -> exit; ASK_USER routes retry -> IMPLEMENT, abort -> ABORT.
json fix_loop() {
    return json{
        {"schema_version", 1},
        {"name", "main"},
        {"channels", json{{"failures", json{{"reducer", "append"}, {"initial", json::array()}}},
                          {"note", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"IMPLEMENT", node()}, {"TEST", node()}, {"ASK_USER", node("t.review")},
                       {"ABORT", node()}}},
        {"edges", json::array({edge("__start__", "IMPLEMENT"), edge("IMPLEMENT", "TEST"),
                               edge("TEST", "__end__")})},
        {"conditional_edges",
         json::array({json{{"from", "ASK_USER"},
                           {"condition", "t.check"},
                           {"routes", json{{"pass", "IMPLEMENT"}, {"fail", "ABORT"}}}}})},
        {"interrupt_before", json::array({"ASK_USER"})}};
}

json with_keys(json definition, json policies, json edges, json exits) {
    if (!policies.is_null()) definition["node_retry_policies"] = std::move(policies);
    if (!edges.is_null()) definition["failure_edges"] = std::move(edges);
    if (!exits.is_null()) definition["failure_exits"] = std::move(exits);
    return definition;
}

json design_keys() {
    return with_keys(fix_loop(),
                     json{{"TEST", json{{"max_retries", 2},
                                        {"non_retryable", json::array({"app.compile_error"})},
                                        {"max_visits", 3}}}},
                     json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}, {"error_channel", "failures"}}}),
                     json{{"ABORT", json{{"code", "plan.user_abort"}}}});
}

std::vector<CompilerDiagnostic> parse_errors(const json& definition) {
    GraphRegistry registry;
    return GraphCompiler::parse_report(definition, registry).diagnostics;
}

std::vector<std::string> codes_of(const std::vector<CompilerDiagnostic>& diagnostics) {
    std::vector<std::string> result;
    for (const auto& diagnostic : diagnostics)
        result.push_back(diagnostic.code + "@" + diagnostic.json_pointer);
    return result;
}

std::vector<neograph::graph::Diagnostic> validate_codes(const json& definition, std::string_view code) {
    std::vector<neograph::graph::Diagnostic> result;
    for (const auto& diagnostic : GraphValidator::validate(GraphCompiler::parse(definition)).diagnostics)
        if (diagnostic.code == code) result.push_back(diagnostic);
    return result;
}

void expect_failure_edge(const json& definition, const std::string& reason) {
    const auto errors = parse_errors(definition);
    ASSERT_EQ(errors.size(), 1U) << reason << ":" << ::testing::PrintToString(codes_of(errors));
    EXPECT_EQ(errors[0].code, "GC_FAILURE_EDGE") << reason;
    EXPECT_EQ(errors[0].witness.value("reason", ""), reason) << errors[0].witness.dump();
}

}  // namespace

// ---------------------------------------------------------------------------
// Canonical form and identity
// ---------------------------------------------------------------------------

TEST(FailureRoutingKeys, TheDesignExampleRoundTripsCanonically) {
    const auto topology = GraphCompiler::parse(design_keys());
    ASSERT_EQ(topology.node_retry_policies.count("TEST"), 1U);
    const auto& policy = topology.node_retry_policies.at("TEST");
    EXPECT_EQ(policy.max_retries, 2);
    EXPECT_EQ(policy.max_visits, 3);
    EXPECT_FALSE(policy.initial_delay_ms.has_value()) << "nothing is filled in at compile time";
    ASSERT_EQ(topology.failure_edges.size(), 1U);
    EXPECT_EQ(topology.failure_edges[0].to, "ASK_USER");
    EXPECT_EQ(topology.failure_edges[0].error_channel, std::optional<std::string>("failures"));
    EXPECT_EQ(topology.failure_exits.at("ABORT").code, "plan.user_abort");

    const auto printed = topology.to_json();
    EXPECT_EQ(canonical(printed["node_retry_policies"]),
              canonical(json::parse(R"({"TEST":{"max_retries":2,"max_visits":3,"non_retryable":["app.compile_error"]}})")));
    EXPECT_EQ(canonical(printed["failure_edges"]),
              canonical(json::parse(R"([{"from":"TEST","to":"ASK_USER","error_channel":"failures"}])")));
    EXPECT_EQ(canonical(printed["failure_exits"]), canonical(json::parse(R"({"ABORT":{"code":"plan.user_abort"}})")));
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(design_keys(), topology));
}

TEST(FailureRoutingKeys, DefaultsAndOrderCanonicalize) {
    auto definition = with_keys(
        fix_loop(),
        json{{"IMPLEMENT", json::object()},
             {"TEST", json{{"non_retryable", json::array({"app.z", "app.a"})}}}},
        json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}},
                     json{{"from", "IMPLEMENT"}, {"to", "ASK_USER"}}}),
        json{{"ABORT", json{{"code", "NG.Aborted"}}}});
    const auto printed = GraphCompiler::parse(definition).to_json();
    // An empty per-node policy is the default; the default exit code is implied.
    EXPECT_EQ(canonical(printed["node_retry_policies"]),
              canonical(json::parse(R"({"TEST":{"non_retryable":["app.a","app.z"]}})")));
    EXPECT_EQ(canonical(printed["failure_edges"]),
              canonical(json::parse(R"([{"from":"IMPLEMENT","to":"ASK_USER"},{"from":"TEST","to":"ASK_USER"}])")));
    EXPECT_EQ(canonical(printed["failure_exits"]), canonical(json::parse(R"({"ABORT":{}})")));
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(definition, GraphCompiler::parse(definition)));

    // Unused keys are absent, so definitions without them keep their bytes.
    const auto plain = GraphCompiler::parse(fix_loop()).to_json();
    EXPECT_FALSE(plain.contains("node_retry_policies"));
    EXPECT_FALSE(plain.contains("failure_edges"));
    EXPECT_FALSE(plain.contains("failure_exits"));
    const auto empty =
        GraphCompiler::parse(with_keys(fix_loop(), json::object(), json::array(), json::object())).to_json();
    EXPECT_EQ(canonical(empty), canonical(plain));
}

TEST(FailureRoutingKeys, LegacyDocumentsIgnoreTheKeys) {
    auto legacy = design_keys();
    json copy   = json::object();
    for (const auto& [key, value] : legacy.items())
        if (key != "schema_version") copy[key] = value;
    const auto topology = GraphCompiler::parse(copy);
    EXPECT_TRUE(topology.node_retry_policies.empty());
    EXPECT_TRUE(topology.failure_edges.empty());
    EXPECT_TRUE(topology.failure_exits.empty());
}

// ---------------------------------------------------------------------------
// Field rules
// ---------------------------------------------------------------------------

TEST(FailureRoutingKeys, PolicyBoundsAreFieldValueErrors) {
    struct Case {
        const char* field;
        json        value;
        long long   min;
        long long   max;
    };
    const std::vector<Case> cases{{"max_retries", 11, 0, 10},          {"max_retries", -1, 0, 10},
                                  {"initial_delay_ms", 9, 10, 60000},  {"initial_delay_ms", 60001, 10, 60000},
                                  {"max_delay_ms", 300001, 10, 300000}, {"max_visits", 0, 1, 100},
                                  {"max_visits", 101, 1, 100}};
    for (const auto& item : cases) {
        const auto errors = parse_errors(with_keys(fix_loop(), json{{"TEST", json{{item.field, item.value}}}},
                                                   nullptr, nullptr));
        ASSERT_EQ(errors.size(), 1U) << item.field << " " << item.value.dump();
        EXPECT_EQ(errors[0].code, "GC_FIELD_VALUE");
        EXPECT_EQ(errors[0].json_pointer, std::string("/node_retry_policies/TEST/") + item.field);
        EXPECT_EQ(errors[0].witness.at("min").get<long long>(), item.min);
        EXPECT_EQ(errors[0].witness.at("max").get<long long>(), item.max);
    }
    for (const auto& ok : {json{{"max_retries", 0}}, json{{"max_retries", 10}}, json{{"max_visits", 1}},
                           json{{"max_visits", 100}}, json{{"initial_delay_ms", 10}},
                           json{{"backoff_multiplier", 1.0}}, json{{"backoff_multiplier", 10.0}},
                           json{{"error_text", true}}})
        EXPECT_TRUE(parse_errors(with_keys(fix_loop(), json{{"TEST", ok}}, nullptr, nullptr)).empty()) << ok.dump();
    for (const auto& bad : {json{{"backoff_multiplier", 0.5}}, json{{"backoff_multiplier", 10.5}}})
        EXPECT_EQ(codes_of(parse_errors(with_keys(fix_loop(), json{{"TEST", bad}}, nullptr, nullptr))),
                  (std::vector<std::string>{"GC_FIELD_VALUE@/node_retry_policies/TEST/backoff_multiplier"}));
    // max_delay_ms starts at initial_delay_ms.
    EXPECT_EQ(codes_of(parse_errors(with_keys(
                  fix_loop(), json{{"TEST", json{{"initial_delay_ms", 500}, {"max_delay_ms", 400}}}}, nullptr,
                  nullptr))),
              (std::vector<std::string>{"GC_FIELD_VALUE@/node_retry_policies/TEST/max_delay_ms"}));
    EXPECT_EQ(codes_of(parse_errors(with_keys(fix_loop(), json{{"TEST", json{{"error_text", 1}}}}, nullptr, nullptr))),
              (std::vector<std::string>{"GC_FIELD_TYPE@/node_retry_policies/TEST/error_text"}));
}

TEST(FailureRoutingKeys, NonRetryableCodesAreUniqueKnownAndBounded) {
    const auto with_codes = [](json codes) {
        return with_keys(fix_loop(), json{{"TEST", json{{"non_retryable", std::move(codes)}}}}, nullptr, nullptr);
    };
    EXPECT_TRUE(parse_errors(with_codes(json::array({"app.compile_error", "NG.TaskFailed", "NG.Timeout"}))).empty());
    for (const auto& bad : {json::array({"app.x", "app.x"}), json::array({"NG.Unheard"}),
                            json::array({"compile_error"}), json::array({"App.x"}), json::array({1})}) {
        const auto errors = parse_errors(with_codes(bad));
        ASSERT_FALSE(errors.empty()) << bad.dump();
        EXPECT_EQ(errors[0].code, bad[0].is_string() ? "GC_FIELD_VALUE" : "GC_FIELD_TYPE") << bad.dump();
    }
    json many = json::array();
    for (int index = 0; index < 17; ++index) many.push_back("app.code_" + std::to_string(index));
    EXPECT_EQ(codes_of(parse_errors(with_codes(many))),
              (std::vector<std::string>{"GC_FIELD_VALUE@/node_retry_policies/TEST/non_retryable"}));
}

TEST(FailureRoutingKeys, UnknownFieldsIncludingAttemptTimeoutAreRejected) {
    EXPECT_EQ(codes_of(parse_errors(with_keys(fix_loop(), json{{"TEST", json{{"attempt_timeout_ms", 1000}}}},
                                              nullptr, nullptr))),
              (std::vector<std::string>{"GC_UNKNOWN_FIELD@/node_retry_policies/TEST/attempt_timeout_ms"}));
    EXPECT_EQ(codes_of(parse_errors(with_keys(fix_loop(), nullptr,
                                              json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}, {"via", "x"}}}),
                                              nullptr))),
              (std::vector<std::string>{"GC_UNKNOWN_FIELD@/failure_edges/0/via"}));
    EXPECT_EQ(codes_of(parse_errors(with_keys(fix_loop(), nullptr, nullptr, json{{"ABORT", json{{"why", "x"}}}}))),
              (std::vector<std::string>{"GC_UNKNOWN_FIELD@/failure_exits/ABORT/why"}));
}

TEST(FailureRoutingKeys, GraphLevelOnFailureIsUnknownWithAHint) {
    auto definition            = fix_loop();
    definition["retry_policy"] = json{{"max_retries", 1}, {"on_failure", "ASK_USER"}};
    const auto errors          = parse_errors(definition);
    ASSERT_EQ(codes_of(errors), (std::vector<std::string>{"GC_UNKNOWN_FIELD@/retry_policy/on_failure"}));
    EXPECT_NE(errors[0].message.find("failure_edges"), std::string::npos) << errors[0].message;
}

TEST(FailureRoutingKeys, ExitCodesAreAuthorCodes) {
    for (const auto* code : {"NG.LogicError", "Plan.abort", "abort", ""}) {
        const auto errors =
            parse_errors(with_keys(fix_loop(), nullptr, nullptr, json{{"ABORT", json{{"code", code}}}}));
        ASSERT_EQ(errors.size(), 1U) << code;
        EXPECT_EQ(errors[0].code, "GC_FIELD_VALUE") << code;
        EXPECT_EQ(errors[0].json_pointer, "/failure_exits/ABORT/code") << code;
    }
}

TEST(FailureRoutingKeys, AFailureExitNodeHasNoSuccessors) {
    const auto errors = parse_errors(with_keys(fix_loop(), nullptr, nullptr, json{{"TEST", json::object()}}));
    ASSERT_EQ(errors.size(), 1U);
    EXPECT_EQ(errors[0].code, "GC_FIELD_VALUE");
    EXPECT_EQ(errors[0].json_pointer, "/failure_exits/TEST");
}

// ---------------------------------------------------------------------------
// Failure-edge targets
// ---------------------------------------------------------------------------

TEST(FailureRoutingKeys, TargetRulesAreFailureEdgeErrors) {
    const auto edge_to = [](const char* from, const char* to) {
        return with_keys(fix_loop(), nullptr, json::array({json{{"from", from}, {"to", to}}}),
                         json{{"ABORT", json::object()}});
    };
    expect_failure_edge(edge_to("TEST", "__end__"), "sentinel_target");
    expect_failure_edge(edge_to("TEST", "__start__"), "sentinel_target");
    expect_failure_edge(edge_to("TEST", "TEST"), "self_target");

    // An implicit end (no edges or routes) is rejected; a failure exit is not.
    auto implicit                 = edge_to("TEST", "SINK");
    implicit["nodes"]["SINK"]     = node();
    expect_failure_edge(implicit, "implicit_end_target");
    EXPECT_TRUE(parse_errors(edge_to("TEST", "ABORT")).empty());

    // Every join is rejected, whether or not `from` is a member.
    auto join                                  = edge_to("IMPLEMENT", "TEST");
    join["nodes"]["TEST"]["barrier"]           = json{{"wait_for", json::array({"IMPLEMENT"})}};
    expect_failure_edge(join, "join_target");

    // At most one failure edge per node.
    auto twice = with_keys(fix_loop(), nullptr,
                           json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}},
                                        json{{"from", "TEST"}, {"to", "IMPLEMENT"}}}),
                           nullptr);
    expect_failure_edge(twice, "duplicate_from");

    // error_channel must use the built-in append reducer.
    auto overwrite = with_keys(fix_loop(), nullptr,
                               json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}, {"error_channel", "note"}}}),
                               nullptr);
    expect_failure_edge(overwrite, "error_channel_reducer");
}

TEST(FailureRoutingKeys, UnknownNodesAndChannelsAreE3) {
    const auto definition = with_keys(
        fix_loop(), json{{"NOPE", json{{"max_retries", 1}}}},
        json::array({json{{"from", "TEST"}, {"to", "ASK_USR"}, {"error_channel", "failurs"}}}),
        json{{"GONE", json::object()}});
    std::set<std::string> unknown;
    for (const auto& diagnostic : validate_codes(definition, "E3"))
        unknown.insert(diagnostic.witness.at("unknown").get<std::string>());
    EXPECT_EQ(unknown, (std::set<std::string>{"ASK_USR", "GONE", "NOPE", "failurs"}));
}

// ---------------------------------------------------------------------------
// Validator integration
// ---------------------------------------------------------------------------

TEST(FailureRoutingKeys, FailureEdgesJoinTheRoutedSuccessorMap) {
    // ASK_USER is reachable only through TEST's failure edge.
    auto definition                  = with_keys(fix_loop(), nullptr,
                                                 json::array({json{{"from", "TEST"}, {"to", "ASK_USER"}}}),
                                                 json{{"ABORT", json::object()}});
    EXPECT_TRUE(validate_codes(definition, "E7").empty());
    EXPECT_FALSE(validate_codes(fix_loop(), "E7").empty()) << "without the edge ASK_USER is unreachable";
    EXPECT_EQ(GraphValidator::routed_successors(GraphCompiler::parse(definition)).at("TEST"),
              (std::set<std::string>{"ASK_USER", "__end__"}));
}

TEST(FailureRoutingKeys, AFailureEdgeAroundAJoinIsAnE15Bypass) {
    json definition{{"schema_version", 1},
                    {"name", "main"},
                    {"nodes", json{{"A", node()}, {"B", node()}, {"C", node()}, {"H", node()}, {"D", node()}}},
                    {"edges", json::array({edge("__start__", "A"), edge("A", "B"), edge("A", "C"),
                                           edge("B", "D"), edge("C", "D"), edge("H", "__end__"),
                                           edge("D", "__end__")})},
                    {"failure_edges", json::array({json{{"from", "B"}, {"to", "H"}}})}};
    definition["nodes"]["D"]["barrier"] = json{{"wait_for", json::array({"B", "C"})}};
    const auto e15 = validate_codes(definition, "E15");
    ASSERT_EQ(e15.size(), 1U);
    EXPECT_EQ(canonical(e15[0].witness.at("routes")), canonical(json{{"on_failure", "H"}}));

    // A handler that is an alternative of the member's slot stands in for it.
    definition["edges"].push_back(edge("H", "D"));
    definition["nodes"]["D"]["barrier"] =
        json{{"wait_for", json::array({json{{"any_of", json::array({"B", "H"})}}, "C"})}};
    EXPECT_TRUE(validate_codes(definition, "E15").empty());
}

TEST(FailureRoutingKeys, ErrorChannelCountsAsWritten) {
    // Every node declares effects here, so E6 runs.
    RegistrySnapshotBuilder builder;
    builder.add_node(manifest(ExecutableKind::Node, "t.task", '1'),
                     [](const std::string& name, const json&, const NodeContext&) {
                         return std::make_unique<QuietNode>(name);
                     },
                     json{{"type", "object"}}, json{{"reads", json::array({"failures"})}, {"writes", json::array()}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "append", '3'),
                        [](const json&, const json& incoming) { return json(incoming); });
    const auto registry = std::move(builder).build();
    json       definition{{"schema_version", 1},
                          {"name", "main"},
                          {"channels", json{{"failures", json{{"reducer", "append"}}}}},
                          {"nodes", json{{"A", node()}, {"H", node()}}},
                          {"edges", json::array({edge("__start__", "A"), edge("A", "__end__"),
                                                 edge("H", "__end__")})},
                          {"failure_edges",
                           json::array({json{{"from", "A"}, {"to", "H"}, {"error_channel", "failures"}}})}};
    const auto outcome = compile(cpp_source(program_document(definition)),
                                 ProgramCompiler(registry, {"program-v2-test/v1"}));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    EXPECT_TRUE(with_code(outcome.returned, "P_CORE_E6").empty()) << codes(outcome.returned);
}

TEST(FailureRoutingKeys, RetryPolicyInsideNodeConfigIsLinted) {
    auto definition                              = fix_loop();
    definition["nodes"]["TEST"]["retry_policy"]  = json{{"max_retries", 2}};
    const auto outcome                           = compile(cpp_source(program_document(definition)));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    const auto lint = with_code(outcome.returned, "P_CORE_E18");
    ASSERT_EQ(lint.size(), 1U) << codes(outcome.returned);
    EXPECT_EQ(lint[0].severity, DiagnosticSeverity::Warning);
    EXPECT_EQ(lint[0].primary.json_pointer, "/root/definition/nodes/TEST/retry_policy");
    EXPECT_TRUE(with_code(outcome.bundle->diagnostics(), "P_CORE_E18").empty()) << "the lint is never sealed";
    EXPECT_EQ(diagnostic_doc_link("P_CORE_E18"), "docs/diagnostics.md#p_core_e18");
    EXPECT_EQ(diagnostic_doc_link("P_CORE_GC_FAILURE_EDGE"), "docs/diagnostics.md#p_core_gc_failure_edge");
}

// ---------------------------------------------------------------------------
// Program: sealing, bundles, migration shape
// ---------------------------------------------------------------------------

TEST(FailureRoutingKeys, BundlesSealTheKeysAndNoValidatorWarnings) {
    auto definition               = design_keys();
    definition["nodes"]["orphan"] = node();
    const auto outcome            = compile(cpp_source(program_document(definition)));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    EXPECT_TRUE(outcome.bundle->diagnostics().empty()) << codes(outcome.bundle->diagnostics());
    const auto sealed = outcome.bundle->sealed_core_definitions().at(0).definition;
    EXPECT_TRUE(sealed.contains("node_retry_policies"));
    EXPECT_TRUE(sealed.contains("failure_edges"));
    EXPECT_TRUE(sealed.contains("failure_exits"));
    EXPECT_EQ(ProgramBundle::parse(outcome.bundle->serialize_canonical()).id(), outcome.bundle->id());

    // The sealed validator applies the same rules.
    auto tampered                                  = sealed;
    tampered["node_retry_policies"]["TEST"]["max_retries"] = 11;
    SealedCoreDefinition bad{"main", sealed_core_definition_hash(tampered), tampered};
    EXPECT_THROW((void)core_compiled_plan_identity(bad, "build", outcome.bundle->registry_snapshot_fingerprint(),
                                                   outcome.bundle->executable_registry_identities()),
                 std::invalid_argument);
}

TEST(FailureRoutingKeys, CapabilityFlagsWaitForTheRuntime) {
    // The keys compile now; the flags turn on with the runtime (WP11).
    const auto features = javascript_authoring_capability_manifest().at("features");
    EXPECT_FALSE(features.at("retry_policy.per_node").get<bool>());
    EXPECT_FALSE(features.at("failure_edges").get<bool>());
    EXPECT_FALSE(features.at("failure_exits").get<bool>());
    bool found = false;
    for (const auto& method : javascript_authoring_capability_manifest().at("define").at("graph_builder_methods"))
        found = found || method.at("name").get<std::string>() == "failureExit";
    EXPECT_TRUE(found);
}

#if defined(NEOGRAPH_V2_TESTS_HAVE_QUICKJS)

// The design example: retryPolicy(node, {...}) lowers to two Core keys.
TEST(FailureRoutingKeys, JavaScriptSugarLowersToTheCoreKeys) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("fix")
          .channel("failures", {reducer: "append", initial: []})
          .node("IMPLEMENT", {type: "t.task"}).node("TEST", {type: "t.task"})
          .node("ASK_USER", {type: "t.review"}).node("ABORT", {type: "t.task"})
          .entry("IMPLEMENT").edge("IMPLEMENT", "TEST").exit("TEST")
          .conditionalEdge("ASK_USER", "t.check", {pass: "IMPLEMENT", fail: "ABORT"})
          .retryPolicy("TEST", {max_retries: 2, non_retryable: ["app.compile_error"], max_visits: 3,
                                on_failure: "ASK_USER", error_channel: "failures"})
          .interruptBefore("ASK_USER")
          .failureExit("ABORT", {code: "plan.user_abort"});
    })JS"));
    ASSERT_TRUE(outcome.ok()) << codes(outcome.returned);
    const auto definition = outcome.bundle->sealed_core_definitions().at(0).definition;
    EXPECT_EQ(canonical(definition["node_retry_policies"]),
              canonical(json::parse(R"({"TEST":{"max_retries":2,"max_visits":3,"non_retryable":["app.compile_error"]}})")));
    EXPECT_EQ(canonical(definition["failure_edges"]),
              canonical(json::parse(R"([{"error_channel":"failures","from":"TEST","to":"ASK_USER"}])")));
    EXPECT_EQ(canonical(definition["failure_exits"]), canonical(json::parse(R"({"ABORT":{"code":"plan.user_abort"}})")));
    EXPECT_FALSE(definition.contains("retry_policy")) << "the graph-level policy is untouched";
}

TEST(FailureRoutingKeys, JavaScriptSugarRejectsMisuse) {
    const std::vector<std::string> misuse{
        R"(.retryPolicy("TEST", {error_channel: "failures"}))",          // channel without route
        R"(.retryPolicy("TEST", {max_retries: 1}).retryPolicy("TEST", {max_visits: 2}))",  // twice
        R"(.retryPolicy("NOPE", {max_retries: 1}))",                      // undeclared node
        R"(.retryPolicy("TEST", "ASK_USER"))",                           // not an object
        R"(.failureExit("ABORT").failureExit("ABORT"))",                  // twice
        R"(.failureExit("NOPE"))",                                        // undeclared node
    };
    for (const auto& call : misuse) {
        const auto outcome = compile(js_source(R"JS(export function define() {
            return ng.graph("fix").channel("failures", {reducer: "append", initial: []})
              .node("TEST", {type: "t.task"}).node("ASK_USER", {type: "t.task"}).node("ABORT", {type: "t.task"})
              .entry("TEST").exit("TEST").edge("ASK_USER", "TEST"))JS" + call + R"JS(;
        })JS"));
        ASSERT_FALSE(outcome.ok()) << call;
        EXPECT_EQ(outcome.returned.at(0).code, "P_JS_GRAPH_ARGUMENT") << call << codes(outcome.returned);
    }
}

TEST(FailureRoutingKeys, JavaScriptPolicyErrorsNameTheBuilderCall) {
    const auto outcome = compile(js_source(R"JS(export function define() {
        return ng.graph("fix").node("TEST", {type: "t.task"}).entry("TEST").exit("TEST")
          .retryPolicy("TEST", {max_retries: 2, attempt_timeout_ms: 1000});
    })JS"));
    ASSERT_FALSE(outcome.ok());
    const auto unknown = with_code(outcome.returned, "P_CORE_GC_UNKNOWN_FIELD");
    ASSERT_EQ(unknown.size(), 1U) << codes(outcome.returned);
    EXPECT_EQ(unknown[0].message.rfind("graph.retryPolicy(\"TEST\", {...}) has an unknown field 'attempt_timeout_ms'", 0), 0U)
        << unknown[0].message;
    EXPECT_TRUE(unknown[0].primary.span.has_value());
}

#endif
