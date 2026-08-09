#include <neograph/graph/loader.h>
#include <neograph/graph/node.h>
#include <neograph/mcp/harness_program_translator.h>
#include <neograph/program/compiler.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::mcp;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

ExecutableManifest manifest(ExecutableKind           kind,
                            std::string              name,
                            char                     implementation,
                            std::vector<std::string> capabilities = {},
                            std::vector<std::string> effects      = {}) {
    return {{kind, std::move(name), "1.0.0", digest(implementation)},
            EffectMode::Brokered,
            "test:harness-program-translator",
            std::move(capabilities),
            std::move(effects),
            {}};
}

class NeverRunNode final : public GraphNode {
public:
    explicit NeverRunNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override { co_return NodeOutput{}; }
    std::string                 get_name() const override { return name_; }

private:
    std::string name_;
};

NodeFactoryFn counted_factory(std::atomic<int>& calls) {
    return [&calls](const std::string& name, const json&, const NodeContext&) {
        ++calls;
        return std::make_unique<NeverRunNode>(name);
    };
}

json worker_schema() {
    return {{"type", "object"}, {"additionalProperties", true}};
}

BudgetLimits ceiling() {
    return {86400000, 1000000000, 1000000000, 64, 1, 1000, 1, 1, 1};
}

struct FixtureHost {
    HarnessProgramSnapshots    snapshots;
    HarnessTranslationDefaults defaults;
};

FixtureHost host(std::atomic<int>&        factory_calls,
                 std::vector<std::string> tool_effects = {"filesystem.read"}) {
    const auto provider = manifest(ExecutableKind::Provider, "provider.complete", '3',
                                   {"provider.complete"}, {"model.invoke"});
    HarnessProgramSnapshotConfig config;
    config.registry.worker = {
        manifest(ExecutableKind::Node, std::string(HARNESS_WORKER_NODE_TYPE), '1'),
        counted_factory(factory_calls),
        worker_schema(),
        {{"reads", json::array({"task"})}, {"writes", json::array({"worker_results"})}},
        {}};
    config.registry.judge = {
        manifest(ExecutableKind::Node, std::string(HARNESS_JUDGE_NODE_TYPE), '2'),
        counted_factory(factory_calls),
        worker_schema(),
        {{"reads", json::array({"worker_results"})},
         {"writes", json::array({"final_result"})},
         {"exports", json::array({"final_result"})}},
        {}};
    config.registry.provider =
        HarnessProviderRegistration{provider, {{{"type", "object"}}, {{"type", "object"}}}};
    config.registry.tools.push_back(
        {manifest(ExecutableKind::Tool, "repo.access", '4', {"tool.invoke"}, tool_effects),
         {{{"type", "object"}, {"additionalProperties", false}},
          {{"type", "object"}, {"additionalProperties", true}}}});
    config.registry.conditions.push_back({manifest(ExecutableKind::Condition, "always", '5'),
                                          [](const GraphState&) { return std::string("yes"); },
                                          ConditionSpec{{"yes"}, false}});
    config.owner_scope          = "owner:test";
    config.allowed_capabilities = {"provider.complete", "tool.invoke"};
    config.allowed_effects      = {"model.invoke", "filesystem.read", "filesystem.write"};
    config.budget_ceiling       = ceiling();

    HarnessTranslationDefaults defaults;
    defaults.provider                      = provider.identity;
    defaults.read_only_effects             = {"filesystem.read"};
    defaults.timeout_seconds               = 5;
    defaults.max_core_steps                = 10;
    defaults.max_parallel_workers          = 2;
    defaults.max_worker_retries            = 1;
    defaults.provider_timeout_seconds      = 30;
    defaults.max_output_tokens             = 100;
    defaults.input_token_ceiling_per_round = 200;
    defaults.max_provider_tool_rounds      = 2;
    defaults.monetary_microunits           = 1000;
    return {build_harness_program_snapshots(std::move(config)), std::move(defaults)};
}

