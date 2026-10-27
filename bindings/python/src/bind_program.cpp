#include "async_runtime.h"
#include "failure_text_redactor.h"
#include "json_bridge.h"
#include "provider_bridge.h"

#include <asio/co_spawn.hpp>
#include <neograph/graph/loader.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#ifdef NEOGRAPH_PYBIND_HAS_PROGRAM_SQLITE
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/sqlite_provider_call_broker.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#endif

#include "program/provider_binding.h"

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace py = pybind11;
using namespace pybind11::literals;

namespace neograph::pybind {
namespace {

using namespace neograph::graph;
using namespace neograph::program;

template <typename Outcome>
py::object spawn_program_async(asio::awaitable<Outcome> call) {
    py::object loop = py::module_::import("asyncio").attr("get_running_loop")();
    py::object future = loop.attr("create_future")();
    auto fut_h = hold_py(future);
    auto loop_h = hold_py(std::move(loop));
    asio::co_spawn(
        AsyncRuntime::executor(),
        [](asio::awaitable<Outcome> inner) -> asio::awaitable<std::optional<Outcome>> {
            co_return std::optional<Outcome>(co_await std::move(inner));
        }(std::move(call)),
        [fut_h, loop_h](std::exception_ptr error, std::optional<Outcome> outcome) {
            resolve_future_async(fut_h, loop_h, error, std::move(outcome));
        });
    return future;
}

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

// `help` (X1) is a dict for returned diagnostics and None for sealed ones. An
// exception always carries it: a diagnostic without help gets its doc link.
py::dict diagnostic_to_py(const Diagnostic& diagnostic, bool for_exception = false) {
    py::object span = py::none();
    if (const auto& value = diagnostic.primary.span) {
        span = py::dict("byte_begin"_a = value->byte_begin, "byte_end"_a = value->byte_end,
                        "line_begin"_a = value->line_begin,
                        "column_begin"_a = value->column_begin, "line_end"_a = value->line_end,
                        "column_end"_a = value->column_end);
    }
    py::object help = py::none();
    if (diagnostic.help || for_exception) {
        const auto value =
            diagnostic.help.value_or(DiagnosticHelp{diagnostic_doc_link(diagnostic.code), {}, {}, {}});
        neograph::json encoded;
        to_json(encoded, value);
        help = json_to_py(encoded);
    }
    return py::dict("code"_a = diagnostic.code,
                    "severity"_a = std::string(to_string(diagnostic.severity)),
                    "phase"_a = std::string(to_string(diagnostic.phase)),
                    "message"_a = diagnostic.message,
                    "source_id"_a = diagnostic.primary.source_id,
                    "json_pointer"_a = diagnostic.primary.json_pointer, "span"_a = span,
                    "witness"_a = json_to_py(diagnostic.witness), "help"_a = help);
}

// Sets neograph_engine.<type_name>, a RuntimeError subclass. Its message lists
// every diagnostic with its code, and .diagnostics keeps each diagnostic's
// source site and witness.
void set_program_error(const char*                    type_name,
                       std::string                    message,
                       const std::vector<Diagnostic>& program_diagnostics) {
    py::list diagnostics;
    for (const auto& diagnostic : program_diagnostics) {
        message += "\n" + diagnostic.code + " " + diagnostic.primary.json_pointer +
                   ": " + diagnostic.message;
        if (const auto& span = diagnostic.primary.span) {
            message += " (" + diagnostic.primary.source_id + ":" +
                       std::to_string(span->line_begin) + ":" +
                       std::to_string(span->column_begin) + ")";
        }
        diagnostics.append(diagnostic_to_py(diagnostic, true));
    }
    const auto type = py::module_::import("neograph_engine._neograph").attr(type_name);
    auto exception           = type(message);
    exception.attr("diagnostics") = std::move(diagnostics);
    if (!program_diagnostics.empty()) {
        exception.attr("code") = program_diagnostics.front().code;
        exception.attr("witness") = json_to_py(program_diagnostics.front().witness);
    } else {
        exception.attr("code") = "";
        exception.attr("witness") = py::none();
    }
    PyErr_SetObject(exception.get_type().ptr(), exception.ptr());
}

// Compile, admission and runtime diagnostic errors all reach Python as typed
// exceptions with their diagnostics, from any binding entry point.
void translate_program_errors(std::exception_ptr error) {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const ProgramCompileError& failure) {
        set_program_error("ProgramCompileError", "Program compilation failed",
                          failure.diagnostics());
    } catch (const ProgramAdmissionError& failure) {
        set_program_error("ProgramAdmissionError", "Program admission failed",
                          failure.diagnostics());
    } catch (const ProgramDiagnosticError& failure) {
        set_program_error("ProgramDiagnosticError", "Program operation failed",
                          {failure.diagnostic()});
    } catch (const ProgramSynthesisValidationError& failure) {
        std::string code = "P_SYNTHESIS_SEMANTIC_REJECTED";
        if (failure.evidence().is_object() && failure.evidence().contains("code") &&
            failure.evidence().at("code").is_string()) {
            code = failure.evidence().at("code").get<std::string>();
        }
        Diagnostic diagnostic{
            CompilePhase::Resolve,
            std::move(code),
            DiagnosticSeverity::Error,
            SourceCoordinate{"synthesis_validation", "/", std::nullopt},
            failure.what(),
            json{{"validation_receipt_id", failure.receipt().id()},
                 {"proposal_id", failure.receipt().data().proposal_id},
                 {"reservation_id", failure.receipt().data().reservation_id},
                 {"bundle_id", failure.receipt().data().bundle_id},
                 {"validator_identity", failure.receipt().data().validator_identity},
                 {"contract_identity", failure.receipt().data().contract_identity},
                 {"evidence", failure.evidence()}},
            {},
            std::nullopt};
        set_program_error("ProgramDiagnosticError", "Program operation failed",
                          {std::move(diagnostic)});
    } catch (const ProgramChildSynthesisError& failure) {
        Diagnostic diagnostic{
            CompilePhase::Resolve,
            failure.code(),
            DiagnosticSeverity::Error,
            SourceCoordinate{"child_synthesis", "/", std::nullopt},
            failure.what(),
            json{{"code", failure.code()}},
            {},
            std::nullopt};
        set_program_error("ProgramDiagnosticError", "Program operation failed",
                          {std::move(diagnostic)});
    }
}

// Core registry misses name the C++ registration call. Python callers register
// through the module wrappers, so rewrite the hint.
std::string with_python_hint(std::string message) {
    static constexpr std::pair<std::string_view, std::string_view> spellings[] = {
        {"ReducerRegistry::instance().register_reducer(name, fn)",
         "ng.ReducerRegistry.register_reducer(name, fn)"},
        {"ConditionRegistry::instance().register_condition(name, fn)",
         "ng.ConditionRegistry.register_condition(name, fn)"},
    };
    for (const auto& [cpp, python] : spellings) {
        if (const auto found = message.find(cpp); found != std::string::npos) {
            message.replace(found, cpp.size(), python);
            break;
        }
    }
    return message;
}

// Every ProgramRegistryBuilder.add_registered_* call that names an executable
// the process registry does not hold raises neograph_engine.ProgramRegistryError.
// It subclasses both ValueError (what add_registered_node raised) and
// RuntimeError (what the reducer and condition calls raised), so existing
// handlers keep catching it. .kind is "node", "reducer" or "condition" and
// .name is the missing name.
[[noreturn]] void raise_registry_miss(const char*        kind,
                                      const std::string& name,
                                      const std::string& message) {
    const auto type =
        py::module_::import("neograph_engine._neograph").attr("ProgramRegistryError");
    auto exception         = type(message);
    exception.attr("kind") = kind;
    exception.attr("name") = name;
    PyErr_SetObject(type.ptr(), exception.ptr());
    throw py::error_already_set();
}

class ProgramRegistryBuilderBinding {
public:
    ProgramRegistryBuilderBinding()
        : builder_(std::make_unique<RegistrySnapshotBuilder>()) {}

    // A provider entry is metadata: an exact identity a node can require. The
    // host binds the provider object itself through LocalProgramHost (#372).
    ProgramRegistryBuilderBinding& add_provider(const std::string& name,
                                                std::string semantic_version,
                                                std::string implementation_digest) {
        require_open();
        if (providers_.contains(name))
            throw py::value_error("add_provider: provider '" + name + "' is already registered");
        auto manifest = registered_manifest(ExecutableKind::Provider, name,
                                            std::move(semantic_version),
                                            std::move(implementation_digest), {}, {},
                                            ExecutionGuarantee::Strict);
        const auto identity = manifest.identity;
        builder_->add_provider(std::move(manifest),
                               ProviderMetadata{json::object(), json::object()});
        providers_.emplace(name, identity);
        return *this;
    }

