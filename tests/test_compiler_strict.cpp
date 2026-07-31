// M1 "regression seal" tests (issue #75): consumed-key accounting
// (strict mode via schema_version), CompiledGraph::to_json() re-emission,
// canon() normalization, and translation validation (verify_roundtrip).
//
// The contract under test:
//   canon(definition) == canon(compile(definition).to_json())
// for every accepted document — a mismatch is a silent drop/rewire, the
// v0.1.0–v0.1.7 conditional_edges regression class. Strict documents
// must refuse unknown/unconsumed keys instead of ignoring them.

#include <neograph/graph/compiler.h>
#include <neograph/neograph.h>

#include <gtest/gtest.h>

#include <algorithm>

using namespace neograph;
using namespace neograph::graph;

namespace {

class StrictNoopNode : public GraphNode {
public:
    explicit StrictNoopNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

void ensure_types_registered() {
    // "snoop": registered WITHOUT a declared schema — permissive, any
    // config accepted even in strict mode (models the cookbook's
    // custom nodes with free-form config).
    NodeFactory::instance().register_type(
        "snoop", [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StrictNoopNode>(name);
        });
    // "snoop_closed": registered WITH a declared schema — strict mode
    // enforces closed-world config keys.
    NodeFactory::instance().register_type(
        "snoop_closed",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StrictNoopNode>(name);
        },
        json::parse(R"({"type":"object","properties":{"knob":{"type":"string"}}})"));
    NodeFactory::instance().register_type(
        "snoop_validated",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StrictNoopNode>(name);
        },
        json::parse(R"({
            "type": "object",
            "properties": {
                "count": {"type": "integer"},
                "mode": {"type": "string", "enum": ["fast", "safe"]},
                "options": {
                    "type": "object",
                    "properties": {"enabled": {"type": "boolean"}},
                    "required": ["enabled"]
                }
            },
            "required": ["count", "mode"]
        })"));
}

// The yyjson wrapper has no erase() — rebuild minus one key.
json without_key(const json& obj, const std::string& key) {
    json out = json::object();
    for (const auto& [k, v] : obj.items()) {
        if (k != key) out[k] = v;
    }
    return out;
}

json minimal_strict() {
    return json{
        {"schema_version", 1},
        {"nodes", {{"a", {{"type", "snoop"}}}}},
        {"edges",
         json::array({{{"from", "__start__"}, {"to", "a"}}, {{"from", "a"}, {"to", "__end__"}}})},
    };
}

// Expect compile() to throw and the message to contain every fragment.
void expect_strict_error(const json& def, std::initializer_list<const char*> fragments) {
    ensure_types_registered();
    try {
        GraphCompiler::compile(def, NodeContext{});
        FAIL() << "expected strict validation to reject the document";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        for (const char* f : fragments) {
            EXPECT_NE(msg.find(f), std::string::npos)
                << "error message missing '" << f << "'\nfull message: " << msg;
        }
    }
}

}  // namespace

// =========================================================================
// Strict mode: consumed-key accounting
// =========================================================================

TEST(StrictCompiler, TypoConditionalEdgesIsError) {
    auto def                  = minimal_strict();
    def["conditionnal_edges"] = json::array();  // the historical nightmare
    expect_strict_error(def, {"conditionnal_edges", "unknown or unconsumed"});
}

TEST(StrictCompiler, UnknownChannelKeyIsError) {
    auto def        = minimal_strict();
    def["channels"] = {{"c", {{"reducer", "append"}, {"initail", 5}}}};
    expect_strict_error(def, {"channels.c", "initail"});
}

TEST(StrictCompiler, DeclaredSchemaNodeConfigTypoIsError) {
    auto def          = minimal_strict();
    def["nodes"]["b"] = {{"type", "snoop_closed"}, {"knbo", "x"}};
    expect_strict_error(def, {"nodes.b", "knbo"});
}

TEST(StrictCompiler, BuiltinIntentClassifierConfigTypoIsError) {
    auto def           = minimal_strict();
    def["nodes"]["ic"] = {{"type", "intent_classifier"},
                          {"routes", json::array({"x"})},
                          {"promt", "classify"}};  // typo of "prompt"
    expect_strict_error(def, {"nodes.ic", "promt"});
}

