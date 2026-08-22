#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "canonical_json.h"
#include "registry_access.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::atomic<unsigned> local_factory_calls{0};
std::atomic<unsigned> global_factory_calls{0};
std::atomic<unsigned> reducer_body_calls{0};
std::atomic<unsigned> condition_body_calls{0};

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

ExecutableManifest executable(ExecutableKind                  kind,
                              std::string                     name,
                              char                            implementation,
                              EffectMode                      mode         = EffectMode::Brokered,
                              std::vector<std::string>        capabilities = {},
                              std::vector<std::string>        effects      = {},
                              std::vector<ExecutableIdentity> dependencies = {}) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              mode,
                              "attestation:test",
                              std::move(capabilities),
                              std::move(effects),
                              std::move(dependencies)};
}

class CounterNode final : public GraphNode {
public:
    explicit CounterNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

NodeFactoryFn local_factory() {
    return [](const std::string& name, const json&, const NodeContext&) {
        ++local_factory_calls;
        return std::make_unique<CounterNode>(name);
    };
}

NodeFactoryFn quiet_factory() {
    return [](const std::string& name, const json&, const NodeContext&) {
        return std::make_unique<CounterNode>(name);
    };
}

NodeFactoryFn global_factory() {
    return [](const std::string& name, const json&, const NodeContext&) {
        ++global_factory_calls;
        return std::make_unique<CounterNode>(name);
    };
}

void reset_dispatch_counters() {
    local_factory_calls.store(0);
    global_factory_calls.store(0);
    reducer_body_calls.store(0);
    condition_body_calls.store(0);
}

void expect_dispatch_counters_zero() {
    EXPECT_EQ(local_factory_calls.load(), 0U);
    EXPECT_EQ(global_factory_calls.load(), 0U);
    EXPECT_EQ(reducer_body_calls.load(), 0U);
    EXPECT_EQ(condition_body_calls.load(), 0U);
}

RegistrySnapshot complete_snapshot(bool reverse_registration = false,
                                   char node_digest          = '1',
                                   char tool_digest          = '4',
                                   bool trusted_tool         = true) {
    const ExecutableIdentity tool{ExecutableKind::Tool, "local-tool", "1.0.0", digest(tool_digest)};
    const ExecutableIdentity provider{ExecutableKind::Provider, "local-provider", "1.0.0",
                                      digest('5')};
    const ExecutableIdentity imported{ExecutableKind::Imported, "local-import", "1.0.0",
                                      digest(tool_digest)};
    RegistrySnapshotBuilder  builder;
    const auto               add_node = [&] {
        builder.add_node(
            executable(ExecutableKind::Node, "local-node", node_digest, EffectMode::Brokered,
                                     {"node-capability"}, {"node-effect"}, {provider}),
            local_factory(), json{{"type", "object"}},
            json{{"reads", json::array({"value"})}, {"writes", json::array({"value"})}});
    };
    const auto add_reducer = [&] {
        builder.add_reducer(executable(ExecutableKind::Reducer, "local-reducer", '2'),
                            [](const json&, const json& incoming) {
                                ++reducer_body_calls;
                                return json(incoming);
                            });
    };
    const auto add_condition = [&] {
        builder.add_condition(
            executable(ExecutableKind::Condition, "local-condition", '3'),
            [](const GraphState&) {
                ++condition_body_calls;
                return std::string("done");
            },
            ConditionSpec{{"done"}, false});
    };
    const auto add_tool = [&] {
        builder.add_tool(executable(ExecutableKind::Tool, "local-tool", tool_digest,
                                    trusted_tool ? EffectMode::TrustedNative : EffectMode::Brokered,
                                    {"tool-capability"}, {"tool-effect"}),
                         ToolMetadata{json::object(), json::object()});
    };
    const auto add_import = [&] {
        builder.add_imported(
            executable(ExecutableKind::Imported, "local-import", tool_digest),
            ImportedExecutableMetadata{"module:test", "local-import", digest('e'), tool});
    };
    const auto add_provider = [&] {
        builder.add_provider(
            executable(ExecutableKind::Provider, "local-provider", '5', EffectMode::Brokered,
                       {"provider-capability"}, {"provider-effect"}, {imported}),
            ProviderMetadata{json::object(), json::object()});
    };
    if (reverse_registration) {
        add_condition();
        add_reducer();
        add_node();
        add_provider();
        add_import();
        add_tool();
    } else {
        add_tool();
        add_import();
        add_provider();
        add_node();
        add_reducer();
        add_condition();
    }
    return std::move(builder).build();
}

RegistrySnapshot node_only_snapshot(std::string name   = "local-node",
                                    json        schema = json{{"type", "object"}}) {
    RegistrySnapshotBuilder builder;
    builder.add_node(executable(ExecutableKind::Node, std::move(name), '7'), quiet_factory(),
                     std::move(schema), json::object());
    return std::move(builder).build();
}

ExecutableIdentity configured_provider() {
    return {ExecutableKind::Provider, "configured-provider", "1.0.0", digest('8')};
}

ExecutableIdentity configured_tool(std::string name, char implementation) {
    return {ExecutableKind::Tool, std::move(name), "1.0.0", digest(implementation)};
}

RegistrySnapshot config_requirement_snapshot() {
    const auto               provider = configured_provider();
    const auto               alpha    = configured_tool("configured-alpha", '9');
    const auto               beta     = configured_tool("configured-beta", 'a');
    const ExecutableIdentity imported{ExecutableKind::Imported, "configured-import", "1.0.0",
                                      alpha.implementation_digest};

    RegistrySnapshotBuilder builder;
    builder.add_provider(executable(ExecutableKind::Provider, provider.name, '8'),
                         ProviderMetadata{json::object(), json::object()});
    builder.add_tool(executable(ExecutableKind::Tool, alpha.name, '9'),
                     ToolMetadata{json::object(), json::object()});
    builder.add_tool(executable(ExecutableKind::Tool, beta.name, 'a'),
                     ToolMetadata{json::object(), json::object()});
    builder.add_imported(
        executable(ExecutableKind::Imported, imported.name, '9'),
        ImportedExecutableMetadata{"module:configured", imported.name, digest('b'), alpha});
    builder.add_node(executable(ExecutableKind::Node, "configured-node", '7'), local_factory(),
                     json{{"type", "object"}}, json::object(),
                     [provider, alpha, beta, imported](const json& config) {
                         std::vector<ExecutableIdentity> requirements;
                         if (config.value("provider_id", "") == provider.name) {
                             requirements.push_back(provider);
                         }
                         if (config.contains("tool_ids") && config["tool_ids"].is_array()) {
                             for (const auto& id : config["tool_ids"]) {
                                 if (id == alpha.name)
                                     requirements.push_back(alpha);
                                 else if (id == beta.name)
                                     requirements.push_back(beta);
                             }
                         }
                         if (config.value("import_id", "") == imported.name) {
                             requirements.push_back(imported);
                         }
                         return requirements;
                     });
    return std::move(builder).build();
}

json budgets() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 60000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000000}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 100}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

json core_definition() {
    return json{{"schema_version", 1},
                {"name", "main"},
                {"channels", json{{"value", json{{"reducer", "local-reducer"}, {"initial", 0}}}}},
                {"nodes", json{{"work", json{{"type", "local-node"}}}}},
                {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}}})},
                {"conditional_edges", json::array({json{{"from", "work"},
                                                        {"condition", "local-condition"},
                                                        {"routes", json{{"done", "__end__"}}}}})}};
}

json program_document(json definition = core_definition()) {
    return json{
        {"program_schema_version", 1},
        {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         json{{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", budgets()}};
}

json operation_document(json definition = core_definition()) {
    auto document = program_document(std::move(definition));
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
    document["declared_budget_requirements"][3]["minimum"] = 32;
    document["declared_budget_requirements"][3]["maximum"] = 32;
    document["declared_budget_requirements"][4]["minimum"] = 100;
    document["declared_budget_requirements"][4]["maximum"] = 100;
    return document;
}

json parallel_map_document() {
    auto document = operation_document();
    document["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V3;
    const auto definition = document["root"]["definition"];
    document["root"] = json{
        {"op", "parallel_map"},
        {"name", "main"},
        {"definition", definition},
        {"item_source",
         json{{"literal", json::array({json{{"request", "first"}}, json{{"request", "second"}},
                                       json{{"request", "third"}}})}}},
        {"child_binding", "child"},
        {"input_binding",
         json{{"from", json{{"field", ""}}}, {"to", json{{"field", ""}}}}},
        {"output_binding", json{{"from", json{{"field", ""}}}}},
        {"max_items", 3},
        {"max_in_flight", 2},
        {"max_output_bytes", 65536},
        {"failure_policy", "collect"}};
    auto requirements = document["declared_budget_requirements"];
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        auto requirement = requirements[index];
        const auto resource = requirement["resource"].get<std::string>();
        if (resource == "max_concurrency") {
            requirement["minimum"] = 2;
            requirement["maximum"] = 2;
        } else if (resource == "max_program_operations") {
            requirement["minimum"] = 1;
            requirement["maximum"] = 1;
        } else if (resource == "max_core_steps") {
            requirement["minimum"] = 1;
            requirement["maximum"] = 1;
        } else if (resource == "max_child_depth") {
            requirement["minimum"] = 1;
            requirement["maximum"] = 1;
        } else if (resource == "max_total_children") {
            requirement["minimum"] = 3;
            requirement["maximum"] = 3;
        }
        requirements[index] = std::move(requirement);
    }
    document["declared_budget_requirements"] = std::move(requirements);
    return document;
}

std::uint32_t source_schema_version(const json& document) {
    if (!document.contains("program_schema_version") ||
        !document["program_schema_version"].is_number_unsigned())
        return PROGRAM_SCHEMA_VERSION_V1;
    return document["program_schema_version"].get<std::uint32_t>();
}

ProgramSource source_from(json                        document,
                          std::vector<ImportRef>      imports    = {},
                          std::vector<SourceMapEntry> source_map = {},
                          std::string                 source_id  = "test:program") {
    const auto schema_version = source_schema_version(document);
    return ProgramSource::from_cpp_builder(std::move(source_id), schema_version,
                                           std::move(document), std::move(imports),
                                           std::move(source_map));
}

std::vector<neograph::program::Diagnostic> compile_errors(
    const RegistrySnapshot&     snapshot,
    json                        document,
    std::vector<ImportRef>      imports    = {},
    std::vector<SourceMapEntry> source_map = {}) {
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    try {
        (void)compiler.compile(
            source_from(std::move(document), std::move(imports), std::move(source_map)));
    } catch (const ProgramCompileError& error) {
        return error.diagnostics();
    }
    ADD_FAILURE() << "Expected ProgramCompileError";
    return {};
}

bool contains_code(const std::vector<neograph::program::Diagnostic>& diagnostics,
                   std::string_view                                  code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const auto& diagnostic) { return diagnostic.code == code; });
}

void ensure_global_poison_entries() {
    static const bool registered = [] {
        NodeFactory::instance().register_type("pr5-global-node", global_factory(),
                                              json{{"type", "object"}});
        ReducerRegistry::instance().register_reducer("pr5-global-reducer",
                                                     [](const json&, const json& incoming) {
                                                         ++reducer_body_calls;
                                                         return json(incoming);
                                                     });
        ConditionRegistry::instance().register_condition(
            "pr5-global-condition",
            [](const GraphState&) {
                ++condition_body_calls;
                return std::string("done");
            },
            ConditionSpec{{"done"}, false});
        return true;
    }();
    (void)registered;
}

json node_only_program(std::string type) {
    auto definition = json{{"schema_version", 1},
                           {"name", "main"},
                           {"nodes", json{{"work", json{{"type", std::move(type)}}}}},
                           {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                                  json{{"from", "work"}, {"to", "__end__"}}})},
                           {"conditional_edges", json::array()}};
    return program_document(std::move(definition));
}

}  // namespace

