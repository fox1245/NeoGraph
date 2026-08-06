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
    spec.manifest_id = "harness-contract";
    spec.owner_scope = std::move(owner_scope);
    spec.scope       = "harness-execution";
    spec.requirements = {{"execute", "Execute the retained Harness Program"}};
    spec.acceptance   = {{"runtime", "ProgramRuntime returns the expected outcome", true,
                          std::move(expected)}};
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

json direct_dsl(const json& sealed_worker) {
    return {
        {"schema_version", 1},
        {"name", "harness_fanout_judge"},
        {"channels",
         {{"task", {{"reducer", "overwrite"}, {"initial", json::object()}}},
          {"worker_results", {{"reducer", "append"}, {"initial", json::array()}}},
          {"final_result", {{"reducer", "overwrite"}, {"initial", nullptr}}}}},
        {"nodes",
         {{"worker_0",
           {{"type", std::string(HARNESS_WORKER_NODE_TYPE)},
            {"worker_id", sealed_worker.at("worker_id")}}},
          {"judge",
           {{"type", std::string(HARNESS_JUDGE_NODE_TYPE)},
            {"barrier", {{"wait_for", json::array({"worker_0"})}}}}}}},
        {"edges", json::array({
                      {{"from", "__start__"}, {"to", "worker_0"}},
                      {{"from", "worker_0"}, {"to", "judge"}},
                      {{"from", "judge"}, {"to", "__end__"}},
                  })},
    };
}

json program_root(const json& core, json root) {
    root["name"]       = core.at("name");
    root["definition"] = core;
    return root;
}

json program_request(const json& core, json root) {
    auto value         = request();
    value["harness"] = {{"mode", "program"},
                        {"definition", program_root(core, std::move(root))}};
    return value;
}