TEST(StrictCompiler, MissingRequiredNodeConfigIsError) {
    auto def           = minimal_strict();
    def["nodes"]["ic"] = {{"type", "intent_classifier"}, {"prompt", "classify"}};
    expect_strict_error(def, {"nodes.ic", "required", "routes"});
}

TEST(StrictCompiler, WrongNodeConfigTypeIsError) {
    auto def          = minimal_strict();
    def["nodes"]["v"] = {{"type", "snoop_validated"}, {"count", "many"}, {"mode", "fast"}};
    expect_strict_error(def, {"nodes.v.count", "type", "integer", "string"});
}

TEST(StrictCompiler, NodeConfigEnumMismatchIsError) {
    auto def          = minimal_strict();
    def["nodes"]["v"] = {{"type", "snoop_validated"}, {"count", 3}, {"mode", "turbo"}};
    expect_strict_error(def, {"nodes.v.mode", "enum", "fast", "safe", "turbo"});
}

TEST(StrictCompiler, NestedSchemaKeywordsAreValidated) {
    auto def          = minimal_strict();
    def["nodes"]["v"] = {
        {"type", "snoop_validated"}, {"count", 3}, {"mode", "safe"}, {"options", json::object()}};
    expect_strict_error(def, {"nodes.v.options", "required", "enabled"});
}

TEST(StrictCompiler, SchemaViolationsAreAggregatedInStablePathOrder) {
    ensure_types_registered();
    auto def          = minimal_strict();
    def["nodes"]["v"] = {
        {"type", "snoop_validated"}, {"mode", "turbo"}, {"options", {{"enabled", 1}}}};

    try {
        GraphCompiler::compile(def, NodeContext{});
        FAIL() << "expected strict schema validation to reject the document";
    } catch (const std::runtime_error& e) {
        const std::string msg        = e.what();
        const auto        count_pos  = msg.find("nodes.v: keyword 'required'");
        const auto        mode_pos   = msg.find("nodes.v.mode: keyword 'enum'");
        const auto        option_pos = msg.find("nodes.v.options.enabled: keyword 'type'");
        ASSERT_NE(count_pos, std::string::npos) << msg;
        ASSERT_NE(mode_pos, std::string::npos) << msg;
        ASSERT_NE(option_pos, std::string::npos) << msg;
        EXPECT_LT(count_pos, mode_pos);
        EXPECT_LT(mode_pos, option_pos);
    }
}

TEST(StrictCompiler, PermissiveCustomTypeConfigAllowed) {
    ensure_types_registered();
    auto def          = minimal_strict();
    def["nodes"]["a"] = {
        {"type", "snoop"}, {"model_path", "assets/foo.bin"}, {"vad_threshold", 0.5}};
    EXPECT_NO_THROW(GraphCompiler::compile(def, NodeContext{}));
}

TEST(StrictCompiler, RetryPolicyTypoIsError) {
    auto def            = minimal_strict();
    def["retry_policy"] = {{"max_retry", 3}};  // typo of "max_retries"
    expect_strict_error(def, {"retry_policy", "max_retry"});
}

TEST(StrictCompiler, EmptyBarrierWaitForIsError) {
    auto def                     = minimal_strict();
    def["nodes"]["a"]["barrier"] = {{"wait_for", json::array()}};
    expect_strict_error(def, {"nodes.a.barrier", "silently dropped"});
}

TEST(StrictCompiler, BarrierMissingWaitForIsError) {
    auto def                     = minimal_strict();
    def["nodes"]["a"]["barrier"] = json::object();
    expect_strict_error(def, {"nodes.a.barrier", "silently dropped"});
}

TEST(StrictCompiler, UnknownBarrierKeyIsError) {
    auto def                     = minimal_strict();
    def["nodes"]["a"]["barrier"] = {{"wait_for", json::array({"b"})},
                                    {"wiat_for", json::array({"c"})}};
    expect_strict_error(def, {"nodes.a.barrier", "wiat_for"});
}

