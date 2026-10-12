// NeoGraph plan intake and plan repair, offline.
//
// A planner node (scripted here: it replays recorded answers, so the example needs no
// network) answers with a flat `step-plan/v1`. The registered intake node checks the
// answer against the host's template allow-list. The first answer has three mistakes, so
// the node rebuilds the prompt with the violations and the planner is asked again. The
// second answer is accepted, lowered into a TaskGraphProposal, and expanded by the Program
// into child Programs that compute 2 + 3 and 10 + 20, add the two sums, and upper-case a
// word.
//
// Build with the installed `neograph::program` target.
// Usage: ./example_program_plan_intake     prints what each planner turn saw and the task results.
//
// smoke-expect: s3 completed -> 35

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <asio/awaitable.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) { return "sha256:" + std::string(64, value); }

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:plan-intake-example",
                              {},
                              {},
                              {}};
}

// ---- The planner: replays recorded answers by the number of answers already judged. ----

const std::string kPrompt =
    "Plan the work. Templates: calc.add/v1 (inputs a, b; output sum) and text.upper/v1 "
    "(input text; output upper). Answer with one step-plan/v1 JSON object.";

const std::vector<std::string>& recorded_answers() {
    static const std::vector<std::string> answers = {
        // Attempt 1: an unknown template, a reference to a later step, and a key outside the format.
        R"({"schema":"step-plan/v1","steps":[
              {"id":"s1","use":"calc.ad/v1","params":{"a":2,"b":3}},
              {"id":"s2","use":"calc.add/v1","params":{"a":{"from_step":"s3"},"b":1},"note":"x"},
              {"id":"s3","use":"calc.add/v1","params":{"a":1,"b":1}}]})",
        // Attempt 2: valid. s1 and s2 are independent; s3 adds their sums; s4 follows s3.
        R"({"schema":"step-plan/v1","steps":[
              {"id":"s1","use":"calc.add/v1","params":{"a":2,"b":3}},
              {"id":"s2","use":"calc.add/v1","params":{"a":10,"b":20}},
              {"id":"s3","use":"calc.add/v1","params":{"a":{"from_step":"s1"},"b":{"from_step":"s2"}}},
              {"id":"s4","use":"text.upper/v1","params":{"text":"done"},"after":["s3"]}]})"};
    return answers;
}

class ScriptedPlannerNode final : public GraphNode {
public:
    explicit ScriptedPlannerNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto report  = input.state.get("repair");
        std::size_t judged = 0;
        if (report.is_object() && report.contains("attempts") && report["attempts"].is_number_unsigned())
            judged = report["attempts"].get<std::size_t>();
        const auto& answers = recorded_answers();
        const auto  answer  = answers[std::min(judged, answers.size() - 1)];