const SourceMapEntry* find_source_map_entry(const ProgramBundle& bundle,
                                            std::string_view     generated_pointer) {
    const auto found = std::find_if(
        bundle.source_map().begin(), bundle.source_map().end(), [&](const auto& entry) {
            return entry.generated_pointer == generated_pointer;
        });
    return found == bundle.source_map().end() ? nullptr : &*found;
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

TEST(HarnessProgramTranslator, PresetDslAndCoreProduceEquivalentProgramDocuments) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Reducer, "overwrite"));
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Reducer, "append"));
    EXPECT_TRUE(fixture.snapshots.registry.find(ExecutableKind::Condition, "always"));
    auto preset_request = request();
    auto preset = HarnessRequestTranslator::translate(preset_request, fixture.snapshots.registry,
                                                      fixture.defaults);
    const auto preset_document = preset.source.document();
    const auto sealed_worker   = preset_document["root"]["definition"]["nodes"]["worker_0"];

    auto dsl_request       = preset_request;
    dsl_request["harness"] = {{"mode", "dsl"}, {"definition", direct_dsl(sealed_worker)}};
    auto dsl = HarnessRequestTranslator::translate(dsl_request, fixture.snapshots.registry,
                                                   fixture.defaults);

    auto core_request       = preset_request;
    core_request["harness"] = {{"mode", "core"},
                               {"definition", preset_document["root"]["definition"]}};
    auto core = HarnessRequestTranslator::translate(core_request, fixture.snapshots.registry,
                                                    fixture.defaults);

    EXPECT_EQ(preset.source.document(), dsl.source.document());
    EXPECT_EQ(preset.source.document(), core.source.document());
    EXPECT_EQ(preset.invocation_template.input, dsl.invocation_template.input);
    EXPECT_EQ(preset.invocation_template.input, core.invocation_template.input);
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator,
     ProgramModeBuildsVersionTwoStaticSequenceAndMapsCanonicalOperationsToAuthoredSource) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    EXPECT_EQ(fixture.snapshots.admission_profile.max_program_schema_version(),
              LATEST_PROGRAM_SCHEMA_VERSION);
    const auto preset =
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, fixture.defaults);
    const auto core = preset.source.document()["root"]["definition"];

    auto value = program_request(
        core, json{{"op", "sequence"},
                   {"children",
                    json::array({json{{"op", "call_core"}},
                                 json{{"op", "emit"}, {"value", json{{"kind", "audited"}}}}})}});
    const auto translated =
        HarnessRequestTranslator::translate(value, fixture.snapshots.registry, fixture.defaults);

    EXPECT_EQ(translated.source.schema_version(), PROGRAM_SCHEMA_VERSION_V2);
    EXPECT_EQ(translated.source.document()["program_schema_version"], PROGRAM_SCHEMA_VERSION_V2);
    EXPECT_EQ(translated.invocation_template.budget.max_program_operations, 3U);
    EXPECT_EQ(translated.invocation_template.budget.max_concurrency, 1U);
    EXPECT_EQ(translated.invocation_template.budget.max_core_steps, 10U);
    EXPECT_EQ(translated.invocation_template.budget.model_tokens, 1800U);

    ProgramCompiler compiler(fixture.snapshots.registry,
                             ProgramCompilerConfig{"test:harness-program-mode"});
    const auto bundle = compiler.compile(translated.source);
    const auto* first = find_source_map_entry(bundle, "/operations/0");
    const auto* emit  = find_source_map_entry(bundle, "/operations/1");
    const auto* root  = find_source_map_entry(bundle, "/operations/2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(emit, nullptr);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(first->authored.json_pointer, "/harness/definition/children/0");
    EXPECT_EQ(emit->authored.json_pointer, "/harness/definition/children/1");
    EXPECT_EQ(root->authored.json_pointer, "/harness/definition");
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, ProgramModeCompilesEverySupportedStaticOperation) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    const auto preset =
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, fixture.defaults);
    const auto core = preset.source.document()["root"]["definition"];

    const std::vector<std::pair<std::string, json>> operations = {
        {"call_core", json{{"op", "call_core"}}},
        {"sequence",
         json{{"op", "sequence"},
              {"children", json::array({json{{"op", "call_core"}}})}}},
        {"branch",
         json{{"op", "branch"},
              {"condition", json{{"path", "/task/objective"}, {"exists", true}}},
              {"then", json{{"op", "call_core"}}},
              {"else", json{{"op", "call_core"}}}}},
        {"loop",
         json{{"op", "loop"},
              {"condition", json{{"path", "/not-present"}, {"exists", true}}},
              {"body", json{{"op", "call_core"}}},
              {"max_iterations", 1}}},
        {"retry",
         json{{"op", "retry"},
              {"body", json{{"op", "call_core"}}},
              {"max_attempts", 1}}},
        {"parallel",
         json{{"op", "parallel"},
              {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}}},
        {"race",
         json{{"op", "race"},
              {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})}}},
        {"quorum",
         json{{"op", "quorum"},
              {"branches", json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}}})},
              {"min_success", 1}}},
        {"map",
         json{{"op", "map"},
              {"items", json::array({json{{"item", 1}}, json{{"item", 2}}})},
              {"body", json{{"op", "call_core"}}}}},
        {"await", json{{"op", "await"}, {"body", json{{"op", "call_core"}}}}},
        {"emit",
         json{{"op", "sequence"},
              {"children",
               json::array({json{{"op", "call_core"}},
                            json{{"op", "emit"}, {"value", json{{"kind", "audit"}}}}})}}},
        {"checkpoint",
         json{{"op", "checkpoint"}, {"body", json{{"op", "call_core"}}}}},
        {"cancel",
         json{{"op", "sequence"},
              {"children",
               json::array({json{{"op", "call_core"}},
                            json{{"op", "cancel"}, {"scope", "run"}, {"reason", "test"}}})}}},
        {"return",
         json{{"op", "sequence"},
              {"children",
               json::array({json{{"op", "call_core"}},
                            json{{"op", "return"}, {"value", json{{"outcome", "ok"}}}}})}}},
    };

    ProgramCompiler compiler(fixture.snapshots.registry,
                             ProgramCompilerConfig{"test:harness-static-operations"});
    for (const auto& [name, root] : operations) {
        SCOPED_TRACE(name);
        const auto translated = HarnessRequestTranslator::translate(
            program_request(core, root), fixture.snapshots.registry, fixture.defaults);
        EXPECT_EQ(translated.source.schema_version(), PROGRAM_SCHEMA_VERSION_V2);
        EXPECT_NO_THROW((void)compiler.compile(translated.source));
    }
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator,
     ProgramModeRejectsUnsupportedChildBindingAndStaticBudgetOrGrammarViolations) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    const auto preset =
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, fixture.defaults);
    const auto core = preset.source.document()["root"]["definition"];

    const auto expect_error = [&](json root, std::string_view code, std::string_view pointer) {
        try {
            (void)HarnessRequestTranslator::translate(program_request(core, std::move(root)),
                                                      fixture.snapshots.registry, fixture.defaults);
            ADD_FAILURE() << "expected HarnessTranslationError";
        } catch (const HarnessTranslationError& error) {
            EXPECT_EQ(error.code(), code);
            EXPECT_EQ(error.pointer(), pointer);
        }
    };

    expect_error(json{{"op", "await"},
                      {"body", json{{"op", "spawn"}, {"child_binding", "child-program"}}}},
                 "H_PROGRAM_CHILD_BINDING", "/harness/definition/body");
    expect_error(
        json{{"op", "parallel"},
             {"branches",
              json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}},
                           json{{"op", "call_core"}}})}},
        "H_PROGRAM_CONCURRENCY", "/harness/definition");
    expect_error(json{{"op", "loop"},
                      {"condition", json{{"path", "/never"}, {"exists", true}}},
                      {"body", json{{"op", "call_core"}}},
                      {"max_iterations", 11}},
                 "H_PROGRAM_BUDGET", "/harness/definition");

    try {
        (void)HarnessRequestTranslator::translate(
            program_request(
                core, json{{"op", "race"},
                            {"branches",
                             json::array({json{{"op", "call_core"}}, json{{"op", "call_core"}},
                                          json{{"op", "call_core"}}})}}),
            fixture.snapshots.registry, fixture.defaults);
        ADD_FAILURE() << "expected HarnessTranslationError";
    } catch (const HarnessTranslationError& error) {
        EXPECT_EQ(error.code(), "H_PROGRAM_COMPILE");
        EXPECT_EQ(error.pointer(), "/harness/definition/branches");
        EXPECT_NE(std::string(error.what()).find("P_PLAN_RACE_ARITY"), std::string::npos);
    }

    try {
        (void)HarnessRequestTranslator::translate(
            program_request(core, json{{"op", "loop"},
                                        {"condition", json{{"path", "/never"}, {"exists", true}}},
                                        {"body", json{{"op", "call_core"}}}}),
            fixture.snapshots.registry, fixture.defaults);
        ADD_FAILURE() << "expected HarnessTranslationError";
    } catch (const HarnessTranslationError& error) {
        EXPECT_EQ(error.code(), "H_PROGRAM_COMPILE");
        EXPECT_EQ(error.pointer(), "/harness/definition/max_iterations");
    }
    expect_error(json{{"op", "retry"}, {"body", json{{"op", "call_core"}}}},
                 "H_PROGRAM_COMPILE", "/harness/definition/max_attempts");

    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, ProgramModePreservesDslAndTransportAuthorityBoundaries) {
    std::atomic<int> calls{0};
    auto             fixture = host(calls);
    const auto preset =
        HarnessRequestTranslator::translate(request(), fixture.snapshots.registry, fixture.defaults);
    const auto core = preset.source.document()["root"]["definition"];

    auto dsl = program_request(core, json{{"op", "call_core"}});
    dsl["harness"]["mode"] = "dsl";
    try {
        (void)HarnessRequestTranslator::translate(dsl, fixture.snapshots.registry, fixture.defaults);
        ADD_FAILURE() << "expected HarnessTranslationError";
    } catch (const HarnessTranslationError& error) {
        EXPECT_EQ(error.code(), "H_STRICT_CORE");
        EXPECT_EQ(error.pointer(), "/harness/definition");
    }

    try {
        (void)HarnessRequestTranslator::translate(
            program_request(core, json{{"op", "emit"},
                                        {"value", json{{"command", "untrusted"}}}}),
            fixture.snapshots.registry, fixture.defaults);
        ADD_FAILURE() << "expected HarnessTranslationError";
    } catch (const HarnessTranslationError& error) {
        EXPECT_EQ(error.code(), "H_TRANSPORT_VALUE");
        EXPECT_EQ(error.pointer(), "/harness/definition/value/command");
    }
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
    global_request["workers"] = json::array();
    global_request["harness"] = {
        {"mode", "core"},
        {"definition",
         {{"schema_version", 1},
          {"name", "global_only"},
          {"nodes", {{"work", {{"type", "translator_global_only"}}}}},
          {"edges", json::array({{{"from", "__start__"}, {"to", "work"}},
                                 {{"from", "work"}, {"to", "__end__"}}})}}}};
    auto translated = HarnessRequestTranslator::translate(
        global_request, fixture.snapshots.registry, fixture.defaults);
    ProgramCompiler compiler(fixture.snapshots.registry,
                             ProgramCompilerConfig{"test:harness-translator"});
    EXPECT_THROW((void)compiler.compile(translated.source), ProgramCompileError);
    EXPECT_EQ(calls.load(), 0);
}

