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
    builder.add_reducer(manifest(ExecutableKind::Reducer, "probe.overwrite", '4'),
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
        .budget_ceiling(BudgetLimits{10000, 1, 1, 1, 1, 20, 1, 0, 0})
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    return std::move(builder).build();
}

RunBudget child_budget() { return RunBudget{10000, 0, 0, 1, 1, 20, 0, 0, 0}; }

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
            64 * 1024});

        const auto synthesized = gateway.synthesize(proposal);
        ProgramRuntime runtime(RuntimeConfig{
            catalog,
            std::make_shared<InMemoryCheckpointStore>(),
            {},
            std::make_shared<InMemoryProgramTransitionStore>(),
            1});
        const auto execution = runtime.run(
            "model-synthesis-probe",
            synthesized.version,
            ProgramInvocation{json::object(), child_budget(), "model-synthesis-probe-trace", {}});

        const auto output = execution.output();
        const bool completed = execution.status() == ProgramTerminalStatus::Completed;
        const bool value_ok = output.contains("channels") &&
                              output["channels"].contains("value") &&
                              output["channels"]["value"]["value"] == 12;
        const bool path_ok = output.contains("channels") &&
                             output["channels"].contains("path") &&
                             output["channels"]["path"]["value"] == "model-generated";
        const bool calls_ok = seed_calls == 1 && double_calls == 1 && finish_calls == 1;

        const json report = {
            {"source_kind", to_string(source.kind())},
            {"source_hash", source.source_hash()},
            {"proposal_id", proposal.id()},
            {"reservation_id", synthesized.reservation.id()},
            {"bundle_id", synthesized.bundle.id()},
            {"program_version_id", synthesized.version.id()},
            {"synthesis_receipt_id", synthesized.receipt.id()},
            {"publication_found",
             catalog->find_version("model-synthesis-probe", synthesized.version.id()).has_value()},
            {"terminal_status", to_string(execution.status())},
            {"execution_trace", execution.execution_trace()},
            {"node_calls",
             {{"seed", seed_calls.load()},
              {"double", double_calls.load()},
              {"finish", finish_calls.load()}}},
            {"output", output},
            {"verified", completed && value_ok && path_ok && calls_ok},
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
