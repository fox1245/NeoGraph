// Model-generated QuickJS Program synthesis probe.
//
// Reads JavaScript from stdin, creates an immutable non-executable proposal,
// reserves one dynamic compile, compiles in QuickJS, independently admits the
// exact ProgramVersion, executes it, and emits one JSON evidence record.

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <asio/awaitable.hpp>

#include <atomic>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::atomic<unsigned> seed_calls{0};
std::atomic<unsigned> double_calls{0};
std::atomic<unsigned> finish_calls{0};
std::atomic<unsigned> stale_calls{0};

std::string digest(char value) { return "sha256:" + std::string(64, value); }

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:model-program-synthesis-probe",
                              {},
                              {},
                              {}};
}

class SeedNode final : public GraphNode {
public:
    explicit SeedNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++seed_calls;
        co_return NodeOutput{{ChannelWrite{"value", 6}}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

class DoubleNode final : public GraphNode {
public:
    explicit DoubleNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++double_calls;
        const auto current = input.state.get("value");
        if (!current.is_number_integer())
            throw std::runtime_error("probe.double requires integer channel 'value'");
        co_return NodeOutput{{ChannelWrite{"value", current.get<int>() * 2}}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

class FinishNode final : public GraphNode {
public:
    explicit FinishNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput input) override {
        ++finish_calls;
        if (input.state.get("value") != 12)
            throw std::runtime_error("probe.finish observed the wrong topology result");
        co_return NodeOutput{{ChannelWrite{"path", "model-generated"}}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

class StaleNode final : public GraphNode {
public:
    explicit StaleNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++stale_calls;
        co_return NodeOutput{{ChannelWrite{"value", -1}}};
    }
    std::string get_name() const override { return name_; }
private:
    std::string name_;
};

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "probe.seed", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<SeedNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"reads", json::array()},
             {"writes", json::array({"value"})},
             {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "probe.double", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<DoubleNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"reads", json::array({"value"})},
             {"writes", json::array({"value"})},
             {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "probe.finish", '3'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FinishNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"reads", json::array({"value"})},
             {"writes", json::array({"path"})},
             {"exports", json::array({"path"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "probe.stale", '4'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<StaleNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"reads", json::array()},
             {"writes", json::array({"value"})},
             {"exports", json::array({"value"})}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "probe.overwrite", '5'),
                        [](const json&, const json& incoming) { return incoming; });
    return std::move(builder).build();
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("model-program-synthesis-probe-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy_snapshot(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("model-program-synthesis-probe-policy")
        .semantic_version("1.0.0")
        .owner_scope("model-synthesis-probe")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32})
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    return std::move(builder).build();
}

RunBudget child_budget() { return RunBudget{10000, 0, 0, 1, 1, 20, 0, 0, 0}; }
RunBudget source_budget() { return RunBudget{10000, 0, 0, 1, 2, 20, 0, 0, 0}; }

std::string source_boundary_program() {
    return R"JS(
export function define() {
    const graph = ng.graph("source-stale");
    graph.channel("value", {reducer: "probe.overwrite", initial: 0});
    graph.channel("path", {reducer: "probe.overwrite", initial: ""});
    graph.node("stale", {type: "probe.stale"});
    graph.node("seed", {type: "probe.seed"});
    graph.node("double", {type: "probe.double"});
    graph.node("finish", {type: "probe.finish"});
    graph.entry("stale");
    graph.edge("stale", "seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    yield ng.checkpoint({cursor: 1, intent: "swap-to-model"}, "model-swap:boundary");
    return yield ng.callCore("source-stale", input, "model-swap:stale");
}
)JS";
}

std::string read_source(int argc, char** argv) {
    std::ostringstream buffer;
    if (argc > 2) throw std::invalid_argument("Expected zero arguments or one QuickJS source path");
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::invalid_argument("Could not open QuickJS source path");
        buffer << input.rdbuf();
    } else {
        buffer << std::cin.rdbuf();
    }
    auto source = buffer.str();
    if (source.empty()) throw std::invalid_argument("QuickJS source is required on stdin");
    return source;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        seed_calls = 0;
        double_calls = 0;
        finish_calls = 0;
        stale_calls = 0;

        const auto registry = registry_snapshot();
        const auto profile = admission_profile(registry);
        const auto policy = policy_snapshot(profile);
        auto store = std::make_shared<InMemoryProgramStore>();
        auto catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
            store,
            registry,
            std::make_shared<EngineGenerationCache>(),
            "model-program-synthesis-probe/v1"});
        auto compiler = std::make_shared<ProgramCompiler>(
            registry, ProgramCompilerConfig{"model-program-synthesis-probe/v1"});

        auto source = ProgramSource::from_javascript("model-generated-program.js",
                                                     read_source(argc, argv));
        ProgramSynthesisProposalData proposal_data;
        proposal_data.owner_scope = "model-synthesis-probe";
        proposal_data.lineage_id = digest('a');
        proposal_data.parent_run_id = "model-synthesis-probe-parent";
        proposal_data.source = source;
        proposal_data.requested_budget = child_budget();
        proposal_data.created_at_ms = 1;
        const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

        ProgramSynthesisGateway gateway({
            compiler,
            catalog,
            [](const ProgramSynthesisProposal& value) {
                auto before = child_budget();
                before.max_dynamic_compiles = 1;
                auto after = before;
                after.max_dynamic_compiles = 0;
                return ProgramSynthesisReservation::create(
                    {value.id(), value.data().lineage_id, digest('b'), digest('c'),
                     before, after});
            },
            [profile, policy](const ProgramSynthesisProposal&,
                              const ProgramBundle&,
                              const ProgramSynthesisReservation&) {
                return ProgramAdmission{"model-synthesis-probe", profile, policy, {}};
            },
            64 * 1024,
            [](const ProgramSynthesisProposal&, const ProgramBundle& bundle,
               const ProgramSynthesisReservation&) {
                const auto definitions = bundle.sealed_core_definitions();
                const bool accepted = definitions.size() == 1 &&
                                      definitions.front().name == "model-synthesized";
                return ProgramSynthesisSemanticDecision{
                    digest('d'), digest('e'), accepted,
                    json{{"expected_core", "model-synthesized"},
                         {"observed_core", definitions.empty() ? "" : definitions.front().name}}};
            },
            64 * 1024});

        const auto synthesized = gateway.synthesize(proposal);
        auto transitions = std::make_shared<InMemoryProgramTransitionStore>();
        ProgramRuntime runtime(RuntimeConfig{
            catalog,
            std::make_shared<InMemoryCheckpointStore>(),
            {},
            transitions,
            1});
        const auto direct_execution = runtime.run(
            "model-synthesis-probe",
            synthesized.version,
            ProgramInvocation{json::object(), child_budget(), "model-synthesis-probe-trace", {}});

        const auto direct_output = direct_execution.output();
        const bool direct_completed =
            direct_execution.status() == ProgramTerminalStatus::Completed;
        const bool direct_value_ok = direct_output.contains("channels") &&
                                     direct_output["channels"].contains("value") &&
                                     direct_output["channels"]["value"]["value"] == 12;
        const bool direct_path_ok = direct_output.contains("channels") &&
                                    direct_output["channels"].contains("path") &&
                                    direct_output["channels"]["path"]["value"] ==
                                        "model-generated";
        const bool direct_calls_ok =
            seed_calls == 1 && double_calls == 1 && finish_calls == 1;

        const auto source_program = ProgramSource::from_javascript(
            "runtime-swap-source.js", source_boundary_program());
        const auto source_bundle = compiler->compile(source_program, source_budget());
        const auto source_version = catalog->admit(
            source_bundle,
            ProgramAdmission{"model-synthesis-probe", profile, policy, {}});
        const auto migration_plan = catalog->plan_migration(
            "model-synthesis-probe", source_version.id(), synthesized.version.id());

        seed_calls = 0;
        double_calls = 0;
        finish_calls = 0;
        stale_calls = 0;
        auto source_handle = runtime.start(
            "model-synthesis-probe",
            source_version,
            ProgramInvocation{json::object(), source_budget(), "model-swap-source-trace", {}});
        auto handoff = source_handle.next_handoff();
        const auto handoff_value = handoff.value();
        const auto source_run_id = source_handle.run_id();
        const auto source_lineage_before_swap = transitions->load_run_lineage(
            "model-synthesis-probe", source_run_id);
        if (!source_lineage_before_swap)
            throw std::runtime_error("Source lineage was not durable at the replacement boundary");
        const auto target_input = json{{"handoff", handoff_value},
                                       {"previous_run_id", source_run_id}};
        auto target_handle = runtime.replace(
            "model-synthesis-probe",
            std::move(handoff),
            synthesized.version,
            ProgramInvocation{target_input, source_lineage_before_swap->remaining_budget(),
                              "model-swap-target-trace", {}});
        const auto swap_execution = target_handle.wait();
        const auto source_terminal = source_handle.wait();
        const auto swap_output = swap_execution.output();
        const auto lineage = transitions->load_run_lineage(
            "model-synthesis-probe", source_run_id);
        const auto generation = lineage
            ? transitions->load_generation(
                  "model-synthesis-probe", lineage->lineage_id(), 2)
            : std::nullopt;
        const auto replacement = generation ? generation->replacement_receipt() : std::nullopt;
        const bool swap_completed =
            swap_execution.status() == ProgramTerminalStatus::Completed;
        const bool swap_value_ok = swap_output.contains("channels") &&
                                   swap_output["channels"].contains("value") &&
                                   swap_output["channels"]["value"]["value"] == 12;
        const bool swap_path_ok = swap_output.contains("channels") &&
                                  swap_output["channels"].contains("path") &&
                                  swap_output["channels"]["path"]["value"] ==
                                      "model-generated";
        const bool swap_calls_ok = seed_calls == 1 && double_calls == 1 &&
                                   finish_calls == 1 && stale_calls == 0;
        const bool lineage_ok = lineage && lineage->active_generation() == 2 &&
                                 generation && replacement &&
                                 generation->program_version_id() == synthesized.version.id() &&
                                replacement->source_run_id() == source_run_id &&
                                replacement->target_run_id() == target_handle.run_id();

        json migration_diagnostics = json::array();
        for (const auto& diagnostic : migration_plan.diagnostics()) {
            migration_diagnostics.push_back({{"code", diagnostic.code},
                                             {"dimension", to_string(diagnostic.dimension)}});
        }

        const bool direct_verified =
            direct_completed && direct_value_ok && direct_path_ok && direct_calls_ok;
        const bool swap_verified = swap_completed && swap_value_ok && swap_path_ok && swap_calls_ok &&
                                   lineage_ok &&
                                   generation->program_version_id() == synthesized.version.id();

        const json report = {
            {"source_kind", to_string(source.kind())},
            {"source_hash", source.source_hash()},
            {"proposal_id", proposal.id()},
            {"reservation_id", synthesized.reservation.id()},
            {"bundle_id", synthesized.bundle.id()},
            {"program_version_id", synthesized.version.id()},
            {"synthesis_receipt_id", synthesized.receipt.id()},
            {"replacement_uses_synthesis_version", true},
            {"publication_found",
             catalog->find_version("model-synthesis-probe", synthesized.version.id()).has_value()},
            {"direct_execution",
             {{"terminal_status", to_string(direct_execution.status())},
              {"execution_trace", direct_execution.execution_trace()},
              {"output", direct_output},
              {"verified", direct_verified}}},
            {"runtime_swap",
             {{"source_program_version_id", source_version.id()},
              {"source_run_id", source_run_id},
              {"source_terminal_status", to_string(source_terminal.status())},
              {"target_run_id", target_handle.run_id()},
               {"target_program_version_id", synthesized.version.id()},
              {"target_terminal_status", to_string(swap_execution.status())},
              {"active_generation", lineage ? lineage->active_generation() : 0},
              {"replacement_receipt_id", replacement ? replacement->id() : ""},
              {"execution_trace", swap_execution.execution_trace()},
              {"node_calls",
               {{"seed", seed_calls.load()},
                {"double", double_calls.load()},
                {"finish", finish_calls.load()},
                {"stale", stale_calls.load()}}},
              {"output", swap_output},
              {"verified", swap_verified}}},
            {"graph_migration_plan",
             {{"compatibility", to_string(migration_plan.compatibility())},
              {"diagnostics", migration_diagnostics},
              {"is_compatible", migration_plan.is_compatible()}}},
            {"verified", direct_verified && swap_verified},
        };
        std::cout << report.dump() << '\n';
        return report["verified"].get<bool>() ? 0 : 1;
    } catch (const ProgramCompileError& error) {
        json diagnostics = json::array();
        for (const auto& diagnostic : error.diagnostics()) {
            diagnostics.push_back({{"code", diagnostic.code},
                                   {"message", diagnostic.message},
                                   {"pointer", diagnostic.primary.json_pointer},
                                   {"witness", diagnostic.witness}});
        }
        std::cerr << json{{"error", error.what()}, {"diagnostics", diagnostics}}.dump() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << json{{"error", error.what()}}.dump() << '\n';
        return 3;
    }
}