json worker(std::vector<std::string> tools = {}) {
    return {
        {"id", "reviewer"},
        {"instructions", "Return structured findings"},
        {"tools", std::move(tools)},
        {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
    };
}

json request() {
    return {
        {"task",
         {{"objective", "Review the change"}, {"acceptance", json::array({"Use evidence"})}}},
        {"harness", {{"mode", "preset"}, {"preset", "fanout_judge"}}},
        {"workers", json::array({worker()})},
        {"tool_catalog", json::array()},
        {"budgets",
         {{"max_steps", 10},
          {"timeout_seconds", 5},
          {"max_parallel_workers", 2},
          {"max_worker_retries", 1},
          {"provider_timeout_seconds", 30},
          {"max_output_tokens", 100}}},
        {"policy",
         {{"read_only", true}, {"evidence_required", json::array({"file", "line", "evidence"})}}},
    };
}

ContractManifest frozen_contract(std::string owner_scope, json expected = json::object()) {
    ContractManifestSpec spec;
    spec.manifest_id  = "harness-contract";
    spec.owner_scope  = std::move(owner_scope);
    spec.scope        = "harness-execution";
    spec.requirements = {{"execute", "Execute the retained Harness Program"}};
    spec.acceptance   = {
        {"runtime", "ProgramRuntime returns the expected outcome", true, std::move(expected)}};
    spec.retry_policy = ContractRetryPolicy{2, 10, 5000};
    return ContractManifest::propose(std::move(spec))
        .review(ContractReview{"reviewer", "approved", true})
        .freeze();
}
json without_field(const json& value, std::string_view omitted) {
    json result = json::object();
    for (const auto& [name, item] : value.items()) {
        if (name != omitted) result[name] = item;
    }
    return result;
}

json tool_definition() {
    return {
        {"id", "repo.access"},
        {"description", "Read repository data"},
        {"input_schema", {{"type", "object"}, {"additionalProperties", false}}},
        {"output_schema", {{"type", "object"}, {"additionalProperties", true}}},
        {"read_only", true},
        {"executor",
         {{"kind", "mcp"},
          {"server_ref", "server-a"},
          {"tool", "read"},
          {"url", "https://example.invalid"},
          {"auth", "secret"},
          {"session_id", "session-a"},
          {"process", "worker-a"}}},
    };
}

bool contains_forbidden_transport_key(const json& value) {
    static const std::vector<std::string> keys = {"executor", "server_ref", "agent",  "url",
                                                  "auth",     "session_id", "process"};
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) return true;
            if (contains_forbidden_transport_key(child)) return true;
        }
    } else if (value.is_array()) {
        for (const auto& child : value)
            if (contains_forbidden_transport_key(child)) return true;
    }
    return false;
}