TEST(HarnessProgramTranslator, RequiresFrozenContractAndCarriesItToHarnessProjection) {
    std::atomic<int> calls{0};
    auto              fixture = host(calls);
    auto               proposed = ContractManifest::propose(ContractManifestSpec{
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
    auto rejected = request();
    rejected["contract_manifest"] = json::parse(proposed.serialize_canonical());
    EXPECT_THROW(HarnessRequestTranslator::translate(rejected, fixture.snapshots.registry,
                                                     fixture.defaults),
                 HarnessTranslationError);

    auto frozen = request();
    const auto manifest = frozen_contract("owner:test", json{{"outcome", "zero_findings"}});
    frozen["contract"] = json::parse(manifest.serialize_canonical());
    frozen["workspace_revision"] = "workspace-test-1";
    const auto translated =
        HarnessRequestTranslator::translate(frozen, fixture.snapshots.registry, fixture.defaults);
    ASSERT_TRUE(translated.contract.has_value());
    EXPECT_EQ(translated.contract->content_hash(), manifest.content_hash());
    EXPECT_EQ(translated.wire.workspace_revision, "workspace-test-1");
    EXPECT_EQ(translated.wire.projection["contract_manifest_hash"],
              manifest.content_hash());
    EXPECT_EQ(translated.invocation_template.budget.max_program_operations, 10U);
    EXPECT_EQ(translated.invocation_template.budget.wall_time_ms, 5000U);
}

}  // namespace
