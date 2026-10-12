// Shared setup for the C++ Program API examples (63 and 65 to 69).
//
// A Program needs five things before an example's own story starts: node types that
// run, an immutable registry snapshot that names them, an admission profile and a
// policy that bound what may be admitted, a catalog that stores admitted versions,
// and a ProgramRuntime that runs them. This header builds all five, so each example
// shows only what it teaches:
//
//   examples::program::Registry registry;
//   registry.node("demo.work", [](const std::string& name, const json&, const NodeInput& in) {
//       return NodeOutput{{ChannelWrite{"value", 1}}};
//   });
//   examples::program::Host host(std::move(registry).build());
//   const auto version = host.compile_admit_javascript("demo.js", javascript, budget);
//   const auto result  = host.run(version, json::object(), budget);
//
// Link: neograph::program. JavaScript sources need a build with
// -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON.
#pragma once

#include <neograph/neograph.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace examples::program {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

// A stand-in implementation digest in the shape of a SHA-256. A deployment pins the
// digest of its own build here; the examples only need a stable, distinct value.
inline std::string digest(std::string_view label) {
    std::ostringstream out;
    out << "sha256:" << std::hex << std::setfill('0');
    for (std::uint64_t lane = 0; lane < 4; ++lane) {
        std::uint64_t hash = 1469598103934665603ULL ^ (lane * 0x9E3779B97F4A7C15ULL);
        for (const unsigned char c : label) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        out << std::setw(16) << hash;
    }
    return out.str();
}

inline ExecutableManifest manifest(ExecutableKind kind, const std::string& name) {
    return ExecutableManifest{{kind, name, "1.0.0", digest(name)},
                              EffectMode::Brokered,
                              "attestation:examples",
                              {},
                              {},
                              {}};
}

// ---- Nodes and the registry ---------------------------------------------------------------

// A node whose body is a lambda: (node name, node config, input) -> channel writes.
class FunctionNode final : public SyncGraphNode {
public:
    using Body = std::function<NodeOutput(const std::string&, const json&, const NodeInput&)>;

    FunctionNode(std::string name, json config, Body body)
        : SyncGraphNode(std::move(name)), config_(std::move(config)), body_(std::move(body)) {}

protected:
    NodeOutput run_sync(NodeInput in) override { return body_(get_name(), config_, in); }

private:
    json config_;
    Body body_;
};

// Collects the node types and conditions an example registers, then seals them into the
// immutable RegistrySnapshot a Program is compiled against. The `append` and `overwrite`
// reducers are always included.
class Registry {
public:
    // `effects` declares which channels the node type reads, writes and exports; it is only
    // needed for Programs written as C++ builder JSON (JavaScript Programs may omit it).
    Registry& node(const std::string& type, FunctionNode::Body body,
                   json effects = json::object()) {
        builder_.add_node(
            manifest(ExecutableKind::Node, type),
            [body = std::move(body)](const std::string& name, const json& config,
                                     const NodeContext&) {
                return std::make_unique<FunctionNode>(name, config, body);
            },
            json{{"type", "object"}}, std::move(effects));
        return *this;
    }

    Registry& condition(const std::string& name, std::vector<std::string> labels,
                        std::function<std::string(const GraphState&)> decide) {
        builder_.add_condition(manifest(ExecutableKind::Condition, name), std::move(decide),
                               ConditionSpec{std::move(labels), false});
        return *this;
    }

    RegistrySnapshot build() && {
        for (const char* name : {"append", "overwrite"})
            builder_.add_reducer(manifest(ExecutableKind::Reducer, name),
                                 ReducerRegistry::instance().get(name));
        return std::move(builder_).build();
    }

private:
    RegistrySnapshotBuilder builder_;
};

// ---- Budgets ------------------------------------------------------------------------------

// A run budget. compile_admit seals this exact budget; run() must pass the same one.
inline RunBudget budget(std::uint64_t wall_time_ms = 10'000, std::uint64_t model_tokens = 0,
                        std::uint64_t monetary_microunits = 0) {
    RunBudget value;
    value.wall_time_ms = wall_time_ms;
    value.model_tokens = model_tokens;
    value.monetary_microunits = monetary_microunits;
    value.max_concurrency = 4;
    value.max_program_operations = 16;
    value.max_core_steps = 50;
    return value;
}

// The most any run of a host may be granted. Every resource except the two child limits must
// be positive, even for a Program that never calls a model.
inline BudgetLimits default_ceiling() {
    return BudgetLimits{120'000, 1, 1, 8, 64, 2'000, 1, 0, 0};
}

// ---- The host: registry, admission, catalog and runtime ------------------------------------

struct Stores {
    std::shared_ptr<ProgramStore> program = std::make_shared<InMemoryProgramStore>();
    std::shared_ptr<ProgramTransitionStore> transitions =
        std::make_shared<InMemoryProgramTransitionStore>();
    std::shared_ptr<CheckpointStore> checkpoints = std::make_shared<InMemoryCheckpointStore>();
};

