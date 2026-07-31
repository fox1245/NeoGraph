#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/program/program.h>

#include "canonical_json.h"
#include "registry_access.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
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

ProgramSource source_from(json                        document,
                          std::vector<ImportRef>      imports    = {},
                          std::vector<SourceMapEntry> source_map = {},
                          std::string                 source_id  = "test:program") {
    return ProgramSource::from_cpp_builder(std::move(source_id), 1, std::move(document),
                                           std::move(imports), std::move(source_map));
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
        ReducerRegistry::instance().register_reducer(
            "pr5-global-reducer", [](const json&, const json& incoming) { return json(incoming); });
        ConditionRegistry::instance().register_condition(
            "pr5-global-condition", [](const GraphState&) { return std::string("done"); },
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
    auto            compact  = ProgramSource::from_canonical_json("format:test", document.dump());
    SourceMapEntry mapped{"/root/definition", {"dsl:test", "/graph", SourceSpan{0, 4, 1, 1, 1, 5}}};
    auto           formatted =
        ProgramSource::from_canonical_json("format:test", document.dump(2), {}, {mapped});

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

TEST(ProgramCompilerTest, RejectsNonCallCoreMultipleRootAndNameMismatch) {
    auto snapshot          = complete_snapshot();
    auto non_core          = program_document();
    non_core["root"]["op"] = "sequence";
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(non_core)), "P_ROOT_OPERATION"));

    auto multiple    = program_document();
    multiple["root"] = json::array({multiple["root"], multiple["root"]});
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(multiple)), "P_SCHEMA_TYPE"));

    auto mismatch                          = program_document();
    mismatch["root"]["definition"]["name"] = "different";
    EXPECT_TRUE(contains_code(compile_errors(snapshot, std::move(mismatch)), "P_ROOT_NAME"));
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
        auto budget = invalid["declared_budget_requirements"][index];
        if (budget["resource"] == "max_program_operations") budget["maximum"] = 2;
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
            auto budget = document["declared_budget_requirements"][index];
            if (budget["resource"] == "max_child_depth") budget["maximum"] = 1;
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
