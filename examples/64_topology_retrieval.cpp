// NeoGraph Example 64: Retrieved topology candidate -> verified P1 migration.
//
// A production retriever may be the local Faiss + BM25 + RRF + reranker
// cookbook. It returns only ordered TopologyVersionId candidates. This example
// is the NeoGraph-side consumer: resolve the exact admitted artifacts, prove
// compatibility, prepare GraphSemanticMigrationAdapter when permitted, and
// transition one running lineage at a durable GraphEngine safe point.
//
// Build: cmake --build build --target example_topology_retrieval
// Run:   ./example_topology_retrieval

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
                              "attestation:topology-retrieval-example",
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
        manifest(ExecutableKind::Node, "retrieval.short-blocking", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WorkNode>(name);
        },
        json{{"type", "object"}},
        json{{"reads", json::array()}, {"writes", json::array({"value"})},
             {"exports", json::array({"value"})}});
    builder.add_node(
        manifest(ExecutableKind::Node, "retrieval.followup", '2'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<FollowupNode>(name);
        },
        json{{"type", "object"}}, json::object());
    builder.add_reducer(manifest(ExecutableKind::Reducer, "retrieval.overwrite", '3'),
                        [](const json&, const json& incoming) { return incoming; });
    return std::move(builder).build();
}

AdmissionProfile profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("topology-retrieval-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Strict)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("topology-retrieval-policy")
        .semantic_version("1.0.0")
        .owner_scope("topology-retrieval")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32})
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

ProgramSource source_program(std::uint64_t migration_epoch) {
    const json definition = {
        {"schema_version", 1},
        {"name", "main"},
        {"channels", {{"value", {{"reducer", "retrieval.overwrite"}, {"initial", ""}}}}},
        {"nodes", {{"work", {{"type", "retrieval.short-blocking"},
                                  {"migration_epoch", migration_epoch}}},
                   {"followup", {{"type", "retrieval.followup"}}}}},
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
    return ProgramSource::from_cpp_builder("topology-retrieval-example", LATEST_PROGRAM_SCHEMA_VERSION,
                                           document);
}

struct RerankedTopologyCandidate {
    std::string topology_version_id;
    double      dense_score = 0.0;
    double      bm25_score  = 0.0;
    double      rrf_score   = 0.0;
    double      rerank_score = 0.0;
};

}  // namespace

int main() {
    try {
        work_calls = 0;
        followup_calls = 0;
        const auto registry = registry_snapshot();
        const auto admission_profile = profile(registry);
        const auto admission_policy = policy(admission_profile);
        auto store = std::make_shared<InMemoryProgramStore>();
        auto catalog = std::make_shared<ProgramCatalog>(CatalogConfig{
            store, registry, std::make_shared<EngineGenerationCache>(),
            "topology-retrieval-example/v1"});
        ProgramCompiler compiler(registry, {"topology-retrieval-example/v1"});

        const auto source_bundle = compiler.compile(source_program(1));
        const auto source_version = catalog->admit(
            source_bundle, ProgramAdmission{"topology-retrieval", admission_profile, admission_policy, {}});
        const auto target_bundle = compiler.compile(source_program(2));
        const auto target_version = catalog->admit(
            target_bundle, ProgramAdmission{"topology-retrieval", admission_profile, admission_policy, {}});

        // This is the exact contract that an external dense + BM25 -> RRF ->
        // rerank service returns. It contains an immutable candidate reference,
        // not graph JSON, a capability grant, or a command to execute code.
        const std::vector<RerankedTopologyCandidate> reranked = {
            {target_version.id(), 0.93, 12.4, 0.0328, 0.97},
        };
        const auto& selected = reranked.front();
        const auto candidate = catalog->resolve_version("topology-retrieval", selected.topology_version_id);
        if (!candidate) throw std::runtime_error("Retrieved TopologyVersionId was not admitted");
        const auto stored_source = store->get_bundle("topology-retrieval", source_version.bundle_id());
        const auto stored_target = store->get_bundle("topology-retrieval", candidate->bundle_id());
        if (!stored_source || !stored_target) throw std::runtime_error("Retrieved bundle was not admitted");

        // A normal migration plan correctly rejects changed materialization.
        // Only this explicit, host-prepared P1 adapter can prove the identity
        // projection of channels/frontier/barriers for the successor.
        const auto baseline = catalog->plan_migration(
            "topology-retrieval", source_version.id(), candidate->id());
        const auto adapter = GraphSemanticMigrationAdapter::prepare(
            source_version, *stored_source, *candidate, *stored_target);

        auto transitions = std::make_shared<InMemoryProgramTransitionStore>();
        ProgramRuntime runtime(RuntimeConfig{
            catalog, std::make_shared<InMemoryCheckpointStore>(), {}, transitions, 1});
        auto source = runtime.start(
            "topology-retrieval", source_version,
            ProgramInvocation{json::object(), runtime_budget(), "topology-retrieval-source", {}});
        auto target = runtime.migrate_graph(
            source, ProgramGraphMigrationTarget{candidate->id(), "topology-retrieval-target", {}, adapter});
        const auto result = target.wait();
        const auto source_result = source.wait();
        const auto lineage = transitions->load_run_lineage("topology-retrieval", source.run_id());
        const bool verified = !baseline.is_compatible() &&
                              result.status() == ProgramTerminalStatus::Completed &&
                              work_calls == 1 && followup_calls == 1 && lineage &&
                              lineage->active_generation() == 2;
        std::cout << json{
            {"retrieval_contract", "dense+bm25->rrf->rerank returns only TopologyVersionId"},
            {"candidate", {{"topology_version_id", selected.topology_version_id},
                           {"dense_score", selected.dense_score}, {"bm25_score", selected.bm25_score},
                           {"rrf_score", selected.rrf_score}, {"rerank_score", selected.rerank_score}}},
            {"baseline_compatibility", to_string(baseline.compatibility())},
            {"semantic_adapter_id", adapter.id()},
            {"source_terminal_status", to_string(source_result.status())},
            {"target_terminal_status", to_string(result.status())},
            {"active_generation", lineage ? lineage->active_generation() : 0},
            {"node_calls", {{"work", work_calls.load()}, {"followup", followup_calls.load()}}},
            {"verified", verified},
        }.dump() << '\n';
        return verified ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << json{{"error", error.what()}}.dump() << '\n';
        return 2;
    }
}
