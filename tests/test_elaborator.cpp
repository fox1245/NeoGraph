// M4 elaboration-layer tests (issue #75): vars / templates / when /
// source map / determinism, plus the upgrade chain's IR-equivalence
// guarantee.
//
// The property under test for the DSL surface: every DSL document
// normalizes to a UNIQUE core document (total, deterministic), errors
// carry DSL source coordinates, and the expanded core sails through
// the full strict pipeline (compile + validate + TV).

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/elaborator.h>
#include <neograph/graph/validator.h>

#include <filesystem>
#include <fstream>
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

void expect_elab_error(const json& doc, std::initializer_list<const char*> fragments) {
    try {
        Elaborator::elaborate(doc);
        FAIL() << "expected elaboration error";
    } catch (const std::runtime_error& e) {
        const std::string msg = e.what();
        for (const char* f : fragments) {
            EXPECT_NE(msg.find(f), std::string::npos)
                << "missing '" << f << "' in: " << msg;
        }
    }
}

} // namespace

// =========================================================================
// vars
// =========================================================================

TEST(Elaborator, VarWholeValueAndInterpolation) {
    json doc = {
        {"vars", {{"target", "__end__"}, {"n", 3}, {"greet", "hello ${n} worlds"}}},
        {"nodes", {{"a", {{"type", "enoop"}, {"note", {{"$var", "greet"}}}}}}},
        {"edges", json::array({ {{"from", "__start__"}, {"to", "a"}},
                                {{"from", "a"}, {"to", "${target}"}} })},
    };
    auto r = Elaborator::elaborate(doc);
    EXPECT_EQ(r.core["nodes"]["a"]["note"].get<std::string>(), "hello 3 worlds");
    EXPECT_EQ(r.core["edges"][1]["to"].get<std::string>(), "__end__");
    EXPECT_FALSE(r.core.contains("vars"));
}

TEST(Elaborator, ExactInterpolationSubstitutesWholeValue) {
    json doc = {
        {"vars", {{"routes", json::array({"x", "y"})}}},
        {"nodes", {{"a", {{"type", "enoop"}, {"routes", "${routes}"}}}}},
    };
    auto r = Elaborator::elaborate(doc);
    EXPECT_TRUE(r.core["nodes"]["a"]["routes"].is_array());
    EXPECT_EQ(r.core["nodes"]["a"]["routes"].size(), 2u);
}

TEST(Elaborator, ChainedVarsResolve) {
    json doc = {
        {"vars", {{"a", "v"}, {"b", "${a}-suffix"}, {"c", {{"$var", "b"}}}}},
        {"nodes", {{"n", {{"type", "enoop"}, {"x", "${c}"}}}}},
    };
    auto r = Elaborator::elaborate(doc);
    EXPECT_EQ(r.core["nodes"]["n"]["x"].get<std::string>(), "v-suffix");
}

TEST(Elaborator, VarCycleIsErrorWithCoordinate) {
    json doc = {{"vars", {{"a", "${b}"}, {"b", "${a}"}}}, {"nodes", json::object()}};
    expect_elab_error(doc, {"vars.", "cyclic"});
}

TEST(Elaborator, UnknownVarIsErrorWithPath) {
    json doc = {{"nodes", {{"a", {{"type", "enoop"}, {"x", "${nope}"}}}}}};
    expect_elab_error(doc, {"$.nodes.a.x", "nope"});
}

TEST(Elaborator, AnnotationsAreNotSubstituted) {
    json doc = {{"_comment", "docs may say ${anything} freely"},
                {"nodes", {{"a", {{"type", "enoop"},
                                  {"_comment", "also ${here}"}}}}}};
    auto r = Elaborator::elaborate(doc);
    EXPECT_NE(r.core["_comment"].get<std::string>().find("${anything}"),
              std::string::npos);
}

// =========================================================================
// templates / use
// =========================================================================

namespace {

json expert_dsl() {
    return json{
        {"schema_version", 1},
        {"vars", {{"final", "__end__"}}},
        {"templates", {{"expert", {
            {"params", json::array({"target"})},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"worker", {{"type", "enoop"},
                                   {"tag", "@{target}"}}}}},
            {"edges", json::array({
                {{"from", "worker"}, {"to", {{"$param", "target"}}}} })},
        }}}},
        {"use", json::array({
            {{"template", "expert"}, {"prefix", "math"},
             {"args", {{"target", "__end__"}}}},
            {{"template", "expert"}, {"prefix", "code"},
             {"args", {{"target", {{"$var", "final"}}}}}},
        })},
        {"nodes", {{"router", {{"type", "enoop"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "router"}},
            {{"from", "router"}, {"to", "math_worker"}},
            {{"from", "router"}, {"to", "code_worker"}} })},
    };
}

} // namespace

