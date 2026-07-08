// M1 "regression seal" tests (issue #75): consumed-key accounting
// (strict mode via schema_version), CompiledGraph::to_json() re-emission,
// canon() normalization, and translation validation (verify_roundtrip).
//
// The contract under test:
//   canon(definition) == canon(compile(definition).to_json())
// for every accepted document — a mismatch is a silent drop/rewire, the
// v0.1.0–v0.1.7 conditional_edges regression class. Strict documents
// must refuse unknown/unconsumed keys instead of ignoring them.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>

using namespace neograph;
using namespace neograph::graph;

namespace {

class StrictNoopNode : public GraphNode {
public:
    explicit StrictNoopNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void ensure_types_registered() {
    // "snoop": registered WITHOUT a declared schema — permissive, any
    // config accepted even in strict mode (models the cookbook's
    // custom nodes with free-form config).
    NodeFactory::instance().register_type(
        "snoop",
        [](const std::string& name, const json&, const NodeContext&) {
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
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "a"}, {"to", "__end__"}} })},
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

} // namespace

// =========================================================================
// Strict mode: consumed-key accounting
// =========================================================================

TEST(StrictCompiler, TypoConditionalEdgesIsError) {
    auto def = minimal_strict();
    def["conditionnal_edges"] = json::array();   // the historical nightmare
    expect_strict_error(def, {"conditionnal_edges", "unknown or unconsumed"});
}

TEST(StrictCompiler, UnknownChannelKeyIsError) {
    auto def = minimal_strict();
    def["channels"] = {{"c", {{"reducer", "append"}, {"initail", 5}}}};
    expect_strict_error(def, {"channels.c", "initail"});
}

TEST(StrictCompiler, DeclaredSchemaNodeConfigTypoIsError) {
    auto def = minimal_strict();
    def["nodes"]["b"] = {{"type", "snoop_closed"}, {"knbo", "x"}};
    expect_strict_error(def, {"nodes.b", "knbo"});
}

TEST(StrictCompiler, BuiltinIntentClassifierConfigTypoIsError) {
    auto def = minimal_strict();
    def["nodes"]["ic"] = {{"type", "intent_classifier"},
                          {"routes", json::array({"x"})},
                          {"promt", "classify"}};   // typo of "prompt"
    expect_strict_error(def, {"nodes.ic", "promt"});
}

TEST(StrictCompiler, PermissiveCustomTypeConfigAllowed) {
    ensure_types_registered();
    auto def = minimal_strict();
    def["nodes"]["a"] = {{"type", "snoop"},
                         {"model_path", "assets/foo.bin"},
                         {"vad_threshold", 0.5}};
    EXPECT_NO_THROW(GraphCompiler::compile(def, NodeContext{}));
}

TEST(StrictCompiler, RetryPolicyTypoIsError) {
    auto def = minimal_strict();
    def["retry_policy"] = {{"max_retry", 3}};   // typo of "max_retries"
    expect_strict_error(def, {"retry_policy", "max_retry"});
}

TEST(StrictCompiler, EmptyBarrierWaitForIsError) {
    auto def = minimal_strict();
    def["nodes"]["a"]["barrier"] = {{"wait_for", json::array()}};
    expect_strict_error(def, {"nodes.a.barrier", "silently dropped"});
}

TEST(StrictCompiler, BarrierMissingWaitForIsError) {
    auto def = minimal_strict();
    def["nodes"]["a"]["barrier"] = json::object();
    expect_strict_error(def, {"nodes.a.barrier", "silently dropped"});
}

TEST(StrictCompiler, UnknownBarrierKeyIsError) {
    auto def = minimal_strict();
    def["nodes"]["a"]["barrier"] = {{"wait_for", json::array({"b"})},
                                    {"wiat_for", json::array({"c"})}};
    expect_strict_error(def, {"nodes.a.barrier", "wiat_for"});
}