struct HostOptions {
    std::string owner = "tenant:example";
    BudgetLimits ceiling = default_ceiling();
    std::size_t scheduler_threads = 1;
    std::size_t worker_count = 1;
    std::string compiler_build_id = "neograph-cpp-examples/v1";
    bool soundness_check = false;
};

class Host {
public:
    explicit Host(RegistrySnapshot registry, HostOptions options = {}, Stores stores = {})
        : registry_(std::move(registry)),
          options_(std::move(options)),
          stores_(std::move(stores)),
          profile_(make_profile(registry_)),
          policy_(make_policy(profile_, options_)),
          compiler_(registry_, make_compiler_config(options_)) {
        CatalogConfig catalog_config{stores_.program, registry_,
                                     std::make_shared<EngineGenerationCache>(),
                                     options_.compiler_build_id};
        catalog_config.worker_count = options_.worker_count;
        catalog_ = std::make_shared<ProgramCatalog>(std::move(catalog_config));
        RuntimeConfig config;
        config.catalog = catalog_;
        config.checkpoints = stores_.checkpoints;
        config.transitions = stores_.transitions;
        config.scheduler_threads = options_.scheduler_threads;
        runtime_ = std::make_unique<ProgramRuntime>(std::move(config));
    }

    // Compile and admit a Program written as C++ builder JSON (its budget is in the document).
    ProgramVersion compile_admit(const ProgramSource& source) {
        return admit(compiler_.compile(source));
    }

    // Compile and admit a JavaScript Program under a run budget. The budget is sealed into the
    // version as both its minimum and its maximum.
    ProgramVersion compile_admit_javascript(std::string source_id, std::string text,
                                            const RunBudget& run_budget) {
        return admit(compile_javascript(std::move(source_id), std::move(text), run_budget));
    }

    // Compile only: the bundle carries the compiler's warnings (compile_diagnostics()).
    ProgramBundle compile_javascript(std::string source_id, std::string text,
                                     const RunBudget& run_budget) const {
        return compiler_.compile(ProgramSource::from_javascript(std::move(source_id), std::move(text)),
                                 run_budget);
    }

    ProgramResult run(const ProgramVersion& version, json input, const RunBudget& run_budget) {
        return runtime_->run(options_.owner, version,
                             ProgramInvocation{std::move(input), run_budget, "example", {}});
    }

    // Answer the pending question of an Interrupted result and wait for the run to continue.
    ProgramResult resume(const ProgramResult& paused, json value) {
        return runtime_->resume(options_.owner, paused.run_id(),
                                ProgramResume{std::move(value), "example-resume", {}, pending_id(paused)})
            .wait();
    }

    // Start a second run from the paused run's exact checkpoint and answer its question there.
    // `share` is carved out of what the paused run has left.
    ProgramResult fork(const ProgramResult& paused, json value, const ProgramVersion& version,
                       const RunBudget& share) {
        return runtime_
            ->fork(options_.owner,
                   ExactProgramCheckpointReference{paused.run_id(), paused.checkpoint()->checkpoint_id},
                   version, ProgramInvocation{json::object(), share, "example-fork", {}},
                   ProgramResume{std::move(value), "example-fork-resume", {}, pending_id(paused)})
            .wait();
    }

    ProgramRuntime& runtime() { return *runtime_; }
    const std::string& owner() const { return options_.owner; }

private:
    static std::string pending_id(const ProgramResult& paused) {
        if (!paused.interrupt() || !paused.interrupt()->pending_input)
            throw std::logic_error("the result is not an interrupted run with a pending question");
        return paused.interrupt()->pending_input->call_id();
    }

    ProgramVersion admit(const ProgramBundle& bundle) {
        return catalog_->admit(bundle, ProgramAdmission{options_.owner, profile_, policy_, {}});
    }

    static AdmissionProfile make_profile(const RegistrySnapshot& registry) {
        AdmissionProfileBuilder builder;
        builder.id("examples-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
            .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_source_kind(SourceKind::JavaScript)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry.identities()) builder.allow_executable(identity);
        return std::move(builder).build();
    }

    static PolicySnapshot make_policy(const AdmissionProfile& profile, const HostOptions& options) {
        PolicySnapshotBuilder builder;
        builder.id("examples-policy")
            .semantic_version("1.0.0")
            .owner_scope(options.owner)
            .admission_profile(profile)
            .budget_ceiling(options.ceiling)
            .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
        return std::move(builder).build();
    }

    static ProgramCompilerConfig make_compiler_config(const HostOptions& options) {
        ProgramCompilerConfig config{options.compiler_build_id, {}};
        config.soundness_check = options.soundness_check;
        return config;
    }

    RegistrySnapshot registry_;
    HostOptions options_;
    Stores stores_;
    AdmissionProfile profile_;
    PolicySnapshot policy_;
    ProgramCompiler compiler_;
    std::shared_ptr<ProgramCatalog> catalog_;
    std::unique_ptr<ProgramRuntime> runtime_;
};

}  // namespace examples::program