TEST(StrictCompiler, InlineConditionalToIsError) {
    // 'to' on an inline conditional edge is silently ignored by the
    // parser (routing goes through 'routes') — strict mode surfaces it.
    auto def = minimal_strict();
    def["edges"].push_back({{"from", "a"},
                            {"to", "__end__"},
                            {"condition", "has_tool_calls"},
                            {"routes", {{"true", "a"}}}});
    expect_strict_error(def, {"edges[2]", "'to'"});
}

TEST(StrictCompiler, UnknownPlainEdgeKeyIsError) {
    auto def = minimal_strict();
    def["edges"].push_back({{"from", "a"}, {"to", "__end__"}, {"weight", 3}});
    expect_strict_error(def, {"edges[2]", "weight"});
}

TEST(StrictCompiler, AnnotationsAlwaysAllowed) {
    ensure_types_registered();
    auto def                      = minimal_strict();
    def["_comment"]               = "top-level note";
    def["x-studio"]               = {{"zoom", 1.5}};
    def["channels"]               = {{"c", {{"reducer", "append"}, {"_comment", "log"}}}};
    def["nodes"]["a"]["_comment"] = "node note";
    def["nodes"]["a"]["x-pos"]    = {{"x", 10}, {"y", 20}};
    EXPECT_NO_THROW(GraphCompiler::compile(def, NodeContext{}));
}

TEST(StrictCompiler, AggregatesAllErrors) {
    auto def                  = minimal_strict();
    def["conditionnal_edges"] = json::array();
    def["retry_policy"]       = {{"max_retry", 3}};
    expect_strict_error(def, {"2 error(s)", "conditionnal_edges", "max_retry"});
}

TEST(StrictCompiler, SchemaVersionTooNewIsError) {
    auto def              = minimal_strict();
    def["schema_version"] = 2;
    expect_strict_error(def, {"newer than this engine"});
}

TEST(StrictCompiler, SchemaVersionMustBeInteger) {
    auto def              = minimal_strict();
    def["schema_version"] = "1";
    expect_strict_error(def, {"schema_version", "integer"});
}

TEST(CompilerDiagnostics, SchemaVersionHasStableCodePointerAndWitness) {
    ensure_types_registered();
    auto def              = minimal_strict();
    def["schema_version"] = 2;
    const auto report     = GraphCompiler::parse_report(def, GraphRegistry::global());
    ASSERT_TRUE(report.has_errors());
    ASSERT_EQ(report.diagnostics.size(), 1u);
    const auto& diagnostic = report.diagnostics.front();
    EXPECT_EQ(diagnostic.code, "GC_SCHEMA_VERSION");
    EXPECT_EQ(diagnostic.json_pointer, "/schema_version");
    EXPECT_EQ(diagnostic.witness["actual"].get<int>(), 2);
    EXPECT_EQ(diagnostic.witness["maximum"].get<int>(), TOPOLOGY_SCHEMA_VERSION);
    EXPECT_FALSE(report.topology.has_value());
}

TEST(CompilerDiagnostics, FieldTypeAndRequiredFieldsAreTyped) {
    ensure_types_registered();
    auto def        = minimal_strict();
    def["channels"] = "not-an-object";
    def["edges"].push_back({{"from", "a"}});
    const auto report = GraphCompiler::parse_report(def, GraphRegistry::global());
    ASSERT_TRUE(report.has_errors());
    bool saw_type     = false;
    bool saw_required = false;
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.code == "GC_FIELD_TYPE") {
            saw_type = true;
            EXPECT_EQ(diagnostic.json_pointer, "/channels");
            EXPECT_EQ(diagnostic.witness["expected"].get<std::string>(), "object");
            EXPECT_EQ(diagnostic.witness["actual"].get<std::string>(), "string");
        }
        if (diagnostic.code == "GC_FIELD_REQUIRED") {
            saw_required = true;
            EXPECT_EQ(diagnostic.json_pointer, "/edges/2/to");
            EXPECT_EQ(diagnostic.witness["field"].get<std::string>(), "to");
        }
    }
    EXPECT_TRUE(saw_type);
    EXPECT_TRUE(saw_required);
}

