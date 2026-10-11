// X4 capability manifest base (Program API v2, WP2; #365).
//
// The JavaScript authoring manifest publishes one flag per feature, a JSON
// Schema per graph-builder argument and a .d.ts signature per method. Two
// schema files back it: the manifest's own schema and the strict Core
// definition schema for constrained decoding. The manifest must validate
// against its own schema, and real programs must validate against both the
// Core schema and the builder argument schemas.

#include "json_schema_lite.h"
#include "program_v2_test_support.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef NEOGRAPH_SOURCE_DIR
#error "NEOGRAPH_SOURCE_DIR must name the repository root"
#endif

namespace {

using namespace program_v2_test;
namespace fs = std::filesystem;

json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.generic_string());
    std::stringstream text;
    text << input.rdbuf();
    return json::parse(text.str());
}

const fs::path kRoot{NEOGRAPH_SOURCE_DIR};

json schema_file(const char* name) { return read_json(kRoot / "schemas" / name); }

std::string errors_of(const std::vector<std::string>& errors) {
    std::string result;
    for (const auto& error : errors) result += "\n  " + error;
    return result;
}

const json& builder_method(const json& manifest, std::string_view name) {
    for (const auto& method : manifest.at("define").at("graph_builder_methods"))
        if (method.at("name").get<std::string>() == name) return method;
    throw std::runtime_error("no builder method " + std::string(name));
}

json argument_schema(const json& manifest, std::string_view method, std::size_t index) {
    return builder_method(manifest, method).at("arguments").at(index).at("schema");
}

// The sealed definitions of the golden identity corpus: real programs from the
// DSL capability samples and the Program API v2 work packages.
std::vector<std::pair<std::string, json>> corpus_definitions() {
    const auto golden = read_json(kRoot / "tests" / "fixtures" / "identity_corpus" / "golden.json");
    std::vector<std::pair<std::string, json>> result;
    for (const auto& entry : golden.at("entries"))
        result.emplace_back(entry.at("id").get<std::string>(), entry.at("definition"));
    return result;
}

}  // namespace

TEST(CapabilityManifest, ValidatesAgainstItsOwnSchema) {
    const auto              manifest = javascript_authoring_capability_manifest();
    json_schema_lite::Checker checker(schema_file("javascript-capability-manifest-v1.schema.json"));
    const auto              errors = checker.check(manifest);
    EXPECT_TRUE(errors.empty()) << errors_of(errors);
    EXPECT_EQ(manifest.at("schemas").at("core_definition").get<std::string>(),
              schema_file("core-definition-v1.schema.json").at("$id").get<std::string>());
    EXPECT_EQ(manifest.at("schemas").at("manifest").get<std::string>(),
              schema_file("javascript-capability-manifest-v1.schema.json").at("$id").get<std::string>());
}

TEST(CapabilityManifest, PublishesOneFlagPerFeature) {
    const auto  manifest = javascript_authoring_capability_manifest();
    const auto& features = manifest.at("features");
    for (const auto* enabled : {"graph.core", "graph.channels", "graph.conditional_edges",
                                "graph.barrier", "graph.interrupts", "graph.retry_policy",
                                "diagnostics.help"})
        EXPECT_TRUE(features.at(enabled).get<bool>()) << enabled;
    // Flags for work packages that have not landed yet. Each package flips its
    // own flag together with the feature.
    EXPECT_FALSE(features.at("retry.attempt_timeout").get<bool>());

    // Every builder method belongs to a published feature. A method of a
    // feature that is off compiles and validates but does not run yet (the
    // #364 sugar before its runtime).
    for (const auto& method : manifest.at("define").at("graph_builder_methods")) {
        const auto feature = method.at("feature").get<std::string>();
        ASSERT_TRUE(features.contains(feature)) << method.at("name").dump();
        if (!features.at(feature).get<bool>())
            EXPECT_EQ(method.at("name").get<std::string>(), "failureExit");
    }
}

TEST(CapabilityManifest, EveryMethodHasADeclarationAndArgumentSchemas) {
    const auto manifest = javascript_authoring_capability_manifest();
    const auto prelude  = manifest.at("dts_prelude").get<std::string>();
    EXPECT_NE(prelude.find("interface GraphBuilder"), std::string::npos) << prelude;
    for (const auto* group : {"ng_methods", "graph_builder_methods"}) {
        for (const auto& method : manifest.at("define").at(group)) {
            const auto name = method.at("name").get<std::string>();
            const auto dts  = method.at("dts").get<std::string>();
            EXPECT_EQ(dts.rfind(name + "(", 0), 0U) << dts;
            EXPECT_NE(dts.find("): "), std::string::npos) << dts;
            const auto& arguments = method.at("arguments");
            EXPECT_LE(arguments.size(), std::max<std::uint64_t>(1, method.at("maximum_arity").get<std::uint64_t>()))
                << name;
            for (const auto& argument : arguments) {
                // A schema is usable when the checker accepts some value with it.
                json_schema_lite::Checker checker(argument.at("schema"));
                EXPECT_NO_THROW((void)checker.check(json::object())) << name;
            }
        }
    }
    for (const auto& method : manifest.at("main").at("command_methods")) {
        const auto name = method.at("name").get<std::string>();
        EXPECT_EQ(method.at("dts").get<std::string>().rfind(name + "(", 0), 0U) << name;
    }
}