TEST(ProgramCompilerTest, CompilesOneRootAndRoundTripsBundle) {
    reset_dispatch_counters();
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      bundle = compiler.compile(source_from(program_document()));
    const auto      parsed = ProgramBundle::parse(bundle.serialize_canonical());

    EXPECT_EQ(parsed.id(), bundle.id());
    EXPECT_EQ(parsed.program_schema_version(), 1U);
    EXPECT_EQ(parsed.source_kind(), SourceKind::CppBuilder);
    EXPECT_EQ(parsed.registry_snapshot_fingerprint(), snapshot.fingerprint());
    ASSERT_EQ(parsed.sealed_core_definitions().size(), 1U);
    EXPECT_EQ(parsed.sealed_core_definitions()[0].name, "main");
    EXPECT_EQ(parsed.orchestration_plan().plan["operations"][0]["op"], "call_core");
    EXPECT_EQ(parsed.core_plan_identities().size(), 1U);
    EXPECT_EQ(parsed.source_map().size(), 3U);
    for (const auto pointer : {"/root", "/root/definition", "/operations/0"}) {
        EXPECT_NE(std::find_if(parsed.source_map().begin(), parsed.source_map().end(),
                               [pointer](const auto& entry) {
                                   return entry.generated_pointer == pointer &&
                                          entry.authored.json_pointer == pointer;
                               }),
                  parsed.source_map().end());
    }
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, SourceKindIsPreservedAndBoundIntoBundleIdentity) {
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      document = program_document();
    const auto builder = compiler.compile(ProgramSource::from_cpp_builder("builder", 1, document));

    EXPECT_EQ(builder.source_kind(), SourceKind::CppBuilder);
    EXPECT_EQ(ProgramBundle::parse(builder.serialize_canonical()).source_kind(),
              SourceKind::CppBuilder);
}

TEST(ProgramCompilerTest, StoredCanonicalJsonSourceIsNotACompilationFrontend) {
    auto snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    auto stored = json::parse(
        ProgramSource::from_cpp_builder("legacy", 1, program_document()).serialize_canonical());
    stored["kind"] = "canonical_json";

    const auto legacy = ProgramSource::parse(stored.dump());
    EXPECT_EQ(legacy.kind(), SourceKind::CanonicalJson);
    try {
        (void)compiler.compile(legacy);
        FAIL() << "legacy canonical Program JSON unexpectedly compiled";
    } catch (const ProgramCompileError& error) {
        ASSERT_EQ(error.diagnostics().size(), 1U);
        EXPECT_EQ(error.diagnostics().front().code, "P_MIGRATION_PROGRAM_JSON");
    }
}

#if defined(NEOGRAPH_PROGRAM_TESTS_HAVE_QUICKJS)
TEST(ProgramCompilerTest, JavaScriptDefineBuildsOneCoreDefinitionWithOrdinaryControlFlow) {
    const auto source = ProgramSource::from_javascript(
        "control.js",
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                const stages = [
                    ["draft", "local-node"],
                    ["review", "local-node"],
                ];
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                for (const [name, type] of stages) {
                    graph.node(name, {type});
                }
                graph.entry("draft");
                graph.edge("draft", "review");
                graph.conditionalEdge("review", "local-condition", {done: "__end__"});
                return graph;
            }
        )JS");

    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto      bundle = compiler.compile(source);

    EXPECT_EQ(bundle.source_kind(), SourceKind::JavaScript);
    ASSERT_EQ(bundle.sealed_core_definitions().size(), 1U);
    EXPECT_EQ(bundle.sealed_core_definitions().front().name, "main");
    EXPECT_EQ(bundle.orchestration_plan().plan["operations"][0]["op"], "call_core");
}

TEST(ProgramCompilerTest, JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls) {
    const auto source = ProgramSource::from_javascript(
        "all-graph-primitives.js",
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                graph.channel("audit", {reducer: "local-reducer", initial: []});
                graph.node("draft", {type: "local-node"});
                graph.node("review", {type: "local-node"});
                graph.node("terminal", {type: "local-node"});
                graph.entry("draft");
                graph.edge("draft", "review");
                graph.conditionalEdge("review", "local-condition", {done: "terminal"});
                graph.barrier("review", ["draft"]);
                graph.interruptBefore("draft");
                graph.interruptBefore("review");
                graph.interruptAfter("review");
                graph.interruptAfter("terminal");
                graph.retryPolicy({
                    max_retries: 2,
                    initial_delay_ms: 10,
                    backoff_multiplier: 2,
                    max_delay_ms: 100,
                });
                graph.exit("terminal");
                return graph;
            }
        )JS");

    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto bundle = compiler.compile(source);
    const auto definitions = bundle.sealed_core_definitions();
    ASSERT_EQ(definitions.size(), 1U);
    const auto definition = definitions.front().definition;

    ASSERT_TRUE(definition.contains("channels"));
    EXPECT_EQ(definition.at("channels").size(), 2U);
    ASSERT_TRUE(definition.contains("nodes"));
    EXPECT_EQ(definition.at("nodes").size(), 3U);
    EXPECT_EQ(definition.at("nodes").at("review").at("barrier").at("wait_for"),
              json::array({"draft"}));
    EXPECT_EQ(definition.at("interrupt_before"), json::array({"draft", "review"}));
    EXPECT_EQ(definition.at("interrupt_after"), json::array({"review", "terminal"}));
    EXPECT_EQ(definition.at("retry_policy"),
              (json{{"max_retries", 2},
                    {"initial_delay_ms", 10},
                    {"backoff_multiplier", 2.0},
                    {"max_delay_ms", 100}}));
    ASSERT_EQ(definition.at("conditional_edges").size(), 1U);
    EXPECT_EQ(definition.at("conditional_edges").at(0).at("routes").at("done"),
              "terminal");
    ASSERT_EQ(definition.at("edges").size(), 3U);
    EXPECT_EQ(definition.at("edges").at(0),
              (json{{"from", "__start__"}, {"to", "draft"}}));
    EXPECT_EQ(definition.at("edges").at(1),
              (json{{"from", "draft"}, {"to", "review"}}));
    EXPECT_EQ(definition.at("edges").at(2),
              (json{{"from", "terminal"}, {"to", "__end__"}}));
}

TEST(ProgramCompilerTest, JavaScriptCapabilityManifestMatchesTheInstalledGraphBuilder) {
    const auto source = ProgramSource::from_javascript(
        "graph-capability-manifest.js",
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                const methods = Object.keys(graph).sort();
                for (const method of methods) {
                    graph.node(method, {type: "local-node"});
                }
                graph.entry(methods[0]);
                for (let index = 1; index < methods.length; ++index) {
                    graph.edge(methods[index - 1], methods[index]);
                }
                graph.exit(methods[methods.length - 1]);
                return graph;
            }
        )JS");
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto bundle = compiler.compile(source);
    const auto definition = bundle.sealed_core_definitions().front().definition;

    std::vector<std::string> installed;
    for (const auto& [name, unused] : definition.at("nodes").items()) {
        (void)unused;
        installed.push_back(name);
    }
    std::sort(installed.begin(), installed.end());

    std::vector<std::string> declared;
    const auto manifest = javascript_authoring_capability_manifest();
    for (const auto& method : manifest.at("define").at("graph_builder_methods"))
        declared.push_back(method.at("name").get<std::string>());
    std::sort(declared.begin(), declared.end());
    EXPECT_EQ(installed, declared);
    EXPECT_EQ(manifest.at("javascript_profile"), ProgramSource::JAVASCRIPT_PROFILE);
    EXPECT_EQ(manifest.at("ng_api_version"), ProgramSource::JAVASCRIPT_NG_API_VERSION);
}

TEST(ProgramCompilerTest, JavaScriptHostBudgetAndContractsReplaceEvaluatedDeclarations) {
    const auto           source = ProgramSource::from_javascript("host-authority.js",
                                                                 R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                graph.node("work", {type: "local-node"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }
        )JS");
    const RunBudget      budget{5000, 1000, 1, 2, 4, 20, 0, 0, 0};
    const ContractRecord input{1,
                               {{"type", "object"},
                                {"required", json::array({"task"})},
                                {"properties", {{"task", {{"type", "object"}}}}},
                                {"additionalProperties", false}}};
    const ContractRecord output{1,
                                {{"type", "object"},
                                 {"required", json::array({"outcome"})},
                                 {"properties", {{"outcome", {{"type", "string"}}}}},
                                 {"additionalProperties", false}}};

    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript-host/v1"});
    const auto      bundle = compiler.compile(source, budget, input, output);

    EXPECT_EQ(bundle.input_contract().schema_version, input.schema_version);
    EXPECT_EQ(bundle.input_contract().schema, input.schema);
    EXPECT_EQ(bundle.output_contract().schema_version, output.schema_version);
    EXPECT_EQ(bundle.output_contract().schema, output.schema);
    const json expected = {
        {"wall_time_ms", budget.wall_time_ms},
        {"model_tokens", budget.model_tokens},
        {"monetary_microunits", budget.monetary_microunits},
        {"max_concurrency", budget.max_concurrency},
        {"max_program_operations", budget.max_program_operations},
        {"max_core_steps", budget.max_core_steps},
        {"max_dynamic_compiles", budget.max_dynamic_compiles},
        {"max_child_depth", budget.max_child_depth},
        {"max_total_children", budget.max_total_children},
    };
    ASSERT_EQ(bundle.declared_budget_requirements().size(), expected.size());
    for (const auto& requirement : bundle.declared_budget_requirements()) {
        ASSERT_TRUE(expected.contains(requirement.resource));
        EXPECT_EQ(requirement.minimum, expected.at(requirement.resource).get<std::uint64_t>());
        EXPECT_EQ(requirement.maximum, expected.at(requirement.resource).get<std::uint64_t>());
    }
}

TEST(ProgramCompilerTest, JavaScriptHostBudgetBoundsSealAnAdmissibleInterval) {
    const auto source = ProgramSource::from_javascript("host-budget-bounds.js", R"JS(
        export function define() {
            const graph = ng.graph("main");
            graph.channel("value", {reducer: "local-reducer", initial: 0});
            graph.node("work", {type: "local-node"});
            graph.entry("work");
            graph.exit("work");
            return graph;
        }
    )JS");
    const ProgramBudgetBounds bounds{
        RunBudget{1, 0, 0, 1, 1, 1, 0, 0, 0},
        RunBudget{5000, 1000, 1, 2, 4, 20, 0, 0, 0}};
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript-bounds/v1"});
    const auto bundle = compiler.compile(source, bounds);

    const json expected_minimum = {
        {"wall_time_ms", 1}, {"model_tokens", 0}, {"monetary_microunits", 0},
        {"max_concurrency", 1}, {"max_program_operations", 1}, {"max_core_steps", 1},
        {"max_dynamic_compiles", 0}, {"max_child_depth", 0}, {"max_total_children", 0},
    };
    const json expected_maximum = {
        {"wall_time_ms", 5000}, {"model_tokens", 1000}, {"monetary_microunits", 1},
        {"max_concurrency", 2}, {"max_program_operations", 4}, {"max_core_steps", 20},
        {"max_dynamic_compiles", 0}, {"max_child_depth", 0}, {"max_total_children", 0},
    };
    for (const auto& requirement : bundle.declared_budget_requirements()) {
        EXPECT_EQ(requirement.minimum,
                  expected_minimum.at(requirement.resource).get<std::uint64_t>());
        EXPECT_EQ(requirement.maximum,
                  expected_maximum.at(requirement.resource).get<std::uint64_t>());
    }

    auto inverted = bounds;
    inverted.minimum.max_core_steps = inverted.maximum.max_core_steps + 1;
    EXPECT_THROW((void)compiler.compile(source, inverted), std::invalid_argument);
}

TEST(ProgramCompilerTest, JavaScriptDefinitionMustReturnOneGraphBuilder) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "missing-builder.js", "export function define() { return {name: 'main'}; }");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        ASSERT_TRUE(contains_code(error.diagnostics(), "P_JS_DEFINE_VALUE"));
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_JS_DEFINE_VALUE";
            });
        ASSERT_NE(it, error.diagnostics().end());
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_EQ(it->primary.source_id, "missing-builder.js");
        EXPECT_GT(it->primary.span->byte_end, it->primary.span->byte_begin);
        EXPECT_EQ(it->primary.span->line_begin, 1U);
    }
}

TEST(ProgramCompilerTest, JavaScriptDefinitionCannotReturnMoreThanOneBuilder) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "two-builders.js",
        R"JS(
            export function define() {
                const first = ng.graph("first");
                const second = ng.graph("second");
                return [first, second];
            }
        )JS");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_DEFINE_VALUE"));
    }
}