TEST(CompilerDiagnostics, UnknownFieldPointerUsesRfc6901Escaping) {
    ensure_types_registered();
    auto def          = minimal_strict();
    def["bad/~field"] = true;
    const auto report = GraphCompiler::parse_report(def, GraphRegistry::global());
    ASSERT_TRUE(report.has_errors());
    ASSERT_EQ(report.diagnostics.size(), 1u);
    const auto& diagnostic = report.diagnostics.front();
    EXPECT_EQ(diagnostic.code, "GC_UNKNOWN_FIELD");
    EXPECT_EQ(diagnostic.json_pointer, "/bad~1~0field");
    EXPECT_EQ(diagnostic.witness["field"].get<std::string>(), "bad/~field");
}

TEST(CompilerDiagnostics, NodeSchemaBarrierAndEdgeFamiliesCarryWitnesses) {
    ensure_types_registered();
    {
        auto def          = minimal_strict();
        def["nodes"]["v"] = {{"type", "snoop_validated"}, {"count", 1}, {"mode", "turbo"}};
        const auto report = GraphCompiler::parse_report(def, GraphRegistry::global());
        ASSERT_TRUE(report.has_errors());
        const auto it = std::find_if(report.diagnostics.begin(), report.diagnostics.end(),
                                     [](const CompilerDiagnostic& diagnostic) {
                                         return diagnostic.code == "GC_NODE_CONFIG";
                                     });
        ASSERT_NE(it, report.diagnostics.end());
        EXPECT_EQ(it->json_pointer, "/nodes/v/mode");
        EXPECT_EQ(it->witness["node_type"].get<std::string>(), "snoop_validated");
        EXPECT_EQ(it->witness["keyword"].get<std::string>(), "enum");
        EXPECT_EQ(it->witness["actual"].get<std::string>(), "turbo");
    }
    {
        auto def                     = minimal_strict();
        def["nodes"]["a"]["barrier"] = json::object();
        const auto report            = GraphCompiler::parse_report(def, GraphRegistry::global());
        ASSERT_TRUE(report.has_errors());
        const auto& diagnostic = report.diagnostics.front();
        EXPECT_EQ(diagnostic.code, "GC_BARRIER");
        EXPECT_EQ(diagnostic.json_pointer, "/nodes/a/barrier");
        EXPECT_TRUE(diagnostic.witness.contains("wait_for"));
    }
    {
        auto def = minimal_strict();
        def["edges"].push_back("not-an-edge");
        const auto report = GraphCompiler::parse_report(def, GraphRegistry::global());
        ASSERT_TRUE(report.has_errors());
        const auto& diagnostic = report.diagnostics.front();
        EXPECT_EQ(diagnostic.code, "GC_EDGE");
        EXPECT_EQ(diagnostic.json_pointer, "/edges/2");
        EXPECT_EQ(diagnostic.witness["edge"].get<std::string>(), "not-an-edge");
    }
}

TEST(CompilerDiagnostics, RetryRangeWitnessPreservesUnsignedInput) {
    ensure_types_registered();
    auto def                           = minimal_strict();
    def["retry_policy"]                = json::object();
    def["retry_policy"]["max_retries"] = json::parse("18446744073709551615");
    const auto report                  = GraphCompiler::parse_report(def, GraphRegistry::global());

    ASSERT_TRUE(report.has_errors());
    const auto diagnostic =
        std::find_if(report.diagnostics.begin(), report.diagnostics.end(), [](const auto& value) {
            return value.code == "GC_FIELD_VALUE" &&
                   value.json_pointer == "/retry_policy/max_retries";
        });
    ASSERT_NE(diagnostic, report.diagnostics.end());
    EXPECT_EQ(diagnostic->witness["actual"].dump(), "18446744073709551615");
}

