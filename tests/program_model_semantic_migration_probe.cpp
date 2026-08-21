// Model-generated declaration-only QuickJS topology -> admitted P1 semantic
// adapter -> durable GraphEngine generation migration probe.

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
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

std::atomic<unsigned> work_calls{0};
std::atomic<unsigned> followup_calls{0};

std::string digest(char value) { return "sha256:" + std::string(64, value); }

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:model-semantic-migration-probe",
                              {},
                              {},
                              {}};
}

class WorkNode final : public GraphNode {
public:
    explicit WorkNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++work_calls;
        auto timer = asio::steady_timer(co_await asio::this_coro::executor);
        timer.expires_after(std::chrono::milliseconds(100));
        co_await timer.async_wait(asio::use_awaitable);
        co_return NodeOutput{{ChannelWrite{"value", "source-work-completed"}}};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class FollowupNode final : public GraphNode {
public:
    explicit FollowupNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<NodeOutput> run(NodeInput) override {
        ++followup_calls;
        co_return NodeOutput{};
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "semantic.short-blocking", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WorkNode>(name);
        },
        json{{"type", "object"}},
        json{{"reads", json::array()}, {"writes", json::array({"value"})},
             {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "semantic.followup", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FollowupNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "semantic.overwrite", '3'),
                        [](const json&, const json& incoming) { return incoming; });
    return std::move(builder).build();
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("model-semantic-migration-probe-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Strict)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy_snapshot(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("model-semantic-migration-probe-policy")
        .semantic_version("1.0.0")
        .owner_scope("model-semantic-migration-probe")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{60000, 10000, 1000000, 4, 32, 100, 4, 4, 32})
        .minimum_execution_guarantee(ExecutionGuarantee::Strict);
    return std::move(builder).build();
}

RunBudget runtime_budget() { return RunBudget{10000, 0, 0, 1, 32, 20, 0, 0, 0}; }

json budget_requirements() {
    return json::array({
        json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
        json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 32}},
        json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
        json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

ProgramSource source_program() {
    const json definition = {
        {"schema_version", 1},
        {"name", "main"},
        {"channels", {{"value", {{"reducer", "semantic.overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"work", {{"type", "semantic.short-blocking"}}},
                   {"followup", {{"type", "semantic.followup"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                json{{"from", "work"}, {"to", "followup"}},
                                json{{"from", "followup"}, {"to", "__end__"}}})},
    };
    const json document = {
        {"program_schema_version", LATEST_PROGRAM_SCHEMA_VERSION},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", definition}}},
        {"declared_budget_requirements", budget_requirements()},
    };
    return ProgramSource::from_cpp_builder("semantic-migration-source", LATEST_PROGRAM_SCHEMA_VERSION,
                                           document);
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
    const auto source = buffer.str();
    if (source.empty()) throw std::invalid_argument("QuickJS source is required on stdin");
    return source;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        work_calls = 0;
        followup_calls = 0;
        const auto registry = registry_snapshot();
        const auto profile = admission_profile(registry);
        const auto policy = policy_snapshot(profile);
        auto store = std::make_shared<InMemoryProgramStore>();
        auto catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
            store, registry, std::make_shared<EngineGenerationCache>(),
            "model-semantic-migration-probe/v1"});
        auto compiler = std::make_shared<ProgramCompiler>(
            registry, ProgramCompilerConfig{"model-semantic-migration-probe/v1"});

        const auto source_bundle = compiler->compile(source_program());
        const auto source_version = catalog->admit(
            source_bundle, ProgramAdmission{"model-semantic-migration-probe", profile, policy, {}});
        const auto model_source = ProgramSource::from_javascript(
            "model-semantic-migration-target.js", read_source(argc, argv));
        ProgramSynthesisProposalData proposal_data;
        proposal_data.owner_scope = "model-semantic-migration-probe";
        proposal_data.lineage_id = digest('a');
        proposal_data.parent_run_id = "model-semantic-migration-parent";
        proposal_data.source = model_source;
        proposal_data.requested_budget = runtime_budget();
        proposal_data.created_at_ms = 1;
        const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));
        ProgramSynthesisGateway gateway({
            compiler,
            catalog,
            [](const ProgramSynthesisProposal& value) {
                auto before = runtime_budget();
                before.max_dynamic_compiles = 1;
                auto after = before;
                after.max_dynamic_compiles = 0;
                return ProgramSynthesisReservation::create(
                    {value.id(), value.data().lineage_id, digest('b'), digest('c'), before, after});
            },
            [profile, policy](const ProgramSynthesisProposal&, const ProgramBundle&,
                              const ProgramSynthesisReservation&) {
                return ProgramAdmission{"model-semantic-migration-probe", profile, policy, {}};
            },
            64 * 1024});
        const auto synthesized = gateway.synthesize(proposal);
        const auto stored_source = store->get_bundle(
            "model-semantic-migration-probe", source_version.bundle_id());
        if (!stored_source) throw std::runtime_error("Source bundle was not admitted");
        if (synthesized.bundle.control_source()) {
            throw std::runtime_error("Semantic migration target must not export runtime control");
        }
        const auto baseline_plan = MigrationPlan::between(
            source_version, *stored_source, synthesized.version, synthesized.bundle);
        const auto adapter = GraphSemanticMigrationAdapter::prepare(
            source_version, *stored_source, synthesized.version, synthesized.bundle);

        auto transitions = std::make_shared<InMemoryProgramTransitionStore>();
        ProgramRuntime runtime(RuntimeConfig{
            catalog, std::make_shared<InMemoryCheckpointStore>(), {}, transitions, 1});
        auto source = runtime.start(
            "model-semantic-migration-probe", source_version,
            ProgramInvocation{json::object(), runtime_budget(), "model-semantic-migration-source", {}});
        auto target = runtime.migrate_graph(
            source, ProgramGraphMigrationTarget{synthesized.version.id(),
                                                "model-semantic-migration-target", {}, adapter});
        const auto result = target.wait();
        const auto source_result = source.wait();
        const auto lineage = transitions->load_run_lineage(
            "model-semantic-migration-probe", source.run_id());
        const auto generation = lineage
            ? transitions->load_generation("model-semantic-migration-probe", lineage->lineage_id(), 2)
            : std::nullopt;
        const auto receipt = generation ? generation->graph_migration_receipt() : std::nullopt;
        const bool verified = !baseline_plan.is_compatible() &&
                              result.status() == ProgramTerminalStatus::Completed &&
                              work_calls == 1 && followup_calls == 1 && lineage &&
                              lineage->active_generation() == 2 && generation && receipt &&
                              receipt->semantic_adapter() &&
                              receipt->semantic_adapter()->id() == adapter.id() &&
                              generation->program_version_id() == synthesized.version.id();
        const json report = {
            {"model_source_hash", model_source.source_hash()},
            {"proposal_id", proposal.id()},
            {"reservation_id", synthesized.reservation.id()},
            {"target_bundle_id", synthesized.bundle.id()},
            {"target_program_version_id", synthesized.version.id()},
            {"semantic_adapter_id", adapter.id()},
            {"baseline_compatibility", to_string(baseline_plan.compatibility())},
            {"runtime", {{"source_run_id", source.run_id()},
                         {"source_terminal_status", to_string(source_result.status())},
                         {"target_run_id", target.run_id()},
                         {"target_terminal_status", to_string(result.status())},
                         {"active_generation", lineage ? lineage->active_generation() : 0},
                         {"node_calls", {{"work", work_calls.load()},
                                         {"followup", followup_calls.load()}}},
                         {"verified", verified}}},
            {"verified", verified},
        };
        std::cout << report.dump() << '\n';
        return verified ? 0 : 1;
    } catch (const ProgramCompileError& error) {
        json diagnostics = json::array();
        for (const auto& diagnostic : error.diagnostics()) {
            diagnostics.push_back({{"code", diagnostic.code}, {"message", diagnostic.message},
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