TEST(HarnessProgramTranslator, PresetAndJavaScriptAreTheOnlyPublicationFrontends) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Reducer, "overwrite"));
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Reducer, "append"));
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Condition, "always"));
    const auto schema_modes =
        harness_program_request_schema().at("properties").at("harness").at("properties");
    EXPECT_EQ(schema_modes.at("mode").at("enum"), json::array({"preset", "javascript"}));
    EXPECT_FALSE(schema_modes.contains("definition"));

    auto preset = HarnessRequestTranslator::translate(request(), fixture.snapshots.registry,
                                                      fixture.defaults);
    EXPECT_EQ(preset.source.kind(), SourceKind::CppBuilder);
    EXPECT_EQ(preset.wire.authoring_frontend, AuthoringFrontend::TrustedCpp);
    EXPECT_EQ(preset.wire.mode, "preset");

    auto javascript_request       = request();
    javascript_request["harness"] = {
        {"mode", "javascript"},
        {"source_id", "harness:test/control.js"},
        {"source", "export function define() { return ng.graph('main'); }"},
    };
    auto javascript = HarnessRequestTranslator::translate(
        javascript_request, fixture.snapshots.registry, fixture.defaults);
    EXPECT_EQ(javascript.source.kind(), SourceKind::JavaScript);
    EXPECT_EQ(javascript.wire.authoring_frontend, AuthoringFrontend::JavaScript);
    EXPECT_EQ(javascript.wire.mode, "javascript");
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, TransportAndCredentialFieldsRemainHostOnly) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    auto             value   = request();
    value["workers"][0]      = worker({"repo.access"});
    value["tool_catalog"]    = json::array({tool_definition()});

    auto translated =
        HarnessRequestTranslator::translate(value, fixture.snapshots.registry, fixture.defaults);
    EXPECT_FALSE(contains_forbidden_transport_key(translated.source.document()));
    ASSERT_EQ(translated.bindings.tools.size(), 1u);
    EXPECT_EQ(translated.bindings.tools[0].host_configuration["id"], "repo.access");
    EXPECT_EQ(translated.bindings.tools[0].host_configuration["executor"]["server_ref"],
              "server-a");
    EXPECT_EQ(translated.bindings.tools[0].host_configuration["executor"]["auth"], "secret");
    ProgramCompiler compiler(fixture.snapshots.registry,
                             ProgramCompilerConfig{"test:harness-translator-binding"});
    const auto      bundle     = compiler.compile(translated.source);
    const auto      identities = bundle.executable_registry_identities();
    EXPECT_NE(std::find(identities.begin(), identities.end(), *fixture.defaults.provider),
              identities.end());
    EXPECT_NE(
        std::find(identities.begin(), identities.end(), translated.bindings.tools[0].executable),
        identities.end());
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, EmitsNineExactFiniteTotalBudgetsAndRejectsUnboundedOrOverflow) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    auto translated    = HarnessRequestTranslator::translate(request(), fixture.snapshots.registry,
                                                             fixture.defaults);
    const auto budgets = translated.source.document()["declared_budget_requirements"];
    ASSERT_EQ(budgets.size(), 9u);
    for (const auto& budget : budgets) {
        EXPECT_TRUE(budget["minimum"].is_number_unsigned() ||
                    budget["minimum"].is_number_integer());
        EXPECT_EQ(budget["minimum"], budget["maximum"]);
    }
    EXPECT_EQ(translated.invocation_template.budget.wall_time_ms, 5000u);
    EXPECT_EQ(translated.invocation_template.budget.model_tokens, 1800u);
    EXPECT_EQ(translated.invocation_template.budget.max_concurrency, 1u);
    EXPECT_EQ(translated.invocation_template.budget.max_program_operations, 1u);
    EXPECT_EQ(translated.invocation_template.budget.max_dynamic_compiles, 0u);
    EXPECT_EQ(translated.invocation_template.budget.max_child_depth, 0u);
    EXPECT_EQ(translated.invocation_template.budget.max_total_children, 0u);

    auto unbounded              = fixture.defaults;
    unbounded.max_output_tokens = 0;
    EXPECT_THROW(
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, unbounded),
        HarnessTranslationError);

    auto overflow                          = fixture.defaults;
    overflow.input_token_ceiling_per_round = std::numeric_limits<std::uint64_t>::max() - 50;
    EXPECT_THROW(
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, overflow),
        HarnessTranslationError);
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, RejectionsNeverDispatchFactoriesOrUseGlobalFallback) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);

    auto malformed = without_field(request(), "task");
    EXPECT_THROW(HarnessRequestTranslator::translate(malformed, fixture.snapshots.registry,
                                                     fixture.defaults),
                 HarnessTranslationError);

    auto missing_tool          = request();
    missing_tool["workers"][0] = worker({"not.registered"});
    EXPECT_THROW(HarnessRequestTranslator::translate(missing_tool, fixture.snapshots.registry,
                                                     fixture.defaults),
                 HarnessTranslationError);

    auto write_fixture                = host(calls, {"filesystem.write"});
    auto undeclared_effect            = request();
    undeclared_effect["workers"][0]   = worker({"repo.access"});
    undeclared_effect["tool_catalog"] = json::array({tool_definition()});
    EXPECT_THROW(HarnessRequestTranslator::translate(
                     undeclared_effect, write_fixture.snapshots.registry, write_fixture.defaults),
                 HarnessTranslationError);

    NodeFactory::instance().register_type("translator_global_only", counted_factory(calls),
                                          {{"type", "object"}, {"additionalProperties", false}},
                                          json::object());
    auto global_request       = request();
    global_request["harness"] = {
        {"mode", "javascript"},
        {"source",
         R"JS(export function define() {
    const graph = ng.graph("global_only");
    graph.node("work", "translator_global_only", {});
    graph.entry("work");
    graph.exit("work");
    return graph;
})JS"},
    };
    auto translated = HarnessRequestTranslator::translate(
        global_request, fixture.snapshots.registry, fixture.defaults);
    ProgramCompiler compiler(fixture.snapshots.registry,
                             ProgramCompilerConfig{"test:harness-translator"});
    EXPECT_THROW((void)compiler.compile(translated.source), ProgramCompileError);
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, RequiresFrozenContractAndCarriesItToHarnessProjection) {
    std::atomic<int> calls{0};
    auto             fixture      = host(calls);
    auto             proposed     = ContractManifest::propose(ContractManifestSpec{
        1,
        "unfrozen-harness-contract",
        "owner:test",
        "harness-execution",
                        {},
                        {{"execute", "Execute the retained Harness Program"}},
                        {},
                        {{"runtime", "ProgramRuntime returns the expected outcome", true, json::object()}},
                        {},
                        {},
                        {},
                        {},
        ContractRetryPolicy{1, 1, 1000}});
    auto             rejected     = request();
    rejected["contract_manifest"] = json::parse(proposed.serialize_canonical());
    EXPECT_THROW(
        HarnessRequestTranslator::translate(rejected, fixture.snapshots.registry, fixture.defaults),
        HarnessTranslationError);

    auto       frozen   = request();
    const auto manifest = frozen_contract("owner:test", json{{"outcome", "zero_findings"}});
    frozen["contract"]  = json::parse(manifest.serialize_canonical());
    frozen["workspace_revision"] = "workspace-test-1";
    const auto translated =
        HarnessRequestTranslator::translate(frozen, fixture.snapshots.registry, fixture.defaults);
    ASSERT_TRUE(translated.contract.has_value());
    EXPECT_EQ(translated.contract->content_hash(), manifest.content_hash());
    EXPECT_EQ(translated.wire.workspace_revision, "workspace-test-1");
    EXPECT_EQ(translated.wire.projection["contract_manifest_hash"], manifest.content_hash());
    EXPECT_EQ(translated.invocation_template.budget.max_program_operations, 10U);
    EXPECT_EQ(translated.invocation_template.budget.wall_time_ms, 5000U);
}