TEST(ProgramCompilerTest, JavaScriptSourceCannotOutrunItsInterruptBudget) {
    ProgramCompilerConfig config;
    config.compiler_build_id              = "program-compiler-test/javascript/v1";
    config.javascript.max_interrupt_polls = 1;
    ProgramCompiler compiler(complete_snapshot(), std::move(config));
    const auto source = ProgramSource::from_javascript(
        "infinite.js", "export function define() { while (true) { } }");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_RESOURCE_LIMIT"));
    }
}

TEST(ProgramCompilerTest, JavaScriptStrictProfileDeniesAmbientCapabilitiesDeterministically) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const std::vector<std::string> denied_sources = {
        "export function define() { Date.now(); }",
        "export function define() { eval('1 + 1'); }",
        "export function define() { Function('return 1')(); }",
        "export function define() { (function() {}).constructor('return 1')(); }",
        "export function define() { (function*() {}).constructor('yield 1')(); }",
        "export function define() { (async function() {}).constructor('return 1')(); }",
        "export function define() { (async function*() {}).constructor('yield 1')(); }",
    };
    for (std::size_t index = 0; index < denied_sources.size(); ++index) {
        const auto source = ProgramSource::from_javascript(
            "ambient-denied-" + std::to_string(index) + ".js", denied_sources[index]);
        try {
            (void)compiler.compile(source);
            FAIL() << "expected ambient capability denial for source " << index;
        } catch (const ProgramCompileError& error) {
            EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_AMBIENT_DENIED"))
                << "source " << index;
        }
    }
}

TEST(ProgramCompilerTest, JavaScriptStrictProfileReplacesRandomAndHidesWorkerFacilities) {
    const auto source = ProgramSource::from_javascript(
        "strict-profile.js",
        R"JS(
            export function define() {
                if (Math.random() !== 0 || typeof Worker !== "undefined" ||
                    typeof SharedWorker !== "undefined" || typeof SharedArrayBuffer !== "undefined" ||
                    typeof Atomics !== "undefined" || typeof require !== "undefined" ||
                    typeof fetch !== "undefined") {
                    throw new Error("strict profile leaked a nondeterministic facility");
                }
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                graph.node("work", {type: "local-node"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }
        )JS");
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    EXPECT_EQ(compiler.compile(source).source_kind(), SourceKind::JavaScript);
}

TEST(ProgramCompilerTest, JavaScriptSourceStopsAtItsWallTimeCeiling) {
    ProgramCompilerConfig config;
    config.compiler_build_id              = "program-compiler-test/javascript-wall-time/v1";
    config.javascript.max_interrupt_polls = std::numeric_limits<std::uint64_t>::max();
    config.javascript.max_wall_time_ms    = 1;
    ProgramCompiler compiler(complete_snapshot(), std::move(config));
    const auto source = ProgramSource::from_javascript(
        "wall-time.js", "export function define() { while (true) { } }");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_TIMEOUT"));
    }
}

TEST(ProgramCompilerTest, JavaScriptGraphBuilderAccountsNativeDocumentBytes) {
    ProgramCompilerConfig config;
    config.compiler_build_id                  = "program-compiler-test/javascript-bytes/v1";
    config.javascript.max_generated_document_bytes = 256;
    ProgramCompiler compiler(complete_snapshot(), std::move(config));
    const auto source = ProgramSource::from_javascript(
        "native-bytes.js",
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.node("work", {type: "local-node", padding: "x".repeat(512)});
                return graph;
            }
        )JS");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_GRAPH_LIMIT"));
    }
}

TEST(ProgramCompilerTest, JavaScriptRetainedBuildersShareTheQuickJsMemoryCeiling) {
    ProgramCompilerConfig config;
    config.compiler_build_id              = "program-compiler-test/javascript-native-live/v1";
    config.javascript.memory_limit_bytes  = 8u * 1024u * 1024u;
    config.javascript.max_stack_bytes     = 128u * 1024u;
    config.javascript.max_interrupt_polls = 100'000'000u;
    ProgramCompiler compiler(complete_snapshot(), std::move(config));
    const auto      source = ProgramSource::from_javascript("native-live-builders.js",
                                                            R"JS(
            export function define() {
                const retained = [];
                for (let index = 0; index < 16; ++index) {
                    retained.push(ng.graph("retained-" + index + "-" + "x".repeat(1024 * 1024)));
                }
                return ng.graph("main");
            }
        )JS");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected cumulative native-memory rejection";
    } catch (const ProgramCompileError& error) {
        ASSERT_FALSE(error.diagnostics().empty());
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_RESOURCE_LIMIT"))
            << error.diagnostics().front().code << ": "
            << error.diagnostics().front().message << " "
            << error.diagnostics().front().witness.dump();
    }
}

TEST(ProgramCompilerTest, JavaScriptPreservesSafeIntegerBoundariesAndFiniteDoubles) {
    ProgramCompiler compiler(complete_snapshot(),
                             {"program-compiler-test/javascript-safe-number/v1"});
    const auto      source = ProgramSource::from_javascript("safe-number-boundaries.js",
                                                            R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: null});
                graph.node("work", {
                    type: "local-node",
                    positive_safe: 9007199254740991,
                    negative_safe: -9007199254740991,
                    fractional: 1234.5
                });
                graph.entry("work");
                graph.exit("work");
                return graph;
            }
        )JS");

    std::optional<ProgramBundle> bundle;
    try {
        bundle = compiler.compile(source);
    } catch (const ProgramCompileError& error) {
        for (const auto& diagnostic : error.diagnostics()) {
            ADD_FAILURE() << diagnostic.code << " " << diagnostic.primary.json_pointer << ": "
                          << diagnostic.message << " " << diagnostic.witness.dump();
        }
        return;
    }
    ASSERT_EQ(bundle->sealed_core_definitions().size(), 1U);
    const auto& node = bundle->sealed_core_definitions().front().definition.at("nodes").at("work");
    EXPECT_EQ(node.at("positive_safe").get<std::uint64_t>(), 9007199254740991ULL);
    EXPECT_EQ(node.at("negative_safe").get<std::int64_t>(), -9007199254740991LL);
    EXPECT_EQ(node.at("fractional").get<double>(), 1234.5);
}

TEST(ProgramCompilerTest, JavaScriptRejectsUnsafeIntegralNumbersBeforeJsonConversion) {
    ProgramCompiler                                        compiler(complete_snapshot(),
                                                                    {"program-compiler-test/javascript-unsafe-number/v1"});
    const std::vector<std::pair<std::string, std::string>> cases{
        {"positive-2p53.js", "9007199254740992"},
        {"rounded-2p53-plus-one.js", "9007199254740993"},
        {"negative-2p53.js", "-9007199254740992"},
    };

    for (const auto& [source_id, literal] : cases) {
        const auto source = ProgramSource::from_javascript(
            source_id,
            "export function define() { const graph = ng.graph(\"main\"); "
            "graph.node(\"work\", {type: \"local-node\", value: " +
                literal + "}); return graph; }");
        try {
            (void)compiler.compile(source);
            FAIL() << "expected safe-integer rejection for " << literal;
        } catch (const ProgramCompileError& error) {
            const auto diagnostic =
                std::find_if(error.diagnostics().begin(), error.diagnostics().end(),
                             [](const auto& value) { return value.code == "P_JS_GRAPH_VALUE"; });
            ASSERT_NE(diagnostic, error.diagnostics().end()) << literal;
            EXPECT_NE(diagnostic->message.find("outside the JavaScript safe-integer range"),
                      std::string::npos)
                << literal;
        }
    }
}

TEST(ProgramCompilerTest, JavaScriptPreservesEmbeddedNulPropertyKeys) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript-nul-key/v1"});
    const auto      source = ProgramSource::from_javascript("nul-key.js",
                                                            R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                graph.node("work", {type: "local-node", "a\u0000b": 1, a: 2});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }
        )JS");

    const auto bundle = compiler.compile(source);
    ASSERT_EQ(bundle.sealed_core_definitions().size(), 1U);
    const auto& node = bundle.sealed_core_definitions().front().definition.at("nodes").at("work");
    const std::string embedded_nul("a\0b", 3);
    ASSERT_TRUE(node.contains(embedded_nul));
    EXPECT_EQ(node.at(embedded_nul), 1);
    EXPECT_EQ(node.at("a"), 2);
}

TEST(ProgramCompilerTest, JavaScriptHostExposesOnlyItsFrozenVersionedGraphBinding) {
    const auto source = ProgramSource::from_javascript(
        "sandbox.js",
        R"JS(
            export function define() {
                if (typeof std !== "undefined" || typeof os !== "undefined" ||
                    typeof process !== "undefined" || typeof neograph !== "undefined" ||
                    typeof ng.callCore !== "undefined" || ng.apiVersion !== 1 ||
                    !Object.isFrozen(ng)) {
                    throw new Error("host capability leaked");
                }
                const graph = ng.graph("main");
                graph.channel("value", {reducer: "local-reducer", initial: 0});
                graph.node("work", {type: "local-node"});
                graph.entry("work");
                graph.exit("work");
                return graph;
            }
        )JS");

    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    EXPECT_EQ(compiler.compile(source).source_kind(), SourceKind::JavaScript);
}

TEST(ProgramCompilerTest, JavaScriptGraphBuilderRejectsUnknownObjectsBeforeCoreAdmission) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "invalid-builder-value.js",
        R"JS(
            export function define() {
                const graph = ng.graph("main");
                graph.node("work", new Date());
                graph.entry("work");
                return graph;
            }
        )JS");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_AMBIENT_DENIED"));
    }
}

TEST(ProgramCompilerTest, JavaScriptSyntaxDiagnosticsCarryExactSourceSites) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "syntax.js", "export function define() {\n  const graph = ;\n  return graph;\n}\n");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_JS_EVALUATION";
            });
        ASSERT_NE(it, error.diagnostics().end());
        EXPECT_EQ(it->primary.source_id, "syntax.js");
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_GE(it->primary.span->line_begin, 2U);
        EXPECT_GT(it->primary.span->byte_end, it->primary.span->byte_begin);
    }
}

TEST(ProgramCompilerTest, JavaScriptBridgeDiagnosticsCarryExactSourceSites) {
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "bridge.js",
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.node(\"work\", new Map());\n"
        "  return graph;\n"
        "}\n");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_JS_GRAPH_VALUE";
            });
        ASSERT_NE(it, error.diagnostics().end());
        EXPECT_EQ(it->primary.source_id, "bridge.js");
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_GE(it->primary.span->line_begin, 2U);
        EXPECT_GT(it->primary.span->byte_end, it->primary.span->byte_begin);
    }
}

TEST(ProgramCompilerTest, JavaScriptSealedModuleClosureExecutesAtCompileTime) {
    const std::string leaf_source = R"JS(
        export const nodeType = "local-node";
    )JS";
    ProgramModuleData leaf_data;
    leaf_data.owner_scope            = "tenant:sealed";
    leaf_data.coordinate             = ModuleCoordinate{"sealed", "node-type", "1.0.0", ""};
    leaf_data.attestation_id         = "attestation:sealed";
    leaf_data.pure_javascript_source = leaf_source;
    const auto leaf = ProgramModule::create(std::move(leaf_data));

    const auto helper_name = ModuleCoordinate{"sealed", "stages", "1.0.0", ""}.qualified_name();
    const std::string helper_source =
        "import { nodeType } from \"" + leaf.coordinate().qualified_name() + "\";\n"
        "export const stages = [[\"draft\", nodeType], [\"review\", nodeType]];\n";
    ProgramModuleData helper_data;
    helper_data.owner_scope            = "tenant:sealed";
    helper_data.coordinate             = ModuleCoordinate{"sealed", "stages", "1.0.0", ""};
    helper_data.attestation_id         = "attestation:sealed";
    helper_data.dependencies           = {leaf.coordinate()};
    helper_data.pure_javascript_source = helper_source;
    const auto helper = ProgramModule::create(std::move(helper_data));
    EXPECT_EQ(helper.coordinate().qualified_name(), helper_name);

    ModuleResolution resolution;
    resolution.root     = helper.coordinate();
    resolution.modules  = {helper, leaf};
    resolution.receipts = {{helper.coordinate().qualified_name(), helper.id()},
                           {leaf.coordinate().qualified_name(), leaf.id()}};
    ASSERT_NO_THROW(validate_module_resolution(resolution));

    const auto source = ProgramSource::from_javascript(
        "sealed-root.js",
        "import { stages } from \"" + helper.coordinate().qualified_name() + "\";\n"
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.channel(\"value\", {reducer: \"local-reducer\", initial: 0});\n"
        "  for (const [name, type] of stages) graph.node(name, {type});\n"
        "  graph.entry(\"draft\");\n"
        "  graph.edge(\"draft\", \"review\");\n"
        "  graph.conditionalEdge(\"review\", \"local-condition\", {done: \"__end__\"});\n"
        "  return graph;\n"
        "}\n",
        resolution.imports(), {},
        {{helper.coordinate().qualified_name(), helper.id(), helper_source},
         {leaf.coordinate().qualified_name(), leaf.id(), leaf_source}});

    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto      bundle = compiler.compile(source, resolution);

    EXPECT_EQ(bundle.module_dependency_merkle_root(), resolution.dependency_merkle_root());
    ASSERT_EQ(bundle.sealed_core_definitions().size(), 1U);
    EXPECT_EQ(bundle.sealed_core_definitions().front().definition.at("nodes").size(), 2U);
}

