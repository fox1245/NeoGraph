// Schema export + topology round-trip guards (issue #56).
//
// NodeFactory::export_schema() is the contract a separate visual
// topology editor consumes so its block palette cannot drift from the
// engine. These tests pin (a) the exported document's shape, (b) the
// built-in node/reducer/condition catalog, and (c) the one historical
// trap the editor must never silently reintroduce: the top-level
// `conditional_edges` block was dropped by the compiler in
// v0.1.0–v0.1.7 (fixed v0.1.8). A visual branch that doesn't survive
// loader→compile is a broken graph, so we guard it here.

#include <gtest/gtest.h>
#include <neograph/graph/loader.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/node.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

static bool array_has(const json& arr, const std::string& v) {
    for (const auto& e : arr) {
        if (e.is_string() && e.get<std::string>() == v) return true;
    }
    return false;
}

// ── exported document shape ──

TEST(SchemaExport, DocumentShape) {
    auto doc = NodeFactory::instance().export_schema();

    ASSERT_TRUE(doc.contains("neograph_version"));
    EXPECT_TRUE(doc["neograph_version"].is_string());
    EXPECT_FALSE(doc["neograph_version"].get<std::string>().empty());

    EXPECT_EQ(doc.value("$schema", ""),
              "https://json-schema.org/draft/2020-12/schema");

    ASSERT_TRUE(doc.contains("topology"));
    const auto& topo = doc["topology"];
    EXPECT_EQ(topo.value("type", ""), "object");
    ASSERT_TRUE(topo.contains("required"));
    EXPECT_TRUE(array_has(topo["required"], "nodes"));
    // Envelope keys the loader actually reads must be advertised.
    const auto& props = topo["properties"];
    for (const char* k : {"name", "channels", "nodes", "edges",
                          "conditional_edges", "interrupt_before",
                          "interrupt_after", "retry_policy"}) {
        EXPECT_TRUE(props.contains(k)) << "missing topology key: " << k;
    }

    ASSERT_TRUE(doc.contains("node_types"));
    for (const char* t : {"llm_call", "tool_dispatch",
                          "intent_classifier", "subgraph"}) {
        EXPECT_TRUE(doc["node_types"].contains(t)) << "missing node type: " << t;
    }

    EXPECT_TRUE(array_has(doc["reducers"], "overwrite"));
    EXPECT_TRUE(array_has(doc["reducers"], "append"));
    EXPECT_TRUE(array_has(doc["conditions"], "has_tool_calls"));
    EXPECT_TRUE(array_has(doc["conditions"], "route_channel"));
}

// The CMake compile-definition must actually reach this TU; if the
// wiring regresses, the version degrades to the "unknown" fallback and
// the editor's drift detection goes blind.
TEST(SchemaExport, VersionIsStampedNotFallback) {
    auto doc = NodeFactory::instance().export_schema();
    EXPECT_NE(doc["neograph_version"].get<std::string>(), "unknown");
}

// ── built-in node config schemas ──

TEST(SchemaExport, BuiltinNodeConfigSchemas) {
    auto nt = NodeFactory::instance().export_schema()["node_types"];

    EXPECT_EQ(nt["llm_call"].value("type", ""), "object");
    EXPECT_EQ(nt["tool_dispatch"].value("type", ""), "object");

    ASSERT_TRUE(nt["intent_classifier"].contains("required"));
    EXPECT_TRUE(array_has(nt["intent_classifier"]["required"], "routes"));
    EXPECT_TRUE(nt["intent_classifier"]["properties"].contains("prompt"));

    ASSERT_TRUE(nt["subgraph"].contains("required"));
    EXPECT_TRUE(array_has(nt["subgraph"]["required"], "definition"));
}

// ── introspection accessors ──