TEST(HarnessProgramTranslator, RejectsLegacyAuthoringWithStableMigrationDiagnostics) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    for (const auto mode : {"dsl", "core", "program", "program_json"}) {
        auto value       = request();
        value["harness"] = {{"mode", mode}, {"definition", json::object()}};
        try {
            (void)HarnessRequestTranslator::translate(value, fixture.snapshots.registry,
                                                      fixture.defaults);
            FAIL() << "legacy mode unexpectedly accepted: " << mode;
        } catch (const HarnessTranslationError& error) {
            const auto expected =
                std::string(std::string_view(mode) == "dsl"    ? "H_MIGRATION_CORE_DSL"
                            : std::string_view(mode) == "core" ? "H_MIGRATION_CORE_JSON"
                                                               : "H_MIGRATION_PROGRAM_JSON");
            EXPECT_EQ(error.code(), expected);
            EXPECT_EQ(error.pointer(), "/harness/mode");
        }
    }
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, JavaScriptModeRequiresExplicitSourceEnvelope) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    auto             value   = request();
    value["harness"]         = {
        {"mode", "javascript"},
        {"source_id", "harness:direct.js"},
        {"source", "export function define() { return ng.graph('harness_fanout_judge'); }"},
    };

    const auto translated =
        HarnessRequestTranslator::translate(value, fixture.snapshots.registry, fixture.defaults);
    EXPECT_EQ(translated.source.kind(), SourceKind::JavaScript);
    EXPECT_EQ(translated.source.document().at("language"), "javascript");
    EXPECT_EQ(translated.source.document().at("source"), value["harness"]["source"]);
    EXPECT_EQ(translated.wire.authoring_frontend, AuthoringFrontend::JavaScript);
    EXPECT_EQ(translated.wire.mode, "javascript");
    EXPECT_EQ(calls.load(), 0);

    auto missing_mode       = value;
    missing_mode["harness"] = {{"definition", json::object()}};
    try {
        (void)HarnessRequestTranslator::translate(missing_mode, fixture.snapshots.registry,
                                                  fixture.defaults);
        FAIL() << "missing authoring mode unexpectedly accepted";
    } catch (const HarnessTranslationError& error) {
        EXPECT_EQ(error.code(), "H_AUTHORING_MODE_REQUIRED");
        EXPECT_EQ(error.pointer(), "/harness/mode");
    }
}

}  // namespace