    ProgramRegistryBuilderBinding& add_registered_node(
        const std::string& type,
        std::string semantic_version,
        std::string implementation_digest,
        std::vector<std::string> capabilities,
        std::vector<std::string> effects,
        ExecutionGuarantee guarantee,
        std::optional<std::string> provider) {
        require_open();
        if (provider) {
            // Only the reviewed Core llm_call node may hold a bound provider;
            // it sends every call through the host's pricing broker.
            if (type != "llm_call")
                throw py::value_error("add_registered_node: provider= binds only the built-in "
                                      "'llm_call' node, not '" + type + "'");
            const auto found = providers_.find(*provider);
            if (found == providers_.end())
                throw py::value_error("add_registered_node: provider '" + *provider +
                                      "' is not registered; call add_provider(\"" + *provider +
                                      "\", ...) first");
            auto manifest = registered_manifest(ExecutableKind::Node, type,
                                                std::move(semantic_version),
                                                std::move(implementation_digest),
                                                std::move(capabilities), std::move(effects),
                                                guarantee);
            // A static requirement, so the registry fingerprint and every
            // bundle that uses the node name the exact provider identity.
            manifest.required_executables = {found->second};
            builder_->add_core_llm_call(std::move(manifest), {});
            return *this;
        }
        // The manifest copies the type's config schema and effects now, so the
        // type must already be registered (#370).
        const auto types = NodeFactory::instance().registered_types();
        if (std::find(types.begin(), types.end(), type) == types.end()) {
            raise_registry_miss(
                "node", type,
                "add_registered_node: node type '" + type +
                    "' is not registered; register it first with @ng.node(\"" + type +
                    "\") or ng.NodeFactory.register_type(\"" + type + "\", factory)");
        }
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
        ReducerFn reducer;
        try {
            reducer = ReducerRegistry::instance().get(name);
        } catch (const std::runtime_error& error) {
            raise_registry_miss("reducer", name, with_python_hint(error.what()));
        }
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
        ConditionFn condition;
        try {
            condition = ConditionRegistry::instance().get(name);
        } catch (const std::runtime_error& error) {
            raise_registry_miss("condition", name, with_python_hint(error.what()));
        }
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
    std::map<std::string, ExecutableIdentity> providers_;
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

AdmissionProfile local_profile(const RegistrySnapshot& registry,
                               std::uint32_t max_supported_child_depth) {
    AdmissionProfileBuilder builder;
    builder.id("python-local-program-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .max_supported_child_depth(max_supported_child_depth)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_source_kind(SourceKind::CppBuilder)
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

// The policy a synthesized child is admitted under: the host's budget ceiling, as for every Program it admits, and
// only the capabilities, effects and modules of the compiled child.
PolicySnapshot synthesis_policy(const AdmissionProfile& profile,
                                const std::string& owner_scope,
                                const BudgetLimits& ceiling,
                                const std::vector<std::string>& capabilities,
                                const std::vector<std::string>& effects,
                                const ProgramSource& source,
                                ExecutionGuarantee minimum_guarantee = ExecutionGuarantee::Unmanaged) {
    PolicySnapshotBuilder builder;
    builder.id("python-local-program-synthesis-policy")
        .semantic_version("1.0.0")
        .owner_scope(owner_scope)
        .admission_profile(profile)
        .budget_ceiling(ceiling)
        .minimum_execution_guarantee(minimum_guarantee);
    for (const auto& capability : capabilities) builder.allow_capability(capability);
    for (const auto& effect : effects) builder.allow_effect(effect);
    for (const auto& imp : source.imports()) builder.allow_module_digest(imp.content_identity);
    return std::move(builder).build();
}

ModuleLinkReceipt create_module_link_receipt(const std::string& owner_scope,
                                             const ProgramVersion& parent,
                                             const std::string& binding_name,
                                             const ProgramBundle& child_bundle,
                                             const ProgramVersion& child,
                                             const RunBudget& max_budget) {
    const auto parent_policy         = parent.policy_snapshot();
    const auto child_policy          = child.policy_snapshot();
    ProgramModuleData module_data;
    module_data.owner_scope          = owner_scope;
    module_data.coordinate           = {"local", "parent", "1.0.0", ""};
    module_data.attestation_id       = parent.id();
    module_data.allowed_capabilities = parent_policy.allowed_capabilities();
    module_data.declared_effects     = parent_policy.allowed_effects();
    module_data.children.push_back({binding_name,
                                    child.id(),
                                    {{"input", child_bundle.input_contract()}},
                                    {{"output", child_bundle.output_contract()}},
                                    child_policy.allowed_capabilities(),
                                    child_policy.allowed_effects(),
                                    budget_limits(max_budget),
                                    child.execution_guarantee()});
    if (max_budget.max_child_depth > parent_policy.max_supported_child_depth()) {
        throw std::invalid_argument("Child depth exceeds the supported hard ceiling");
    }
    auto             module = ProgramModule::create(std::move(module_data));
    ModuleResolution resolution{module.coordinate(), {module}, {}};
    return link_module_child(resolution, module, binding_name, child_bundle, child);
}

ProgramCompilerConfig compiler_config(std::string             compiler_build_id,
                                      JavaScriptCompileLimits javascript_limits,
                                      bool                    require_acyclic,
                                      bool                    echo_routes,
                                      bool                    soundness_check,
                                      std::size_t             soundness_state_cap,
                                      std::uint64_t           max_supported_child_depth) {
    ProgramCompilerConfig config{std::move(compiler_build_id), javascript_limits};
    config.require_acyclic           = require_acyclic;
    config.echo_routes               = echo_routes;
    config.soundness_check           = soundness_check;
    config.soundness_state_cap       = soundness_state_cap;
    config.max_supported_child_depth =
        validate_max_supported_child_depth(max_supported_child_depth);
    return config;
}

struct HostStores {
    std::shared_ptr<ProgramStore>            program;
    std::shared_ptr<ProgramTransitionStore>  transitions;
    std::shared_ptr<graph::CheckpointStore>  checkpoints;
#ifdef NEOGRAPH_PYBIND_HAS_PROGRAM_SQLITE
    std::shared_ptr<SQLiteProgramProviderCallJournal> journal;
#endif
};

// In memory by default. With a directory, every store is a SQLite file in it
// and runs survive the process: program.db, checkpoints.db, provider_calls.db.
HostStores host_stores(const std::optional<std::string>& directory,
                       const std::shared_ptr<sp::NativeArchive>& archive) {
    HostStores stores;
    if (!directory) {
        stores.program     = std::make_shared<InMemoryProgramStore>();
        stores.transitions = std::make_shared<InMemoryProgramTransitionStore>();
        stores.checkpoints = std::make_shared<InMemoryCheckpointStore>();
        return stores;
    }
#ifdef NEOGRAPH_PYBIND_HAS_PROGRAM_SQLITE
    const std::filesystem::path root(*directory);
    std::filesystem::create_directories(root);
    const auto program = (root / "program.db").string();
    stores.program     = std::make_shared<SQLiteProgramStore>(program);
    stores.transitions = std::make_shared<SQLiteProgramTransitionStore>(program);
    stores.checkpoints = std::make_shared<graph::SqliteCheckpointStore>((root / "checkpoints.db").string());
    stores.journal     = std::make_shared<SQLiteProgramProviderCallJournal>(
        (root / "provider_calls.db").string(), archive);
    return stores;
#else
    (void)archive;
    throw py::value_error("sqlite_directory needs neograph_engine built with NEOGRAPH_BUILD_SQLITE");
#endif
}

class PyProgramHandoff {
public:
    explicit PyProgramHandoff(std::shared_ptr<ProgramHandoff> handoff = nullptr)
        : handoff_(std::move(handoff)) {}

    explicit operator bool() const noexcept {
        return handoff_ && static_cast<bool>(*handoff_);
    }

    ExactProgramHandoff wait() const {
        if (!handoff_ || !*handoff_) {
            throw py::value_error("ProgramHandoff is empty or already released");
        }
        // A copy taken with the GIL held: release() on another thread cannot free it while the GIL is released.
        const auto held = handoff_;
        py::gil_scoped_release release;
        return ExactProgramHandoff{held->reference(), held->value()};
    }

    py::object wait_async() const {
        if (!handoff_ || !*handoff_) {
            throw py::value_error("ProgramHandoff is empty or already released");
        }
        return spawn_program_async(
            [](std::shared_ptr<ProgramHandoff> held) -> asio::awaitable<ExactProgramHandoff> {
                co_return co_await held->wait_async();
            }(handoff_));
    }

    ExactProgramHandoffReference reference() const {
        if (!handoff_ || !*handoff_) {
            throw py::value_error("ProgramHandoff is empty or already released");
        }
        const auto held = handoff_;
        py::gil_scoped_release release;
        return held->reference();
    }

    py::object value() const {
        if (!handoff_ || !*handoff_) {
            throw py::value_error("ProgramHandoff is empty or already released");
        }
        const auto held = handoff_;
        const auto raw  = [&] {
            py::gil_scoped_release release;
            return held->value();
        }();
        return json_to_py(raw);
    }

    void release() noexcept {
        if (!handoff_) return;
        // Taken out with the GIL held, so no other Python thread reads the member while it changes; the hold is
        // released (which can wait for the runtime) without the GIL.
        auto dropped = std::move(handoff_);
        if (PyGILState_Check()) {
            py::gil_scoped_release release;
            dropped.reset();
        } else {
            dropped.reset();
        }
    }

    ProgramHandoff* get() const noexcept { return handoff_.get(); }
    const std::shared_ptr<ProgramHandoff>& shared() const noexcept { return handoff_; }

private:
    std::shared_ptr<ProgramHandoff> handoff_;
};

struct HostRuntimeBindings {
    mutable std::mutex mutex;
    std::map<std::tuple<std::string, std::string, std::string>, ProgramRuntimeChildBinding>
        child_bindings;
    std::map<std::tuple<std::string, std::string, std::string>, ProgramChildSynthesisGrant>
        child_synthesis_grants;
    std::map<std::string, ProgramSynthesisReservation> reservations;
    // The host's semantic validator (register_synthesis_validator): a Python callable taking (proposal, bundle,
    // reservation) and returning a ProgramSynthesisSemanticDecision. Without one, every synthesis is refused.
    std::shared_ptr<py::object> semantic_validator;
};

ProgramGraphMigrationTarget build_migration_target(
    py::object target,
    std::optional<GraphSemanticMigrationAdapter> semantic_adapter,
    std::optional<std::string> requested_run_id) {
    ProgramGraphMigrationTarget resolved;
    if (py::isinstance<ProgramGraphMigrationTarget>(target)) {
        resolved = target.cast<ProgramGraphMigrationTarget>();
    } else if (py::isinstance<ProgramVersion>(target)) {
        resolved.target_program_version_id = target.cast<const ProgramVersion&>().id();
    } else if (py::isinstance<py::str>(target)) {
        resolved.target_program_version_id = target.cast<std::string>();
    } else {
        throw py::type_error(
            "migrate_graph() target must be ProgramGraphMigrationTarget, ProgramVersion, or str");
    }
    if (semantic_adapter) resolved.semantic_adapter = std::move(*semantic_adapter);
    if (requested_run_id) resolved.requested_run_id = std::move(*requested_run_id);
    return resolved;
}

class LocalProgramHost {
public:
    LocalProgramHost(RegistrySnapshot registry,
                     std::string owner_scope,
                     RunBudget budget_ceiling,
                     std::string compiler_build_id,
                     std::size_t scheduler_threads,
                     std::shared_ptr<sp::NativeArchive> native_history_archive,
                     JavaScriptCompileLimits javascript_limits,
                     bool require_acyclic,
                     std::size_t worker_count,
                     py::object node_context_object,
                     std::optional<std::string> binding_label,
                     std::optional<std::string> sqlite_directory,
                     bool soundness_check,
                     std::size_t soundness_state_cap,
                     py::object failure_text_redactor,
                     bool fork_may_resend_provider_calls,
                     std::uint64_t max_supported_child_depth = MAX_SUPPORTED_CHILD_DEPTH)
        : registry_(std::move(registry)),
          owner_scope_(std::move(owner_scope)),
          compiler_build_id_(std::move(compiler_build_id)),
          max_supported_child_depth_(validate_max_supported_child_depth(max_supported_child_depth)),
          profile_(local_profile(registry_, max_supported_child_depth_)),
          policy_(local_policy(registry_, profile_, owner_scope_, budget_ceiling)),
          compiler_(std::make_shared<ProgramCompiler>(
              registry_, compiler_config(compiler_build_id_, javascript_limits, require_acyclic,
                                         false, soundness_check, soundness_state_cap,
                                         max_supported_child_depth_))),
          stores_(host_stores(sqlite_directory, native_history_archive)),
          runtime_bindings_(std::make_shared<HostRuntimeBindings>()) {
        std::optional<NodeContext> node_context;
        if (!node_context_object.is_none()) {
            // Python tools live on the wrapper, outside the C++ context.
            if (py::hasattr(node_context_object, "_pytools") &&
                py::len(node_context_object.attr("_pytools")) != 0)
                throw py::value_error("LocalProgramHost binds no tools; pass a NodeContext without tools");
            node_context = node_context_object.cast<NodeContext>();
        }
        std::string label;
        if (node_context) {
            if (!node_context->provider)
                throw py::value_error("node_context needs a provider: ng.NodeContext(provider=...)");
            if (!node_context->tools.view().empty())
                throw py::value_error("LocalProgramHost binds no tools; pass a NodeContext without tools");
            if (!binding_label)
                throw py::value_error("node_context needs binding_label=..., a non-secret name for "
                                      "this provider binding");
            label = neograph::program::detail::validated_binding_label(*binding_label);
        } else if (binding_label) {
            throw py::value_error("binding_label names a provider binding; pass node_context too");
        }
        if (node_context && sqlite_directory && !native_history_archive)
            throw py::value_error(
                "a durable host that binds a provider needs native_history_archive=...: its "
                "provider-call journal keeps every outcome under the archive's native custody");
        CatalogConfig catalog{stores_.program, registry_, std::make_shared<EngineGenerationCache>(),
                              compiler_build_id_};
        catalog.worker_count = worker_count;
        catalog.capability_binder = neograph::program::detail::single_provider_binder(node_context, label);
        catalog.failure_text_redactor =
            neograph::pybind::python_failure_text_redactor(std::move(failure_text_redactor));
        catalog_ = std::make_shared<ProgramCatalog>(std::move(catalog));

        ProgramSynthesisGatewayConfig gateway_config;
        gateway_config.compiler = compiler_;
        gateway_config.catalog  = catalog_;
        gateway_config.reserve  = [bindings = runtime_bindings_](
                                     const ProgramSynthesisProposal& proposal)
            -> ProgramSynthesisReservation {
            std::lock_guard<std::mutex> lock(bindings->mutex);
            const auto it = bindings->reservations.find(proposal.id());
            if (it == bindings->reservations.end()) {
                throw std::invalid_argument(
                    "Synthesis proposal has no active reservation on this host; call "
                    "reserve_synthesis() first");
            }
            return it->second;
        };
        gateway_config.reserve_child =
            [bindings = runtime_bindings_](
                const ProgramSynthesisProposal& proposal,
                const ProgramChildSynthesisAuthorization&) -> ProgramSynthesisReservation {
            std::lock_guard<std::mutex> lock(bindings->mutex);
            const auto it = bindings->reservations.find(proposal.id());
            if (it == bindings->reservations.end()) {
                throw std::invalid_argument(
                    "Child synthesis proposal has no active reservation on this host");
            }
            return it->second;
        };
        gateway_config.admission = [profile = profile_, ceiling = policy_.budget_ceiling()](
                                       const ProgramSynthesisProposal& proposal,
                                       const ProgramBundle& bundle,
                                       const ProgramSynthesisReservation&) -> ProgramAdmission {
            return ProgramAdmission{
                proposal.data().owner_scope,
                profile,
                synthesis_policy(profile, proposal.data().owner_scope, ceiling,
                                 bundle.capability_effect_closure().capabilities,
                                 bundle.capability_effect_closure().effects, proposal.source(),
                                 bundle.execution_guarantee()),
                {}};
        };
        gateway_config.validate_semantics =
            [bindings = runtime_bindings_](
                const ProgramSynthesisProposal&    proposal,
                const ProgramBundle&               bundle,
                const ProgramSynthesisReservation& reservation)
            -> ProgramSynthesisSemanticDecision {
            std::shared_ptr<py::object> validator;
            {
                std::lock_guard<std::mutex> lock(bindings->mutex);
                validator = bindings->semantic_validator;
            }
            if (!validator) {
                throw std::invalid_argument(
                    "Program synthesis needs the host's semantic validator; call "
                    "register_synthesis_validator() first");
            }
            py::gil_scoped_acquire gil;
            try {
                return (*validator)(proposal, bundle, reservation).cast<ProgramSynthesisSemanticDecision>();
            } catch (const py::error_already_set& error) {
                throw std::runtime_error(std::string("Program synthesis semantic validator failed: ") +
                                         error.what());
            } catch (const py::cast_error&) {
                throw std::invalid_argument(
                    "Program synthesis semantic validator must return a ProgramSynthesisSemanticDecision");
            }
        };
        synthesis_gateway_ = std::make_shared<ProgramSynthesisGateway>(std::move(gateway_config));

        RuntimeConfig config;
        config.catalog = catalog_;
        config.checkpoints = stores_.checkpoints;
        config.transitions = stores_.transitions;
        config.scheduler_threads = scheduler_threads;
        config.native_history_archive = std::move(native_history_archive);
        config.fork_may_resend_provider_calls = fork_may_resend_provider_calls;
        config.child_binding_resolver =
            [bindings = runtime_bindings_](std::string_view owner_scope,
                                           std::string_view parent_program_version_id,
                                           std::string_view binding_name)
            -> std::optional<ProgramRuntimeChildBinding> {
            std::lock_guard<std::mutex> lock(bindings->mutex);
            const auto it = bindings->child_bindings.find(
                {std::string(owner_scope), std::string(parent_program_version_id),
                 std::string(binding_name)});
            if (it == bindings->child_bindings.end()) return std::nullopt;
            return it->second;
        };
        config.child_synthesis_gateway = synthesis_gateway_;
        config.child_synthesis_grant_resolver =
            [bindings = runtime_bindings_](std::string_view owner_scope,
                                           std::string_view parent_run_id,
                                           std::string_view grant_id)
            -> std::optional<ProgramChildSynthesisGrant> {
            std::lock_guard<std::mutex> lock(bindings->mutex);
            const auto it = bindings->child_synthesis_grants.find(
                {std::string(owner_scope), std::string(parent_run_id), std::string(grant_id)});
            if (it == bindings->child_synthesis_grants.end()) return std::nullopt;
            return it->second;
        };
        if (node_context) {
            // Every built-in provider call goes through a pricing broker: the
            // SQLite journal when the host is durable, in process otherwise.
            config.require_core_provider_call_broker = true;
#ifdef NEOGRAPH_PYBIND_HAS_PROGRAM_SQLITE
            if (stores_.journal) {
                // The journal keys the deployment by the label, never by the key.
                const auto deployment = neograph::program::detail::provider_deployment_identity(label);
                config.core_provider_call_resolver =
                    [journal = stores_.journal, deployment](const ProgramCoreProviderCallContext& context)
                    -> std::optional<ProgramCoreProviderCallBinding> {
                    return journal->bind_priced(context, deployment);
                };
                config.core_provider_calls_journaled = true;
            }
#endif
            if (!config.core_provider_call_resolver) {
                config.core_provider_call_resolver = [](const ProgramCoreProviderCallContext& context)
                    -> std::optional<ProgramCoreProviderCallBinding> {
                    return bind_priced_provider_call(context);
                };
            }
        }
        runtime_ = std::make_shared<ProgramRuntime>(std::move(config));
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

    ProgramBundle compile(const ProgramSource& source, py::object budget) const {
        if (py::isinstance<ProgramBudgetBounds>(budget)) {
            return compiler_->compile(source, budget.cast<const ProgramBudgetBounds&>());
        }
        return compiler_->compile(source, budget.cast<const RunBudget&>());
    }

    ProgramVersion admit(const ProgramBundle& bundle) {
        return catalog_->admit(
            bundle, ProgramAdmission{owner_scope_, profile_, policy_, {}});
    }

    ProgramVersion compile_admit(const ProgramSource& source, py::object budget) {
        return admit(compile(source, std::move(budget)));
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

    // Continue a run that a stopped process left running, from the durable
    // stores. Recovery never sends a journaled provider request again.
    ProgramHandle reconnect(const std::string& run_id) {
        py::gil_scoped_release release;
        return runtime_->reconnect(owner_scope_, run_id);
    }

    ProgramHandle resume(std::string run_id,
                         py::object value,
                         std::optional<std::string> pending_id,
                         std::string trace_id) {
        auto owned_value = py_to_json(value);
        auto exact_id    = pending_id ? std::move(*pending_id) : awaiting_pending_id(run_id);
        py::gil_scoped_release release;
        try {
            return runtime_->resume(owner_scope_, run_id,
                                    ProgramResume{std::move(owned_value), std::move(trace_id), {},
                                                  std::move(exact_id)});
        } catch (const ProgramDiagnosticError& error) {
            if (error.diagnostic().code != "P_RUN_NOT_FOUND" || durable_) throw;
            // Runs live only in this host's memory; say so (#370).
            auto diagnostic    = error.diagnostic();
            diagnostic.message = "run '" + run_id +
                "' was not found in this LocalProgramHost; runs are kept in memory by the "
                "host that started them";
            throw ProgramDiagnosticError(std::move(diagnostic));
        }
    }

    // Fork a paused run into a new run of `version` that answers its pending
    // call with `value` (#390). Without checkpoint_id, the source run's exact
    // checkpoint is forked; without pending_id, its single awaiting call.
    ProgramHandle fork(std::string run_id,
                       py::object value,
                       const ProgramVersion& version,
                       const RunBudget& budget,
                       py::object input,
                       std::optional<std::string> checkpoint_id,
                       std::optional<std::string> pending_id,
                       std::optional<std::string> fork_run_id,
                       std::string trace_id) {
        auto owned_value = py_to_json(value);
        auto owned_input = input.is_none() ? json::object() : py_to_json(input);
        auto exact_id    = pending_id ? std::move(*pending_id) : awaiting_pending_id(run_id);
        std::string source_checkpoint;
        if (checkpoint_id) {
            source_checkpoint = std::move(*checkpoint_id);
        } else {
            const auto record = BlockingProgramTransitionStore(stores_.transitions).load(owner_scope_, run_id);
            if (!record || !record->exact_checkpoint())
                throw py::value_error("fork() needs checkpoint_id: run '" + run_id +
                                      "' has no exact checkpoint in this host");
            source_checkpoint = record->exact_checkpoint()->checkpoint_id;
        }
        ProgramInvocation invocation{std::move(owned_input), budget, trace_id, {}};
        if (fork_run_id) invocation.requested_run_id = std::move(*fork_run_id);
        py::gil_scoped_release release;
        return runtime_->fork(owner_scope_,
                              ExactProgramCheckpointReference{run_id, std::move(source_checkpoint)},
                              version, std::move(invocation),
                              ProgramResume{std::move(owned_value), trace_id + "-resume", {},
                                            std::move(exact_id)});
    }

    py::object run_async(const ProgramVersion& version,
                         py::object input,
                         const RunBudget& budget,
                         std::string trace_id) {
        auto owned_input = py_to_json(input);
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               ProgramVersion ver, ProgramInvocation inv) -> asio::awaitable<ProgramResult> {
                co_return co_await runtime->run_async(
                    std::move(owner_scope), std::move(ver), std::move(inv));
            }(runtime_, owner_scope_, version,
              ProgramInvocation{std::move(owned_input), budget, std::move(trace_id), {}}));
    }

    py::object start_async(const ProgramVersion& version,
                           py::object input,
                           const RunBudget& budget,
                           std::string trace_id) {
        auto owned_input = py_to_json(input);
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               ProgramVersion ver, ProgramInvocation inv) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->start_async(
                    std::move(owner_scope), std::move(ver), std::move(inv));
            }(runtime_, owner_scope_, version,
              ProgramInvocation{std::move(owned_input), budget, std::move(trace_id), {}}));
    }

    py::object reconnect_async(std::string run_id) {
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               std::string id) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->reconnect_async(
                    std::move(owner_scope), std::move(id));
            }(runtime_, owner_scope_, std::move(run_id)));
    }

    py::object resume_async(std::string run_id,
                            py::object value,
                            std::optional<std::string> pending_id,
                            std::string trace_id) {
        auto owned_value = py_to_json(value);
        auto exact_id    = pending_id ? std::move(*pending_id) : awaiting_pending_id(run_id);
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               std::string id, ProgramResume res) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->resume_handle_async(
                    std::move(owner_scope), std::move(id), std::move(res));
            }(runtime_, owner_scope_, std::move(run_id),
              ProgramResume{std::move(owned_value), std::move(trace_id), {},
                            std::move(exact_id)}));
    }

    ModuleLinkReceipt bind_child(const ProgramVersion& parent,
                                 const std::string& binding_name,
                                 const ProgramVersion& child,
                                 const RunBudget& max_budget) {
        if (parent.ownership_scope() != owner_scope_ || child.ownership_scope() != owner_scope_) {
            throw py::value_error("bind_child() versions must belong to this host's owner_scope");
        }
        const auto child_bundle = [&] {
            py::gil_scoped_release release;
            return BlockingProgramStore(stores_.program).get_bundle(owner_scope_, child.bundle_id());
        }();
        if (!child_bundle) {
            throw py::value_error("bind_child() child bundle was not found in this host");
        }
        auto receipt = create_module_link_receipt(
            owner_scope_, parent, binding_name, *child_bundle, child, max_budget);
        {
            std::lock_guard<std::mutex> lock(runtime_bindings_->mutex);
            runtime_bindings_->child_bindings.insert_or_assign(
                std::make_tuple(owner_scope_, parent.id(), binding_name),
                ProgramRuntimeChildBinding{receipt, child});
        }
        return receipt;
    }

    ProgramHandle start_child(const ProgramHandle& parent,
                              const ModuleLinkReceipt& link,
                              const ProgramVersion& version,
                              py::object input,
                              const RunBudget& budget,
                              std::optional<std::string> child_run_id,
                              std::string trace_id) {
        auto owned_input = py_to_json(input);
        ProgramInvocation invocation{
            std::move(owned_input), budget, std::move(trace_id), {}};
        invocation.parent_run_id = parent.run_id();
        if (child_run_id) invocation.requested_run_id = std::move(*child_run_id);
        py::gil_scoped_release release;
        return runtime_->start_child(owner_scope_, parent, link, version, std::move(invocation));
    }

    py::object start_child_async(const ProgramHandle& parent,
                                 const ModuleLinkReceipt& link,
                                 const ProgramVersion& version,
                                 py::object input,
                                 const RunBudget& budget,
                                 std::optional<std::string> child_run_id,
                                 std::string trace_id) {
        auto owned_input = py_to_json(input);
        ProgramInvocation invocation{
            std::move(owned_input), budget, std::move(trace_id), {}};
        invocation.parent_run_id = parent.run_id();
        if (child_run_id) invocation.requested_run_id = std::move(*child_run_id);
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               ProgramHandle par, ModuleLinkReceipt lnk, ProgramVersion ver,
               ProgramInvocation inv) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->start_child_async(
                    std::move(owner_scope), std::move(par), std::move(lnk),
                    std::move(ver), std::move(inv));
            }(runtime_, owner_scope_, parent, link, version, std::move(invocation)));
    }

    ProgramHandle replace(py::object source,
                          const ProgramVersion& target,
                          py::object input,
                          const RunBudget& budget,
                          std::optional<std::string> replacement_run_id,
                          std::string trace_id) {
        auto owned_input = py_to_json(input);
        ProgramInvocation invocation{std::move(owned_input), budget, std::move(trace_id), {}};
        if (replacement_run_id) invocation.requested_run_id = std::move(*replacement_run_id);
        if (py::isinstance<PyProgramHandoff>(source)) {
            auto& handoff = source.cast<PyProgramHandoff&>();
            if (!handoff.get()) {
                throw py::value_error("replace() received an empty ProgramHandoff");
            }
            py::gil_scoped_release release;
            return runtime_->replace(owner_scope_, std::move(*handoff.get()), target,
                                     std::move(invocation));
        }
        ExactProgramHandoffReference reference;
        if (py::isinstance<ExactProgramHandoffReference>(source)) {
            reference = source.cast<ExactProgramHandoffReference>();
        } else if (py::isinstance<ExactProgramHandoff>(source)) {
            reference = source.cast<const ExactProgramHandoff&>().reference;
        } else {
            throw py::type_error(
                "replace() source must be ProgramHandoff, ExactProgramHandoff, or "
                "ExactProgramHandoffReference");
        }
        py::gil_scoped_release release;
        return runtime_->replace(owner_scope_, std::move(reference), target,
                                 std::move(invocation));
    }

    py::object replace_async(py::object source,
                             const ProgramVersion& target,
                             py::object input,
                             const RunBudget& budget,
                             std::optional<std::string> replacement_run_id,
                             std::string trace_id) {
        auto owned_input = py_to_json(input);
        ProgramInvocation invocation{std::move(owned_input), budget, std::move(trace_id), {}};
        if (replacement_run_id) invocation.requested_run_id = std::move(*replacement_run_id);
        if (py::isinstance<PyProgramHandoff>(source)) {
            auto handoff = source.cast<PyProgramHandoff>().shared();
            if (!handoff) {
                throw py::value_error("replace_async() received an empty ProgramHandoff");
            }
            return spawn_program_async(
                [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
                   std::shared_ptr<ProgramHandoff> held, ProgramVersion ver,
                   ProgramInvocation inv) -> asio::awaitable<ProgramHandle> {
                    // The exact reference, awaited on the runtime's executor: a refused replacement leaves the source
                    // run and this hold as they were.
                    const auto exact = co_await held->wait_async();
                    co_return co_await runtime->replace_async(std::move(owner_scope), exact.reference,
                                                              std::move(ver), std::move(inv));
                }(runtime_, owner_scope_, std::move(handoff), target, std::move(invocation)));
        }
        ExactProgramHandoffReference reference;
        if (py::isinstance<ExactProgramHandoffReference>(source)) {
            reference = source.cast<ExactProgramHandoffReference>();
        } else if (py::isinstance<ExactProgramHandoff>(source)) {
            reference = source.cast<const ExactProgramHandoff&>().reference;
        } else {
            throw py::type_error(
                "replace_async() source must be ProgramHandoff, ExactProgramHandoff, or "
                "ExactProgramHandoffReference");
        }
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               ExactProgramHandoffReference ref, ProgramVersion ver,
               ProgramInvocation inv) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->replace_async(
                    std::move(owner_scope), std::move(ref), std::move(ver), std::move(inv));
            }(runtime_, owner_scope_, std::move(reference), target, std::move(invocation)));
    }

    ProgramHandle migrate_graph(
        const ProgramHandle& source,
        py::object target,
        std::optional<GraphSemanticMigrationAdapter> semantic_adapter,
        std::optional<std::string> requested_run_id) {
        auto resolved = build_migration_target(
            std::move(target), std::move(semantic_adapter), std::move(requested_run_id));
        py::gil_scoped_release release;
        return runtime_->migrate_graph(source, std::move(resolved));
    }

    py::object migrate_graph_async(
        const ProgramHandle& source,
        py::object target,
        std::optional<GraphSemanticMigrationAdapter> semantic_adapter,
        std::optional<std::string> requested_run_id) {
        auto resolved = build_migration_target(
            std::move(target), std::move(semantic_adapter), std::move(requested_run_id));
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, ProgramHandle src,
               ProgramGraphMigrationTarget tgt) -> asio::awaitable<ProgramHandle> {
                co_return co_await runtime->migrate_graph_async(
                    std::move(src), std::move(tgt));
            }(runtime_, source, std::move(resolved)));
    }

    std::shared_ptr<ProgramSynthesisGateway> synthesis_gateway() const noexcept {
        return synthesis_gateway_;
    }

    ProgramSynthesisReservation reserve_synthesis(
        const ProgramHandle& source,
        PyProgramHandoff& handoff,
        const ProgramSynthesisProposal& proposal,
        std::optional<std::string> expected_lineage_head) {
        if (!handoff.get()) {
            throw py::value_error("reserve_synthesis() received an empty ProgramHandoff");
        }
        std::string head = expected_lineage_head ? *expected_lineage_head : std::string{};
        if (head.empty()) {
            const auto lineage = run_lineage(source.logical_run_id());
            if (lineage) head = lineage->id();
        }
        auto reservation = [&] {
            py::gil_scoped_release release;
            return runtime_->reserve_synthesis(
                owner_scope_, source, *handoff.get(), proposal, head);
        }();
        {
            std::lock_guard<std::mutex> lock(runtime_bindings_->mutex);
            runtime_bindings_->reservations.insert_or_assign(proposal.id(), reservation);
        }
        return reservation;
    }

    py::object reserve_synthesis_async(
        const ProgramHandle& source,
        PyProgramHandoff& handoff,
        const ProgramSynthesisProposal& proposal,
        std::optional<std::string> expected_lineage_head) {
        auto held = handoff.shared();
        if (!held) {
            throw py::value_error("reserve_synthesis_async() received an empty ProgramHandoff");
        }
        std::string head = expected_lineage_head ? *expected_lineage_head : std::string{};
        if (head.empty()) {
            const auto lineage = run_lineage(source.logical_run_id());
            if (lineage) head = lineage->id();
        }
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime,
               std::shared_ptr<HostRuntimeBindings> bindings,
               std::string owner_scope, ProgramHandle src,
               std::shared_ptr<ProgramHandoff> h,
               ProgramSynthesisProposal prop, std::string expected_head)
            -> asio::awaitable<ProgramSynthesisReservation> {
                auto reservation = co_await runtime->reserve_synthesis_async(
                    std::move(owner_scope), std::move(src), *h, prop, std::move(expected_head));
                {
                    std::lock_guard<std::mutex> lock(bindings->mutex);
                    bindings->reservations.insert_or_assign(prop.id(), reservation);
                }
                co_return reservation;
            }(runtime_, runtime_bindings_, owner_scope_, source, std::move(held),
              proposal, std::move(head)));
    }

    ProgramSynthesisResult synthesize(const ProgramSynthesisProposal& proposal) {
        py::gil_scoped_release release;
        return synthesis_gateway_->synthesize(proposal);
    }

    void register_synthesis_validator(py::object validate) {
        if (!PyCallable_Check(validate.ptr()))
            throw py::type_error("register_synthesis_validator() needs a callable");
        auto held = std::shared_ptr<py::object>(new py::object(std::move(validate)),
                                                &neograph::pybind::delete_with_gil<py::object>);
        std::lock_guard<std::mutex> lock(runtime_bindings_->mutex);
        runtime_bindings_->semantic_validator = std::move(held);
    }

    // Refused before anything is reserved or recorded: without the host's validator no synthesis can be accepted.
    void require_synthesis_validator() const {
        std::lock_guard<std::mutex> lock(runtime_bindings_->mutex);
        if (!runtime_bindings_->semantic_validator)
            throw py::value_error(
                "Program synthesis needs the host's semantic validator; call register_synthesis_validator() first");
    }

    void authorize_child_synthesis_grant(const ProgramChildSynthesisGrant& grant) {
        if (grant.data().owner_scope != owner_scope_) {
            Diagnostic diagnostic{
                CompilePhase::Resolve,
                "P_CHILD_SYNTHESIS_OWNER",
                DiagnosticSeverity::Error,
                SourceCoordinate{"child_synthesis_grant", "/owner_scope", std::nullopt},
                "Child synthesis grant owner_scope does not match LocalProgramHost owner_scope",
                json{{"expected_owner_scope", owner_scope_},
                     {"grant_owner_scope", grant.data().owner_scope}},
                {},
                std::nullopt};
            throw ProgramDiagnosticError(std::move(diagnostic));
        }
        std::lock_guard<std::mutex> lock(runtime_bindings_->mutex);
        runtime_bindings_->child_synthesis_grants.insert_or_assign(
            std::make_tuple(owner_scope_, grant.data().parent_run_id, grant.id()), grant);
    }

    ProgramChildSynthesisRecord prepare_child_synthesis(
        const ProgramHandle& parent,
        const PyProgramHandoff& handoff,
        const ProgramSynthesisProposal& proposal,
        const ProgramChildSynthesisGrant& grant,
        std::string binding_name) {
        if (!handoff.get()) {
            throw py::value_error("prepare_child_synthesis() received an empty ProgramHandoff");
        }
        require_synthesis_validator();
        authorize_child_synthesis_grant(grant);
        py::gil_scoped_release release;
        return runtime_->prepare_child_synthesis(
            owner_scope_, parent, *handoff.get(), proposal, grant, std::move(binding_name));
    }

    py::object prepare_child_synthesis_async(
        const ProgramHandle& parent,
        const PyProgramHandoff& handoff,
        const ProgramSynthesisProposal& proposal,
        const ProgramChildSynthesisGrant& grant,
        std::string binding_name) {
        auto held = handoff.shared();
        if (!held) {
            throw py::value_error(
                "prepare_child_synthesis_async() received an empty ProgramHandoff");
        }
        require_synthesis_validator();
        authorize_child_synthesis_grant(grant);
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               ProgramHandle par, std::shared_ptr<ProgramHandoff> h,
               ProgramSynthesisProposal prop, ProgramChildSynthesisGrant gr,
               std::string name) -> asio::awaitable<ProgramChildSynthesisRecord> {
                co_return co_await runtime->prepare_child_synthesis_async(
                    std::move(owner_scope), std::move(par), *h, std::move(prop),
                    std::move(gr), std::move(name));
            }(runtime_, owner_scope_, parent, std::move(held), proposal, grant,
              std::move(binding_name)));
    }

    ProgramChildSynthesisRecord recover_child_synthesis(
        const std::string& parent_run_id,
        const std::string& proposal_id) {
        py::gil_scoped_release release;
        return runtime_->recover_child_synthesis(owner_scope_, parent_run_id, proposal_id);
    }

    py::object recover_child_synthesis_async(std::string parent_run_id,
                                             std::string proposal_id) {
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               std::string par_id, std::string prop_id)
            -> asio::awaitable<ProgramChildSynthesisRecord> {
                co_return co_await runtime->recover_child_synthesis_async(
                    std::move(owner_scope), std::move(par_id), std::move(prop_id));
            }(runtime_, owner_scope_, std::move(parent_run_id), std::move(proposal_id)));
    }

    std::vector<ProgramHandle> recover_children(const std::string& parent_run_id) {
        py::gil_scoped_release release;
        return runtime_->recover_children(owner_scope_, parent_run_id);
    }

    py::object recover_children_async(std::string parent_run_id) {
        return spawn_program_async(
            [](std::shared_ptr<ProgramRuntime> runtime, std::string owner_scope,
               std::string par_id) -> asio::awaitable<std::vector<ProgramHandle>> {
                co_return co_await runtime->recover_children_async(
                    std::move(owner_scope), std::move(par_id));
            }(runtime_, owner_scope_, std::move(parent_run_id)));
    }

    std::vector<ProgramChildSynthesisRecord> child_syntheses(
        const std::string& parent_run_id) const {
        py::gil_scoped_release release;
        return BlockingProgramTransitionStore(stores_.transitions)
            .load_child_syntheses(owner_scope_, parent_run_id);
    }

    std::optional<ProgramRunRecord> run_record(const std::string& run_id) const {
        py::gil_scoped_release release;
        return BlockingProgramTransitionStore(stores_.transitions).load(owner_scope_, run_id);
    }

    std::optional<ProgramBundle> bundle(const std::string& bundle_id) const {
        py::gil_scoped_release release;
        return BlockingProgramStore(stores_.program).get_bundle(owner_scope_, bundle_id);
    }

    std::optional<ProgramVersion> version(const std::string& version_id) const {
        py::gil_scoped_release release;
        return catalog_->resolve_version(owner_scope_, version_id);
    }

    std::optional<ProgramRunLineage> run_lineage(const std::string& logical_run_id) const {
        py::gil_scoped_release release;
        BlockingProgramTransitionStore transitions(stores_.transitions);
        auto found = transitions.load_lineage(
            owner_scope_, program_run_lineage_id(owner_scope_, logical_run_id));
        if (!found) {
            found = transitions.load_lineage(owner_scope_, logical_run_id);
        }
        return found;
    }

    std::optional<ProgramRunGeneration> run_generation(
        const std::string& logical_run_id,
        std::uint64_t generation) const {
        py::gil_scoped_release release;
        BlockingProgramTransitionStore transitions(stores_.transitions);
        auto found = transitions.load_generation(
            owner_scope_, program_run_lineage_id(owner_scope_, logical_run_id), generation);
        if (!found) {
            found = transitions.load_generation(owner_scope_, logical_run_id, generation);
        }
        return found;
    }

    const std::string& owner_scope() const noexcept { return owner_scope_; }
    std::uint32_t max_supported_child_depth() const noexcept {
        return max_supported_child_depth_;
    }