TEST(SchemaExport, RegistryNameAccessorsSortedAndComplete) {
    auto rn = ReducerRegistry::instance().names();
    EXPECT_TRUE(std::is_sorted(rn.begin(), rn.end()));
    EXPECT_NE(std::find(rn.begin(), rn.end(), "overwrite"), rn.end());
    EXPECT_NE(std::find(rn.begin(), rn.end(), "append"), rn.end());

    auto cn = ConditionRegistry::instance().names();
    EXPECT_TRUE(std::is_sorted(cn.begin(), cn.end()));
    EXPECT_NE(std::find(cn.begin(), cn.end(), "has_tool_calls"), cn.end());

    auto types = NodeFactory::instance().registered_types();
    EXPECT_TRUE(std::is_sorted(types.begin(), types.end()));
    EXPECT_NE(std::find(types.begin(), types.end(), "llm_call"), types.end());
}

// ── 3-arg register_type carries a declared schema; 2-arg is permissive ──
// Unique type names: the registries are process-wide singletons that
// persist across test cases (documented in loader.h).

TEST(SchemaExport, ThreeArgRegisterTypeCarriesSchema) {
    NodeFactory::instance().register_type(
        "schema_export_test_typed",
        [](const std::string& n, const json&, const NodeContext& ctx) {
            return std::unique_ptr<GraphNode>(new LLMCallNode(n, ctx));
        },
        json::parse(R"({"type":"object","properties":{"k":{"type":"integer"}},"required":["k"]})"));

    auto nt = NodeFactory::instance().export_schema()["node_types"];
    ASSERT_TRUE(nt.contains("schema_export_test_typed"));
    EXPECT_TRUE(array_has(nt["schema_export_test_typed"]["required"], "k"));
    EXPECT_EQ(nt["schema_export_test_typed"]["properties"]["k"].value("type", ""),
              "integer");
}

TEST(SchemaExport, TwoArgRegisterTypeGetsPermissiveDefault) {
    NodeFactory::instance().register_type(
        "schema_export_test_bare",
        [](const std::string& n, const json&, const NodeContext& ctx) {
            return std::unique_ptr<GraphNode>(new LLMCallNode(n, ctx));
        });

    auto nt = NodeFactory::instance().export_schema()["node_types"];
    ASSERT_TRUE(nt.contains("schema_export_test_bare"));
    EXPECT_EQ(nt["schema_export_test_bare"].value("type", ""), "object");
    // Permissive default does not pin any required fields.
    EXPECT_FALSE(nt["schema_export_test_bare"].contains("required"));
}

// ── REGRESSION GUARD: top-level conditional_edges must survive ──

TEST(SchemaExport, TopLevelConditionalEdgesRoundTrip) {
    json def = json::parse(R"({
        "nodes": {
            "agent": { "type": "llm_call" },
            "tools": { "type": "tool_dispatch" }
        },
        "edges": [ { "from": "__start__", "to": "agent" } ],
        "conditional_edges": [
            { "from": "agent", "condition": "has_tool_calls",
              "routes": { "true": "tools", "false": "__end__" } }
        ]
    })");

    auto cg = GraphCompiler::compile(def, NodeContext{});

    ASSERT_EQ(cg.conditional_edges.size(), 1u)
        << "top-level conditional_edges silently dropped (v0.1.0-v0.1.7 regression)";
    const auto& ce = cg.conditional_edges[0];
    EXPECT_EQ(ce.from, "agent");
    EXPECT_EQ(ce.condition, "has_tool_calls");
    EXPECT_EQ(ce.routes.at("true"), "tools");
    EXPECT_EQ(ce.routes.at("false"), "__end__");
}

TEST(SchemaExport, InlineConditionalEdgeFormStillRoundTrips) {
    json def = json::parse(R"({
        "nodes": {
            "agent": { "type": "llm_call" },
            "tools": { "type": "tool_dispatch" }
        },
        "edges": [
            { "from": "__start__", "to": "agent" },
            { "from": "agent", "condition": "has_tool_calls",
              "routes": { "true": "tools" } }
        ]
    })");

    auto cg = GraphCompiler::compile(def, NodeContext{});

    ASSERT_EQ(cg.conditional_edges.size(), 1u);
    EXPECT_EQ(cg.conditional_edges[0].from, "agent");
    EXPECT_EQ(cg.conditional_edges[0].routes.at("true"), "tools");
}