TEST(CompilerDiagnostics, LocalReportReturnsAllGlobalOnlyRegistryMisses) {
    ensure_types_registered();
    auto def                 = minimal_strict();
    def["channels"]          = {{"state", {{"reducer", "append"}}}};
    def["conditional_edges"] = json::array(
        {{{"from", "a"}, {"condition", "route_channel"}, {"routes", {{"default", "__end__"}}}}});
    GraphRegistry local;
    const auto    report = GraphCompiler::parse_local_report(def, local);
    ASSERT_TRUE(report.has_errors());
    ASSERT_FALSE(report.topology.has_value());
    ASSERT_EQ(report.diagnostics.size(), 3u);
    EXPECT_EQ(report.diagnostics[0].code, "GC_REGISTRY_MISS");
    EXPECT_EQ(report.diagnostics[0].json_pointer, "/channels/state/reducer");
    EXPECT_EQ(report.diagnostics[0].witness["kind"].get<std::string>(), "reducer");
    EXPECT_EQ(report.diagnostics[0].witness["name"].get<std::string>(), "append");
    EXPECT_EQ(report.diagnostics[1].code, "GC_REGISTRY_MISS");
    EXPECT_EQ(report.diagnostics[1].json_pointer, "/nodes/a/type");
    EXPECT_EQ(report.diagnostics[1].witness["kind"].get<std::string>(), "node");
    EXPECT_EQ(report.diagnostics[1].witness["name"].get<std::string>(), "snoop");
    EXPECT_EQ(report.diagnostics[2].code, "GC_REGISTRY_MISS");
    EXPECT_EQ(report.diagnostics[2].json_pointer, "/conditional_edges/0/condition");
    EXPECT_EQ(report.diagnostics[2].witness["kind"].get<std::string>(), "condition");
    EXPECT_EQ(report.diagnostics[2].witness["name"].get<std::string>(), "route_channel");
}

TEST(CompilerDiagnostics, RoundTripReportIsTotalForMalformedAuthoredShapes) {
    ensure_types_registered();
    const auto topology = GraphCompiler::parse(minimal_strict());

    std::vector<json> malformed;
    {
        auto value    = minimal_strict();
        value["name"] = json::object();
        malformed.push_back(std::move(value));
    }
    {
        auto value                                  = minimal_strict();
        value["retry_policy"]["backoff_multiplier"] = "not-a-number";
        malformed.push_back(std::move(value));
    }
    {
        auto value        = minimal_strict();
        value["edges"][0] = json{{"from", json::array()}, {"to", "a"}};
        malformed.push_back(std::move(value));
    }
    {
        auto value                = minimal_strict();
        value["interrupt_before"] = json::array({1});
        malformed.push_back(std::move(value));
    }
    {
        auto value                                 = minimal_strict();
        value["nodes"]["a"]["barrier"]["wait_for"] = "not-an-array";
        malformed.push_back(std::move(value));
    }

    for (const auto& value : malformed) {
        RoundTripReport report;
        EXPECT_NO_THROW(report = GraphCompiler::verify_roundtrip_report(value, topology));
        EXPECT_TRUE(report.has_errors());
    }
}

// =========================================================================
// Lenient mode: historical behavior preserved byte-for-byte
// =========================================================================

TEST(LenientCompiler, UnknownTopLevelKeyStillIgnored) {
    ensure_types_registered();
    json def = {{"nodes", {{"a", {{"type", "snoop"}}}}}, {"conditionnal_edges", json::array()}};
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    EXPECT_TRUE(cg.conditional_edges.empty());
    EXPECT_EQ(cg.schema_version, 0);
}

