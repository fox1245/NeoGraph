#include "json_bridge.h"
#include "provider_bridge.h"

#include <neograph/graph/loader.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;
using namespace pybind11::literals;

namespace neograph::pybind {
namespace {

using namespace neograph::graph;
using namespace neograph::program;

ExecutableManifest registered_manifest(ExecutableKind kind,
                                       const std::string& name,
                                       std::string semantic_version,
                                       std::string implementation_digest,
                                       std::vector<std::string> capabilities,
                                       std::vector<std::string> effects,
                                       ExecutionGuarantee guarantee) {
    return ExecutableManifest{{kind, name, std::move(semantic_version),
                               std::move(implementation_digest)},
                              EffectMode::Brokered,
                              "python-registered-executable",
                              std::move(capabilities),
                              std::move(effects),
                              {},
                              guarantee};
}

std::runtime_error python_compile_error(const ProgramCompileError& error) {
    std::string message = "Program compilation failed";
    for (const auto& diagnostic : error.diagnostics()) {
        message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer +
                   ": " + diagnostic.message;
    }
    return std::runtime_error(std::move(message));
}

class ProgramRegistryBuilderBinding {
public:
    ProgramRegistryBuilderBinding()
        : builder_(std::make_unique<RegistrySnapshotBuilder>()) {}

    ProgramRegistryBuilderBinding& add_registered_node(
        const std::string& type,
        std::string semantic_version,
        std::string implementation_digest,
        std::vector<std::string> capabilities,
        std::vector<std::string> effects,
        ExecutionGuarantee guarantee) {
        require_open();
        auto schema = NodeFactory::instance().config_schema(type);
        auto node_effects = NodeFactory::instance().node_effects(type);
        if (!schema.is_object()) schema = json{{"type", "object"}};
        if (!node_effects.is_object()) node_effects = json::object();
        builder_->add_node(
            registered_manifest(ExecutableKind::Node, type, std::move(semantic_version),
                                std::move(implementation_digest), std::move(capabilities),
                                std::move(effects), guarantee),
            [type](const std::string& name, const json& config, const NodeContext& context) {
                return NodeFactory::instance().create(type, name, config, context);
            },
            std::move(schema), std::move(node_effects));
        return *this;
    }

    ProgramRegistryBuilderBinding& add_registered_reducer(
        const std::string& name,
        std::string semantic_version,
        std::string implementation_digest) {
        require_open();
        auto reducer = ReducerRegistry::instance().get(name);
        builder_->add_reducer(
            registered_manifest(ExecutableKind::Reducer, name, std::move(semantic_version),
                                std::move(implementation_digest), {}, {},
                                ExecutionGuarantee::Strict),
            std::move(reducer));
        return *this;
    }

    ProgramRegistryBuilderBinding& add_registered_condition(
        const std::string& name,
        std::string semantic_version,
        std::string implementation_digest) {
        require_open();
        auto condition = ConditionRegistry::instance().get(name);
        auto spec = ConditionRegistry::instance().condition_spec(name);
        builder_->add_condition(
            registered_manifest(ExecutableKind::Condition, name, std::move(semantic_version),
                                std::move(implementation_digest), {}, {},
                                ExecutionGuarantee::Strict),
            std::move(condition), std::move(spec));
        return *this;
    }

    RegistrySnapshot build() {
        require_open();
        auto builder = std::move(*builder_);
        builder_.reset();
        return std::move(builder).build();
    }

private:
    void require_open() const {
        if (!builder_) throw std::logic_error("ProgramRegistryBuilder is already sealed");
    }

