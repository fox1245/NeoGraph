// NeoGraph Program quickstart.
//
// Build with the installed `neograph::program` target.  The example creates
// one immutable RegistrySnapshot, compiles a ProgramSource containing one
// `call_core` operation, admits it, and runs it through ProgramRuntime.  The
// same GraphEngine remains the only node executor; Program owns the lifecycle
// envelope and durable identities.
//
// Usage: ./example_program_quickstart     prints: 1

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <asio/awaitable.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) { return "sha256:" + std::string(64, value); }

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:program-quickstart",
                              {},
                              {},
                              {}};
}

class IncrementNode final : public GraphNode {
public:
    explicit IncrementNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto current = input.state.get("counter");
        const auto value = current.is_number_integer() ? current.get<int>() : 0;
        co_return NodeOutput{{ChannelWrite{"counter", value + 1}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "quickstart.increment", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncrementNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"reads", json::array({"counter"})},
             {"writes", json::array({"counter"})},
             {"exports", json::array({"counter"})}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '2'),
                        [](const json&, const json& incoming) { return incoming; });
    return std::move(builder).build();
}

json core_definition() {
    return {
        {"schema_version", 1},
        {"name", "program-quickstart-core"},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes", {{"increment", {{"type", "quickstart.increment"}}}}},
        {"edges",
         json::array({{{"from", "__start__"}, {"to", "increment"}},
                      {{"from", "increment"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
}

json program_document() {
    return {
        {"program_schema_version", 1},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "call_core"},
                  {"name", "program-quickstart-core"},
                  {"definition", core_definition()}}},
        {"declared_budget_requirements",
         json::array({
             {{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
             {{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
             {{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
             {{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
             {{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
             {{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
         })},
    };
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("program-quickstart-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy_snapshot(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("program-quickstart-policy")
        .semantic_version("1.0.0")
        .owner_scope("quickstart")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{10000, 1, 1, 1, 1, 20, 1, 1, 1});
    return std::move(builder).build();
}

}  // namespace

int main() {
    const auto registry = registry_snapshot();
    const auto profile = admission_profile(registry);
    const auto policy = policy_snapshot(profile);

    ProgramCompiler compiler(registry, {"program-quickstart/v1"});
    const auto source = ProgramSource::from_cpp_builder(
        "example:program-quickstart", 1, program_document());
    const auto bundle = compiler.compile(source);

    auto store = std::make_shared<InMemoryProgramStore>();
    auto catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
        store, registry, std::make_shared<EngineGenerationCache>(), "program-quickstart/v1"});
    const auto version =
        catalog->admit(bundle, ProgramAdmission{"quickstart", profile, policy, {}});

    ProgramRuntime runtime(RuntimeConfig{
        catalog,
        std::make_shared<InMemoryCheckpointStore>(),
        {},
        std::make_shared<InMemoryProgramTransitionStore>(),
        1,
    });
    const auto result = runtime.run(
        "quickstart", version, ProgramInvocation{json::object(), RunBudget{10000, 0, 0, 1, 1, 20, 0, 0, 0}});
    if (result.status() != ProgramTerminalStatus::Completed)
        throw std::runtime_error("Program quickstart did not complete");

    std::cout << result.output()["channels"]["counter"]["value"] << '\n';
    return 0;
}