        json messages = input.state.get("messages");
        std::cout << "planner turn " << judged + 1 << " sees " << messages.size() << " message(s)\n";
        json next = json::array();
        for (std::size_t index = 0; index < messages.size(); ++index) next.push_back(messages[index]);
        next.push_back(json{{"role", "assistant"}, {"content", answer}});
        ChannelWrite write;
        write.channel = "messages";
        write.mode    = ChannelWrite::Mode::Overwrite;
        write.value   = std::move(next);
        co_return NodeOutput{{std::move(write)}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// ---- The templates: child Programs over one deterministic node each. ----

class AddNode final : public GraphNode {
public:
    explicit AddNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto a = input.state.get("a");
        const auto b = input.state.get("b");
        if (!a.is_number() || !b.is_number()) throw std::invalid_argument("calc.add needs numbers");
        co_return NodeOutput{{ChannelWrite{"value", a.get<double>() + b.get<double>()}}};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class UpperNode final : public GraphNode {
public:
    explicit UpperNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        auto text = input.state.get("text").get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        co_return NodeOutput{{ChannelWrite{"value", text}}};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

TaskGraphTemplateContract template_contract(std::string id, char identity, std::string binding,
                                            std::vector<std::string> inputs, std::string artifact) {
    TaskGraphTemplateContract contract;
    contract.template_id      = std::move(id);
    contract.content_identity = digest(identity);
    contract.input_fields     = std::move(inputs);
    contract.output_artifacts = {TaskGraphArtifactContract{std::move(artifact), {"/channels/value/value"}}};
    contract.budget_ceiling   = TaskGraphBudget{1000, 100, 100, 4096};
    contract.child_binding    = std::move(binding);
    contract.kind             = "program";
    return contract;
}

std::vector<TaskGraphTemplateContract> allowlist() {
    return {template_contract("calc.add/v1", '3', "add", {"/a", "/b"}, "sum"),
            template_contract("text.upper/v1", '4', "upper", {"/text"}, "upper")};
}

TaskGraphProposalLimits proposal_limits() {
    TaskGraphProposalLimits limits;
    limits.max_tasks               = 6;
    limits.max_edges               = 12;
    limits.max_depth               = 6;
    limits.per_task_budget_ceiling = TaskGraphBudget{1000, 100, 100, 4096};
    limits.total_budget_ceiling    = TaskGraphBudget{6000, 600, 600, 24576};
    limits.max_literal_bytes       = 256;       // literals are refused unless the host sets these
    limits.max_total_literal_bytes = 1024;
    return limits;
}

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "example.scripted_planner", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<ScriptedPlannerNode>(name);
        },
        json{{"type", "object"}}, json{{"writes", json::array({"messages"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "example.add", '5'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<AddNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_node(
        manifest(ExecutableKind::Node, "example.upper", '6'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<UpperNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '2'),
                        [](const json&, const json& incoming) { return incoming; });

    // The intake node, its route condition and the report node. The allow-list and limits are
    // the host's: the sealed graph can only choose max_attempts and the feedback mode.
    StepPlanRepairNodeOptions repair;
    repair.intake.source_id          = "example:planner";
    repair.intake.proposal_limits    = proposal_limits();
    repair.intake.template_allowlist = allowlist();
    repair.intake.template_info      = {{"calc.add/v1", {"a", "b"}, "Add two numbers."},
                                        {"text.upper/v1", {"text"}, "Upper-case a text."}};
    repair.attestation_id            = "attestation:plan-intake-example";
    add_step_plan_repair(builder, repair);
    return std::move(builder).build();
}

// ---- The planner graph: plan -> intake -> (plan again), at most three attempts. ----

json planner_definition() {
    return {
        {"schema_version", 1},
        {"name", "main"},
        {"channels",
         {{"messages", {{"reducer", "overwrite"},
                        {"initial", json::array({{{"role", "user"}, {"content", kPrompt}}})}}},
          {"proposal", {{"reducer", "overwrite"}, {"initial", nullptr}}},
          {"repair", {{"reducer", "overwrite"}, {"initial", json::object()}}}}},
        {"nodes",
         {{"plan", {{"type", "example.scripted_planner"}}},
          {"intake", {{"type", "neograph.step_plan_intake/v1"},
                      {"max_attempts", 3},
                      {"feedback", "replace"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "plan"}},
                               {{"from", "plan"}, {"to", "intake"}}})},
        {"conditional_edges",
         json::array({{{"from", "intake"},
                       {"condition", "neograph.step_plan_route/v1"},
                       {"routes", {{"ok", "__end__"}, {"retry", "plan"}, {"give_up", "__end__"}}}}})},
        {"node_retry_policies", {{"plan", {{"max_visits", 3}}}}},
    };
}

json requirements(std::uint64_t max_wall_time, std::uint64_t depth, std::uint64_t children,
                  std::uint64_t compiles) {
    return json::array({
        {{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", max_wall_time}},
        {{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
        {{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
        {{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 2}},
        {{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 32}},
        {{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        {{"resource", "max_dynamic_compiles"}, {"minimum", compiles}, {"maximum", compiles}},
        {{"resource", "max_child_depth"}, {"minimum", depth}, {"maximum", depth}},
        {{"resource", "max_total_children"}, {"minimum", children}, {"maximum", children}},
    });
}

// A child Program with one node that reads its inputs from channels.
json child_document(const std::string& node_type, const std::vector<std::string>& inputs) {
    json channels{{"value", {{"reducer", "overwrite"}, {"initial", ""}}}};
    for (const auto& input : inputs)
        channels[input] = json{{"reducer", "overwrite"}, {"initial", json::object()}};
    json definition{
        {"schema_version", 1},
        {"name", "child"},
        {"channels", std::move(channels)},
        {"nodes", {{"work", {{"type", node_type}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "work"}},
                               {{"from", "work"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
    json declared = requirements(3000, 0, 0, 0);
    declared[1]["maximum"] = 100;   // the link below must cover what a child declares
    declared[2]["maximum"] = 100;
    declared[3]["maximum"] = 1;
    declared[4]["maximum"] = 1;
    declared[5]["maximum"] = 3;
    return {
        {"program_schema_version", 1},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "call_core"}, {"name", "child"}, {"definition", std::move(definition)}}},
        {"declared_budget_requirements", std::move(declared)},
    };
}

// The parent: the planner graph, then the expansion of the proposal it produced.
json parent_document() {
    json expand{{"op", "expand_task_graph"},
                {"proposal_source", {{"artifact", "input"}, {"field", "/channels/proposal/value"}}},
                {"max_tasks", 4},
                {"max_edges", 8},
                {"max_depth", 4},
                {"max_dynamic_compiles", 1},
                {"max_total_children", 4},
                {"max_concurrency", 2},
                {"failure_policy", "collect"}};
    return {
        {"program_schema_version", PROGRAM_SCHEMA_VERSION_V4},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "sequence"},
                  {"name", "main"},
                  {"definition", planner_definition()},
                  {"children", json::array({{{"op", "call_core"}}, expand})}}},
        {"declared_budget_requirements", requirements(10000, 4, 4, 1)},
    };
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("plan-intake-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(PROGRAM_SCHEMA_VERSION_V4)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy_snapshot(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("plan-intake-policy")
        .semantic_version("1.0.0")
        .owner_scope("example")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32});
    return std::move(builder).build();
}

}  // namespace

int run_example() {
    const auto registry = registry_snapshot();
    const auto profile  = admission_profile(registry);
    const auto policy   = policy_snapshot(profile);

    auto store   = std::make_shared<InMemoryProgramStore>();
    auto catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
        store, registry, std::make_shared<EngineGenerationCache>(), "plan-intake-example/v1"});
    ProgramCompiler compiler(registry, {"plan-intake-example/v1"});
    const auto      admit = [&](json document) {
        return catalog->admit(
            compiler.compile(ProgramSource::from_cpp_builder("example:plan-intake", 1, std::move(document))),
            ProgramAdmission{"example", profile, policy, {}});
    };

    // The parent is a schema-4 Program (it expands a task graph); the templates are its linked children.
    const auto parent_source =
        ProgramSource::from_cpp_builder("example:plan-intake-parent", PROGRAM_SCHEMA_VERSION_V4, parent_document());
    const auto parent = catalog->admit(compiler.compile(parent_source),
                                       ProgramAdmission{"example", profile, policy, {}});
    const auto add_version   = admit(child_document("example.add", {"a", "b"}));
    const auto upper_version = admit(child_document("example.upper", {"text"}));

    ProgramModuleData module_data;
    module_data.owner_scope    = "example";
    module_data.coordinate     = ModuleCoordinate{"example", "plan-intake", "1.0.0", ""};
    module_data.attestation_id = "attestation:plan-intake-example";
    const std::vector<std::pair<std::string, ProgramVersion>> children = {{"add", add_version},
                                                                          {"upper", upper_version}};
    std::vector<ProgramBundle> bundles;
    for (const auto& [name, version] : children) {
        const auto bundle = store->get_bundle(version.bundle_id());
        if (!bundle) throw std::runtime_error("child bundle was not admitted");
        bundles.push_back(*bundle);
        module_data.children.push_back(ChildProgramDescriptor{
            name, version.id(), {ModulePort{"input", bundle->input_contract()}},
            {ModulePort{"output", bundle->output_contract()}},
            bundle->capability_effect_closure().capabilities,
            bundle->capability_effect_closure().effects, BudgetLimits{3000, 100, 100, 1, 1, 3, 0, 0, 0},
            ExecutionGuarantee::Strict});
    }
    const auto       module = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution;
    resolution.root = module.coordinate();
    resolution.modules.push_back(module);
    std::map<std::string, ProgramRuntimeChildBinding> bindings;
    for (std::size_t index = 0; index < children.size(); ++index) {
        auto receipt = link_module_child(resolution, module, children[index].first, bundles[index],
                                         children[index].second);
        bindings.emplace(children[index].first,
                         ProgramRuntimeChildBinding{std::move(receipt), children[index].second});
    }

    RuntimeConfig config{
        catalog,
        std::make_shared<InMemoryCheckpointStore>(),
        {},
        std::make_shared<InMemoryProgramTransitionStore>(),
        2,
        [&bindings, parent_id = parent.id()](std::string_view owner, std::string_view version,
                                             std::string_view binding)
            -> std::optional<ProgramRuntimeChildBinding> {
            if (owner != "example" || version != parent_id) return std::nullopt;
            const auto found = bindings.find(std::string(binding));
            if (found == bindings.end()) return std::nullopt;
            return found->second;
        }};
    config.task_graph_fragments = std::make_shared<InMemoryTaskGraphFragmentStore>();
    config.task_graph_policy_resolver =
        [](std::string_view owner, std::string_view, std::string_view)
        -> std::optional<TaskGraphExpansionPolicy> {
        if (owner != "example") return std::nullopt;
        TaskGraphExpansionPolicy expansion;
        expansion.limits             = proposal_limits();
        expansion.template_allowlist = allowlist();
        return expansion;
    };
    ProgramRuntime runtime(std::move(config));

    const auto result = runtime.run(
        "example", parent,
        ProgramInvocation{json::object(), RunBudget{10000, 1000, 1000, 2, 32, 20, 1, 4, 4}});
    if (result.status() != ProgramTerminalStatus::Completed)
        throw std::runtime_error("the example did not complete: " + result.serialize_canonical());

    const auto output = result.output();
    std::cout << "tasks:\n";
    for (std::size_t index = 0; index < output["tasks"].size(); ++index) {
        const auto task = output["tasks"][index];
        std::cout << "  " << task["task_id"].get<std::string>() << " "
                  << task["state"].get<std::string>() << " -> "
                  << task["output"]["channels"]["value"]["value"].dump() << '\n';
    }
    return 0;
}

int main() {
    try {
        return run_example();
    } catch (const ProgramCompileError& error) {
        std::cerr << error.what() << '\n';
        for (const auto& diagnostic : error.diagnostics())
            std::cerr << "  " << diagnostic.code << " " << diagnostic.primary.json_pointer << ": "
                      << diagnostic.message << '\n';
    } catch (const ProgramAdmissionError& error) {
        std::cerr << error.what() << '\n';
        for (const auto& diagnostic : error.diagnostics())
            std::cerr << "  " << diagnostic.code << " " << diagnostic.primary.json_pointer << ": "
                      << diagnostic.message << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