TEST(Elaborator, TemplateInstantiationPrefixesAndSubstitutes) {
    ensure_etypes();
    auto r = Elaborator::elaborate(expert_dsl());
    const json& core = r.core;

    ASSERT_TRUE(core["nodes"].contains("math_worker"));
    ASSERT_TRUE(core["nodes"].contains("code_worker"));
    EXPECT_EQ(core["nodes"]["math_worker"]["tag"].get<std::string>(), "__end__");
    EXPECT_FALSE(core.contains("templates"));
    EXPECT_FALSE(core.contains("use"));
    // channels merged once, not duplicated or prefixed
    EXPECT_TRUE(core["channels"].contains("messages"));

    // Local refs prefixed; the $var arg resolved through the template.
    bool math_edge = false, code_edge = false;
    for (const auto& e : core["edges"]) {
        if (e.value("from", "") == "math_worker")
            math_edge = (e.value("to", "") == "__end__");
        if (e.value("from", "") == "code_worker")
            code_edge = (e.value("to", "") == "__end__");
    }
    EXPECT_TRUE(math_edge);
    EXPECT_TRUE(code_edge);
}

TEST(Elaborator, ExpandedCoreSurvivesFullStrictPipeline) {
    ensure_etypes();
    auto r = Elaborator::elaborate(expert_dsl());
    CompiledGraph cg;
    ASSERT_NO_THROW(cg = GraphCompiler::compile(r.core, NodeContext{}))
        << r.core.dump(2);
    EXPECT_FALSE(GraphValidator::validate(cg).has_errors());
    EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(r.core, cg));
}

TEST(Elaborator, SourceMapPointsAtInstantiations) {
    auto r = Elaborator::elaborate(expert_dsl());
    bool found = false;
    for (const auto& m : r.sourcemap) {
        if (m["target"].get<std::string>() == "/nodes/math_worker") {
            found = true;
            EXPECT_NE(m["source"].get<std::string>().find("use[0]"), std::string::npos);
            EXPECT_NE(m["source"].get<std::string>().find("'expert'"), std::string::npos);
        }
    }
    EXPECT_TRUE(found) << r.sourcemap.dump(2);
}

TEST(Elaborator, WhenFalseSkipsInstantiation) {
    auto doc = expert_dsl();
    json uses = json::array();
    uses.push_back({{"template", "expert"}, {"prefix", "math"},
                    {"args", {{"target", "__end__"}}}, {"when", false}});
    doc["use"] = uses;
    doc["edges"] = json::array({ {{"from", "__start__"}, {"to", "router"}} });
    auto r = Elaborator::elaborate(doc);
    EXPECT_FALSE(r.core["nodes"].contains("math_worker"));
    EXPECT_TRUE(r.core["nodes"].contains("router"));
}

TEST(Elaborator, ArgErrorsCarrySourceCoordinates) {
    auto doc = expert_dsl();
    json uses = json::array();
    uses.push_back({{"template", "expert"}, {"prefix", "m"}, {"args", json::object()}});
    doc["use"] = uses;
    expect_elab_error(doc, {"use[0].args", "missing arg 'target'"});

    uses = json::array();
    uses.push_back({{"template", "expert"}, {"prefix", "m"},
                    {"args", {{"target", "__end__"}, {"typo", 1}}}});
    doc["use"] = uses;
    expect_elab_error(doc, {"use[0].args", "unexpected arg 'typo'"});

    uses = json::array();
    uses.push_back({{"template", "ghost"}, {"prefix", "m"}, {"args", json::object()}});
    doc["use"] = uses;
    expect_elab_error(doc, {"use[0]", "unknown template 'ghost'"});
}

TEST(Elaborator, NodeCollisionIsError) {
    auto doc = expert_dsl();
    doc["nodes"]["math_worker"] = json{{"type", "enoop"}};
    expect_elab_error(doc, {"use[0]", "math_worker", "collides"});
}

TEST(Elaborator, LeftoverParamOutsideTemplateIsError) {
    json doc = {{"nodes", {{"a", {{"type", "enoop"},
                                  {"cfg", {{"$param", "oops"}}}}}}}};
    expect_elab_error(doc, {"$.nodes.a.cfg", "$param", "outside"});
}

// =========================================================================
// totality / determinism / idempotence
// =========================================================================

TEST(Elaborator, DeterministicAndIdempotent) {
    auto r1 = Elaborator::elaborate(expert_dsl());
    auto r2 = Elaborator::elaborate(expert_dsl());
    EXPECT_EQ(r1.core, r2.core);

    // Elaboration is the identity on core documents.
    auto r3 = Elaborator::elaborate(r1.core);
    EXPECT_EQ(GraphCompiler::canon(r3.core), GraphCompiler::canon(r1.core));
    EXPECT_TRUE(r3.sourcemap.empty());
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

    // Already-current documents pass through unchanged.
    EXPECT_EQ(GraphCompiler::upgrade_to_latest(up), up);
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
