// Warm single-call_core Program overhead benchmark.
//
// Compares the same three-node sequential Core graph when invoked directly and
// through an admitted Program. Compilation, admission, and warm-up happen before
// timing; Program timing intentionally includes its runtime envelope, journal,
// checkpoint, and result construction.
//
// Usage: bench_program [iterations]

#include <neograph/neograph.h>
#include <neograph/program/program.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char value) {
    return "sha256:" + std::string(64, value);
}

class IncrementNode final : public GraphNode {
public:
    explicit IncrementNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto current = input.state.get("counter");
        const auto value   = current.is_number_integer() ? current.get<int>() : 0;
        co_return  NodeOutput{{ChannelWrite{"counter", value + 1}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
    return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                              EffectMode::Brokered,
                              "attestation:bench-program",
                              {},
                              {},
                              {}};
}

RegistrySnapshot registry_snapshot() {
    RegistrySnapshotBuilder builder;
    builder.add_node(
        manifest(ExecutableKind::Node, "bench-program-inc", '1'),
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncrementNode>(name);
        },
        json{{"type", "object"}, {"additionalProperties", false}},
        json{{"writes", json::array({"counter"})}});
    builder.add_reducer(manifest(ExecutableKind::Reducer, "overwrite", '2'),
                        [](const json&, const json& incoming) { return incoming; });
    return std::move(builder).build();
}

json core_definition() {
    return {
        {"schema_version", 1},
        {"name", "bench-program-seq"},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes",
         {{"a", {{"type", "bench-program-inc"}}},
          {"b", {{"type", "bench-program-inc"}}},
          {"c", {{"type", "bench-program-inc"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "a"}},
                               {{"from", "a"}, {"to", "b"}},
                               {{"from", "b"}, {"to", "c"}},
                               {{"from", "c"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
}

json budget_requirements() {
    return json::array({
        {{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
        {{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
        {{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
        {{"resource", "max_core_steps"}, {"minimum", 3}, {"maximum", 20}},
        {{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
        {{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
    });
}

json program_document() {
    return {
        {"program_schema_version", 1},
        {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
        {"root",
         {{"op", "call_core"}, {"name", "bench-program-seq"}, {"definition", core_definition()}}},
        {"declared_budget_requirements", budget_requirements()},
    };
}

AdmissionProfile admission_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("bench-program-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(1)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities())
        builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot policy_snapshot(const AdmissionProfile& profile) {
    PolicySnapshotBuilder builder;
    builder.id("bench-program-policy")
        .semantic_version("1.0.0")
        .owner_scope("bench-program")
        .admission_profile(profile)
        .budget_ceiling(BudgetLimits{10000, 1, 1, 1, 1, 20, 1, 1, 1});
    return std::move(builder).build();
}

struct BenchResult {
    double total_ms;
    double per_iter_us;
};

template <class Operation>
BenchResult measure(int iterations, Operation&& operation) {
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < iterations; ++index)
        operation();
    const auto total_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return {total_ms, total_ms * 1000.0 / iterations};
}

}  // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 1000;
    if (iterations <= 0) throw std::invalid_argument("iterations must be positive");

    NodeFactory::instance().register_type(
        "bench-program-inc", [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<IncrementNode>(name);
        });
    auto core = GraphEngine::compile(core_definition(), NodeContext{});

    const auto      registry = registry_snapshot();
    const auto      profile  = admission_profile(registry);
    const auto      policy   = policy_snapshot(profile);
    ProgramCompiler compiler(registry, {"bench-program/v1"});
    const auto source = ProgramSource::from_cpp_builder("bench:program", 1, program_document());
    const auto bundle = [&] {
        try {
            return compiler.compile(source);
        } catch (const ProgramCompileError& error) {
            for (const auto& diagnostic : error.diagnostics()) {
                std::cerr << diagnostic.code << ' ' << diagnostic.primary.json_pointer << ": "
                          << diagnostic.message << '\n';
            }
            throw;
        }
    }();
    auto store = std::make_shared<InMemoryProgramStore>();
    auto cache = std::make_shared<EngineGenerationCache>();
    auto catalog =
        std::make_shared<ProgramCatalog>(CatalogConfig{store, registry, cache, "bench-program/v1"});
    const auto version =
        catalog->admit(bundle, ProgramAdmission{"bench-program", profile, policy, {}});
    auto           checkpoints = std::make_shared<InMemoryCheckpointStore>();
    auto           transitions = std::make_shared<InMemoryProgramTransitionStore>();
    ProgramRuntime runtime(RuntimeConfig{catalog, checkpoints, {}, transitions, 1});
    const RunBudget budget{10000, 0, 0, 1, 1, 20, 0, 0, 0};

    for (int index = 0; index < 10; ++index) {
        (void)core->run(RunConfig{});
        const auto result = runtime.run(
            "bench-program",
            version,
            ProgramInvocation{json::object(), budget, "bench-program-warmup", {}});
        if (result.status() != ProgramTerminalStatus::Completed) {
            throw std::runtime_error("Program warm-up did not complete");
        }
    }

    const auto direct  = measure(iterations, [&] { (void)core->run(RunConfig{}); });
    const auto wrapped = measure(iterations, [&] {
        const auto result = runtime.run(
            "bench-program",
            version,
            ProgramInvocation{json::object(), budget, "bench-program", {}});
        if (result.status() != ProgramTerminalStatus::Completed) {
            throw std::runtime_error("Program benchmark run did not complete");
        }
    });

    std::cout << "workload\titerations\ttotal_ms\tper_iter_us\n"
              << "core_seq\t" << iterations << '\t' << direct.total_ms << '\t' << direct.per_iter_us
              << '\n'
              << "program_single_call_core\t" << iterations << '\t' << wrapped.total_ms << '\t'
              << wrapped.per_iter_us << '\n'
              << "program_overhead_ratio\t" << wrapped.per_iter_us / direct.per_iter_us << '\n';
    return EXIT_SUCCESS;
}
