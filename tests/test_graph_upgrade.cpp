// Strict Core compatibility tests: legacy topology documents upgrade to v1
// without changing the compiled graph IR.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/scheduler.h>
#include <neograph/graph/state.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

class ENoopNode : public GraphNode {
public:
    explicit ENoopNode(std::string n) : name_(std::move(n)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

void ensure_etypes() {
    NodeFactory::instance().register_type(
        "enoop",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ENoopNode>(name);
        });
}

// =========================================================================
// upgrade chain: legacy corpus -> v1, IR-equivalent
// =========================================================================

namespace {

json strip_version(const json& j) {
    json out = json::object();
    for (const auto& [k, v] : j.items()) {
        if (k != "schema_version") out[k] = v;
    }
    return out;
}

} // namespace

TEST(Upgrade, LegacyDocumentUpgradesToEquivalentIR) {
    ensure_etypes();
    // Legacy document exercising every quarantine rule: unknown
    // top-level key, unknown retry key, empty barrier, inline
    // conditional with dead 'to', unknown channel key.
    json legacy = {
        {"name", "legacy"},
        {"conditionnal_edges", json::array()},               // the typo
        {"channels", {{"c", {{"reducer", "append"}, {"initail", 1}}}}},
        {"nodes", {{"a", {{"type", "enoop"},
                          {"barrier", {{"wait_for", json::array()}}}}},
                   {"b", {{"type", "enoop"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "a"}},
            {{"from", "a"}, {"to", "b"}, {"weight", 3}},
            {{"from", "b"}, {"to", "__end__"},
             {"condition", "route_channel"},
             {"routes", {{"default", "__end__"}}}} })},
        {"retry_policy", {{"max_retries", 2}, {"max_retry", 9}}},
    };

    // Legacy compiles leniently.
    auto cg_legacy = GraphCompiler::compile(legacy, NodeContext{});

    // Upgraded compiles STRICTLY (throws on any leftover refusal).
    json up = GraphCompiler::upgrade_to_latest(legacy);
    EXPECT_EQ(up["schema_version"].get<int>(), 1);
    CompiledGraph cg_up;
    ASSERT_NO_THROW(cg_up = GraphCompiler::compile(up, NodeContext{}))
        << up.dump(2);

    // Same IR modulo the version stamp (data quarantined into x- keys
    // is annotation-invisible to canon on both sides).
    EXPECT_EQ(GraphCompiler::canon(strip_version(cg_legacy.to_json())),
              GraphCompiler::canon(strip_version(cg_up.to_json())))
        << "legacy IR: " << cg_legacy.to_json().dump(2)
        << "\nupgraded IR: " << cg_up.to_json().dump(2);

    // Quarantine preserved the data.
    EXPECT_TRUE(up.contains("x-upgraded-conditionnal_edges"));
    EXPECT_TRUE(up["retry_policy"].contains("x-upgraded-max_retry"));
    EXPECT_TRUE(up["channels"]["c"].contains("x-upgraded-initail"));
    EXPECT_FALSE(up["nodes"]["a"].contains("barrier"));
    EXPECT_EQ(up["nodes"]["a"]["x-upgraded-barrier"],
              legacy["nodes"]["a"]["barrier"]);

    // Already-current documents pass through unchanged.
    EXPECT_EQ(GraphCompiler::upgrade_to_latest(up), up);
}

TEST(Upgrade, QuarantinePreservesExistingAnnotationsOnNameCollision) {
    ensure_etypes();
    json legacy = {
        {"conditionnal_edges", json::array()},
        {"x-upgraded-conditionnal_edges", "existing-top-level-annotation"},
        {"channels", {{"c", {{"reducer", "append"},
                               {"initail", 1},
                               {"x-upgraded-initail", "existing-channel-annotation"}}}}},
        {"nodes", {{"a", {{"type", "enoop"},
                            {"barrier", {{"wait_for", json::array()}}},
                            {"x-upgraded-barrier", "existing-node-annotation"}}}}},
    };

    const json up = GraphCompiler::upgrade_to_latest(legacy);

    EXPECT_EQ(up["x-upgraded-conditionnal_edges"],
              "existing-top-level-annotation");
    EXPECT_EQ(up["x-upgraded-conditionnal_edges-2"], json::array());
    EXPECT_EQ(up["channels"]["c"]["x-upgraded-initail"],
              "existing-channel-annotation");
    EXPECT_EQ(up["channels"]["c"]["x-upgraded-initail-2"], 1);
    EXPECT_EQ(up["nodes"]["a"]["x-upgraded-barrier"],
              "existing-node-annotation");
    EXPECT_EQ(up["nodes"]["a"]["x-upgraded-barrier-2"],
              legacy["nodes"]["a"]["barrier"]);
    EXPECT_NO_THROW(GraphCompiler::compile(up, NodeContext{}));
}

TEST(Upgrade, ExplicitDefaultRouteBecomesStrictFallback) {
    ensure_etypes();
    json legacy = {
        {"nodes", {{"a", {{"type", "enoop"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "a"}}})},
        {"conditional_edges", json::array({
            {{"from", "a"}, {"condition", "route_channel"},
             {"routes", {{"default", "__end__"}, {"fast", "a"}}}}
        })},
    };

    auto upgraded = GraphCompiler::upgrade_to_latest(legacy);
    auto cg = GraphCompiler::compile(upgraded, NodeContext{});
    Scheduler scheduler(cg.edges, cg.conditional_edges);
    GraphState state;
    state.init_channel("__route__", ReducerType::OVERWRITE, nullptr,
                       json("unknown"));
    EXPECT_EQ(scheduler.resolve_next_nodes("a", state),
              std::vector<std::string>{"__end__"});
}

TEST(Upgrade, DoesNotPromoteAccidentalLegacyLastRouteToDefault) {
    ensure_etypes();
    json legacy = {
        {"nodes", {{"a", {{"type", "enoop"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "a"}}})},
        {"conditional_edges", json::array({
            {{"from", "a"}, {"condition", "route_channel"},
             {"routes", {{"fast", "a"}, {"zzz_fallback", "__end__"}}}}
        })},
    };

    auto upgraded = GraphCompiler::upgrade_to_latest(legacy);
    EXPECT_FALSE(upgraded["conditional_edges"][0]["routes"].contains("default"));
    auto cg = GraphCompiler::compile(upgraded, NodeContext{});
    Scheduler scheduler(cg.edges, cg.conditional_edges);
    GraphState state;
    state.init_channel("__route__", ReducerType::OVERWRITE, nullptr,
                       json("unknown"));
    EXPECT_THROW(scheduler.resolve_next_nodes("a", state), std::runtime_error);
}

TEST(Upgrade, CorpusUpgradesLosslessly) {
    // Every corpus fixture stripped to legacy form, upgraded, and both
    // compiled — IR must match (fixture set from M3, all built-ins).
    ensure_etypes();
    std::filesystem::path here(__FILE__);
    const auto dir = here.parent_path() / "fixtures" / "topology_corpus";
    size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const auto fname = entry.path().filename().string();
        if (fname == "manifest.json") continue;
        std::ifstream f(entry.path());
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        const json strict_doc = json::parse(content);
        const json legacy = strip_version(strict_doc);

        auto cg_legacy = GraphCompiler::compile(legacy, NodeContext{});
        const json up = GraphCompiler::upgrade_to_latest(legacy);
        CompiledGraph cg_up;
        ASSERT_NO_THROW(cg_up = GraphCompiler::compile(up, NodeContext{})) << fname;
        EXPECT_EQ(GraphCompiler::canon(strip_version(cg_legacy.to_json())),
                  GraphCompiler::canon(strip_version(cg_up.to_json()))) << fname;
        ++checked;
    }
    EXPECT_GE(checked, 15u);
}
} // namespace