TEST(ProgramCompilerTest, JavaScriptAmbientImportsFailBeforeAnyDispatch) {
    reset_dispatch_counters();
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "ambient.js",
        "import \"std\";\n"
        "export function define() { return ng.graph(\"main\"); }\n");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_IMPORT_UNRESOLVED";
            });
        ASSERT_NE(it, error.diagnostics().end());
        EXPECT_EQ(it->phase, CompilePhase::Resolve);
        EXPECT_EQ(it->primary.source_id, "ambient.js");
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_EQ(it->primary.span->line_begin, 1U);
    }
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, JavaScriptDynamicImportsFailBeforeAnyDispatch) {
    reset_dispatch_counters();
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto source = ProgramSource::from_javascript(
        "dynamic.js",
        "export function define() {\n"
        "  import(\"module\");\n"
        "  return ng.graph(\"main\");\n"
        "}\n");

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_IMPORT_DYNAMIC";
            });
        ASSERT_NE(it, error.diagnostics().end());
        EXPECT_EQ(it->phase, CompilePhase::Resolve);
        EXPECT_EQ(it->primary.source_id, "dynamic.js");
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_EQ(it->primary.span->line_begin, 2U);
    }
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, JavaScriptBundleRoundTripPinsExactFrontendIdentity) {
    const auto source = ProgramSource::from_javascript(
        "identity.js",
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.channel(\"value\", {reducer: \"local-reducer\", initial: 0});\n"
        "  graph.node(\"work\", {type: \"local-node\"});\n"
        "  graph.entry(\"work\");\n"
        "  graph.exit(\"work\");\n"
        "  return graph;\n"
        "}\n");
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto bundle  = compiler.compile(source);
    const auto parsed  = ProgramBundle::parse(bundle.serialize_canonical());
    const auto runtime = source.javascript_runtime_identity();

    EXPECT_EQ(bundle.javascript_runtime(), runtime);
    EXPECT_EQ(parsed.javascript_runtime(), runtime);
    const auto encoded = json::parse(bundle.serialize_canonical());
    EXPECT_EQ(encoded.at("quickjs_release"), runtime.quickjs_release);
    EXPECT_EQ(encoded.at("quickjs_archive_digest"), runtime.quickjs_archive_digest);
    EXPECT_EQ(encoded.at("quickjs_build_options"), runtime.quickjs_build_options);
    EXPECT_EQ(encoded.at("javascript_profile"), runtime.profile);
    EXPECT_EQ(encoded.at("javascript_profile_version"), runtime.profile_version);
    EXPECT_EQ(encoded.at("ng_api_version"), runtime.ng_api_version);

    ProgramCompilerConfig mismatch;
    mismatch.compiler_build_id      = "program-compiler-test/javascript/v1";
    mismatch.quickjs_build_options += ";changed";
    ProgramCompiler incompatible(complete_snapshot(), std::move(mismatch));
    try {
        (void)incompatible.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        EXPECT_TRUE(contains_code(error.diagnostics(), "P_JS_RUNTIME_IDENTITY"));
    }
}

TEST(ProgramCompilerTest, JavaScriptSourceMapRoundTripsAlongsideBundleSites) {
    const SourceMapEntry authored{
        "/root", {"authoring.dsl", "/graph", SourceSpan{4, 18, 2, 3, 2, 17}}};
    const auto source = ProgramSource::from_javascript(
        "mapped.js",
        "export function define() {\n"
        "  const graph = ng.graph(\"main\");\n"
        "  graph.channel(\"value\", {reducer: \"local-reducer\", initial: 0});\n"
        "  graph.node(\"work\", {type: \"local-node\"});\n"
        "  graph.entry(\"work\");\n"
        "  graph.exit(\"work\");\n"
        "  return graph;\n"
        "}\n",
        {}, {authored});
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    const auto bundle = compiler.compile(source);
    const auto parsed = ProgramBundle::parse(bundle.serialize_canonical());
    const auto it = std::find(parsed.source_map().begin(), parsed.source_map().end(), authored);
    EXPECT_NE(it, parsed.source_map().end());
}

TEST(ProgramCompilerTest, JavaScriptReceiptMatchedStaticImportStillFailsBeforeEvaluation) {
    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:compiler";
    module_data.coordinate     = ModuleCoordinate{"sealed", "allowed", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    const auto module          = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = module.coordinate();
    resolution.modules.push_back(module);
    resolution.receipts.push_back({module.coordinate().qualified_name(), module.id()});

    const auto source =
        ProgramSource::from_javascript("allowlist.js",
                                       "import \"sealed:allowed@1.0.0\";\n"
                                       "export function define() { return ng.graph(\"main\"); }\n",
                                       {{module.coordinate().qualified_name(), module.id()}});
    reset_dispatch_counters();
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});
    try {
        (void)compiler.compile(source, resolution);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto it = std::find_if(
            error.diagnostics().begin(), error.diagnostics().end(), [](const auto& diagnostic) {
                return diagnostic.code == "P_IMPORT_UNRESOLVED";
            });
        ASSERT_NE(it, error.diagnostics().end());
        EXPECT_EQ(it->phase, CompilePhase::Resolve);
        EXPECT_EQ(it->primary.source_id, "allowlist.js");
        ASSERT_TRUE(it->primary.span.has_value());
        EXPECT_EQ(it->witness.at("receipt_bound_source_available"), false);
        EXPECT_EQ(it->primary.span->line_begin, 1U);
    }
    expect_dispatch_counters_zero();
}
#else
TEST(ProgramCompilerTest, JavaScriptSourceRequiresQuickJSControlFeature) {
    const auto source = ProgramSource::from_javascript(
        "disabled.js", "export function define() { return ng.graph(\"main\"); }\n");
    ProgramCompiler compiler(complete_snapshot(), {"program-compiler-test/javascript/v1"});

    try {
        (void)compiler.compile(source);
        FAIL() << "expected ProgramCompileError";
    } catch (const ProgramCompileError& error) {
        const auto diagnostic =
            std::find_if(error.diagnostics().begin(), error.diagnostics().end(),
                         [](const auto& value) { return value.code == "P_JS_UNAVAILABLE"; });
        ASSERT_NE(diagnostic, error.diagnostics().end());
        EXPECT_EQ(diagnostic->phase, CompilePhase::Source);
        EXPECT_EQ(diagnostic->primary.source_id, "disabled.js");
    }
}
#endif

TEST(ProgramCompilerTest, RejectsInvalidJavaScriptCompilerLimits) {
    ProgramCompilerConfig zero_interrupts{"program-compiler-test/javascript-limits/v1"};
    zero_interrupts.javascript.max_interrupt_polls = 0;
    EXPECT_THROW((void)ProgramCompiler(complete_snapshot(), std::move(zero_interrupts)),
                 std::invalid_argument);

    ProgramCompilerConfig zero_wall_time{"program-compiler-test/javascript-limits/v1"};
    zero_wall_time.javascript.max_wall_time_ms = 0;
    EXPECT_THROW((void)ProgramCompiler(complete_snapshot(), std::move(zero_wall_time)),
                 std::invalid_argument);

    ProgramCompilerConfig zero_document_bytes{"program-compiler-test/javascript-limits/v1"};
    zero_document_bytes.javascript.max_generated_document_bytes = 0;
    EXPECT_THROW((void)ProgramCompiler(complete_snapshot(), std::move(zero_document_bytes)),
                 std::invalid_argument);

    ProgramCompilerConfig oversized_stack{"program-compiler-test/javascript-limits/v1"};
    oversized_stack.javascript.memory_limit_bytes = 1024;
    oversized_stack.javascript.max_stack_bytes    = 2048;
    EXPECT_THROW((void)ProgramCompiler(complete_snapshot(), std::move(oversized_stack)),
                 std::invalid_argument);
}

TEST(ProgramCompilerTest, EquivalentKeyAndRegistrationOrderProduceSameBundle) {
    reset_dispatch_counters();
    auto            first_snapshot  = complete_snapshot(false);
    auto            second_snapshot = complete_snapshot(true);
    ProgramCompiler first(first_snapshot, {"program-compiler-test/v1"});
    ProgramCompiler second(second_snapshot, {"program-compiler-test/v1"});

    auto first_document                       = program_document();
    json second_document                      = json::object();
    second_document["root"]                   = first_document["root"];
    second_document["output_contract"]        = first_document["output_contract"];
    second_document["program_schema_version"] = 1;
    second_document["declared_budget_requirements"] =
        first_document["declared_budget_requirements"];
    second_document["input_contract"] = first_document["input_contract"];

    const auto first_bundle  = first.compile(source_from(first_document));
    const auto second_bundle = second.compile(source_from(second_document));
    EXPECT_EQ(first_snapshot.fingerprint(), second_snapshot.fingerprint());
    EXPECT_EQ(first_bundle.serialize_canonical(), second_bundle.serialize_canonical());
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest,
     SourceFormattingAndSourceMapDoNotChangeCanonicalProgramHashButDoChangeStoredAttribution) {
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      document = program_document();
    auto            compact  = ProgramSource::from_cpp_builder("format:test", 1, document);
    SourceMapEntry mapped{"/root/definition", {"dsl:test", "/graph", SourceSpan{0, 4, 1, 1, 1, 5}}};
    auto           formatted = ProgramSource::from_cpp_builder("format:test", 1, document, {}, {mapped});

    const auto first  = compiler.compile(compact);
    const auto second = compiler.compile(formatted);
    EXPECT_EQ(first.canonical_program_hash(), second.canonical_program_hash());
    EXPECT_EQ(first.source_hash(), second.source_hash());
    EXPECT_NE(first.id(), second.id());
    const auto it = std::find_if(
        second.source_map().begin(), second.source_map().end(),
        [](const auto& entry) { return entry.generated_pointer == "/root/definition"; });
    ASSERT_NE(it, second.source_map().end());
    EXPECT_EQ(it->authored.source_id, "dsl:test");
}
TEST(ProgramCompilerTest, DiagnosticsRestoreAuthoredCoordinatesAfterCoreNormalization) {
    auto document = program_document();
    document["root"]["definition"]["name"] = "different";
    const SourceMapEntry mapped{
        "/root/definition", {"dsl:test", "/graph", SourceSpan{10, 20, 1, 1, 1, 11}}};

    const auto diagnostics = compile_errors(complete_snapshot(), std::move(document), {}, {mapped});
    const auto it = std::find_if(
        diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "P_ROOT_NAME";
        });
    ASSERT_NE(it, diagnostics.end());
    EXPECT_EQ(it->primary.source_id, "dsl:test");
    EXPECT_EQ(it->primary.json_pointer, "/graph/name");
    EXPECT_FALSE(it->primary.span.has_value());
}