TEST(CapabilityManifest, ArgumentSchemasAcceptWhatTheBuilderAccepts) {
    const auto manifest = javascript_authoring_capability_manifest();
    const auto accepts  = [&](std::string_view method, std::size_t index, const json& value) {
        json_schema_lite::Checker checker(argument_schema(manifest, method, index));
        return checker.check(value);
    };
    EXPECT_TRUE(accepts("node", 0, json("plan")).empty());
    EXPECT_FALSE(accepts("node", 0, json("__end__")).empty());
    EXPECT_TRUE(accepts("node", 1, json{{"type", "t.task"}, {"goal", "x"}}).empty());
    EXPECT_FALSE(accepts("node", 1, json{{"goal", "x"}}).empty()) << "type is required";
    EXPECT_TRUE(accepts("channel", 1, json{{"reducer", "append"}, {"initial", json::array()}}).empty());
    EXPECT_FALSE(accepts("channel", 1, json{{"reducer", "append"}, {"retain", 1}}).empty());
    EXPECT_TRUE(accepts("conditionalEdge", 2, json{{"pass", "B"}, {"fail", "__end__"}}).empty());
    EXPECT_FALSE(accepts("conditionalEdge", 2, json::object()).empty());
    EXPECT_TRUE(accepts("barrier", 1, json::array({"B", "C"})).empty());
    EXPECT_FALSE(accepts("barrier", 1, json::array()).empty());
    EXPECT_TRUE(accepts("retryPolicy", 0, json{{"max_retries", 2}, {"initial_delay_ms", 10}}).empty());
    EXPECT_FALSE(accepts("retryPolicy", 0, json{{"max_retries", -1}}).empty());
    EXPECT_FALSE(accepts("retryPolicy", 0, json{{"on_exhausted", "B"}}).empty());
    EXPECT_TRUE(accepts("interruptBefore", 0, json("A")).empty());
    EXPECT_TRUE(accepts("interruptBefore", 0, json::array({"A", "B"})).empty());
}

TEST(CoreDefinitionSchema, CorpusProgramsValidate) {
    json_schema_lite::Checker checker(schema_file("core-definition-v1.schema.json"));
    std::size_t               checked = 0;
    for (const auto& [id, definition] : corpus_definitions()) {
        const auto errors = checker.check(definition);
        EXPECT_TRUE(errors.empty()) << id << errors_of(errors);
        ++checked;
    }
    EXPECT_GE(checked, 20U);
}

TEST(CoreDefinitionSchema, RejectsWhatTheSealedValidatorRejects) {
    json_schema_lite::Checker checker(schema_file("core-definition-v1.schema.json"));
    const json                base{{"schema_version", 1},
                                   {"name", "main"},
                                   {"nodes", json{{"a", json{{"type", "t.task"}}}}},
                                   {"edges", json::array({json{{"from", "__start__"}, {"to", "a"}}})}};
    EXPECT_TRUE(checker.check(base).empty()) << errors_of(checker.check(base));
    auto unknown           = base;
    unknown["on_exhausted"] = "a";
    EXPECT_FALSE(checker.check(unknown).empty());
    auto reserved                  = base;
    reserved["nodes"]["__end__"]   = json{{"type", "t.task"}};
    EXPECT_FALSE(checker.check(reserved).empty());
    auto untyped                   = base;
    untyped["nodes"]["b"]          = json::object();
    EXPECT_FALSE(checker.check(untyped).empty());
    auto version                   = base;
    version["schema_version"]      = 0;
    EXPECT_FALSE(checker.check(version).empty());
    auto channel                   = base;
    channel["channels"]            = json{{"c", json{{"reducer", "append"}, {"persistence", "x"}}}};
    EXPECT_FALSE(checker.check(channel).empty()) << "sealed channels hold only reducer and initial";
    auto empty_wait                = base;
    empty_wait["nodes"]["a"]["barrier"] = json{{"wait_for", json::array()}};
    EXPECT_FALSE(checker.check(empty_wait).empty());
}

// The pieces of every corpus definition are what the builder arguments would
// have produced, so they validate against those argument schemas.
TEST(CoreDefinitionSchema, CorpusPiecesValidateAgainstTheBuilderArguments) {
    const auto manifest = javascript_authoring_capability_manifest();
    const auto check    = [&](std::string_view method, std::size_t index, const json& value,
                           const std::string& where) {
        json_schema_lite::Checker checker(argument_schema(manifest, method, index));
        const auto              errors = checker.check(value);
        EXPECT_TRUE(errors.empty()) << where << errors_of(errors);
    };
    for (const auto& [id, definition] : corpus_definitions()) {
        for (const auto& [name, node] : definition.at("nodes").items()) {
            check("node", 0, json(name), id + " node name " + name);
            json config = json::object();
            for (const auto& [key, value] : node.items())
                if (key != "barrier") config[key] = value;
            check("node", 1, config, id + " node " + name);
            if (node.contains("barrier"))
                check("barrier", 1, node.at("barrier").at("wait_for"), id + " barrier " + name);
        }
        if (definition.contains("channels"))
            for (const auto& [name, channel] : definition.at("channels").items())
                check("channel", 1, channel, id + " channel " + name);
        if (definition.contains("conditional_edges"))
            for (const auto& edge : definition.at("conditional_edges"))
                check("conditionalEdge", 2, edge.at("routes"), id + " routes");
        if (definition.contains("retry_policy"))
            check("retryPolicy", 0, definition.at("retry_policy"), id + " retry policy");
    }
}

TEST(CoreDefinitionSchema, AgreesWithTheStoredBundleSchema) {
    const auto core   = schema_file("core-definition-v1.schema.json");
    const auto bundle = schema_file("program-bundle-v1.schema.json");
    std::set<std::string> core_keys, bundle_keys;
    for (const auto& [key, unused] : core.at("properties").items()) core_keys.insert(key);
    for (const auto& [key, unused] : bundle.at("$defs").at("strictCoreDefinition").at("properties").items())
        bundle_keys.insert(key);
    EXPECT_EQ(core_keys, bundle_keys);
}