    std::unique_ptr<RegistrySnapshotBuilder> builder_;
};

BudgetLimits budget_limits(const RunBudget& value) {
    if (value.max_dynamic_compiles > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("max_dynamic_compiles exceeds uint32 range");
    }
    return {value.wall_time_ms,
            value.model_tokens,
            value.monetary_microunits,
            value.max_concurrency,
            value.max_program_operations,
            value.max_core_steps,
            static_cast<std::uint32_t>(value.max_dynamic_compiles),
            value.max_child_depth,
            value.max_total_children};
}

AdmissionProfile local_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("python-local-program-profile")
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

PolicySnapshot local_policy(const RegistrySnapshot& registry,
                            const AdmissionProfile& profile,
                            const std::string& owner_scope,
                            const RunBudget& ceiling) {
    PolicySnapshotBuilder builder;
    builder.id("python-local-program-policy")
        .semantic_version("1.0.0")
        .owner_scope(owner_scope)
        .admission_profile(profile)
        .budget_ceiling(budget_limits(ceiling))
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    std::vector<std::string> capabilities;
    std::vector<std::string> effects;
    for (const auto& identity : registry.identities()) {
        const auto manifest = registry.find(identity.kind, identity.name);
        if (!manifest) continue;
        capabilities.insert(capabilities.end(), manifest->required_capabilities.begin(),
                            manifest->required_capabilities.end());
        effects.insert(effects.end(), manifest->declared_effects.begin(),
                       manifest->declared_effects.end());
    }
    std::sort(capabilities.begin(), capabilities.end());
    capabilities.erase(std::unique(capabilities.begin(), capabilities.end()), capabilities.end());
    std::sort(effects.begin(), effects.end());
    effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
    for (auto& capability : capabilities) builder.allow_capability(std::move(capability));
    for (auto& effect : effects) builder.allow_effect(std::move(effect));
    return std::move(builder).build();
}

class LocalProgramHost {
public:
    LocalProgramHost(RegistrySnapshot registry,
                     std::string owner_scope,
                     RunBudget budget_ceiling,
                     std::string compiler_build_id,
                     std::size_t scheduler_threads,
                     std::shared_ptr<sp::NativeArchive> native_history_archive)
        : registry_(std::move(registry)),
          owner_scope_(std::move(owner_scope)),
          compiler_build_id_(std::move(compiler_build_id)),
          profile_(local_profile(registry_)),
          policy_(local_policy(registry_, profile_, owner_scope_, budget_ceiling)),
          compiler_(std::make_shared<ProgramCompiler>(
              registry_, ProgramCompilerConfig{compiler_build_id_})),
          store_(std::make_shared<InMemoryProgramStore>()),
          catalog_(std::make_shared<ProgramCatalog>(CatalogConfig{
              store_, registry_, std::make_shared<EngineGenerationCache>(), compiler_build_id_})),
          checkpoints_(std::make_shared<InMemoryCheckpointStore>()),
          transitions_(std::make_shared<InMemoryProgramTransitionStore>()) {
        RuntimeConfig config;
        config.catalog = catalog_;
        config.checkpoints = checkpoints_;
        config.transitions = transitions_;
        config.scheduler_threads = scheduler_threads;
        config.native_history_archive = std::move(native_history_archive);
        runtime_ = std::make_unique<ProgramRuntime>(std::move(config));
    }

    ~LocalProgramHost() {
        // Runtime teardown cancels and joins scheduler work. A Python node on
        // that work may need the GIL before it can finish. Other host members
        // are destroyed after this scope, with the caller's GIL restored.
        if (PyGILState_Check()) {
            py::gil_scoped_release release;
            runtime_.reset();
        } else {
            runtime_.reset();
        }
    }

    ProgramBundle compile(const ProgramSource& source, const RunBudget& budget) const {
        try {
            return compiler_->compile(source, budget);
        } catch (const ProgramCompileError& error) {
            throw python_compile_error(error);
        }
    }

    ProgramVersion admit(const ProgramBundle& bundle) {
        return catalog_->admit(
            bundle, ProgramAdmission{owner_scope_, profile_, policy_, {}});
    }

    ProgramVersion compile_admit(const ProgramSource& source, const RunBudget& budget) {
        return admit(compile(source, budget));
    }
    ProgramActivationResult activate(const ProgramVersion& version,
                                     std::uint64_t expected_generation) {
        return catalog_->activate(owner_scope_, version.id(), expected_generation);
    }

    ProgramActivationResult rollback(const ProgramVersion& version,
                                     std::uint64_t expected_generation) {
        return catalog_->rollback(owner_scope_, version.id(), expected_generation);
    }

    std::optional<ProgramActivation> activation() const {
        return catalog_->activation(owner_scope_);
    }

    std::pair<ProgramActivation, ProgramHandle> start_active(
        py::object input, const RunBudget& budget, std::string trace_id) {
        RunInvocation request;
        request.owner_scope = owner_scope_;
        request.agent_id = "python-local";
        request.run_id = Checkpoint::generate_id();
        request.budget = budget;
        request.input = py_to_json(input);
        request.message_sequence = 1;
        request.idempotency_key = request.run_id;
        request.correlation_id = std::move(trace_id);
        auto selected = [&] {
            py::gil_scoped_release release;
            return runtime_->start_active(std::move(request));
        }();
        return {std::move(selected.activation), std::move(selected.handle)};
    }