TEST(ProgramCompilerTest,
     CoreSemanticChangeCompilerBuildIdRegistryDigestAndDependencyEachChangeExpectedIdentity) {
    auto            baseline_snapshot = complete_snapshot(false, '1', '4');
    ProgramCompiler baseline_compiler(baseline_snapshot, {"compiler:A"});
    const auto      baseline = baseline_compiler.compile(source_from(program_document()));

    auto semantic_document                                              = program_document();
    semantic_document["root"]["definition"]["nodes"]["work"]["variant"] = 1;
    const auto semantic = baseline_compiler.compile(source_from(std::move(semantic_document)));
    EXPECT_NE(baseline.canonical_program_hash(), semantic.canonical_program_hash());
    EXPECT_NE(baseline.sealed_core_definitions()[0].definition_hash,
              semantic.sealed_core_definitions()[0].definition_hash);
    EXPECT_NE(baseline.core_plan_identities()[0], semantic.core_plan_identities()[0]);

    ProgramCompiler compiler_changed(baseline_snapshot, {"compiler:B"});
    const auto      compiler_variant = compiler_changed.compile(source_from(program_document()));
    EXPECT_EQ(baseline.canonical_program_hash(), compiler_variant.canonical_program_hash());
    EXPECT_NE(baseline.core_plan_identities()[0], compiler_variant.core_plan_identities()[0]);

    auto            node_changed_snapshot = complete_snapshot(false, '8', '4');
    ProgramCompiler node_changed(node_changed_snapshot, {"compiler:A"});
    const auto      node_variant = node_changed.compile(source_from(program_document()));
    EXPECT_EQ(baseline.canonical_program_hash(), node_variant.canonical_program_hash());
    EXPECT_NE(baseline.registry_snapshot_fingerprint(),
              node_variant.registry_snapshot_fingerprint());
    EXPECT_NE(baseline.core_plan_identities()[0], node_variant.core_plan_identities()[0]);

    auto            dependency_changed_snapshot = complete_snapshot(false, '1', '9');
    ProgramCompiler dependency_changed(dependency_changed_snapshot, {"compiler:A"});
    const auto dependency_variant = dependency_changed.compile(source_from(program_document()));
    EXPECT_EQ(baseline.canonical_program_hash(), dependency_variant.canonical_program_hash());
    EXPECT_NE(baseline.core_plan_identities()[0], dependency_variant.core_plan_identities()[0]);
    EXPECT_NE(baseline.executable_registry_identities(),
              dependency_variant.executable_registry_identities());
}

TEST(ProgramCompilerTest,
     ConfigRequirementResolverSelectsExactProviderToolAndImportedClosureDeterministically) {
    reset_dispatch_counters();
    const auto snapshot   = config_requirement_snapshot();
    auto       selected   = node_only_program("configured-node");
    auto       config     = selected["root"]["definition"]["nodes"]["work"];
    config["provider_id"] = "configured-provider";
    config["tool_ids"]    = json::array({"configured-beta", "configured-alpha", "configured-beta"});
    config["import_id"]   = "configured-import";

    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      bundle   = compiler.compile(source_from(std::move(selected)));
    const auto      expected = std::vector<ExecutableIdentity>{
        {ExecutableKind::Imported, "configured-import", "1.0.0", digest('9')},
        {ExecutableKind::Node, "configured-node", "1.0.0", digest('7')},
        configured_provider(),
        configured_tool("configured-alpha", '9'),
        configured_tool("configured-beta", 'a'),
    };
    EXPECT_EQ(bundle.executable_registry_identities(), expected);

    auto alpha_only             = node_only_program("configured-node");
    auto alpha_config           = alpha_only["root"]["definition"]["nodes"]["work"];
    alpha_config["provider_id"] = "configured-provider";
    alpha_config["tool_ids"]    = json::array({"configured-alpha"});
    const auto alpha_bundle     = compiler.compile(source_from(std::move(alpha_only)));
    EXPECT_NE(alpha_bundle.executable_registry_identities(),
              bundle.executable_registry_identities());
    EXPECT_NE(alpha_bundle.core_plan_identities(), bundle.core_plan_identities());
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest,
     ConfigRequirementResolverRejectsUnsupportedMissingAndMismatchedExactIdentity) {
    const auto compile_with = [](ExecutableIdentity returned, ExecutableIdentity registered) {
        RegistrySnapshotBuilder builder;
        builder.add_provider(executable(ExecutableKind::Provider, registered.name,
                                        registered.implementation_digest.back()),
                             ProviderMetadata{json::object(), json::object()});
        builder.add_node(executable(ExecutableKind::Node, "resolver-node", '7'), local_factory(),
                         json{{"type", "object"}}, json::object(), [returned](const json&) {
                             return std::vector<ExecutableIdentity>{returned};
                         });
        auto snapshot = std::move(builder).build();
        return compile_errors(snapshot, node_only_program("resolver-node"));
    };

    reset_dispatch_counters();
    const ExecutableIdentity registered{ExecutableKind::Provider, "registered-provider", "1.0.0",
                                        digest('c')};
    const ExecutableIdentity missing{ExecutableKind::Provider, "missing-provider", "1.0.0",
                                     digest('d')};
    const auto               missing_errors = compile_with(missing, registered);
    const auto               missing_error  = std::find_if(
        missing_errors.begin(), missing_errors.end(),
        [](const auto& error) { return error.code == "P_REGISTRY_DEPENDENCY_MISSING"; });
    ASSERT_NE(missing_error, missing_errors.end());
    EXPECT_EQ(missing_error->primary.json_pointer, "/root/definition/nodes/work");

    auto mismatched                  = registered;
    mismatched.implementation_digest = digest('d');
    const auto mismatch_errors       = compile_with(mismatched, registered);
    const auto mismatch_error        = std::find_if(
        mismatch_errors.begin(), mismatch_errors.end(),
        [](const auto& error) { return error.code == "P_REGISTRY_IDENTITY_MISMATCH"; });
    ASSERT_NE(mismatch_error, mismatch_errors.end());
    EXPECT_EQ(mismatch_error->primary.json_pointer, "/root/definition/nodes/work");

    auto unsupported = registered;
    unsupported.kind = ExecutableKind::Reducer;
    EXPECT_TRUE(
        contains_code(compile_with(unsupported, registered), "P_REGISTRY_REQUIREMENT_KIND"));
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, ConfigRequirementResolverExceptionIsPointerDiagnosticBeforeDispatch) {
    RegistrySnapshotBuilder builder;
    builder.add_node(executable(ExecutableKind::Node, "throwing-resolver-node", '7'),
                     local_factory(), json{{"type", "object"}}, json::object(),
                     [](const json&) -> std::vector<ExecutableIdentity> {
                         throw std::runtime_error("resolver failure");
                     });
    auto snapshot = std::move(builder).build();

    reset_dispatch_counters();
    const auto errors = compile_errors(snapshot, node_only_program("throwing-resolver-node"));
    const auto resolver_error = std::find_if(errors.begin(), errors.end(), [](const auto& error) {
        return error.code == "P_REGISTRY_REQUIREMENT_RESOLVER";
    });
    ASSERT_NE(resolver_error, errors.end());
    EXPECT_EQ(resolver_error->primary.json_pointer, "/root/definition/nodes/work");
    EXPECT_EQ(resolver_error->witness["exception"], "resolver failure");
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, SourceRequirementLookalikeFieldsCannotGrantExecutables) {
    reset_dispatch_counters();
    auto snapshot                  = node_only_snapshot("lookalike-node");
    auto source                    = node_only_program("lookalike-node");
    auto config                    = source["root"]["definition"]["nodes"]["work"];
    config["provider_id"]          = "source-provider";
    config["tool_ids"]             = json::array({"source-tool"});
    config["required_executables"] = json::array({json{{"kind", "provider"},
                                                       {"name", "source-provider"},
                                                       {"semantic_version", "1.0.0"},
                                                       {"implementation_digest", digest('e')}}});

    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      bundle = compiler.compile(source_from(std::move(source)));
    ASSERT_EQ(bundle.executable_registry_identities().size(), 1U);
    EXPECT_EQ(bundle.executable_registry_identities().front().kind, ExecutableKind::Node);
    EXPECT_EQ(bundle.executable_registry_identities().front().name, "lookalike-node");
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, RejectsUnknownProgramFieldsAndWrongNestedTypes) {
    auto snapshot = complete_snapshot();
    EXPECT_THROW((void)ProgramCompiler(snapshot, ProgramCompilerConfig{""}), std::invalid_argument);
    EXPECT_THROW((void)ProgramCompiler(snapshot, ProgramCompilerConfig{"bad\nbuild"}),
                 std::invalid_argument);
    EXPECT_THROW((void)ProgramCompiler(snapshot, ProgramCompilerConfig{"bad\xc2\x80"
                                                                       "build"}),
                 std::invalid_argument);
    auto unknown                                        = program_document();
    unknown["compiler_build_id"]                        = "source-authored";
    unknown["input_contract"]["extra"]                  = true;
    unknown["root"]["extra"]                            = true;
    unknown["declared_budget_requirements"][0]["extra"] = true;
    const auto unknown_errors = compile_errors(snapshot, std::move(unknown));
    EXPECT_TRUE(contains_code(unknown_errors, "P_SCHEMA_UNKNOWN_FIELD"));

    auto wrong                                          = program_document();
    wrong["input_contract"]["schema"]                   = json::array();
    wrong["root"]["definition"]                         = "not-an-object";
    wrong["declared_budget_requirements"][0]["minimum"] = 1.5;
    const auto wrong_errors = compile_errors(snapshot, std::move(wrong));
    EXPECT_TRUE(contains_code(wrong_errors, "P_SCHEMA_TYPE"));
}

TEST(ProgramCompilerTest, AcceptsSequenceRootAndRejectsMalformedRootPlans) {
    auto snapshot = complete_snapshot();
    auto sequence = operation_document();
    sequence["root"]["op"] = "sequence";
    sequence["root"]["children"] = json::array({
        json{{"op", "call_core"}},
        json{{"op", "emit"}, {"value", json{{"kind", "done"}}}},
        json{{"op", "return"}, {"value", json{{"ok", true}}}},
    });
    std::optional<ProgramBundle> sequence_bundle;
    try {
        sequence_bundle = ProgramCompiler(snapshot, {"program-compiler-test/v1"})
                              .compile(source_from(std::move(sequence)));
    } catch (const ProgramCompileError& error) {
        for (const auto& diagnostic : error.diagnostics()) {
            ADD_FAILURE() << diagnostic.code << " " << diagnostic.primary.json_pointer << " "
                          << diagnostic.message << " witness=" << diagnostic.witness.dump();
        }
        return;
    }
    ASSERT_TRUE(sequence_bundle.has_value());
    ASSERT_EQ(sequence_bundle->orchestration_plan().plan["root"], "root");
    ASSERT_EQ(sequence_bundle->orchestration_plan().plan["operations"].size(), 4U);
    EXPECT_EQ(sequence_bundle->orchestration_plan().plan["operations"][0]["op"], "call_core");
    EXPECT_EQ(sequence_bundle->orchestration_plan().plan["operations"][1]["op"], "emit");
    EXPECT_EQ(sequence_bundle->orchestration_plan().plan["operations"][2]["op"], "return");
    auto legacy_sequence = program_document();
    legacy_sequence["root"] = json{{"op", "sequence"},
                                   {"children",
                                    json::array({json{{"op", "call_core"}},
                                                 json{{"op", "emit"}, {"value", "legacy"}}})}};
    EXPECT_TRUE(
        contains_code(compile_errors(snapshot, std::move(legacy_sequence)), "P_SCHEMA_OPERATION"));

    auto malformed = operation_document();
    malformed["root"]["op"] = "sequence";
    const auto malformed_errors = compile_errors(snapshot, std::move(malformed));
    EXPECT_TRUE(contains_code(malformed_errors, "P_SCHEMA_REQUIRED"));

    auto multiple    = program_document();
    multiple["root"] = json::array({multiple["root"], multiple["root"]});
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(multiple)), "P_SCHEMA_TYPE"));

    auto mismatch                          = program_document();
    mismatch["root"]["definition"]["name"] = "different";
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(mismatch)), "P_ROOT_NAME"));
}

TEST(ProgramCompilerTest, RejectsNonBinaryRaceDuringCompilation) {
    const auto snapshot = complete_snapshot();
    auto       document = operation_document();
    const auto definition = document["root"]["definition"];
    document["root"] = json{{"op", "race"},
                            {"name", "main"},
                            {"definition", definition},
                            {"branches",
                             json::array({json{{"op", "call_core"}},
                                          json{{"op", "return"}, {"value", 1}},
                                          json{{"op", "return"}, {"value", 2}}})}};

    const auto diagnostics = compile_errors(snapshot, std::move(document));
    EXPECT_TRUE(contains_code(diagnostics, "P_PLAN_RACE_ARITY"));
}