TEST(StrictCompiler, InlineConditionalToIsError) {
    // 'to' on an inline conditional edge is silently ignored by the
    // parser (routing goes through 'routes') — strict mode surfaces it.
    auto def = minimal_strict();
    def["edges"].push_back({{"from", "a"}, {"to", "__end__"},
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
    auto def = minimal_strict();
    def["_comment"] = "top-level note";
    def["x-studio"] = {{"zoom", 1.5}};
    def["channels"] = {{"c", {{"reducer", "append"}, {"_comment", "log"}}}};
    def["nodes"]["a"]["_comment"] = "node note";
    def["nodes"]["a"]["x-pos"] = {{"x", 10}, {"y", 20}};
    EXPECT_NO_THROW(GraphCompiler::compile(def, NodeContext{}));
}

TEST(StrictCompiler, AggregatesAllErrors) {
    auto def = minimal_strict();
    def["conditionnal_edges"] = json::array();
    def["retry_policy"] = {{"max_retry", 3}};
    expect_strict_error(def, {"2 error(s)", "conditionnal_edges", "max_retry"});
}

TEST(StrictCompiler, SchemaVersionTooNewIsError) {
    auto def = minimal_strict();
    def["schema_version"] = 2;
    expect_strict_error(def, {"newer than this engine"});
}

TEST(StrictCompiler, SchemaVersionMustBeInteger) {
    auto def = minimal_strict();
    def["schema_version"] = "1";
    expect_strict_error(def, {"schema_version", "integer"});
}

// =========================================================================
// Lenient mode: historical behavior preserved byte-for-byte
// =========================================================================

TEST(LenientCompiler, UnknownTopLevelKeyStillIgnored) {
    ensure_types_registered();
    json def = {{"nodes", {{"a", {{"type", "snoop"}}}}},
                {"conditionnal_edges", json::array()}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_TRUE(cg.conditional_edges.empty());
    EXPECT_EQ(cg.schema_version, 0);
}

TEST(LenientCompiler, EmptyBarrierStillDropped) {
    ensure_types_registered();
    json def = {{"nodes", {{"a", {{"type", "snoop"},
                                  {"barrier", {{"wait_for", json::array()}}}}}}}};
    auto cg = GraphCompiler::compile(def, NodeContext{});
    EXPECT_TRUE(cg.barrier_specs.empty());
}

// =========================================================================
// canon() + to_json(): translation-validation round-trip
// =========================================================================

namespace {

json full_feature_def() {
    return json{
        {"schema_version", 1},
        {"name", "full"},
        {"channels", {
            {"messages", {{"reducer", "append"}}},
            {"counter",  {{"reducer", "overwrite"}, {"initial", 42}}},
            {"maybe",    {{"reducer", "overwrite"}, {"initial", nullptr}}},
        }},
        {"nodes", {
            {"a", {{"type", "snoop"}, {"free_form", {{"nested", true}}}}},
            {"b", {{"type", "snoop_closed"}, {"knob", "v"}}},
            {"j", {{"type", "snoop"},
                   {"barrier", {{"wait_for", json::array({"b", "a"})}}}}},
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "__start__"}, {"to", "b"}},
            {{"from", "a"}, {"to", "j"}},
            {{"from", "b"}, {"to", "j"}},
            // legacy inline conditional form:
            {{"from", "j"}, {"condition", "has_tool_calls"},
             {"routes", {{"true", "a"}, {"false", "__end__"}}}},
        })},
        {"conditional_edges", json::array({
            {{"from", "a"}, {"condition", "route_channel"},
             {"routes", {{"x", "b"}, {"default", "__end__"}}}},
        })},
        {"interrupt_before", json::array({"j"})},
        {"interrupt_after", json::array({"a", "b"})},
        {"retry_policy", {{"max_retries", 3}, {"backoff_multiplier", 1.5}}},
    };
}

} // namespace

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
    auto def = full_feature_def();
    auto cg  = GraphCompiler::compile(def, NodeContext{});
    auto reemitted = cg.to_json();
    ASSERT_TRUE(reemitted["channels"]["maybe"].contains("initial"));
    EXPECT_TRUE(reemitted["channels"]["maybe"]["initial"].is_null());
    EXPECT_FALSE(reemitted["channels"]["messages"].contains("initial"));
}

TEST(RoundTrip, InlineAndTopLevelConditionalCanonToSameForm) {
    json inline_form = {{"nodes", {{"a", {{"type", "snoop"}}}}},
                        {"edges", json::array({
                            {{"from", "a"}, {"condition", "route_channel"},
                             {"routes", {{"x", "__end__"}}}} })}};
    json top_form    = {{"nodes", {{"a", {{"type", "snoop"}}}}},
                        {"conditional_edges", json::array({
                            {{"from", "a"}, {"condition", "route_channel"},
                             {"routes", {{"x", "__end__"}}}} })}};
    EXPECT_EQ(GraphCompiler::canon(inline_form), GraphCompiler::canon(top_form));
}

TEST(RoundTrip, CanonIsIdempotent) {
    auto def = full_feature_def();
    auto once = GraphCompiler::canon(def);
    EXPECT_EQ(once, GraphCompiler::canon(once));
}

TEST(RoundTrip, CanonStripsAnnotationsAndSortsInterrupts) {
    json def = {{"_comment", "note"},
                {"nodes", {{"a", {{"type", "snoop"}, {"x-pos", 1}}}}},
                {"interrupt_before", json::array({"b", "a", "b"})}};
    auto c = GraphCompiler::canon(def);
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
        for (auto& [key, target] : ce.routes) target = "j";   // rewire all
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
    auto def = without_key(full_feature_def(), "schema_version");   // legacy
    auto cg = GraphCompiler::compile(def, NodeContext{});
    cg.conditional_edges.clear();
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg));
}