    ProgramResult run(const ProgramVersion& version,
                      py::object input,
                      const RunBudget& budget,
                      std::string trace_id) {
        auto owned_input = py_to_json(input);
        py::gil_scoped_release release;
        return runtime_->run(owner_scope_, version,
                             ProgramInvocation{std::move(owned_input), budget,
                                               std::move(trace_id), {}});
    }

    ProgramHandle start(const ProgramVersion& version,
                        py::object input,
                        const RunBudget& budget,
                        std::string trace_id) {
        auto owned_input = py_to_json(input);
        py::gil_scoped_release release;
        return runtime_->start(owner_scope_, version,
                               ProgramInvocation{std::move(owned_input), budget,
                                                 std::move(trace_id), {}});
    }

    ProgramHandle resume(std::string run_id,
                         py::object value,
                         std::string pending_id,
                         std::string trace_id) {
        auto owned_value = py_to_json(value);
        py::gil_scoped_release release;
        return runtime_->resume(owner_scope_, std::move(run_id),
                                ProgramResume{std::move(owned_value), std::move(trace_id), {},
                                              std::move(pending_id)});
    }

    const std::string& owner_scope() const noexcept { return owner_scope_; }

private:
    RegistrySnapshot registry_;
    std::string owner_scope_;
    std::string compiler_build_id_;
    AdmissionProfile profile_;
    PolicySnapshot policy_;
    std::shared_ptr<ProgramCompiler> compiler_;
    std::shared_ptr<InMemoryProgramStore> store_;
    std::shared_ptr<ProgramCatalog> catalog_;
    std::shared_ptr<InMemoryCheckpointStore> checkpoints_;
    std::shared_ptr<InMemoryProgramTransitionStore> transitions_;
    std::unique_ptr<ProgramRuntime> runtime_;
};

template <typename T>
void bind_program_identity(py::class_<T>& type) {
    type.def_property_readonly("id", &T::id)
        .def("serialize_canonical", &T::serialize_canonical);
}

}  // namespace

void init_program(py::module_& m) {
    using namespace neograph::program;

    m.def("javascript_authoring_capability_manifest",
          [] { return json_to_py(neograph::program::javascript_authoring_capability_manifest()); },
          "Return the installed QuickJS graph-builder and command vocabulary.");

    py::enum_<SourceKind>(m, "ProgramSourceKind")
        .value("CanonicalJson", SourceKind::CanonicalJson)
        .value("CppBuilder", SourceKind::CppBuilder)
        .value("JavaScript", SourceKind::JavaScript);
    py::enum_<ExecutionGuarantee>(m, "ExecutionGuarantee")
        .value("Strict", ExecutionGuarantee::Strict)
        .value("Recorded", ExecutionGuarantee::Recorded)
        .value("Unmanaged", ExecutionGuarantee::Unmanaged);
    py::enum_<ProgramTerminalStatus>(m, "ProgramTerminalStatus")
        .value("Completed", ProgramTerminalStatus::Completed)
        .value("Interrupted", ProgramTerminalStatus::Interrupted)
        .value("Cancelled", ProgramTerminalStatus::Cancelled)
        .value("BudgetExhausted", ProgramTerminalStatus::BudgetExhausted)
        .value("TimedOut", ProgramTerminalStatus::TimedOut)
        .value("Failed", ProgramTerminalStatus::Failed)
        .value("AmbiguousEffect", ProgramTerminalStatus::AmbiguousEffect)
        .value("CheckpointIncompatible", ProgramTerminalStatus::CheckpointIncompatible);

    py::class_<RunBudget>(m, "ProgramRunBudget")
        .def(py::init<>())
        .def_readwrite("wall_time_ms", &RunBudget::wall_time_ms)
        .def_readwrite("model_tokens", &RunBudget::model_tokens)
        .def_readwrite("monetary_microunits", &RunBudget::monetary_microunits)
        .def_readwrite("max_concurrency", &RunBudget::max_concurrency)
        .def_readwrite("max_program_operations", &RunBudget::max_program_operations)
        .def_readwrite("max_core_steps", &RunBudget::max_core_steps)
        .def_readwrite("max_dynamic_compiles", &RunBudget::max_dynamic_compiles)
        .def_readwrite("max_child_depth", &RunBudget::max_child_depth)
        .def_readwrite("max_total_children", &RunBudget::max_total_children);

    py::class_<ProgramUsage>(m, "ProgramUsage")
        .def_readonly("wall_time_ms", &ProgramUsage::wall_time_ms)
        .def_readonly("model_tokens", &ProgramUsage::model_tokens)
        .def_readonly("monetary_microunits", &ProgramUsage::monetary_microunits)
        .def_readonly("program_operations", &ProgramUsage::program_operations)
        .def_readonly("core_steps", &ProgramUsage::core_steps)
        .def_readonly("peak_concurrency", &ProgramUsage::peak_concurrency);

    py::class_<ProgramSource> source(m, "ProgramSource");
    source.def_static("from_javascript",
                      [](std::string source_id, std::string source_text) {
                          return ProgramSource::from_javascript(
                              std::move(source_id), std::move(source_text));
                      },
                      py::arg("source_id"), py::arg("source_text"))
        .def_static("parse", &ProgramSource::parse)
        .def_property_readonly("kind", &ProgramSource::kind)
        .def_property_readonly("source_id", &ProgramSource::source_id)
        .def_property_readonly("source_hash", &ProgramSource::source_hash)
        .def_property_readonly("document", [](const ProgramSource& value) {
            return json_to_py(value.document());
        })
        .def("serialize_canonical", &ProgramSource::serialize_canonical);

    py::class_<RegistrySnapshot> registry(m, "ProgramRegistrySnapshot");
    registry.def_property_readonly("fingerprint", &RegistrySnapshot::fingerprint)
        .def_property_readonly("manifest", [](const RegistrySnapshot& value) {
            return json_to_py(value.manifest());
        })
        .def("serialize_canonical", &RegistrySnapshot::serialize_canonical);

    py::class_<ProgramRegistryBuilderBinding>(m, "ProgramRegistryBuilder")
        .def(py::init<>())
        .def("add_registered_node", &ProgramRegistryBuilderBinding::add_registered_node,
             py::arg("type_name"), py::arg("semantic_version"),
             py::arg("implementation_digest"),
             py::arg("required_capabilities") = std::vector<std::string>{},
             py::arg("declared_effects") = std::vector<std::string>{},
             py::arg("execution_guarantee") = ExecutionGuarantee::Strict,
             py::return_value_policy::reference_internal)
        .def("add_registered_reducer", &ProgramRegistryBuilderBinding::add_registered_reducer,
             py::arg("name"), py::arg("semantic_version"),
             py::arg("implementation_digest"),
             py::return_value_policy::reference_internal)
        .def("add_registered_condition", &ProgramRegistryBuilderBinding::add_registered_condition,
             py::arg("name"), py::arg("semantic_version"),
             py::arg("implementation_digest"),
             py::return_value_policy::reference_internal)
        .def("build", &ProgramRegistryBuilderBinding::build);

    py::class_<ProgramBundle> bundle(m, "ProgramBundle");
    bundle.def_static("parse", &ProgramBundle::parse)
        .def_property_readonly("id", &ProgramBundle::id)
        .def_property_readonly("source_kind", &ProgramBundle::source_kind)
        .def_property_readonly("source_hash", &ProgramBundle::source_hash)
        .def_property_readonly("execution_guarantee", &ProgramBundle::execution_guarantee)
        .def_property_readonly("sealed_core_definitions", [](const ProgramBundle& value) {
            py::list result;
            for (const auto& definition : value.sealed_core_definitions()) {
                result.append(py::dict("name"_a = definition.name,
                                       "definition_hash"_a = definition.definition_hash,
                                       "definition"_a = json_to_py(definition.definition)));
            }
            return result;
        })
        .def_property_readonly("diagnostics", [](const ProgramBundle& value) {
            py::list result;
            for (const auto& diagnostic : value.diagnostics()) {
                result.append(py::dict("code"_a = diagnostic.code,
                                       "message"_a = diagnostic.message,
                                       "witness"_a = json_to_py(diagnostic.witness)));
            }
            return result;
        })
        .def("serialize_canonical", &ProgramBundle::serialize_canonical);

    py::class_<ProgramCompiler>(m, "ProgramCompiler")
        .def(py::init([](RegistrySnapshot registry, std::string compiler_build_id) {
            return std::make_unique<ProgramCompiler>(
                std::move(registry), ProgramCompilerConfig{std::move(compiler_build_id)});
        }), py::arg("registry"), py::arg("compiler_build_id") = "neograph-python-program/v1")
        .def("compile", [](const ProgramCompiler& self, const ProgramSource& source,
                            py::object budget) {
            try {
                return budget.is_none() ? self.compile(source)
                                        : self.compile(source, budget.cast<RunBudget>());
            } catch (const ProgramCompileError& error) {
                throw python_compile_error(error);
            }
        }, py::arg("source"), py::arg("budget") = py::none());

    py::class_<ProgramVersion> version(m, "ProgramVersion");
    version.def_static("parse", &ProgramVersion::parse)
        .def_property_readonly("id", &ProgramVersion::id)
        .def_property_readonly("bundle_id", &ProgramVersion::bundle_id)
        .def_property_readonly("owner_scope", &ProgramVersion::ownership_scope)
        .def_property_readonly("execution_guarantee", &ProgramVersion::execution_guarantee)
        .def("serialize_canonical", &ProgramVersion::serialize_canonical);

    py::class_<ProgramFailure>(m, "ProgramFailure")
        .def_readonly("code", &ProgramFailure::code)
        .def_readonly("message", &ProgramFailure::message)
        .def_readonly("operation_id", &ProgramFailure::operation_id)
        .def_readonly("core_node", &ProgramFailure::core_node)
        .def_readonly("attempts", &ProgramFailure::attempts)
        .def_property_readonly("witness", [](const ProgramFailure& value) { return json_to_py(value.witness); })
        .def_readonly("provider_outcome", &ProgramFailure::provider_outcome)
        .def_property_readonly("provider_cause", [](const ProgramFailure& value) { return provider_cause(value.provider_cause); })
        .def("__eq__", [](const ProgramFailure& left, const ProgramFailure& right) { return left == right; }, py::is_operator());
    py::class_<CoreCheckpointIdentity>(m, "ProgramCoreCheckpointIdentity")
        .def_readonly("core_name", &CoreCheckpointIdentity::core_name)
        .def_readonly("core_generation_id", &CoreCheckpointIdentity::core_generation_id)
        .def_readonly("core_thread_id", &CoreCheckpointIdentity::core_thread_id)
        .def_readonly("checkpoint_id", &CoreCheckpointIdentity::checkpoint_id)
        .def_readonly("checkpoint_schema_version", &CoreCheckpointIdentity::checkpoint_schema_version);
    py::enum_<ProgramPendingInputKind>(m, "ProgramPendingInputKind")
        .value("Input", ProgramPendingInputKind::Input).value("CapabilityResult", ProgramPendingInputKind::CapabilityResult);
    py::enum_<ProgramPendingState>(m, "ProgramPendingState")
        .value("Awaiting", ProgramPendingState::Awaiting).value("Consumed", ProgramPendingState::Consumed)
        .value("Expired", ProgramPendingState::Expired).value("Cancelled", ProgramPendingState::Cancelled)
        .value("Ambiguous", ProgramPendingState::Ambiguous);
    py::class_<ProgramPendingInput>(m, "ProgramPendingInput")
        .def_property_readonly("operation_id", &ProgramPendingInput::operation_id)
        .def_property_readonly("call_id", &ProgramPendingInput::call_id)
        .def_property_readonly("kind", &ProgramPendingInput::kind)
        .def_property_readonly("state", &ProgramPendingInput::state)
        .def_property_readonly("payload", [](const ProgramPendingInput& value) { return json_to_py(value.payload()); })
        .def_property_readonly("result_schema", [](const ProgramPendingInput& value) { return json_to_py(value.result_schema()); })
        .def_property_readonly("expires_at_unix_ms", &ProgramPendingInput::expires_at_unix_ms)
        .def("serialize_canonical", &ProgramPendingInput::serialize_canonical);
    py::class_<ProgramPendingEffect>(m, "ProgramPendingEffect")
        .def_property_readonly("operation_id", &ProgramPendingEffect::operation_id)
        .def_property_readonly("call_id", &ProgramPendingEffect::call_id)
        .def_property_readonly("effect_id", &ProgramPendingEffect::effect_id)
        .def_property_readonly("state", &ProgramPendingEffect::state)
        .def_property_readonly("payload", [](const ProgramPendingEffect& value) { return json_to_py(value.payload()); })
        .def_property_readonly("result_schema", [](const ProgramPendingEffect& value) { return json_to_py(value.result_schema()); })
        .def_property_readonly("expires_at_unix_ms", &ProgramPendingEffect::expires_at_unix_ms)
        .def("serialize_canonical", &ProgramPendingEffect::serialize_canonical);
    py::class_<ProgramInterrupt>(m, "ProgramInterrupt")
        .def_readonly("core_node", &ProgramInterrupt::core_node)
        .def_property_readonly("value", [](const ProgramInterrupt& value) { return json_to_py(value.value); })
        .def_readonly("pending_input", &ProgramInterrupt::pending_input)
        .def_readonly("pending_effect", &ProgramInterrupt::pending_effect);
    py::class_<ProgramResult> result(m, "ProgramResult");
    result.def_static("parse", &ProgramResult::parse)
        .def_property_readonly("id", &ProgramResult::id)
        .def_property_readonly("status", &ProgramResult::status)
        .def_property_readonly("run_id", &ProgramResult::run_id)
        .def_property_readonly("program_version_id", &ProgramResult::program_version_id)
        .def_property_readonly("bundle_id", &ProgramResult::bundle_id)
        .def_property_readonly("operation_id", &ProgramResult::operation_id)
        .def_property_readonly("attempt", &ProgramResult::attempt)
        .def_property_readonly("checkpoint", &ProgramResult::checkpoint)
        .def_property_readonly("interrupt", &ProgramResult::interrupt)
        .def_property_readonly("provider_budget_authority", &ProgramResult::provider_budget_authority)
        .def_property_readonly("output", [](const ProgramResult& value) {
            return json_to_py(value.output());
        })
        .def_property_readonly("usage", &ProgramResult::usage)
        .def_property_readonly("remaining_budget", &ProgramResult::remaining_budget)
        .def_property_readonly("execution_trace", &ProgramResult::execution_trace)
        .def_property_readonly("failure", &ProgramResult::failure)
        .def("serialize_canonical", &ProgramResult::serialize_canonical);

    py::class_<ProgramHandle>(m, "ProgramHandle")
        .def_property_readonly("run_id", &ProgramHandle::run_id)
        .def_property_readonly("program_version_id", &ProgramHandle::program_version_id)
        .def_property_readonly("attempt", &ProgramHandle::attempt)
        .def("cancel", &ProgramHandle::cancel)
        .def("wait", &ProgramHandle::wait, py::call_guard<py::gil_scoped_release>())
        .def("try_result", &ProgramHandle::try_result);

    py::enum_<ProgramActivationResult>(m, "ProgramActivationResult")
        .value("Activated", ProgramActivationResult::Activated)
        .value("AlreadyPresent", ProgramActivationResult::AlreadyPresent)
        .value("Conflict", ProgramActivationResult::Conflict);
    py::class_<ProgramActivation>(m, "ProgramActivation")
        .def_property_readonly("owner_scope", &ProgramActivation::owner_scope)
        .def_property_readonly("active_version_id", &ProgramActivation::active_version_id)
        .def_property_readonly("generation", &ProgramActivation::generation)
        .def_property_readonly("policy_snapshot_hash", &ProgramActivation::policy_snapshot_hash)
        .def_property_readonly("id", &ProgramActivation::id)
        .def_static("parse", &ProgramActivation::parse)
        .def("serialize_canonical", &ProgramActivation::serialize_canonical);

    py::class_<LocalProgramHost>(m, "LocalProgramHost",
        "Owner-scoped in-memory Program compiler, Catalog, and durable runtime. "
        "It uses the same C++ ProgramRuntime as native callers.")
        .def(py::init<RegistrySnapshot, std::string, RunBudget, std::string, std::size_t,
                      std::shared_ptr<sp::NativeArchive>>(),
             py::arg("registry"), py::arg("owner_scope"), py::arg("budget_ceiling"),
             py::arg("compiler_build_id") = "neograph-python-program/v1",
             py::arg("scheduler_threads") = 1,
             py::arg("native_history_archive") = nullptr)
        .def_property_readonly("owner_scope", &LocalProgramHost::owner_scope)
        .def("compile", &LocalProgramHost::compile)
        .def("admit", &LocalProgramHost::admit)
        .def("compile_admit", &LocalProgramHost::compile_admit)
        .def("activate", &LocalProgramHost::activate,
             py::arg("version"), py::arg("expected_generation"))
        .def("rollback", &LocalProgramHost::rollback,
             py::arg("version"), py::arg("expected_generation"))
        .def("activation", &LocalProgramHost::activation)
        .def("start_active", &LocalProgramHost::start_active,
             py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program-active")
        .def("run", &LocalProgramHost::run,
             py::arg("version"), py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program")
        .def("start", &LocalProgramHost::start,
             py::arg("version"), py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program")
        .def("resume", &LocalProgramHost::resume,
             py::arg("run_id"), py::arg("value"), py::arg("pending_id") = "",
             py::arg("trace_id") = "python-program-resume");
}

}  // namespace neograph::pybind