private:
    // The runtime accepts only the exact pending call id. Without one, address
    // the run's single awaiting call; any other run state keeps the empty id so
    // the runtime reports its own diagnostic.
    std::string awaiting_pending_id(const std::string& run_id) const {
        const auto record = BlockingProgramTransitionStore(stores_.transitions).load(owner_scope_, run_id);
        if (!record || record->continuation().state != ContinuationState::Interrupted) return {};
        const auto input  = record->pending_input();
        const auto effect = record->pending_effect();
        if (!input && !effect) return {};
        std::vector<std::string> awaiting;
        if (input && input->state() == ProgramPendingState::Awaiting)
            awaiting.push_back(input->call_id());
        if (effect && effect->state() == ProgramPendingState::Awaiting)
            awaiting.push_back(effect->call_id());
        if (awaiting.size() == 1) return awaiting.front();
        throw py::value_error("resume() needs pending_id: run '" + run_id + "' has " +
                              std::to_string(awaiting.size()) +
                              " awaiting pending calls; pass the exact id from "
                              "result.interrupt.pending_input.call_id "
                              "(or result.interrupt.pending_effect.call_id)");
    }

    RegistrySnapshot registry_;
    std::string owner_scope_;
    std::string compiler_build_id_;
    std::uint32_t max_supported_child_depth_ = MAX_SUPPORTED_CHILD_DEPTH;
    AdmissionProfile profile_;
    PolicySnapshot policy_;
    std::shared_ptr<ProgramCompiler> compiler_;
    HostStores stores_;
    bool durable_ = stores_.program && !std::dynamic_pointer_cast<InMemoryProgramStore>(stores_.program);
    std::shared_ptr<HostRuntimeBindings> runtime_bindings_;
    std::shared_ptr<ProgramCatalog> catalog_;
    std::shared_ptr<ProgramSynthesisGateway> synthesis_gateway_;
    std::shared_ptr<ProgramRuntime> runtime_;
};