TEST(ProgramCompilerTest, SealsLoweredOperationsAsTypedImmutablePlanNodes) {
    auto snapshot = complete_snapshot();
    auto document = operation_document();
    auto root = json{{"op", "sequence"},
                     {"name", "main"},
                     {"definition", document["root"]["definition"]}};
    root["children"] = json::array(
        {json{{"op", "call_core"}}, json{{"op", "branch"},
                                         {"condition", json{{"path", "/route"}, {"equals", "ok"}}},
                                         {"then", json{{"op", "return"}, {"value", 1}}},
                                         {"else", json{{"op", "return"}, {"value", 0}}}}});
    document["root"] = std::move(root);

    const auto bundle = ProgramCompiler(snapshot, {"program-compiler-test/v1"})
                            .compile(source_from(std::move(document)));
    const auto& plan = bundle.typed_orchestration_plan();
    EXPECT_EQ(plan.schema_version(), ProgramPlan::SCHEMA_VERSION);
    EXPECT_EQ(plan.root_id(), "root");
    EXPECT_EQ(plan.root().operation(), ProgramOperationKind::Sequence);
    EXPECT_EQ(plan.root().dispatch().operation, ProgramOperationKind::Sequence);
    EXPECT_EQ(plan.root().dispatch().source_pointer, "/root");
    EXPECT_EQ(plan.root().dispatch().children, std::vector<std::string>({"root.0", "root.1"}));
    EXPECT_EQ(plan.root().dispatch_descriptor(), plan.root().dispatch());
    EXPECT_EQ(plan.root().source_pointer(), "/root");
    ASSERT_EQ(plan.root().children().size(), 2U);
    EXPECT_EQ(plan.root().children()[0], "root.0");
    ASSERT_NE(plan.find("root.1"), nullptr);
    EXPECT_EQ(plan.find("root.1")->operation(), ProgramOperationKind::Branch);
    EXPECT_EQ(plan.find("root.1")->dispatch().operation, ProgramOperationKind::Branch);
    EXPECT_EQ(plan.find("root.1")->dispatch().source_pointer, "/root/children/1");
    EXPECT_EQ(plan.find("root.1")->dispatch().then_id, std::optional<std::string>("root.1.0"));
    EXPECT_EQ(plan.find("root.1")->dispatch().else_id, std::optional<std::string>("root.1.1"));
    EXPECT_EQ(plan.find("root.1")->then_id(), std::optional<std::string>("root.1.0"));
    EXPECT_EQ(plan.find("root.1")->else_id(), std::optional<std::string>("root.1.1"));
    EXPECT_EQ(detail::canonical_json_bytes(plan.to_json()),
              detail::canonical_json_bytes(bundle.orchestration_plan().plan));
}

TEST(ProgramCompilerTest, AllowsExplicitChildCapacityForProgramRuntimeStartChild) {
    const auto snapshot     = complete_snapshot();
    auto       document     = program_document();
    auto       requirements = document["declared_budget_requirements"];
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        const auto resource = requirements[index]["resource"].get<std::string>();
        if (resource == "max_child_depth" || resource == "max_total_children") {
            requirements[index]["minimum"] = 1;
            requirements[index]["maximum"] = 1;
        }
    }
    document["declared_budget_requirements"] = std::move(requirements);

    EXPECT_NO_THROW((void)ProgramCompiler(snapshot, {"program-compiler-test/v1"})
                        .compile(source_from(document)));
}

TEST(ProgramCompilerTest, LowersOnlyDirectlyJoinedVerifiedChildBindingSpawns) {
    const auto snapshot = complete_snapshot();
    auto document = operation_document();
    const auto definition = document["root"]["definition"];
    document["root"]      = json{{"op", "await"},
                                 {"name", "main"},
                                 {"definition", definition},
                                 {"body", json{{"op", "spawn"}, {"child_binding", "child"}}}};
    auto requirements     = document["declared_budget_requirements"];
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        const auto resource = requirements[index]["resource"].get<std::string>();
        if (resource == "max_program_operations") {
            requirements[index]["minimum"] = 2;
            requirements[index]["maximum"] = 2;
        } else if (resource == "max_child_depth" || resource == "max_total_children") {
            requirements[index]["minimum"] = 1;
            requirements[index]["maximum"] = 1;
        }
    }
    document["declared_budget_requirements"] = std::move(requirements);

    const auto bundle =
        ProgramCompiler(snapshot, {"program-compiler-test/v1"}).compile(source_from(document));
    const auto& plan = bundle.typed_orchestration_plan();
    ASSERT_EQ(plan.root().operation(), ProgramOperationKind::Await);
    ASSERT_TRUE(plan.root().body().has_value());
    const auto* spawn = plan.find(*plan.root().body());
    ASSERT_NE(spawn, nullptr);
    EXPECT_EQ(spawn->operation(), ProgramOperationKind::Spawn);
    EXPECT_EQ(spawn->child_binding(), std::optional<std::string>("child"));
    EXPECT_FALSE(spawn->body().has_value());

    auto unjoined    = document;
    unjoined["root"] = json{
        {"op", "spawn"}, {"name", "main"}, {"definition", definition}, {"child_binding", "child"}};
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(unjoined)), "P_PLAN_SPAWN_SHAPE"));

    auto inline_spawn    = document;
    inline_spawn["root"] = json{{"op", "spawn"},
                                {"name", "main"},
                                {"definition", definition},
                                {"child_binding", "child"},
                                {"body", json{{"op", "call_core"}}}};
    EXPECT_TRUE(
        contains_code(compile_errors(snapshot, std::move(inline_spawn)), "P_PLAN_SPAWN_SHAPE"));
}

TEST(ProgramCompilerTest, LowersOnlyVersionThreeBoundedParallelMaps) {
    const auto snapshot = complete_snapshot();
    const auto document = parallel_map_document();

    const auto bundle =
        ProgramCompiler(snapshot, {"program-compiler-test/v1"}).compile(source_from(document));
    const auto& plan = bundle.typed_orchestration_plan();
    ASSERT_EQ(plan.root().operation(), ProgramOperationKind::ParallelMap);
    ASSERT_TRUE(plan.root().parallel_map().has_value());
    const auto& map = *plan.root().parallel_map();
    EXPECT_EQ(map.child_binding, "child");
    EXPECT_EQ(map.max_items, 3U);
    EXPECT_EQ(map.max_in_flight, 2U);
    EXPECT_EQ(map.failure_policy, ProgramParallelMapFailurePolicy::Collect);
    EXPECT_EQ(map.literal_items.size(), 3U);

    const auto requirements = derive_static_budget_requirements(plan);
    EXPECT_EQ(requirements.max_program_operations, 1U);
    EXPECT_EQ(requirements.max_concurrency, 2U);
    EXPECT_EQ(requirements.max_core_steps, 0U);
    EXPECT_EQ(requirements.max_child_depth, 1U);
    EXPECT_EQ(requirements.max_total_children, 3U);
    {
        SCOPED_TRACE("version-two parallel_map rejection");
        auto v2 = document;
        v2["program_schema_version"] = PROGRAM_SCHEMA_VERSION_V2;
        EXPECT_EQ(v2["program_schema_version"].get<std::uint32_t>(), PROGRAM_SCHEMA_VERSION_V2);
        EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(v2)), "P_SCHEMA_OPERATION"));
    }

    {
        SCOPED_TRACE("literal item source exceeds max_items");
        auto exceeds_items = document;
        auto root = exceeds_items["root"];
        root["max_items"] = 2;
        exceeds_items["root"] = std::move(root);
        EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(exceeds_items)),
                                  "P_PARALLEL_MAP_BOUND"));
    }

    {
        SCOPED_TRACE("max_in_flight exceeds max_items");
        auto exceeds_items = document;
        auto root = exceeds_items["root"];
        root["max_in_flight"] = 4;
        exceeds_items["root"] = std::move(root);
        ASSERT_EQ(exceeds_items["root"]["max_in_flight"].get<std::uint32_t>(), 4U);
        const SourceMapEntry bound_mapping{
            "/root/max_in_flight",
            {"dsl:test", "/map/concurrency", SourceSpan{10, 20, 1, 1, 1, 11}}};
        const auto bound_errors =
            compile_errors(snapshot, std::move(exceeds_items), {}, {bound_mapping});
        EXPECT_TRUE(std::any_of(bound_errors.begin(), bound_errors.end(), [](const auto& diagnostic) {
            return diagnostic.code == "P_PARALLEL_MAP_BOUND" &&
                   diagnostic.primary.json_pointer == "/map/concurrency";
        }));
    }

    {
        SCOPED_TRACE("ambiguous item source");
        auto ambiguous_source = document;
        auto root = ambiguous_source["root"];
        auto item_source = root["item_source"];
        item_source["artifact"] = "input";
        item_source["field"] = "/items";
        root["item_source"] = std::move(item_source);
        ambiguous_source["root"] = std::move(root);
        ASSERT_TRUE(ambiguous_source["root"]["item_source"].contains("artifact"));
        EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(ambiguous_source)),
                                  "P_PARALLEL_MAP_SOURCE"));
    }

    {
        SCOPED_TRACE("malformed input binding");
        auto bad_binding = document;
        auto root = bad_binding["root"];
        auto input_binding = root["input_binding"];
        input_binding["to"] = json::object();
        root["input_binding"] = std::move(input_binding);
        bad_binding["root"] = std::move(root);
        ASSERT_TRUE(bad_binding["root"]["input_binding"]["to"].empty());
        EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(bad_binding)),
                                  "P_PARALLEL_MAP_BINDING"));
    }

    {
        SCOPED_TRACE("malformed JSON Pointer");
        auto malformed_pointer = document;
        malformed_pointer["root"]["input_binding"]["from"]["field"] = "/items/bad~2pointer";
        const auto diagnostics = compile_errors(snapshot, std::move(malformed_pointer));
        EXPECT_TRUE(contains_code(diagnostics, "P_PARALLEL_MAP_BINDING"));
    }
}
TEST(ProgramCompilerTest, RejectsPerItemAuthorityFieldsInsteadOfInterpretingThem) {
    const auto snapshot = complete_snapshot();
    auto       document = parallel_map_document();
    document["root"]["capabilities"] = json::array({"admin"});

    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(document)),
                              "P_SCHEMA_UNKNOWN_FIELD"));
}

TEST(ProgramCompilerTest, DispatchDescriptorWalkBenchmarkExcludesCoreExecution) {
    const auto plan = ProgramPlan::from_json(
        json{{"root", "root"},
             {"operations", json::array({json{{"id", "root"},
                                              {"op", "sequence"},
                                              {"source_pointer", "/root"},
                                              {"children", json::array({"root.0", "root.1"})}},
                                         json{{"id", "root.0"},
                                              {"op", "return"},
                                              {"source_pointer", "/root/children/0"},
                                              {"value", 0}},
                                         json{{"id", "root.1"},
                                              {"op", "return"},
                                              {"source_pointer", "/root/children/1"},
                                              {"value", 1}}})}});

    constexpr std::size_t iterations = 100000;
    std::uint64_t         checksum   = 0;
    const auto            started    = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        const auto* root = plan.find("root");
        ASSERT_NE(root, nullptr);
        checksum += static_cast<std::uint8_t>(root->dispatch().operation);
        for (const auto& child_id : root->dispatch().children) {
            const auto* child = plan.find(child_id);
            ASSERT_NE(child, nullptr);
            checksum += static_cast<std::uint8_t>(child->dispatch().operation);
        }
    }
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    RecordProperty("dispatch_only_iterations", static_cast<std::int64_t>(iterations));
    RecordProperty("dispatch_only_elapsed_ns", static_cast<std::int64_t>(elapsed_ns));
    EXPECT_NE(checksum, 0U);
}

TEST(ProgramCompilerTest, TypedPlanRejectsDanglingAndUnknownOperationFields) {
    auto unknown = json{{"root", "root"}, {"operations", json::array()}};
    unknown["operations"].push_back(
        json{{"id", "root"}, {"op", "return"}, {"value", true}, {"unexpected", 1}});
    EXPECT_THROW((void)ProgramPlan::from_json(unknown), std::invalid_argument);

    auto dangling = json{{"root", "root"}, {"operations", json::array()}};
    dangling["operations"].push_back(
        json{{"id", "root"}, {"op", "sequence"}, {"children", json::array({"missing"})}});
    EXPECT_THROW((void)ProgramPlan::from_json(dangling), std::invalid_argument);

    auto bad_condition          = json{{"root", "root"}, {"operations", json::array()}};
    bad_condition["operations"] = json::array(
        {json{
             {"id", "return"}, {"op", "return"}, {"source_pointer", "/root/then"}, {"value", true}},
         json{{"id", "root"},
              {"op", "branch"},
              {"source_pointer", "/root"},
              {"condition", json{{"path", "route"}, {"equals", true}}},
              {"then", "return"}}});
    EXPECT_THROW((void)ProgramPlan::from_json(bad_condition), std::invalid_argument);
}