TEST(LenientCompiler, EmptyBarrierStillDropped) {
    ensure_types_registered();
    json def = {
        {"nodes", {{"a", {{"type", "snoop"}, {"barrier", {{"wait_for", json::array()}}}}}}}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_TRUE(cg.barrier_specs.empty());
}

TEST(LenientCompiler, EmptyStringsAndNegativeRetryKeepHistoricalParseBehavior) {
    ensure_types_registered();
    auto def                = minimal_strict();
    def                     = without_key(def, "schema_version");
    def["name"]             = "";
    def["nodes"]["a"][""]   = 1;
    def["edges"][0]["from"] = "";
    def["retry_policy"]     = json{{"max_retries", -1}};

    const auto topology = GraphCompiler::parse(def);
    EXPECT_EQ(topology.name, "");
    EXPECT_TRUE(topology.node_defs.at("a").contains(""));
    EXPECT_EQ(topology.edges.front().from, "");
    ASSERT_TRUE(topology.retry_policy.has_value());
    EXPECT_EQ(topology.retry_policy->max_retries, -1);
}

TEST(LenientCompiler, InvalidDeclaredNodeSchemaStillUsesLegacyFactoryBehavior) {
    ensure_types_registered();
    json def = {
        {"nodes", {{"v", {{"type", "snoop_validated"}, {"count", "many"}, {"mode", "turbo"}}}}}};
    EXPECT_NO_THROW(GraphCompiler::compile(def, NodeContext{}));
}

TEST(LenientCompiler, ConditionalMismatchKeepsHistoricalLastRouteFallback) {
    ensure_types_registered();
    auto def = minimal_strict();
    def      = without_key(def, "schema_version");
    def["conditional_edges"] =
        json::array({{{"from", "a"},
                      {"condition", "route_channel"},
                      {"routes", {{"fast", "a"}, {"zzz_default", "__end__"}}}}});

    auto       cg = GraphCompiler::compile(def, NodeContext{});
    Scheduler  scheduler(cg.edges, cg.conditional_edges);
    GraphState state;
    state.init_channel("__route__", ReducerType::OVERWRITE, nullptr, json("unknown"));
    EXPECT_EQ(scheduler.resolve_next_nodes("a", state), std::vector<std::string>{"__end__"});
    const auto emitted = cg.to_json();
    EXPECT_EQ(emitted["conditional_edges"][0]["routes"], def["conditional_edges"][0]["routes"]);
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg));
}

TEST(StrictCompiler, ConditionalExplicitDefaultSurvivesRoundTrip) {
    ensure_types_registered();
    auto def                 = minimal_strict();
    def["conditional_edges"] = json::array({{{"from", "a"},
                                             {"condition", "route_channel"},
                                             {"routes", {{"fast", "a"}, {"default", "__end__"}}}}});

    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_EQ(cg.conditional_edges[0].routes.at("default"), "__end__");
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg));
}

// =========================================================================
// canon() + to_json(): translation-validation round-trip
// =========================================================================

namespace {

json full_feature_def() {
    return json{
        {"schema_version", 1},
        {"name", "full"},
        {"channels",
         {
             {"messages", {{"reducer", "append"}}},
             {"counter", {{"reducer", "overwrite"}, {"initial", 42}}},
             {"maybe", {{"reducer", "overwrite"}, {"initial", nullptr}}},
         }},
        {"nodes",
         {
             {"a", {{"type", "snoop"}, {"free_form", {{"nested", true}}}}},
             {"b", {{"type", "snoop_closed"}, {"knob", "v"}}},
             {"j", {{"type", "snoop"}, {"barrier", {{"wait_for", json::array({"b", "a"})}}}}},
         }},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "a"}},
                      {{"from", "__start__"}, {"to", "b"}},
                      {{"from", "a"}, {"to", "j"}},
                      {{"from", "b"}, {"to", "j"}},
                      // legacy inline conditional form:
                      {{"from", "j"},
                       {"condition", "has_tool_calls"},
                       {"routes", {{"true", "a"}, {"false", "__end__"}}}},
                  })},
        {"conditional_edges", json::array({
                                  {{"from", "a"},
                                   {"condition", "route_channel"},
                                   {"routes", {{"x", "b"}, {"default", "__end__"}}}},
                              })},
        {"interrupt_before", json::array({"j"})},
        {"interrupt_after", json::array({"a", "b"})},
        {"retry_policy", {{"max_retries", 3}, {"backoff_multiplier", 1.5}}},
    };
}

}  // namespace