template <typename T>
void bind_program_identity(py::class_<T>& type) {
    type.def_property_readonly("id", &T::id)
        .def("serialize_canonical", &T::serialize_canonical);
}

}  // namespace

void init_program(py::module_& m) {
    using namespace neograph::program;

    m.attr("MAX_SUPPORTED_CHILD_DEPTH") = MAX_SUPPORTED_CHILD_DEPTH;
    m.attr("MAX_REPRESENTABLE_CHILD_DEPTH") = MAX_REPRESENTABLE_CHILD_DEPTH;

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

    py::enum_<ExecutableKind>(m, "ProgramExecutableKind")
        .value("Node", ExecutableKind::Node)
        .value("Reducer", ExecutableKind::Reducer)
        .value("Condition", ExecutableKind::Condition)
        .value("Provider", ExecutableKind::Provider)
        .value("Tool", ExecutableKind::Tool)
        .value("Imported", ExecutableKind::Imported);

    py::class_<ExecutableIdentity>(m, "ProgramExecutableIdentity")
        .def(py::init<ExecutableKind, std::string, std::string, std::string>(),
             py::arg("kind") = ExecutableKind::Node,
             py::arg("name") = "",
             py::arg("semantic_version") = "",
             py::arg("implementation_digest") = "")
        .def_readwrite("kind", &ExecutableIdentity::kind)
        .def_readwrite("name", &ExecutableIdentity::name)
        .def_readwrite("semantic_version", &ExecutableIdentity::semantic_version)
        .def_readwrite("implementation_digest", &ExecutableIdentity::implementation_digest)
        .def("__eq__", [](const ExecutableIdentity& left, const ExecutableIdentity& right) {
            return left == right;
        }, py::is_operator());

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
        .def_readwrite("max_total_children", &RunBudget::max_total_children)
        .def_readwrite("max_node_retries", &RunBudget::max_node_retries,
                       "Run-level ceiling on node retries (#364); None means none.")
        .def("__eq__", [](const RunBudget& left, const RunBudget& right) {
            return left == right;
        }, py::is_operator());

    py::class_<ProgramBudgetBounds>(m, "ProgramBudgetBounds")
        .def(py::init<RunBudget, RunBudget>(),
             py::arg("minimum") = RunBudget{},
             py::arg("maximum") = RunBudget{})
        .def_readwrite("minimum", &ProgramBudgetBounds::minimum)
        .def_readwrite("maximum", &ProgramBudgetBounds::maximum)
        .def("__eq__", [](const ProgramBudgetBounds& left, const ProgramBudgetBounds& right) {
            return left == right;
        }, py::is_operator());

    py::class_<BudgetLimits>(m, "ProgramBudgetLimits")
        .def_readonly("wall_time_ms", &BudgetLimits::wall_time_ms)
        .def_readonly("model_tokens", &BudgetLimits::model_tokens)
        .def_readonly("monetary_microunits", &BudgetLimits::monetary_microunits)
        .def_readonly("max_concurrency", &BudgetLimits::max_concurrency)
        .def_readonly("max_program_operations", &BudgetLimits::max_program_operations)
        .def_readonly("max_core_steps", &BudgetLimits::max_core_steps)
        .def_readonly("max_dynamic_compiles", &BudgetLimits::max_dynamic_compiles)
        .def_readonly("max_child_depth", &BudgetLimits::max_child_depth)
        .def_readonly("max_total_children", &BudgetLimits::max_total_children)
        .def("__eq__", [](const BudgetLimits& left, const BudgetLimits& right) {
            return left == right;
        }, py::is_operator());

    py::class_<ProgramUsage>(m, "ProgramUsage")
        .def_readonly("wall_time_ms", &ProgramUsage::wall_time_ms)
        .def_readonly("model_tokens", &ProgramUsage::model_tokens)
        .def_readonly("monetary_microunits", &ProgramUsage::monetary_microunits)
        .def_readonly("program_operations", &ProgramUsage::program_operations)
        .def_readonly("core_steps", &ProgramUsage::core_steps)
        .def_readonly("peak_concurrency", &ProgramUsage::peak_concurrency)
        .def_readonly("node_retries", &ProgramUsage::node_retries,
                      "Node retries taken under max_node_retries (#364).");

    py::class_<ProgramMonetaryRecord>(m, "ProgramMonetaryRecord",
        "One priced provider call of a Program attempt, in micro-USD (1e-6 USD). evidence is "
        "'reported' (the provider's own cost), 'estimated' (the provider policy's model catalogue "
        "applied to the reported tokens; catalog_version and catalog_digest name it, pricing holds "
        "the entry and price rows) or 'held' (outcome or usage unknown, so the reservation is "
        "charged; reason says why). served_by is the upstream a routing gateway named as having "
        "served the call (OpenRouter's provider), or None when the response named none.")
        .def_readonly("call_id", &ProgramMonetaryRecord::call_id)
        .def_readonly("operation_id", &ProgramMonetaryRecord::operation_id)
        .def_readonly("node", &ProgramMonetaryRecord::node)
        .def_readonly("origin", &ProgramMonetaryRecord::origin)
        .def_readonly("model", &ProgramMonetaryRecord::model)
        .def_property_readonly("evidence", [](const ProgramMonetaryRecord& value) {
            return std::string(to_string(value.evidence));
        })
        .def_readonly("microunits", &ProgramMonetaryRecord::microunits)
        .def_readonly("reserved_microunits", &ProgramMonetaryRecord::reserved_microunits)
        .def_readonly("basis", &ProgramMonetaryRecord::basis)
        .def_readonly("catalog_version", &ProgramMonetaryRecord::catalog_version)
        .def_readonly("catalog_digest", &ProgramMonetaryRecord::catalog_digest)
        .def_readonly("conservative", &ProgramMonetaryRecord::conservative)
        .def_readonly("may_undercount", &ProgramMonetaryRecord::may_undercount)
        .def_readonly("replayed", &ProgramMonetaryRecord::replayed)
        .def_readonly("reason", &ProgramMonetaryRecord::reason)
        .def_property_readonly("pricing", [](const ProgramMonetaryRecord& value) {
            return json_to_py(value.pricing);
        })
        .def_readonly("served_by", &ProgramMonetaryRecord::served_by)
        .def("__repr__", [](const ProgramMonetaryRecord& value) {
            return "<ProgramMonetaryRecord " + value.model + " " +
                   std::string(to_string(value.evidence)) + " " +
                   std::to_string(value.microunits) + ">";
        });

    py::class_<ProgramSource> source(m, "ProgramSource");
    source.def_static("from_javascript",
                      [](std::string source_id, std::string source_text) {
                          return ProgramSource::from_javascript(
                              std::move(source_id), std::move(source_text));
                      },
                      py::arg("source_id"), py::arg("source_text"))
        .def_static("from_cpp_builder",
                    [](std::string source_id, py::object document, std::uint32_t schema_version) {
                        return ProgramSource::from_cpp_builder(
                            std::move(source_id), schema_version, py_to_json(document));
                    },
                    py::arg("source_id"), py::arg("document"),
                    py::arg("schema_version") = LATEST_PROGRAM_SCHEMA_VERSION)
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
        .def_property_readonly("identities", &RegistrySnapshot::identities)
        .def_property_readonly("manifest", [](const RegistrySnapshot& value) {
            return json_to_py(value.manifest());
        })
        .def("serialize_canonical", &RegistrySnapshot::serialize_canonical);

    py::class_<ProgramRegistryBuilderBinding>(m, "ProgramRegistryBuilder")
        .def(py::init<>())
        .def("add_provider", &ProgramRegistryBuilderBinding::add_provider,
             py::arg("name"), py::arg("semantic_version"), py::arg("implementation_digest"),
             py::return_value_policy::reference_internal,
             "Register a provider identity that llm_call nodes can require "
             "(add_registered_node(\"llm_call\", ..., provider=name)). The provider object is "
             "bound by LocalProgramHost(node_context=..., binding_label=...).")
        .def("add_registered_node", &ProgramRegistryBuilderBinding::add_registered_node,
             py::arg("type_name"), py::arg("semantic_version"),
             py::arg("implementation_digest"),
             py::arg("required_capabilities") = std::vector<std::string>{},
             py::arg("declared_effects") = std::vector<std::string>{},
             py::arg("execution_guarantee") = ExecutionGuarantee::Strict,
             py::arg("provider") = py::none(),
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
        .def_property_readonly("executable_registry_identities",
                               &ProgramBundle::executable_registry_identities)
        .def_property_readonly("required_capabilities", [](const ProgramBundle& value) {
            return value.capability_effect_closure().capabilities;
        })
        .def_property_readonly("declared_effects", [](const ProgramBundle& value) {
            return value.capability_effect_closure().effects;
        })
        .def_property_readonly("sealed_core_definitions", [](const ProgramBundle& value) {
            py::list result;
            for (const auto& definition : value.sealed_core_definitions()) {
                result.append(py::dict("name"_a = definition.name,
                                       "definition_hash"_a = definition.definition_hash,
                                       "definition"_a = json_to_py(definition.definition)));
            }
            return result;
        })
        // diagnostics is the sealed copy (part of id); compile_diagnostics is what
        // the compiler returned, with JavaScript call sites and authoring lints.
        .def_property_readonly("diagnostics", [](const ProgramBundle& value) {
            py::list result;
            for (const auto& diagnostic : value.diagnostics())
                result.append(diagnostic_to_py(diagnostic));
            return result;
        })
        .def_property_readonly("compile_diagnostics", [](const ProgramBundle& value) {
            py::list result;
            for (const auto& diagnostic : value.compile_diagnostics())
                result.append(diagnostic_to_py(diagnostic));
            return result;
        })
        .def("serialize_canonical", &ProgramBundle::serialize_canonical);

    py::class_<JavaScriptCompileLimits>(m, "ProgramJavaScriptCompileLimits",
        "Host ceilings for compile-time JavaScript evaluation. A budget passed to "
        "compile() lowers the wall-time ceiling to its wall_time_ms.")
        .def(py::init<>())
        .def_readwrite("memory_limit_bytes", &JavaScriptCompileLimits::memory_limit_bytes)
        .def_readwrite("max_stack_bytes", &JavaScriptCompileLimits::max_stack_bytes)
        .def_readwrite("max_interrupt_polls", &JavaScriptCompileLimits::max_interrupt_polls)
        .def_readwrite("max_wall_time_ms", &JavaScriptCompileLimits::max_wall_time_ms)
        .def_readwrite("max_generated_document_bytes",
                       &JavaScriptCompileLimits::max_generated_document_bytes);

    py::exception<ProgramCompileError>(m, "ProgramCompileError", PyExc_RuntimeError);
    py::exception<ProgramAdmissionError>(m, "ProgramAdmissionError", PyExc_RuntimeError);
    py::exception<ProgramDiagnosticError>(m, "ProgramDiagnosticError", PyExc_RuntimeError);
    m.attr("ProgramChildSynthesisError") = m.attr("ProgramDiagnosticError");
    m.attr("ProgramSynthesisValidationError") = m.attr("ProgramDiagnosticError");
    py::register_exception_translator(&translate_program_errors);
    {
        // Raised by raise_registry_miss for node, reducer and condition misses
        // alike. Both bases keep the earlier handlers working.
        const auto name = m.attr("__name__").cast<std::string>() + ".ProgramRegistryError";
        const auto bases =
            py::make_tuple(py::handle(PyExc_ValueError), py::handle(PyExc_RuntimeError));
        PyObject* type = PyErr_NewExceptionWithDoc(
            name.c_str(),
            "A ProgramRegistryBuilder.add_registered_* call named an executable that is "
            "not registered in this process. Subclasses ValueError and RuntimeError; "
            ".kind is 'node', 'reducer' or 'condition' and .name is the missing name.",
            bases.ptr(), nullptr);
        if (type == nullptr) throw py::error_already_set();
        m.attr("ProgramRegistryError") = py::reinterpret_steal<py::object>(type);
    }

    py::class_<ProgramCompiler>(m, "ProgramCompiler")
        .def(py::init([](RegistrySnapshot registry, std::string compiler_build_id,
                         JavaScriptCompileLimits javascript_limits, bool require_acyclic,
                         bool echo_routes, bool soundness_check, std::size_t soundness_state_cap,
                         std::uint64_t max_supported_child_depth) {
            return std::make_unique<ProgramCompiler>(
                std::move(registry),
                compiler_config(std::move(compiler_build_id), javascript_limits,
                                require_acyclic, echo_routes, soundness_check,
                                soundness_state_cap, max_supported_child_depth));
        }), py::arg("registry"), py::arg("compiler_build_id") = "neograph-python-program/v1",
            py::arg("javascript_limits") = JavaScriptCompileLimits{},
            py::arg("require_acyclic") = false, py::arg("echo_routes") = false,
            py::arg("soundness_check") = false, py::arg("soundness_state_cap") = 100000,
            py::arg("max_supported_child_depth") = MAX_SUPPORTED_CHILD_DEPTH,
            "Compile Program sources against a registry snapshot. soundness_check=True "
            "also explores every run of the root Core and returns P_CORE_S1-P_CORE_S5 "
            "warnings and a P_CORE_SOUNDNESS summary in compile_diagnostics; the bundle "
            "is the same either way.")
        .def("compile", [](const ProgramCompiler& self, const ProgramSource& source,
                            py::object budget) {
            if (budget.is_none()) return self.compile(source);
            if (py::isinstance<ProgramBudgetBounds>(budget)) {
                return self.compile(source, budget.cast<const ProgramBudgetBounds&>());
            }
            return self.compile(source, budget.cast<const RunBudget&>());
        }, py::arg("source"), py::arg("budget") = py::none());

    py::class_<ProgramVersion> version(m, "ProgramVersion");
    version.def_static("parse", &ProgramVersion::parse)
        .def_property_readonly("id", &ProgramVersion::id)
        .def_property_readonly("bundle_id", &ProgramVersion::bundle_id)
        .def_property_readonly("owner_scope", &ProgramVersion::ownership_scope)
        .def_property_readonly("execution_guarantee", &ProgramVersion::execution_guarantee)
        .def_property_readonly("policy_snapshot_fingerprint", [](const ProgramVersion& value) {
            return value.policy_snapshot().fingerprint();
        })
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
        .def_property_readonly("core_node", &ProgramPendingInput::core_node)
        .def_property_readonly("core_interrupt_value", [](const ProgramPendingInput& value) {
            return json_to_py(value.core_interrupt_value());
        })
        .def_property_readonly("core_interrupt_id", &ProgramPendingInput::core_interrupt_id,
            "Id of the Core interrupt this answers when its Core step paused on several "
            "(#388), else ''. Resuming the run answers exactly that interrupt.")
        .def("serialize_canonical", &ProgramPendingInput::serialize_canonical);
    py::class_<ProgramPendingEffect>(m, "ProgramPendingEffect")
        .def_property_readonly("operation_id", &ProgramPendingEffect::operation_id)
        .def_property_readonly("call_id", &ProgramPendingEffect::call_id)
        .def_property_readonly("effect_id", &ProgramPendingEffect::effect_id)
        .def_property_readonly("state", &ProgramPendingEffect::state)
        .def_property_readonly("payload", [](const ProgramPendingEffect& value) { return json_to_py(value.payload()); })
        .def_property_readonly("result_schema", [](const ProgramPendingEffect& value) { return json_to_py(value.result_schema()); })
        .def_property_readonly("expires_at_unix_ms", &ProgramPendingEffect::expires_at_unix_ms)
        .def_property_readonly("core_interrupt_id", &ProgramPendingEffect::core_interrupt_id,
            "Id of the Core interrupt this answers when its Core step paused on several "
            "(#388), else ''.")
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
        .def_property_readonly("monetary_records", &ProgramResult::monetary_records,
                               "Priced provider calls of this attempt, in settlement order.")
        .def_property_readonly("remaining_budget", &ProgramResult::remaining_budget)
        .def_property_readonly("execution_trace", &ProgramResult::execution_trace)
        .def_property_readonly("failure", &ProgramResult::failure)
        .def("serialize_canonical", &ProgramResult::serialize_canonical);

    py::class_<ProgramNotice>(m, "ProgramNotice",
        "A fact the runtime reports about how it started a run. It is not an error and does "
        "not change the run's outcome.")
        .def_readonly("code", &ProgramNotice::code)
        .def_readonly("message", &ProgramNotice::message)
        .def_property_readonly("witness", [](const ProgramNotice& value) { return json_to_py(value.witness); })
        .def("__eq__", [](const ProgramNotice& left, const ProgramNotice& right) { return left == right; }, py::is_operator());

    py::class_<ExactProgramHandoffReference>(m, "ExactProgramHandoffReference")
        .def(py::init<std::string, std::string, std::string, std::string>(),
             py::arg("source_run_id") = "",
             py::arg("source_journal_head") = "",
             py::arg("command_coordinate_id") = "",
             py::arg("command_entry_id") = "")
        .def_readwrite("source_run_id", &ExactProgramHandoffReference::source_run_id)
        .def_readwrite("source_journal_head", &ExactProgramHandoffReference::source_journal_head)
        .def_readwrite("command_coordinate_id",
                       &ExactProgramHandoffReference::command_coordinate_id)
        .def_readwrite("command_entry_id", &ExactProgramHandoffReference::command_entry_id)
        .def("__eq__",
             [](const ExactProgramHandoffReference& left,
                const ExactProgramHandoffReference& right) { return left == right; },
             py::is_operator());

    py::class_<ExactProgramHandoff>(m, "ExactProgramHandoff")
        .def(py::init([](ExactProgramHandoffReference reference, py::object value) {
            return ExactProgramHandoff{std::move(reference), py_to_json(value)};
        }),
             py::arg("reference") = ExactProgramHandoffReference{},
             py::arg("value") = py::dict())
        .def_readwrite("reference", &ExactProgramHandoff::reference)
        .def_property(
            "value",
            [](const ExactProgramHandoff& value) { return json_to_py(value.value); },
            [](ExactProgramHandoff& self, py::object value) { self.value = py_to_json(value); })
        .def("__eq__",
             [](const ExactProgramHandoff& left, const ExactProgramHandoff& right) {
                 return left.reference == right.reference && left.value == right.value;
             },
             py::is_operator());

    py::class_<PyProgramHandoff>(m, "ProgramHandoff")
        .def("__bool__", &PyProgramHandoff::operator bool)
        .def("wait", &PyProgramHandoff::wait)
        .def("wait_async", &PyProgramHandoff::wait_async)
        .def("release", &PyProgramHandoff::release)
        .def_property_readonly("reference", &PyProgramHandoff::reference)
        .def_property_readonly("value", &PyProgramHandoff::value);

    py::class_<ProgramReplacementReceipt>(m, "ProgramReplacementReceipt")
        .def_static("parse", &ProgramReplacementReceipt::parse)
        .def_property_readonly("id", &ProgramReplacementReceipt::id)
        .def_property_readonly("owner_scope", &ProgramReplacementReceipt::owner_scope)
        .def_property_readonly("lineage_id", &ProgramReplacementReceipt::lineage_id)
        .def_property_readonly("source_generation", &ProgramReplacementReceipt::source_generation)
        .def_property_readonly("source_generation_id",
                               &ProgramReplacementReceipt::source_generation_id)
        .def_property_readonly("source_lineage_head_id",
                               &ProgramReplacementReceipt::source_lineage_head_id)
        .def_property_readonly("source_run_id", &ProgramReplacementReceipt::source_run_id)
        .def_property_readonly("source_run_record_id",
                               &ProgramReplacementReceipt::source_run_record_id)
        .def_property_readonly("source_journal_head",
                               &ProgramReplacementReceipt::source_journal_head)
        .def_property_readonly("source_program_version_id",
                               &ProgramReplacementReceipt::source_program_version_id)
        .def_property_readonly("source_bundle_id", &ProgramReplacementReceipt::source_bundle_id)
        .def_property_readonly("checkpoint_coordinate_id",
                               &ProgramReplacementReceipt::checkpoint_coordinate_id)
        .def_property_readonly("checkpoint_entry_id",
                               &ProgramReplacementReceipt::checkpoint_entry_id)
        .def_property_readonly("handoff_value_identity",
                               &ProgramReplacementReceipt::handoff_value_identity)
        .def_property_readonly("target_generation", &ProgramReplacementReceipt::target_generation)
        .def_property_readonly("target_run_id", &ProgramReplacementReceipt::target_run_id)
        .def_property_readonly("target_program_version_id",
                               &ProgramReplacementReceipt::target_program_version_id)
        .def_property_readonly("target_bundle_id", &ProgramReplacementReceipt::target_bundle_id)
        .def_property_readonly("target_input_identity",
                               &ProgramReplacementReceipt::target_input_identity)
        .def_property_readonly("target_invocation_id",
                               &ProgramReplacementReceipt::target_invocation_id)
        .def_property_readonly("target_binding_fingerprint",
                               &ProgramReplacementReceipt::target_binding_fingerprint)
        .def_property_readonly("target_initial_run_record_id",
                               &ProgramReplacementReceipt::target_initial_run_record_id)
        .def_property_readonly("target_initial_journal_head",
                               &ProgramReplacementReceipt::target_initial_journal_head)
        .def("matches_handoff",
             [](const ProgramReplacementReceipt& self, py::object handoff_value) {
                 return self.matches_handoff(py_to_json(handoff_value));
             },
             py::arg("handoff_value"))
        .def("matches_target_input",
             [](const ProgramReplacementReceipt& self, py::object target_input) {
                 return self.matches_target_input(py_to_json(target_input));
             },
             py::arg("target_input"))
        .def("serialize_canonical", &ProgramReplacementReceipt::serialize_canonical);

    py::class_<ModuleLinkReceipt>(m, "ProgramModuleLinkReceipt")
        .def_static("parse", &ModuleLinkReceipt::parse)
        .def_property_readonly("id", &ModuleLinkReceipt::id)
        .def_property_readonly("owner_scope", &ModuleLinkReceipt::owner_scope)
        .def_property_readonly("parent_module_id", &ModuleLinkReceipt::parent_module_id)
        .def_property_readonly("dependency_merkle_root",
                               &ModuleLinkReceipt::dependency_merkle_root)
        .def_property_readonly("child_name", &ModuleLinkReceipt::child_name)
        .def_property_readonly("binding_name", &ModuleLinkReceipt::child_name)
        .def_property_readonly("child_program_version_id",
                               &ModuleLinkReceipt::child_program_version_id)
        .def_property_readonly("child_bundle_id", &ModuleLinkReceipt::child_bundle_id)
        .def_property_readonly("child_input_contract_fingerprint",
                               &ModuleLinkReceipt::child_input_contract_fingerprint)
        .def_property_readonly("child_output_contract_fingerprint",
                               &ModuleLinkReceipt::child_output_contract_fingerprint)
        .def_property_readonly("granted_capabilities",
                               &ModuleLinkReceipt::granted_capabilities)
        .def_property_readonly("granted_effects", &ModuleLinkReceipt::granted_effects)
        .def_property_readonly("budget", &ModuleLinkReceipt::budget)
        .def_property_readonly("max_budget", &ModuleLinkReceipt::budget)
        .def_property_readonly("minimum_execution_guarantee",
                               &ModuleLinkReceipt::minimum_execution_guarantee)
        .def("serialize_canonical", &ModuleLinkReceipt::serialize_canonical);
    m.attr("ModuleLinkReceipt") = m.attr("ProgramModuleLinkReceipt");

    py::enum_<ProgramChildState>(m, "ProgramChildState")
        .value("Publishing", ProgramChildState::Publishing)
        .value("Dispatched", ProgramChildState::Dispatched)
        .value("Completed", ProgramChildState::Completed)
        .value("Cancelled", ProgramChildState::Cancelled)
        .value("Failed", ProgramChildState::Failed);

    py::class_<ProgramPersistedInvocation>(m, "ProgramPersistedInvocation")
        .def_property_readonly("input",
                               [](const ProgramPersistedInvocation& self) {
                                   return json_to_py(self.input);
                               })
        .def_readonly("granted_budget", &ProgramPersistedInvocation::granted_budget)
        .def_readonly("trace_id", &ProgramPersistedInvocation::trace_id)
        .def_readonly("parent_run_id", &ProgramPersistedInvocation::parent_run_id)
        .def_readonly("child_depth", &ProgramPersistedInvocation::child_depth);

    py::class_<ProgramChildRecord>(m, "ProgramChildRecord")
        .def_readonly("child_run_id", &ProgramChildRecord::child_run_id)
        .def_readonly("link_id", &ProgramChildRecord::link_id)
        .def_readonly("module_link_receipt_id", &ProgramChildRecord::link_id)
        .def_readonly("link_receipt", &ProgramChildRecord::link_receipt)
        .def_readonly("invocation", &ProgramChildRecord::invocation)
        .def_readonly("state", &ProgramChildRecord::state)
        .def_readonly("terminal_result", &ProgramChildRecord::terminal_result)
        .def_readonly("result", &ProgramChildRecord::terminal_result)
        .def_readonly("terminal_generation", &ProgramChildRecord::terminal_generation)
        .def_property_readonly("binding_name",
                               [](const ProgramChildRecord& self) {
                                   return ModuleLinkReceipt::parse(self.link_receipt).child_name();
                               })
        .def_property_readonly("program_version_id",
                               [](const ProgramChildRecord& self) {
                                   return ModuleLinkReceipt::parse(self.link_receipt)
                                       .child_program_version_id();
                               })
        .def_property_readonly("bundle_id",
                               [](const ProgramChildRecord& self) {
                                   return ModuleLinkReceipt::parse(self.link_receipt)
                                       .child_bundle_id();
                               })
        .def_property_readonly("reserved_budget",
                               [](const ProgramChildRecord& self) {
                                   return self.invocation.granted_budget;
                               })
        .def("__eq__",
             [](const ProgramChildRecord& left, const ProgramChildRecord& right) {
                 return left == right;
             },
             py::is_operator());

    py::class_<ProgramRunRecord>(m, "ProgramRunRecord")
        .def_static("parse", &ProgramRunRecord::parse)
        .def_property_readonly("id", &ProgramRunRecord::id)
        .def_property_readonly("owner_scope", &ProgramRunRecord::owner_scope)
        .def_property_readonly("run_id", &ProgramRunRecord::run_id)
        .def_property_readonly("logical_run_id", &ProgramRunRecord::logical_run_id)
        .def_property_readonly("program_version_id", &ProgramRunRecord::program_version_id)
        .def_property_readonly("bundle_id", &ProgramRunRecord::bundle_id)
        .def_property_readonly("binding_fingerprint", &ProgramRunRecord::binding_fingerprint)
        .def_property_readonly("child_depth", &ProgramRunRecord::child_depth)
        .def_property_readonly("children",
                               [](const ProgramRunRecord& self) { return self.children(); })
        .def_property_readonly("invocation_budget",
                               [](const ProgramRunRecord& self) {
                                   return self.invocation().budget;
                               })
        .def_property_readonly("remaining_budget", &ProgramRunRecord::remaining_budget)
        .def_property_readonly("exact_checkpoint", &ProgramRunRecord::exact_checkpoint)
        .def_property_readonly("pending_input", &ProgramRunRecord::pending_input)
        .def_property_readonly("pending_effect", &ProgramRunRecord::pending_effect)
        .def_property_readonly("terminal_result", &ProgramRunRecord::terminal_result)
        .def_property_readonly("journal_head", &ProgramRunRecord::journal_head)
        .def_property_readonly("event_sequence", &ProgramRunRecord::event_sequence)
        .def_property_readonly("effect_sequence", &ProgramRunRecord::effect_sequence)
        .def_property_readonly("created_at_ms", &ProgramRunRecord::created_at_ms)
        .def_property_readonly("updated_at_ms", &ProgramRunRecord::updated_at_ms)
        .def("serialize_canonical", &ProgramRunRecord::serialize_canonical);

    py::class_<ProgramRunLineage>(m, "ProgramRunLineage")
        .def_static("parse", &ProgramRunLineage::parse)
        .def_property_readonly("id", &ProgramRunLineage::id)
        .def_property_readonly("owner_scope", &ProgramRunLineage::owner_scope)
        .def_property_readonly("lineage_id", &ProgramRunLineage::lineage_id)
        .def_property_readonly("root_run_id", &ProgramRunLineage::root_run_id)
        .def_property_readonly("logical_run_id", &ProgramRunLineage::root_run_id)
        .def_property_readonly("active_generation", &ProgramRunLineage::active_generation)
        .def_property_readonly("active_generation_id", &ProgramRunLineage::active_generation_id)
        .def_property_readonly("active_run_record_id", &ProgramRunLineage::active_run_record_id)
        .def_property_readonly("active_journal_head", &ProgramRunLineage::active_journal_head)
        .def_property_readonly("remaining_budget", &ProgramRunLineage::remaining_budget)
        .def_property_readonly("inflight_reservation", &ProgramRunLineage::inflight_reservation)
        .def_property_readonly("predecessor_head_id", &ProgramRunLineage::predecessor_head_id)
        .def_property_readonly("created_at_ms", &ProgramRunLineage::created_at_ms)
        .def_property_readonly("updated_at_ms", &ProgramRunLineage::updated_at_ms)
        .def_property_readonly("committed_descendant_budget",
                               &ProgramRunLineage::committed_descendant_budget)
        .def("serialize_canonical", &ProgramRunLineage::serialize_canonical);

    py::class_<ProgramRunGeneration>(m, "ProgramRunGeneration")
        .def_static("parse", &ProgramRunGeneration::parse)
        .def_property_readonly("id", &ProgramRunGeneration::id)
        .def_property_readonly("owner_scope", &ProgramRunGeneration::owner_scope)
        .def_property_readonly("lineage_id", &ProgramRunGeneration::lineage_id)
        .def_property_readonly("generation", &ProgramRunGeneration::generation)
        .def_property_readonly("run_id", &ProgramRunGeneration::run_id)
        .def_property_readonly("program_version_id", &ProgramRunGeneration::program_version_id)
        .def_property_readonly("bundle_id", &ProgramRunGeneration::bundle_id)
        .def_property_readonly("initial_run_record_id",
                               &ProgramRunGeneration::initial_run_record_id)
        .def_property_readonly("initial_journal_head",
                               &ProgramRunGeneration::initial_journal_head)
        .def_property_readonly("predecessor_generation_id",
                               &ProgramRunGeneration::predecessor_generation_id)
        .def_property_readonly("created_at_ms", &ProgramRunGeneration::created_at_ms)
        .def_property_readonly("child_depth", &ProgramRunGeneration::child_depth)
        .def_property_readonly("replacement_receipt",
                               &ProgramRunGeneration::replacement_receipt)
        .def_property_readonly("graph_migration_receipt",
                               &ProgramRunGeneration::graph_migration_receipt)
        .def("serialize_canonical", &ProgramRunGeneration::serialize_canonical);

    m.def("program_run_lineage_id", &program_run_lineage_id,
          py::arg("owner_scope"), py::arg("root_run_id"));

    py::enum_<MigrationCompatibility>(m, "MigrationCompatibility")
        .value("NewRunsOnly", MigrationCompatibility::NewRunsOnly)
        .value("ForkCompatible", MigrationCompatibility::ForkCompatible)
        .value("DrainOnly", MigrationCompatibility::DrainOnly)
        .value("OperatorReconciliation", MigrationCompatibility::OperatorReconciliation)
        .value("Blocked", MigrationCompatibility::Blocked)
        .value("Compatible", MigrationCompatibility::Compatible);
    m.attr("GraphMigrationCompatibility") = m.attr("MigrationCompatibility");

    py::class_<MigrationDiagnostic>(m, "MigrationDiagnostic")
        .def_readonly("code", &MigrationDiagnostic::code)
        .def_readonly("message", &MigrationDiagnostic::message)
        .def_property_readonly("dimension", [](const MigrationDiagnostic& self) {
            return std::string(to_string(self.dimension));
        })
        .def_property_readonly("source", [](const MigrationDiagnostic& self) {
            return json_to_py(self.source);
        })
        .def_property_readonly("target", [](const MigrationDiagnostic& self) {
            return json_to_py(self.target);
        })
        .def("__eq__",
             [](const MigrationDiagnostic& left, const MigrationDiagnostic& right) {
                 return left == right;
             },
             py::is_operator());
    m.attr("MigrationBlocker") = m.attr("MigrationDiagnostic");

    py::class_<MigrationPlan>(m, "MigrationPlan")
        .def_static("between",
                    [](const ProgramVersion& source,
                       std::optional<ProgramBundle> source_bundle,
                       const ProgramVersion& target,
                       std::optional<ProgramBundle> target_bundle) {
                        if (source_bundle && target_bundle) {
                            return MigrationPlan::between(source, *source_bundle, target,
                                                          *target_bundle);
                        }
                        return MigrationPlan::between(source, target);
                    },
                    py::arg("source"),
                    py::arg("source_bundle") = py::none(),
                    py::arg("target"),
                    py::arg("target_bundle") = py::none())
        .def_static("parse", &MigrationPlan::parse)
        .def_property_readonly("id", &MigrationPlan::id)
        .def_property_readonly("source_version_id", &MigrationPlan::source_version_id)
        .def_property_readonly("target_version_id", &MigrationPlan::target_version_id)
        .def_property_readonly("owner_scope", &MigrationPlan::owner_scope)
        .def_property_readonly("compatibility", &MigrationPlan::compatibility)
        .def_property_readonly("blockers", &MigrationPlan::blockers)
        .def_property_readonly("diagnostics", &MigrationPlan::diagnostics)
        .def("is_compatible", &MigrationPlan::is_compatible)
        .def("compatible", &MigrationPlan::is_compatible)
        .def("serialize_canonical", &MigrationPlan::serialize_canonical);
    m.attr("GraphMigrationPlan") = m.attr("MigrationPlan");

    py::class_<GraphMetadataChange>(m, "GraphMetadataChange")
        .def(py::init<std::string, std::string>(),
             py::arg("kind") = "",
             py::arg("json_pointer") = "")
        .def_readwrite("kind", &GraphMetadataChange::kind)
        .def_readwrite("json_pointer", &GraphMetadataChange::json_pointer)
        .def("__eq__",
             [](const GraphMetadataChange& left, const GraphMetadataChange& right) {
                 return left == right;
             },
             py::is_operator());

    py::class_<GraphSemanticMigrationAdapter>(m, "GraphSemanticMigrationAdapter")
        .def(py::init(&GraphSemanticMigrationAdapter::prepare),
             py::arg("source_version"),
             py::arg("source_bundle"),
             py::arg("target_version"),
             py::arg("target_bundle"))
        .def_static("prepare", &GraphSemanticMigrationAdapter::prepare,
                    py::arg("source_version"),
                    py::arg("source_bundle"),
                    py::arg("target_version"),
                    py::arg("target_bundle"))
        .def_static("parse", &GraphSemanticMigrationAdapter::parse)
        .def_property_readonly("id", &GraphSemanticMigrationAdapter::id)
        .def_property_readonly("owner_scope", &GraphSemanticMigrationAdapter::owner_scope)
        .def_property_readonly("source_program_version_id",
                               &GraphSemanticMigrationAdapter::source_program_version_id)
        .def_property_readonly("source_bundle_id",
                               &GraphSemanticMigrationAdapter::source_bundle_id)
        .def_property_readonly("target_program_version_id",
                               &GraphSemanticMigrationAdapter::target_program_version_id)
        .def_property_readonly("target_bundle_id",
                               &GraphSemanticMigrationAdapter::target_bundle_id)
        .def_property_readonly("source_core_name",
                               &GraphSemanticMigrationAdapter::source_core_name)
        .def_property_readonly("target_core_name",
                               &GraphSemanticMigrationAdapter::target_core_name)
        .def_property_readonly("source_core_generation_id",
                               &GraphSemanticMigrationAdapter::source_core_generation_id)
        .def_property_readonly("target_core_generation_id",
                               &GraphSemanticMigrationAdapter::target_core_generation_id)
        .def_property_readonly("topology_shape_id",
                               &GraphSemanticMigrationAdapter::topology_shape_id)
        .def_property_readonly("metadata_changes",
                               &GraphSemanticMigrationAdapter::metadata_changes)
        .def("binds", &GraphSemanticMigrationAdapter::binds,
             py::arg("source_version"), py::arg("source_bundle"),
             py::arg("target_version"), py::arg("target_bundle"))
        .def("migration_plan", &GraphSemanticMigrationAdapter::migration_plan,
             py::arg("source_version"), py::arg("source_bundle"),
             py::arg("target_version"), py::arg("target_bundle"))
        .def("serialize_canonical", &GraphSemanticMigrationAdapter::serialize_canonical);

    py::class_<GraphMigrationCapsule>(m, "GraphMigrationCapsule")
        .def_static("parse", &GraphMigrationCapsule::parse)
        .def_property_readonly("id", &GraphMigrationCapsule::id)
        .def_property_readonly("owner_scope", &GraphMigrationCapsule::owner_scope)
        .def_property_readonly("lineage_id", &GraphMigrationCapsule::lineage_id)
        .def_property_readonly("source_generation", &GraphMigrationCapsule::source_generation)
        .def_property_readonly("source_generation_id", &GraphMigrationCapsule::source_generation_id)
        .def_property_readonly("source_lineage_head_id",
                               &GraphMigrationCapsule::source_lineage_head_id)
        .def_property_readonly("source_run_id", &GraphMigrationCapsule::source_run_id)
        .def_property_readonly("source_program_version_id",
                               &GraphMigrationCapsule::source_program_version_id)
        .def_property_readonly("source_bundle_id", &GraphMigrationCapsule::source_bundle_id)
        .def_property_readonly("core_checkpoint", &GraphMigrationCapsule::core_checkpoint)
        .def("serialize_canonical", &GraphMigrationCapsule::serialize_canonical);

    py::class_<ProgramGraphMigrationReceipt>(m, "ProgramGraphMigrationReceipt")
        .def_static("parse", &ProgramGraphMigrationReceipt::parse)
        .def_property_readonly("id", &ProgramGraphMigrationReceipt::id)
        .def_property_readonly("capsule", &ProgramGraphMigrationReceipt::capsule)
        .def_property_readonly("source_bundle", &ProgramGraphMigrationReceipt::source_bundle)
        .def_property_readonly("source_version", &ProgramGraphMigrationReceipt::source_version)
        .def_property_readonly("target_bundle", &ProgramGraphMigrationReceipt::target_bundle)
        .def_property_readonly("target_version", &ProgramGraphMigrationReceipt::target_version)
        .def_property_readonly("semantic_adapter", &ProgramGraphMigrationReceipt::semantic_adapter)
        .def_property_readonly("migration_plan_id",
                               &ProgramGraphMigrationReceipt::migration_plan_id)
        .def_property_readonly("target_generation",
                               &ProgramGraphMigrationReceipt::target_generation)
        .def_property_readonly("target_run_id", &ProgramGraphMigrationReceipt::target_run_id)
        .def_property_readonly("target_program_version_id",
                               &ProgramGraphMigrationReceipt::target_program_version_id)
        .def_property_readonly("target_bundle_id", &ProgramGraphMigrationReceipt::target_bundle_id)
        .def_property_readonly("target_invocation_id",
                               &ProgramGraphMigrationReceipt::target_invocation_id)
        .def_property_readonly("target_binding_fingerprint",
                               &ProgramGraphMigrationReceipt::target_binding_fingerprint)
        .def_property_readonly("target_initial_run_record_id",
                               &ProgramGraphMigrationReceipt::target_initial_run_record_id)
        .def_property_readonly("target_initial_journal_head",
                               &ProgramGraphMigrationReceipt::target_initial_journal_head)
        .def_property_readonly("target_core_checkpoint",
                               &ProgramGraphMigrationReceipt::target_core_checkpoint)
        .def("serialize_canonical", &ProgramGraphMigrationReceipt::serialize_canonical);

    py::class_<ProgramGraphMigrationTarget>(m, "ProgramGraphMigrationTarget")
        .def(py::init([](py::object target_program_version_id,
                         std::optional<GraphSemanticMigrationAdapter> semantic_adapter,
                         std::string requested_run_id) {
            ProgramGraphMigrationTarget target;
            if (py::isinstance<ProgramVersion>(target_program_version_id)) {
                target.target_program_version_id =
                    target_program_version_id.cast<const ProgramVersion&>().id();
            } else {
                target.target_program_version_id =
                    target_program_version_id.cast<std::string>();
            }
            target.semantic_adapter = std::move(semantic_adapter);
            target.requested_run_id = std::move(requested_run_id);
            return target;
        }),
             py::arg("target_program_version_id") = "",
             py::arg("semantic_adapter") = py::none(),
             py::arg("requested_run_id") = "")
        .def_readwrite("target_program_version_id",
                       &ProgramGraphMigrationTarget::target_program_version_id)
        .def_readwrite("semantic_adapter", &ProgramGraphMigrationTarget::semantic_adapter)
        .def_readwrite("requested_run_id", &ProgramGraphMigrationTarget::requested_run_id);

    m.def("program_synthesis_source_identity", &program_synthesis_source_identity,
          py::arg("source"));

    const auto make_synthesis_proposal = [](std::string owner_scope,
                                            std::string parent_run_id,
                                            ProgramSource source,
                                            const RunBudget& requested_budget,
                                            std::optional<std::string> lineage_id,
                                            std::vector<std::string> requested_capabilities,
                                            std::vector<std::string> requested_effects,
                                            std::optional<std::string> instruction_id,
                                            std::int64_t created_at_ms) {
        std::string resolved_lineage =
            (lineage_id && !lineage_id->empty())
                ? std::move(*lineage_id)
                : program_run_lineage_id(owner_scope, parent_run_id);
        return ProgramSynthesisProposal::create(ProgramSynthesisProposalData{
            std::move(owner_scope),
            std::move(resolved_lineage),
            std::move(parent_run_id),
            std::move(instruction_id),
            std::move(source),
            std::move(requested_capabilities),
            std::move(requested_effects),
            requested_budget,
            created_at_ms});
    };

    py::class_<ProgramSynthesisProposal>(m, "ProgramSynthesisProposal")
        .def(py::init(make_synthesis_proposal),
             py::arg("owner_scope"),
             py::arg("parent_run_id"),
             py::arg("source"),
             py::arg("requested_budget"),
             py::arg("lineage_id") = py::none(),
             py::arg("requested_capabilities") = std::vector<std::string>{},
             py::arg("requested_effects") = std::vector<std::string>{},
             py::arg("instruction_id") = py::none(),
             py::arg("created_at_ms") = 0)
        .def_static("create", make_synthesis_proposal,
                    py::arg("owner_scope"),
                    py::arg("parent_run_id"),
                    py::arg("source"),
                    py::arg("requested_budget"),
                    py::arg("lineage_id") = py::none(),
                    py::arg("requested_capabilities") = std::vector<std::string>{},
                    py::arg("requested_effects") = std::vector<std::string>{},
                    py::arg("instruction_id") = py::none(),
                    py::arg("created_at_ms") = 0)
        .def_static("parse", &ProgramSynthesisProposal::parse)
        .def_property_readonly("id", &ProgramSynthesisProposal::id)
        .def_property_readonly("proposal_id", &ProgramSynthesisProposal::id)
        .def_property_readonly("owner_scope",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().owner_scope;
                               })
        .def_property_readonly("lineage_id",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().lineage_id;
                               })
        .def_property_readonly("parent_run_id",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().parent_run_id;
                               })
        .def_property_readonly("instruction_id",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().instruction_id;
                               })
        .def_property_readonly("source", &ProgramSynthesisProposal::source)
        .def_property_readonly("candidate_source", &ProgramSynthesisProposal::source)
        .def_property_readonly("requested_capabilities",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().requested_capabilities;
                               })
        .def_property_readonly("requested_effects",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().requested_effects;
                               })
        .def_property_readonly("requested_budget",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().requested_budget;
                               })
        .def_property_readonly("created_at_ms",
                               [](const ProgramSynthesisProposal& self) {
                                   return self.data().created_at_ms;
                               })
        .def("serialize_canonical", &ProgramSynthesisProposal::serialize_canonical);

    py::class_<ProgramSynthesisReservation>(m, "ProgramSynthesisReservation")
        .def_static("parse", &ProgramSynthesisReservation::parse)
        .def_property_readonly("id", &ProgramSynthesisReservation::id)
        .def_property_readonly("proposal_id",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().proposal_id;
                               })
        .def_property_readonly("lineage_id",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().lineage_id;
                               })
        .def_property_readonly("source_lineage_head_id",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().source_lineage_head_id;
                               })
        .def_property_readonly("reserved_lineage_head_id",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().reserved_lineage_head_id;
                               })
        .def_property_readonly("source_remaining",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().source_remaining;
                               })
        .def_property_readonly("remaining_after_reservation",
                               [](const ProgramSynthesisReservation& self) {
                                   return self.data().remaining_after_reservation;
                               })
        .def("serialize_canonical", &ProgramSynthesisReservation::serialize_canonical);

    py::class_<ProgramSynthesisSemanticDecision>(m, "ProgramSynthesisSemanticDecision")
        .def(py::init([](std::string validator_identity,
                         std::string contract_identity,
                         bool accepted,
                         py::object evidence) {
            return ProgramSynthesisSemanticDecision{
                std::move(validator_identity),
                std::move(contract_identity),
                accepted,
                evidence.is_none() ? json::object() : py_to_json(evidence)};
        }),
             py::arg("validator_identity"),
             py::arg("contract_identity"),
             py::arg("accepted"),
             py::arg("evidence") = py::none())
        .def_readwrite("validator_identity", &ProgramSynthesisSemanticDecision::validator_identity)
        .def_readwrite("contract_identity", &ProgramSynthesisSemanticDecision::contract_identity)
        .def_readwrite("accepted", &ProgramSynthesisSemanticDecision::accepted)
        .def_property(
            "evidence",
            [](const ProgramSynthesisSemanticDecision& self) { return json_to_py(self.evidence); },
            [](ProgramSynthesisSemanticDecision& self, py::object val) {
                self.evidence = py_to_json(val);
            });

    py::class_<ProgramSynthesisValidationReceipt>(m, "ProgramSynthesisValidationReceipt")
        .def_static("parse", &ProgramSynthesisValidationReceipt::parse)
        .def_property_readonly("id", &ProgramSynthesisValidationReceipt::id)
        .def_property_readonly("proposal_id",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().proposal_id;
                               })
        .def_property_readonly("reservation_id",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().reservation_id;
                               })
        .def_property_readonly("bundle_id",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().bundle_id;
                               })
        .def_property_readonly("validator_identity",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().validator_identity;
                               })
        .def_property_readonly("contract_identity",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().contract_identity;
                               })
        .def_property_readonly("accepted",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().accepted;
                               })
        .def_property_readonly("evidence_digest",
                               [](const ProgramSynthesisValidationReceipt& self) {
                                   return self.data().evidence_digest;
                               })
        .def("serialize_canonical", &ProgramSynthesisValidationReceipt::serialize_canonical);

    py::class_<ProgramSynthesisReceipt>(m, "ProgramSynthesisReceipt")
        .def_static("parse", &ProgramSynthesisReceipt::parse)
        .def_property_readonly("id", &ProgramSynthesisReceipt::id)
        .def_property_readonly("proposal_id",
                               [](const ProgramSynthesisReceipt& self) {
                                   return self.data().proposal_id;
                               })
        .def_property_readonly("reservation_id",
                               [](const ProgramSynthesisReceipt& self) {
                                   return self.data().reservation_id;
                               })
        .def_property_readonly("bundle_id",
                               [](const ProgramSynthesisReceipt& self) {
                                   return self.data().bundle_id;
                               })
        .def_property_readonly("program_version_id",
                               [](const ProgramSynthesisReceipt& self) {
                                   return self.data().program_version_id;
                               })
        .def_property_readonly("policy_snapshot_fingerprint",
                               [](const ProgramSynthesisReceipt& self) {
                                   return self.data().policy_snapshot_fingerprint;
                               })
        .def("serialize_canonical", &ProgramSynthesisReceipt::serialize_canonical);

    py::class_<ProgramSynthesisResult>(m, "ProgramSynthesisResult")
        .def_readonly("reservation", &ProgramSynthesisResult::reservation)
        .def_readonly("bundle", &ProgramSynthesisResult::bundle)
        .def_readonly("validation", &ProgramSynthesisResult::validation)
        .def_property_readonly("validation_evidence",
                               [](const ProgramSynthesisResult& self) {
                                   return json_to_py(self.validation_evidence);
                               })
        .def_readonly("version", &ProgramSynthesisResult::version)
        .def_readonly("receipt", &ProgramSynthesisResult::receipt);

    const auto make_child_synthesis_grant =
        [](std::string owner_scope,
           std::string parent_run_id,
           py::object parent_version,
           std::string expected_lineage_head_id,
           py::object reviewed_source,
           const RunBudget& child_budget_ceiling,
           std::string template_identity,
           std::string semantic_validator_identity,
           std::string semantic_contract_identity,
           std::vector<std::string> allowed_capabilities,
           std::vector<std::string> allowed_effects,
           std::optional<std::string> lineage_id,
           std::optional<std::string> parent_policy_fingerprint,
           std::optional<ExecutionGuarantee> minimum_execution_guarantee,
           std::size_t max_source_bytes,
           std::size_t max_sealed_modules) {
            std::string parent_version_id;
            std::string policy_fp =
                parent_policy_fingerprint ? *parent_policy_fingerprint : std::string{};
            ExecutionGuarantee resolved_guarantee =
                minimum_execution_guarantee.value_or(ExecutionGuarantee::Strict);
            if (py::isinstance<ProgramVersion>(parent_version)) {
                const auto& ver = parent_version.cast<const ProgramVersion&>();
                parent_version_id = ver.id();
                if (policy_fp.empty()) policy_fp = ver.policy_snapshot().fingerprint();
                if (!minimum_execution_guarantee) {
                    resolved_guarantee = ver.execution_guarantee();
                }
            } else {
                parent_version_id = parent_version.cast<std::string>();
            }
            std::string reviewed_id;
            if (py::isinstance<ProgramSource>(reviewed_source)) {
                reviewed_id = program_synthesis_source_identity(
                    reviewed_source.cast<const ProgramSource&>());
            } else {
                reviewed_id = reviewed_source.cast<std::string>();
            }
            std::string resolved_lineage =
                (lineage_id && !lineage_id->empty())
                    ? std::move(*lineage_id)
                    : program_run_lineage_id(owner_scope, parent_run_id);
            ProgramChildSynthesisGrantData data;
            data.owner_scope                 = std::move(owner_scope);
            data.parent_run_id               = std::move(parent_run_id);
            data.parent_program_version_id   = std::move(parent_version_id);
            data.parent_policy_fingerprint   = std::move(policy_fp);
            data.lineage_id                  = std::move(resolved_lineage);
            data.expected_lineage_head_id    = std::move(expected_lineage_head_id);
            data.template_identity           = std::move(template_identity);
            data.reviewed_source_identity    = std::move(reviewed_id);
            data.semantic_validator_identity = std::move(semantic_validator_identity);
            data.semantic_contract_identity  = std::move(semantic_contract_identity);
            data.allowed_capabilities        = std::move(allowed_capabilities);
            data.allowed_effects             = std::move(allowed_effects);
            data.child_budget_ceiling        = child_budget_ceiling;
            data.minimum_execution_guarantee = resolved_guarantee;
            data.max_source_bytes            = max_source_bytes;
            data.max_sealed_modules          = max_sealed_modules;
            return ProgramChildSynthesisGrant::create(std::move(data));
        };

    py::class_<ProgramChildSynthesisGrant>(m, "ProgramChildSynthesisGrant")
        .def(py::init(make_child_synthesis_grant),
             py::arg("owner_scope"),
             py::arg("parent_run_id"),
             py::arg("parent_version"),
             py::arg("expected_lineage_head_id"),
             py::arg("reviewed_source"),
             py::arg("child_budget_ceiling"),
             py::arg("template_identity"),
             py::arg("semantic_validator_identity"),
             py::arg("semantic_contract_identity"),
             py::arg("allowed_capabilities") = std::vector<std::string>{},
             py::arg("allowed_effects") = std::vector<std::string>{},
             py::arg("lineage_id") = py::none(),
             py::arg("parent_policy_fingerprint") = py::none(),
             py::arg("minimum_execution_guarantee") = py::none(),
             py::arg("max_source_bytes") = ProgramChildSynthesisGrantData{}.max_source_bytes,
             py::arg("max_sealed_modules") = ProgramChildSynthesisGrantData{}.max_sealed_modules)
        .def_static("create", make_child_synthesis_grant,
                    py::arg("owner_scope"),
                    py::arg("parent_run_id"),
                    py::arg("parent_version"),
                    py::arg("expected_lineage_head_id"),
                    py::arg("reviewed_source"),
                    py::arg("child_budget_ceiling"),
                    py::arg("template_identity"),
                    py::arg("semantic_validator_identity"),
                    py::arg("semantic_contract_identity"),
                    py::arg("allowed_capabilities") = std::vector<std::string>{},
                    py::arg("allowed_effects") = std::vector<std::string>{},
                    py::arg("lineage_id") = py::none(),
                    py::arg("parent_policy_fingerprint") = py::none(),
                    py::arg("minimum_execution_guarantee") = py::none(),
                    py::arg("max_source_bytes") = ProgramChildSynthesisGrantData{}.max_source_bytes,
                    py::arg("max_sealed_modules") = ProgramChildSynthesisGrantData{}.max_sealed_modules)
        .def_static("parse", &ProgramChildSynthesisGrant::parse)
        .def_property_readonly("id", &ProgramChildSynthesisGrant::id)
        .def_property_readonly("grant_id", &ProgramChildSynthesisGrant::id)
        .def_property_readonly("owner_scope",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().owner_scope;
                               })
        .def_property_readonly("parent_run_id",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().parent_run_id;
                               })
        .def_property_readonly("parent_program_version_id",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().parent_program_version_id;
                               })
        .def_property_readonly("parent_policy_fingerprint",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().parent_policy_fingerprint;
                               })
        .def_property_readonly("lineage_id",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().lineage_id;
                               })
        .def_property_readonly("expected_lineage_head_id",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().expected_lineage_head_id;
                               })
        .def_property_readonly("template_identity",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().template_identity;
                               })
        .def_property_readonly("reviewed_source_identity",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().reviewed_source_identity;
                               })
        .def_property_readonly("semantic_validator_identity",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().semantic_validator_identity;
                               })
        .def_property_readonly("semantic_contract_identity",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().semantic_contract_identity;
                               })
        .def_property_readonly("allowed_capabilities",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().allowed_capabilities;
                               })
        .def_property_readonly("allowed_effects",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().allowed_effects;
                               })
        .def_property_readonly("child_budget_ceiling",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().child_budget_ceiling;
                               })
        .def_property_readonly("minimum_execution_guarantee",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().minimum_execution_guarantee;
                               })
        .def_property_readonly("max_source_bytes",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().max_source_bytes;
                               })
        .def_property_readonly("max_sealed_modules",
                               [](const ProgramChildSynthesisGrant& self) {
                                   return self.data().max_sealed_modules;
                               })
        .def("serialize_canonical", &ProgramChildSynthesisGrant::serialize_canonical);

    py::class_<ProgramChildSynthesisParent>(m, "ProgramChildSynthesisParent")
        .def(py::init<ProgramVersion, ProgramRunRecord, ProgramRunLineage, ProgramRunGeneration>(),
             py::arg("version"),
             py::arg("run"),
             py::arg("lineage"),
             py::arg("generation"))
        .def_readonly("version", &ProgramChildSynthesisParent::version)
        .def_readonly("run", &ProgramChildSynthesisParent::run)
        .def_readonly("lineage", &ProgramChildSynthesisParent::lineage)
        .def_readonly("generation", &ProgramChildSynthesisParent::generation);

    py::class_<ProgramChildSynthesisAuthorization>(m, "ProgramChildSynthesisAuthorization")
        .def_property_readonly("id", &ProgramChildSynthesisAuthorization::id)
        .def_property_readonly("proposal_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().proposal_id;
                               })
        .def_property_readonly("grant_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().grant_id;
                               })
        .def_property_readonly("parent_run_record_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().parent_run_record_id;
                               })
        .def_property_readonly("parent_generation_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().parent_generation_id;
                               })
        .def_property_readonly("registry_fingerprint",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().registry_fingerprint;
                               })
        .def_property_readonly("semantic_validator_identity",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().semantic_validator_identity;
                               })
        .def_property_readonly("semantic_contract_identity",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().semantic_contract_identity;
                               })
        .def_property_readonly("lineage_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().lineage_id;
                               })
        .def_property_readonly("source_lineage_head_id",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().source_lineage_head_id;
                               })
        .def_property_readonly("parent_remaining",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().parent_remaining;
                               })
        .def_property_readonly("child_budget",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().child_budget;
                               })
        .def_property_readonly("minimum_execution_guarantee",
                               [](const ProgramChildSynthesisAuthorization& self) {
                                   return self.data().minimum_execution_guarantee;
                               })
        .def("serialize_canonical", &ProgramChildSynthesisAuthorization::serialize_canonical);

    m.def("authorize_program_child_synthesis", &authorize_program_child_synthesis,
          py::arg("proposal"), py::arg("grant"), py::arg("parent"));

    py::class_<ProgramChildSynthesisResult>(m, "ProgramChildSynthesisResult")
        .def_readonly("authorization", &ProgramChildSynthesisResult::authorization)
        .def_readonly("synthesis", &ProgramChildSynthesisResult::synthesis);

    py::enum_<ProgramChildSynthesisState>(m, "ProgramChildSynthesisState")
        .value("Reserved", ProgramChildSynthesisState::Reserved)
        .value("Compiling", ProgramChildSynthesisState::Compiling)
        .value("Compiled", ProgramChildSynthesisState::Compiled)
        .value("Validating", ProgramChildSynthesisState::Validating)
        .value("Validated", ProgramChildSynthesisState::Validated)
        .value("Admitting", ProgramChildSynthesisState::Admitting)
        .value("Admitted", ProgramChildSynthesisState::Admitted)
        .value("Bound", ProgramChildSynthesisState::Bound)
        .value("Dispatching", ProgramChildSynthesisState::Dispatching)
        .value("Spawned", ProgramChildSynthesisState::Spawned)
        .value("Failed", ProgramChildSynthesisState::Failed)
        .value("ReconciliationRequired", ProgramChildSynthesisState::ReconciliationRequired);

    py::class_<ProgramChildSynthesisRecord>(m, "ProgramChildSynthesisRecord")
        .def_static("parse", &ProgramChildSynthesisRecord::parse)
        .def_property_readonly("id", &ProgramChildSynthesisRecord::id)
        .def_property_readonly("proposal",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().proposal;
                               })
        .def_property_readonly("grant",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().grant;
                               })
        .def_property_readonly("parent",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().parent;
                               })
        .def_property_readonly("reservation",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().reservation;
                               })
        .def_property_readonly("binding_name",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().binding_name;
                               })
        .def_property_readonly("state",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().state;
                               })
        .def_property_readonly("revision",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().revision;
                               })
        .def_property_readonly("previous_id",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().previous_id;
                               })
        .def_property_readonly("artifacts",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return json_to_py(self.data().artifacts);
                               })
        .def_property_readonly("error_code",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().error_code;
                               })
        .def_property_readonly("proposal_id",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().proposal.id();
                               })
        .def_property_readonly("grant_id",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().grant.id();
                               })
        .def_property_readonly("owner_scope",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().proposal.data().owner_scope;
                               })
        .def_property_readonly("parent_run_id",
                               [](const ProgramChildSynthesisRecord& self) {
                                   return self.data().proposal.data().parent_run_id;
                               })
        .def_property_readonly("bundle",
                               [](const ProgramChildSynthesisRecord& self)
                                   -> std::optional<ProgramBundle> {
                                   const auto& a = self.data().artifacts;
                                   if (!a.is_object() || !a.contains("bundle")) return std::nullopt;
                                   return ProgramBundle::parse(a.at("bundle").dump());
                               })
        .def_property_readonly("version",
                               [](const ProgramChildSynthesisRecord& self)
                                   -> std::optional<ProgramVersion> {
                                   const auto& a = self.data().artifacts;
                                   if (!a.is_object() || !a.contains("version")) return std::nullopt;
                                   return ProgramVersion::parse(a.at("version").dump());
                               })
        .def_property_readonly("link_receipt",
                               [](const ProgramChildSynthesisRecord& self)
                                   -> std::optional<ModuleLinkReceipt> {
                                   const auto& a = self.data().artifacts;
                                   if (!a.is_object() || !a.contains("link")) return std::nullopt;
                                   return ModuleLinkReceipt::parse(a.at("link").dump());
                               })
        .def("serialize_canonical", &ProgramChildSynthesisRecord::serialize_canonical);

    py::class_<ProgramSynthesisGateway, std::shared_ptr<ProgramSynthesisGateway>>(
        m, "ProgramSynthesisGateway")
        .def("synthesize", &ProgramSynthesisGateway::synthesize,
             py::arg("proposal"),
             py::call_guard<py::gil_scoped_release>())
        .def("synthesize_child", &ProgramSynthesisGateway::synthesize_child,
             py::arg("proposal"), py::arg("grant"), py::arg("parent"),
             py::call_guard<py::gil_scoped_release>());

    py::class_<ProgramHandle>(m, "ProgramHandle")
        .def_property_readonly("run_id", &ProgramHandle::run_id)
        .def_property_readonly("logical_run_id", &ProgramHandle::logical_run_id)
        .def_property_readonly("program_version_id", &ProgramHandle::program_version_id)
        .def_property_readonly("attempt", &ProgramHandle::attempt)
        .def_property_readonly("notices", &ProgramHandle::notices,
            "Facts the runtime reports about the run, as ProgramNotice values; none is an error. "
            "host.fork() gives P_FORK_PROVIDER_CALLS_NOT_JOURNALED when the host has no durable "
            "provider-call journal (no sqlite_directory) and the paused attempt recorded provider "
            "calls in the paused operation. A host with a journal reports, as the run executes, "
            "P_PROVIDER_CALL_POLICY_CHANGED when a resume or a fork found a provider call under "
            "another SchemaProvider descriptor policy identity and used the journaled outcome, and "
            "P_FORK_PROVIDER_CALL_RESENT when a fork sent a call its source had already journaled "
            "because fork_may_resend_provider_calls allows it. A priced call that was charged more "
            "than its reservation gives P_MONEY_OVER_HOLD, with the witness call_id, operation_id, "
            "node, model, evidence ('reported' or 'estimated'), reserved_microunits, microunits and "
            "over_by, and served_by (the upstream that served the call) when the call's record has "
            "one; the call is charged as reported and the run's status and budgets are as they "
            "would be without the notice. Read them after wait() to see every one.")
        .def("cancel", &ProgramHandle::cancel)
        .def("cancel_async", [](ProgramHandle& self) {
            return spawn_program_async(
                [](ProgramHandle handle) -> asio::awaitable<bool> {
                    co_return co_await handle.cancel_async();
                }(self));
        })
        .def("wait", &ProgramHandle::wait, py::call_guard<py::gil_scoped_release>())
        .def("wait_async", [](const ProgramHandle& self) {
            return spawn_program_async(
                [](ProgramHandle handle) -> asio::awaitable<ProgramResult> {
                    co_return co_await handle.wait_async();
                }(self));
        })
        .def("try_result", &ProgramHandle::try_result)
        .def("latest_checkpoint", &ProgramHandle::latest_checkpoint)
        .def("latest_handoff", &ProgramHandle::latest_handoff,
             py::call_guard<py::gil_scoped_release>())
        .def("latest_handoff_async", [](const ProgramHandle& self) {
            return spawn_program_async(
                [](ProgramHandle handle) -> asio::awaitable<std::optional<ExactProgramHandoff>> {
                    co_return co_await handle.latest_handoff_async();
                }(self));
        })
        .def("next_handoff", [](const ProgramHandle& self) {
            auto handoff = [&] {
                py::gil_scoped_release release;
                return self.next_handoff();
            }();
            return PyProgramHandoff(std::make_shared<ProgramHandoff>(std::move(handoff)));
        })
        .def("next_handoff_async", [](const ProgramHandle& self) {
            return spawn_program_async(
                [](ProgramHandle handle) -> asio::awaitable<PyProgramHandoff> {
                    auto handoff = co_await handle.next_handoff_async();
                    co_return PyProgramHandoff(
                        std::make_shared<ProgramHandoff>(std::move(handoff)));
                }(self));
        })
        .def("snapshot", &ProgramHandle::snapshot, py::call_guard<py::gil_scoped_release>())
        .def("snapshot_async", [](const ProgramHandle& self) {
            return spawn_program_async(
                [](ProgramHandle handle) -> asio::awaitable<ProgramRunRecord> {
                    co_return co_await handle.snapshot_async();
                }(self));
        });

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
        "It uses the same C++ ProgramRuntime as native callers. Runs live only in "
        "this host's memory. scheduler_threads sizes the Program scheduler that "
        "drives concurrent callCore work and deadlines; worker_count sizes each "
        "Core engine's pool for parallel fan-out inside a graph (1 runs fan-out "
        "branches on the scheduler thread) and must not exceed an admitted "
        "Program's max_concurrency. node_context binds a provider to the llm_call "
        "nodes registered with provider=...; binding_label is its non-secret name in "
        "Program identities. Bound calls are priced against each run's "
        "monetary_microunits budget (0 grants nothing). sqlite_directory keeps runs, "
        "checkpoints and a provider-call journal in SQLite files there, so a run "
        "survives the process (reconnect, resume). failure_text_redactor is the #364 "
        "hook for the free text of failure records (see GraphEngine.compile); "
        "without it no text is written. fork_may_resend_provider_calls (#543): a fork "
        "repeats the provider calls of its paused invocation, and the provider-call "
        "journal replays them. When the journal has the source's call at the same "
        "position but can neither adopt it nor show that the call differs, the fork "
        "would send a call the provider has probably charged already. False, the default, "
        "refuses it before it is sent, and the fork fails; True sends it, which the "
        "provider may charge again, and the handle's notices say so.")
        .def(py::init<RegistrySnapshot, std::string, RunBudget, std::string, std::size_t,
                      std::shared_ptr<sp::NativeArchive>, JavaScriptCompileLimits, bool,
                      std::size_t, py::object, std::optional<std::string>,
                      std::optional<std::string>, bool, std::size_t, py::object, bool,
                      std::uint64_t>(),
             py::arg("registry"), py::arg("owner_scope"), py::arg("budget_ceiling"),
             py::arg("compiler_build_id") = "neograph-python-program/v1",
             py::arg("scheduler_threads") = 1,
             py::arg("native_history_archive") = nullptr,
             py::arg("javascript_limits") = JavaScriptCompileLimits{},
             py::arg("require_acyclic") = false,
             py::arg("worker_count") = 1,
             py::arg("node_context") = py::none(),
             py::arg("binding_label") = py::none(),
             py::arg("sqlite_directory") = py::none(),
             py::arg("soundness_check") = false,
             py::arg("soundness_state_cap") = 100000,
             py::arg("failure_text_redactor") = py::none(),
             py::arg("fork_may_resend_provider_calls") = false,
             py::arg("max_supported_child_depth") = MAX_SUPPORTED_CHILD_DEPTH)
        .def_property_readonly("owner_scope", &LocalProgramHost::owner_scope)
        .def_property_readonly("max_supported_child_depth",
                               &LocalProgramHost::max_supported_child_depth)
        .def_property_readonly("synthesis_gateway", &LocalProgramHost::synthesis_gateway)
        .def("reconnect", &LocalProgramHost::reconnect, py::arg("run_id"),
             "Continue a run that a stopped process left running, from this host's "
             "sqlite_directory. A journaled provider call is not sent again.")
        .def("reconnect_async", &LocalProgramHost::reconnect_async, py::arg("run_id"))
        .def("compile", &LocalProgramHost::compile,
             py::arg("source"), py::arg("budget"))
        .def("admit", &LocalProgramHost::admit, py::arg("bundle"))
        .def("compile_admit", &LocalProgramHost::compile_admit,
             py::arg("source"), py::arg("budget"))
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
        .def("run_async", &LocalProgramHost::run_async,
             py::arg("version"), py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program")
        .def("start", &LocalProgramHost::start,
             py::arg("version"), py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program")
        .def("start_async", &LocalProgramHost::start_async,
             py::arg("version"), py::arg("input"), py::arg("budget"),
             py::arg("trace_id") = "python-program")
        .def("resume", &LocalProgramHost::resume,
             py::arg("run_id"), py::arg("value"), py::arg("pending_id") = py::none(),
             py::arg("trace_id") = "python-program-resume",
             "Resume an interrupted run. Without pending_id, the run's single awaiting "
             "pending call is addressed; pass result.interrupt.pending_input.call_id "
             "to name it exactly.")
        .def("resume_async", &LocalProgramHost::resume_async,
             py::arg("run_id"), py::arg("value"), py::arg("pending_id") = py::none(),
             py::arg("trace_id") = "python-program-resume")
        .def("fork", &LocalProgramHost::fork,
             py::arg("run_id"), py::arg("value"), py::arg("version"), py::arg("budget"),
             py::arg("input") = py::none(), py::arg("checkpoint_id") = py::none(),
             py::arg("pending_id") = py::none(), py::arg("fork_run_id") = py::none(),
             py::arg("trace_id") = "python-program-fork",
             "Fork an interrupted run into a new run of `version` that answers its pending "
             "call with `value`, and return the new run's handle. budget is the fork's share of "
             "the source run's remaining_budget: wall time, tokens, money, operations, core steps "
             "and dynamic compiles are taken out of what the source has left, and max_concurrency "
             "is the fork's own limit, up to the source's, which the source keeps. A fork larger "
             "than the remainder raises P_FORK_BUDGET, and one that would leave the source "
             "without wall_time_ms or max_core_steps raises P_FORK_SOURCE_BUDGET. checkpoint_id "
             "defaults to the run's exact checkpoint and pending_id to its single awaiting call. "
             "The fork "
             "replays what the paused run finished, as resume does: a finished node or Send "
             "is neither run nor charged again (#390). A node that runs again in the fork "
             "replays the provider calls it made before it asked only from a durable "
             "provider-call journal, which a host has when it has sqlite_directory; without "
             "one it sends and charges them again, and the returned handle's notices name "
             "the calls the paused attempt recorded (P_FORK_PROVIDER_CALLS_NOT_JOURNALED).")
        .def("bind_child", &LocalProgramHost::bind_child,
             py::arg("parent"), py::arg("binding_name"), py::arg("child"), py::arg("max_budget"),
             "Link an admitted child ProgramVersion to a parent ProgramVersion under binding_name "
             "and register the binding on this host for ng.spawn_child and start_child.")
        .def("link_child", &LocalProgramHost::bind_child,
             py::arg("parent"), py::arg("binding_name"), py::arg("child"), py::arg("max_budget"))
        .def("start_child", &LocalProgramHost::start_child,
             py::arg("parent"), py::arg("link"), py::arg("version"),
             py::arg("input"), py::arg("budget"),
             py::arg("child_run_id") = py::none(),
             py::arg("trace_id") = "python-program-child")
        .def("start_child_async", &LocalProgramHost::start_child_async,
             py::arg("parent"), py::arg("link"), py::arg("version"),
             py::arg("input"), py::arg("budget"),
             py::arg("child_run_id") = py::none(),
             py::arg("trace_id") = "python-program-child")
        .def("replace", &LocalProgramHost::replace,
             py::arg("source"), py::arg("target"), py::arg("input"), py::arg("budget"),
             py::arg("replacement_run_id") = py::none(),
             py::arg("trace_id") = "python-program-replace",
             "Replace a handed-off Program run with an admitted target version. "
             "Target authority must be attenuated within the source version, budget must equal "
             "the source run's remaining budget, and active children of the source run are kept.")
        .def("replace_async", &LocalProgramHost::replace_async,
             py::arg("source"), py::arg("target"), py::arg("input"), py::arg("budget"),
             py::arg("replacement_run_id") = py::none(),
             py::arg("trace_id") = "python-program-replace")
        .def("migrate_graph", &LocalProgramHost::migrate_graph,
             py::arg("source"), py::arg("target"),
             py::arg("semantic_adapter") = py::none(),
             py::arg("requested_run_id") = py::none(),
             "Migrate a live root Core Program run to an admitted target version at a super-step "
             "boundary.")
        .def("migrate_graph_async", &LocalProgramHost::migrate_graph_async,
             py::arg("source"), py::arg("target"),
             py::arg("semantic_adapter") = py::none(),
             py::arg("requested_run_id") = py::none())
        .def("reserve_synthesis", &LocalProgramHost::reserve_synthesis,
             py::arg("source"), py::arg("handoff"), py::arg("proposal"),
             py::arg("expected_lineage_head") = py::none())
        .def("reserve_synthesis_async", &LocalProgramHost::reserve_synthesis_async,
             py::arg("source"), py::arg("handoff"), py::arg("proposal"),
             py::arg("expected_lineage_head") = py::none())
        .def("synthesize", &LocalProgramHost::synthesize,
             py::arg("proposal"),
             "Run the synthesis gateway (proposal -> compile -> semantic verify -> admit) "
             "under attenuated authority and reserved budget.")
        .def("register_synthesis_validator", &LocalProgramHost::register_synthesis_validator,
             py::arg("validate"),
             "Register the host's semantic validator: validate(proposal, bundle, reservation) returns a "
             "ProgramSynthesisSemanticDecision. The gateway accepts a decision only when its validator and "
             "contract identities are the ones the child synthesis grant names. Without a validator every "
             "synthesis is refused. The callable runs with the GIL held, on the thread that synthesizes.")
        .def("authorize_child_synthesis_grant",
             &LocalProgramHost::authorize_child_synthesis_grant,
             py::arg("grant"),
             "Register a ProgramChildSynthesisGrant on this host for ng.spawn_synthesized_child "
             "and prepare_child_synthesis.")
        .def("prepare_child_synthesis", &LocalProgramHost::prepare_child_synthesis,
             py::arg("parent"), py::arg("handoff"), py::arg("proposal"),
             py::arg("grant"), py::arg("binding_name"),
             "Compile, admit, link, and publish a synthesized child Program on a handed-off "
             "parent run.")
        .def("prepare_child_synthesis_async", &LocalProgramHost::prepare_child_synthesis_async,
             py::arg("parent"), py::arg("handoff"), py::arg("proposal"),
             py::arg("grant"), py::arg("binding_name"))
        .def("recover_child_synthesis", &LocalProgramHost::recover_child_synthesis,
             py::arg("parent_run_id"), py::arg("proposal_id"))
        .def("recover_child_synthesis_async", &LocalProgramHost::recover_child_synthesis_async,
             py::arg("parent_run_id"), py::arg("proposal_id"))
        .def("recover_children", &LocalProgramHost::recover_children,
             py::arg("parent_run_id"))
        .def("recover_children_async", &LocalProgramHost::recover_children_async,
             py::arg("parent_run_id"))
        .def("child_syntheses", &LocalProgramHost::child_syntheses,
             py::arg("parent_run_id"))
        .def("run_record", &LocalProgramHost::run_record,
             py::arg("run_id"))
        .def("bundle", &LocalProgramHost::bundle,
             py::arg("bundle_id"))
        .def("version", &LocalProgramHost::version,
             py::arg("version_id"))
        .def("run_lineage", &LocalProgramHost::run_lineage,
             py::arg("logical_run_id"))
        .def("run_generation", &LocalProgramHost::run_generation,
             py::arg("logical_run_id"), py::arg("generation"));
}

}  // namespace neograph::pybind