TEST(ProgramCompilerTest, TypedPlanRejectsParallelMapLiteralBeyondMaxItems) {
    const auto invalid = json{
        {"root", "root"},
        {"operations",
         json::array({json{{"id", "root"},
                            {"op", "parallel_map"},
                            {"source_pointer", "/root"},
                            {"item_source", json{{"literal", json::array({1, 2})}}},
                            {"child_binding", "child"},
                            {"input_binding",
                             json{{"from", json{{"field", ""}}},
                                  {"to", json{{"field", ""}}}}},
                            {"output_binding", json{{"from", json{{"field", ""}}}}},
                            {"max_items", 1},
                            {"max_in_flight", 1},
                            {"max_output_bytes", 2},
                            {"failure_policy", "fail_fast"}}})}};
    EXPECT_THROW((void)ProgramPlan::from_json(invalid), std::invalid_argument);
}

TEST(ProgramCompilerTest, RejectsMalformedConditionPointerDuringNormalization) {
    auto snapshot = complete_snapshot();
    auto document = operation_document();
    document["root"] = json{{"op", "branch"},
                            {"name", "main"},
                            {"definition", document["root"]["definition"]},
                            {"condition", json{{"path", "route~2"}, {"exists", true}}},
                            {"then", json{{"op", "call_core"}}}};

    const auto diagnostics = compile_errors(snapshot, std::move(document));
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "P_PLAN_CONDITION" &&
               diagnostic.primary.json_pointer == "/root/condition/path";
    }));
}

TEST(ProgramCompilerTest, RejectsCancelScopesTheRuntimeCannotEnforce) {
    auto snapshot = complete_snapshot();
    auto document = operation_document();
    document["root"] = json{{"op", "sequence"},
                            {"name", "main"},
                            {"definition", document["root"]["definition"]},
                            {"children", json::array({json{{"op", "cancel"}, {"scope", "branch"}},
                                                      json{{"op", "call_core"}}})}};

    const auto diagnostics = compile_errors(snapshot, std::move(document));
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "P_PLAN_CANCEL_SCOPE" &&
               diagnostic.primary.json_pointer == "/root/children/0/scope";
    }));
}

TEST(ProgramCompilerTest, RejectsMissingDuplicateAndInvalidBudgetClosure) {
    auto snapshot        = complete_snapshot();
    auto missing         = program_document();
    json missing_budgets = json::array();
    for (std::size_t index = 1; index < missing["declared_budget_requirements"].size(); ++index) {
        missing_budgets.push_back(missing["declared_budget_requirements"][index]);
    }
    missing["declared_budget_requirements"] = std::move(missing_budgets);
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(missing)), "P_BUDGET_INVALID"));

    auto duplicate = program_document();
    duplicate["declared_budget_requirements"].push_back(
        duplicate["declared_budget_requirements"][0]);
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(duplicate)), "P_BUDGET_INVALID"));

    auto invalid = program_document();
    for (std::size_t index = 0; index < invalid["declared_budget_requirements"].size(); ++index) {
        if (invalid["declared_budget_requirements"][index]["resource"] ==
            "max_program_operations") {
            invalid["declared_budget_requirements"][index]["minimum"] = 2;
            invalid["declared_budget_requirements"][index]["maximum"] = 1;
        }
    }
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(invalid)), "P_BUDGET_INVALID"));
}

TEST(ProgramCompilerTest, RejectsGlobalOnlyNodeReducerAndCondition) {
    ensure_global_poison_entries();
    reset_dispatch_counters();

    RegistrySnapshotBuilder empty;
    auto                    empty_snapshot = std::move(empty).build();
    EXPECT_FALSE(compile_errors(empty_snapshot, node_only_program("pr5-global-node")).empty());

    auto node_snapshot                                 = node_only_snapshot();
    auto reducer_definition                            = core_definition();
    reducer_definition["channels"]["value"]["reducer"] = "pr5-global-reducer";
    EXPECT_FALSE(compile_errors(node_snapshot, program_document(reducer_definition)).empty());

    auto condition_definition = node_only_program("local-node");
    condition_definition["root"]["definition"]["edges"] =
        json::array({json{{"from", "__start__"}, {"to", "work"}}});
    condition_definition["root"]["definition"]["conditional_edges"] =
        json::array({json{{"from", "work"},
                          {"condition", "pr5-global-condition"},
                          {"routes", json{{"done", "__end__"}}}}});
    EXPECT_FALSE(compile_errors(node_snapshot, std::move(condition_definition)).empty());
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, MapsCoreParseAndValidationDiagnosticsThroughEscapedSourceMapPointers) {
    const std::string escaped_node = "a/b~.[x]";
    auto              schema       = json{{"type", "object"}, {"required", json::array({"value"})}};
    auto              snapshot     = node_only_snapshot("strict-node", schema);
    auto              definition   = json{{"schema_version", 1},
                                          {"name", "main"},
                                          {"nodes", json{{escaped_node, json{{"type", "strict-node"}}}}},
                                          {"edges", json::array()},
                                          {"conditional_edges", json::array()}};
    SourceSpan        exact_span{10, 20, 2, 1, 2, 11};
    SourceMapEntry    mapping{"/root/definition/nodes/a~1b~0.[x]",
                              {"dsl:test", "/graph/node", exact_span}};
    const auto parse_errors = compile_errors(snapshot, program_document(definition), {}, {mapping});
    const auto parsed       = std::find_if(
        parse_errors.begin(), parse_errors.end(),
        [](const auto& diagnostic) { return diagnostic.phase == CompilePhase::CoreParse; });
    ASSERT_NE(parsed, parse_errors.end());
    EXPECT_EQ(parsed->primary.source_id, "dsl:test");
    EXPECT_TRUE(parsed->primary.json_pointer.starts_with("/graph/node"));
    EXPECT_FALSE(parsed->primary.span.has_value());
    EXPECT_TRUE(parsed->witness.contains("core_code"));

    auto validation_definition =
        json{{"schema_version", 1},
             {"name", "main"},
             {"nodes", json{{escaped_node, json{{"type", "strict-node"}, {"value", 1}}}}},
             {"edges", json::array({json{{"from", "__start__"}, {"to", escaped_node}},
                                    json{{"from", escaped_node}, {"to", "missing"}}})},
             {"conditional_edges", json::array()}};
    SourceMapEntry root_mapping{"/root/definition", {"dsl:test", "/graph", exact_span}};
    const auto     validation_errors =
        compile_errors(snapshot, program_document(validation_definition), {}, {root_mapping});
    const auto validated =
        std::find_if(validation_errors.begin(), validation_errors.end(),
                     [](const auto& diagnostic) { return diagnostic.code == "P_CORE_E3"; });
    ASSERT_NE(validated, validation_errors.end());
    EXPECT_TRUE(validated->primary.json_pointer.starts_with("/graph/"));
    EXPECT_FALSE(validated->primary.span.has_value());

    auto exact_document    = program_document();
    exact_document["root"] = json::array();
    SourceMapEntry exact_mapping{"/root", {"dsl:test", "/authored-root", exact_span}};
    const auto     exact_errors =
        compile_errors(snapshot, std::move(exact_document), {}, {exact_mapping});
    const auto exact =
        std::find_if(exact_errors.begin(), exact_errors.end(), [](const auto& diagnostic) {
            return diagnostic.code == "P_SCHEMA_TYPE" &&
                   diagnostic.primary.json_pointer == "/authored-root";
        });
    ASSERT_NE(exact, exact_errors.end());
    ASSERT_TRUE(exact->primary.span.has_value());
    EXPECT_EQ(*exact->primary.span, exact_span);
}

TEST(ProgramCompilerTest, InlineConditionalValidationUsesAuthoredEdgePointer) {
    auto snapshot                   = complete_snapshot();
    auto definition                 = core_definition();
    definition["edges"]             = json::array({json{{"from", "__start__"}, {"to", "work"}},
                                                   json{{"type", "conditional"},
                                                        {"from", "work"},
                                                        {"condition", "local-condition"},
                                                        {"routes", json{{"other", "__end__"}}}}});
    definition["conditional_edges"] = json::array();

    const SourceSpan     span{30, 50, 4, 1, 4, 21};
    const SourceMapEntry mapping{"/root/definition/edges/1",
                                 {"dsl:test", "/graph/inline-branch", span}};
    const auto           errors =
        compile_errors(snapshot, program_document(std::move(definition)), {}, {mapping});
    const auto diagnostic = std::find_if(
        errors.begin(), errors.end(), [](const auto& value) { return value.code == "P_CORE_E10"; });

    ASSERT_NE(diagnostic, errors.end());
    EXPECT_EQ(diagnostic->primary.source_id, "dsl:test");
    EXPECT_EQ(diagnostic->primary.json_pointer, "/graph/inline-branch");
    ASSERT_TRUE(diagnostic->primary.span.has_value());
    EXPECT_EQ(*diagnostic->primary.span, span);
}

TEST(ProgramCompilerTest, RejectsInvalidInlineConditionalTypeWithoutDispatch) {
    auto snapshot       = complete_snapshot();
    auto definition     = core_definition();
    definition["edges"] = json::array(
        {json{{"from", "__start__"}, {"to", "work"}}, json{{"type", "branch"},
                                                           {"from", "work"},
                                                           {"condition", "local-condition"},
                                                           {"routes", json{{"done", "__end__"}}}}});
    definition["conditional_edges"] = json::array();

    reset_dispatch_counters();
    const auto errors = compile_errors(snapshot, program_document(std::move(definition)));
    EXPECT_TRUE(contains_code(errors, "P_CORE_GC_FIELD_VALUE"));
    EXPECT_FALSE(contains_code(errors, "P_COMPILER_INTERNAL"));
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, RejectsUnsealableCoreValuesAsTypedFieldDiagnostics) {
    const auto        snapshot = complete_snapshot();
    std::vector<json> documents;
    {
        auto document                               = program_document();
        document["root"]["definition"]["nodes"][""] = json{{"type", "local-node"}};
        documents.push_back(std::move(document));
    }
    {
        auto document                                       = program_document();
        document["root"]["definition"]["nodes"]["work"][""] = 1;
        documents.push_back(std::move(document));
    }
    {
        auto document                                  = program_document();
        document["root"]["definition"]["retry_policy"] = json{{"max_retries", -1}};
        documents.push_back(std::move(document));
    }
    {
        auto document                                  = program_document();
        document["root"]["definition"]["retry_policy"] = json{{"backoff_multiplier", 1.0e300}};
        documents.push_back(std::move(document));
    }

    for (auto& document : documents) {
        reset_dispatch_counters();
        const auto errors = compile_errors(snapshot, std::move(document));
        EXPECT_TRUE(contains_code(errors, "P_CORE_GC_FIELD_VALUE"));
        EXPECT_FALSE(contains_code(errors, "P_COMPILER_INTERNAL"));
        expect_dispatch_counters_zero();
    }
}

TEST(ProgramCompilerTest, WarningsAreStoredAndErrorsPreventBundleConstruction) {
    auto snapshot                                             = node_only_snapshot();
    auto warning_document                                     = node_only_program("local-node");
    warning_document["root"]["definition"]["nodes"]["orphan"] = json{{"type", "local-node"}};
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      warning_bundle  = compiler.compile(source_from(std::move(warning_document)));
    const auto      stored_warnings = warning_bundle.diagnostics();
    EXPECT_TRUE(std::any_of(
        stored_warnings.begin(), stored_warnings.end(),
        [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Warning; }));

    auto error_document                                    = node_only_program("local-node");
    error_document["root"]["definition"]["edges"][1]["to"] = "missing";
    const auto errors = compile_errors(snapshot, std::move(error_document));
    EXPECT_TRUE(contains_code(errors, "P_CORE_E3"));
}