TEST(RoundTrip, FullFeatureCanonEquality) {
    ensure_types_registered();
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    EXPECT_EQ(GraphCompiler::canon(def), GraphCompiler::canon(cg.to_json()))
        << "input:    " << GraphCompiler::canon(def).dump(2)
        << "\nreemit: " << GraphCompiler::canon(cg.to_json()).dump(2);
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg));
}

TEST(RoundTrip, ExplicitNullInitialSurvives) {
    ensure_types_registered();
    auto def       = full_feature_def();
    auto cg        = GraphCompiler::compile(def, NodeContext{});
    auto reemitted = cg.to_json();
    ASSERT_TRUE(reemitted["channels"]["maybe"].contains("initial"));
    EXPECT_TRUE(reemitted["channels"]["maybe"]["initial"].is_null());
    EXPECT_FALSE(reemitted["channels"]["messages"].contains("initial"));
}

TEST(RoundTrip, InlineAndTopLevelConditionalCanonToSameForm) {
    json inline_form = {
        {"nodes", {{"a", {{"type", "snoop"}}}}},
        {"edges",
         json::array(
             {{{"from", "a"}, {"condition", "route_channel"}, {"routes", {{"x", "__end__"}}}}})}};
    json top_form = {
        {"nodes", {{"a", {{"type", "snoop"}}}}},
        {"conditional_edges",
         json::array(
             {{{"from", "a"}, {"condition", "route_channel"}, {"routes", {{"x", "__end__"}}}}})}};
    EXPECT_EQ(GraphCompiler::canon(inline_form), GraphCompiler::canon(top_form));
}

TEST(RoundTrip, CanonIsIdempotent) {
    auto def  = full_feature_def();
    auto once = GraphCompiler::canon(def);
    EXPECT_EQ(once, GraphCompiler::canon(once));
}

TEST(RoundTrip, CanonStripsAnnotationsAndSortsInterrupts) {
    json def = {{"_comment", "note"},
                {"nodes", {{"a", {{"type", "snoop"}, {"x-pos", 1}}}}},
                {"interrupt_before", json::array({"b", "a", "b"})}};
    auto c   = GraphCompiler::canon(def);
    EXPECT_FALSE(c.contains("_comment"));
    EXPECT_FALSE(c["nodes"]["a"].contains("x-pos"));
    EXPECT_EQ(c["interrupt_before"], json::array({"a", "b"}));
}

// =========================================================================
// Translation validation catches the v0.1.x mutant class
// =========================================================================

TEST(TranslationValidation, DetectsConditionalEdgesDrop) {
    // Simulate reverting the v0.1.8 fix: compile correctly, then drop
    // the compiled conditional edges — exactly what v0.1.0–v0.1.7 did.
    ensure_types_registered();
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    cg.conditional_edges.clear();
    EXPECT_THROW(GraphCompiler::verify_roundtrip(def, cg), std::runtime_error);
}

TEST(TranslationValidation, DetectsRouteRewiring) {
    // An edge that survives but routes to the wrong target must fail —
    // presence-only comparison would miss this.
    ensure_types_registered();
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    ASSERT_FALSE(cg.conditional_edges.empty());
    for (auto& ce : cg.conditional_edges) {
        for (auto& [key, target] : ce.routes)
            target = "j";  // rewire all
    }
    EXPECT_THROW(GraphCompiler::verify_roundtrip(def, cg), std::runtime_error);
}

TEST(TranslationValidation, DetectsBarrierDrop) {
    ensure_types_registered();
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    cg.barrier_specs.clear();
    EXPECT_THROW(GraphCompiler::verify_roundtrip(def, cg), std::runtime_error);
}

TEST(TranslationValidation, DetectsInterruptDrop) {
    ensure_types_registered();
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    cg.interrupt_before.clear();
    EXPECT_THROW(GraphCompiler::verify_roundtrip(def, cg), std::runtime_error);
}

TEST(TranslationValidation, LenientDocumentWarnsButDoesNotThrow) {
    ensure_types_registered();
    auto def = without_key(full_feature_def(), "schema_version");  // legacy
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    cg.conditional_edges.clear();
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg));
}