TEST(ProgramCompilerTest, ComputesTransitiveExecutableCapabilityEffectAndTrustedNativeClosure) {
    reset_dispatch_counters();
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      bundle = compiler.compile(source_from(program_document()));

    EXPECT_EQ(bundle.executable_registry_identities().size(), 6U);
    EXPECT_NE(std::find_if(bundle.executable_registry_identities().begin(),
                           bundle.executable_registry_identities().end(),
                           [](const auto& identity) {
                               return identity.kind == ExecutableKind::Imported &&
                                      identity.name == "local-import";
                           }),
              bundle.executable_registry_identities().end());
    const auto& closure = bundle.capability_effect_closure();
    EXPECT_NE(std::find(closure.capabilities.begin(), closure.capabilities.end(),
                        TRUSTED_NATIVE_CAPABILITY),
              closure.capabilities.end());
    EXPECT_NE(
        std::find(closure.capabilities.begin(), closure.capabilities.end(), "provider-capability"),
        closure.capabilities.end());
    EXPECT_NE(std::find(closure.effects.begin(), closure.effects.end(), "tool-effect"),
              closure.effects.end());
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, RejectsMissingOrMismatchedManifestDependencies) {
    const ExecutableIdentity missing{ExecutableKind::Tool, "missing", "1.0.0", digest('a')};
    RegistrySnapshotBuilder  missing_builder;
    missing_builder.add_node(
        executable(ExecutableKind::Node, "node", 'b', EffectMode::Brokered, {}, {}, {missing}),
        quiet_factory(), json::object(), json::object());
    EXPECT_THROW((void)std::move(missing_builder).build(), std::invalid_argument);

    const ExecutableIdentity expected{ExecutableKind::Tool, "tool", "1.0.0", digest('c')};
    RegistrySnapshotBuilder  mismatch_builder;
    mismatch_builder.add_tool(executable(ExecutableKind::Tool, "tool", 'd'),
                              ToolMetadata{json::object(), json::object()});
    mismatch_builder.add_node(
        executable(ExecutableKind::Node, "node", 'e', EffectMode::Brokered, {}, {}, {expected}),
        quiet_factory(), json::object(), json::object());
    EXPECT_THROW((void)std::move(mismatch_builder).build(), std::invalid_argument);
}

TEST(ProgramCompilerTest, ImportMerkleRootIsOrderIndependentAndRejectsDuplicateSourceIds) {
    auto                         snapshot = complete_snapshot();
    ProgramCompiler              compiler(snapshot, {"program-compiler-test/v1"});
    const std::vector<ImportRef> ordered{
        {"a", digest('a')}, {"b", digest('b')}, {"c", digest('c')}};
    const std::vector<ImportRef> reversed{
        {"c", digest('c')}, {"b", digest('b')}, {"a", digest('a')}};
    const auto first  = compiler.compile(source_from(program_document(), ordered));
    const auto second = compiler.compile(source_from(program_document(), reversed));
    EXPECT_EQ(first.module_dependency_merkle_root(), second.module_dependency_merkle_root());
    EXPECT_EQ(first.id(), second.id());

    std::vector<std::string> leaves;
    for (const auto& import_ref : ordered) {
        const json leaf{{"source_id", import_ref.source_id},
                        {"content_identity", import_ref.content_identity}};
        leaves.push_back(neograph::program::detail::sha256_identity(
            "program-import-leaf/v1", neograph::program::detail::canonical_json_bytes(leaf)));
    }
    const auto first_pair =
        neograph::program::detail::sha256_identity("program-import-node/v1", leaves[0] + leaves[1]);
    const auto duplicated_odd =
        neograph::program::detail::sha256_identity("program-import-node/v1", leaves[2] + leaves[2]);
    const auto expected = neograph::program::detail::sha256_identity("program-import-node/v1",
                                                                     first_pair + duplicated_odd);
    EXPECT_EQ(first.module_dependency_merkle_root(), expected);

    EXPECT_TRUE(contains_code(
        compile_errors(snapshot, program_document(), {{"a", digest('a')}, {"a", digest('a')}}),
        "P_IMPORT_CONFLICT"));
    EXPECT_TRUE(contains_code(
        compile_errors(snapshot, program_document(), {{"a", digest('a')}, {"a", digest('b')}}),
        "P_IMPORT_CONFLICT"));
}

TEST(ProgramCompilerTest, VerifiedModuleResolutionCarriesCoordinatesIntoBundle) {
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});

    ProgramModuleData module_data;
    module_data.owner_scope    = "tenant:compiler";
    module_data.coordinate     = ModuleCoordinate{"test", "verified", "1.0.0", ""};
    module_data.attestation_id = "attestation:test";
    const auto module          = ProgramModule::create(std::move(module_data));

    ModuleResolution resolution;
    resolution.root = module.coordinate();
    resolution.modules.push_back(module);
    resolution.receipts.push_back({module.coordinate().qualified_name(), module.id()});
    const auto source =
        source_from(program_document(), {{module.coordinate().qualified_name(), module.id()}});

    const auto bundle = compiler.compile(source, resolution);
    ASSERT_EQ(bundle.module_coordinates(), std::vector<ModuleCoordinate>{module.coordinate()});
    EXPECT_EQ(ProgramBundle::parse(bundle.serialize_canonical()).module_coordinates(),
              bundle.module_coordinates());

    auto unpinned = source_from(program_document(), {{"test:other@1.0.0", digest('f')}});
    EXPECT_THROW((void)compiler.compile(unpinned, resolution), ProgramCompileError);
}

TEST(ProgramCompilerTest, CompilesOneRootWithoutRuntimeObjectsOrCallableDispatch) {
    static_assert(!std::is_copy_constructible_v<ProgramCompiler>);
    static_assert(std::is_move_constructible_v<ProgramCompiler>);
    static_assert(std::is_same_v<decltype(std::declval<const ProgramCompiler&>().compile(
                                     std::declval<const ProgramSource&>())),
                                 ProgramBundle>);
    reset_dispatch_counters();
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      bundle = compiler.compile(source_from(program_document()));
    EXPECT_EQ(bundle.executable_registry_identities().size(), 6U);
    expect_dispatch_counters_zero();
}

TEST(ProgramCompilerTest, EveryFailurePathLeavesDispatchCountersZero) {
    ensure_global_poison_entries();
    auto                               snapshot = complete_snapshot();
    std::vector<std::function<bool()>> rejection_paths;

    rejection_paths.push_back([&] {
        auto document              = program_document();
        document["input_contract"] = nullptr;
        return !compile_errors(snapshot, std::move(document)).empty();
    });
    rejection_paths.push_back([&] {
        RegistrySnapshotBuilder empty;
        auto                    local = std::move(empty).build();
        return !compile_errors(local, node_only_program("pr5-global-node")).empty();
    });
    rejection_paths.push_back([&] {
        auto local    = node_only_snapshot();
        auto document = node_only_program("local-node");
        document["root"]["definition"]["channels"] =
            json{{"value", json{{"reducer", "pr5-global-reducer"}}}};
        return !compile_errors(local, std::move(document)).empty();
    });
    rejection_paths.push_back([&] {
        auto local    = node_only_snapshot();
        auto document = node_only_program("local-node");
        document["root"]["definition"]["edges"] =
            json::array({json{{"from", "__start__"}, {"to", "work"}}});
        document["root"]["definition"]["conditional_edges"] =
            json::array({json{{"from", "work"},
                              {"condition", "pr5-global-condition"},
                              {"routes", json{{"done", "__end__"}}}}});
        return !compile_errors(local, std::move(document)).empty();
    });
    rejection_paths.push_back([&] {
        auto document                           = program_document();
        document["root"]["definition"]["edges"] = "malformed";
        return !compile_errors(snapshot, std::move(document)).empty();
    });
    rejection_paths.push_back([&] {
        const auto report = neograph::program::detail::RegistrySnapshotAccess::parse_local_report(
            snapshot, core_definition());
        if (!report.topology) return false;
        auto mutated = *report.topology;
        mutated.name = "roundtrip-seam-mutation";
        return neograph::program::detail::RegistrySnapshotAccess::verify_roundtrip_report(
                   core_definition(), mutated)
            .has_errors();
    });
    rejection_paths.push_back([&] {
        auto document                                    = program_document();
        document["root"]["definition"]["edges"][0]["to"] = "missing";
        return !compile_errors(snapshot, std::move(document)).empty();
    });
    rejection_paths.push_back([&] {
        RegistrySnapshotBuilder builder;
        builder.add_node(executable(ExecutableKind::Node, "node", 'a', EffectMode::Brokered, {}, {},
                                    {{ExecutableKind::Tool, "missing", "1.0.0", digest('b')}}),
                         quiet_factory(), json::object(), json::object());
        try {
            (void)std::move(builder).build();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    });
    rejection_paths.push_back([&] {
        auto document = program_document();
        for (std::size_t index = 0; index < document["declared_budget_requirements"].size();
             ++index) {
            if (document["declared_budget_requirements"][index]["resource"] == "max_child_depth")
                document["declared_budget_requirements"][index]["maximum"] = 1;
        }
        return !compile_errors(snapshot, std::move(document)).empty();
    });

    for (auto& reject : rejection_paths) {
        reset_dispatch_counters();
        EXPECT_TRUE(reject());
        expect_dispatch_counters_zero();
    }
}

TEST(ProgramCompilerTest, ProgramCompileErrorOwnsNestedWitnessJson) {
    json nested = json{{"outer", json{{"items", json::array({json{{"value", 7}}})}}}};
    neograph::program::Diagnostic diagnostic;
    diagnostic.code    = "P_SCHEMA_TYPE";
    diagnostic.witness = nested;
    ProgramCompileError error(std::vector<neograph::program::Diagnostic>{diagnostic});
    nested["outer"]["items"][0]["value"]             = 99;
    diagnostic.witness["outer"]["items"][0]["value"] = 101;

    ASSERT_EQ(error.diagnostics().size(), 1U);
    EXPECT_EQ(error.diagnostics()[0].witness["outer"]["items"][0]["value"], 7);
}

TEST(ProgramCompilerTest, ParsedUntrustedBundleCannotBypassRecomputationContract) {
    auto            snapshot = complete_snapshot();
    ProgramCompiler compiler(snapshot, {"program-compiler-test/v1"});
    const auto      compiled           = compiler.compile(source_from(program_document()));
    const auto      trusted_definition = compiled.sealed_core_definitions()[0];
    const auto      recomputed         = core_compiled_plan_identity(
        trusted_definition, compiled.compiler_build_id(), compiled.registry_snapshot_fingerprint(),
        compiled.executable_registry_identities());
    ASSERT_EQ(recomputed, compiled.core_plan_identities()[0].compiled_plan_identity);

    ProgramBundleData forged;
    forged.source_kind                    = compiled.source_kind();
    forged.source_hash                    = compiled.source_hash();
    forged.canonical_program_hash         = compiled.canonical_program_hash();
    forged.compiler_build_id              = compiled.compiler_build_id();
    forged.program_schema_version         = compiled.program_schema_version();
    forged.registry_snapshot_fingerprint  = compiled.registry_snapshot_fingerprint();
    forged.module_dependency_merkle_root  = compiled.module_dependency_merkle_root();
    forged.input_contract                 = compiled.input_contract();
    forged.output_contract                = compiled.output_contract();
    forged.orchestration_plan             = compiled.orchestration_plan();
    forged.sealed_core_definitions        = compiled.sealed_core_definitions();
    forged.core_plan_identities           = {{"main", digest('f')}};
    forged.capability_effect_closure      = compiled.capability_effect_closure();
    forged.executable_registry_identities = compiled.executable_registry_identities();
    forged.declared_budget_requirements   = compiled.declared_budget_requirements();
    forged.source_map                     = compiled.source_map();
    forged.diagnostics                    = compiled.diagnostics();

    const auto parsed_untrusted =
        ProgramBundle::parse(ProgramBundle(std::move(forged)).serialize_canonical());
    const auto required_recomputation = core_compiled_plan_identity(
        parsed_untrusted.sealed_core_definitions()[0], parsed_untrusted.compiler_build_id(),
        parsed_untrusted.registry_snapshot_fingerprint(),
        parsed_untrusted.executable_registry_identities());
    EXPECT_NE(parsed_untrusted.core_plan_identities()[0].compiled_plan_identity,
              required_recomputation);
}
